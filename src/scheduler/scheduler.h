/* src/scheduler/scheduler.h
 * Production work-stealing scheduler — Phase 2 Epic 4.
 * See ADR 009 for design rationale.
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

/* Forward declarations. */
struct STA_VM;
struct STA_Actor;

/* ── Run queue (mutex-protected FIFO) ─────────────────────────────────── */
/* Stories 1-4 use a global FIFO queue. Story 5 replaces with per-thread
 * Chase-Lev deques. */

typedef struct STA_RunQueue {
    struct STA_Actor *head;
    struct STA_Actor *tail;
    pthread_mutex_t   mutex;
    uint32_t          count;
} STA_RunQueue;

/* ── Per-thread worker state ──────────────────────────────────────────── */

typedef struct STA_SchedWorker {
    pthread_t          thread;
    int                index;       /* 0-based thread index              */
    struct STA_Actor  *current;     /* actor being executed right now     */

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

    STA_RunQueue       run_queue;     /* global FIFO (Stories 1-4)        */

    _Atomic bool       running;       /* false = shutdown requested       */

    /* Idle-thread wakeup (pthread_cond_signal per ADR 009). */
    pthread_mutex_t    wake_mutex;
    pthread_cond_t     wake_cond;
} STA_Scheduler;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

/* Initialize the scheduler with the given number of threads.
 * Does not start any OS threads yet. */
int  sta_scheduler_init(struct STA_VM *vm, uint32_t num_threads);

/* Start the scheduler — launches worker threads. */
int  sta_scheduler_start(struct STA_VM *vm);

/* Stop the scheduler — signals stop, wakes all workers, joins threads. */
void sta_scheduler_stop(struct STA_VM *vm);

/* Destroy the scheduler — free worker array, destroy sync primitives. */
void sta_scheduler_destroy(struct STA_VM *vm);

/* ── Run queue operations ─────────────────────────────────────────────── */

/* Enqueue an actor onto the global run queue. Thread-safe.
 * Signals idle threads. */
void sta_scheduler_enqueue(STA_Scheduler *sched, struct STA_Actor *actor);

/* Dequeue an actor from the global run queue. Thread-safe.
 * Returns NULL if the queue is empty. */
struct STA_Actor *sta_scheduler_dequeue(STA_Scheduler *sched);
