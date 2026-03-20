/* src/scheduler/scheduler.c
 * Production work-stealing scheduler — Phase 2 Epic 4.
 * See scheduler.h, deque.h, and ADR 009 for design rationale.
 *
 * Story 5: per-thread Chase-Lev deques with work stealing.
 * Owner pushes/pops from bottom (LIFO); stealers take from top (FIFO).
 */
#include "scheduler.h"
#include "actor/actor.h"
#include "vm/vm_state.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>  /* sysconf */

/* ── External enqueue ─────────────────────────────────────────────────── */

void sta_scheduler_enqueue(STA_Scheduler *sched, struct STA_Actor *actor) {
    /* Round-robin across worker deques for external pushes.
     * sta_deque_push is safe: only the owner thread pops, but any thread
     * can push (the push path uses release store on bottom which is safe
     * from any thread — only pop has the owner-only constraint).
     *
     * Actually, Chase-Lev push is owner-only by design. For external
     * enqueue from non-worker threads, we use a simple approach:
     * pick a target worker and push via the deque. Since the deque's
     * push reads bottom with relaxed ordering (owner-private), calling
     * push from a non-owner thread is technically a data race.
     *
     * Solution: push always goes through a thread-safe path.
     * We use atomic fetch_add for round-robin and deque_push which
     * only the owner should call. For external enqueue, we need a
     * different strategy.
     *
     * Simplest correct approach: keep a small mutex-protected overflow
     * queue for external pushes. Workers drain it periodically.
     * But that adds complexity. Alternative: make push safe for
     * external callers by noting that bottom is _Atomic and the
     * store is release. The issue is the relaxed load of bottom in
     * push — if two threads push simultaneously, they read the same
     * bottom and overwrite the same slot.
     *
     * Final design: external enqueue uses the deque's steal interface
     * in reverse — no, that doesn't work either.
     *
     * Pragmatic solution: keep a global overflow queue (mutex-protected)
     * for external pushes. Workers check it when their deque is empty.
     */

    /* Push to global overflow queue. Workers drain this. */
    actor->next_runnable = NULL;

    pthread_mutex_lock(&sched->wake_mutex);
    /* Link into overflow list. */
    actor->next_runnable = sched->overflow_head;
    sched->overflow_head = actor;
    pthread_cond_signal(&sched->wake_cond);
    pthread_mutex_unlock(&sched->wake_mutex);
}

/* Drain overflow queue into the worker's local deque.
 * Called by the worker thread under the wake_mutex. */
static struct STA_Actor *drain_overflow(STA_Scheduler *sched,
                                         STA_SchedWorker *worker) {
    struct STA_Actor *list = sched->overflow_head;
    sched->overflow_head = NULL;

    struct STA_Actor *first = NULL;
    while (list) {
        struct STA_Actor *next = list->next_runnable;
        list->next_runnable = NULL;
        if (!first) {
            first = list;  /* return the first one directly */
        } else {
            sta_deque_push(&worker->deque, list);
        }
        list = next;
    }
    return first;
}

/* ── Work stealing ────────────────────────────────────────────────────── */

/* Try to steal one actor from any other worker's deque.
 * Random victim selection to avoid herding. */
static struct STA_Actor *try_steal(STA_Scheduler *sched,
                                    STA_SchedWorker *self) {
    uint32_t n = sched->num_threads;
    if (n <= 1) return NULL;

    /* Simple round-robin starting from (self+1). */
    for (uint32_t i = 1; i < n; i++) {
        uint32_t victim = ((uint32_t)self->index + i) % n;
        struct STA_Actor *a = sta_deque_steal(&sched->workers[victim].deque);
        if (a) {
            self->steals++;
            return a;
        }
    }
    return NULL;
}

/* ── Worker dispatch loop ─────────────────────────────────────────────── */

typedef struct {
    STA_SchedWorker *worker;
    STA_Scheduler   *sched;
} WorkerArg;

static void *worker_thread_main(void *arg) {
    WorkerArg *wa = arg;
    STA_SchedWorker *worker = wa->worker;
    STA_Scheduler *sched = wa->sched;
    STA_VM *vm = sched->vm;
    free(wa);

    while (atomic_load_explicit(&sched->running, memory_order_acquire)) {
        struct STA_Actor *actor = NULL;

        /* 1. Pop from own deque (LIFO — cache-friendly). */
        actor = sta_deque_pop(&worker->deque);

        /* 2. Try stealing from other workers. */
        if (!actor) {
            actor = try_steal(sched, worker);
        }

        /* 3. Check overflow queue (external enqueues). */
        if (!actor) {
            pthread_mutex_lock(&sched->wake_mutex);
            if (sched->overflow_head) {
                actor = drain_overflow(sched, worker);
            }
            pthread_mutex_unlock(&sched->wake_mutex);
        }

        if (actor) {
            /* READY → RUNNING (atomic claim). */
            uint32_t expected = STA_ACTOR_READY;
            if (!atomic_compare_exchange_strong_explicit(
                    &actor->state, &expected, STA_ACTOR_RUNNING,
                    memory_order_acq_rel, memory_order_relaxed)) {
                continue;
            }
            worker->current = actor;

            int rc = sta_actor_process_one_preemptible(vm, actor);
            if (rc == STA_ACTOR_MSG_PROCESSED) {
                worker->messages_processed++;
            }
            worker->actors_run++;

            worker->current = NULL;

            if (rc == STA_ACTOR_MSG_EXCEPTION) {
                /* Actor terminated due to unhandled exception.
                 * handle_actor_exception already set state to TERMINATED,
                 * notified supervisor, and drained the mailbox. Do not
                 * re-enqueue or change state. */
            } else if (rc == STA_ACTOR_MSG_PREEMPTED) {
                atomic_store_explicit(&actor->state, STA_ACTOR_READY,
                                      memory_order_release);
                sta_deque_push(&worker->deque, actor);
            } else if (!sta_mailbox_is_empty(&actor->mailbox)) {
                atomic_store_explicit(&actor->state, STA_ACTOR_READY,
                                      memory_order_release);
                sta_deque_push(&worker->deque, actor);
            } else {
                atomic_store_explicit(&actor->state, STA_ACTOR_SUSPENDED,
                                      memory_order_release);
            }
        } else {
            /* No work anywhere — sleep. */
            pthread_mutex_lock(&sched->wake_mutex);
            if (atomic_load_explicit(&sched->running, memory_order_acquire)
                && !sched->overflow_head) {
                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                deadline.tv_nsec += 5000000;  /* 5 ms timeout */
                if (deadline.tv_nsec >= 1000000000L) {
                    deadline.tv_sec += 1;
                    deadline.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&sched->wake_cond, &sched->wake_mutex,
                                       &deadline);
            }
            pthread_mutex_unlock(&sched->wake_mutex);
        }
    }

    return NULL;
}

/* ── Lifecycle ────────────────────────────────────────────────────────── */

int sta_scheduler_init(struct STA_VM *vm, uint32_t num_threads) {
    if (num_threads == 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = (cores > 0) ? (uint32_t)cores : 1;
    }

    STA_Scheduler *sched = calloc(1, sizeof(STA_Scheduler));
    if (!sched) return -1;

    sched->vm = vm;
    sched->num_threads = num_threads;
    atomic_store_explicit(&sched->running, false, memory_order_relaxed);
    atomic_store_explicit(&sched->enqueue_rr, 0u, memory_order_relaxed);
    sched->overflow_head = NULL;

    pthread_mutex_init(&sched->wake_mutex, NULL);
    pthread_cond_init(&sched->wake_cond, NULL);

    sched->workers = calloc(num_threads, sizeof(STA_SchedWorker));
    if (!sched->workers) {
        pthread_mutex_destroy(&sched->wake_mutex);
        pthread_cond_destroy(&sched->wake_cond);
        free(sched);
        return -1;
    }

    for (uint32_t i = 0; i < num_threads; i++) {
        sched->workers[i].index = (int)i;
        sched->workers[i].current = NULL;
        sched->workers[i].actors_run = 0;
        sched->workers[i].messages_processed = 0;
        sched->workers[i].steals = 0;
        sta_deque_init(&sched->workers[i].deque);
    }

    vm->scheduler = sched;
    return 0;
}

int sta_scheduler_start(struct STA_VM *vm) {
    STA_Scheduler *sched = vm->scheduler;
    if (!sched) return -1;

    atomic_store_explicit(&sched->running, true, memory_order_release);

    for (uint32_t i = 0; i < sched->num_threads; i++) {
        WorkerArg *wa = malloc(sizeof(WorkerArg));
        if (!wa) return -1;
        wa->worker = &sched->workers[i];
        wa->sched = sched;
        pthread_create(&sched->workers[i].thread, NULL,
                       worker_thread_main, wa);
    }

    return 0;
}

void sta_scheduler_stop(struct STA_VM *vm) {
    STA_Scheduler *sched = vm->scheduler;
    if (!sched) return;

    atomic_store_explicit(&sched->running, false, memory_order_release);

    pthread_mutex_lock(&sched->wake_mutex);
    pthread_cond_broadcast(&sched->wake_cond);
    pthread_mutex_unlock(&sched->wake_mutex);

    for (uint32_t i = 0; i < sched->num_threads; i++) {
        pthread_join(sched->workers[i].thread, NULL);
    }
}

void sta_scheduler_destroy(struct STA_VM *vm) {
    STA_Scheduler *sched = vm->scheduler;
    if (!sched) return;

    pthread_mutex_destroy(&sched->wake_mutex);
    pthread_cond_destroy(&sched->wake_cond);
    free(sched->workers);
    free(sched);
    vm->scheduler = NULL;
}
