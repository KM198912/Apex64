#pragma once
#include <stddef.h>
#include <stdint.h>

/* Small ssize_t typedef for kernel use */
typedef intptr_t ssize_t;

/* Minimal VFS interfaces for mounting & opening files.
 * This is intentionally small: add more ops as needed.
 */

/* Generic file handle layout used by vfs & fs implementations */
struct vfs_fh {
    ssize_t (*read)(void *ctx, void *buf, size_t offset, size_t len);
    void (*close)(void *ctx);
    void *ctx;
};

struct vfs_mount {
    const char *mount_point; /* prefix, e.g. "/" or "/boot" */
    void *fs_data;
    struct vfs_ops *ops;
};

struct vfs_ops {
    void *(*mount)(void *mount_data); /* return fs-specific handle */
    void (*unmount)(void *fs);
    void *(*open)(void *fs, const char *path, size_t *out_size);
    /* the vfs layer calls read via the returned handle (struct vfs_fh) */
    /* optional: readdir, stat */
};

/* Mount a filesystem at a given path. Returns 0 on success */
int vfs_mount(const char *path, struct vfs_ops *ops, void *mount_data);

/* Open a file by path. Returns an opaque file handle (struct vfs_fh*) or NULL */
void *vfs_open(const char *path, size_t *out_size);
ssize_t vfs_read(void *fh, void *buf, size_t offset, size_t len);
void vfs_close(void *fh);

/* Helper: convenience wrapper to read an entire file into a buffer allocated by caller */
ssize_t vfs_read_all(const char *path, void *buf, size_t buf_len);

/* Simple integer FD API (wrapper around handle API) */
int vfs_fd_open(const char *path);
ssize_t vfs_fd_read(int fd, void *buf, size_t offset, size_t len);
int vfs_fd_close(int fd);
