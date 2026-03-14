/* src/vm/heap.c
 * Actor-local heap — bump allocator on an aligned slab.
 * Phase 1 — see heap.h for interface documentation.
 */
#include "heap.h"
#include <stdlib.h>
#include <string.h>

struct STA_Heap {
    char  *base;      /* 16-byte aligned slab */
    size_t capacity;  /* total usable bytes   */
    size_t used;      /* bytes consumed       */
};

STA_Heap *sta_heap_create(size_t capacity) {
    /* Round up to 16-byte multiple. */
    capacity = (capacity + 15u) & ~(size_t)15u;
    if (capacity == 0) capacity = 16u;

    STA_Heap *heap = malloc(sizeof(*heap));
    if (!heap) return NULL;

    /* aligned_alloc: alignment must divide size; both are multiples of 16. */
    heap->base = aligned_alloc(16, capacity);
    if (!heap->base) { free(heap); return NULL; }

    memset(heap->base, 0, capacity);
    heap->capacity = capacity;
    heap->used     = 0;
    return heap;
}

void sta_heap_destroy(STA_Heap *heap) {
    if (!heap) return;
    free(heap->base);
    free(heap);
}

STA_ObjHeader *sta_heap_alloc(STA_Heap *heap, uint32_t class_index, uint32_t nwords) {
    size_t raw   = sta_alloc_size(nwords);
    /* Round up to 16-byte multiple so the next object starts 16-byte aligned. */
    size_t bytes = (raw + 15u) & ~(size_t)15u;

    if (heap->used + bytes > heap->capacity) return NULL;

    STA_ObjHeader *h = (STA_ObjHeader *)(heap->base + heap->used);
    heap->used += bytes;

    /* Header init — payload is already zeroed (slab was zeroed at creation). */
    h->class_index = class_index;
    h->size        = nwords;
    h->gc_flags    = STA_GC_WHITE;
    h->obj_flags   = 0;
    h->reserved    = 0;
    return h;
}

size_t sta_heap_used(const STA_Heap *heap) {
    return heap->used;
}

size_t sta_heap_capacity(const STA_Heap *heap) {
    return heap->capacity;
}
