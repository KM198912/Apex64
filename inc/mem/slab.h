#pragma once
#include <stddef.h>
#include <stdint.h>

/* Simple slab allocator for small objects (power-of-two size classes)
 * Supported sizes: 16,32,64,128,256,512,1024,2048
 */
void slab_init(void);
void *slab_alloc(size_t size);
void slab_free(void *ptr);

/* Debug helpers */
size_t slab_free_objects(size_t size_class);

/* Magazines: configure max CPUs & magazine size for slab */
#define SLAB_MAX_CPUS 4
#define SLAB_MAGAZINE_SIZE 16
