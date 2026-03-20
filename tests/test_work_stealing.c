/* tests/test_work_stealing.c
 * Phase 2 Epic 4 Story 5: Work stealing with Chase-Lev deques.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#include "scheduler/scheduler.h"
#include "scheduler/deque.h"
#include "actor/actor.h"
#include "vm/vm_state.h"
#include "vm/interpreter.h"
#include "vm/special_objects.h"
#include "vm/symbol_table.h"
#include "vm/method_dict.h"
#include "compiler/compiler.h"
#include <sta/vm.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static STA_VM *make_vm(void) {
    STA_VMConfig cfg = { .image_path = NULL };
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);
    return vm;
}

static struct STA_Actor *make_child_actor(STA_VM *vm) {
    struct STA_Actor *child = sta_actor_create(vm, 65536, 8192);
    assert(child != NULL);
    child->vm = vm;
    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    STA_ObjHeader *obj_h = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(obj_h != NULL);
    child->behavior_class = obj_cls;
    child->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    return child;
}

static STA_OOP intern(STA_VM *vm, const char *name) {
    return sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, name, strlen(name));
}

static bool wait_for_suspended(struct STA_Actor *actor, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        if (atomic_load(&actor->state) == STA_ACTOR_SUSPENDED) return true;
        usleep(1000);
    }
    return false;
}

/* ── Test 1: deque unit test (push/pop/steal) ────────────────────────── */

static void test_deque_basic(void) {
    STA_VM *vm = make_vm();
    STA_WorkDeque dq;
    sta_deque_init(&dq);

    struct STA_Actor *a1 = sta_actor_create(vm, 128, 128);
    struct STA_Actor *a2 = sta_actor_create(vm, 128, 128);
    struct STA_Actor *a3 = sta_actor_create(vm, 128, 128);
    a1->actor_id = 1; a2->actor_id = 2; a3->actor_id = 3;

    /* Push 3. */
    assert(sta_deque_push(&dq, a1) == 0);
    assert(sta_deque_push(&dq, a2) == 0);
    assert(sta_deque_push(&dq, a3) == 0);
    assert(sta_deque_size(&dq) == 3);

    /* Pop (LIFO): should get a3 first. */
    struct STA_Actor *p = sta_deque_pop(&dq);
    assert(p == a3);

    /* Steal (FIFO): should get a1. */
    struct STA_Actor *s = sta_deque_steal(&dq);
    assert(s == a1);

    /* Remaining: a2. */
    p = sta_deque_pop(&dq);
    assert(p == a2);

    /* Empty. */
    assert(sta_deque_pop(&dq) == NULL);
    assert(sta_deque_steal(&dq) == NULL);

    sta_actor_destroy(a1);
    sta_actor_destroy(a2);
    sta_actor_destroy(a3);
    sta_vm_destroy(vm);
    printf("  PASS: deque_basic\n");
}

/* ── Test 2: deque capacity ──────────────────────────────────────────── */

static void test_deque_capacity(void) {
    STA_VM *vm = make_vm();
    STA_WorkDeque dq;
    sta_deque_init(&dq);

    /* Fill to capacity. */
    struct STA_Actor *actors[STA_DEQUE_CAPACITY];
    for (uint32_t i = 0; i < STA_DEQUE_CAPACITY; i++) {
        actors[i] = sta_actor_create(vm, 128, 128);
        assert(sta_deque_push(&dq, actors[i]) == 0);
    }
    assert(sta_deque_size(&dq) == (int)STA_DEQUE_CAPACITY);

    /* Overflow. */
    struct STA_Actor *extra = sta_actor_create(vm, 128, 128);
    assert(sta_deque_push(&dq, extra) == -1);

    /* Drain. */
    for (uint32_t i = 0; i < STA_DEQUE_CAPACITY; i++) {
        assert(sta_deque_pop(&dq) != NULL);
    }
    assert(sta_deque_pop(&dq) == NULL);

    for (uint32_t i = 0; i < STA_DEQUE_CAPACITY; i++) {
        sta_actor_destroy(actors[i]);
    }
    sta_actor_destroy(extra);
    sta_vm_destroy(vm);
    printf("  PASS: deque_capacity\n");
}

/* ── Test 3: work stealing with imbalanced placement ─────────────────── */

static void test_imbalanced_stealing(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Create 6 actors, all enqueued at once (will go to overflow queue,
     * first worker to drain gets them all). */
    struct STA_Actor *actors[6];
    STA_OOP sel = intern(vm, "yourself");
    struct STA_Actor *root = vm->root_actor;

    for (int i = 0; i < 6; i++) {
        actors[i] = make_child_actor(vm);
        actors[i]->actor_id = (uint32_t)(300 + i);
        /* Send 2 messages each. */
        sta_actor_send_msg(root, actors[i], sel, NULL, 0);
        sta_actor_send_msg(root, actors[i], sel, NULL, 0);
    }

    /* Wait for all to complete. */
    for (int i = 0; i < 6; i++) {
        bool done = wait_for_suspended(actors[i], 2000);
        assert(done);
    }

    sta_scheduler_stop(vm);

    /* All mailboxes empty. */
    for (int i = 0; i < 6; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
    }

    /* Check if stealing happened (one worker should have stolen). */
    uint64_t total_steals = 0;
    for (uint32_t i = 0; i < vm->scheduler->num_threads; i++) {
        total_steals += vm->scheduler->workers[i].steals;
    }
    /* Steals may or may not happen depending on timing. Just verify
     * all actors completed — that's the correctness requirement. */
    printf("  PASS: imbalanced_stealing (steals=%llu)\n",
           (unsigned long long)total_steals);

    sta_scheduler_destroy(vm);
    for (int i = 0; i < 6; i++) {
        sta_actor_destroy(actors[i]);
    }
    sta_vm_destroy(vm);
}

/* ── Test 4: long-running actors with work stealing ──────────────────── */

static void install_method(STA_VM *vm, struct STA_Actor *actor,
                           const char *src, const char *selector_name) {
    STA_OOP cls = actor->behavior_class;
    STA_OOP md = sta_class_method_dict(cls);
    if (md == 0) {
        md = sta_method_dict_create(&vm->root_actor->heap, 8);
        assert(md != 0);
        STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)cls;
        sta_payload(cls_h)[STA_CLASS_SLOT_METHODDICT] = md;
    }
    STA_CompileResult cr = sta_compile_method(
        src, cls, NULL, 0,
        &vm->symbol_table, &vm->immutable_space,
        &vm->root_actor->heap, vm->specials[SPC_SMALLTALK]);
    assert(cr.method != 0);
    STA_OOP sel = intern(vm, selector_name);
    sta_method_dict_insert(&vm->root_actor->heap, md, sel, cr.method);
}

static void test_long_running_stealing(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Two long-running actors — preemption means they get re-enqueued
     * to the local deque, and the other worker can steal them. */
    struct STA_Actor *a1 = make_child_actor(vm);
    struct STA_Actor *a2 = make_child_actor(vm);
    a1->actor_id = 400;
    a2->actor_id = 401;

    install_method(vm, a1,
        "work | i | i := 0. [i < 2000] whileTrue: [i := i + 1]. ^ i",
        "work");
    install_method(vm, a2,
        "work | i | i := 0. [i < 2000] whileTrue: [i := i + 1]. ^ i",
        "work");

    STA_OOP sel = intern(vm, "work");
    struct STA_Actor *root = vm->root_actor;
    sta_actor_send_msg(root, a1, sel, NULL, 0);
    sta_actor_send_msg(root, a2, sel, NULL, 0);

    bool done1 = wait_for_suspended(a1, 3000);
    bool done2 = wait_for_suspended(a2, 3000);
    assert(done1);
    assert(done2);

    sta_scheduler_stop(vm);

    uint64_t total_runs = 0;
    uint64_t total_steals = 0;
    for (uint32_t i = 0; i < vm->scheduler->num_threads; i++) {
        total_runs += vm->scheduler->workers[i].actors_run;
        total_steals += vm->scheduler->workers[i].steals;
    }

    printf("  PASS: long_running_stealing (runs=%llu, steals=%llu)\n",
           (unsigned long long)total_runs,
           (unsigned long long)total_steals);

    sta_scheduler_destroy(vm);
    sta_actor_destroy(a1);
    sta_actor_destroy(a2);
    sta_vm_destroy(vm);
}

/* ── Test 5: existing tests still work (backward compat) ─────────────── */

static void test_backward_compat(void) {
    /* Just verify sta_eval still works with scheduler not started. */
    STA_VM *vm = make_vm();

    STA_Handle *result = sta_eval(vm, "3 + 4");
    assert(result != NULL);
    const char *s = sta_inspect_cstring(vm, result);
    assert(strcmp(s, "7") == 0);
    sta_handle_release(vm, result);

    sta_vm_destroy(vm);
    printf("  PASS: backward_compat\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_work_stealing (Epic 4 Story 5):\n");

    test_deque_basic();
    test_deque_capacity();
    test_imbalanced_stealing();
    test_long_running_stealing();
    test_backward_compat();

    printf("All work-stealing tests passed.\n");
    return 0;
}
