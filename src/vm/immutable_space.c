/* src/vm/immutable_space.c
 * Shared immutable region — mmap-backed bump allocator.
 * Phase 1 — see immutable_space.h for interface documentation.
 */
#include "immutable_space.h"
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

struct STA_ImmutableSpace {
    char  *base;      /* page-aligned mmap'd region  */
    size_t capacity;  /* total mapped bytes           */
    size_t used;      /* bytes consumed so far        */
    int    sealed;    /* 1 after mprotect(PROT_READ)  */
};

static size_t page_round_up(size_t n) {
    long pgsz = sysconf(_SC_PAGESIZE);
    size_t p = (size_t)pgsz;
    return (n + p - 1) / p * p;
}

STA_ImmutableSpace *sta_immutable_space_create(size_t min_capacity) {
    if (min_capacity == 0) min_capacity = 1;
    size_t capacity = page_round_up(min_capacity);

    STA_ImmutableSpace *sp = malloc(sizeof(*sp));
    if (!sp) return NULL;

    void *mem = mmap(NULL, capacity,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON,
                     -1, 0);
    if (mem == MAP_FAILED) { free(sp); return NULL; }

    sp->base     = mem;
    sp->capacity = capacity;
    sp->used     = 0;
    sp->sealed   = 0;
    return sp;
}

void sta_immutable_space_destroy(STA_ImmutableSpace *sp) {
    if (!sp) return;
    /* Unseal before munmap so the kernel can reclaim the pages cleanly. */
    if (sp->sealed) {
        mprotect(sp->base, sp->capacity, PROT_READ | PROT_WRITE);
    }
    if (sp->base) munmap(sp->base, sp->capacity);
    free(sp);
}

STA_ObjHeader *sta_immutable_alloc(STA_ImmutableSpace *sp,
                                   uint32_t class_index, uint32_t nwords) {
    if (sp->sealed) return NULL;

    /* Align bump pointer to 16-byte boundary (STA_ALLOC_UNIT). */
    size_t aligned = (sp->used + 15u) & ~(size_t)15u;
    size_t bytes   = sta_alloc_size(nwords);

    if (aligned + bytes > sp->capacity) return NULL;

    sp->used = aligned + bytes;
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
    return sp->used;
}
