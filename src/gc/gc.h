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

/* ── GC statistics (Story 6 placeholder) ───────────────────────────────── */

typedef struct STA_GCStats {
    uint32_t gc_count;              /* number of collections              */
    size_t   gc_bytes_reclaimed;    /* cumulative bytes freed             */
    size_t   gc_bytes_survived;     /* bytes surviving most recent GC     */
    size_t   current_heap_size;     /* current heap capacity              */
} STA_GCStats;
