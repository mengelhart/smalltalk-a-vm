/* src/actor/deep_copy.h
 * Cross-actor deep copy engine — Phase 2 Epic 3.
 * Recursively copies an object graph from one heap to another.
 * Immutable objects (SmallInts, Characters, Symbols, anything with
 * STA_OBJ_IMMUTABLE) are shared by pointer — never copied.
 * Uses a visited set hash map for cycle detection and shared structure
 * preservation. Resolves open decision #9 from PROJECT_STATUS.md.
 * See ADR 008.
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include "vm/oop.h"
#include "vm/heap.h"

/* Forward declarations. */
typedef struct STA_ClassTable STA_ClassTable;
struct STA_VM;
struct STA_Actor;

/* ── Deep copy ───────────────────────────────────────────────────────── */

/* Deep copy the object graph rooted at 'root' from source heap to
 * target heap. Returns the copy of root on the target heap.
 *
 * Behavior:
 * - Immediates (SmallInt, Character) pass through unchanged.
 * - Immutable objects (STA_OBJ_IMMUTABLE flag) are shared by pointer.
 * - Mutable heap objects are recursively copied onto target heap.
 * - Visited set ensures:
 *   - Cycles don't cause infinite recursion.
 *   - Shared sub-objects are copied once, not duplicated.
 * - Byte-indexable objects (String, ByteArray) have their raw bytes
 *   memcpy'd (not scanned as OOPs).
 *
 * class_table is needed to look up class format (byte vs OOP indexable).
 *
 * Returns 0 on allocation failure. */
STA_OOP sta_deep_copy(STA_OOP root,
                       STA_Heap *source,
                       STA_Heap *target,
                       STA_ClassTable *class_table);

/* GC-aware variant: uses sta_heap_alloc_gc on the target actor's heap,
 * so allocation failure triggers GC before giving up.
 * The target_actor must be the actor that owns the target heap.
 * vm is needed for GC (immutable space bounds, class table).
 *
 * Returns 0 on allocation failure (after GC + retry). */
STA_OOP sta_deep_copy_gc(STA_OOP root,
                          STA_Heap *source,
                          struct STA_VM *vm,
                          struct STA_Actor *target_actor,
                          STA_ClassTable *class_table);

/* ── Transfer copy (for future reply routing, Epic 7A) ────────────────── */

/* Deep copy the object graph rooted at 'root' into a standalone
 * transfer heap (malloc'd, not owned by any actor). Used by reply
 * routing to deep-copy a mutable return value out of the target
 * actor's heap into a transfer buffer that the sender can later
 * copy from.
 *
 * Same semantics as sta_deep_copy: immediates and immutables pass
 * through unchanged. Mutable heap objects are recursively copied
 * into dst_heap (which must be initialized by the caller).
 *
 * Returns the copy of root in dst_heap. Returns 0 on failure. */
STA_OOP sta_deep_copy_to_transfer(STA_OOP root,
                                   STA_Heap *src_heap,
                                   STA_Heap *dst_heap,
                                   struct STA_VM *vm);

/* ── Pre-flight size estimation ──────────────────────────────────────── */

/* Estimate the total heap bytes needed to deep copy the object graph
 * rooted at 'root'. Walks the same graph as deep copy (skipping
 * immediates, nil, and immutables) but only measures — no allocation.
 *
 * Uses a visited set to avoid double-counting shared sub-objects in DAGs.
 * Safe with cycles.
 *
 * Returns the estimated number of bytes (aligned allocations). */
size_t sta_deep_copy_estimate(STA_OOP root, STA_ClassTable *class_table);

/* Estimate total bytes for an array of roots (e.g., message args).
 * Uses a single visited set across all roots so shared sub-objects
 * between args are not double-counted. */
size_t sta_deep_copy_estimate_roots(STA_OOP *roots, uint8_t count,
                                     STA_ClassTable *class_table);
