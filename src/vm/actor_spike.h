/* src/vm/actor_spike.h
 * Phase 0 spike: actor struct layout for density measurement.
 * NOT the permanent implementation.
 */
#pragma once
#include <stdint.h>
#include <stdatomic.h>
#include "oop_spike.h"

/* Mailbox node — one per queued message */
typedef struct STA_MboxNode {
    struct STA_MboxNode *next;   /* 8 bytes — MPSC queue link */
    STA_OOP              msg;    /* 8 bytes — the message OOP */
} STA_MboxNode;                  /* 16 bytes */

/* Lock-free MPSC mailbox — stub head/tail pointers */
typedef struct {
    _Atomic(STA_MboxNode *) head;    /* 8 bytes */
    STA_MboxNode           *tail;    /* 8 bytes — written only by owner */
} STA_Mailbox;                       /* 16 bytes */

/* Actor runtime struct */
typedef struct STA_Actor {
    /* Identity and dispatch */
    uint32_t   class_index;     /* 4 bytes — class of this actor */
    uint32_t   actor_id;        /* 4 bytes — unique within the image */

    /* Concurrency */
    STA_Mailbox mailbox;        /* 16 bytes */
    _Atomic uint32_t reductions;/* 4 bytes — budget counter */
    uint32_t   sched_flags;     /* 4 bytes — runnable, suspended, etc. */

    /* Heap */
    void      *heap_base;       /* 8 bytes — start of nursery slab */
    void      *heap_bump;       /* 8 bytes — next allocation point */
    void      *heap_limit;      /* 8 bytes — end of nursery slab */

    /* Supervision */
    struct STA_Actor *supervisor;  /* 8 bytes — pointer to supervisor actor */
    uint32_t   restart_strategy;   /* 4 bytes */
    uint32_t   restart_count;      /* 4 bytes */

    /* Capability address (opaque, unforgeable within runtime) */
    uint64_t   capability_token;   /* 8 bytes */
} STA_Actor;
/* Target: sizeof(STA_Actor) <= 96 bytes.
 * Per-actor creation budget:
 *   sizeof(STA_Actor)  = measured
 *   initial nursery    = 128 bytes (minimum viable slab)
 *   actor identity obj = STA_HEADER_ALIGNED + 0 payload = 16 bytes
 *   ─────────────────────────────────────────────────
 *   total              = should be <= 300 bytes        */
