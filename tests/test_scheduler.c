/* tests/test_scheduler.c
 * Phase 2 Epic 4 Story 1: Scheduler lifecycle and single-thread dispatch.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "scheduler/scheduler.h"
#include "actor/actor.h"
#include "vm/vm_state.h"
#include "vm/interpreter.h"
#include "vm/special_objects.h"
#include "vm/compiled_method.h"
#include "vm/symbol_table.h"
#include "vm/method_dict.h"
#include <sta/vm.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static STA_VM *make_vm(void) {
    STA_VMConfig cfg = { .image_path = NULL };
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);
    return vm;
}

/* ── Test 1: init and destroy without starting ────────────────────────── */

static void test_init_destroy(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    assert(vm->scheduler != NULL);
    assert(vm->scheduler->num_threads == 1);
    assert(atomic_load(&vm->scheduler->running) == false);

    sta_scheduler_destroy(vm);
    assert(vm->scheduler == NULL);

    sta_vm_destroy(vm);
    printf("  PASS: init_destroy\n");
}

/* ── Test 2: start and stop with no actors ────────────────────────────── */

static void test_start_stop_empty(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);

    rc = sta_scheduler_start(vm);
    assert(rc == 0);
    assert(atomic_load(&vm->scheduler->running) == true);

    /* Let the worker thread spin briefly. */
    usleep(10000);  /* 10 ms */

    sta_scheduler_stop(vm);
    assert(atomic_load(&vm->scheduler->running) == false);

    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
    printf("  PASS: start_stop_empty\n");
}

/* ── Test 3: enqueue/dequeue actors ──────────────────────────────────── */

static void test_enqueue_dequeue(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);

    /* Create two test actors. */
    struct STA_Actor *a1 = sta_actor_create(vm, 1024, 512);
    struct STA_Actor *a2 = sta_actor_create(vm, 1024, 512);
    assert(a1 && a2);
    a1->actor_id = 10;
    a2->actor_id = 20;

    STA_Scheduler *sched = vm->scheduler;

    /* Enqueue a1, a2. */
    sta_scheduler_enqueue(sched, a1);
    sta_scheduler_enqueue(sched, a2);
    assert(sched->run_queue.count == 2);

    /* Dequeue — FIFO order. */
    struct STA_Actor *d1 = sta_scheduler_dequeue(sched);
    assert(d1 == a1);
    assert(d1->actor_id == 10);

    struct STA_Actor *d2 = sta_scheduler_dequeue(sched);
    assert(d2 == a2);
    assert(d2->actor_id == 20);

    /* Empty. */
    struct STA_Actor *d3 = sta_scheduler_dequeue(sched);
    assert(d3 == NULL);

    sta_actor_destroy(a1);
    sta_actor_destroy(a2);
    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
    printf("  PASS: enqueue_dequeue\n");
}

/* ── Test 4: single actor dispatched by scheduler ────────────────────── */

static void test_single_actor_dispatch(void) {
    STA_VM *vm = make_vm();

    /* Create a child actor with a simple method. */
    struct STA_Actor *child = sta_actor_create(vm, 4096, 2048);
    assert(child != NULL);
    child->actor_id = 42;
    child->vm = vm;

    /* Set up actor behavior: use Object class (has methods via bootstrap).
     * We'll send #yourself which is prim 42 — returns self. */
    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    STA_ObjHeader *obj_h = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(obj_h != NULL);
    child->behavior_class = obj_cls;
    child->behavior_obj = (STA_OOP)(uintptr_t)obj_h;

    /* Send a message to the child actor. */
    STA_OOP yourself_sel = sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, "yourself", 8);
    assert(yourself_sel != 0);

    /* Use the root actor as sender. */
    struct STA_Actor *root = vm->root_actor;
    int send_rc = sta_actor_send_msg(root, child, yourself_sel, NULL, 0);
    assert(send_rc == 0);
    assert(!sta_mailbox_is_empty(&child->mailbox));

    /* Init scheduler, enqueue child, start, and wait for processing. */
    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);

    atomic_store_explicit(&child->state, STA_ACTOR_READY, memory_order_relaxed);
    sta_scheduler_enqueue(vm->scheduler, child);

    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Wait for the message to be processed. */
    for (int i = 0; i < 100; i++) {
        if (sta_mailbox_is_empty(&child->mailbox)) break;
        usleep(5000);
    }

    sta_scheduler_stop(vm);

    /* Verify: message was processed (mailbox empty). */
    assert(sta_mailbox_is_empty(&child->mailbox));

    /* Worker processed the actor. */
    assert(vm->scheduler->workers[0].actors_run >= 1);
    assert(vm->scheduler->workers[0].messages_processed >= 1);

    /* Actor should be suspended (no more messages). */
    uint32_t state = atomic_load_explicit(&child->state, memory_order_acquire);
    assert(state == STA_ACTOR_SUSPENDED);

    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: single_actor_dispatch\n");
}

/* ── Test 5: multiple messages processed ─────────────────────────────── */

static void test_multiple_messages(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *child = sta_actor_create(vm, 4096, 2048);
    assert(child != NULL);
    child->actor_id = 43;
    child->vm = vm;

    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    STA_ObjHeader *obj_h = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(obj_h != NULL);
    child->behavior_class = obj_cls;
    child->behavior_obj = (STA_OOP)(uintptr_t)obj_h;

    STA_OOP yourself_sel = sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, "yourself", 8);

    /* Send 5 messages. */
    struct STA_Actor *root = vm->root_actor;
    for (int i = 0; i < 5; i++) {
        int send_rc = sta_actor_send_msg(root, child, yourself_sel, NULL, 0);
        assert(send_rc == 0);
    }
    assert(sta_mailbox_count(&child->mailbox) == 5);

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);

    atomic_store_explicit(&child->state, STA_ACTOR_READY, memory_order_relaxed);
    sta_scheduler_enqueue(vm->scheduler, child);

    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Wait for all messages to be processed. */
    for (int i = 0; i < 100; i++) {
        if (sta_mailbox_is_empty(&child->mailbox)) break;
        usleep(5000);
    }

    sta_scheduler_stop(vm);

    assert(sta_mailbox_is_empty(&child->mailbox));
    assert(vm->scheduler->workers[0].messages_processed >= 5);

    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: multiple_messages\n");
}

/* ── Test 6: scheduler cleans up via vm_destroy ──────────────────────── */

static void test_vm_destroy_stops_scheduler(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Destroy VM — should stop and destroy the scheduler. */
    sta_vm_destroy(vm);
    /* If we get here without hanging, it works. */
    printf("  PASS: vm_destroy_stops_scheduler\n");
}

/* ── Test 7: actor state transitions ─────────────────────────────────── */

static void test_actor_state_transitions(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *child = sta_actor_create(vm, 1024, 512);
    assert(child != NULL);

    /* CREATED at birth. */
    assert(atomic_load(&child->state) == STA_ACTOR_CREATED);

    /* Manual transition to READY. */
    atomic_store(&child->state, STA_ACTOR_READY);
    assert(atomic_load(&child->state) == STA_ACTOR_READY);

    /* Manual transition to RUNNING. */
    atomic_store(&child->state, STA_ACTOR_RUNNING);
    assert(atomic_load(&child->state) == STA_ACTOR_RUNNING);

    /* Manual transition to SUSPENDED. */
    atomic_store(&child->state, STA_ACTOR_SUSPENDED);
    assert(atomic_load(&child->state) == STA_ACTOR_SUSPENDED);

    /* Manual transition to TERMINATED. */
    atomic_store(&child->state, STA_ACTOR_TERMINATED);
    assert(atomic_load(&child->state) == STA_ACTOR_TERMINATED);

    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: actor_state_transitions\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_scheduler (Epic 4 Story 1):\n");

    test_init_destroy();
    test_start_stop_empty();
    test_enqueue_dequeue();
    test_single_actor_dispatch();
    test_multiple_messages();
    test_vm_destroy_stops_scheduler();
    test_actor_state_transitions();

    printf("All scheduler story 1 tests passed.\n");
    return 0;
}
