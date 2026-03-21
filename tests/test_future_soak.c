/* tests/test_future_soak.c
 * Phase 2 Epics 7A + 7B: Futures stress tests.
 * Tests 1–3 (Epic 7A): table contention, reply routing, table growth.
 * Test 4 (Epic 7B): sustained ask/wait soak — 100 actors × 10 asks
 *   each = 1,000 round trips where actors suspend on wait and are
 *   woken by the resolve path. Closes issue #330.
 *
 * Usage:
 *   ./build/tests/test_future_soak
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>

#include "scheduler/scheduler.h"
#include "actor/actor.h"
#include "actor/future.h"
#include "actor/future_table.h"
#include "actor/registry.h"
#include "actor/mailbox.h"
#include "actor/mailbox_msg.h"
#include "vm/vm_state.h"
#include "vm/heap.h"
#include "vm/oop.h"
#include "vm/class_table.h"
#include "vm/method_dict.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include "vm/special_objects.h"
#include "vm/interpreter.h"
#include "compiler/compiler.h"
#include <sta/vm.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static STA_VM *make_vm(void) {
    STA_VMConfig cfg = {0};
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);
    return vm;
}

static STA_OOP intern(STA_VM *vm, const char *name) {
    return sta_symbol_intern(&vm->immutable_space,
                              &vm->symbol_table,
                              name, strlen(name));
}

static void install_method(STA_VM *vm, const char *selector_str,
                            const char *source) {
    STA_OOP cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    STA_CompileResult r = sta_compile_method(
        source, cls, NULL, 0,
        &vm->symbol_table, &vm->immutable_space,
        &vm->root_actor->heap,
        vm->specials[SPC_SMALLTALK]);
    if (r.had_error) {
        fprintf(stderr, "compile error: %s\nsource: %s\n", r.error_msg, source);
    }
    assert(!r.had_error);

    STA_OOP md = sta_class_method_dict(cls);
    assert(md != 0);
    STA_OOP selector = intern(vm, selector_str);
    sta_method_dict_insert(&vm->root_actor->heap, md, selector, r.method);
}

static struct STA_Actor *make_service_actor(STA_VM *vm, size_t heap_size) {
    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *a = sta_actor_create(vm, heap_size, 4096);
    assert(a != NULL);
    a->behavior_class = obj_cls;

    STA_ObjHeader *obj_h = sta_heap_alloc(&a->heap, STA_CLS_OBJECT, 0);
    assert(obj_h != NULL);
    a->behavior_obj = (STA_OOP)(uintptr_t)obj_h;

    atomic_store_explicit(&a->state, STA_ACTOR_SUSPENDED,
                          memory_order_relaxed);
    sta_actor_register(a);
    return a;
}

static struct STA_Actor *make_sender_actor(STA_VM *vm) {
    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *a = sta_actor_create(vm, 256, 128);
    assert(a != NULL);
    a->behavior_class = obj_cls;

    STA_ObjHeader *obj_h = sta_heap_alloc(&a->heap, STA_CLS_OBJECT, 0);
    assert(obj_h != NULL);
    a->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    sta_actor_register(a);
    return a;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 1: Future table stress
 * 4 pthreads concurrently create and resolve 10,000 futures total.
 * No actors, no scheduler — pure table contention test.
 * ══════════════════════════════════════════════════════════════════════════ */

#define T1_THREADS       4
#define T1_FUTURES_EACH  2500

typedef struct {
    STA_FutureTable *table;
    uint32_t         sender_id;   /* fake sender_id for future creation */
    int              count;
    _Atomic int     *completed;
    _Atomic int     *error_flag;
} TableStressCtx;

static void *table_stress_thread(void *arg) {
    TableStressCtx *ctx = (TableStressCtx *)arg;

    for (int i = 0; i < ctx->count; i++) {
        /* Create a future. */
        STA_Future *f = sta_future_table_new(ctx->table, ctx->sender_id);
        if (!f) {
            fprintf(stderr, "future_table_new failed at iteration %d\n", i);
            atomic_store(ctx->error_flag, 1);
            return NULL;
        }
        assert(f->future_id > 0);
        assert(atomic_load(&f->state) == STA_FUTURE_PENDING);

        /* Resolve it with a SmallInt value. */
        STA_OOP *buf = malloc(sizeof(STA_OOP));
        assert(buf != NULL);
        buf[0] = STA_SMALLINT_OOP(i);
        bool won = sta_future_resolve(f, buf, 1, NULL);
        assert(won);

        /* Verify resolution. */
        uint32_t state = atomic_load_explicit(&f->state, memory_order_acquire);
        assert(state == STA_FUTURE_RESOLVED);
        assert(f->result_count == 1);
        assert(f->result_buf[0] == STA_SMALLINT_OOP(i));

        /* Remove from table and release. */
        sta_future_table_remove(ctx->table, f->future_id);
        sta_future_release(f);

        atomic_fetch_add(ctx->completed, 1);
    }

    return NULL;
}

static void test_future_table_stress(void) {
    int total = T1_THREADS * T1_FUTURES_EACH;
    printf("  test_future_table_stress (%d threads × %d futures = %d total)...\n",
           T1_THREADS, T1_FUTURES_EACH, total);
    fflush(stdout);

    STA_FutureTable *table = sta_future_table_create(256, NULL);
    assert(table != NULL);

    _Atomic int completed = 0;
    _Atomic int error_flag = 0;

    TableStressCtx ctxs[T1_THREADS];
    pthread_t threads[T1_THREADS];

    for (int i = 0; i < T1_THREADS; i++) {
        ctxs[i] = (TableStressCtx){
            .table = table,
            .sender_id = (uint32_t)(i + 1),
            .count = T1_FUTURES_EACH,
            .completed = &completed,
            .error_flag = &error_flag,
        };
        pthread_create(&threads[i], NULL, table_stress_thread, &ctxs[i]);
    }

    for (int i = 0; i < T1_THREADS; i++)
        pthread_join(threads[i], NULL);

    int done = atomic_load(&completed);
    printf("    completed: %d/%d\n", done, total);

    assert(atomic_load(&error_flag) == 0);
    assert(done == total);
    assert(table->count == 0);

    sta_future_table_destroy(table);
    printf("  PASS\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 2: Reply routing at moderate scale
 * 10 actors, 100 asks each = 1,000 round trips via the scheduler.
 * Proves end-to-end plumbing.  Not a throughput test.
 * ══════════════════════════════════════════════════════════════════════════ */

#define T2_SENDERS       10
#define T2_ASKS_EACH     100

static void test_reply_routing_moderate(void) {
    printf("  test_reply_routing_moderate (%d senders × %d asks)...\n",
           T2_SENDERS, T2_ASKS_EACH);
    fflush(stdout);

    STA_VM *vm = make_vm();
    install_method(vm, "answer", "answer ^42");

    struct STA_Actor *service = make_service_actor(vm, 65536);
    uint32_t service_id = service->actor_id;

    struct STA_Actor *senders[T2_SENDERS];
    for (int i = 0; i < T2_SENDERS; i++)
        senders[i] = make_sender_actor(vm);

    sta_scheduler_init(vm, 0);
    sta_scheduler_start(vm);

    STA_OOP sel = intern(vm, "answer");
    int resolved = 0;

    /* Each sender sends sequentially from the main thread — no extra
     * pthreads needed.  This avoids the scheduler-starvation problem
     * that polling from many threads causes.  Each ask creates one
     * pending future at a time. */
    for (int s = 0; s < T2_SENDERS; s++) {
        for (int a = 0; a < T2_ASKS_EACH; a++) {
            STA_Future *f = NULL;
            int err = 0;

            /* Retry on mailbox full — backpressure. */
            for (int retry = 0; retry < 10000; retry++) {
                f = sta_actor_ask_msg(vm, senders[s]->actor_id,
                                       service_id, sel, NULL, 0, &err);
                if (f != NULL) break;
                if (err == STA_ERR_MAILBOX_FULL) {
                    usleep(100);
                    continue;
                }
                fprintf(stderr, "ask_msg error: %d (sender %d, ask %d)\n",
                        err, s, a);
                assert(0);
            }
            assert(f != NULL);

            /* Poll with 100µs sleep — yields CPU to scheduler threads. */
            uint32_t state;
            for (int t = 0; t < 50000; t++) {
                state = atomic_load_explicit(&f->state, memory_order_acquire);
                if (state == STA_FUTURE_RESOLVED || state == STA_FUTURE_FAILED)
                    break;
                usleep(100);
            }
            state = atomic_load_explicit(&f->state, memory_order_acquire);
            if (state != STA_FUTURE_RESOLVED) {
                fprintf(stderr, "future not resolved: state=%u "
                        "(sender %d, ask %d)\n", state, s, a);
                sta_future_table_remove(vm->future_table, f->future_id);
                sta_future_release(f);
                assert(0);
            }

            assert(f->result_count == 1);
            assert(f->result_buf[0] == STA_SMALLINT_OOP(42));

            sta_future_table_remove(vm->future_table, f->future_id);
            sta_future_release(f);
            resolved++;
        }
    }

    sta_scheduler_stop(vm);

    int expected = T2_SENDERS * T2_ASKS_EACH;
    printf("    resolved: %d/%d\n", resolved, expected);

    assert(resolved == expected);
    assert(vm->future_table->count == 0);

    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
    printf("  PASS\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 3: Future table growth
 * 300 concurrent pending futures exceed the initial 256-slot capacity.
 * Verifies the table grows correctly.  No scheduler needed.
 * ══════════════════════════════════════════════════════════════════════════ */

#define T3_FUTURE_COUNT  300

static void test_future_table_growth(void) {
    printf("  test_future_table_growth (%d concurrent pending futures)...\n",
           T3_FUTURE_COUNT);
    fflush(stdout);

    STA_FutureTable *table = sta_future_table_create(256, NULL);
    assert(table != NULL);

    STA_Future *futures[T3_FUTURE_COUNT];

    /* Create 300 pending futures — triggers growth past 256 capacity. */
    for (int i = 0; i < T3_FUTURE_COUNT; i++) {
        futures[i] = sta_future_table_new(table, 1);
        assert(futures[i] != NULL);
        assert(futures[i]->future_id > 0);
        assert(atomic_load(&futures[i]->state) == STA_FUTURE_PENDING);
    }

    printf("    count=%u capacity=%u (was 256)\n",
           table->count, table->capacity);
    assert(table->count == T3_FUTURE_COUNT);
    assert(table->capacity > 256);

    /* Verify all futures are still findable via lookup. */
    for (int i = 0; i < T3_FUTURE_COUNT; i++) {
        STA_Future *found = sta_future_table_lookup(table, futures[i]->future_id);
        assert(found != NULL);
        assert(found->future_id == futures[i]->future_id);
        sta_future_release(found);  /* release lookup ref */
    }

    /* Resolve all, remove from table, release. */
    for (int i = 0; i < T3_FUTURE_COUNT; i++) {
        STA_OOP *buf = malloc(sizeof(STA_OOP));
        assert(buf != NULL);
        buf[0] = STA_SMALLINT_OOP(i);
        bool won = sta_future_resolve(futures[i], buf, 1, NULL);
        assert(won);

        sta_future_table_remove(table, futures[i]->future_id);
        sta_future_release(futures[i]);
    }

    assert(table->count == 0);

    sta_future_table_destroy(table);
    printf("  PASS\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 4: Sustained ask/wait soak (Epic 7B — closes #330)
 * 100 sender actors × 10 asks each = 1,000 round trips.
 * Each sender executes a Smalltalk method that creates a Future proxy
 * and calls wait — exercising the full STA_PRIM_SUSPEND → scheduler
 * frees thread → resolve → wake_waiter → scheduler reschedules path.
 *
 * Key difference from test 2: test 2 drives asks from the main C thread
 * and polls with usleep.  This test has actors themselves doing the
 * waiting, with real actor suspension and wake under contention.
 * ══════════════════════════════════════════════════════════════════════════ */

#include <time.h>

#define T4_SENDERS       100
#define T4_ASKS_EACH     10
#define T4_TOTAL         (T4_SENDERS * T4_ASKS_EACH)
#define T4_TIMEOUT_SEC   30

static void test_sustained_ask_wait(void) {
    int total = T4_TOTAL;
    printf("  test_sustained_ask_wait (%d senders × %d asks = %d round trips)...\n",
           T4_SENDERS, T4_ASKS_EACH, total);
    fflush(stdout);

    STA_VM *vm = make_vm();

    /* ── Install behavior methods ──────────────────────────────────── */

    /* Service: answer ^42 */
    install_method(vm, "answer", "answer ^42");

    /* Sender: doWaitOn: aFutureId
     *   Creates a Future proxy, sets its futureId ivar, calls wait.
     *   This is the Smalltalk-level code that suspends the actor. */
    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    {
        const char *source =
            "doWaitOn: aFutureId "
            "| f | "
            "f := Future new. "
            "f instVarAt: 1 put: aFutureId. "
            "^f wait";
        STA_CompileResult r = sta_compile_method(
            source, obj_cls, NULL, 0,
            &vm->symbol_table, &vm->immutable_space,
            &vm->root_actor->heap,
            vm->specials[SPC_SMALLTALK]);
        if (r.had_error) {
            fprintf(stderr, "compile error: %s\n", r.error_msg);
        }
        assert(!r.had_error);

        STA_OOP md = sta_class_method_dict(obj_cls);
        STA_OOP sel = intern(vm, "doWaitOn:");
        sta_method_dict_insert(&vm->root_actor->heap, md, sel, r.method);
    }

    /* ── Create actors ────────────────────────────────────────────── */

    struct STA_Actor *service = make_service_actor(vm, 65536);
    uint32_t service_id = service->actor_id;

    struct STA_Actor *senders[T4_SENDERS];
    for (int i = 0; i < T4_SENDERS; i++) {
        senders[i] = make_service_actor(vm, 8192);  /* larger heap for Future proxies */
    }

    /* ── Start scheduler ──────────────────────────────────────────── */

    sta_scheduler_init(vm, 0);  /* 0 = all cores */
    sta_scheduler_start(vm);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* ── Fire all asks + doWaitOn: messages ────────────────────────── */

    STA_Future **futures = calloc(T4_TOTAL, sizeof(STA_Future *));
    assert(futures);
    STA_OOP answer_sel = intern(vm, "answer");
    STA_OOP wait_sel = intern(vm, "doWaitOn:");
    int idx = 0;

    for (int s = 0; s < T4_SENDERS; s++) {
        for (int r = 0; r < T4_ASKS_EACH; r++) {
            /* 1. Create the ask: future (sender→service). */
            int err = 0;
            STA_Future *f = NULL;
            for (int retry = 0; retry < 10000; retry++) {
                f = sta_actor_ask_msg(vm, senders[s]->actor_id,
                                       service_id, answer_sel, NULL, 0, &err);
                if (f) break;
                if (err == STA_ERR_MAILBOX_FULL) { usleep(100); continue; }
                fprintf(stderr, "ask_msg error: %d\n", err);
                assert(0);
            }
            assert(f != NULL);
            futures[idx] = f;

            /* 2. Send doWaitOn: to the sender actor with the future_id.
             *    The sender will create a Future proxy and call wait,
             *    which suspends it until the service resolves the future. */
            STA_OOP fid_arg = STA_SMALLINT_OOP((intptr_t)f->future_id);
            int send_rc = sta_actor_send_msg(vm, vm->root_actor,
                                              senders[s]->actor_id,
                                              wait_sel, &fid_arg, 1);
            assert(send_rc == 0);
            idx++;
        }
    }

    /* ── Wait for all futures to resolve ───────────────────────────── */

    int resolved = 0;
    int failed = 0;
    int timed_out = 0;

    for (int i = 0; i < T4_TOTAL; i++) {
        STA_Future *f = futures[i];
        int done = 0;

        for (int t = 0; t < T4_TIMEOUT_SEC * 1000; t++) {
            uint32_t st = atomic_load_explicit(&f->state, memory_order_acquire);
            if (st == STA_FUTURE_RESOLVED) { resolved++; done = 1; break; }
            if (st == STA_FUTURE_FAILED) { failed++; done = 1; break; }
            usleep(1000);
        }
        if (!done) {
            timed_out++;
            fprintf(stderr, "  TIMEOUT: future %d (id=%u) still pending\n",
                    i, f->future_id);
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed = (double)(t1.tv_sec - t0.tv_sec) +
                     (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;

    printf("    resolved: %d/%d  failed: %d  timed_out: %d\n",
           resolved, total, failed, timed_out);
    printf("    elapsed: %.3fs  throughput: %.0f round-trips/s\n",
           elapsed, resolved / elapsed);

    /* ── Cleanup ──────────────────────────────────────────────────── */

    /* Release our ask_msg refs (table refs were released by wait consumers
     * or remain for unresolved futures). */
    for (int i = 0; i < T4_TOTAL; i++) {
        sta_future_release(futures[i]);
    }
    free(futures);

    sta_scheduler_stop(vm);

    printf("    future_table count after stop: %u\n", vm->future_table->count);

    assert(resolved == total);
    assert(failed == 0);
    assert(timed_out == 0);

    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
    printf("  PASS\n");
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("=== FUTURE SOAK TEST (Epics 7A + 7B) ===\n\n");

    test_future_table_stress();
    test_reply_routing_moderate();
    test_future_table_growth();
    test_sustained_ask_wait();

    printf("\nAll future soak tests passed.\n");
    return 0;
}
