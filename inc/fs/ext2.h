#pragma once
#include <stddef.h>

/* Minimal ext2 read-only support for mounting block devices via VFS.
 * Only supports basic ops: mount, open, read, close for files in the root tree.
 */

struct vfs_ops *ext2_get_ops(void);
