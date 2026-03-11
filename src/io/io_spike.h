/* src/io/io_spike.h
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: async I/O integration via libuv.
 * See docs/spikes/spike-005-async-io.md and ADR 011 (to be written).
 *
 * Hard rules:
 *   - No uv_loop_t, uv_timer_t, uv_tcp_t, or any uv_*_t in this header.
 *   - Only <stdint.h>, <stdatomic.h>, <stdbool.h>, <stddef.h> included here.
 *   - libuv types are confined to io_spike.c.
 */
#pragma once

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <pthread.h>

/* Pull in the scheduler spike types we reuse: STA_WorkDequeA, STA_NotifCond,
 * STA_SpikeActor (for the deque element type), and sta_now_ns().
 * All are spike code; this inter-spike dependency is intentional. */
#include "src/scheduler/scheduler_spike.h"

/* ── I/O state ─────────────────────────────────────────────────────────── */

typedef enum {
    STA_IO_IDLE    = 0,  /* no pending I/O operation */
    STA_IO_PENDING = 1,  /* I/O initiated; actor suspended */
} STA_IoState;

/* Extends the scheduler sched_flags constants from scheduler_spike.h.
 * STA_SCHED_NONE / RUNNABLE / RUNNING are defined there.
 * SUSPENDED means the actor is waiting for an I/O completion and must NOT
 * be picked up by the scheduling loop until the I/O callback re-enqueues it. */
#define STA_SCHED_SUSPENDED 0x04u

/* ── Working spike actor (STA_IoSpikeActor) ────────────────────────────── */
/*
 * The scheduling-compatible actor struct used in io spike tests.
 * STA_SpikeActor is embedded as the first field so that:
 *   (STA_SpikeActor *)io_actor  — valid C (points to first member)
 * and the Chase-Lev deque (which stores STA_SpikeActor *) can hold
 * STA_IoSpikeActor * via cast without the deque ever dereferencing the type.
 *
 * Permanent I/O fields: io_state (4) + io_result (4) = 8 bytes.
 * All other fields in this struct are spike-only plumbing.
 */
struct STA_IoSubsystem;   /* forward — defined in io_spike.c */
struct STA_IoSched;       /* forward — defined below */

typedef struct STA_IoSpikeActor {
    /* == First field: scheduler-visible base == */
    STA_SpikeActor    sched;        /* 40 bytes — embed for deque cast validity */

    /* == Permanent I/O fields (minimum production additions) == */
    _Atomic uint32_t  io_state;     /*  4 — STA_IO_IDLE / STA_IO_PENDING       */
    int32_t           io_result;    /*  4 — completion result (0 = ok, <0 = err)*/

    /* == Spike-only plumbing == */
    void            (*run_fn)(struct STA_IoSpikeActor *);  /* 8 — execute hook */
    struct STA_IoSubsystem *io;     /*  8 — back-pointer for run_fn to call I/O */

    /* TCP: connected stream handle (uv_tcp_t *), opaque to header */
    void             *tcp_handle;   /*  8 — set by accept/connect callback       */

    /* Read/write buffer (caller-provided; valid until I/O completes) */
    uint8_t          *io_buf;       /*  8 */
    size_t            io_buf_len;   /*  8 */
    ssize_t           io_bytes;     /*  8 — bytes transferred by read/write cb   */
} STA_IoSpikeActor;
/* sizeof: 40 + 4+4 + 8+8 + 8 + 8+8+8 = 96 bytes */

/* ── Density measurement struct (STA_ActorIo) ─────────────────────────── */
/*
 * NOT used in scheduling logic. Exists only for sizeof() measurement.
 * Represents the projected production STA_Actor after applying:
 *   - STA_ActorRevised fields (ADR 009, Q6): 120 bytes
 *   - stack_base + stack_top (ADR 010 consequence): +16 bytes
 *   - io_state + io_result (this spike):           + 8 bytes
 *                                                  ─────────
 *                                             total: 144 bytes
 * Expected creation cost: 144 + 128 (nursery) + 16 (identity) = 288 bytes.
 * Scenario: Low (+8 bytes from ADR 010 baseline of 136).
 */
typedef struct STA_ActorIo {
    /* Identity */
    uint32_t         class_index;      /*  4 */
    uint32_t         actor_id;         /*  4 */
    /* Mailbox: STA_MpscList layout (ADR 008) */
    void            *mbox_tail;        /*  8  _Atomic(STA_ListNode *) */
    void            *mbox_head;        /*  8  STA_ListNode * */
    uint8_t          mbox_stub[16];    /* 16  sentinel STA_ListNode */
    uint32_t         mbox_count;       /*  4  _Atomic uint32_t */
    uint32_t         mbox_limit;       /*  4  uint32_t */
    /* Concurrency */
    _Atomic uint32_t reductions;       /*  4 */
    _Atomic uint32_t sched_flags;      /*  4 */
    /* Heap */
    void            *heap_base;        /*  8 */
    void            *heap_bump;        /*  8 */
    void            *heap_limit_ptr;   /*  8 */
    /* Supervision */
    void            *supervisor;       /*  8 */
    uint32_t         restart_strategy; /*  4 */
    uint32_t         restart_count;    /*  4 */
    uint64_t         capability_token; /*  8 */
    /* Scheduler fields (ADR 009) */
    void            *next_runnable;    /*  8 */
    uint32_t         home_thread;      /*  4 */
    uint32_t         _pad0;            /*  4 */
    /* Stack fields (ADR 010) */
    void            *stack_base;       /*  8 */
    void            *stack_top;        /*  8 */
    /* I/O suspension fields — Spike 005 additions */
    _Atomic uint32_t io_state;         /*  4 — STA_IO_IDLE / STA_IO_PENDING */
    int32_t          io_result;        /*  4 — completion result */
} STA_ActorIo;
/* Expected sizeof: 144 bytes.  Verify with static_assert in io_spike.c. */

/* ── I/O-aware scheduling context ──────────────────────────────────────── */
/*
 * Lightweight scheduler for io spike tests. Understands STA_SCHED_SUSPENDED.
 *
 * Uses a mutex-protected FIFO (not the Chase-Lev LIFO deque) so that
 * all actors get fair round-robin access from a single scheduler thread.
 * The Chase-Lev deque's LIFO owner-pop would cause starvation when a
 * compute actor repeatedly re-enqueues itself at the hot bottom while a
 * suspended actor's re-enqueue lands below the advancing top pointer.
 *
 * Intrusive link: actor->sched.next_runnable (STA_SpikeActor *) is reused
 * as the FIFO next-pointer; cast to STA_IoSpikeActor * is valid because
 * STA_SpikeActor is the first field of STA_IoSpikeActor.
 */
typedef struct STA_IoSched {
    /* FIFO run queue */
    STA_IoSpikeActor    *run_head;   /* front (dequeue end) */
    STA_IoSpikeActor    *run_tail;   /* back  (enqueue end) */
    pthread_mutex_t      run_mutex;
    /* Sleep/wake notification */
    STA_NotifCond        notif;
    _Atomic int          running;    /* 1 = active, 0 = shutdown */
    _Atomic uint32_t     remaining;  /* actors not yet retired */
    pthread_t            thread;
    struct STA_IoSubsystem *io;      /* back-pointer for I/O callbacks */
} STA_IoSched;

/* ── I/O subsystem (opaque) ─────────────────────────────────────────────── */
/*
 * Contains uv_loop_t and other libuv types — defined in io_spike.c.
 * Callers use sta_io_new() / sta_io_free() to allocate.
 */
typedef struct STA_IoSubsystem STA_IoSubsystem;

/* ── Function declarations ─────────────────────────────────────────────── */

/* I/O subsystem lifecycle */
STA_IoSubsystem *sta_io_new    (void);
void             sta_io_free   (STA_IoSubsystem *io);
int              sta_io_init   (STA_IoSubsystem *io, STA_IoSched *sched);
void             sta_io_destroy(STA_IoSubsystem *io);

/* I/O-aware scheduler lifecycle */
int  sta_io_sched_init   (STA_IoSched *s, STA_IoSubsystem *io);
void sta_io_sched_start  (STA_IoSched *s);
void sta_io_sched_stop   (STA_IoSched *s);
void sta_io_sched_destroy(STA_IoSched *s);

/* Push actor to the scheduler's run queue (called from I/O callbacks on the
 * I/O thread, and from test setup on the main thread). */
int  sta_io_sched_push(STA_IoSched *s, STA_IoSpikeActor *actor);

/* I/O operations — all called from actor run_fn on the scheduler thread.
 * Each sets actor->io_state = STA_IO_PENDING and actor->sched_flags =
 * STA_SCHED_SUSPENDED before returning.  The I/O callback calls
 * sta_io_resume_actor() which sets io_state = IDLE and re-enqueues the actor.
 *
 * Memory ordering:
 *   - io_state IDLE→PENDING: memory_order_release (on scheduler thread)
 *   - io_state PENDING→IDLE: memory_order_release (on I/O thread)
 *   - Scheduler thread reads io_state with memory_order_acquire
 *   - The release/acquire pair provides happens-before for io_result and
 *     tcp_handle writes preceding the IDLE store.
 */
void sta_io_timer_start  (STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                           uint64_t delay_ms);

void sta_io_tcp_listen   (STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                           uint16_t port);
void sta_io_tcp_connect  (STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                           const char *host, uint16_t port);
void sta_io_tcp_read     (STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                           uint8_t *buf, size_t len);
void sta_io_tcp_write    (STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                           const uint8_t *buf, size_t len);

/* Internal: called by libuv callbacks on the I/O thread to restore the actor
 * to runnable state.  Writes result, transitions io_state PENDING→IDLE with
 * release semantics, then calls sta_io_sched_push(). */
void sta_io_resume_actor(STA_IoSpikeActor *actor, int32_t result);
