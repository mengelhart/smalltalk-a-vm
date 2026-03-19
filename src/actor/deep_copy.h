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

/* Forward-declare STA_ClassTable to avoid pulling in class_table.h. */
typedef struct STA_ClassTable STA_ClassTable;

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
