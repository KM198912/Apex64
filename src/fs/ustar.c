#include <fs/ustar.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/alloc.h>
#include <kernel/kprintf.h>
#include <stddef.h>
#include <stdint.h>

/* Minimal USTAR (tar) reader: only supports files (typeflag '0' or '\0') and names.
 * Does NOT support directories specially; path matching is literal.
 */

struct ustar_entry {
    char *name;
    void *data;
    size_t size;
    struct ustar_entry *next;
};

struct ustar_fs {
    void *base;
    size_t size;
    struct ustar_entry *entries;
};

/* Header layout: 512-byte ustar header */
struct ustar_hdr {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

static size_t oct_to_size(const char *s, size_t len)
{
    size_t v = 0;
    for (size_t i = 0; i < len && s[i] >= '0' && s[i] <= '7'; ++i) {
        v = (v << 3) | (size_t)(s[i] - '0');
    }
    return v;
}


/* Define a forward read and close to avoid lambdas */
static ssize_t ustar_file_read(void *ctx, void *buf, size_t off, size_t len)
{
    struct ustar_entry *ent = (struct ustar_entry*)ctx;
    if (off >= ent->size) return 0;
    if (off + len > ent->size) len = ent->size - off;
    memcpy(buf, (uint8_t*)ent->data + off, len);
    return (ssize_t)len;
}
static void ustar_file_close(void *ctx)
{
    (void)ctx; /* ctx is part of ustar entries; handle is freed elsewhere */
}

static void *ustar_open(void *fs, const char *path, size_t *out_size)
{
    struct ustar_fs *u = fs;
    if (path[0] == '/') path++;
    for (struct ustar_entry *e = u->entries; e; e = e->next) {
        if (strcmp(e->name, path) == 0) {
            struct vfs_fh *h = kmalloc(sizeof(*h));
            if (!h) return NULL;
            h->read = ustar_file_read;
            h->close = ustar_file_close;
            h->ctx = e;
            if (out_size) *out_size = e->size;
            return h;
        }
    }
    return NULL;
}
static void *ustar_mount(void *mount_data)
{
    /* mount_data is { base, size } pair */
    void **args = mount_data;
    void *base = args[0];
    size_t size = (size_t)args[1];

    struct ustar_fs *u = kmalloc(sizeof(*u));
    if (!u) return NULL;
    u->base = base;
    u->size = size;
    u->entries = NULL;

    size_t off = 0;
    while (off + 512 <= size) {
        struct ustar_hdr *h = (struct ustar_hdr*)((uint8_t*)base + off);
        if (h->name[0] == '\0') break; /* end */
        size_t fsz = oct_to_size(h->size, sizeof(h->size));
        char *name = strdup(h->name);
        /* normalize names: strip leading './' or '/' */
        if (name) {
            if (name[0] == '/') {
                /* shift left */
                size_t l = strlen(name);
                memmove(name, name+1, l);
            } else if (name[0] == '.' && name[1] == '/') {
                size_t l = strlen(name);
                memmove(name, name+2, l-1);
            }
        }
        if (h->typeflag == '0' || h->typeflag == '\0') {
            struct ustar_entry *e = kmalloc(sizeof(*e));
            e->name = name;
            e->data = (uint8_t*)base + off + 512;
            e->size = fsz;
            e->next = u->entries;
            u->entries = e;
            kprintf("ustar: found %s size=%zu\n", e->name, fsz);
        } else {
            if (name) kfree(name);
        }
        /* advance by header+data rounded up to 512 */
        size_t blocks = (fsz + 511) / 512;
        off += 512 + blocks * 512;
    }
    return u;
}

static void ustar_unmount(void *fs)
{
    struct ustar_fs *u = fs;
    struct ustar_entry *e = u->entries;
    while (e) {
        struct ustar_entry *n = e->next;
        kfree(e->name);
        kfree(e);
        e = n;
    }
    kfree(u);
}

/* Export vfs_ops */
static struct vfs_ops ustar_ops = {
    .mount = ustar_mount,
    .unmount = ustar_unmount,
    .open = ustar_open,
};

struct vfs_ops *ustar_get_ops(void) { return &ustar_ops; }

void *ustar_mount_from_memory(void *base, size_t size)
{
    void *args[2]; args[0] = base; args[1] = (void*)size;
    return ustar_mount(args);
}
