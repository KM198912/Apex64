#pragma once

/* Retrieve the value for a kernel command-line key.
 * Returns an allocated string (caller must free via kfree) or NULL.
 */
char *cmdline_get(const char *key);
