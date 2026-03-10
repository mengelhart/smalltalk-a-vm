/* tests/test_mailbox_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: MPSC mailbox correctness, overflow, and copy semantics.
 * Also contains latency benchmarks (compiled separately with -DSTA_BENCH=1).
 * See docs/spikes/spike-002-mailbox.md and ADR 008.
 *
 * CHOSEN DESIGN: Variant A (linked list + capacity counter).
 * Variant B (ring buffer) tests retained for reference/comparison only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include "../src/actor/mailbox_spike.h"

/* ── Helpers ──────────────────────────────────────────────────────────────── */

#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL [%s:%d]: %s\n", __FILE__, __LINE__, msg); \
            g_failures++; \
        } else { \
            printf("  OK: %s\n", msg); \
        } \
    } while (0)

static int g_failures = 0;

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* ── Section 1: Variant A — unbounded single-threaded correctness ─────────── */

static void test_list_basic(void) {
    printf("\n=== Variant A (list): single-threaded correctness (unbounded) ===\n");

    STA_MpscList q;
    sta_list_init(&q, 0);   /* 0 = unbounded */

    STA_OOP out;
    CHECK(sta_list_dequeue(&q, &out) == STA_ERR_MAILBOX_EMPTY,
          "list: dequeue empty returns MAILBOX_EMPTY");

    enum { N = 8 };
    STA_ListNode nodes[N];
    for (int i = 0; i < N; i++) {
        CHECK(sta_list_enqueue(&q, &nodes[i], STA_SMALLINT_OOP(i)) == STA_OK,
              "list: enqueue returns OK");
    }
    for (int i = 0; i < N; i++) {
        CHECK(sta_list_dequeue(&q, &out) == STA_OK, "list: dequeue returns OK");
        CHECK(STA_IS_SMALLINT(out), "list: dequeued value is SmallInt");
        CHECK(STA_SMALLINT_VAL(out) == i, "list: dequeued value matches FIFO order");
    }
    CHECK(sta_list_dequeue(&q, &out) == STA_ERR_MAILBOX_EMPTY,
          "list: empty after draining");
}

/* ── Section 2: Variant A — bounded overflow (capacity counter) ──────────── */

#define TEST_LIMIT 16u   /* small limit for fast overflow test */

static void test_list_bounded(void) {
    printf("\n=== Variant A (list): bounded overflow (limit=%u) ===\n", TEST_LIMIT);

    STA_MpscList q;
    sta_list_init(&q, TEST_LIMIT);

    STA_ListNode nodes[TEST_LIMIT];
    STA_OOP out;

    /* Fill to limit — all should succeed. */
    for (uint32_t i = 0; i < TEST_LIMIT; i++) {
        CHECK(sta_list_enqueue(&q, &nodes[i], STA_SMALLINT_OOP((intptr_t)i)) == STA_OK,
              "list bounded: enqueue within limit returns OK");
    }

    /* One more must fail — drop-newest. */
    STA_ListNode extra;
    CHECK(sta_list_enqueue(&q, &extra, STA_SMALLINT_OOP(9999)) == STA_ERR_MAILBOX_FULL,
          "list bounded: enqueue at limit returns MAILBOX_FULL");

    /* Drain — consumer sees exactly TEST_LIMIT messages in order. */
    for (uint32_t i = 0; i < TEST_LIMIT; i++) {
        CHECK(sta_list_dequeue(&q, &out) == STA_OK, "list bounded: dequeue returns OK");
        CHECK(STA_IS_SMALLINT(out), "list bounded: dequeued value is SmallInt");
        CHECK((uint32_t)STA_SMALLINT_VAL(out) == i,
              "list bounded: dequeued value matches FIFO order");
    }
    CHECK(sta_list_dequeue(&q, &out) == STA_ERR_MAILBOX_EMPTY,
          "list bounded: empty after draining");

    /* After draining the counter resets — can fill again. */
    for (uint32_t i = 0; i < TEST_LIMIT; i++) {
        CHECK(sta_list_enqueue(&q, &nodes[i], STA_SMALLINT_OOP(0)) == STA_OK,
              "list bounded: second fill after drain succeeds");
    }
    for (uint32_t i = 0; i < TEST_LIMIT; i++) {
        sta_list_dequeue(&q, &out);  /* drain */
    }
}

/* ── Section 3: Variant B — single-threaded correctness (reference) ─────── */

static void test_ring_basic(void) {
    printf("\n=== Variant B (ring): single-threaded correctness [reference] ===\n");

    STA_MpscRing q;
    sta_ring_init(&q);

    STA_OOP out;
    CHECK(sta_ring_dequeue(&q, &out) == STA_ERR_MAILBOX_EMPTY,
          "ring: dequeue empty returns MAILBOX_EMPTY");

    for (uint32_t i = 0; i < STA_RING_CAPACITY; i++) {
        CHECK(sta_ring_enqueue(&q, STA_SMALLINT_OOP((intptr_t)i)) == STA_OK,
              "ring: enqueue into non-full slot returns OK");
    }
    CHECK(sta_ring_enqueue(&q, STA_SMALLINT_OOP(9999)) == STA_ERR_MAILBOX_FULL,
          "ring: enqueue into full ring returns MAILBOX_FULL");

    for (uint32_t i = 0; i < STA_RING_CAPACITY; i++) {
        CHECK(sta_ring_dequeue(&q, &out) == STA_OK, "ring: dequeue returns OK");
        CHECK(STA_IS_SMALLINT(out), "ring: dequeued value is SmallInt");
        CHECK((uint32_t)STA_SMALLINT_VAL(out) == i,
              "ring: dequeued value matches FIFO order");
    }
    CHECK(sta_ring_dequeue(&q, &out) == STA_ERR_MAILBOX_EMPTY,
          "ring: empty after draining");
}

/* ── Section 4: Multi-threaded stress — Variant A (CHOSEN DESIGN) ─────────── */
/*
 * 4 producer threads × MSGS_PER_PRODUCER messages → 1 consumer.
 * Bounded list (limit=256). Producers spin on MAILBOX_FULL.
 * Consumer verifies: no loss, no duplication, FIFO per-producer.
 * This is the TSan correctness gate for the chosen design.
 */

#define NUM_PRODUCERS     4
#define MSGS_PER_PRODUCER 250000   /* 1M total */
#define STRESS_LIMIT      256u

typedef struct {
    STA_MpscList *q;
    int           producer_id;
} ListProducerArg;

static void *list_producer_fn(void *arg) {
    ListProducerArg *a = arg;
    for (int seq = 0; seq < MSGS_PER_PRODUCER; seq++) {
        STA_ListNode *node = malloc(sizeof(STA_ListNode));
        if (!node) { fprintf(stderr, "malloc failed\n"); return NULL; }
        intptr_t val = ((intptr_t)a->producer_id << 20) | (intptr_t)seq;
        /* Spin on FULL — consumer drains, making room. Node is untouched on
         * FULL so the same allocation is reused on the next attempt.        */
        while (sta_list_enqueue(a->q, node, STA_SMALLINT_OOP(val))
               == STA_ERR_MAILBOX_FULL) {
            sched_yield();
        }
    }
    return NULL;
}

static void test_list_stress(void) {
    printf("\n=== Variant A (list, limit=%u): 4-producer stress — TSan gate ===\n",
           STRESS_LIMIT);

    STA_MpscList q;
    sta_list_init(&q, STRESS_LIMIT);

    pthread_t       threads[NUM_PRODUCERS];
    ListProducerArg args[NUM_PRODUCERS];
    for (int i = 0; i < NUM_PRODUCERS; i++) {
        args[i].q = &q;
        args[i].producer_id = i;
        pthread_create(&threads[i], NULL, list_producer_fn, &args[i]);
    }

    int next_seq[NUM_PRODUCERS];
    memset(next_seq, 0, sizeof(next_seq));
    int total_received = 0;
    int order_ok       = 1;

    while (total_received < NUM_PRODUCERS * MSGS_PER_PRODUCER) {
        STA_OOP out;
        if (sta_list_dequeue(&q, &out) == STA_OK) {
            intptr_t val = STA_SMALLINT_VAL(out);
            int pid = (int)((val >> 20) & 0xF);
            int seq = (int)(val & 0xFFFFF);
            if (pid < 0 || pid >= NUM_PRODUCERS) {
                order_ok = 0;
            } else if (seq != next_seq[pid]) {
                order_ok = 0;
            } else {
                next_seq[pid]++;
            }
            total_received++;
            /* Nodes leak intentionally — TSan checks races, not leaks. */
        }
    }

    for (int i = 0; i < NUM_PRODUCERS; i++) {
        pthread_join(threads[i], NULL);
    }

    CHECK(total_received == NUM_PRODUCERS * MSGS_PER_PRODUCER,
          "list stress: received all 1M messages (no loss)");
    CHECK(order_ok, "list stress: per-producer FIFO order preserved");
}

/* ── Section 5: Copy correctness ─────────────────────────────────────────── */

static STA_ObjHeader *make_object(uint32_t class_idx, uint32_t nslots,
                                   uint8_t obj_flags) {
    size_t sz = 16u + (size_t)nslots * sizeof(STA_OOP);
    STA_ObjHeader *h = malloc(sz);
    memset(h, 0, sz);
    h->class_index = class_idx;
    h->size        = nslots;
    h->gc_flags    = STA_GC_WHITE;
    h->obj_flags   = obj_flags;
    h->reserved    = 0;
    return h;
}

static STA_OOP *obj_slots(STA_ObjHeader *h) {
    return (STA_OOP *)((char *)h + 16u);
}

static void test_copy_correctness(void) {
    printf("\n=== Copy correctness: 5-element Array ===\n");

    STA_ObjHeader *str_obj = make_object(2, 1, 0);
    obj_slots(str_obj)[0] = STA_SMALLINT_OOP(0xDEAD);
    STA_OOP str_oop = (STA_OOP)(uintptr_t)str_obj;

    STA_ObjHeader *sym_obj = make_object(3, 1, STA_OBJ_IMMUTABLE);
    obj_slots(sym_obj)[0] = STA_SMALLINT_OOP(0xBEEF);
    STA_OOP sym_oop = (STA_OOP)(uintptr_t)sym_obj;

    STA_ObjHeader *arr = make_object(1, 5, 0);
    STA_OOP *slots = obj_slots(arr);
    slots[0] = STA_SMALLINT_OOP(42);
    slots[1] = str_oop;
    slots[2] = STA_SMALLINT_OOP(-7);
    slots[3] = sym_oop;
    slots[4] = STA_SMALLINT_OOP(0);
    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr;

    STA_MsgCopy mc;
    int rc = sta_msg_copy_deep(arr_oop, &mc);
    CHECK(rc == STA_OK,        "copy: sta_msg_copy_deep returns OK");
    CHECK(mc.root != 0,        "copy: root OOP is non-null");
    CHECK(mc.root != arr_oop,  "copy: root OOP differs from source (was copied)");

    STA_ObjHeader *dst_arr  = (STA_ObjHeader *)(uintptr_t)mc.root;
    STA_OOP       *dst_slots = obj_slots(dst_arr);

    CHECK(STA_IS_SMALLINT(dst_slots[0]),           "copy slot 0: SmallInt preserved");
    CHECK(STA_SMALLINT_VAL(dst_slots[0]) == 42,    "copy slot 0: value is 42");

    CHECK(!STA_IS_SMALLINT(dst_slots[1]),           "copy slot 1: String is heap OOP");
    CHECK(dst_slots[1] != str_oop,
          "copy slot 1: mutable String was deep-copied (new address)");
    STA_ObjHeader *dst_str = (STA_ObjHeader *)(uintptr_t)dst_slots[1];
    CHECK(obj_slots(dst_str)[0] == obj_slots(str_obj)[0],
          "copy slot 1: String payload preserved");

    CHECK(STA_IS_SMALLINT(dst_slots[2]),            "copy slot 2: SmallInt preserved");
    CHECK(STA_SMALLINT_VAL(dst_slots[2]) == -7,     "copy slot 2: value is -7");

    CHECK(dst_slots[3] == sym_oop,
          "copy slot 3: immutable Symbol shared by pointer (not copied)");

    CHECK(STA_IS_SMALLINT(dst_slots[4]),            "copy slot 4: SmallInt preserved");
    CHECK(STA_SMALLINT_VAL(dst_slots[4]) == 0,      "copy slot 4: value is 0");

    sta_msg_copy_free(&mc);
    CHECK(mc.root == 0, "copy: free zeroes root");

    free(arr);
    free(str_obj);
    free(sym_obj);
}

/* ── Section 6: Latency benchmarks (STA_BENCH only) ─────────────────────── */

#ifdef STA_BENCH

#define BENCH_WARMUP 1000
#define BENCH_ITERS  100000

static void print_percentiles(const char *label, uint64_t *samples, int n) {
    qsort(samples, (size_t)n, sizeof(uint64_t), cmp_u64);
    uint64_t median = samples[n / 2];
    uint64_t p99    = samples[(int)(n * 0.99)];
    printf("  %-48s  median=%4" PRIu64 " ns  p99=%6" PRIu64 " ns\n",
           label, median, p99);
}

/* ---- Bounded list SPSC ---- */
typedef struct { STA_MpscList *q; } SpscListArg;

static void *spsc_list_consumer(void *arg) {
    SpscListArg *a = arg;
    STA_OOP out;
    int n = 0;
    while (n < BENCH_WARMUP + BENCH_ITERS) {
        if (sta_list_dequeue(a->q, &out) == STA_OK) n++;
    }
    return NULL;
}

static void bench_list_spsc(void) {
    STA_MpscList q;
    sta_list_init(&q, STRESS_LIMIT);

    SpscListArg carg = { .q = &q };
    pthread_t ct;
    pthread_create(&ct, NULL, spsc_list_consumer, &carg);

    uint64_t     *samples = malloc(BENCH_ITERS * sizeof(uint64_t));
    STA_ListNode *nodes   = malloc((BENCH_WARMUP + BENCH_ITERS) * sizeof(STA_ListNode));

    for (int i = 0; i < BENCH_WARMUP + BENCH_ITERS; i++) {
        uint64_t t0 = now_ns();
        while (sta_list_enqueue(&q, &nodes[i], STA_SMALLINT_OOP(i))
               == STA_ERR_MAILBOX_FULL)
            sched_yield();
        uint64_t t1 = now_ns();
        if (i >= BENCH_WARMUP) samples[i - BENCH_WARMUP] = t1 - t0;
    }

    pthread_join(ct, NULL);
    print_percentiles("list (bounded) SPSC enqueue latency", samples, BENCH_ITERS);
    free(samples);
    free(nodes);
}

/* ---- Ring SPSC (reference comparison) ---- */
typedef struct { STA_MpscRing *q; } SpscRingArg;

static void *spsc_ring_consumer(void *arg) {
    SpscRingArg *a = arg;
    STA_OOP out;
    int n = 0;
    while (n < BENCH_WARMUP + BENCH_ITERS) {
        if (sta_ring_dequeue(a->q, &out) == STA_OK) n++;
    }
    return NULL;
}

static void bench_ring_spsc(void) {
    STA_MpscRing *q = malloc(sizeof(STA_MpscRing));
    sta_ring_init(q);

    SpscRingArg carg = { .q = q };
    pthread_t ct;
    pthread_create(&ct, NULL, spsc_ring_consumer, &carg);

    uint64_t *samples = malloc(BENCH_ITERS * sizeof(uint64_t));

    for (int i = 0; i < BENCH_WARMUP + BENCH_ITERS; i++) {
        uint64_t t0 = now_ns();
        while (sta_ring_enqueue(q, STA_SMALLINT_OOP(i)) == STA_ERR_MAILBOX_FULL)
            sched_yield();
        uint64_t t1 = now_ns();
        if (i >= BENCH_WARMUP) samples[i - BENCH_WARMUP] = t1 - t0;
    }

    pthread_join(ct, NULL);
    print_percentiles("ring (reference) SPSC enqueue latency", samples, BENCH_ITERS);
    free(samples);
    free(q);
}

/* ---- Bounded list 4P1C ---- */
#define BENCH_4P_ITERS 25000

typedef struct {
    STA_MpscList *q;
    uint64_t     *samples;
    int           iters;
} P4ListArg;

static void *p4_list_producer(void *arg) {
    P4ListArg *a = arg;
    for (int i = 0; i < BENCH_WARMUP + a->iters; i++) {
        STA_ListNode *node = malloc(sizeof(STA_ListNode));
        uint64_t t0 = now_ns();
        while (sta_list_enqueue(a->q, node, STA_SMALLINT_OOP(i))
               == STA_ERR_MAILBOX_FULL)
            sched_yield();
        uint64_t t1 = now_ns();
        if (i >= BENCH_WARMUP) a->samples[i - BENCH_WARMUP] = t1 - t0;
    }
    return NULL;
}

static void bench_list_4p1c(void) {
    STA_MpscList q;
    sta_list_init(&q, STRESS_LIMIT);

    uint64_t  *all_samples = malloc(4 * BENCH_4P_ITERS * sizeof(uint64_t));
    pthread_t  threads[4];
    P4ListArg  args[4];
    for (int i = 0; i < 4; i++) {
        args[i].q       = &q;
        args[i].samples = all_samples + i * BENCH_4P_ITERS;
        args[i].iters   = BENCH_4P_ITERS;
        pthread_create(&threads[i], NULL, p4_list_producer, &args[i]);
    }

    STA_OOP out;
    int consumed = 0;
    int expected = 4 * (BENCH_WARMUP + BENCH_4P_ITERS);
    while (consumed < expected) {
        if (sta_list_dequeue(&q, &out) == STA_OK) consumed++;
    }

    for (int i = 0; i < 4; i++) pthread_join(threads[i], NULL);

    print_percentiles("list (bounded) 4P1C enqueue latency (per-producer)",
                      all_samples, 4 * BENCH_4P_ITERS);
    free(all_samples);
}

/* ---- Copy cost ---- */
static void bench_copy_cost(void) {
    STA_ObjHeader *str_obj = malloc(16u + sizeof(STA_OOP));
    memset(str_obj, 0, 16u + sizeof(STA_OOP));
    str_obj->class_index = 2; str_obj->size = 1; str_obj->gc_flags = STA_GC_WHITE;
    ((STA_OOP *)((char *)str_obj + 16))[0] = STA_SMALLINT_OOP(0xDEAD);

    STA_ObjHeader *sym_obj = malloc(16u + sizeof(STA_OOP));
    memset(sym_obj, 0, 16u + sizeof(STA_OOP));
    sym_obj->class_index = 3; sym_obj->size = 1;
    sym_obj->gc_flags = STA_GC_WHITE; sym_obj->obj_flags = STA_OBJ_IMMUTABLE;
    ((STA_OOP *)((char *)sym_obj + 16))[0] = STA_SMALLINT_OOP(0xBEEF);

    STA_ObjHeader *arr = malloc(16u + 5 * sizeof(STA_OOP));
    memset(arr, 0, 16u + 5 * sizeof(STA_OOP));
    arr->class_index = 1; arr->size = 5; arr->gc_flags = STA_GC_WHITE;
    STA_OOP *slots = (STA_OOP *)((char *)arr + 16);
    slots[0] = STA_SMALLINT_OOP(42);
    slots[1] = (STA_OOP)(uintptr_t)str_obj;
    slots[2] = STA_SMALLINT_OOP(-7);
    slots[3] = (STA_OOP)(uintptr_t)sym_obj;
    slots[4] = STA_SMALLINT_OOP(0);
    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr;

    uint64_t *samples = malloc(BENCH_ITERS * sizeof(uint64_t));
    for (int i = 0; i < BENCH_WARMUP + BENCH_ITERS; i++) {
        STA_MsgCopy mc;
        uint64_t t0 = now_ns();
        sta_msg_copy_deep(arr_oop, &mc);
        uint64_t t1 = now_ns();
        sta_msg_copy_free(&mc);
        if (i >= BENCH_WARMUP) samples[i - BENCH_WARMUP] = t1 - t0;
    }

    print_percentiles("copy: 5-elem Array (1 mutable sub-obj)", samples, BENCH_ITERS);
    free(samples);
    free(arr); free(str_obj); free(sym_obj);
}

static void run_benchmarks(void) {
    printf("\n=== Latency benchmarks (-O2, no TSan) ===\n");
    bench_list_spsc();
    bench_ring_spsc();
    bench_list_4p1c();
    bench_copy_cost();
    printf("\n(These numbers go into ADR 008.)\n");
}

#endif /* STA_BENCH */

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("Spike 002: MPSC mailbox (Variant A + counter) and cross-actor copy\n");

    test_list_basic();
    test_list_bounded();
    test_ring_basic();

    /* Stress tests are the TSan correctness gate.
     * Skipped in the bench build — correctness validated by ctest. */
#ifndef STA_BENCH
    test_list_stress();
#endif

    test_copy_correctness();

#ifdef STA_BENCH
    run_benchmarks();
#endif

    printf("\n=== Spike 002 summary ===\n");
    if (g_failures == 0) {
        printf("All checks passed.\n");
        printf("Next step: run bench_mailbox_spike (-O2, no TSan) and record\n");
        printf("latency numbers in ADR 008.\n");
        return 0;
    } else {
        printf("%d check(s) FAILED.\n", g_failures);
        return 1;
    }
}
