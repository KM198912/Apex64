#include <fs/ext2.h>
#include <fs/vfs.h>
#include <block/block.h>
#include <lib/string.h>
#include <kernel/kprintf.h>
#include <lib/alloc.h>
#include <stdint.h>
#include <stddef.h>

/* Extremely small ext2 reader. Assumptions/simplifications:
 * - block size <= 4096
 * - inode size 128
 * - single group / small files
 * - only supports files and directories with direct blocks
 */

struct ext2_super {
    uint32_t s_inodes_count;
    uint32_t s_blocks_count;
    uint32_t s_r_blocks_count;
    uint32_t s_free_blocks_count;
    uint32_t s_free_inodes_count;
    uint32_t s_first_data_block;
    uint32_t s_log_block_size;
    uint32_t s_log_frag_size;
    uint32_t s_blocks_per_group;
    uint32_t s_frags_per_group;
    uint32_t s_inodes_per_group;
    uint32_t s_mtime;
    uint32_t s_wtime;
    uint16_t s_mnt_count;
    uint16_t s_max_mnt_count;
    uint16_t s_magic;
    uint16_t s_state;
    uint16_t s_errors;
    uint16_t s_minor_rev_level;
    uint32_t s_lastcheck;
    uint32_t s_checkinterval;
    uint32_t s_creator_os;
    uint32_t s_rev_level;
    uint16_t s_def_resuid;
    uint16_t s_def_resgid;
    uint32_t s_first_ino;    /* first usable inode for rev >= 1 */
    uint16_t s_inode_size;   /* inode size for rev >= 1 */
    uint16_t s_block_group_nr;
    /* truncated */
};

struct ext2_inode {
    uint16_t i_mode;
    uint16_t i_uid;
    uint32_t i_size;
    uint32_t i_atime;
    uint32_t i_ctime;
    uint32_t i_mtime;
    uint32_t i_dtime;
    uint16_t i_gid;
    uint16_t i_links_count;
    uint32_t i_blocks;
    uint32_t i_flags;
    uint32_t i_osd1;
    uint32_t i_block[15];
};

struct ext2_fs {
    char devname[16];
    struct ext2_super sb;
    uint32_t block_size;
    uint32_t inode_size;
    uint32_t inode_table_block;
};

static int read_sectors_from_dev(const char *dev, uint64_t lba, uint16_t count, void *buf, size_t len)
{
    return block_read(dev, lba, count, buf, len);
}

static int ext2_read_block(const char *dev, uint32_t block_no, void *buf, size_t buf_len, uint32_t block_size)
{
    uint64_t lba = (uint64_t)block_no * (block_size / 512);
    uint16_t cnt = (uint16_t)(block_size / 512);
    int r = read_sectors_from_dev(dev, lba, cnt, buf, buf_len);
    if (r != 0) {
        klog(0, "ext2: ext2_read_block failed dev=%s block_no=%u lba=%llu cnt=%u err=%d\n", dev, (unsigned)block_no, (unsigned long long)lba, (unsigned)cnt, r);
        return r;
    }
    return 0;
}

/* forward declarations */
static int ext2_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_inode *out);

static void *ext2_mount(void *mount_data)
{
    const char *dev = (const char*)mount_data;
    struct ext2_fs *fs = kmalloc(sizeof(*fs));
    if (!fs) return NULL;
    size_t i = 0; for (; i + 1 < sizeof(fs->devname) && dev[i]; ++i) fs->devname[i] = dev[i]; fs->devname[i] = '\0';

    uint8_t buf[1024*2]; /* enough for superblock + more */
    /* superblock at offset 1024 bytes -> sector 2 (assuming 512-byte sectors) */
    int r = read_sectors_from_dev(dev, 2, 2, buf, sizeof(buf));
    if (r != 0) {
        klog(0, "ext2: failed to read superblock from %s (err=%d)\n", dev, r);
        kfree(fs);
        return NULL;
    }
    struct ext2_super *sb = (struct ext2_super*)(buf + 0);
    if (sb->s_magic != 0xEF53) {
        klog(0, "ext2: bad magic 0x%04x\n", sb->s_magic);
        kfree(fs);
        return NULL;
    }
    fs->sb = *sb;
    fs->block_size = 1024u << sb->s_log_block_size;
    fs->inode_size = 128;

    klog(1, "ext2: super: first_data_block=%u inodes_count=%u inodes_per_group=%u\n",
            (unsigned)sb->s_first_data_block, (unsigned)sb->s_inodes_count, (unsigned)sb->s_inodes_per_group);

    /* group descriptor starts after superblock; for block_size=1024 it's block 2 */
    uint32_t gd_block = sb->s_first_data_block + 1;
    klog(1, "ext2: block_size=%u s_log_block_size=%u gd_block=%u\n", fs->block_size, (unsigned)sb->s_log_block_size, gd_block);
    uint8_t *gd = kmalloc(fs->block_size);
    if (!gd) { klog(0, "ext2: out of memory for group descriptor\n"); kfree(fs); return NULL; }
    int r2 = ext2_read_block(dev, gd_block, gd, fs->block_size, fs->block_size);
    if (r2 != 0) {
        klog(0, "ext2: failed to read group descriptor (err=%d)\n", r2);
        kfree(gd);
        kfree(fs);
        return NULL;
    }
    /* bg_inode_table at offset 8 in group descriptor */
    uint32_t inode_table = *((uint32_t*)(gd + 8));
    fs->inode_table_block = inode_table;
    klog(1, "ext2: mounted %s blocksize=%u inode_table=%u\n", dev, fs->block_size, inode_table);

    /* Determine inode size using superblock (rev >= 1 supports dynamic inode size) */
    if (sb->s_rev_level >= 1 && sb->s_inode_size != 0) {
        fs->inode_size = sb->s_inode_size;
    } else {
        fs->inode_size = 128; /* legacy default */
    }
    klog(1, "ext2: inode_size=%u\n", fs->inode_size);

    kfree(gd);
    return fs;
}

static void ext2_unmount(void *fs)
{
    struct ext2_fs *e = fs;
    kfree(e);
}

/* read inode by number (1-based) */
static int ext2_read_inode(struct ext2_fs *fs, uint32_t ino, struct ext2_inode *out)
{
    uint32_t index = ino - 1;
    uint32_t inodes_per_block = fs->block_size / fs->inode_size;
    uint32_t block = fs->inode_table_block + (index / inodes_per_block);
    uint32_t offset = (index % inodes_per_block) * fs->inode_size;
    uint8_t blockbuf[4096];
    if (ext2_read_block(fs->devname, block, blockbuf, sizeof(blockbuf), fs->block_size) != 0) return -1;
    memcpy(out, blockbuf + offset, sizeof(*out));
    return 0;
}

/* find entry in directory inode by name, return inode number or 0 */
static uint32_t ext2_find_in_dir(struct ext2_fs *fs, struct ext2_inode *dir, const char *name)
{
    /* iterate direct blocks only for simplicity */
    for (int i = 0; i < 12; ++i) {
        if (dir->i_block[i] == 0) continue;
        uint8_t blk[4096];
        if (ext2_read_block(fs->devname, dir->i_block[i], blk, sizeof(blk), fs->block_size) != 0) continue;
        uint32_t off = 0;
        while (off < fs->block_size) {
            uint32_t inode = *((uint32_t*)(blk + off + 0));
            uint16_t rec_len = *((uint16_t*)(blk + off + 4));
            size_t name_len = (size_t)*(uint8_t*)(blk + off + 6);
            if (!inode) break;
            char entry_name[256];
            if (name_len >= sizeof(entry_name)) name_len = sizeof(entry_name)-1;
            memcpy(entry_name, blk + off + 8, name_len);
            entry_name[name_len] = '\0';
            if (strcmp(entry_name, name) == 0) return inode;
            off += rec_len;
            if (rec_len == 0) break;
        }
    }
    return 0;
}

/* open: resolve path components */
static ssize_t ext2_file_read(void *ctxp, void *buf, size_t offset, size_t len);
static void ext2_file_close(void *ctxp);

static void *ext2_open(void *fs, const char *path, size_t *out_size)
{
    struct ext2_fs *efs = fs;
    if (path[0] == '/') path++;
    struct ext2_inode inode;
    if (ext2_read_inode(efs, 2, &inode) != 0) return NULL; /* root inode */
    char comp[256];
    const char *p = path;
    while (*p) {
        size_t i = 0;
        while (*p && *p != '/') { if (i + 1 < sizeof(comp)) comp[i++] = *p; p++; }
        comp[i] = '\0';
        if (i == 0) break;
        uint32_t ino = ext2_find_in_dir(efs, &inode, comp);
        if (!ino) return NULL;
        if (ext2_read_inode(efs, ino, &inode) != 0) return NULL;
        if (*p == '/') p++;
    }

    /* Build file handle */
    struct vfs_fh *h = kmalloc(sizeof(*h));
    if (!h) return NULL;
    /* store inode pointer data in ctx by allocating a small struct */
    struct { struct ext2_inode ino; struct ext2_fs *fs; } *ctx = kmalloc(sizeof(*ctx));
    if (!ctx) { kfree(h); return NULL; }
    ctx->ino = inode; ctx->fs = efs;
    h->ctx = ctx;
    h->read = ext2_file_read;
    h->close = ext2_file_close;
    if (out_size) *out_size = inode.i_size;
    return h;
}

/* lambdas not supported; implement wrapper read/close - but for simplicity, we'll instead define static wrappers using function pointers above. */

/* For C, replace lambdas with static functions: */
static ssize_t ext2_file_read(void *ctxp, void *buf, size_t offset, size_t len)
{
    struct { struct ext2_inode ino; struct ext2_fs *fs; } *c = ctxp;
    size_t total = c->ino.i_size;
    if (offset >= total) return 0;
    if (offset + len > total) len = total - offset;
    size_t copied = 0;
    uint32_t blocksize = c->fs->block_size;
    while (copied < len) {
        uint32_t block_index = (offset + copied) / blocksize;
        uint32_t block_off = (offset + copied) % blocksize;
        if (block_index >= 12) break; /* not handling indirection */
        uint32_t blk = c->ino.i_block[block_index];
        if (!blk) break;
        uint8_t blockbuf[4096];
        if (ext2_read_block(c->fs->devname, blk, blockbuf, sizeof(blockbuf), blocksize) != 0) break;
        size_t to_copy = blocksize - block_off;
        if (to_copy > len - copied) to_copy = len - copied;
        memcpy((uint8_t*)buf + copied, blockbuf + block_off, to_copy);
        copied += to_copy;
    }
    return (ssize_t)copied;
}
static void ext2_file_close(void *ctxp)
{
    kfree(ctxp);
}

static struct vfs_ops ext2_ops = {
    .mount = ext2_mount,
    .unmount = ext2_unmount,
    .open = ext2_open
};

struct vfs_ops *ext2_get_ops(void) { return &ext2_ops; }
