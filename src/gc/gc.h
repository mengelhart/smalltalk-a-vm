/* src/gc/gc.h
 * Per-actor Cheney semi-space copying garbage collector.
 * Phase 2 Epic 5 — permanent. See ADR 007 (gc_flags, forwarding).
 *
 * Each actor has an independent mutable heap. When allocation fails,
 * sta_gc_collect() copies live objects from the current heap (from-space)
 * to a newly allocated region (to-space), installs forwarding pointers,
 * and frees the old region. No cross-actor coordination is needed.
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include "vm/oop.h"
#include "vm/heap.h"
#include <stddef.h>
#include <stdint.h>

/* Forward declarations. */
struct STA_VM;
struct STA_Actor;

/* ── Public GC API ─────────────────────────────────────────────────────── */

/* Run a Cheney semi-space collection on the actor's heap.
 * After return, the actor's heap fields (base, capacity, used) point to
 * the new to-space. The old from-space is freed.
 *
 * Returns 0 (STA_OK) on success, negative on failure.
 * Caller should retry allocation after a successful GC. */
int sta_gc_collect(struct STA_VM *vm, struct STA_Actor *actor);

/* ── GC-aware allocation ───────────────────────────────────────────────── */

/* Allocate an object on the actor's heap, triggering GC if needed.
 *
 * Flow:
 *   1. Try sta_heap_alloc → success? return.
 *   2. Run sta_gc_collect → retry alloc → success? return.
 *   3. Grow heap (heap growth policy) → retry alloc → success? return.
 *   4. Return NULL (out of memory).
 *
 * IMPORTANT: caller must set actor->saved_frame to the current innermost
 * frame BEFORE calling this function, so GC can walk the stack roots.
 * After return, any OOP stored in a C local variable that pointed into
 * the old heap is STALE — re-read from rooted locations. */
STA_ObjHeader *sta_heap_alloc_gc(struct STA_VM *vm, struct STA_Actor *actor,
                                  uint32_t class_index, uint32_t nwords);

/* Grow the actor's heap to at least min_capacity bytes.
 * Copies all current content to a new larger region.
 * Returns 0 on success, -1 on allocation failure. */
int sta_heap_grow(STA_Heap *heap, size_t min_capacity);

/* ── GC statistics ─────────────────────────────────────────────────────── */

typedef struct STA_GCStats {
    uint32_t gc_count;              /* number of collections              */
    uint32_t _pad;                  /* alignment padding                  */
    size_t   gc_bytes_reclaimed;    /* cumulative bytes freed             */
    size_t   gc_bytes_survived;     /* bytes surviving most recent GC     */
    /* current_heap_size: read from actor->heap.capacity directly. */
} STA_GCStats;
/* sizeof = 24 bytes on LP64 (4 + 4 + 8 + 8) */
