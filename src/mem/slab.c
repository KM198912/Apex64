#include <mem/slab.h>
#include <mem/pmm.h>
#include <kernel/kprintf.h>
#include <common/boot.h>
#include <lib/string.h>
#include <stdint.h>
static const size_t slab_sizes[] = {16,32,64,128,256,512,1024,2048};
#define SLAB_CLASS_COUNT (sizeof(slab_sizes)/sizeof(slab_sizes[0]))

struct slab_page {
    struct slab_page *next;
    uint32_t obj_size;
    uint16_t free_count;
    uint16_t objs_per_page;
    void *free_list; /* pointer to first free object */
};

struct magazine {
    void *objs[SLAB_MAGAZINE_SIZE];
    uint16_t count;
};

struct slab_cache {
    size_t obj_size;
    struct slab_page *partial; /* pages with free objects */
    struct magazine mags[SLAB_MAX_CPUS]; /* per-cpu magazines */
};

static struct slab_cache caches[SLAB_CLASS_COUNT];

/* current CPU id stub (returns 0 for now) - replace with SMP-aware routine later */
static inline int get_cpu_id(void) { return 0; }

static int size_to_index(size_t sz)
{
    for (size_t i = 0; i < SLAB_CLASS_COUNT; ++i) if (sz <= slab_sizes[i]) return (int)i;
    return -1;
}

static struct slab_page *create_slab_page(size_t obj_size)
{
    uint64_t phys = pmm_alloc_frame();
    if (!phys) return NULL;
    void *base = PHYS_TO_VIRT(phys);
    memset(base, 0, PAGE_SIZE);
    struct slab_page *sp = (struct slab_page*)base;
    sp->next = NULL;
    sp->obj_size = (uint32_t)obj_size;

    size_t hdr = sizeof(struct slab_page);
    size_t usable = PAGE_SIZE - hdr;
    sp->objs_per_page = (uint16_t)(usable / obj_size);
    sp->free_count = sp->objs_per_page;
    sp->free_list = NULL;

    uint8_t *data = (uint8_t*)base + hdr;
    for (size_t i = 0; i < sp->objs_per_page; ++i) {
        void *obj = data + i * obj_size;
        /* store next pointer in the object itself */
        *(void**)obj = sp->free_list;
        sp->free_list = obj;
    }
    return sp;
}

void slab_init(void)
{
    for (size_t i = 0; i < SLAB_CLASS_COUNT; ++i) {
        caches[i].obj_size = slab_sizes[i];
        caches[i].partial = NULL;
        for (size_t c = 0; c < SLAB_MAX_CPUS; ++c) {
            caches[i].mags[c].count = 0;
        }
    }
    kprintf("slab: initialized size classes up to %zu, mags=%d cpus size=%d\n",
            slab_sizes[SLAB_CLASS_COUNT-1], SLAB_MAX_CPUS, SLAB_MAGAZINE_SIZE);
}

void *slab_alloc(size_t size)
{
    int idx = size_to_index(size);
    if (idx < 0) return NULL; /* too big for slab */

    struct slab_cache *c = &caches[idx];
    int cpu = get_cpu_id();

    /* Try pop from magazine first (per-cpu, lockless for now) */
    struct magazine *m = &c->mags[cpu];
    if (m->count > 0) {
        void *obj = m->objs[--m->count];
        return obj;
    }

    /* Fall back to partial slab page */
    struct slab_page *sp = c->partial;
    if (!sp || sp->free_count == 0) {
        sp = create_slab_page(c->obj_size);
        if (!sp) return NULL;
        sp->next = c->partial;
        c->partial = sp;
    }

    void *obj = sp->free_list;
    sp->free_list = *(void**)obj;
    sp->free_count--;
    return obj;
}

void slab_free(void *ptr)
{
    if (!ptr) return;
    uintptr_t page_base = (uintptr_t)ptr & ~(PAGE_SIZE - 1);
    struct slab_page *sp = (struct slab_page*)page_base;
    size_t obj_size = sp->obj_size;
    if (obj_size == 0 || sp->objs_per_page == 0) {
        /* not a slab page */
        return;
    }

    int idx = size_to_index(obj_size);
    if (idx < 0) return;

    struct slab_cache *c = &caches[idx];
    int cpu = get_cpu_id();
    struct magazine *m = &c->mags[cpu];

    /* Try to push into magazine first */
    if (m->count < SLAB_MAGAZINE_SIZE) {
        m->objs[m->count++] = ptr;
        return;
    }

    /* push back onto page free list */
    *(void**)ptr = sp->free_list;
    sp->free_list = ptr;
    sp->free_count++;

    if (sp->free_count == sp->objs_per_page) {
        /* fully free: if we have a spare magazine page capacity, we can keep the page; but for now return to PMM */
        struct slab_page **pp = &c->partial;
        while (*pp && *pp != sp) pp = &((*pp)->next);
        if (*pp == sp) {
            *pp = sp->next;
        }
        uint64_t phys = (uint64_t)page_base;
        pmm_free_frame(phys);
    }
}

size_t slab_free_objects(size_t size_class)
{
    int idx = size_to_index(size_class);
    if (idx < 0) return 0;
    struct slab_cache *c = &caches[idx];
    size_t total = 0;
    for (struct slab_page *p = c->partial; p; p = p->next) total += p->free_count;
    /* include magazines */
    for (int cpu = 0; cpu < SLAB_MAX_CPUS; ++cpu) total += c->mags[cpu].count;
    return total;
}
