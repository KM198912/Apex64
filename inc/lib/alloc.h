#pragma once
#include <stddef.h>

/* Kernel allocator: small objects via slab, large allocations are page-backed. */
void *kmalloc(size_t size);
void kfree(void *ptr);
