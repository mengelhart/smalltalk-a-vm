/* src/io/io_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: async I/O integration via libuv.
 * See docs/spikes/spike-005-async-io.md and ADR 011 (to be written).
 *
 * Architecture (Option A — dedicated I/O thread):
 *   Scheduler threads never call uv_run() or any blocking syscall.
 *   One OS thread owns the uv_loop_t and runs uv_run(UV_RUN_DEFAULT).
 *   I/O requests are dispatched to this thread via uv_async_send() after
 *   being placed in a mutex-protected FIFO.  Completion callbacks fire on
 *   the I/O thread and call sta_io_resume_actor(), which pushes the actor
 *   back to the scheduler's Chase-Lev deque via sta_io_sched_push().
 *
 * Re-enqueue model (Model A — direct deque push):
 *   The I/O callback calls sta_io_sched_push() directly.  No message
 *   allocation on the completion path.  Actor resumes mid-message rather
 *   than at a clean message boundary; this is acceptable in the spike
 *   because actors are always at the top of their execution when suspended.
 *   See Q6 in the spike doc for the full tradeoff statement.
 */

#include "io_spike.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

/* ── Static assert: density measurement struct must be 144 bytes ───────── */
_Static_assert(sizeof(STA_ActorIo) == 144,
    "STA_ActorIo size mismatch: expected 144 bytes for Low density scenario");

/* ── I/O request types ─────────────────────────────────────────────────── */

typedef enum {
    STA_IOREQ_TIMER   = 1,
    STA_IOREQ_LISTEN  = 2,
    STA_IOREQ_CONNECT = 3,
    STA_IOREQ_READ    = 4,
    STA_IOREQ_WRITE   = 5,
    STA_IOREQ_STOP    = 99,   /* sentinel: shut down the I/O loop */
} STA_IoReqType;

typedef struct STA_IoRequest {
    struct STA_IoRequest *next;    /* intrusive linked list */
    STA_IoReqType         type;
    STA_IoSpikeActor     *actor;
    /* Per-request parameters */
    union {
        struct { uint64_t delay_ms; }                       timer;
        struct { uint16_t port; }                           listen;
        struct { char host[64]; uint16_t port; }            connect;
        struct { /* buf/len read from actor->io_buf/len */ } read;
        struct { const uint8_t *buf; size_t len; }          write;
    } u;
} STA_IoRequest;

/* ── I/O subsystem (full definition — uv types confined here) ──────────── */

struct STA_IoSubsystem {
    uv_loop_t        loop;
    uv_async_t       wakeup;      /* cross-thread wakeup for new requests */
    pthread_t        thread;
    STA_IoSched     *sched;       /* back-pointer to the scheduler */

    /* Startup barrier: I/O thread signals ready before sta_io_init returns */
    pthread_mutex_t  start_mutex;
    pthread_cond_t   start_cond;
    int              started;     /* protected by start_mutex */

    /* Request FIFO (scheduler threads → I/O thread) */
    pthread_mutex_t  fifo_mutex;
    STA_IoRequest   *fifo_head;
    STA_IoRequest   *fifo_tail;

    /* Shutdown flag */
    _Atomic int      running;
};

/* ── Forward declarations (callbacks) ─────────────────────────────────── */

static void wakeup_cb       (uv_async_t *handle);
static void handle_close_cb (uv_handle_t *handle);
static void noop_close_cb   (uv_handle_t *handle);
static void close_walk_cb   (uv_handle_t *handle, void *arg);
static void timer_cb        (uv_timer_t *handle);
static void connection_cb   (uv_stream_t *server, int status);
static void connect_cb      (uv_connect_t *req, int status);
static void alloc_cb        (uv_handle_t *handle, size_t suggested, uv_buf_t *buf);
static void read_cb         (uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void write_cb        (uv_write_t *req, int status);

/* ── FIFO helpers (called under fifo_mutex or on I/O thread only) ──────── */

static void fifo_enqueue(STA_IoSubsystem *io, STA_IoRequest *req) {
    req->next = NULL;
    pthread_mutex_lock(&io->fifo_mutex);
    if (io->fifo_tail) {
        io->fifo_tail->next = req;
    } else {
        io->fifo_head = req;
    }
    io->fifo_tail = req;
    pthread_mutex_unlock(&io->fifo_mutex);
}

static STA_IoRequest *fifo_drain(STA_IoSubsystem *io) {
    pthread_mutex_lock(&io->fifo_mutex);
    STA_IoRequest *list = io->fifo_head;
    io->fifo_head = io->fifo_tail = NULL;
    pthread_mutex_unlock(&io->fifo_mutex);
    return list;
}

/* ── I/O thread main ────────────────────────────────────────────────────── */

static void *io_thread_main(void *arg) {
    STA_IoSubsystem *io = arg;

    /* Signal that the loop is running before blocking in uv_run. */
    pthread_mutex_lock(&io->start_mutex);
    io->started = 1;
    pthread_cond_signal(&io->start_cond);
    pthread_mutex_unlock(&io->start_mutex);

    uv_run(&io->loop, UV_RUN_DEFAULT);
    return NULL;
}

/* ── Wakeup async callback (I/O thread) ─────────────────────────────────── */
/*
 * Fires when any scheduler thread calls uv_async_send(&io->wakeup).
 * Drains the request FIFO and processes each pending request.
 */
static void wakeup_cb(uv_async_t *handle) {
    STA_IoSubsystem *io = handle->data;

    /* Check shutdown flag first. */
    if (!atomic_load_explicit(&io->running, memory_order_acquire)) {
        uv_stop(&io->loop);
        return;
    }

    STA_IoRequest *req = fifo_drain(io);

    while (req) {
        STA_IoRequest *next = req->next;

        if (req->type == STA_IOREQ_STOP) {
            free(req);
            req = next;
            uv_stop(&io->loop);
            return;
        }

        switch (req->type) {

        case STA_IOREQ_TIMER: {
            uv_timer_t *t = malloc(sizeof(uv_timer_t));
            if (!t) { sta_io_resume_actor(req->actor, UV_ENOMEM); break; }
            uv_timer_init(&io->loop, t);
            t->data = req->actor;
            uv_timer_start(t, timer_cb, req->u.timer.delay_ms, 0);
            break;
        }

        case STA_IOREQ_LISTEN: {
            uv_tcp_t *srv = malloc(sizeof(uv_tcp_t));
            if (!srv) { sta_io_resume_actor(req->actor, UV_ENOMEM); break; }
            uv_tcp_init(&io->loop, srv);
            srv->data = req->actor;
            /* Store listen handle in actor->tcp_handle until accept */
            req->actor->tcp_handle = srv;

            struct sockaddr_in addr;
            uv_ip4_addr("0.0.0.0", req->u.listen.port, &addr);
            uv_tcp_bind(srv, (const struct sockaddr *)&addr, 0);
            int r = uv_listen((uv_stream_t *)srv, 1, connection_cb);
            if (r != 0) {
                req->actor->tcp_handle = NULL;
                uv_close((uv_handle_t *)srv, handle_close_cb);
                sta_io_resume_actor(req->actor, r);
            }
            break;
        }

        case STA_IOREQ_CONNECT: {
            uv_tcp_t *cli = malloc(sizeof(uv_tcp_t));
            if (!cli) { sta_io_resume_actor(req->actor, UV_ENOMEM); break; }
            uv_tcp_init(&io->loop, cli);
            /* Store handle in actor before connect callback can fire */
            req->actor->tcp_handle = cli;

            uv_connect_t *creq = malloc(sizeof(uv_connect_t));
            if (!creq) {
                req->actor->tcp_handle = NULL;
                uv_close((uv_handle_t *)cli, handle_close_cb);
                sta_io_resume_actor(req->actor, UV_ENOMEM);
                break;
            }
            creq->data = req->actor;

            struct sockaddr_in addr;
            uv_ip4_addr(req->u.connect.host, req->u.connect.port, &addr);
            int r = uv_tcp_connect(creq, cli,
                                   (const struct sockaddr *)&addr, connect_cb);
            if (r != 0) {
                req->actor->tcp_handle = NULL;
                free(creq);
                uv_close((uv_handle_t *)cli, handle_close_cb);
                sta_io_resume_actor(req->actor, r);
            }
            break;
        }

        case STA_IOREQ_READ: {
            uv_tcp_t *stream = req->actor->tcp_handle;
            if (!stream) { sta_io_resume_actor(req->actor, UV_EINVAL); break; }
            stream->data = req->actor;
            int r = uv_read_start((uv_stream_t *)stream, alloc_cb, read_cb);
            if (r != 0) {
                sta_io_resume_actor(req->actor, r);
            }
            break;
        }

        case STA_IOREQ_WRITE: {
            uv_tcp_t *stream = req->actor->tcp_handle;
            if (!stream) { sta_io_resume_actor(req->actor, UV_EINVAL); break; }

            uv_write_t *wreq = malloc(sizeof(uv_write_t));
            if (!wreq) { sta_io_resume_actor(req->actor, UV_ENOMEM); break; }
            wreq->data = req->actor;

            uv_buf_t uvbuf = uv_buf_init((char *)req->u.write.buf,
                                          (unsigned)req->u.write.len);
            int r = uv_write(wreq, (uv_stream_t *)stream, &uvbuf, 1, write_cb);
            if (r != 0) {
                free(wreq);
                sta_io_resume_actor(req->actor, r);
            }
            break;
        }

        default:
            break;
        }

        free(req);
        req = next;
    }
}

/* ── libuv callbacks ────────────────────────────────────────────────────── */

static void handle_close_cb(uv_handle_t *handle) {
    free(handle);
}

static void noop_close_cb(uv_handle_t *handle) {
    (void)handle;  /* embedded handle — do not free */
}

/* Called by uv_walk in sta_io_destroy to close any handles left open
 * (e.g., TCP connections that actors did not explicitly close). */
static void close_walk_cb(uv_handle_t *handle, void *arg) {
    (void)arg;
    if (!uv_is_closing(handle)) {
        uv_close(handle, handle_close_cb);
    }
}

static void timer_cb(uv_timer_t *handle) {
    STA_IoSpikeActor *actor = handle->data;
    uv_timer_stop(handle);
    uv_close((uv_handle_t *)handle, handle_close_cb);
    sta_io_resume_actor(actor, 0);
}

static void connection_cb(uv_stream_t *server, int status) {
    STA_IoSpikeActor *actor = server->data;
    STA_IoSubsystem  *io    = actor->io;  /* back-pointer via actor */

    if (status < 0) {
        uv_close((uv_handle_t *)server, handle_close_cb);
        actor->tcp_handle = NULL;
        sta_io_resume_actor(actor, status);
        return;
    }

    uv_tcp_t *client = malloc(sizeof(uv_tcp_t));
    if (!client) {
        uv_close((uv_handle_t *)server, handle_close_cb);
        actor->tcp_handle = NULL;
        sta_io_resume_actor(actor, UV_ENOMEM);
        return;
    }
    uv_tcp_init(&io->loop, client);
    client->data = actor;

    int r = uv_accept(server, (uv_stream_t *)client);

    /* Close the listening socket — spike handles one connection */
    uv_close((uv_handle_t *)server, handle_close_cb);

    if (r != 0) {
        uv_close((uv_handle_t *)client, handle_close_cb);
        actor->tcp_handle = NULL;
        sta_io_resume_actor(actor, r);
        return;
    }

    /* Swap: actor now holds the accepted connection, not the listen socket */
    actor->tcp_handle = client;
    sta_io_resume_actor(actor, 0);
}

static void connect_cb(uv_connect_t *req, int status) {
    STA_IoSpikeActor *actor = req->data;
    free(req);
    /* tcp_handle was already set to the uv_tcp_t in the CONNECT request */
    if (status != 0) {
        uv_close((uv_handle_t *)actor->tcp_handle, handle_close_cb);
        actor->tcp_handle = NULL;
    }
    sta_io_resume_actor(actor, status);
}

static void alloc_cb(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
    STA_IoSpikeActor *actor = handle->data;
    (void)suggested;
    buf->base = (char *)actor->io_buf;
    buf->len  = (unsigned)actor->io_buf_len;
}

static void read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
    STA_IoSpikeActor *actor = stream->data;
    (void)buf;

    if (nread == UV_EOF || nread == 0) return;  /* wait for real data */

    uv_read_stop(stream);

    if (nread < 0) {
        actor->io_bytes = 0;
        sta_io_resume_actor(actor, (int32_t)nread);
    } else {
        actor->io_bytes = nread;
        sta_io_resume_actor(actor, 0);
    }
}

static void write_cb(uv_write_t *req, int status) {
    STA_IoSpikeActor *actor = req->data;
    free(req);
    actor->io_bytes = (status == 0) ? (ssize_t)actor->io_buf_len : 0;
    sta_io_resume_actor(actor, status);
}

/* ── sta_io_resume_actor ────────────────────────────────────────────────── */
/*
 * Called from libuv callbacks on the I/O thread.
 * Writes result, transitions io_state PENDING→IDLE (release), then
 * re-enqueues the actor via sta_io_sched_push (Model A: direct deque push).
 *
 * Memory ordering:
 *   - actor->io_result written BEFORE the release store on io_state.
 *     Any scheduler thread that acquire-loads io_state == IDLE is
 *     guaranteed to see the io_result write (happens-before).
 *   - actor->tcp_handle is similarly ordered: written before IDLE store.
 */
void sta_io_resume_actor(STA_IoSpikeActor *actor, int32_t result) {
    actor->io_result = result;
    atomic_store_explicit(&actor->io_state, STA_IO_IDLE, memory_order_release);
    sta_io_sched_push(actor->io->sched, actor);
}

/* ── FIFO run-queue helpers ─────────────────────────────────────────────── */
/*
 * Enqueue at the tail (used by sta_io_sched_push and re-queue in the loop).
 * Caller must hold s->run_mutex OR ensure single-writer access.
 * Reuses actor->sched.next_runnable as the intrusive link.
 */
static void fifo_run_enqueue_locked(STA_IoSched *s, STA_IoSpikeActor *actor) {
    /* Clear the intrusive link using the embedded STA_SpikeActor field. */
    actor->sched.next_runnable = NULL;
    if (s->run_tail) {
        s->run_tail->sched.next_runnable = (struct STA_SpikeActor *)actor;
    } else {
        s->run_head = actor;
    }
    s->run_tail = actor;
}

/* Dequeue from the head. Returns NULL if empty. Caller holds run_mutex. */
static STA_IoSpikeActor *fifo_run_dequeue_locked(STA_IoSched *s) {
    STA_IoSpikeActor *actor = s->run_head;
    if (!actor) return NULL;
    s->run_head = (STA_IoSpikeActor *)actor->sched.next_runnable;
    if (!s->run_head) s->run_tail = NULL;
    actor->sched.next_runnable = NULL;
    return actor;
}

/* ── sta_io_sched_push ──────────────────────────────────────────────────── */
/*
 * Appends actor to the FIFO and wakes the scheduler thread.
 * Called from the I/O thread (resume path) and from test setup.
 */
int sta_io_sched_push(STA_IoSched *s, STA_IoSpikeActor *actor) {
    atomic_store_explicit(&actor->sched.sched_flags, STA_SCHED_RUNNABLE,
                          memory_order_release);
    pthread_mutex_lock(&s->run_mutex);
    fifo_run_enqueue_locked(s, actor);
    pthread_mutex_unlock(&s->run_mutex);
    sta_notif_wake(&s->notif);
    return 0;
}

/* ── I/O-aware scheduling loop ─────────────────────────────────────────── */
/*
 * FIFO-based single-thread scheduler that understands STA_SCHED_SUSPENDED.
 *
 * After actor->run_fn() returns, inspect sched_flags:
 *   SUSPENDED: actor is waiting for I/O; I/O callback will call
 *              sta_io_sched_push() to re-enqueue. Do NOT re-enqueue here.
 *   RUNNABLE:  I/O completed before we read the flag; callback already
 *              re-enqueued. Do NOT re-enqueue again.
 *   RUNNING:   normal exit (compute actor or post-IO return); re-enqueue
 *              or retire based on max_runs.
 *
 * The FIFO gives every actor fair access — essential when a compute actor
 * re-enqueues on every quantum, otherwise it would monopolise the thread
 * if a LIFO structure were used.
 */
static void *io_sched_thread_main(void *arg) {
    STA_IoSched *s = arg;

    while (atomic_load_explicit(&s->running, memory_order_acquire)) {

        pthread_mutex_lock(&s->run_mutex);
        STA_IoSpikeActor *actor = fifo_run_dequeue_locked(s);
        pthread_mutex_unlock(&s->run_mutex);

        if (!actor) {
            sta_notif_wait(&s->notif, &s->running);
            continue;
        }

        /* Mark running and record wake timestamp for latency measurement. */
        atomic_store_explicit(&actor->sched.sched_flags, STA_SCHED_RUNNING,
                              memory_order_release);
        atomic_store_explicit(&actor->sched.start_ns, sta_now_ns(),
                              memory_order_release);

        /* Execute the actor's current task. */
        actor->run_fn(actor);

        /* Increment run count unconditionally. */
        uint32_t rc = atomic_fetch_add_explicit(&actor->sched.run_count, 1u,
                                                memory_order_acq_rel) + 1u;

        /* Read sched_flags to determine post-execute fate. */
        uint32_t flags = atomic_load_explicit(&actor->sched.sched_flags,
                                              memory_order_acquire);

        if (flags == STA_SCHED_SUSPENDED) {
            /* Suspended for I/O — sta_io_sched_push() will re-enqueue. */
            continue;
        }
        if (flags == STA_SCHED_RUNNABLE) {
            /* I/O callback fired and re-enqueued before we got here. Skip. */
            continue;
        }

        /* Normal case (RUNNING): decide retire vs re-enqueue. */
        if (actor->sched.max_runs > 0 && rc >= actor->sched.max_runs) {
            atomic_store_explicit(&actor->sched.sched_flags, STA_SCHED_NONE,
                                  memory_order_release);
            atomic_fetch_sub_explicit(&s->remaining, 1u, memory_order_acq_rel);
        } else {
            /* Re-enqueue at the tail of the FIFO (round-robin). */
            atomic_store_explicit(&actor->sched.sched_flags, STA_SCHED_RUNNABLE,
                                  memory_order_release);
            pthread_mutex_lock(&s->run_mutex);
            fifo_run_enqueue_locked(s, actor);
            pthread_mutex_unlock(&s->run_mutex);
        }
    }
    return NULL;
}

/* ── STA_IoSched lifecycle ──────────────────────────────────────────────── */

int sta_io_sched_init(STA_IoSched *s, STA_IoSubsystem *io) {
    s->run_head = s->run_tail = NULL;
    pthread_mutex_init(&s->run_mutex, NULL);
    sta_notif_init(&s->notif);
    atomic_store_explicit(&s->running,   1,  memory_order_relaxed);
    atomic_store_explicit(&s->remaining, 0u, memory_order_relaxed);
    s->io = io;
    if (io) io->sched = s;
    return 0;
}

void sta_io_sched_start(STA_IoSched *s) {
    pthread_create(&s->thread, NULL, io_sched_thread_main, s);
}

void sta_io_sched_stop(STA_IoSched *s) {
    atomic_store_explicit(&s->running, 0, memory_order_release);
    sta_notif_wake(&s->notif);
    pthread_join(s->thread, NULL);
}

void sta_io_sched_destroy(STA_IoSched *s) {
    sta_notif_destroy(&s->notif);
    pthread_mutex_destroy(&s->run_mutex);
}

/* ── STA_IoSubsystem lifecycle ──────────────────────────────────────────── */

STA_IoSubsystem *sta_io_new(void) {
    STA_IoSubsystem *io = calloc(1, sizeof(STA_IoSubsystem));
    return io;
}

void sta_io_free(STA_IoSubsystem *io) {
    free(io);
}

int sta_io_init(STA_IoSubsystem *io, STA_IoSched *sched) {
    assert(io);
    io->sched = sched;
    if (sched) sched->io = io;

    atomic_store_explicit(&io->running, 1, memory_order_relaxed);
    io->fifo_head = io->fifo_tail = NULL;

    pthread_mutex_init(&io->fifo_mutex, NULL);
    pthread_mutex_init(&io->start_mutex, NULL);
    pthread_cond_init(&io->start_cond, NULL);
    io->started = 0;

    uv_loop_init(&io->loop);
    io->loop.data = io;   /* back-pointer for connection_cb */

    /* Sentinel async handle: keeps the loop alive until sta_io_destroy. */
    uv_async_init(&io->loop, &io->wakeup, wakeup_cb);
    io->wakeup.data = io;

    /* Start I/O thread. */
    pthread_create(&io->thread, NULL, io_thread_main, io);

    /* Wait until uv_run has started. */
    pthread_mutex_lock(&io->start_mutex);
    while (!io->started) {
        pthread_cond_wait(&io->start_cond, &io->start_mutex);
    }
    pthread_mutex_unlock(&io->start_mutex);

    return 0;
}

void sta_io_destroy(STA_IoSubsystem *io) {
    assert(io);

    /* Signal the I/O thread to stop. wakeup_cb checks running and calls
     * uv_stop(), which causes uv_run(UV_RUN_DEFAULT) to return. */
    atomic_store_explicit(&io->running, 0, memory_order_release);
    uv_async_send(&io->wakeup);

    pthread_join(io->thread, NULL);
    /* The I/O thread has exited; we are now the sole owner of the loop. */

    /* Close the wakeup async handle (embedded — use noop callback, not free). */
    uv_close((uv_handle_t *)&io->wakeup, noop_close_cb);

    /* Walk remaining open handles (e.g., TCP connections not explicitly closed
     * by actors) and close them so uv_loop_close succeeds. */
    uv_walk(&io->loop, close_walk_cb, NULL);

    /* Run until all close callbacks have fired. */
    uv_run(&io->loop, UV_RUN_DEFAULT);

    uv_loop_close(&io->loop);

    pthread_cond_destroy(&io->start_cond);
    pthread_mutex_destroy(&io->start_mutex);
    pthread_mutex_destroy(&io->fifo_mutex);

    /* Drain any FIFO requests that were never processed. */
    STA_IoRequest *req = fifo_drain(io);
    while (req) {
        STA_IoRequest *next = req->next;
        free(req);
        req = next;
    }
}

/* ── I/O request helpers (called on scheduler thread from actor run_fn) ─── */
/*
 * Each helper:
 *   1. Sets sched_flags = SUSPENDED (release)   ← prevents re-queue
 *   2. Sets io_state    = PENDING   (release)   ← marks I/O in progress
 *   3. Enqueues the request in the FIFO (under mutex)
 *   4. Calls uv_async_send() — non-blocking, wakes the I/O thread
 *   5. Returns immediately
 *
 * The ordering in steps 1–2 before the FIFO enqueue ensures the I/O
 * callback (which fires after the I/O thread processes the request) always
 * sees SUSPENDED when it reads sched_flags, even for very fast I/O.
 */

static void suspend_actor(STA_IoSpikeActor *actor) {
    atomic_store_explicit(&actor->sched.sched_flags, STA_SCHED_SUSPENDED,
                          memory_order_release);
    atomic_store_explicit(&actor->io_state, STA_IO_PENDING,
                          memory_order_release);
}

void sta_io_timer_start(STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                         uint64_t delay_ms) {
    suspend_actor(actor);

    STA_IoRequest *req = calloc(1, sizeof(STA_IoRequest));
    req->type           = STA_IOREQ_TIMER;
    req->actor          = actor;
    req->u.timer.delay_ms = delay_ms;

    fifo_enqueue(io, req);
    uv_async_send(&io->wakeup);
}

void sta_io_tcp_listen(STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                        uint16_t port) {
    suspend_actor(actor);

    STA_IoRequest *req = calloc(1, sizeof(STA_IoRequest));
    req->type          = STA_IOREQ_LISTEN;
    req->actor         = actor;
    req->u.listen.port = port;

    fifo_enqueue(io, req);
    uv_async_send(&io->wakeup);
}

void sta_io_tcp_connect(STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                         const char *host, uint16_t port) {
    suspend_actor(actor);

    STA_IoRequest *req = calloc(1, sizeof(STA_IoRequest));
    req->type            = STA_IOREQ_CONNECT;
    req->actor           = actor;
    req->u.connect.port  = port;
    strncpy(req->u.connect.host, host, sizeof(req->u.connect.host) - 1);

    fifo_enqueue(io, req);
    uv_async_send(&io->wakeup);
}

void sta_io_tcp_read(STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                      uint8_t *buf, size_t len) {
    actor->io_buf     = buf;
    actor->io_buf_len = len;
    suspend_actor(actor);

    STA_IoRequest *req = calloc(1, sizeof(STA_IoRequest));
    req->type  = STA_IOREQ_READ;
    req->actor = actor;

    fifo_enqueue(io, req);
    uv_async_send(&io->wakeup);
}

void sta_io_tcp_write(STA_IoSubsystem *io, STA_IoSpikeActor *actor,
                       const uint8_t *buf, size_t len) {
    actor->io_buf     = (uint8_t *)buf;
    actor->io_buf_len = len;
    suspend_actor(actor);

    STA_IoRequest *req = calloc(1, sizeof(STA_IoRequest));
    req->type         = STA_IOREQ_WRITE;
    req->actor        = actor;
    req->u.write.buf  = buf;
    req->u.write.len  = len;

    fifo_enqueue(io, req);
    uv_async_send(&io->wakeup);
}
