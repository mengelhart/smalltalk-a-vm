/* src/scheduler/scheduler.c
 * Production work-stealing scheduler — Phase 2 Epic 4.
 * See scheduler.h and ADR 009 for design rationale.
 */
#include "scheduler.h"
#include "actor/actor.h"
#include "vm/vm_state.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>  /* sysconf */

/* ── Run queue operations ─────────────────────────────────────────────── */

static void run_queue_init(STA_RunQueue *rq) {
    rq->head = NULL;
    rq->tail = NULL;
    rq->count = 0;
    pthread_mutex_init(&rq->mutex, NULL);
}

static void run_queue_destroy(STA_RunQueue *rq) {
    pthread_mutex_destroy(&rq->mutex);
}

void sta_scheduler_enqueue(STA_Scheduler *sched, struct STA_Actor *actor) {
    actor->next_runnable = NULL;

    pthread_mutex_lock(&sched->run_queue.mutex);
    if (sched->run_queue.tail) {
        sched->run_queue.tail->next_runnable = actor;
    } else {
        sched->run_queue.head = actor;
    }
    sched->run_queue.tail = actor;
    sched->run_queue.count++;
    pthread_mutex_unlock(&sched->run_queue.mutex);

    /* Wake an idle worker. */
    pthread_mutex_lock(&sched->wake_mutex);
    pthread_cond_signal(&sched->wake_cond);
    pthread_mutex_unlock(&sched->wake_mutex);
}

struct STA_Actor *sta_scheduler_dequeue(STA_Scheduler *sched) {
    pthread_mutex_lock(&sched->run_queue.mutex);
    struct STA_Actor *actor = sched->run_queue.head;
    if (actor) {
        sched->run_queue.head = actor->next_runnable;
        if (!sched->run_queue.head) {
            sched->run_queue.tail = NULL;
        }
        sched->run_queue.count--;
        actor->next_runnable = NULL;
    }
    pthread_mutex_unlock(&sched->run_queue.mutex);
    return actor;
}

/* ── Worker dispatch loop ─────────────────────────────────────────────── */

/* Thread arg: passed to each worker thread at creation, freed by worker. */
typedef struct {
    STA_SchedWorker *worker;
    STA_Scheduler   *sched;
} WorkerArg;

static void *worker_thread_main(void *arg) {
    WorkerArg *wa = arg;
    STA_SchedWorker *worker = wa->worker;
    STA_Scheduler *sched = wa->sched;
    STA_VM *vm = sched->vm;
    free(wa);  /* allocated per-thread in scheduler_start */

    while (atomic_load_explicit(&sched->running, memory_order_acquire)) {
        struct STA_Actor *actor = sta_scheduler_dequeue(sched);

        if (actor) {
            /* READY → RUNNING (atomic claim — only one thread can win). */
            uint32_t expected = STA_ACTOR_READY;
            if (!atomic_compare_exchange_strong_explicit(
                    &actor->state, &expected, STA_ACTOR_RUNNING,
                    memory_order_acq_rel, memory_order_relaxed)) {
                /* Another thread claimed this actor (should not happen
                 * with mutex queue, but defense-in-depth for Story 5). */
                continue;
            }
            worker->current = actor;

            /* Process one message (or resume preempted execution).
             * Preemption-aware: returns PREEMPTED if reduction quota
             * was exhausted mid-execution. */
            int rc = sta_actor_process_one_preemptible(vm, actor);
            if (rc == STA_ACTOR_MSG_PROCESSED) {
                worker->messages_processed++;
            }
            worker->actors_run++;

            worker->current = NULL;

            if (rc == STA_ACTOR_MSG_PREEMPTED) {
                /* Actor was preempted — re-enqueue immediately.
                 * RUNNING → READY. */
                atomic_store_explicit(&actor->state, STA_ACTOR_READY,
                                      memory_order_release);
                sta_scheduler_enqueue(sched, actor);
            } else if (!sta_mailbox_is_empty(&actor->mailbox)) {
                /* More messages waiting — re-enqueue.
                 * RUNNING → READY. */
                atomic_store_explicit(&actor->state, STA_ACTOR_READY,
                                      memory_order_release);
                sta_scheduler_enqueue(sched, actor);
            } else {
                /* No more work — suspend.
                 * RUNNING → SUSPENDED. */
                atomic_store_explicit(&actor->state, STA_ACTOR_SUSPENDED,
                                      memory_order_release);
            }
        } else {
            /* No work — sleep on condition variable.
             * Re-check under the mutex to prevent lost wakeups. */
            pthread_mutex_lock(&sched->wake_mutex);
            /* Double-check: work may have arrived between dequeue and lock. */
            if (atomic_load_explicit(&sched->running, memory_order_acquire)) {
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

    run_queue_init(&sched->run_queue);
    pthread_mutex_init(&sched->wake_mutex, NULL);
    pthread_cond_init(&sched->wake_cond, NULL);

    sched->workers = calloc(num_threads, sizeof(STA_SchedWorker));
    if (!sched->workers) {
        run_queue_destroy(&sched->run_queue);
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

    /* Wake all workers so they observe the stop flag. */
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

    run_queue_destroy(&sched->run_queue);
    pthread_mutex_destroy(&sched->wake_mutex);
    pthread_cond_destroy(&sched->wake_cond);
    free(sched->workers);
    free(sched);
    vm->scheduler = NULL;
}
