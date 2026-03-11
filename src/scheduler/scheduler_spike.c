/* src/scheduler/scheduler_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: work-stealing scheduler and reduction-based preemption.
 * See docs/spikes/spike-003-scheduler.md and ADR 009.
 */

#include "scheduler_spike.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Portable spin barrier ─────────────────────────────────────────────── */

void sta_barrier_init(STA_Barrier *b, int total) {
    b->total = total;
    atomic_store_explicit(&b->count, total, memory_order_relaxed);
    atomic_store_explicit(&b->phase, 0,     memory_order_relaxed);
}

void sta_barrier_wait(STA_Barrier *b) {
    int phase = atomic_load_explicit(&b->phase, memory_order_relaxed);
    int prev  = atomic_fetch_sub_explicit(&b->count, 1, memory_order_acq_rel);
    if (prev == 1) {
        /* Last arrival: reset count then advance phase to release waiters. */
        atomic_store_explicit(&b->count, b->total, memory_order_release);
        atomic_fetch_add_explicit(&b->phase, 1, memory_order_release);
    } else {
        /* Spin until phase advances. */
        while (atomic_load_explicit(&b->phase, memory_order_acquire) == phase) {
            /* compiler barrier prevents the loop being optimised away */
            atomic_thread_fence(memory_order_seq_cst);
        }
    }
}

/* ── Timing ────────────────────────────────────────────────────────────── */

uint64_t sta_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── Deque A: Chase-Lev implementation ─────────────────────────────────── */
/*
 * Memory ordering audit (Q5):
 *
 * push (owner only):
 *   1. relaxed load  bottom — no sharing; owner is the only writer
 *   2. acquire load  top    — syncs with stealer's seq_cst CAS on top;
 *                             ensures we see the latest top before checking
 *                             capacity
 *   3. relaxed store buf[b] — ordered before the release store on bottom (4)
 *   4. release store bottom — synchronizes with any acquire-load of bottom
 *                             in steal/pop; after observing this new bottom,
 *                             any loader is guaranteed to see the buf write
 *
 * pop (owner only):
 *   1. relaxed load  bottom — private to owner
 *   2. relaxed store bottom (decremented) — tentatively claim the slot
 *   3. seq_cst fence        — prevents CPU reordering with the top load;
 *                             creates a happens-before edge with the stealer's
 *                             seq_cst CAS, closing the race window on the
 *                             last element
 *   4. relaxed load  top    — read after fence; correct due to seq_cst ordering
 *   5. relaxed load  buf[b] — safe: only the owner pops from bottom; no
 *                             concurrent write to this slot
 *   6. seq_cst CAS   top    — resolves race with stealers on the last element
 *
 * steal (any thread):
 *   1. acquire load  top    — syncs with other stealers' CAS releases;
 *                             establishes our view of top before reading bottom
 *   2. seq_cst fence        — ensures the top load is not reordered with the
 *                             bottom load on arm64 (where LDR can be reordered
 *                             freely without barriers)
 *   3. acquire load  bottom — syncs with push's release store; after this we
 *                             see all buf writes that preceded that push
 *   4. relaxed load  buf[t] — safe: acquire on bottom in (3) established
 *                             happens-before for all prior buf[t] writes;
 *                             the slot at t was written in a prior push
 *   5. seq_cst CAS   top    — arbitrates concurrent stealers; the one use of
 *                             seq_cst per spike constraints (see Q5 audit)
 *
 * buf entries are _Atomic(STA_SpikeActor *) so all concurrent accesses are
 * through atomic operations — TSan sees no data race on array slots.
 */

void sta_dequeA_init(STA_WorkDequeA *dq) {
    atomic_store_explicit(&dq->top,    0u, memory_order_relaxed);
    atomic_store_explicit(&dq->bottom, 0u, memory_order_relaxed);
    for (uint32_t i = 0; i < STA_DEQUE_CAPACITY; i++)
        atomic_store_explicit(&dq->buf[i], NULL, memory_order_relaxed);
}

int sta_dequeA_push(STA_WorkDequeA *dq, STA_SpikeActor *a) {
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&dq->top,    memory_order_acquire);
    if (b - t >= STA_DEQUE_CAPACITY) return -1;  /* full */
    atomic_store_explicit(&dq->buf[b & STA_DEQUE_MASK], a, memory_order_relaxed);
    /* Release store: publishes the buf write to any thread that subsequently
     * acquire-loads bottom. */
    atomic_store_explicit(&dq->bottom, b + 1u, memory_order_release);
    return 0;
}

STA_SpikeActor *sta_dequeA_pop(STA_WorkDequeA *dq) {
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    if (b == 0) {
        /* bottom would wrap; definitely empty. */
        return NULL;
    }
    b -= 1u;
    /* Tentatively claim slot b. */
    atomic_store_explicit(&dq->bottom, b, memory_order_relaxed);
    /* seq_cst fence: closes the race with concurrent steal's seq_cst CAS.
     * After this fence, the load of top below sees any CAS that completed
     * before or at the same time as our decrement of bottom. */
    atomic_thread_fence(memory_order_seq_cst);
    uint32_t t = atomic_load_explicit(&dq->top, memory_order_relaxed);

    if ((int32_t)(t - b) > 0) {
        /* A stealer advanced top past our claimed b — queue is empty. */
        atomic_store_explicit(&dq->bottom, b + 1u, memory_order_relaxed);
        return NULL;
    }

    STA_SpikeActor *a =
        atomic_load_explicit(&dq->buf[b & STA_DEQUE_MASK], memory_order_relaxed);

    if (t == b) {
        /* Last element: race with stealers — use CAS on top to resolve. */
        uint32_t expected = t;
        bool won = atomic_compare_exchange_strong_explicit(
            &dq->top, &expected, t + 1u,
            memory_order_seq_cst, memory_order_relaxed);
        /* Either way, advance bottom to t+1 (queue is now empty). */
        atomic_store_explicit(&dq->bottom, t + 1u, memory_order_relaxed);
        return won ? a : NULL;
    }
    /* t < b: more than one element — no stealer race possible. */
    return a;
}

STA_SpikeActor *sta_dequeA_steal(STA_WorkDequeA *dq) {
    uint32_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    /* seq_cst fence: ensures the top load is not reordered with the bottom
     * load on arm64.  Without this, a CPU could speculate bottom before top,
     * leading to a stale view of the queue size. */
    atomic_thread_fence(memory_order_seq_cst);
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    if ((int32_t)(b - t) <= 0) return NULL;  /* empty */

    STA_SpikeActor *a =
        atomic_load_explicit(&dq->buf[t & STA_DEQUE_MASK], memory_order_relaxed);

    uint32_t expected = t;
    if (!atomic_compare_exchange_strong_explicit(
            &dq->top, &expected, t + 1u,
            memory_order_seq_cst, memory_order_relaxed)) {
        return NULL;  /* lost the CAS: another stealer or pop got it */
    }
    return a;
}

int sta_dequeA_size(const STA_WorkDequeA *dq) {
    /* Approximate: may be transiently negative during concurrent ops. */
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&dq->top,    memory_order_relaxed);
    return (int32_t)(b - t);
}

/* ── Deque B: ring-buffer implementation ──────────────────────────────── */
/*
 * Identical steal and pop protocols to Variant A.  The only difference is
 * that push returns -1 when full instead of growing the array.
 * Memory ordering: same as Variant A (see audit above).
 */

void sta_dequeB_init(STA_WorkDequeB *dq) {
    atomic_store_explicit(&dq->top,    0u, memory_order_relaxed);
    atomic_store_explicit(&dq->bottom, 0u, memory_order_relaxed);
    for (uint32_t i = 0; i < STA_DEQUE_CAPACITY; i++)
        atomic_store_explicit(&dq->buf[i], NULL, memory_order_relaxed);
}

int sta_dequeB_push(STA_WorkDequeB *dq, STA_SpikeActor *a) {
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&dq->top,    memory_order_acquire);
    if (b - t >= STA_DEQUE_CAPACITY) return -1;  /* full — no resize */
    atomic_store_explicit(&dq->buf[b & STA_DEQUE_MASK], a, memory_order_relaxed);
    atomic_store_explicit(&dq->bottom, b + 1u, memory_order_release);
    return 0;
}

STA_SpikeActor *sta_dequeB_pop(STA_WorkDequeB *dq) {
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    if (b == 0) return NULL;
    b -= 1u;
    atomic_store_explicit(&dq->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    uint32_t t = atomic_load_explicit(&dq->top, memory_order_relaxed);
    if ((int32_t)(t - b) > 0) {
        atomic_store_explicit(&dq->bottom, b + 1u, memory_order_relaxed);
        return NULL;
    }
    STA_SpikeActor *a =
        atomic_load_explicit(&dq->buf[b & STA_DEQUE_MASK], memory_order_relaxed);
    if (t == b) {
        uint32_t expected = t;
        bool won = atomic_compare_exchange_strong_explicit(
            &dq->top, &expected, t + 1u,
            memory_order_seq_cst, memory_order_relaxed);
        atomic_store_explicit(&dq->bottom, t + 1u, memory_order_relaxed);
        return won ? a : NULL;
    }
    return a;
}

STA_SpikeActor *sta_dequeB_steal(STA_WorkDequeB *dq) {
    uint32_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    if ((int32_t)(b - t) <= 0) return NULL;
    STA_SpikeActor *a =
        atomic_load_explicit(&dq->buf[t & STA_DEQUE_MASK], memory_order_relaxed);
    uint32_t expected = t;
    if (!atomic_compare_exchange_strong_explicit(
            &dq->top, &expected, t + 1u,
            memory_order_seq_cst, memory_order_relaxed)) {
        return NULL;
    }
    return a;
}

int sta_dequeB_size(const STA_WorkDequeB *dq) {
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&dq->top,    memory_order_relaxed);
    return (int32_t)(b - t);
}

/* ── Notification: Option 1 — pthread_cond_signal ─────────────────────── */

#if STA_NOTIF_MODE == 1

void sta_notif_init(STA_NotifState *n) {
    pthread_mutex_init(&n->mutex, NULL);
    pthread_cond_init(&n->cond, NULL);
    atomic_store_explicit(&n->pending, 0, memory_order_relaxed);
}

void sta_notif_destroy(STA_NotifState *n) {
    pthread_cond_destroy(&n->cond);
    pthread_mutex_destroy(&n->mutex);
}

/* Called by enqueuer after pushing to the deque.
 * Acquires the mutex to prevent the lost-wakeup race (see spike doc Q3). */
void sta_notif_wake(STA_NotifState *n) {
    pthread_mutex_lock(&n->mutex);
    atomic_store_explicit(&n->pending, 1, memory_order_relaxed);
    pthread_cond_signal(&n->cond);
    pthread_mutex_unlock(&n->mutex);
}

/* Called by the scheduler thread when it finds no work.
 * Holds the mutex while re-checking work availability (caller's deque is
 * checked by the caller before and after this call). */
void sta_notif_wait(STA_NotifState *n, const _Atomic int *running) {
    pthread_mutex_lock(&n->mutex);
    /* Spurious wakeup guard: only sleep if no pending signal. */
    while (!atomic_load_explicit(&n->pending, memory_order_relaxed) &&
           atomic_load_explicit(running, memory_order_acquire)) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 5000000;  /* 5 ms timeout — prevents permanent sleep */
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec  += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&n->cond, &n->mutex, &deadline);
    }
    atomic_store_explicit(&n->pending, 0, memory_order_relaxed);
    pthread_mutex_unlock(&n->mutex);
}

/* ── Notification: Option 2 — pipe write ──────────────────────────────── */

#elif STA_NOTIF_MODE == 2

void sta_notif_init(STA_NotifState *n) {
    int fds[2];
    if (pipe(fds) != 0) { perror("pipe"); abort(); }
    n->read_fd  = fds[0];
    n->write_fd = fds[1];
}

void sta_notif_destroy(STA_NotifState *n) {
    close(n->read_fd);
    close(n->write_fd);
}

void sta_notif_wake(STA_NotifState *n) {
    char byte = 1;
    /* write(2) is async-signal-safe and does not require a lock.
     * We write after the deque push (release store on bottom) so the
     * scheduler sees the push before or when it wakes from poll(). */
    ssize_t r;
    do { r = write(n->write_fd, &byte, 1); } while (r == -1 && errno == EINTR);
}

/* Drain all pending wakeup bytes from the pipe (non-blocking). */
static void drain_pipe(int fd) {
    char buf[64];
    ssize_t r;
    do { r = read(fd, buf, sizeof(buf)); } while (r > 0);
}

void sta_notif_wait(STA_NotifState *n, const _Atomic int *running) {
    /* Drain stale wakeups from previous iteration. */
    drain_pipe(n->read_fd);
    /* The scheduler re-checks its deque after drain (in the caller's loop)
     * before calling poll, preventing the lost-wakeup race. */
    if (!atomic_load_explicit(running, memory_order_acquire)) return;
    struct pollfd pfd = { .fd = n->read_fd, .events = POLLIN };
    poll(&pfd, 1, 5);  /* 5 ms timeout */
    drain_pipe(n->read_fd);
}

/* ── Notification: Option 3 — spinning with exponential backoff ────────── */

#else  /* STA_NOTIF_MODE == 3 */

void sta_notif_init(STA_NotifState *n) {
    atomic_store_explicit(&n->backoff_ns, 1ULL, memory_order_relaxed);
}

void sta_notif_destroy(STA_NotifState *n) {
    (void)n;
}

void sta_notif_wake(STA_NotifState *n) {
    /* Reset backoff so the scheduler spins fast again. */
    atomic_store_explicit(&n->backoff_ns, 1ULL, memory_order_release);
}

void sta_notif_wait(STA_NotifState *n, const _Atomic int *running) {
    if (!atomic_load_explicit(running, memory_order_acquire)) return;
    uint64_t ns = atomic_load_explicit(&n->backoff_ns, memory_order_relaxed);
    /* Busy spin for 'ns' nanoseconds using a compiler barrier. */
    uint64_t end = sta_now_ns() + ns;
    while (sta_now_ns() < end) {
        /* Compiler barrier prevents optimizing the loop away. */
        atomic_thread_fence(memory_order_seq_cst);
    }
    /* Exponential backoff: double up to 1 ms, then yield. */
    uint64_t next = (ns < 1000000ULL) ? ns * 2ULL : 1000000ULL;
    if (next >= 1000000ULL) {
        sched_yield();
        next = 1ULL;  /* reset after yield */
    }
    atomic_store_explicit(&n->backoff_ns, next, memory_order_relaxed);
}

#endif  /* STA_NOTIF_MODE */

/* ── Scheduler internals ─────────────────────────────────────────────── */

/* Stub "execute" function: decrement the reduction counter to zero.
 * Models one scheduling quantum.  The real interpreter will replace this
 * with bytecode dispatch.  Wall-clock duration measured in Q4. */
static void execute_actor(STA_SpikeActor *actor) {
    atomic_store_explicit(&actor->reductions, STA_REDUCTION_QUOTA,
                          memory_order_relaxed);
    uint32_t r;
    while ((r = atomic_load_explicit(&actor->reductions,
                                     memory_order_relaxed)) > 0) {
        atomic_store_explicit(&actor->reductions, r - 1u, memory_order_relaxed);
    }
}

/* Try to steal one actor from any thread other than self_idx.
 * Iterates starting from (self_idx + 1) in round-robin order. */
static STA_SpikeActor *try_steal(STA_Scheduler *s, int self_idx) {
    for (int i = 1; i < s->nthreads; i++) {
        int victim = (self_idx + i) % s->nthreads;
        STA_SpikeActor *a;
#if STA_DEQUE_VARIANT == 1
        a = sta_dequeA_steal(&s->threads[victim].deque);
#else
        a = sta_dequeB_steal(&s->threads[victim].deque);
#endif
        if (a) {
            atomic_fetch_add_explicit(
                &s->threads[self_idx].steal_successes, 1ULL,
                memory_order_relaxed);
            return a;
        }
        atomic_fetch_add_explicit(
            &s->threads[self_idx].steal_attempts, 1ULL,
            memory_order_relaxed);
    }
    return NULL;
}

/* Main scheduling loop for each OS thread. */
static void *sched_thread_main(void *arg) {
    STA_SchedThread *self  = arg;
    STA_Scheduler   *sched = self->sched;

    /* All threads wait here so no thread starts scheduling before all are
     * ready — ensures the startup latency measurement is clean. */
    sta_barrier_wait(&sched->start_barrier);

    while (atomic_load_explicit(&sched->running, memory_order_acquire)) {
        STA_SpikeActor *actor = NULL;

        /* 1. Try own deque. */
#if STA_DEQUE_VARIANT == 1
        actor = sta_dequeA_pop(&self->deque);
#else
        actor = sta_dequeB_pop(&self->deque);
#endif

        /* 2. Try stealing. */
        if (!actor) {
            actor = try_steal(sched, self->index);
        }

        if (actor) {
            /* Mark running. */
            atomic_store_explicit(&actor->sched_flags, STA_SCHED_RUNNING,
                                  memory_order_release);

            /* Record wake timestamp for latency measurement (Q3). */
            uint64_t start = sta_now_ns();
            atomic_store_explicit(&actor->start_ns, start,
                                  memory_order_release);

            execute_actor(actor);

            atomic_fetch_add_explicit(&self->actors_run, 1ULL,
                                      memory_order_relaxed);

            /* Decide whether to re-enqueue or retire. */
            uint32_t rc = atomic_fetch_add_explicit(&actor->run_count, 1u,
                                                    memory_order_acq_rel) + 1u;
            if (rc < actor->max_runs) {
                /* Re-enqueue at the home thread's deque (actor is still
                 * runnable).  If push fails (deque full), re-try on self. */
                int target = (int)(actor->home_thread % (uint32_t)sched->nthreads);
                int pushed  = -1;
#if STA_DEQUE_VARIANT == 1
                pushed = sta_dequeA_push(&sched->threads[target].deque, actor);
                if (pushed != 0)
                    pushed = sta_dequeA_push(&self->deque, actor);
#else
                pushed = sta_dequeB_push(&sched->threads[target].deque, actor);
                if (pushed != 0)
                    pushed = sta_dequeB_push(&self->deque, actor);
#endif
                if (pushed == 0) {
                    atomic_store_explicit(&actor->sched_flags,
                                          STA_SCHED_RUNNABLE,
                                          memory_order_release);
                    /* Wake target thread if it differs from self. */
                    if (target != self->index) {
                        sta_notif_wake(&sched->threads[target].notif);
                    }
                }
            } else {
                /* Actor has completed all runs — retire. */
                atomic_store_explicit(&actor->sched_flags, STA_SCHED_NONE,
                                      memory_order_release);
                atomic_fetch_sub_explicit(&sched->remaining, 1u,
                                          memory_order_acq_rel);
            }
        } else {
            /* No work found anywhere.  For Option 1/2: re-check deque while
             * holding the notification lock to prevent lost wakeup; for
             * Option 3: spin. */
            sta_notif_wait(&self->notif, &sched->running);
        }
    }

    return NULL;
}

/* ── Scheduler lifecycle ─────────────────────────────────────────────── */

int sta_sched_init(STA_Scheduler *s, int nthreads) {
    assert(nthreads >= 1 && nthreads <= (int)STA_MAX_THREADS);

    s->threads = calloc((size_t)nthreads, sizeof(STA_SchedThread));
    if (!s->threads) return -1;

    s->nthreads = nthreads;
    atomic_store_explicit(&s->running,   1, memory_order_relaxed);
    atomic_store_explicit(&s->remaining, 0u, memory_order_relaxed);

    /* +1: main thread also calls sta_barrier_wait() in sta_sched_start(). */
    sta_barrier_init(&s->start_barrier, nthreads + 1);

    for (int i = 0; i < nthreads; i++) {
        STA_SchedThread *t = &s->threads[i];
        t->index = i;
        t->sched  = s;
        atomic_store_explicit(&t->actors_run,       0ULL, memory_order_relaxed);
        atomic_store_explicit(&t->steal_attempts,   0ULL, memory_order_relaxed);
        atomic_store_explicit(&t->steal_successes,  0ULL, memory_order_relaxed);
        atomic_store_explicit(&t->steal_cas_failures, 0ULL, memory_order_relaxed);
#if STA_DEQUE_VARIANT == 1
        sta_dequeA_init(&t->deque);
#else
        sta_dequeB_init(&t->deque);
#endif
        sta_notif_init(&t->notif);
    }
    return 0;
}

void sta_sched_start(STA_Scheduler *s) {
    for (int i = 0; i < s->nthreads; i++) {
        pthread_create(&s->threads[i].thread, NULL,
                       sched_thread_main, &s->threads[i]);
    }
    /* Release all scheduler threads simultaneously. */
    sta_barrier_wait(&s->start_barrier);
}

void sta_sched_stop(STA_Scheduler *s) {
    atomic_store_explicit(&s->running, 0, memory_order_release);
    /* Wake all threads so they observe the stop flag. */
    for (int i = 0; i < s->nthreads; i++) {
        sta_notif_wake(&s->threads[i].notif);
    }
    for (int i = 0; i < s->nthreads; i++) {
        pthread_join(s->threads[i].thread, NULL);
    }
}

void sta_sched_destroy(STA_Scheduler *s) {
    for (int i = 0; i < s->nthreads; i++) {
        sta_notif_destroy(&s->threads[i].notif);
    }
    /* STA_Barrier has no dynamic resources to destroy. */
    free(s->threads);
    s->threads  = NULL;
    s->nthreads = 0;
}

int sta_sched_push(STA_Scheduler *s, int thread_idx, STA_SpikeActor *actor) {
    assert(thread_idx >= 0 && thread_idx < s->nthreads);
    STA_SchedThread *t = &s->threads[thread_idx];
    int ret;
#if STA_DEQUE_VARIANT == 1
    ret = sta_dequeA_push(&t->deque, actor);
#else
    ret = sta_dequeB_push(&t->deque, actor);
#endif
    if (ret == 0) {
        atomic_store_explicit(&actor->sched_flags, STA_SCHED_RUNNABLE,
                              memory_order_release);
        sta_notif_wake(&t->notif);
    }
    return ret;
}
