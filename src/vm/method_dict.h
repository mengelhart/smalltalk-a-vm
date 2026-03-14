/* src/vm/method_dict.h
 * MethodDictionary — Smalltalk object for selector → method lookup.
 * Phase 1 — permanent. See ADR 013 (method dispatch).
 *
 * A MethodDictionary is a heap-allocated Smalltalk object
 * (class_index = STA_CLS_METHODDICTIONARY, 15).
 *
 * Payload layout:
 *   slot 0: tally   — SmallInt OOP (number of entries)
 *   slot 1: array   — OOP → backing Array object
 *
 * The backing Array has capacity * 2 slots laid out as:
 *   [selector0, method0, selector1, method1, …]
 *
 * Lookup: hash(selector) mod capacity → index, linear probe by 1
 * (stepping by 2 in the array to skip method slots).
 *
 * Concurrency (Phase 1 → Phase 2 contract):
 *   Phase 1 is single-threaded. Phase 2 will use atomic swap of the
 *   backing array pointer (slot 1) for lock-free reads during dispatch.
 *   The write path will be serialized by install_lock (ADR 013).
 *   Do not implement atomics or locks in Phase 1 — document only.
 */
#pragma once
#include "oop.h"
#include "heap.h"
#include <stdint.h>

/* ── Create ────────────────────────────────────────────────────────────── */

/* Allocate a MethodDictionary + backing Array on the heap.
 * capacity is the number of selector/method pairs (not the Array size,
 * which is capacity * 2). Returns the MethodDictionary OOP, or 0 on failure. */
STA_OOP sta_method_dict_create(STA_Heap *heap, uint32_t capacity);

/* ── Lookup ────────────────────────────────────────────────────────────── */

/* Look up a selector in the dictionary. Returns the method OOP, or 0
 * if the selector is not found. Hash is obtained via sta_symbol_get_hash(). */
STA_OOP sta_method_dict_lookup(STA_OOP dict, STA_OOP selector);

/* ── Insert ────────────────────────────────────────────────────────────── */

/* Insert a (selector, method) pair. Returns 0 on success, -1 on failure.
 * Grows the backing array if load factor exceeds 70%: allocates a new
 * Array, rehashes all entries, stores the new Array OOP in dict slot 1,
 * and updates the tally in slot 0. */
int sta_method_dict_insert(STA_Heap *heap, STA_OOP dict,
                           STA_OOP selector, STA_OOP method);
