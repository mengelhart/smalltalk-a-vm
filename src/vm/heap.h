/* src/vm/heap.h
 * Actor-local heap allocator — Phase 1 single-actor variant.
 * Phase 1 — permanent. See ADR 007 (density), ADR 014 (stack slab growth).
 *
 * Phase 1 uses a single global heap. The API accepts a heap context pointer
 * so Phase 2 can add per-actor heaps without changing call sites.
 *
 * Bump allocator: objects are allocated contiguously from a nursery slab.
 * No GC in Phase 1 — the nursery is not collected, just filled.
 */
#pragma once
#include "oop.h"
#include <stddef.h>

typedef struct STA_Heap STA_Heap;

/* Create a heap with the given nursery capacity in bytes.
 * Capacity is rounded up to a multiple of 16.
 * Returns NULL on allocation failure. */
STA_Heap *sta_heap_create(size_t nursery_capacity);

/* Destroy the heap and free all backing memory. */
void sta_heap_destroy(STA_Heap *heap);

/* Allocate an object with the given class index and nwords payload slots.
 * Header is initialized (gc_flags = WHITE, obj_flags = 0, reserved = 0).
 * Payload words are zeroed. Allocation is 16-byte aligned.
 * Returns NULL if the nursery is full. */
STA_ObjHeader *sta_heap_alloc(STA_Heap *heap, uint32_t class_index, uint32_t nwords);

/* Bytes currently used in the nursery. */
size_t sta_heap_used(const STA_Heap *heap);

/* Total nursery capacity in bytes. */
size_t sta_heap_capacity(const STA_Heap *heap);
