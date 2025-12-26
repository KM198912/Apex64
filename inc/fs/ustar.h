#pragma once
#include <stddef.h>

/* Mount a ustar archive from memory and return a vfs mount handle
 * The returned handle is owned by the VFS and freed on unmount.
 */
void *ustar_mount_from_memory(void *base, size_t size);

/* Helper: create and return a prefilled vfs_ops for ustar */
struct vfs_ops *ustar_get_ops(void);
