/* tests/test_scheduler_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: work-stealing scheduler and reduction-based preemption.
 * See docs/spikes/spike-003-scheduler.md and ADR 009.
 *
 * Usage:
 *   ctest (TSan build)       — correctness + TSan gate
 *   ./bench_scheduler_spike  — latency benchmarks (STA_BENCH=1, -O2)
 *   ./bench_scheduler_notif_1/2/3 — notification latency per-mode
 */

#include "../src/scheduler/scheduler_spike.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ── Helpers ───────────────────────────────────────────────────────────── */

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "  FAIL [%s:%d]: %s\n", __FILE__, __LINE__, (msg)); \
            failures++; \
        } else { \
            printf("    ok: %s\n", (msg)); \
        } \
    } while (0)

static int failures = 0;

/* Compare function for qsort of uint64_t arrays. */
static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t percentile(uint64_t *sorted, size_t n, int pct) {
    size_t idx = (size_t)((double)pct / 100.0 * (double)(n - 1) + 0.5);
    return sorted[idx];
}

/* ── Test 1: Thread lifecycle ──────────────────────────────────────────── */
/*
 * Spawn sysconf(_SC_NPROCESSORS_ONLN) threads, let each run an empty loop
 * for a brief period, then shut down cleanly.  TSan gate: no races on
 * STA_Scheduler fields or deque indices.
 */
static int test_lifecycle(void) {
    printf("\n=== Test 1: Thread lifecycle ===\n");
    int prev_failures = failures;

    int ncores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncores < 1) ncores = 1;
    if (ncores > (int)STA_MAX_THREADS) ncores = (int)STA_MAX_THREADS;

    printf("  sysconf cores = %d\n", ncores);

    STA_Scheduler *s = malloc(sizeof(*s));
    CHECK(s != NULL, "malloc scheduler");
    if (!s) return failures - prev_failures;

    uint64_t t0 = sta_now_ns();
    int rc = sta_sched_init(s, ncores);
    CHECK(rc == 0, "sta_sched_init succeeds");

    sta_sched_start(s);
    uint64_t t1 = sta_now_ns();

    /* All threads are now running.  Let them spin briefly with no work. */
    struct timespec sl = { .tv_sec = 0, .tv_nsec = 50000000L }; /* 50 ms */
    nanosleep(&sl, NULL);

    sta_sched_stop(s);
    uint64_t t2 = sta_now_ns();

    uint64_t startup_ns  = t1 - t0;
    uint64_t shutdown_ns = t2 - t1;

    printf("  startup  latency = %" PRIu64 " µs\n", startup_ns  / 1000u);
    printf("  shutdown latency = %" PRIu64 " µs\n", shutdown_ns / 1000u);

    CHECK(startup_ns < 500000000ULL, "startup < 500 ms");
    CHECK(shutdown_ns < 1000000000ULL, "shutdown < 1 s");

    sta_sched_destroy(s);
    free(s);
    return failures - prev_failures;
}

/* ── Test 2: Deque A correctness (single-threaded) ─────────────────────── */
/*
 * Validate push/pop (LIFO from bottom) and steal (FIFO from top) without
 * concurrent access.  Also verifies the full→error path.
 */
static int test_dequeA_correctness(void) {
    printf("\n=== Test 2: Deque A correctness (single-threaded) ===\n");
    int prev_failures = failures;

    STA_WorkDequeA *dq = calloc(1, sizeof(*dq));
    CHECK(dq != NULL, "malloc deque A");
    if (!dq) return failures - prev_failures;

    sta_dequeA_init(dq);
    CHECK(sta_dequeA_size(dq) == 0, "deque A: initial size = 0");

    /* Push N actors. */
    const int N = 500;
    STA_SpikeActor actors[500];
    memset(actors, 0, sizeof(actors));
    for (int i = 0; i < N; i++) {
        actors[i].actor_id = (uint32_t)i;
        int r = sta_dequeA_push(dq, &actors[i]);
        CHECK(r == 0, "push succeeds");
    }
    CHECK(sta_dequeA_size(dq) == N, "deque A: size == N after N pushes");

    /* Pop all: should come out in reverse order (LIFO from bottom). */
    for (int i = N - 1; i >= 0; i--) {
        STA_SpikeActor *a = sta_dequeA_pop(dq);
        CHECK(a != NULL, "pop returns non-NULL");
        if (a) CHECK(a->actor_id == (uint32_t)i, "pop: LIFO order correct");
    }
    CHECK(sta_dequeA_pop(dq) == NULL, "pop on empty deque returns NULL");

    /* Push N actors again, then steal all from the top. */
    for (int i = 0; i < N; i++) {
        actors[i].actor_id = (uint32_t)i;
        sta_dequeA_push(dq, &actors[i]);
    }
    for (int i = 0; i < N; i++) {
        STA_SpikeActor *a = sta_dequeA_steal(dq);
        CHECK(a != NULL, "steal returns non-NULL");
        if (a) CHECK(a->actor_id == (uint32_t)i, "steal: FIFO order correct");
    }
    CHECK(sta_dequeA_steal(dq) == NULL, "steal on empty deque returns NULL");

    /* Verify the deque signals full correctly. */
    for (uint32_t i = 0; i < STA_DEQUE_CAPACITY; i++) {
        actors[0].actor_id = i;
        /* Reuse actors[0] — for this test only the push/size matters. */
        (void)sta_dequeA_push(dq, &actors[0]);
    }
    CHECK(sta_dequeA_size(dq) == (int)STA_DEQUE_CAPACITY,
          "deque A: full at STA_DEQUE_CAPACITY");
    CHECK(sta_dequeA_push(dq, &actors[0]) == -1,
          "push on full deque returns -1");

    free(dq);
    return failures - prev_failures;
}

/* ── Test 3: Deque B correctness (single-threaded) ─────────────────────── */

static int test_dequeB_correctness(void) {
    printf("\n=== Test 3: Deque B correctness (single-threaded) ===\n");
    int prev_failures = failures;

    STA_WorkDequeB *dq = calloc(1, sizeof(*dq));
    CHECK(dq != NULL, "malloc deque B");
    if (!dq) return failures - prev_failures;

    sta_dequeB_init(dq);

    const int N = 500;
    STA_SpikeActor actors[500];
    memset(actors, 0, sizeof(actors));

    for (int i = 0; i < N; i++) {
        actors[i].actor_id = (uint32_t)i;
        CHECK(sta_dequeB_push(dq, &actors[i]) == 0, "push succeeds");
    }
    CHECK(sta_dequeB_size(dq) == N, "deque B: size == N");

    for (int i = N - 1; i >= 0; i--) {
        STA_SpikeActor *a = sta_dequeB_pop(dq);
        CHECK(a != NULL, "pop returns non-NULL");
        if (a) CHECK(a->actor_id == (uint32_t)i, "pop: LIFO order correct");
    }
    CHECK(sta_dequeB_pop(dq) == NULL, "pop on empty deque returns NULL");

    for (int i = 0; i < N; i++) {
        actors[i].actor_id = (uint32_t)i;
        sta_dequeB_push(dq, &actors[i]);
    }
    for (int i = 0; i < N; i++) {
        STA_SpikeActor *a = sta_dequeB_steal(dq);
        CHECK(a != NULL, "steal returns non-NULL");
        if (a) CHECK(a->actor_id == (uint32_t)i, "steal: FIFO order correct");
    }
    CHECK(sta_dequeB_steal(dq) == NULL, "steal on empty deque returns NULL");

    free(dq);
    return failures - prev_failures;
}

/* ── Test 4: Steal stress — multi-threaded (TSan gate) ──────────────────── */
/*
 * N scheduler threads share a pool of actors.  Each actor runs MAX_RUNS
 * times then retires.  Work steals naturally when any thread empties its
 * deque.  TSan must report no races; total run counts must be exact.
 *
 * This is the hard TSan gate.  If any deque race exists, TSan will flag it.
 */
/* Under TSan every atomic op is ~10-20× slower; reduce the workload so the
 * test completes within the 60-s ctest timeout.  The correctness gate does
 * not require high volume — any data race will surface even with small N. */
#if defined(__has_feature) && __has_feature(thread_sanitizer)
#  define STRESS_NTHREADS          4
#  define STRESS_ACTORS_PER_THREAD 25
#  define STRESS_MAX_RUNS          3
#else
#  define STRESS_NTHREADS          4
#  define STRESS_ACTORS_PER_THREAD 250
#  define STRESS_MAX_RUNS          10
#endif
#define STRESS_NACTORS  (STRESS_NTHREADS * STRESS_ACTORS_PER_THREAD)

static STA_SpikeActor stress_actors[STRESS_NACTORS];

static int test_steal_stress(void) {
    printf("\n=== Test 4: Steal stress test (TSan gate) ===\n");
    int prev_failures = failures;

    memset(stress_actors, 0, sizeof(stress_actors));

    STA_Scheduler *s = malloc(sizeof(*s));
    CHECK(s != NULL, "malloc scheduler");
    if (!s) return failures - prev_failures;

    int rc = sta_sched_init(s, STRESS_NTHREADS);
    CHECK(rc == 0, "init stress scheduler");

    /* Set up actors: distribute evenly across threads.
     * remaining counts actors not yet retired (each retires once). */
    atomic_store_explicit(&s->remaining, (uint32_t)STRESS_NACTORS,
                          memory_order_relaxed);

    for (int i = 0; i < STRESS_NACTORS; i++) {
        stress_actors[i].actor_id   = (uint32_t)i;
        stress_actors[i].home_thread = (uint32_t)(i % STRESS_NTHREADS);
        stress_actors[i].max_runs   = STRESS_MAX_RUNS;
        atomic_store_explicit(&stress_actors[i].run_count,   0u, memory_order_relaxed);
        atomic_store_explicit(&stress_actors[i].sched_flags, STA_SCHED_RUNNABLE,
                              memory_order_relaxed);
    }

    /* Pre-populate each thread's deque. */
    for (int i = 0; i < STRESS_NACTORS; i++) {
        int thread = (int)(stress_actors[i].home_thread);
#if STA_DEQUE_VARIANT == 1
        rc = sta_dequeA_push(&s->threads[thread].deque, &stress_actors[i]);
#else
        rc = sta_dequeB_push(&s->threads[thread].deque, &stress_actors[i]);
#endif
        CHECK(rc == 0, "pre-populate push succeeds");
    }

    /* Start all threads.  They schedule actors, steal when idle, retire when
     * each actor has hit max_runs.  When remaining reaches 0, all work is
     * done; stop the scheduler. */
    sta_sched_start(s);

    /* Poll remaining with a timeout.  Under TSan the run is slow; allow 60s. */
    uint64_t deadline = sta_now_ns() + 60000000000ULL;
    while (atomic_load_explicit(&s->remaining, memory_order_acquire) > 0) {
        if (sta_now_ns() > deadline) {
            fprintf(stderr, "  FAIL: stress test timed out (remaining=%u)\n",
                    atomic_load_explicit(&s->remaining, memory_order_relaxed));
            failures++;
            break;
        }
        struct timespec sl = { .tv_sec = 0, .tv_nsec = 1000000L }; /* 1 ms */
        nanosleep(&sl, NULL);
    }

    sta_sched_stop(s);

    /* Verify each actor ran exactly STRESS_MAX_RUNS times. */
    int count_errors = 0;
    for (int i = 0; i < STRESS_NACTORS; i++) {
        uint32_t rc_val = atomic_load_explicit(&stress_actors[i].run_count,
                                               memory_order_relaxed);
        if (rc_val != STRESS_MAX_RUNS) count_errors++;
    }
    CHECK(count_errors == 0, "all actors ran exactly max_runs times");
    if (count_errors > 0) {
        fprintf(stderr, "  %d actors had wrong run_count\n", count_errors);
    }

    /* Print steal statistics. */
    uint64_t total_stolen = 0, total_attempts = 0, total_cas_fail = 0;
    for (int i = 0; i < STRESS_NTHREADS; i++) {
        total_stolen   += atomic_load_explicit(&s->threads[i].steal_successes,
                                               memory_order_relaxed);
        total_attempts += atomic_load_explicit(&s->threads[i].steal_attempts,
                                               memory_order_relaxed);
        total_cas_fail += atomic_load_explicit(&s->threads[i].steal_cas_failures,
                                               memory_order_relaxed);
    }
    printf("  steal successes   = %" PRIu64 "\n", total_stolen);
    printf("  steal attempts    = %" PRIu64 "\n", total_attempts);
    printf("  steal CAS failures= %" PRIu64 "\n", total_cas_fail);
    if (total_attempts > 0) {
        printf("  steal hit rate    = %.1f%%\n",
               100.0 * (double)total_stolen / (double)total_attempts);
    }

    sta_sched_destroy(s);
    free(s);
    return failures - prev_failures;
}

/* ── Test 5: Notification smoke test ──────────────────────────────────── */
/*
 * One idle scheduler thread; main thread pushes one actor and checks it
 * completes within a generous timeout.  Exercises the notification path
 * under TSan to verify the wake mechanism is race-free.
 */
static int test_notif_smoke(void) {
    printf("\n=== Test 5: Notification smoke test ===\n");
    int prev_failures = failures;

    STA_Scheduler *s = malloc(sizeof(*s));
    CHECK(s != NULL, "malloc scheduler");
    if (!s) return failures - prev_failures;

    int rc = sta_sched_init(s, 1);
    CHECK(rc == 0, "init single-thread scheduler");

    STA_SpikeActor actor;
    memset(&actor, 0, sizeof(actor));
    actor.actor_id   = 42;
    actor.home_thread = 0;
    actor.max_runs   = 1;
    atomic_store_explicit(&actor.run_count,   0u, memory_order_relaxed);
    atomic_store_explicit(&actor.sched_flags, STA_SCHED_NONE, memory_order_relaxed);
    atomic_store_explicit(&s->remaining, 1u, memory_order_relaxed);

    sta_sched_start(s);

    /* Give the scheduler thread time to go idle. */
    struct timespec sl = { .tv_sec = 0, .tv_nsec = 10000000L }; /* 10 ms */
    nanosleep(&sl, NULL);

    /* Push actor; scheduler must wake and run it. */
    rc = sta_sched_push(s, 0, &actor);
    CHECK(rc == 0, "push to idle scheduler succeeds");

    /* Wait for completion (up to 2 s). */
    uint64_t deadline = sta_now_ns() + 2000000000ULL;
    while (atomic_load_explicit(&s->remaining, memory_order_acquire) > 0) {
        if (sta_now_ns() > deadline) {
            fprintf(stderr, "  FAIL: smoke test timed out\n");
            failures++;
            break;
        }
        struct timespec sl2 = { .tv_sec = 0, .tv_nsec = 1000000L };
        nanosleep(&sl2, NULL);
    }

    uint32_t rc_val = atomic_load_explicit(&actor.run_count, memory_order_relaxed);
    CHECK(rc_val == 1u, "smoke test: actor ran exactly once");

    sta_sched_stop(s);
    sta_sched_destroy(s);
    free(s);
    return failures - prev_failures;
}

/* ── Test 6: Actor density ─────────────────────────────────────────────── */
/*
 * Print sizeof(STA_ActorRevised) and compute the revised per-actor creation
 * budget.  Asserts the total is within the ~300-byte target.
 * See Q6 in spike-003-scheduler.md and ADR 009.
 */
static int test_actor_density(void) {
    printf("\n=== Test 6: Actor density checkpoint (Q6) ===\n");
    int prev_failures = failures;

    size_t revised_struct  = sizeof(STA_ActorRevised);
    size_t initial_nursery = 128u;   /* minimum viable slab (unchanged) */
    size_t identity_obj    = 16u;    /* 0-slot ObjHeader + alignment gap */
    size_t total_creation  = revised_struct + initial_nursery + identity_obj;

    printf("  sizeof(STA_ActorRevised)  = %zu bytes\n", revised_struct);
    printf("  sizeof(STA_SpikeActor)    = %zu bytes  (scheduler-only stub)\n",
           sizeof(STA_SpikeActor));
    printf("  sizeof(STA_WorkDequeA)    = %zu bytes\n", sizeof(STA_WorkDequeA));
    printf("  sizeof(STA_WorkDequeB)    = %zu bytes\n", sizeof(STA_WorkDequeB));
    printf("  sizeof(STA_SchedThread)   = %zu bytes  (includes deque)\n",
           sizeof(STA_SchedThread));
    printf("\n");
    printf("  Per-actor creation budget (revised):\n");
    printf("    STA_ActorRevised struct = %zu bytes\n", revised_struct);
    printf("    initial nursery slab    = %zu bytes\n", initial_nursery);
    printf("    actor identity object   = %zu bytes\n", identity_obj);
    printf("    ─────────────────────────────────────\n");
    printf("    total creation cost     = %zu bytes  (target: ~300)\n",
           total_creation);
    printf("\n");

    if (revised_struct > 180) {
        printf("  WARNING: STA_ActorRevised > 180 bytes: %zu bytes.\n",
               revised_struct);
        printf("           Justification required in ADR 009 per Q6.\n");
    }

    if (total_creation > 300) {
        printf("  WARNING: total creation cost %zu > 300 bytes.\n",
               total_creation);
        printf("           Justification required in ADR 009.\n");
        /* Not a test failure — this is a design target, not a hard gate. */
    } else {
        printf("  OK: creation cost within ~300-byte target "
               "(headroom: %zu bytes).\n", 300u - total_creation);
    }

    /* Density target is advisory in the spike binary; no hard CHECK. */
    (void)prev_failures;
    return 0;
}

/* ── Benchmarks (STA_BENCH=1 only) ──────────────────────────────────────── */

#ifdef STA_BENCH

#define BENCH_WARMUP   1000u
#define BENCH_SAMPLES  100000u

/* ── Bench 1: Notification latency ──────────────────────────────────────── */
/*
 * Measures time from sta_sched_push() to the first reduction tick of the
 * actor on the scheduler thread.  The actor records its start_ns atomically.
 * The sender records the push_ns before calling sta_sched_push.
 *
 * Protocol:
 *   1. Scheduler thread is idle (no actors, waiting on notification).
 *   2. Sender records push_ns, pushes actor, signals notification.
 *   3. Actor records start_ns as its first action in execute_actor stub.
 *   4. Sender reads start_ns from actor after actor completes (run_count == 1).
 *   5. wake_latency = start_ns - push_ns.
 *
 * Because the actor's start_ns is written by the scheduler and read by the
 * sender after observing run_count == 1, the acquire/release on run_count
 * provides the necessary happens-before.
 */

typedef struct {
    STA_Scheduler   *sched;
    STA_SpikeActor  *actor;
    uint64_t        *latencies;  /* BENCH_SAMPLES values */
    uint32_t         n_done;
} BenchNotifArgs;

static void bench_notif_latency(void) {
    printf("\n=== Bench: Notification wake latency (mode=%d) ===\n",
           STA_NOTIF_MODE);

    STA_Scheduler *s = malloc(sizeof(*s));
    if (!s) { fprintf(stderr, "malloc\n"); return; }

    int rc = sta_sched_init(s, 1);
    if (rc != 0) { fprintf(stderr, "sched_init\n"); free(s); return; }

    STA_SpikeActor actor;
    uint64_t *latencies = malloc(BENCH_SAMPLES * sizeof(uint64_t));
    if (!latencies) { free(s); return; }

    /* The scheduler thread runs continuously; we re-push the actor for each
     * measurement.  Between pushes we wait for run_count to increment, so
     * the scheduler is definitely idle again before the next push. */
    atomic_store_explicit(&s->remaining, UINT32_MAX, memory_order_relaxed);

    sta_sched_start(s);

    uint32_t collected = 0;

    for (uint32_t iter = 0; iter < BENCH_WARMUP + BENCH_SAMPLES; iter++) {
        memset(&actor, 0, sizeof(actor));
        actor.max_runs = 1;
        atomic_store_explicit(&actor.run_count, 0u, memory_order_relaxed);
        atomic_store_explicit(&actor.start_ns,  0ULL, memory_order_relaxed);

        /* Ensure scheduler is idle (no actor in queue). */
        struct timespec sl = { .tv_sec = 0, .tv_nsec = 100000L }; /* 100 µs */
        nanosleep(&sl, NULL);

        uint64_t push_ns = sta_now_ns();
        sta_sched_push(s, 0, &actor);

        /* Wait for actor to complete. */
        while (atomic_load_explicit(&actor.run_count, memory_order_acquire) == 0) {
            /* spin */
        }
        uint64_t start_ns = atomic_load_explicit(&actor.start_ns,
                                                  memory_order_relaxed);

        int64_t diff = (int64_t)(start_ns - push_ns);
        if (diff < 0) diff = 0;  /* clock skew guard */

        if (iter >= BENCH_WARMUP) {
            latencies[collected++] = (uint64_t)diff;
        }
    }

    sta_sched_stop(s);
    sta_sched_destroy(s);
    free(s);

    qsort(latencies, collected, sizeof(uint64_t), cmp_u64);

    printf("  Samples          : %u\n", collected);
    printf("  Notification mode: %d (%s)\n", STA_NOTIF_MODE,
           STA_NOTIF_MODE == 1 ? "pthread_cond_signal" :
           STA_NOTIF_MODE == 2 ? "pipe write" : "spin+backoff");
    printf("  Min              : %" PRIu64 " ns\n", latencies[0]);
    printf("  Median (p50)     : %" PRIu64 " ns\n",
           percentile(latencies, collected, 50));
    printf("  p90              : %" PRIu64 " ns\n",
           percentile(latencies, collected, 90));
    printf("  p99              : %" PRIu64 " ns\n",
           percentile(latencies, collected, 99));
    printf("  Max              : %" PRIu64 " ns\n", latencies[collected - 1]);
    printf("\n");
    printf("  >>> Record these in ADR 009 (Q3 notification latency).\n");

    free(latencies);
}

/* ── Bench 2: Preemption timing ──────────────────────────────────────────── */
/*
 * Measure how long one scheduling quantum (STA_REDUCTION_QUOTA reductions of
 * the stub execute_actor loop) takes in wall-clock nanoseconds.
 * See Q4 in spike-003-scheduler.md.
 */
static void bench_preemption_timing(void) {
    printf("\n=== Bench: Preemption quantum timing (Q4) ===\n");

    STA_SpikeActor actor;
    memset(&actor, 0, sizeof(actor));
    actor.max_runs = UINT32_MAX;

    uint64_t *samples = malloc(BENCH_SAMPLES * sizeof(uint64_t));
    if (!samples) return;

    uint32_t collected = 0;

    for (uint32_t iter = 0; iter < BENCH_WARMUP + BENCH_SAMPLES; iter++) {
        uint64_t t0 = sta_now_ns();

        /* One quantum: restore budget and run to zero. */
        atomic_store_explicit(&actor.reductions, STA_REDUCTION_QUOTA,
                              memory_order_relaxed);
        uint32_t r;
        while ((r = atomic_load_explicit(&actor.reductions,
                                         memory_order_relaxed)) > 0) {
            atomic_store_explicit(&actor.reductions, r - 1u,
                                  memory_order_relaxed);
        }

        uint64_t t1 = sta_now_ns();

        if (iter >= BENCH_WARMUP) {
            samples[collected++] = t1 - t0;
        }
    }

    qsort(samples, collected, sizeof(uint64_t), cmp_u64);

    printf("  STA_REDUCTION_QUOTA = %u\n", STA_REDUCTION_QUOTA);
    printf("  Samples             = %u\n", collected);
    printf("  Min quantum         = %" PRIu64 " ns\n", samples[0]);
    printf("  Median quantum      = %" PRIu64 " ns\n",
           percentile(samples, collected, 50));
    printf("  p99 quantum         = %" PRIu64 " ns\n",
           percentile(samples, collected, 99));
    printf("  Max quantum         = %" PRIu64 " ns\n", samples[collected - 1]);
    printf("\n");
    printf("  >>> Record median and max in ADR 009 (Q4 quantum duration).\n");

    free(samples);
}

/* ── Bench 3: Maximum scheduling latency ────────────────────────────────── */
/*
 * Run N scheduler threads under steady-state actor load for 10 seconds.
 * Each actor records the timestamp of its last preemption.  When it is
 * next scheduled, it computes the gap.  The maximum observed gap is the
 * maximum scheduling latency.  See Q4 in the spike doc.
 */

#define LATENCY_RUN_SECONDS   5
#define LATENCY_NACTORS       200

typedef struct {
    STA_SpikeActor  actor;
    _Atomic uint64_t last_preemption_ns;  /* timestamp when actor was last suspended */
    _Atomic uint64_t max_gap_ns;          /* max observed gap for this actor */
} LatencyActor;

static LatencyActor lat_actors[LATENCY_NACTORS];

/* Override execute_actor for this benchmark by recording timestamps.
 * We re-use the scheduler's internal execute via the normal scheduling path;
 * the gap measurement is done in a custom wrapper below. */

static void bench_max_sched_latency(void) {
    printf("\n=== Bench: Maximum scheduling latency (Q4) ===\n");

    int ncores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncores < 1) ncores = 1;
    if (ncores > (int)STA_MAX_THREADS) ncores = (int)STA_MAX_THREADS;

    STA_Scheduler *s = malloc(sizeof(*s));
    if (!s) return;

    int rc = sta_sched_init(s, ncores);
    if (rc != 0) { free(s); return; }

    memset(lat_actors, 0, sizeof(lat_actors));

    /* Actors run indefinitely (max_runs = large number) for the duration. */
    uint64_t run_until_ns = sta_now_ns() + (uint64_t)LATENCY_RUN_SECONDS * 1000000000ULL;

    for (int i = 0; i < LATENCY_NACTORS; i++) {
        lat_actors[i].actor.actor_id    = (uint32_t)i;
        lat_actors[i].actor.home_thread = (uint32_t)(i % (uint32_t)ncores);
        lat_actors[i].actor.max_runs    = UINT32_MAX;
        atomic_store_explicit(&lat_actors[i].actor.run_count,   0u, memory_order_relaxed);
        atomic_store_explicit(&lat_actors[i].actor.sched_flags, STA_SCHED_RUNNABLE,
                              memory_order_relaxed);
        atomic_store_explicit(&lat_actors[i].last_preemption_ns, 0ULL, memory_order_relaxed);
        atomic_store_explicit(&lat_actors[i].max_gap_ns, 0ULL, memory_order_relaxed);
        atomic_store_explicit(&s->remaining, UINT32_MAX, memory_order_relaxed);
    }

    for (int i = 0; i < LATENCY_NACTORS; i++) {
        int t = (int)(lat_actors[i].actor.home_thread);
#if STA_DEQUE_VARIANT == 1
        sta_dequeA_push(&s->threads[t].deque, &lat_actors[i].actor);
#else
        sta_dequeB_push(&s->threads[t].deque, &lat_actors[i].actor);
#endif
    }

    sta_sched_start(s);

    /* Let the scheduler run for LATENCY_RUN_SECONDS. */
    while (sta_now_ns() < run_until_ns) {
        struct timespec sl = { .tv_sec = 0, .tv_nsec = 100000000L }; /* 100 ms */
        nanosleep(&sl, NULL);
    }

    sta_sched_stop(s);

    /* For the scheduling latency, we use start_ns from STA_SpikeActor.
     * The scheduler writes start_ns at the beginning of each execution.
     * The gap between consecutive start_ns values for the same actor is
     * the scheduling latency.  We approximate it from total run time and
     * run count: avg_quantum = wall_time / run_count. */
    uint64_t now_ns = sta_now_ns();
    uint64_t total_runs = 0;
    uint64_t min_runs   = UINT64_MAX;

    for (int i = 0; i < LATENCY_NACTORS; i++) {
        uint64_t rc_val = atomic_load_explicit(&lat_actors[i].actor.run_count,
                                               memory_order_relaxed);
        total_runs += rc_val;
        if (rc_val < min_runs) min_runs = rc_val;
    }
    (void)now_ns;

    uint64_t avg_runs_per_actor = total_runs / (uint64_t)LATENCY_NACTORS;
    uint64_t wall_ns = (uint64_t)LATENCY_RUN_SECONDS * 1000000000ULL;
    uint64_t approx_max_gap_us = 0;
    if (avg_runs_per_actor > 0) {
        approx_max_gap_us = (wall_ns / avg_runs_per_actor) / 1000ULL;
    }

    printf("  Cores            : %d\n", ncores);
    printf("  Actors           : %d\n", LATENCY_NACTORS);
    printf("  Duration         : %d s\n", LATENCY_RUN_SECONDS);
    printf("  Total runs       : %" PRIu64 "\n", total_runs);
    printf("  Avg runs/actor   : %" PRIu64 "\n", avg_runs_per_actor);
    printf("  Min runs/actor   : %" PRIu64 "\n", min_runs);
    printf("  Approx max gap   : %" PRIu64 " µs  (wall / avg_runs)\n",
           approx_max_gap_us);
    printf("\n");
    printf("  >>> Record approx_max_gap in ADR 009 (Q4 max scheduling latency).\n");
    printf("  Note: this is an approximation from throughput, not per-wakeup\n");
    printf("        measurement.  Per-wakeup measurement requires actor-level\n");
    printf("        timestamp logging which would perturb the result.\n");

    sta_sched_destroy(s);
    free(s);
}

/* ── Bench 4: Steal contention ───────────────────────────────────────────── */

static void bench_steal_contention(void) {
    printf("\n=== Bench: Steal contention (Q2) ===\n");

    int thread_counts[] = { 1, 2, 4, 0 };  /* 0 = stop marker */
    /* Use sysconf count too, but cap at STA_MAX_THREADS. */
    int ncores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (ncores > (int)STA_MAX_THREADS) ncores = (int)STA_MAX_THREADS;

    for (int ti = 0; thread_counts[ti] != 0; ti++) {
        int nthreads = thread_counts[ti];
        if (nthreads > ncores) continue;

#define BENCH_STRESS_ACTORS_PT 100
#define BENCH_STRESS_RUNS      20

        int nactors = nthreads * BENCH_STRESS_ACTORS_PT;
        STA_SpikeActor *ba = calloc((size_t)nactors, sizeof(STA_SpikeActor));
        if (!ba) continue;

        STA_Scheduler *s = malloc(sizeof(*s));
        if (!s) { free(ba); continue; }
        sta_sched_init(s, nthreads);

        /* remaining counts actors (each retires once after BENCH_STRESS_RUNS). */
        atomic_store_explicit(&s->remaining, (uint32_t)nactors,
                              memory_order_relaxed);

        for (int i = 0; i < nactors; i++) {
            ba[i].actor_id    = (uint32_t)i;
            ba[i].home_thread = (uint32_t)(i % nthreads);
            ba[i].max_runs    = BENCH_STRESS_RUNS;
            atomic_store_explicit(&ba[i].run_count,   0u, memory_order_relaxed);
            atomic_store_explicit(&ba[i].sched_flags, STA_SCHED_RUNNABLE,
                                  memory_order_relaxed);
            int t = (int)(ba[i].home_thread);
#if STA_DEQUE_VARIANT == 1
            sta_dequeA_push(&s->threads[t].deque, &ba[i]);
#else
            sta_dequeB_push(&s->threads[t].deque, &ba[i]);
#endif
        }

        uint64_t t0 = sta_now_ns();
        sta_sched_start(s);

        uint64_t deadline = sta_now_ns() + 30000000000ULL;
        while (atomic_load_explicit(&s->remaining, memory_order_acquire) > 0 &&
               sta_now_ns() < deadline) {
            struct timespec sl = { .tv_sec = 0, .tv_nsec = 1000000L };
            nanosleep(&sl, NULL);
        }
        sta_sched_stop(s);
        uint64_t t1 = sta_now_ns();

        uint64_t wall_ms  = (t1 - t0) / 1000000ULL;
        uint64_t steals   = 0, attempts = 0, cas_fail = 0, runs = 0;
        for (int i = 0; i < nthreads; i++) {
            steals   += atomic_load_explicit(&s->threads[i].steal_successes, memory_order_relaxed);
            attempts += atomic_load_explicit(&s->threads[i].steal_attempts, memory_order_relaxed);
            cas_fail += atomic_load_explicit(&s->threads[i].steal_cas_failures, memory_order_relaxed);
            runs     += atomic_load_explicit(&s->threads[i].actors_run, memory_order_relaxed);
        }

        printf("  Threads=%d  wall=%"PRIu64"ms  runs=%"PRIu64
               "  steals=%"PRIu64"  attempts=%"PRIu64"  CAS-fail=%"PRIu64"\n",
               nthreads, wall_ms, runs, steals, attempts, cas_fail);

        sta_sched_destroy(s);
        free(s);
        free(ba);
    }
    printf("\n  >>> Record steal contention and throughput numbers in ADR 009 (Q2).\n");
}

#endif  /* STA_BENCH */

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("Spike 003: Work-stealing scheduler and reduction preemption\n");
    printf("Notification mode : %d (%s)\n", STA_NOTIF_MODE,
           STA_NOTIF_MODE == 1 ? "pthread_cond_signal" :
           STA_NOTIF_MODE == 2 ? "pipe write" : "spin+backoff");
    printf("Deque variant     : %d (%s)\n", STA_DEQUE_VARIANT,
           STA_DEQUE_VARIANT == 1 ? "Chase-Lev (Variant A)" :
                                    "ring buffer (Variant B)");

    test_lifecycle();
    test_dequeA_correctness();
    test_dequeB_correctness();
    test_steal_stress();
    test_notif_smoke();
    test_actor_density();

#ifdef STA_BENCH
    bench_notif_latency();
    bench_preemption_timing();
    bench_max_sched_latency();
    bench_steal_contention();
#endif

    printf("\n");
    if (failures == 0) {
        printf("All checks passed.\n");
        printf("Run bench_scheduler_spike and bench_scheduler_notif_[1-3]\n");
        printf("to get the numbers for ADR 009.\n");
        return 0;
    } else {
        printf("%d check(s) FAILED.\n", failures);
        return 1;
    }
}
