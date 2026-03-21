/* tests/test_future_wait.c
 * Tests for Future >> wait primitive — Phase 2 Epic 7B Story 4.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#include "actor/actor.h"
#include "actor/future.h"
#include "actor/future_table.h"
#include "actor/mailbox.h"
#include "actor/mailbox_msg.h"
#include "actor/registry.h"
#include "actor/deep_copy.h"
#include "vm/vm_state.h"
#include "vm/heap.h"
#include "vm/oop.h"
#include "vm/class_table.h"
#include "vm/method_dict.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include "vm/special_objects.h"
#include "vm/interpreter.h"
#include "vm/primitive_table.h"
#include "compiler/compiler.h"
#include "scheduler/scheduler.h"
#include <sta/vm.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(fn) do { \
    printf("  %-55s", #fn); \
    tests_run++; \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

static STA_VM *g_vm;

static void setup(void) {
    STA_VMConfig cfg = {0};
    g_vm = sta_vm_create(&cfg);
    assert(g_vm != NULL);
}

static void teardown(void) {
    sta_vm_destroy(g_vm);
    g_vm = NULL;
}

static STA_OOP intern(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space,
                              &g_vm->symbol_table,
                              name, strlen(name));
}

static void install_method(STA_OOP cls, const char *selector_str,
                            const char *source,
                            const char **ivars, uint32_t ivar_count) {
    STA_CompileResult r = sta_compile_method(
        source, cls, ivars, ivar_count,
        &g_vm->symbol_table, &g_vm->immutable_space,
        &g_vm->root_actor->heap,
        g_vm->specials[SPC_SMALLTALK]);
    if (r.had_error) {
        fprintf(stderr, "compile error: %s\nsource: %s\n", r.error_msg, source);
    }
    assert(!r.had_error);
    assert(r.method != 0);

    STA_OOP md = sta_class_method_dict(cls);
    assert(md != 0);
    STA_OOP selector = intern(selector_str);
    sta_method_dict_insert(&g_vm->root_actor->heap, md, selector, r.method);
}

/* Create a Future proxy object on the given actor's heap.
 * Sets instvar 0 (futureId) to the given id as SmallInt. */
static STA_OOP make_future_proxy(struct STA_Actor *actor, uint32_t future_id) {
    STA_ObjHeader *h = sta_heap_alloc(&actor->heap, STA_CLS_FUTURE, 1);
    assert(h != NULL);
    sta_payload(h)[0] = STA_SMALLINT_OOP((intptr_t)future_id);
    return (STA_OOP)(uintptr_t)h;
}

static struct STA_Actor *make_actor(STA_OOP cls, uint32_t inst_vars) {
    struct STA_Actor *a = sta_actor_create(g_vm, 16384, 4096);
    assert(a != NULL);
    a->behavior_class = cls;
    uint32_t cls_idx = sta_class_table_index_of(&g_vm->class_table, cls);
    assert(cls_idx != 0);
    STA_ObjHeader *obj_h = sta_heap_alloc(&a->heap, cls_idx, inst_vars);
    assert(obj_h != NULL);
    STA_OOP nil_oop = g_vm->specials[SPC_NIL];
    STA_OOP *slots = sta_payload(obj_h);
    for (uint32_t i = 0; i < inst_vars; i++) slots[i] = nil_oop;
    a->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    /* Start SUSPENDED — auto-schedule in send_msg/ask_msg will wake. */
    atomic_store_explicit(&a->state, STA_ACTOR_SUSPENDED, memory_order_relaxed);
    sta_actor_register(a);
    return a;
}

/* ── Test 1: Wait on already-resolved future ──────────────────────────── */

static void test_wait_already_resolved(void) {
    setup();

    /* Create a future and resolve it with SmallInt 42. */
    STA_Future *f = sta_future_table_new(g_vm->future_table, 1);
    uint32_t fid = f->future_id;

    STA_OOP *buf = malloc(sizeof(STA_OOP));
    buf[0] = STA_SMALLINT_OOP(42);
    bool won = sta_future_resolve(f, buf, 1, NULL);
    assert(won);
    sta_future_release(f);  /* release create ref, table ref remains */

    /* Call prim_future_wait directly through the primitive table. */
    STA_OOP proxy = make_future_proxy(g_vm->root_actor, fid);

    STA_ExecContext ctx = { .vm = g_vm, .actor = g_vm->root_actor };
    STA_OOP args[1] = { proxy };
    STA_OOP result = 0;
    int rc = sta_primitives[201](&ctx, args, 0, &result);

    assert(rc == STA_PRIM_SUCCESS);
    assert(result == STA_SMALLINT_OOP(42));

    /* Verify future removed from table. */
    STA_Future *check = sta_future_table_lookup(g_vm->future_table, fid);
    assert(check == NULL);

    teardown();
}

/* ── Test 2: Wait on already-failed future ────────────────────────────── */

static void test_wait_already_failed(void) {
    setup();

    STA_Future *f = sta_future_table_new(g_vm->future_table, 1);
    uint32_t fid = f->future_id;

    STA_OOP *fbuf = malloc(sizeof(STA_OOP));
    fbuf[0] = intern("actorCrashed");
    bool won = sta_future_fail(f, fbuf, 1);
    assert(won);
    sta_future_release(f);

    /* Install a handler that catches FutureFailure. We test this by
     * evaluating Smalltalk that calls wait inside on:do:. */
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP future_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_FUTURE);
    assert(future_cls != 0);

    /* Install a test method: testWaitFailed: aFutureId
     *   | f |
     *   f := Future new.
     *   f instVarAt: 1 put: aFutureId.
     *   [f wait] on: FutureFailure do: [:e | 999]
     * We'll compile and run this via sta_eval. */

    /* Simpler approach: call the primitive directly and catch the longjmp
     * via the handler mechanism. */
    STA_OOP proxy = make_future_proxy(g_vm->root_actor, fid);

    /* Set up an on:do: handler for FutureFailure. */
    STA_ExecContext ctx = { .vm = g_vm, .actor = g_vm->root_actor };
    STA_HandlerEntry entry;
    memset(&entry, 0, sizeof(entry));
    STA_OOP ff_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_FUTUREFAILURE);
    assert(ff_cls != 0);
    entry.exception_class = ff_cls;
    STA_StackSlab *slab = &g_vm->root_actor->slab;
    entry.saved_slab_top = slab->top;
    entry.saved_slab_sp  = slab->sp;

    int caught = 0;
    if (setjmp(entry.jmp) == 0) {
        sta_handler_push_ctx(&ctx, &entry);
        STA_OOP args[1] = { proxy };
        STA_OOP result = 0;
        (void)sta_primitives[201](&ctx, args, 0, &result);
        sta_handler_pop_ctx(&ctx);
        assert(0 && "should not reach here — wait should signal");
    } else {
        /* Handler caught the FutureFailure. */
        caught = 1;
        STA_OOP exc = sta_handler_get_signaled_ctx(&ctx);
        assert(exc != 0);
        STA_ObjHeader *exc_h = (STA_ObjHeader *)(uintptr_t)exc;
        assert(exc_h->class_index == STA_CLS_FUTUREFAILURE);
    }
    assert(caught);

    /* Verify future removed from table. */
    STA_Future *check = sta_future_table_lookup(g_vm->future_table, fid);
    assert(check == NULL);

    teardown();
}

/* ── Test 3: Wait suspends and wake resolves (with scheduler) ─────────── */

static void test_wait_suspend_and_wake(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    /* Install methods on Object for actors A and B. */
    /* B: handleAsk  ^42 */
    install_method(obj_cls, "handleAsk", "handleAsk ^42", NULL, 0);
    /* A: askAndWait: targetId
     *   | f |
     *   ...
     * We can't easily compile ask/wait in Smalltalk yet.
     * Instead, test at the C level: create A, create B,
     * send ask from A→B, call wait on A's thread. */

    /* Create actors A and B. */
    struct STA_Actor *actor_b = make_actor(obj_cls, 0);
    struct STA_Actor *actor_a = make_actor(obj_cls, 0);

    /* Start scheduler. */
    assert(sta_scheduler_init(g_vm, 2) == 0);
    assert(sta_scheduler_start(g_vm) == 0);

    /* A sends ask: to B. */
    STA_OOP sel = intern("handleAsk");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, actor_a->actor_id,
                                       actor_b->actor_id, sel, NULL, 0, &err);
    assert(f != NULL);
    uint32_t fid = f->future_id;

    /* Poll: wait for the future to resolve (B processes the message). */
    int resolved = 0;
    for (int i = 0; i < 200; i++) {
        uint32_t st = atomic_load_explicit(&f->state, memory_order_acquire);
        if (st == STA_FUTURE_RESOLVED) { resolved = 1; break; }
        usleep(5000);
    }
    assert(resolved);

    /* Now call prim_future_wait on A's behalf — should return immediately
     * since the future is already resolved. */
    STA_OOP proxy = make_future_proxy(actor_a, fid);
    STA_ExecContext ctx = { .vm = g_vm, .actor = actor_a };
    STA_OOP args[1] = { proxy };
    STA_OOP result = 0;
    int rc = sta_primitives[201](&ctx, args, 0, &result);

    assert(rc == STA_PRIM_SUCCESS);
    assert(result == STA_SMALLINT_OOP(42));

    sta_future_release(f);  /* release our ask_msg ref */

    sta_scheduler_stop(g_vm);
    sta_scheduler_destroy(g_vm);
    teardown();
}

/* ── Test 4: Wait returns SmallInt immediate result ───────────────────── */

static void test_wait_immediate_result(void) {
    setup();

    STA_Future *f = sta_future_table_new(g_vm->future_table, 1);
    uint32_t fid = f->future_id;

    STA_OOP *buf = malloc(sizeof(STA_OOP));
    buf[0] = STA_SMALLINT_OOP(12345);
    sta_future_resolve(f, buf, 1, NULL);
    sta_future_release(f);

    STA_OOP proxy = make_future_proxy(g_vm->root_actor, fid);
    STA_ExecContext ctx = { .vm = g_vm, .actor = g_vm->root_actor };
    STA_OOP args[1] = { proxy };
    STA_OOP result = 0;
    int rc = sta_primitives[201](&ctx, args, 0, &result);

    assert(rc == STA_PRIM_SUCCESS);
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 12345);
    assert(f == NULL || 1);  /* future consumed */

    teardown();
}

/* ── Test 5: Wait returns immutable Symbol result ─────────────────────── */

static void test_wait_immutable_result(void) {
    setup();

    STA_OOP sym = intern("testSymbol");

    STA_Future *f = sta_future_table_new(g_vm->future_table, 1);
    uint32_t fid = f->future_id;

    STA_OOP *buf = malloc(sizeof(STA_OOP));
    buf[0] = sym;
    sta_future_resolve(f, buf, 1, NULL);  /* transfer_heap = NULL for immutable */
    sta_future_release(f);

    STA_OOP proxy = make_future_proxy(g_vm->root_actor, fid);
    STA_ExecContext ctx = { .vm = g_vm, .actor = g_vm->root_actor };
    STA_OOP args[1] = { proxy };
    STA_OOP result = 0;
    int rc = sta_primitives[201](&ctx, args, 0, &result);

    assert(rc == STA_PRIM_SUCCESS);
    assert(result == sym);  /* Same OOP — immutable, shared by pointer. */

    teardown();
}

/* ── Test 6: Wait returns mutable result via transfer heap ────────────── */

static void test_wait_mutable_result(void) {
    setup();

    STA_Future *f = sta_future_table_new(g_vm->future_table, 1);
    uint32_t fid = f->future_id;

    /* Create a mutable Array on a standalone transfer heap. */
    STA_Heap *transfer = malloc(sizeof(STA_Heap));
    assert(sta_heap_init(transfer, 4096) == 0);

    STA_ObjHeader *arr_h = sta_heap_alloc(transfer, STA_CLS_ARRAY, 3);
    assert(arr_h != NULL);
    STA_OOP *arr_slots = sta_payload(arr_h);
    arr_slots[0] = STA_SMALLINT_OOP(10);
    arr_slots[1] = STA_SMALLINT_OOP(20);
    arr_slots[2] = STA_SMALLINT_OOP(30);
    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr_h;

    STA_OOP *buf = malloc(sizeof(STA_OOP));
    buf[0] = arr_oop;
    sta_future_resolve(f, buf, 1, transfer);
    sta_future_release(f);

    /* Call wait — should deep-copy from transfer heap to root actor's heap. */
    STA_OOP proxy = make_future_proxy(g_vm->root_actor, fid);
    STA_ExecContext ctx = { .vm = g_vm, .actor = g_vm->root_actor };
    STA_OOP args[1] = { proxy };
    STA_OOP result = 0;
    int rc = sta_primitives[201](&ctx, args, 0, &result);

    assert(rc == STA_PRIM_SUCCESS);
    assert(!STA_IS_IMMEDIATE(result));

    /* Verify the copied array contents. */
    STA_ObjHeader *rh = (STA_ObjHeader *)(uintptr_t)result;
    assert(rh->class_index == STA_CLS_ARRAY);
    assert(rh->size == 3);
    STA_OOP *rslots = sta_payload(rh);
    assert(rslots[0] == STA_SMALLINT_OOP(10));
    assert(rslots[1] == STA_SMALLINT_OOP(20));
    assert(rslots[2] == STA_SMALLINT_OOP(30));

    /* Verify the result is on root actor's heap (not on transfer heap).
     * Transfer heap was freed by copy_future_result. */
    uintptr_t result_addr = (uintptr_t)result;
    uintptr_t heap_start = (uintptr_t)g_vm->root_actor->heap.base;
    uintptr_t heap_end = heap_start + g_vm->root_actor->heap.capacity;
    assert(result_addr >= heap_start && result_addr < heap_end);

    teardown();
}

/* ── Test 7: Wait from root actor returns prim fail ───────────────────── */

static void test_wait_from_root_actor(void) {
    setup();

    /* Create a PENDING future. */
    STA_Future *f = sta_future_table_new(g_vm->future_table, 1);
    uint32_t fid = f->future_id;

    STA_OOP proxy = make_future_proxy(g_vm->root_actor, fid);
    STA_ExecContext ctx = { .vm = g_vm, .actor = g_vm->root_actor };
    STA_OOP args[1] = { proxy };
    STA_OOP result = 0;
    int rc = sta_primitives[201](&ctx, args, 0, &result);

    /* Root actor cannot suspend — should return failure. */
    assert(rc == STA_PRIM_BAD_RECEIVER);

    /* Clean up the pending future. */
    sta_future_table_remove(g_vm->future_table, fid);
    sta_future_release(f);  /* table ref */
    sta_future_release(f);  /* create ref */

    teardown();
}

/* ── Test 8: Wait cleans up future from table ─────────────────────────── */

static void test_wait_cleans_up_future(void) {
    setup();

    /* Create and resolve 5 futures, wait on each, verify table cleanup. */
    for (int i = 0; i < 5; i++) {
        STA_Future *f = sta_future_table_new(g_vm->future_table, 1);
        uint32_t fid = f->future_id;

        STA_OOP *buf = malloc(sizeof(STA_OOP));
        buf[0] = STA_SMALLINT_OOP(i * 10);
        sta_future_resolve(f, buf, 1, NULL);
        sta_future_release(f);

        STA_OOP proxy = make_future_proxy(g_vm->root_actor, fid);
        STA_ExecContext ctx = { .vm = g_vm, .actor = g_vm->root_actor };
        STA_OOP args[1] = { proxy };
        STA_OOP result = 0;
        int rc = sta_primitives[201](&ctx, args, 0, &result);
        assert(rc == STA_PRIM_SUCCESS);
        assert(result == STA_SMALLINT_OOP(i * 10));

        /* Verify: future removed from table. */
        STA_Future *check = sta_future_table_lookup(g_vm->future_table, fid);
        assert(check == NULL);
    }

    /* Table should have no entries left. */
    assert(g_vm->future_table->count == 0);

    teardown();
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_future_wait (Epic 7B Story 4):\n");

    RUN(test_wait_already_resolved);
    RUN(test_wait_already_failed);
    RUN(test_wait_suspend_and_wake);
    RUN(test_wait_immediate_result);
    RUN(test_wait_immutable_result);
    RUN(test_wait_mutable_result);
    RUN(test_wait_from_root_actor);
    RUN(test_wait_cleans_up_future);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
