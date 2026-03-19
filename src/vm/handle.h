/* src/vm/handle.h
 * Handle table — reference-counted, growable, per-VM.
 * Per ADR 013: STA_Handle* is stable; OOP updated in-place on GC move.
 * Growth allocates a new slab; old slabs are never freed; all pointers
 * remain valid for the table's lifetime.
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include "oop.h"
#include <stdint.h>
#include <stdbool.h>

/* ── STA_Handle (the entry the public API pointer refers to) ──────────── */

struct STA_Handle {
    STA_OOP   oop;          /* the rooted object (updated by GC on move) */
    uint32_t  refcount;     /* 0 = free slot */
    uint32_t  next_free;    /* index of next free slot (when refcount == 0),
                               UINT32_MAX = end of free list.
                               Encoded as global flat index across all slabs. */
};

/* ── Slab ─────────────────────────────────────────────────────────────── */

#define STA_HANDLE_SLAB_SIZE 64  /* entries per slab */

typedef struct STA_HandleSlab {
    struct STA_Handle entries[STA_HANDLE_SLAB_SIZE];
    struct STA_HandleSlab *next;  /* linked list of slabs */
} STA_HandleSlab;

/* ── STA_HandleTable ──────────────────────────────────────────────────── */

typedef struct STA_HandleTable {
    STA_HandleSlab *first;       /* head of slab chain */
    STA_HandleSlab *last;        /* tail — append new slabs here */
    uint32_t        slab_count;  /* number of slabs */
    uint32_t        count;       /* number of live (refcount > 0) handles */
    uint32_t        free_head;   /* flat index of first free slot, UINT32_MAX = none */
} STA_HandleTable;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

int  sta_handle_table_init(STA_HandleTable *t);
void sta_handle_table_destroy(STA_HandleTable *t);

/* ── Operations ───────────────────────────────────────────────────────── */

/* Create a new handle for the given OOP. Returns NULL on OOM. */
struct STA_Handle *sta_handle_create(STA_HandleTable *t, STA_OOP oop);

/* Read the current OOP from a handle. */
STA_OOP sta_handle_get(struct STA_Handle *h);

/* Increment refcount. Returns the same pointer. */
struct STA_Handle *sta_handle_retain_entry(STA_HandleTable *t, struct STA_Handle *h);

/* Decrement refcount. Frees the slot when it reaches zero. */
void sta_handle_release_entry(STA_HandleTable *t, struct STA_Handle *h);
