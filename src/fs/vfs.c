#include <fs/vfs.h>
#include <lib/string.h>
#include <kernel/kprintf.h>
#include <lib/alloc.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_MOUNTS 8

struct mount_entry {
    char mount_point[32];
    struct vfs_ops *ops;
    void *fs;
};

static struct mount_entry mounts[MAX_MOUNTS];

int vfs_mount(const char *path, struct vfs_ops *ops, void *mount_data)
{
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        if (mounts[i].ops == NULL) {
            /* safe copy of mount point */
            size_t n = sizeof(mounts[i].mount_point);
            size_t j = 0;
            for (; j + 1 < n && path[j]; ++j) mounts[i].mount_point[j] = path[j];
            mounts[i].mount_point[j] = '\0';
            /* attempt mount first */
            void *fs = ops->mount ? ops->mount(mount_data) : mount_data;
            if (!fs) return -1;
            mounts[i].ops = ops;
            mounts[i].fs = fs;
                    klog(1, "vfs: mounted %s\n", mounts[i].mount_point);
            return 0;
        }
    }
    return -1;
}

int vfs_unmount(const char *path)
{
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        if (mounts[i].ops && strcmp(mounts[i].mount_point, path) == 0) {
            if (mounts[i].ops->unmount) mounts[i].ops->unmount(mounts[i].fs);
            klog(1, "vfs: unmounted %s\n", mounts[i].mount_point);
            mounts[i].ops = NULL;
            mounts[i].fs = NULL;
            mounts[i].mount_point[0] = '\0';
            return 0;
        }
    }
    return -1;
}
int vfs_list_dir(const char *path)
{
    size_t sz = 0;
    void *fh = vfs_open(path, &sz);
    if (!fh) return -1;

    /* Read the full directory blob (size may be 0 for empty dirs) */
    size_t buf_len = sz ? sz : 4096;
    uint8_t *buf = kmalloc(buf_len);
    if (!buf) { vfs_close(fh); return -1; }

    ssize_t r = vfs_read(fh, buf, 0, buf_len);
    if (r <= 0) {
        kprintf("vfs: %s appears empty or unreadable (r=%d)\n", path, (int)r);
        kfree(buf);
        vfs_close(fh);
        return 0;
    }

    kprintf("vfs: listing %s (bytes=%d):\n", path, (int)r);
    size_t off = 0; int count = 0;
    while (off + 8 <= (size_t)r) {
        uint32_t ino = *((uint32_t*)(buf + off + 0));
        uint16_t rec_len = *((uint16_t*)(buf + off + 4));
        size_t name_len = (size_t)*(uint8_t*)(buf + off + 6);
        if (!ino) break;
        if (rec_len < 8) break;
        if (name_len > 255) name_len = 255;
        char name[256];
        if (off + 8 + name_len <= (size_t)r) memcpy(name, buf + off + 8, name_len);
        name[name_len] = '\0';
        kprintf("  %s\n", name);
        off += rec_len;
        ++count;
        if (rec_len == 0) break; /* avoid infinite loop */
    }

    kfree(buf);
    vfs_close(fh);
    return count;
}
/* Find best mount by prefix match */
static struct mount_entry *find_mount(const char *path, const char **rel)
{
    struct mount_entry *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < MAX_MOUNTS; ++i) {
        if (!mounts[i].ops) continue;
        const char *mp = mounts[i].mount_point;
        size_t l = strlen(mp);
        if (l > best_len && strncmp(path, mp, l) == 0) {
            best = &mounts[i];
            best_len = l;
        }
    }
    if (best) {
        *rel = path + best_len;
        if (**rel == '/') (*rel)++;
    }
    return best;
}

void *vfs_open(const char *path, size_t *out_size)
{
    const char *rel = NULL;
    struct mount_entry *m = find_mount(path, &rel);
    if (!m) return NULL;
    if (m->ops->open) return m->ops->open(m->fs, rel, out_size);
    return NULL;
}

ssize_t vfs_read(void *fh, void *buf, size_t offset, size_t len)
{
    if (!fh) return -1;
    struct vfs_fh *h = (struct vfs_fh*)fh;
    if (!h->read) return -1;
    return h->read(h->ctx, buf, offset, len);
}

void vfs_close(void *fh)
{
    if (!fh) return;
    struct vfs_fh *h = (struct vfs_fh*)fh;
    if (h->close) h->close(h->ctx);
    kfree(h);
}

/* FD table (simple global table) */
#define MAX_FDS 32
struct fd_entry { int used; void *fh; size_t size; };
static struct fd_entry fds[MAX_FDS];

int vfs_fd_open(const char *path)
{
    size_t sz = 0;
    void *fh = vfs_open(path, &sz);
    if (!fh) return -1;
    for (int i = 0; i < MAX_FDS; ++i) {
        if (!fds[i].used) {
            fds[i].used = 1;
            fds[i].fh = fh;
            fds[i].size = sz;
            return i;
        }
    }
    /* no space */
    vfs_close(fh);
    return -1;
}

ssize_t vfs_fd_read(int fd, void *buf, size_t offset, size_t len)
{
    if (fd < 0 || fd >= MAX_FDS) return -1;
    if (!fds[fd].used) return -1;
    return vfs_read(fds[fd].fh, buf, offset, len);
}

int vfs_fd_close(int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return -1;
    if (!fds[fd].used) return -1;
    vfs_close(fds[fd].fh);
    fds[fd].used = 0; fds[fd].fh = NULL; fds[fd].size = 0;
    return 0;
}

ssize_t vfs_read_all(const char *path, void *buf, size_t buf_len)
{
    size_t sz = 0;
    void *fh = vfs_open(path, &sz);
    if (!fh) return -1;
    ssize_t r = vfs_read(fh, buf, 0, buf_len < sz ? buf_len : sz);
    vfs_close(fh);
    return r;
}
