#include <fs/fstab.h>
#include <fs/vfs.h>
#include <fs/ext2.h>
#include <lib/string.h>
#include <kernel/kprintf.h>
#include <lib/alloc.h>
#include <stddef.h>
#include <stdint.h>

/* Very small fstab parser: reads file into a buffer and parses lines
 * Format supported: <device> <mountpoint> <fstype> [options ...]
 */
int fstab_parse_and_mount(const char *path)
{
    char buf[4096];
    ssize_t r = vfs_read_all(path, buf, sizeof(buf));
    if (r < 0) {
        klog(1, "fstab: no %s found (skipping)\n", path);
        /* Diagnostic: check /etc directory presence and list entries */
        char dblk[1024];
        size_t dsz = 0;
        void *dh = vfs_open("/etc", &dsz);
        if (!dh) {
            klog(1, "fstab: vfs_open(/etc) failed - /etc not found on root fs\n");
        } else {
            ssize_t dr = vfs_read(dh, dblk, 0, sizeof(dblk));
            klog(1, "fstab: /etc inode size=%zu bytes, read=%d\n", dsz, (int)dr);
            if (dr > 0) {
                size_t off = 0;
                while (off + 8 < (size_t)dr) {
                    uint32_t ino = *((uint32_t*)(dblk + off + 0));
                    uint16_t rec = *((uint16_t*)(dblk + off + 4));
                    size_t nlen = (size_t)*(uint8_t*)(dblk + off + 6);
                    if (!ino) break;
                    if (rec == 0 || rec > (size_t)dr) {
                        klog(0, "fstab: dir entry with invalid rec_len=%u, aborting scan\n", (unsigned)rec);
                        break;
                    }
                    if (nlen >= 200) nlen = 199;
                    char name[200];
                    memcpy(name, dblk + off + 8, nlen);
                    name[nlen] = '\0';
                    klog(1, "fstab: dir entry ino=%u rec=%u name_len=%zu name=%s\n", ino, rec, nlen, name);
                    off += rec;
                    if (rec == 0) break;
                }
            }
            vfs_close(dh);
        }
        return -1;
    }
    size_t len = (size_t)r;
    /* ensure NUL-termination for parsing simplicity */
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    buf[len] = '\0';

    char *p = buf;
    while (p && *p) {
        /* find end of line without relying on <string.h> */
        char *line = p;
        char *nl = NULL;
        for (char *q = p; *q; ++q) { if (*q == '\n') { nl = q; break; } }
        if (nl) { *nl = '\0'; p = nl + 1; }
        else { p = NULL; }

        /* trim leading whitespace */
        while (*line && (*line == ' ' || *line == '\t' || *line == '\r')) line++;
        if (*line == '\0' || *line == '#') continue; /* empty or comment */

        /* parse first three whitespace separated fields */
        char *dev = line;
        char *sp = dev;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (*sp) { *sp = '\0'; sp++; }
        else sp = NULL;

        if (!sp) continue;
        /* skip additional whitespace to start mountpoint */
        while (*sp == ' ' || *sp == '\t') sp++;
        char *mnt = sp;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (*sp) { *sp = '\0'; sp++; }
        else sp = NULL;

        if (!sp) continue;
        /* skip whitespace to start fstype */
        while (*sp == ' ' || *sp == '\t') sp++;
        char *fstype = sp;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        if (*sp) *sp = '\0';

        /* now have dev, mnt, fstype */
        klog(1, "fstab: entry device=%s mount=%s fstype=%s\n", dev, mnt, fstype);

        /* only support /dev/<name> and ext2 for now */
        if (strncmp(dev, "/dev/", 5) == 0 && strcmp(fstype, "ext2") == 0) {
            const char *devname = dev + 5; /* e.g. sda2 */
            struct vfs_ops *ops = ext2_get_ops();
            if (vfs_mount(mnt, ops, (void*)devname) == 0) {
                klog(1, "fstab: mounted %s -> %s\n", dev, mnt);
            } else {
                klog(0, "fstab: failed to mount %s on %s\n", dev, mnt);
            }
        } else {
            klog(1, "fstab: unsupported entry (skipping)\n");
        }
    }

    return 0;
}
