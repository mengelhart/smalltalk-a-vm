/* src/vm/immutable_space.c
 * Shared immutable region — mmap-backed bump allocator.
 * Phase 1 — see immutable_space.h for interface documentation.
 */
#include "immutable_space.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

static size_t page_round_up(size_t n) {
    long pgsz = sysconf(_SC_PAGESIZE);
    size_t p = (size_t)pgsz;
    return (n + p - 1) / p * p;
}

int sta_immutable_space_init(STA_ImmutableSpace *sp, size_t min_capacity) {
    if (min_capacity == 0) min_capacity = 1;
    size_t capacity = page_round_up(min_capacity);

    void *mem = mmap(NULL, capacity,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON,
                     -1, 0);
    if (mem == MAP_FAILED) return -1;

    sp->base     = mem;
    sp->capacity = capacity;
    atomic_store_explicit(&sp->used, 0, memory_order_relaxed);
    sp->sealed   = 0;
    return 0;
}

void sta_immutable_space_deinit(STA_ImmutableSpace *sp) {
    if (!sp) return;
    if (sp->sealed) {
        mprotect(sp->base, sp->capacity, PROT_READ | PROT_WRITE);
    }
    if (sp->base) munmap(sp->base, sp->capacity);
    sp->base = NULL;
    sp->capacity = 0;
    atomic_store_explicit(&sp->used, 0, memory_order_relaxed);
    sp->sealed = 0;
}

STA_ImmutableSpace *sta_immutable_space_create(size_t min_capacity) {
    STA_ImmutableSpace *sp = malloc(sizeof(*sp));
    if (!sp) return NULL;

    if (sta_immutable_space_init(sp, min_capacity) != 0) {
        free(sp);
        return NULL;
    }
    return sp;
}

void sta_immutable_space_destroy(STA_ImmutableSpace *sp) {
    if (!sp) return;
    sta_immutable_space_deinit(sp);
    free(sp);
}

STA_ObjHeader *sta_immutable_alloc(STA_ImmutableSpace *sp,
                                   uint32_t class_index, uint32_t nwords) {
    if (sp->sealed) return NULL;

    /* Align bump pointer to 16-byte boundary (STA_ALLOC_UNIT). */
    size_t cur = atomic_load_explicit(&sp->used, memory_order_relaxed);
    size_t aligned = (cur + 15u) & ~(size_t)15u;
    size_t bytes   = sta_alloc_size(nwords);

    if (aligned + bytes > sp->capacity) return NULL;

    /* Release store: ensures the object's header+payload writes are visible
     * to any thread that reads 'used' to determine immutable space bounds. */
    atomic_store_explicit(&sp->used, aligned + bytes, memory_order_release);
    STA_ObjHeader *h = (STA_ObjHeader *)(sp->base + aligned);

    /* Zero the full allocation (header + padding + payload). */
    memset(h, 0, bytes);

    h->class_index = class_index;
    h->size        = nwords;
    h->gc_flags    = STA_GC_WHITE;
    h->obj_flags   = STA_OBJ_IMMUTABLE | STA_OBJ_SHARED_IMM;
    return h;
}

int sta_immutable_space_seal(STA_ImmutableSpace *sp) {
    if (sp->sealed) return 0;
    if (mprotect(sp->base, sp->capacity, PROT_READ) != 0) return -1;
    sp->sealed = 1;
    return 0;
}

int sta_immutable_space_is_sealed(const STA_ImmutableSpace *sp) {
    return sp->sealed;
}

const void *sta_immutable_space_base(const STA_ImmutableSpace *sp) {
    return sp->base;
}

size_t sta_immutable_space_used(const STA_ImmutableSpace *sp) {
    /* Relaxed load: GC just needs a consistent value to determine whether
     * an OOP falls within the immutable region. The release store in
     * sta_immutable_alloc ensures the object is fully written before 'used'
     * advances past it. A relaxed load may see a slightly stale bound,
     * which is safe — it just means a very recently allocated immutable
     * object might not be recognized as immutable and would be skipped
     * by the in_from_space check instead. */
    return atomic_load_explicit(
        &((STA_ImmutableSpace *)sp)->used, memory_order_relaxed);
}
