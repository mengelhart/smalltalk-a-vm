/* src/vm/immutable_space.h
 * Shared immutable region allocator.
 * Phase 1 — permanent. See ADR 007 (STA_OBJ_IMMUTABLE, STA_OBJ_SHARED_IMM).
 *
 * Bootstrap-time bump allocator for nil, true, false, symbols, class objects,
 * and any other object that lives in the shared immutable region.
 * After bootstrap, the region is sealed read-only via mprotect — subsequent
 * writes trigger SIGSEGV (the desired behavior).
 *
 * All objects allocated here have STA_OBJ_IMMUTABLE | STA_OBJ_SHARED_IMM
 * set in obj_flags.
 */
#pragma once
#include "oop.h"
#include <stddef.h>

typedef struct STA_ImmutableSpace STA_ImmutableSpace;

/* Create a space of at least min_capacity bytes.
 * Actual capacity is rounded up to the system page size.
 * Returns NULL on failure. */
STA_ImmutableSpace *sta_immutable_space_create(size_t min_capacity);

/* Destroy the space and unmap its memory.
 * Unseals if necessary before unmapping. */
void sta_immutable_space_destroy(STA_ImmutableSpace *sp);

/* Allocate an object with the given class index and nwords payload slots.
 * Header is initialized; obj_flags gets IMMUTABLE | SHARED_IMM.
 * Payload words are zeroed. Returns NULL if the space is full or sealed. */
STA_ObjHeader *sta_immutable_alloc(STA_ImmutableSpace *sp,
                                   uint32_t class_index, uint32_t nwords);

/* Seal the region read-only (mprotect PROT_READ).
 * No further allocations are allowed after sealing.
 * Returns 0 on success, -1 on mprotect failure. */
int sta_immutable_space_seal(STA_ImmutableSpace *sp);

/* Query whether the space has been sealed. */
int sta_immutable_space_is_sealed(const STA_ImmutableSpace *sp);

/* Base address of the mapped region (for contiguity checks). */
const void *sta_immutable_space_base(const STA_ImmutableSpace *sp);

/* Bytes allocated so far. */
size_t sta_immutable_space_used(const STA_ImmutableSpace *sp);
