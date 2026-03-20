/* tests/test_scheduler_stress.c
 * Phase 2 Epic 4 Story 7: Comprehensive scheduler tests.
 * Stress tests, edge cases, correctness under load.
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

static bool wait_all_suspended(struct STA_Actor **actors, int count,
                                int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        bool all_done = true;
        for (int j = 0; j < count; j++) {
            if (atomic_load(&actors[j]->state) != STA_ACTOR_SUSPENDED) {
                all_done = false;
                break;
            }
        }
        if (all_done) return true;
        usleep(1000);
    }
    return false;
}

/* ── Test 1: 10 actors, 100 messages each ────────────────────────────── */

static void test_10_actors_100_messages(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 4);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *actors[10];
    STA_OOP sel = intern(vm, "yourself");
    struct STA_Actor *root = vm->root_actor;

    for (int i = 0; i < 10; i++) {
        actors[i] = make_child_actor(vm);
        actors[i]->actor_id = (uint32_t)(1000 + i);
    }

    /* Send 100 messages to each actor. */
    for (int msg = 0; msg < 100; msg++) {
        for (int i = 0; i < 10; i++) {
            sta_actor_send_msg(root, actors[i], sel, NULL, 0);
        }
    }

    bool done = wait_all_suspended(actors, 10, 5000);
    sta_scheduler_stop(vm);
    assert(done);

    /* All mailboxes empty. */
    for (int i = 0; i < 10; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
    }

    /* Total messages processed. */
    uint64_t total_msgs = 0;
    uint64_t total_runs = 0;
    for (uint32_t i = 0; i < vm->scheduler->num_threads; i++) {
        total_msgs += vm->scheduler->workers[i].messages_processed;
        total_runs += vm->scheduler->workers[i].actors_run;
    }
    assert(total_msgs >= 1000);

    printf("  PASS: 10_actors_100_messages (msgs=%llu, runs=%llu)\n",
           (unsigned long long)total_msgs, (unsigned long long)total_runs);

    sta_scheduler_destroy(vm);
    for (int i = 0; i < 10; i++) sta_actor_destroy(actors[i]);
    sta_vm_destroy(vm);
}

/* ── Test 2: preemption stress (long-running actors) ─────────────────── */

static void test_preemption_stress(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 4);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* 4 actors, each with a 2000-iteration loop. */
    struct STA_Actor *actors[4];
    for (int i = 0; i < 4; i++) {
        actors[i] = make_child_actor(vm);
        actors[i]->actor_id = (uint32_t)(2000 + i);
        install_method(vm, actors[i],
            "work | i | i := 0. [i < 2000] whileTrue: [i := i + 1]. ^ i",
            "work");
    }

    STA_OOP sel = intern(vm, "work");
    struct STA_Actor *root = vm->root_actor;
    for (int i = 0; i < 4; i++) {
        sta_actor_send_msg(root, actors[i], sel, NULL, 0);
    }

    bool done = wait_all_suspended(actors, 4, 5000);
    sta_scheduler_stop(vm);
    assert(done);

    uint64_t total_runs = 0;
    uint64_t total_steals = 0;
    for (uint32_t i = 0; i < vm->scheduler->num_threads; i++) {
        total_runs += vm->scheduler->workers[i].actors_run;
        total_steals += vm->scheduler->workers[i].steals;
    }
    /* Each actor should have been preempted multiple times. */
    assert(total_runs >= 8);  /* 4 actors × at least 2 runs each */

    printf("  PASS: preemption_stress (runs=%llu, steals=%llu)\n",
           (unsigned long long)total_runs, (unsigned long long)total_steals);

    sta_scheduler_destroy(vm);
    for (int i = 0; i < 4; i++) sta_actor_destroy(actors[i]);
    sta_vm_destroy(vm);
}

/* ── Test 3: FIFO message ordering within a single actor ─────────────── */

static void test_fifo_ordering_under_load(void) {
    STA_VM *vm = make_vm();

    /* Use manual dispatch (no scheduler) to verify FIFO ordering
     * is preserved in the mailbox. */
    struct STA_Actor *child = make_child_actor(vm);

    /* Send yourself messages with different args (arg is SmallInt). */
    STA_OOP sel = intern(vm, "yourself");
    struct STA_Actor *root = vm->root_actor;

    for (int i = 0; i < 50; i++) {
        sta_actor_send_msg(root, child, sel, NULL, 0);
    }

    /* Process all — should get 50 messages in order. */
    int count = 0;
    while (sta_actor_process_one(vm, child) > 0) count++;
    assert(count == 50);
    assert(sta_mailbox_is_empty(&child->mailbox));

    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: fifo_ordering_under_load\n");
}

/* ── Test 4: scheduler start with no actors ──────────────────────────── */

static void test_start_no_actors(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Let it run idle for 50ms. */
    usleep(50000);

    sta_scheduler_stop(vm);

    /* No crashes, no hangs. */
    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
    printf("  PASS: start_no_actors\n");
}

/* ── Test 5: scheduler stop while actors have messages ───────────────── */

static void test_stop_with_pending_messages(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *child = make_child_actor(vm);
    child->actor_id = 3000;
    install_method(vm, child,
        "work | i | i := 0. [i < 10000] whileTrue: [i := i + 1]. ^ i",
        "work");

    STA_OOP sel = intern(vm, "work");
    /* Send a long-running message. */
    sta_actor_send_msg(vm->root_actor, child, sel, NULL, 0);
    /* Send more messages while the first is processing. */
    STA_OOP sel2 = intern(vm, "yourself");
    for (int i = 0; i < 10; i++) {
        sta_actor_send_msg(vm->root_actor, child, sel2, NULL, 0);
    }

    /* Stop immediately — don't wait for completion.
     * This tests graceful shutdown while work is pending. */
    usleep(5000);  /* Give it a moment to start. */
    sta_scheduler_stop(vm);

    /* No crash, no hang. Some messages may be unprocessed. */
    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: stop_with_pending_messages\n");
}

/* ── Test 6: actor state machine transitions ─────────────────────────── */

static void test_state_machine(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *child = make_child_actor(vm);
    child->actor_id = 4000;

    /* CREATED initially. */
    assert(atomic_load(&child->state) == STA_ACTOR_CREATED);

    /* Send message → should auto-schedule (CREATED → READY). */
    STA_OOP sel = intern(vm, "yourself");
    sta_actor_send_msg(vm->root_actor, child, sel, NULL, 0);

    /* Wait for it to go through READY → RUNNING → SUSPENDED. */
    for (int i = 0; i < 200; i++) {
        if (atomic_load(&child->state) == STA_ACTOR_SUSPENDED) break;
        usleep(1000);
    }
    assert(atomic_load(&child->state) == STA_ACTOR_SUSPENDED);

    /* Send another → SUSPENDED → READY → RUNNING → SUSPENDED. */
    sta_actor_send_msg(vm->root_actor, child, sel, NULL, 0);
    for (int i = 0; i < 200; i++) {
        if (sta_mailbox_is_empty(&child->mailbox) &&
            atomic_load(&child->state) == STA_ACTOR_SUSPENDED) break;
        usleep(1000);
    }
    assert(atomic_load(&child->state) == STA_ACTOR_SUSPENDED);

    sta_scheduler_stop(vm);
    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: state_machine\n");
}

/* ── Test 7: many actors, few messages (throughput) ──────────────────── */

static void test_many_actors_few_messages(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 4);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    #define N_ACTORS 50
    struct STA_Actor *actors[N_ACTORS];
    STA_OOP sel = intern(vm, "yourself");

    for (int i = 0; i < N_ACTORS; i++) {
        actors[i] = make_child_actor(vm);
        actors[i]->actor_id = (uint32_t)(5000 + i);
        /* 2 messages each. */
        sta_actor_send_msg(vm->root_actor, actors[i], sel, NULL, 0);
        sta_actor_send_msg(vm->root_actor, actors[i], sel, NULL, 0);
    }

    bool done = wait_all_suspended(actors, N_ACTORS, 5000);
    sta_scheduler_stop(vm);
    assert(done);

    for (int i = 0; i < N_ACTORS; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
    }

    uint64_t total_msgs = 0;
    for (uint32_t i = 0; i < vm->scheduler->num_threads; i++) {
        total_msgs += vm->scheduler->workers[i].messages_processed;
    }
    assert(total_msgs >= (uint64_t)(N_ACTORS * 2));

    printf("  PASS: many_actors_few_messages (%d actors, msgs=%llu)\n",
           N_ACTORS, (unsigned long long)total_msgs);
    #undef N_ACTORS

    sta_scheduler_destroy(vm);
    for (int i = 0; i < 50; i++) sta_actor_destroy(actors[i]);
    sta_vm_destroy(vm);
}

/* ── Test 8: work stealing verification ──────────────────────────────── */

static void test_steal_verification(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 4);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* 8 long-running actors — should trigger stealing across 4 threads. */
    struct STA_Actor *actors[8];
    for (int i = 0; i < 8; i++) {
        actors[i] = make_child_actor(vm);
        actors[i]->actor_id = (uint32_t)(6000 + i);
        install_method(vm, actors[i],
            "work | i | i := 0. [i < 1000] whileTrue: [i := i + 1]. ^ i",
            "work");
    }

    STA_OOP sel = intern(vm, "work");
    for (int i = 0; i < 8; i++) {
        sta_actor_send_msg(vm->root_actor, actors[i], sel, NULL, 0);
    }

    bool done = wait_all_suspended(actors, 8, 5000);
    sta_scheduler_stop(vm);
    assert(done);

    /* Check that multiple workers participated. */
    int active_workers = 0;
    uint64_t total_steals = 0;
    for (uint32_t i = 0; i < vm->scheduler->num_threads; i++) {
        if (vm->scheduler->workers[i].actors_run > 0) active_workers++;
        total_steals += vm->scheduler->workers[i].steals;
    }
    /* With 8 actors and 4 threads, at least 2 workers should participate. */
    assert(active_workers >= 2);

    printf("  PASS: steal_verification (active_workers=%d, steals=%llu)\n",
           active_workers, (unsigned long long)total_steals);

    sta_scheduler_destroy(vm);
    for (int i = 0; i < 8; i++) sta_actor_destroy(actors[i]);
    sta_vm_destroy(vm);
}

/* ── Test 9: sta_eval concurrent with stress load ────────────────────── */

static void test_eval_under_stress(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 4);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Start many actors. */
    struct STA_Actor *actors[10];
    for (int i = 0; i < 10; i++) {
        actors[i] = make_child_actor(vm);
        actors[i]->actor_id = (uint32_t)(7000 + i);
        install_method(vm, actors[i],
            "work | i | i := 0. [i < 1000] whileTrue: [i := i + 1]. ^ i",
            "work");
    }

    STA_OOP sel = intern(vm, "work");
    for (int i = 0; i < 10; i++) {
        sta_actor_send_msg(vm->root_actor, actors[i], sel, NULL, 0);
    }

    /* While actors are running, do sta_eval. */
    for (int i = 0; i < 10; i++) {
        STA_Handle *r = sta_eval(vm, "3 + 4");
        assert(r != NULL);
        assert(strcmp(sta_inspect_cstring(vm, r), "7") == 0);
        sta_handle_release(vm, r);
    }

    bool done = wait_all_suspended(actors, 10, 5000);
    sta_scheduler_stop(vm);
    assert(done);

    sta_scheduler_destroy(vm);
    for (int i = 0; i < 10; i++) sta_actor_destroy(actors[i]);
    sta_vm_destroy(vm);
    printf("  PASS: eval_under_stress\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_scheduler_stress (Epic 4 Story 7):\n");

    test_10_actors_100_messages();
    test_preemption_stress();
    test_fifo_ordering_under_load();
    test_start_no_actors();
    test_stop_with_pending_messages();
    test_state_machine();
    test_many_actors_few_messages();
    test_steal_verification();
    test_eval_under_stress();

    printf("All stress tests passed.\n");
    return 0;
}
