/* src/scheduler/scheduler_spike.h
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: work-stealing scheduler and reduction-based preemption.
 * See docs/spikes/spike-003-scheduler.md and ADR 009.
 *
 * Compile-time knobs:
 *   STA_NOTIF_MODE  1 = pthread_cond_signal (default)
 *                   2 = pipe write
 *                   3 = spinning with exponential backoff
 *   STA_DEQUE_VARIANT 1 = Variant A: Chase-Lev deque (default)
 *                     2 = Variant B: ring-buffer deque
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <pthread.h>
#include <unistd.h>

/* ── Compile-time defaults ─────────────────────────────────────────────── */

#ifndef STA_NOTIF_MODE
#define STA_NOTIF_MODE 1
#endif

#ifndef STA_DEQUE_VARIANT
#define STA_DEQUE_VARIANT 1
#endif

/* ── Constants ─────────────────────────────────────────────────────────── */

/* Reductions granted per scheduling quantum (inherited from BEAM).
 * Validated on arm64 M4 Max in Q4; see ADR 009 for measured quantum. */
#define STA_REDUCTION_QUOTA  1000u

/* Deque capacity: power-of-two.  Dynamic resize (Chase-Lev) is a Phase 1
 * concern; fixed capacity is correct for all spike workloads. */
#define STA_DEQUE_CAPACITY   1024u
#define STA_DEQUE_MASK       (STA_DEQUE_CAPACITY - 1u)

/* Upper bound on scheduler thread count (for static arrays). */
#define STA_MAX_THREADS      64u

/* ── Spike actor ───────────────────────────────────────────────────────── */

/* Minimal actor representation for the scheduler spike.
 * Contains only fields the scheduler needs to operate and measure.
 * For the full revised STA_Actor layout (density measurement), see
 * STA_ActorRevised below. */
typedef struct STA_SpikeActor {
    struct STA_SpikeActor *next_runnable;  /*  8 — intrusive run-queue link   */
    _Atomic uint32_t       sched_flags;   /*  4 — see STA_SCHED_* constants  */
    uint32_t               home_thread;   /*  4 — preferred scheduler thread  */
    _Atomic uint32_t       reductions;    /*  4 — remaining quantum budget    */
    uint32_t               actor_id;      /*  4 — test identity               */
    _Atomic uint32_t       run_count;     /*  4 — times scheduled (test use)  */
    uint32_t               max_runs;      /*  4 — retire after this many runs  */
    _Atomic uint64_t       start_ns;      /*  8 — wake-latency timestamp      */
} STA_SpikeActor;                         /* 40 bytes                         */

/* sched_flags bit constants.
 * Exactly one of NONE/RUNNABLE/RUNNING is set at any time. */
#define STA_SCHED_NONE      0x00u  /* parked; not in any run queue */
#define STA_SCHED_RUNNABLE  0x01u  /* in a run queue, waiting for CPU */
#define STA_SCHED_RUNNING   0x02u  /* currently executing on a thread */

/* ── Variant A: Chase-Lev fixed-capacity deque ─────────────────────────── */
/*
 * Owner thread: push (bottom end) and pop (bottom end).
 * Stealing threads: steal (top end) via single CAS.
 *
 * Memory ordering — see spike doc Q5 and Q2 for full audit:
 *   push:  relaxed store to buf slot; release store to bottom.
 *          Any acquire-load of bottom >= (old bottom + 1) sees the buf write.
 *   pop:   relaxed decrement of bottom; seq_cst fence; relaxed load of top.
 *          The fence closes the race window with concurrent stealers.
 *          CAS on top uses seq_cst — the one justified use per spike constraints.
 *   steal: seq_cst fence between acquire-load of top and acquire-load of bottom;
 *          relaxed load of buf slot; seq_cst CAS on top.
 */
typedef struct {
    _Atomic uint32_t            top;                        /* 4 bytes */
    _Atomic uint32_t            bottom;                     /* 4 bytes */
    _Atomic(STA_SpikeActor *)   buf[STA_DEQUE_CAPACITY];   /* 8192 bytes */
} STA_WorkDequeA;                                           /* ~8200 bytes */

/* ── Variant B: ring-buffer deque ─────────────────────────────────────── */
/*
 * Identical CAS-on-top steal protocol.  Simpler than Variant A because
 * there is no resize path; push returns -1 when the deque is full.
 * Measured to compare steal contention and throughput; see ADR 009.
 */
typedef struct {
    _Atomic uint32_t            top;                        /* 4 bytes */
    _Atomic uint32_t            bottom;                     /* 4 bytes */
    _Atomic(STA_SpikeActor *)   buf[STA_DEQUE_CAPACITY];   /* 8192 bytes */
} STA_WorkDequeB;                                           /* ~8200 bytes */

/* Active deque type for the scheduling loop (compile-time selection). */
#if STA_DEQUE_VARIANT == 1
typedef STA_WorkDequeA STA_WorkDeque;
#else
typedef STA_WorkDequeB STA_WorkDeque;
#endif

/* ── Notification state — all three options ────────────────────────────── */
/*
 * Three options compiled as separate types; the typedef STA_NotifState
 * selects the active one for STA_SchedThread at compile time.
 *
 * Lost-wakeup prevention (Option 1):
 *   The enqueuer pushes to the deque (release store), then acquires the
 *   mutex, signals, and releases.  The scheduler, before sleeping, acquires
 *   the mutex and re-checks the deque — this re-check sees any push that
 *   happened before the enqueuer's mutex acquire, closing the race window.
 *
 * Lost-wakeup prevention (Option 2):
 *   The scheduler drains the pipe, re-checks the deque, then polls.
 *   Any push + pipe-write between drain and poll is visible at poll() time.
 *
 * Option 3 never sleeps, so no lost-wakeup is possible.
 */

/* Option 1: pthread_cond_signal */
typedef struct {
    pthread_mutex_t  mutex;    /* ~40 bytes on macOS */
    pthread_cond_t   cond;     /* ~48 bytes on macOS */
    _Atomic int      pending;  /*   4 bytes — set by enqueuer, cleared by waiter */
} STA_NotifCond;

/* Option 2: pipe write (one byte per wakeup) */
typedef struct {
    int read_fd;   /* 4 bytes — scheduler polls this */
    int write_fd;  /* 4 bytes — enqueuer writes to this */
} STA_NotifPipe;

/* Option 3: spinning with exponential backoff */
typedef struct {
    _Atomic uint64_t backoff_ns;  /* 8 bytes — current backoff */
} STA_NotifSpin;

/* Typedef alias for the selected mode */
#if STA_NOTIF_MODE == 1
typedef STA_NotifCond STA_NotifState;
#elif STA_NOTIF_MODE == 2
typedef STA_NotifPipe STA_NotifState;
#else
typedef STA_NotifSpin STA_NotifState;
#endif

/* ── Per-thread scheduler context ─────────────────────────────────────── */

struct STA_Scheduler;  /* forward */

typedef struct STA_SchedThread {
    /* Threading */
    pthread_t             thread;
    int                   index;     /* 0-based index into sched->threads[] */
    struct STA_Scheduler *sched;     /* back-pointer */

    /* Run queue */
    STA_WorkDeque         deque;

    /* Notification channel (chosen at compile time) */
    STA_NotifState        notif;

    /* Statistics — all atomic; updated by the owning thread only except
     * steal_attempts/successes which are written by the stealer thread. */
    _Atomic uint64_t      actors_run;
    _Atomic uint64_t      steal_attempts;
    _Atomic uint64_t      steal_successes;
    _Atomic uint64_t      steal_cas_failures;  /* CAS lost races on top */
} STA_SchedThread;

/* ── Portable spin barrier (pthread_barrier_t not available on macOS) ─── */
/*
 * All N participants call sta_barrier_wait(). The last to arrive resets
 * the count and advances the phase; all others spin on the phase.
 * Safe under TSan: all accesses go through _Atomic.
 */
typedef struct {
    _Atomic int count;   /* participants still to arrive */
    _Atomic int phase;   /* incremented by last arrival */
    int         total;   /* original participant count */
} STA_Barrier;

/* ── Top-level scheduler ────────────────────────────────────────────────── */

typedef struct STA_Scheduler {
    STA_SchedThread  *threads;    /* heap-allocated [nthreads] */
    int               nthreads;
    _Atomic int       running;    /* 1 = active; 0 = shutdown */
    _Atomic uint32_t  remaining;  /* actors not yet retired (tests) */
    STA_Barrier       start_barrier; /* all threads wait before scheduling */
} STA_Scheduler;

/* ── Revised actor layout — density measurement (Q6) ─────────────────── */
/*
 * Represents what src/vm/actor_spike.h would look like after applying the
 * spike-003 consequences listed in ADR 009:
 *   - STA_MpscList (~40 bytes) replaces STA_Mailbox (16 bytes)   [ADR 008]
 *   - sched_flags: uint32_t → _Atomic uint32_t                   [Q5]
 *   - next_runnable (8 bytes) added                              [Q6]
 *   - home_thread  (4 bytes) added                              [Q6]
 *
 * NOT used in scheduler code.  Exists only for sizeof() measurement.
 *
 * STA_MpscList field breakdown (mailbox_spike.h):
 *   _Atomic(STA_ListNode *) tail  8   producer CAS target
 *   STA_ListNode           *head  8   consumer-private
 *   STA_ListNode            stub  16  sentinel (ptr 8 + OOP 8)
 *   _Atomic uint32_t        count  4
 *   uint32_t                limit  4
 *                           total 40 bytes
 */
typedef struct {
    uint32_t         class_index;      /*  4 */
    uint32_t         actor_id;         /*  4 */
    /* mailbox: STA_MpscList inlined (40 bytes) */
    void            *mbox_tail;        /*  8  _Atomic(STA_ListNode *) */
    void            *mbox_head;        /*  8  STA_ListNode * */
    uint8_t          mbox_stub[16];    /* 16  sentinel STA_ListNode */
    uint32_t         mbox_count;       /*  4  _Atomic uint32_t */
    uint32_t         mbox_limit;       /*  4  uint32_t */
    /* concurrency */
    _Atomic uint32_t reductions;       /*  4 */
    _Atomic uint32_t sched_flags;      /*  4  upgraded from uint32_t (Q5) */
    /* heap */
    void            *heap_base;        /*  8 */
    void            *heap_bump;        /*  8 */
    void            *heap_limit_ptr;   /*  8 */
    /* supervision */
    void            *supervisor;       /*  8  struct STA_Actor * */
    uint32_t         restart_strategy; /*  4 */
    uint32_t         restart_count;    /*  4 */
    uint64_t         capability_token; /*  8 */
    /* new scheduler fields (spike-003) */
    void            *next_runnable;    /*  8  intrusive run-queue link */
    uint32_t         home_thread;      /*  4  affinity hint */
    uint32_t         _pad;             /*  4  alignment padding */
} STA_ActorRevised;
/* Expected sizeof: 8 + 40 + 8 + 24 + 24 + 8 + 16 = 128 bytes.
 * Measured value printed by test_actor_density() in the spike test. */

/* ── Function declarations ─────────────────────────────────────────────── */

/* Deque A (Chase-Lev) */
void            sta_dequeA_init  (STA_WorkDequeA *dq);
int             sta_dequeA_push  (STA_WorkDequeA *dq, STA_SpikeActor *a);
STA_SpikeActor *sta_dequeA_pop   (STA_WorkDequeA *dq);
STA_SpikeActor *sta_dequeA_steal (STA_WorkDequeA *dq);
int             sta_dequeA_size  (const STA_WorkDequeA *dq);

/* Deque B (ring buffer) */
void            sta_dequeB_init  (STA_WorkDequeB *dq);
int             sta_dequeB_push  (STA_WorkDequeB *dq, STA_SpikeActor *a);
STA_SpikeActor *sta_dequeB_pop   (STA_WorkDequeB *dq);
STA_SpikeActor *sta_dequeB_steal (STA_WorkDequeB *dq);
int             sta_dequeB_size  (const STA_WorkDequeB *dq);

/* Scheduler lifecycle */
int  sta_sched_init    (STA_Scheduler *s, int nthreads);
void sta_sched_start   (STA_Scheduler *s);
void sta_sched_stop    (STA_Scheduler *s);
void sta_sched_destroy (STA_Scheduler *s);

/* Push an actor to a specific thread's deque and wake that thread.
 * Returns 0 on success, -1 if the deque is full. */
int  sta_sched_push (STA_Scheduler *s, int thread_idx, STA_SpikeActor *actor);

/* Notification primitives (used internally and by bench wakeup tests). */
void sta_notif_init    (STA_NotifState *n);
void sta_notif_destroy (STA_NotifState *n);
void sta_notif_wake    (STA_NotifState *n);
void sta_notif_wait    (STA_NotifState *n, const _Atomic int *running);

/* Portable barrier */
void sta_barrier_init (STA_Barrier *b, int total);
void sta_barrier_wait (STA_Barrier *b);

/* Timing helper: nanoseconds from CLOCK_MONOTONIC_RAW. */
uint64_t sta_now_ns(void);
