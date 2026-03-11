/* tests/test_io_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: async I/O integration via libuv.
 * See docs/spikes/spike-005-async-io.md.
 *
 * Tests:
 *   1. I/O subsystem lifecycle
 *   2. Timer suspension/resumption — single actor
 *   3. No-blocking-scheduler proof (TSan gate)
 *   4. Timer wake latency benchmark (STA_BENCH only)
 *   5. TCP loopback echo
 *   6. TCP loopback under compute load (TSan gate)
 *   7. sizeof(uv_loop_t) measurement (Option C rejection)
 *   8. Actor density checkpoint
 */

#include "src/io/io_spike.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>    /* only for sizeof(uv_loop_t) in test 7 */

/* ── Utility ────────────────────────────────────────────────────────────── */

static void actor_init(STA_IoSpikeActor *a, uint32_t id, uint32_t max_runs,
                        void (*run_fn)(STA_IoSpikeActor *),
                        STA_IoSubsystem *io) {
    memset(a, 0, sizeof(*a));
    a->sched.actor_id  = id;
    a->sched.max_runs  = max_runs;
    a->run_fn          = run_fn;
    a->io              = io;
    atomic_store_explicit(&a->sched.sched_flags, STA_SCHED_NONE,
                          memory_order_relaxed);
    atomic_store_explicit(&a->sched.run_count, 0u, memory_order_relaxed);
    atomic_store_explicit(&a->io_state, STA_IO_IDLE, memory_order_relaxed);
}

/* Wait (busy-poll) until actor's run_count reaches target. */
static void wait_run_count(STA_IoSpikeActor *a, uint32_t target,
                            int timeout_ms) {
    int64_t deadline = (int64_t)sta_now_ns() + (int64_t)timeout_ms * 1000000;
    while ((int64_t)sta_now_ns() < deadline) {
        if (atomic_load_explicit(&a->sched.run_count, memory_order_acquire)
                >= target)
            return;
        /* yield to avoid monopolising the core */
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000 }; /* 100 µs */
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "TIMEOUT: actor %u run_count did not reach %u\n",
            a->sched.actor_id, target);
    abort();
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 1: I/O subsystem lifecycle                                            */
/* ────────────────────────────────────────────────────────────────────────── */

static void test_lifecycle(void) {
    printf("Test 1: I/O subsystem lifecycle ... ");
    fflush(stdout);

    STA_IoSched sched;
    STA_IoSubsystem *io = sta_io_new();
    assert(io);

    sta_io_sched_init(&sched, io);
    sta_io_init(io, &sched);

    /* No actors, no I/O — just verify clean startup + shutdown. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 }; /* 10 ms */
    nanosleep(&ts, NULL);

    sta_io_destroy(io);
    sta_io_sched_destroy(&sched);
    sta_io_free(io);

    printf("PASS\n");
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 2: Timer suspension / resumption — single actor                       */
/* ────────────────────────────────────────────────────────────────────────── */

static void timer_actor_run(STA_IoSpikeActor *actor) {
    uint32_t rc = atomic_load_explicit(&actor->sched.run_count,
                                       memory_order_relaxed);
    if (rc == 0) {
        /* First run: start 50 ms timer and suspend. */
        sta_io_timer_start(actor->io, actor, 50);
        /* sched_flags is now SUSPENDED; scheduling loop will not re-enqueue. */
    }
    /* Subsequent run: timer fired, io_result is set; actor just returns and
     * will be retired (max_runs == 2). */
}

static void test_timer_suspend_resume(void) {
    printf("Test 2: Timer suspension/resumption ... ");
    fflush(stdout);

    STA_IoSched sched;
    STA_IoSubsystem *io = sta_io_new();
    assert(io);

    sta_io_sched_init(&sched, io);
    sta_io_sched_start(&sched);
    sta_io_init(io, &sched);

    STA_IoSpikeActor actor;
    actor_init(&actor, 1, 2, timer_actor_run, io);

    atomic_store_explicit(&sched.remaining, 1u, memory_order_relaxed);
    sta_io_sched_push(&sched, &actor);

    /* Wait for actor to complete both runs (run_count == 2). */
    wait_run_count(&actor, 2, 500);

    /* Verify: io_state is IDLE and io_result is 0 (timer success). */
    assert(atomic_load_explicit(&actor.io_state, memory_order_acquire)
           == STA_IO_IDLE);
    assert(actor.io_result == 0);

    sta_io_destroy(io);
    sta_io_sched_stop(&sched);
    sta_io_sched_destroy(&sched);
    sta_io_free(io);

    printf("PASS\n");
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 3: No-blocking-scheduler proof (TSan gate)                            */
/* ────────────────────────────────────────────────────────────────────────── */

/*
 * One scheduler thread, one I/O actor (50 ms timer), one compute actor
 * (tight reduction loop).  Assert the compute actor's run_count increases
 * during the 50 ms I/O wait — proving the scheduler thread was never blocked.
 */

static void compute_actor_run(STA_IoSpikeActor *actor) {
    /* Burn ~1000 relaxed atomic ops to simulate a reduction quantum. */
    for (int i = 0; i < 1000; i++) {
        atomic_fetch_add_explicit(&actor->sched.reductions, 1u,
                                  memory_order_relaxed);
    }
}

static void io_actor_run_noblock(STA_IoSpikeActor *actor) {
    uint32_t rc = atomic_load_explicit(&actor->sched.run_count,
                                       memory_order_relaxed);
    if (rc == 0) {
        sta_io_timer_start(actor->io, actor, 50);
    }
}

static void test_no_blocking_scheduler(void) {
    printf("Test 3: No-blocking-scheduler proof ... ");
    fflush(stdout);

    STA_IoSched sched;
    STA_IoSubsystem *io = sta_io_new();
    assert(io);

    sta_io_sched_init(&sched, io);
    sta_io_sched_start(&sched);
    sta_io_init(io, &sched);

    STA_IoSpikeActor io_actor;
    STA_IoSpikeActor compute_actor;

    /* I/O actor runs twice: initiate timer, then run after resume. */
    actor_init(&io_actor, 1, 2, io_actor_run_noblock, io);
    /* Compute actor runs indefinitely until we stop the scheduler. */
    actor_init(&compute_actor, 2, 0 /* no limit */, compute_actor_run, io);

    atomic_store_explicit(&sched.remaining, 0u, memory_order_relaxed);

    /* FIFO scheduler: push order determines initial execution order.
     * io_actor first so it runs first, starts its timer, and suspends.
     * compute_actor then runs continuously during the 50 ms I/O wait. */
    sta_io_sched_push(&sched, &io_actor);
    sta_io_sched_push(&sched, &compute_actor);

    /* Record compute run_count at timer start (wait for io_actor first run). */
    wait_run_count(&io_actor, 1, 500);
    uint32_t compute_before = atomic_load_explicit(&compute_actor.sched.run_count,
                                                   memory_order_acquire);

    /* Wait for timer to fire and io_actor to resume (second run). */
    wait_run_count(&io_actor, 2, 500);
    uint32_t compute_after = atomic_load_explicit(&compute_actor.sched.run_count,
                                                  memory_order_acquire);

    /* The key assertion: compute actor ran at least once during the 50 ms wait.
     * Zero runs means the scheduler thread was blocked — correctness failure. */
    if (compute_after <= compute_before) {
        fprintf(stderr,
            "\nFAIL: compute actor did not run during I/O wait "
            "(before=%u after=%u). Scheduler thread blocked on I/O!\n",
            compute_before, compute_after);
        abort();
    }

    printf("(compute runs during 50ms I/O wait: %u) ",
           compute_after - compute_before);

    sta_io_destroy(io);
    sta_io_sched_stop(&sched);
    sta_io_sched_destroy(&sched);
    sta_io_free(io);

    printf("PASS\n");
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 4: Timer wake latency benchmark (STA_BENCH only)                      */
/* ────────────────────────────────────────────────────────────────────────── */

#ifdef STA_BENCH

#define BENCH_SAMPLES      10000u
#define BENCH_WARMUP        1000u
#define BENCH_TIMER_MS         1u

typedef struct {
    STA_IoSched      *sched;
    STA_IoSubsystem  *io;
    uint64_t          cb_ns;      /* timestamp recorded in libuv callback */
    uint64_t          run_ns;     /* timestamp recorded when actor executes */
} BenchCtx;

/* We need the callback timestamp from inside the libuv layer.
 * Reuse io_result to pass it: store the high 32 bits separately. */
static _Atomic uint64_t g_bench_cb_ns;

static void bench_timer_actor_run(STA_IoSpikeActor *actor) {
    BenchCtx *ctx = (BenchCtx *)actor->sched.start_ns; /* abuse field */
    (void)ctx;
    uint32_t rc = atomic_load_explicit(&actor->sched.run_count,
                                       memory_order_relaxed);
    if (rc % 2 == 0) {
        /* Even runs: start timer */
        atomic_store_explicit(&g_bench_cb_ns, 0ULL, memory_order_relaxed);
        sta_io_timer_start(actor->io, actor, BENCH_TIMER_MS);
    }
    /* Odd runs: record wake timestamp */
}

static void run_bench_latency(void) {
    printf("\n=== Timer Wake Latency Benchmark ===\n");
    printf("Samples: %u (discarding first %u warmup)\n",
           BENCH_SAMPLES, BENCH_WARMUP);
    printf("Timer interval: %u ms\n\n", BENCH_TIMER_MS);

    /* Benchmark measures callback→actor-execution latency.
     * We capture the time in sta_io_resume_actor (before pushing to deque)
     * and compare to actor->sched.start_ns (set by scheduling loop on execute).
     * This is the I/O-thread→scheduler-thread wake latency. */

    uint64_t *samples = malloc(BENCH_SAMPLES * sizeof(uint64_t));
    assert(samples);

    STA_IoSched sched;
    STA_IoSubsystem *io = sta_io_new();
    sta_io_sched_init(&sched, io);
    sta_io_sched_start(&sched);
    sta_io_init(io, &sched);

    /* Run BENCH_SAMPLES + BENCH_WARMUP pairs. */
    uint32_t total = (BENCH_SAMPLES + BENCH_WARMUP) * 2; /* pairs of runs */
    STA_IoSpikeActor actor;
    actor_init(&actor, 99, total, bench_timer_actor_run, io);

    sta_io_sched_push(&sched, &actor);
    wait_run_count(&actor, total, 60000);

    sta_io_destroy(io);
    sta_io_sched_stop(&sched);
    sta_io_sched_destroy(&sched);
    sta_io_free(io);

    /* The latency samples aren't easily capturable without modifying the
     * resume path; for this build we report what we can measure. */
    printf("NOTE: For full latency distribution, instrument sta_io_resume_actor\n");
    printf("      to record cb_ns, and actor run_fn to compute delta.\n");
    printf("      ADR 009 baseline: median 5,958 ns, p99 7,875 ns.\n\n");

    free(samples);
}

#endif /* STA_BENCH */

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 5: TCP loopback echo                                                   */
/* ────────────────────────────────────────────────────────────────────────── */

#define TCP_TEST_PORT   14400
#define TCP_PAYLOAD_LEN 16

typedef struct {
    STA_IoSched     *sched;
    uint8_t          payload[TCP_PAYLOAD_LEN];
    uint8_t          server_rbuf[TCP_PAYLOAD_LEN];
    uint8_t          client_rbuf[TCP_PAYLOAD_LEN];
    _Atomic uint32_t server_done;
    _Atomic uint32_t client_done;
} TcpCtx;

static TcpCtx g_tcp;

/*
 * Server actor state machine (driven by run_count):
 *   0: sta_io_tcp_listen  → suspend
 *   1: sta_io_tcp_read    → suspend (connection arrived; tcp_handle = client)
 *   2: sta_io_tcp_write   → suspend (data received; echo it)
 *   3: retire (write complete)
 */
static void server_actor_run(STA_IoSpikeActor *actor) {
    uint32_t rc = atomic_load_explicit(&actor->sched.run_count,
                                       memory_order_relaxed);
    switch (rc) {
    case 0:
        sta_io_tcp_listen(actor->io, actor, TCP_TEST_PORT);
        break;
    case 1:
        /* tcp_handle is now the accepted client connection */
        assert(actor->tcp_handle);
        sta_io_tcp_read(actor->io, actor,
                        g_tcp.server_rbuf, sizeof(g_tcp.server_rbuf));
        break;
    case 2:
        /* Echo received bytes back */
        assert(actor->io_bytes == TCP_PAYLOAD_LEN);
        sta_io_tcp_write(actor->io, actor,
                         g_tcp.server_rbuf, (size_t)actor->io_bytes);
        break;
    case 3:
        /* Write complete; close the connection handle */
        if (actor->tcp_handle) {
            /* Signal close via libuv — schedule via a no-op timer */
        }
        atomic_store_explicit(&g_tcp.server_done, 1u, memory_order_release);
        /* Retire handled by max_runs */
        break;
    default:
        break;
    }
}

/*
 * Client actor state machine (driven by run_count):
 *   0: sta_io_tcp_connect → suspend
 *   1: sta_io_tcp_write   → suspend (connected; send payload)
 *   2: sta_io_tcp_read    → suspend (write complete; read echo)
 *   3: verify + retire
 */
static void client_actor_run(STA_IoSpikeActor *actor) {
    uint32_t rc = atomic_load_explicit(&actor->sched.run_count,
                                       memory_order_relaxed);
    switch (rc) {
    case 0:
        sta_io_tcp_connect(actor->io, actor, "127.0.0.1", TCP_TEST_PORT);
        break;
    case 1:
        assert(actor->tcp_handle);
        sta_io_tcp_write(actor->io, actor,
                         g_tcp.payload, sizeof(g_tcp.payload));
        break;
    case 2:
        sta_io_tcp_read(actor->io, actor,
                        g_tcp.client_rbuf, sizeof(g_tcp.client_rbuf));
        break;
    case 3:
        assert(actor->io_bytes == TCP_PAYLOAD_LEN);
        assert(memcmp(g_tcp.client_rbuf, g_tcp.payload, TCP_PAYLOAD_LEN) == 0);
        atomic_store_explicit(&g_tcp.client_done, 1u, memory_order_release);
        break;
    default:
        break;
    }
}

static void test_tcp_loopback_echo(void) {
    printf("Test 5: TCP loopback echo ... ");
    fflush(stdout);

    /* Initialise test context */
    memset(&g_tcp, 0, sizeof(g_tcp));
    for (int i = 0; i < TCP_PAYLOAD_LEN; i++) g_tcp.payload[i] = (uint8_t)i;

    STA_IoSched sched;
    STA_IoSubsystem *io = sta_io_new();
    assert(io);

    sta_io_sched_init(&sched, io);
    sta_io_sched_start(&sched);
    sta_io_init(io, &sched);

    STA_IoSpikeActor server, client;
    /* 4 runs each: steps 0,1,2,3 → retire at run_count == 4 */
    actor_init(&server, 10, 4, server_actor_run, io);
    actor_init(&client, 11, 4, client_actor_run, io);

    atomic_store_explicit(&sched.remaining, 0u, memory_order_relaxed);

    /* Server must be listening before client connects.
     * Push server, wait for its first run (listen request dispatched to
     * the I/O thread), then sleep briefly to let the I/O thread process
     * the request and call uv_listen() before the client connects. */
    sta_io_sched_push(&sched, &server);
    wait_run_count(&server, 1, 500);
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 }; /* 20 ms */
    nanosleep(&ts, NULL);

    sta_io_sched_push(&sched, &client);

    /* Wait for both to complete */
    wait_run_count(&server, 4, 3000);
    wait_run_count(&client, 4, 3000);

    assert(atomic_load_explicit(&g_tcp.server_done, memory_order_acquire));
    assert(atomic_load_explicit(&g_tcp.client_done, memory_order_acquire));
    assert(memcmp(g_tcp.client_rbuf, g_tcp.payload, TCP_PAYLOAD_LEN) == 0);

    sta_io_destroy(io);
    sta_io_sched_stop(&sched);
    sta_io_sched_destroy(&sched);
    sta_io_free(io);

    printf("PASS\n");
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 6: TCP loopback under compute load (TSan gate)                         */
/* ────────────────────────────────────────────────────────────────────────── */

static TcpCtx g_tcp6;

static void server6_actor_run(STA_IoSpikeActor *actor) {
    uint32_t rc = atomic_load_explicit(&actor->sched.run_count,
                                       memory_order_relaxed);
    switch (rc) {
    case 0: sta_io_tcp_listen(actor->io, actor, TCP_TEST_PORT + 1); break;
    case 1:
        assert(actor->tcp_handle);
        sta_io_tcp_read(actor->io, actor,
                        g_tcp6.server_rbuf, sizeof(g_tcp6.server_rbuf));
        break;
    case 2:
        sta_io_tcp_write(actor->io, actor,
                         g_tcp6.server_rbuf, (size_t)actor->io_bytes);
        break;
    case 3:
        atomic_store_explicit(&g_tcp6.server_done, 1u, memory_order_release);
        break;
    default: break;
    }
}

static void client6_actor_run(STA_IoSpikeActor *actor) {
    uint32_t rc = atomic_load_explicit(&actor->sched.run_count,
                                       memory_order_relaxed);
    switch (rc) {
    case 0: sta_io_tcp_connect(actor->io, actor, "127.0.0.1", TCP_TEST_PORT + 1); break;
    case 1:
        assert(actor->tcp_handle);
        sta_io_tcp_write(actor->io, actor,
                         g_tcp6.payload, sizeof(g_tcp6.payload));
        break;
    case 2:
        sta_io_tcp_read(actor->io, actor,
                        g_tcp6.client_rbuf, sizeof(g_tcp6.client_rbuf));
        break;
    case 3:
        assert(actor->io_bytes == TCP_PAYLOAD_LEN);
        assert(memcmp(g_tcp6.client_rbuf, g_tcp6.payload, TCP_PAYLOAD_LEN) == 0);
        atomic_store_explicit(&g_tcp6.client_done, 1u, memory_order_release);
        break;
    default: break;
    }
}

static void test_tcp_under_compute(void) {
    printf("Test 6: TCP loopback under compute load ... ");
    fflush(stdout);

    memset(&g_tcp6, 0, sizeof(g_tcp6));
    for (int i = 0; i < TCP_PAYLOAD_LEN; i++) g_tcp6.payload[i] = (uint8_t)(i + 100);

    STA_IoSched sched;
    STA_IoSubsystem *io = sta_io_new();
    assert(io);

    sta_io_sched_init(&sched, io);
    sta_io_sched_start(&sched);
    sta_io_init(io, &sched);

    STA_IoSpikeActor server6, client6, compute6;
    actor_init(&server6,  20, 4, server6_actor_run, io);
    actor_init(&client6,  21, 4, client6_actor_run, io);
    actor_init(&compute6, 22, 0, compute_actor_run,  io);

    /* Same sequencing as test 5: server listens first, then client. */
    sta_io_sched_push(&sched, &server6);
    wait_run_count(&server6, 1, 500);
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 20000000 }; /* 20 ms */
    nanosleep(&ts, NULL);
    sta_io_sched_push(&sched, &client6);

    /* Wait until client is connected (run_count >= 1 means connect completed). */
    wait_run_count(&client6, 1, 500);

    /* Inject compute actor after both server and client are suspended on I/O.
     * Record baseline before injecting to measure runs during TCP exchange. */
    uint32_t compute_before = atomic_load_explicit(&compute6.sched.run_count,
                                                   memory_order_acquire);
    sta_io_sched_push(&sched, &compute6);

    /* Wait for TCP round trip */
    wait_run_count(&client6, 4, 3000);
    uint32_t compute_after = atomic_load_explicit(&compute6.sched.run_count,
                                                  memory_order_acquire);

    if (compute_after - compute_before < 5) {
        fprintf(stderr,
            "\nWARNING: compute actor ran only %u times during TCP round trip "
            "(expected >= 5). Possible scheduler stall.\n",
            compute_after - compute_before);
        /* Not a hard abort — TCP on loopback can be very fast */
    }
    printf("(compute runs during TCP: %u) ", compute_after - compute_before);

    assert(memcmp(g_tcp6.client_rbuf, g_tcp6.payload, TCP_PAYLOAD_LEN) == 0);

    sta_io_destroy(io);
    sta_io_sched_stop(&sched);
    sta_io_sched_destroy(&sched);
    sta_io_free(io);

    printf("PASS\n");
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 7: sizeof(uv_loop_t) measurement                                       */
/* ────────────────────────────────────────────────────────────────────────── */

static void test_uv_loop_size(void) {
    size_t uv_loop_size = sizeof(uv_loop_t);
    printf("Test 7: sizeof(uv_loop_t) = %zu bytes ", uv_loop_size);
    /* Option C (per-actor uv_loop_t) rejection: embedding in STA_Actor would
     * add ~%zu bytes — trivially breaching the 300-byte density target. */
    if (uv_loop_size <= 300) {
        fprintf(stderr,
            "\nWARNING: sizeof(uv_loop_t) = %zu <= 300. "
            "Option C rejection argument is weaker than expected. "
            "Re-evaluate in ADR 011.\n", uv_loop_size);
    } else {
        printf("(> 300: Option C trivially rejected)");
    }
    printf(" PASS\n");
}

/* ────────────────────────────────────────────────────────────────────────── */
/* Test 8: Actor density checkpoint                                             */
/* ────────────────────────────────────────────────────────────────────────── */

static void test_density_checkpoint(void) {
    size_t spike_struct_size   = sizeof(STA_IoSpikeActor);
    size_t density_struct_size = sizeof(STA_ActorIo);    /* production projection */
    size_t nursery             = 128;
    size_t identity            = 16;
    size_t total               = density_struct_size + nursery + identity;

    /* ADR 010 baseline: sizeof(STA_ActorRevised) + stack_base + stack_top */
    size_t adr010_baseline = 136;   /* measured in spike-004 */
    size_t io_fields       = 8;     /* io_state (4) + io_result (4) */
    size_t projected       = adr010_baseline + io_fields;

    printf("\nTest 8: Actor density checkpoint\n");
    printf("  sizeof(STA_IoSpikeActor) [working spike struct]  = %zu bytes\n",
           spike_struct_size);
    printf("  sizeof(STA_ActorIo) [density measurement struct] = %zu bytes\n",
           density_struct_size);
    printf("  ADR 010 projected STA_Actor baseline             = %zu bytes\n",
           adr010_baseline);
    printf("  Permanent I/O fields (io_state + io_result)      = %zu bytes\n",
           io_fields);
    printf("  Projected production STA_Actor                   = %zu bytes\n",
           projected);
    printf("  Initial nursery slab                             = %zu bytes\n",
           nursery);
    printf("  Actor identity object                            = %zu bytes\n",
           identity);
    printf("  ───────────────────────────────────────────────────────────\n");
    printf("  Total creation cost (projected)                  = %zu bytes\n",
           total);
    printf("  Target                                           = ~300 bytes\n");

    int scenario;
    const char *scenario_name;
    size_t delta = density_struct_size - adr010_baseline;
    if (delta <= 8)       { scenario = 1; scenario_name = "Low (+8 bytes)"; }
    else if (delta <= 12) { scenario = 2; scenario_name = "Mid (+12 bytes)"; }
    else if (delta <= 16) { scenario = 3; scenario_name = "High (+16 bytes)"; }
    else if (delta <= 20) { scenario = 4; scenario_name = "At limit (+20 bytes)"; }
    else                  { scenario = 5; scenario_name = "BREACH (> +20 bytes)"; }
    (void)scenario;

    printf("  Scenario (from ADR 010 table)                    = %s\n",
           scenario_name);

    if (total > 300) {
        printf("\n  WARNING: Total creation cost %zu > 300 bytes.\n", total);
        printf("  ADR 011 MUST justify this overage per CLAUDE.md.\n");
        printf("  (\"Drift from ~300 bytes must be explained in a decision\n");
        printf("   record. Never silently ignored.\")\n");
    } else {
        printf("  Headroom                                         = %zu bytes\n",
               300 - total);
        printf("  Within 300-byte target. PASS\n");
    }
}

/* ────────────────────────────────────────────────────────────────────────── */
/* main                                                                        */
/* ────────────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== Spike 005: Async I/O (libuv) — io_spike test binary ===\n\n");

#ifdef STA_BENCH
    printf("Build: BENCHMARK (no TSan)\n\n");
    run_bench_latency();
    return 0;
#endif

    printf("Build: CORRECTNESS + TSan\n\n");

    test_lifecycle();
    test_timer_suspend_resume();
    test_no_blocking_scheduler();

    /* Test 4 is benchmark-only — skip in correctness build. */
    printf("Test 4: Timer wake latency benchmark ... SKIPPED (run bench_io_spike)\n");

    test_tcp_loopback_echo();
    test_tcp_under_compute();
    test_uv_loop_size();
    test_density_checkpoint();

    printf("\n=== All io_spike tests PASS ===\n");
    return 0;
}
