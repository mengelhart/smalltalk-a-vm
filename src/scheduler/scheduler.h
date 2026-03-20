/* src/scheduler/scheduler.h
 * Production work-stealing scheduler — Phase 2 Epic 4.
 * See ADR 009 for design rationale.
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include "deque.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

/* Forward declarations. */
struct STA_VM;
struct STA_Actor;

/* ── Per-thread worker state ──────────────────────────────────────────── */

typedef struct STA_SchedWorker {
    pthread_t          thread;
    int                index;       /* 0-based thread index              */
    struct STA_Actor  *current;     /* actor being executed right now     */

    /* Per-thread Chase-Lev deque (Story 5). */
    STA_WorkDeque      deque;

    /* Statistics (diagnostic only, updated by owning thread). */
    uint64_t           actors_run;
    uint64_t           messages_processed;
    uint64_t           steals;
} STA_SchedWorker;

/* ── STA_Scheduler ────────────────────────────────────────────────────── */

typedef struct STA_Scheduler {
    struct STA_VM     *vm;

    STA_SchedWorker   *workers;       /* heap-allocated [num_threads]     */
    uint32_t           num_threads;

    _Atomic bool       running;       /* false = shutdown requested       */

    /* Counter for round-robin external enqueue distribution. */
    _Atomic uint32_t   enqueue_rr;

    /* Overflow queue: external enqueues (from non-worker threads)
     * go here. Workers drain it under wake_mutex. */
    struct STA_Actor  *overflow_head;  /* protected by wake_mutex */

    /* Idle-thread wakeup (pthread_cond_signal per ADR 009). */
    pthread_mutex_t    wake_mutex;
    pthread_cond_t     wake_cond;
} STA_Scheduler;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

int  sta_scheduler_init(struct STA_VM *vm, uint32_t num_threads);
int  sta_scheduler_start(struct STA_VM *vm);
void sta_scheduler_stop(struct STA_VM *vm);
void sta_scheduler_destroy(struct STA_VM *vm);

/* ── Enqueue ──────────────────────────────────────────────────────────── */

/* Enqueue an actor for scheduling. Thread-safe.
 * Pushes to a worker deque (round-robin) and signals idle threads. */
void sta_scheduler_enqueue(STA_Scheduler *sched, struct STA_Actor *actor);
