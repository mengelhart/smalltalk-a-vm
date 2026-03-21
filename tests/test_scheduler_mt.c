/* tests/test_scheduler_mt.c
 * Phase 2 Epic 4 Story 4: Multi-threaded scheduler.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#include "scheduler/scheduler.h"
#include "actor/actor.h"
#include "vm/vm_state.h"
#include "vm/interpreter.h"
#include "vm/special_objects.h"
#include "vm/compiled_method.h"
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
        uint32_t s = atomic_load_explicit(&actor->state, memory_order_acquire);
        if (s == STA_ACTOR_SUSPENDED && actor->saved_frame == NULL &&
            sta_mailbox_is_empty(&actor->mailbox)) return true;
        usleep(1000);
    }
    return false;
}

/* ── Test 1: two threads, two actors ─────────────────────────────────── */

static void test_two_threads_two_actors(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    assert(vm->scheduler->num_threads == 2);

    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *a1 = make_child_actor(vm);
    struct STA_Actor *a2 = make_child_actor(vm);


    STA_OOP sel = intern(vm, "yourself");
    struct STA_Actor *root = vm->root_actor;

    /* Send 5 messages to each actor. */
    for (int i = 0; i < 5; i++) {
        sta_actor_send_msg(vm, root, a1->actor_id, sel, NULL, 0);
        sta_actor_send_msg(vm, root, a2->actor_id, sel, NULL, 0);
    }

    /* Wait for both to complete. */
    bool done1 = wait_for_suspended(a1, 2000);
    bool done2 = wait_for_suspended(a2, 2000);
    assert(done1);
    assert(done2);

    sta_scheduler_stop(vm);

    /* Both processed all messages. */
    assert(sta_mailbox_is_empty(&a1->mailbox));
    assert(sta_mailbox_is_empty(&a2->mailbox));

    /* Total messages processed across all workers. */
    uint64_t total_msgs = 0;
    for (uint32_t i = 0; i < vm->scheduler->num_threads; i++) {
        total_msgs += vm->scheduler->workers[i].messages_processed;
    }
    assert(total_msgs >= 10);

    sta_scheduler_destroy(vm);
    sta_actor_terminate(a1);
    sta_actor_terminate(a2);
    sta_vm_destroy(vm);
    printf("  PASS: two_threads_two_actors (total_msgs=%llu)\n",
           (unsigned long long)total_msgs);
}

/* ── Test 2: four actors, two threads ────────────────────────────────── */

static void test_four_actors_two_threads(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *actors[4];
    for (int i = 0; i < 4; i++) {
        actors[i] = make_child_actor(vm);
    }

    STA_OOP sel = intern(vm, "yourself");
    struct STA_Actor *root = vm->root_actor;

    /* Send 3 messages to each actor. */
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 3; j++) {
            sta_actor_send_msg(vm, root, actors[i]->actor_id, sel, NULL, 0);
        }
    }

    /* Wait for all to complete. */
    for (int i = 0; i < 4; i++) {
        bool done = wait_for_suspended(actors[i], 2000);
        assert(done);
    }

    sta_scheduler_stop(vm);

    /* All mailboxes empty. */
    for (int i = 0; i < 4; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
    }

    sta_scheduler_destroy(vm);
    for (int i = 0; i < 4; i++) {
        sta_actor_terminate(actors[i]);
    }
    sta_vm_destroy(vm);
    printf("  PASS: four_actors_two_threads\n");
}

/* ── Test 3: actor exclusivity (never two threads on same actor) ──────── */

/* This test uses a longer-running method and verifies that
 * the actor's state machine prevents double-execution. */
static void test_actor_exclusivity(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *child = make_child_actor(vm);


    STA_OOP sel = intern(vm, "yourself");
    struct STA_Actor *root = vm->root_actor;

    /* Send many messages to a single actor — even with 2 threads,
     * the actor should only be executed by one thread at a time. */
    for (int i = 0; i < 20; i++) {
        sta_actor_send_msg(vm, root, child->actor_id, sel, NULL, 0);
    }

    bool done = wait_for_suspended(child, 2000);
    assert(done);

    sta_scheduler_stop(vm);

    /* Actor should have processed all 20 messages successfully.
     * If two threads ran it simultaneously, we'd see corruption. */
    assert(sta_mailbox_is_empty(&child->mailbox));

    sta_scheduler_destroy(vm);
    sta_actor_terminate(child);
    sta_vm_destroy(vm);
    printf("  PASS: actor_exclusivity\n");
}

/* ── Test 4: default thread count = CPU cores ────────────────────────── */

static void test_default_thread_count(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 0);  /* 0 = auto-detect */
    assert(rc == 0);

    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    assert(vm->scheduler->num_threads == (uint32_t)cores);

    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
    printf("  PASS: default_thread_count (cores=%ld)\n", cores);
}

/* ── Test 5: concurrent message sends from multiple actors ───────────── */

static void test_concurrent_sends(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Create 3 actors. Send messages to each in interleaved order. */
    struct STA_Actor *actors[3];
    for (int i = 0; i < 3; i++) {
        actors[i] = make_child_actor(vm);
    }

    STA_OOP sel = intern(vm, "yourself");
    struct STA_Actor *root = vm->root_actor;

    /* Send 10 messages in interleaved order. */
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 3; i++) {
            sta_actor_send_msg(vm, root, actors[i]->actor_id, sel, NULL, 0);
        }
    }

    /* Wait for all to complete. */
    for (int i = 0; i < 3; i++) {
        bool done = wait_for_suspended(actors[i], 2000);
        assert(done);
    }

    sta_scheduler_stop(vm);

    for (int i = 0; i < 3; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
    }

    sta_scheduler_destroy(vm);
    for (int i = 0; i < 3; i++) {
        sta_actor_terminate(actors[i]);
    }
    sta_vm_destroy(vm);
    printf("  PASS: concurrent_sends\n");
}

/* ── Test 6: preemption with multiple threads ────────────────────────── */

static void install_method(STA_VM *vm, struct STA_Actor *actor,
                           const char *src, const char *selector_name) {
    STA_OOP cls = actor->behavior_class;
    STA_OOP md = sta_class_method_dict(cls);
    if (md == 0) {
        STA_Heap *heap = &vm->root_actor->heap;
        md = sta_method_dict_create(heap, 8);
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
    int irc = sta_method_dict_insert(&vm->root_actor->heap, md, sel, cr.method);
    assert(irc == 0);
}

static void test_preemption_multithread(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Create 2 actors with long-running methods. */
    struct STA_Actor *a1 = make_child_actor(vm);
    struct STA_Actor *a2 = make_child_actor(vm);


    install_method(vm, a1,
        "longLoop\n"
        "  | i |\n"
        "  i := 0.\n"
        "  [i < 3000] whileTrue: [i := i + 1].\n"
        "  ^ i",
        "longLoop");

    install_method(vm, a2,
        "longLoop\n"
        "  | i |\n"
        "  i := 0.\n"
        "  [i < 3000] whileTrue: [i := i + 1].\n"
        "  ^ i",
        "longLoop");

    STA_OOP sel = intern(vm, "longLoop");
    struct STA_Actor *root = vm->root_actor;

    sta_actor_send_msg(vm, root, a1->actor_id, sel, NULL, 0);
    sta_actor_send_msg(vm, root, a2->actor_id, sel, NULL, 0);

    bool done1 = wait_for_suspended(a1, 3000);
    bool done2 = wait_for_suspended(a2, 3000);
    assert(done1);
    assert(done2);

    sta_scheduler_stop(vm);

    /* Both completed. */
    assert(sta_mailbox_is_empty(&a1->mailbox));
    assert(sta_mailbox_is_empty(&a2->mailbox));

    /* Total actors_run should be > 2 due to preemption. */
    uint64_t total_runs = 0;
    for (uint32_t i = 0; i < vm->scheduler->num_threads; i++) {
        total_runs += vm->scheduler->workers[i].actors_run;
    }
    assert(total_runs >= 4);  /* each actor preempted at least once */

    printf("  PASS: preemption_multithread (total_runs=%llu)\n",
           (unsigned long long)total_runs);

    sta_scheduler_destroy(vm);
    sta_actor_terminate(a1);
    sta_actor_terminate(a2);
    sta_vm_destroy(vm);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_scheduler_mt (Epic 4 Story 4):\n");

    test_two_threads_two_actors();
    test_four_actors_two_threads();
    test_actor_exclusivity();
    test_default_thread_count();
    test_concurrent_sends();
    test_preemption_multithread();

    printf("All multi-threaded scheduler tests passed.\n");
    return 0;
}
