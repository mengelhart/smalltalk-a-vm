/* src/scheduler/deque.h
 * Chase-Lev fixed-capacity work-stealing deque — Phase 2 Epic 4 Story 5.
 * Per ADR 009: proven in Spike 003, TSan clean.
 *
 * Owner thread: push/pop from the bottom (LIFO — cache-friendly).
 * Stealing threads: steal from the top (FIFO — fairness).
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* Forward declaration. */
struct STA_Actor;

/* Deque capacity: power-of-two, fixed. */
#define STA_DEQUE_CAPACITY  1024u
#define STA_DEQUE_MASK      (STA_DEQUE_CAPACITY - 1u)

typedef struct STA_WorkDeque {
    _Atomic uint32_t                  top;
    _Atomic uint32_t                  bottom;
    _Atomic(struct STA_Actor *)       buf[STA_DEQUE_CAPACITY];
} STA_WorkDeque;

void             sta_deque_init (STA_WorkDeque *dq);
int              sta_deque_push (STA_WorkDeque *dq, struct STA_Actor *a);
struct STA_Actor *sta_deque_pop  (STA_WorkDeque *dq);
struct STA_Actor *sta_deque_steal(STA_WorkDeque *dq);
int              sta_deque_size (const STA_WorkDeque *dq);
