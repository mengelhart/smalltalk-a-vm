/* src/vm/heap.c
 * Actor-local heap — bump allocator on an aligned slab.
 * Phase 1 — see heap.h for interface documentation.
 */
#include "heap.h"
#include <stdlib.h>
#include <string.h>

int sta_heap_init(STA_Heap *heap, size_t capacity) {
    capacity = (capacity + 15u) & ~(size_t)15u;
    if (capacity == 0) capacity = 16u;

    heap->base = aligned_alloc(16, capacity);
    if (!heap->base) return -1;

    memset(heap->base, 0, capacity);
    heap->capacity = capacity;
    heap->used     = 0;
    return 0;
}

void sta_heap_deinit(STA_Heap *heap) {
    if (!heap) return;
    free(heap->base);
    heap->base = NULL;
    heap->capacity = 0;
    heap->used = 0;
}

STA_Heap *sta_heap_create(size_t capacity) {
    STA_Heap *heap = malloc(sizeof(*heap));
    if (!heap) return NULL;

    if (sta_heap_init(heap, capacity) != 0) {
        free(heap);
        return NULL;
    }
    return heap;
}

void sta_heap_destroy(STA_Heap *heap) {
    if (!heap) return;
    sta_heap_deinit(heap);
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
