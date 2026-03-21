/* tests/test_reply_routing.c
 * Tests for reply routing — Phase 2 Epic 7A Story 3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>
#include <unistd.h>

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

static struct STA_Actor *make_behavior_actor(STA_OOP cls, uint32_t inst_vars) {
    struct STA_Actor *a = sta_actor_create(g_vm, 16384, 2048);
    assert(a != NULL);
    a->behavior_class = cls;

    uint32_t cls_idx = sta_class_table_index_of(&g_vm->class_table, cls);
    assert(cls_idx != 0);
    STA_ObjHeader *obj_h = sta_heap_alloc(&a->heap, cls_idx, inst_vars);
    assert(obj_h != NULL);
    STA_OOP nil_oop = g_vm->specials[SPC_NIL];
    STA_OOP *slots = sta_payload(obj_h);
    for (uint32_t i = 0; i < inst_vars; i++)
        slots[i] = nil_oop;

    a->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    atomic_store_explicit(&a->state, STA_ACTOR_READY, memory_order_relaxed);
    sta_actor_register(a);
    return a;
}

/* ── Test 1: reply with immediate value (SmallInt) ────────────────── */

static void test_reply_immediate_value(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "answer", "answer ^42", NULL, 0);

    struct STA_Actor *a = make_behavior_actor(obj_cls, 0);  /* sender */
    struct STA_Actor *b = make_behavior_actor(obj_cls, 0);  /* target */

    /* Send ask: from A to B. */
    STA_OOP sel = intern("answer");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b->actor_id,
                                       sel, NULL, 0, &err);
    assert(f != NULL);

    /* Manually dispatch B's message. */
    int rc = sta_actor_process_one_preemptible(g_vm, b);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Verify future resolved with SmallInt 42. */
    uint32_t state = atomic_load_explicit(&f->state, memory_order_acquire);
    assert(state == STA_FUTURE_RESOLVED);
    assert(f->result_count == 1);
    assert(f->result_buf[0] == STA_SMALLINT_OOP(42));
    assert(f->transfer_heap == NULL);

    sta_future_table_remove(g_vm->future_table, f->future_id);
    sta_future_release(f);
    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 2: reply with immutable value (Symbol) ──────────────────── */

static void test_reply_immutable_value(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "name", "name ^#hello", NULL, 0);

    struct STA_Actor *a = make_behavior_actor(obj_cls, 0);
    struct STA_Actor *b = make_behavior_actor(obj_cls, 0);

    STA_OOP sel = intern("name");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b->actor_id,
                                       sel, NULL, 0, &err);
    assert(f != NULL);

    int rc = sta_actor_process_one_preemptible(g_vm, b);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    uint32_t state = atomic_load_explicit(&f->state, memory_order_acquire);
    assert(state == STA_FUTURE_RESOLVED);
    assert(f->result_count == 1);

    /* The result should be the Symbol #hello — an immutable-space object. */
    STA_OOP result = f->result_buf[0];
    assert(STA_IS_HEAP(result));
    STA_ObjHeader *rh = (STA_ObjHeader *)(uintptr_t)result;
    assert(rh->obj_flags & STA_OBJ_IMMUTABLE);
    assert(f->transfer_heap == NULL);

    sta_future_table_remove(g_vm->future_table, f->future_id);
    sta_future_release(f);
    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 3: reply with mutable value (Array) ─────────────────────── */

static void test_reply_mutable_value(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    /* Method that creates and returns a mutable Array. */
    install_method(obj_cls, "makeArray",
                   "makeArray ^(Array new: 3)", NULL, 0);

    struct STA_Actor *a = make_behavior_actor(obj_cls, 0);
    struct STA_Actor *b = make_behavior_actor(obj_cls, 0);

    STA_OOP sel = intern("makeArray");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b->actor_id,
                                       sel, NULL, 0, &err);
    assert(f != NULL);

    int rc = sta_actor_process_one_preemptible(g_vm, b);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    uint32_t state = atomic_load_explicit(&f->state, memory_order_acquire);
    assert(state == STA_FUTURE_RESOLVED);
    assert(f->result_count == 1);

    STA_OOP result = f->result_buf[0];
    assert(STA_IS_HEAP(result));
    assert(f->transfer_heap != NULL);

    /* The result OOP should live in the transfer heap, not B's heap. */
    STA_ObjHeader *rh = (STA_ObjHeader *)(uintptr_t)result;
    char *tbase = f->transfer_heap->base;
    size_t tused = f->transfer_heap->used;
    assert((char *)rh >= tbase && (char *)rh < tbase + tused);
    assert(rh->size == 3);  /* Array new: 3 */

    sta_future_table_remove(g_vm->future_table, f->future_id);
    sta_future_release(f);
    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 4: fire-and-forget — no routing, no crash ───────────────── */

static void test_reply_fire_and_forget_no_routing(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    /* Reuse the "answer" method installed in test 1. */

    struct STA_Actor *a = make_behavior_actor(obj_cls, 0);
    struct STA_Actor *b = make_behavior_actor(obj_cls, 0);

    /* Regular fire-and-forget send. */
    STA_OOP sel = intern("answer");
    int rc = sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0);
    assert(rc == 0);

    /* Dispatch — future_id == 0, no reply routing should happen. */
    rc = sta_actor_process_one_preemptible(g_vm, b);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Future table should be unaffected. */
    assert(g_vm->future_table->count == 0);

    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 5: future already failed — resolve is a no-op ──────────── */

static void test_reply_future_already_failed(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    struct STA_Actor *a = make_behavior_actor(obj_cls, 0);
    struct STA_Actor *b = make_behavior_actor(obj_cls, 0);

    STA_OOP sel = intern("answer");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b->actor_id,
                                       sel, NULL, 0, &err);
    assert(f != NULL);

    /* Simulate supervisor failing the future before dispatch. */
    STA_OOP *fail_buf = malloc(sizeof(STA_OOP));
    fail_buf[0] = STA_SMALLINT_OOP(99);
    bool failed = sta_future_fail(f, fail_buf, 1);
    assert(failed);

    /* Now dispatch — B returns 42, but resolve should lose the CAS. */
    int rc = sta_actor_process_one_preemptible(g_vm, b);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Future should remain FAILED with the supervisor's value. */
    uint32_t state = atomic_load_explicit(&f->state, memory_order_acquire);
    assert(state == STA_FUTURE_FAILED);
    assert(f->result_buf[0] == STA_SMALLINT_OOP(99));

    sta_future_table_remove(g_vm->future_table, f->future_id);
    sta_future_release(f);
    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 6: full round trip without scheduler ────────────────────── */

static void test_reply_round_trip_manual(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    struct STA_Actor *a = make_behavior_actor(obj_cls, 0);
    struct STA_Actor *b = make_behavior_actor(obj_cls, 0);

    /* A asks B "answer" — should return 42. */
    STA_OOP sel = intern("answer");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b->actor_id,
                                       sel, NULL, 0, &err);
    assert(f != NULL);
    assert(atomic_load(&f->state) == STA_FUTURE_PENDING);

    /* Manually dequeue + dispatch B's message. */
    int rc = sta_actor_process_one_preemptible(g_vm, b);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Verify A's future is resolved with 42. */
    assert(atomic_load_explicit(&f->state, memory_order_acquire)
           == STA_FUTURE_RESOLVED);
    assert(f->result_buf[0] == STA_SMALLINT_OOP(42));

    sta_future_table_remove(g_vm->future_table, f->future_id);
    sta_future_release(f);
    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 7: full round trip with scheduler ───────────────────────── */

static void test_reply_round_trip_scheduled(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    struct STA_Actor *b = make_behavior_actor(obj_cls, 0);
    /* Set B to SUSPENDED so the scheduler can pick it up. */
    atomic_store_explicit(&b->state, STA_ACTOR_SUSPENDED, memory_order_relaxed);

    /* Start the scheduler. */
    sta_scheduler_init(g_vm, 0);
    sta_scheduler_start(g_vm);

    /* A asks B. The ask path will auto-schedule B. */
    STA_OOP sel = intern("answer");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, g_vm->root_actor->actor_id,
                                       b->actor_id, sel, NULL, 0, &err);
    assert(f != NULL);

    /* Busy-wait for the future to resolve (timeout 2 seconds). */
    for (int i = 0; i < 2000; i++) {
        uint32_t state = atomic_load_explicit(&f->state, memory_order_acquire);
        if (state == STA_FUTURE_RESOLVED || state == STA_FUTURE_FAILED)
            break;
        usleep(1000);  /* 1ms */
    }

    sta_scheduler_stop(g_vm);
    sta_scheduler_destroy(g_vm);

    uint32_t state = atomic_load_explicit(&f->state, memory_order_acquire);
    assert(state == STA_FUTURE_RESOLVED);
    assert(f->result_buf[0] == STA_SMALLINT_OOP(42));

    sta_future_table_remove(g_vm->future_table, f->future_id);
    sta_future_release(f);
    sta_actor_terminate(b);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_reply_routing:\n");

    setup();
    RUN(test_reply_immediate_value);
    RUN(test_reply_immutable_value);
    RUN(test_reply_mutable_value);
    RUN(test_reply_fire_and_forget_no_routing);
    RUN(test_reply_future_already_failed);
    RUN(test_reply_round_trip_manual);
    RUN(test_reply_round_trip_scheduled);
    teardown();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
