#pragma once

/* Parse a basic /etc/fstab and mount entries.
 * Only supports simple whitespace-separated lines: device mountpoint fstype ...
 * Returns 0 on success (no fatal errors), -1 on failure.
 */
int fstab_parse_and_mount(const char *path);
