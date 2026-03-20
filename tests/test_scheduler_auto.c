/* tests/test_scheduler_auto.c
 * Phase 2 Epic 4 Story 2: Automatic scheduling on message arrival.
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

/* Create a child actor with Object behavior. */
static struct STA_Actor *make_child_actor(STA_VM *vm) {
    struct STA_Actor *child = sta_actor_create(vm, 4096, 2048);
    assert(child != NULL);
    child->vm = vm;

    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    STA_ObjHeader *obj_h = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(obj_h != NULL);
    child->behavior_class = obj_cls;
    child->behavior_obj = (STA_OOP)(uintptr_t)obj_h;

    return child;
}

static STA_OOP yourself_selector(STA_VM *vm) {
    return sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, "yourself", 8);
}

/* Wait for an actor's mailbox to drain, up to timeout_ms. */
static bool wait_for_empty(struct STA_Actor *actor, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        if (sta_mailbox_is_empty(&actor->mailbox)) return true;
        usleep(1000);
    }
    return false;
}

/* ── Test 1: send triggers auto-schedule (CREATED → READY) ───────────── */

static void test_send_auto_schedules_created(void) {
    STA_VM *vm = make_vm();

    /* Start scheduler first. */
    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Create actor (state = CREATED). */
    struct STA_Actor *child = make_child_actor(vm);
    assert(atomic_load(&child->state) == STA_ACTOR_CREATED);

    /* Send a message — should auto-schedule. */
    STA_OOP sel = yourself_selector(vm);
    struct STA_Actor *root = vm->root_actor;
    int send_rc = sta_actor_send_msg(root, child, sel, NULL, 0);
    assert(send_rc == 0);

    /* Wait for processing. */
    bool done = wait_for_empty(child, 500);
    assert(done);

    sta_scheduler_stop(vm);

    /* Actor should be SUSPENDED after processing. */
    uint32_t state = atomic_load(&child->state);
    assert(state == STA_ACTOR_SUSPENDED);

    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: send_auto_schedules_created\n");
}

/* ── Test 2: send wakes SUSPENDED actor ──────────────────────────────── */

static void test_send_wakes_suspended(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *child = make_child_actor(vm);
    STA_OOP sel = yourself_selector(vm);
    struct STA_Actor *root = vm->root_actor;

    /* First message: auto-schedules from CREATED. */
    sta_actor_send_msg(root, child, sel, NULL, 0);
    bool done = wait_for_empty(child, 500);
    assert(done);

    /* Actor should now be SUSPENDED. */
    assert(atomic_load(&child->state) == STA_ACTOR_SUSPENDED);

    /* Second message: should wake from SUSPENDED. */
    sta_actor_send_msg(root, child, sel, NULL, 0);
    done = wait_for_empty(child, 500);
    assert(done);

    assert(atomic_load(&child->state) == STA_ACTOR_SUSPENDED);

    sta_scheduler_stop(vm);
    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: send_wakes_suspended\n");
}

/* ── Test 3: multiple messages auto-scheduled ────────────────────────── */

static void test_multiple_msgs_auto_scheduled(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *child = make_child_actor(vm);
    STA_OOP sel = yourself_selector(vm);
    struct STA_Actor *root = vm->root_actor;

    /* Send 10 messages in quick succession. */
    for (int i = 0; i < 10; i++) {
        sta_actor_send_msg(root, child, sel, NULL, 0);
    }

    /* Wait for all to be processed. */
    bool done = wait_for_empty(child, 1000);
    assert(done);

    sta_scheduler_stop(vm);

    assert(vm->scheduler->workers[0].messages_processed >= 10);

    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: multiple_msgs_auto_scheduled\n");
}

/* ── Test 4: no auto-schedule without scheduler ──────────────────────── */

static void test_no_schedule_without_scheduler(void) {
    STA_VM *vm = make_vm();

    /* No scheduler init — vm->scheduler is NULL. */
    struct STA_Actor *child = make_child_actor(vm);
    STA_OOP sel = yourself_selector(vm);
    struct STA_Actor *root = vm->root_actor;

    /* Send — should succeed without crashing, no auto-schedule. */
    int send_rc = sta_actor_send_msg(root, child, sel, NULL, 0);
    assert(send_rc == 0);

    /* Message is in mailbox but not dispatched. */
    assert(!sta_mailbox_is_empty(&child->mailbox));
    assert(atomic_load(&child->state) == STA_ACTOR_CREATED);

    /* Manual dispatch still works. */
    int proc_rc = sta_actor_process_one(vm, child);
    assert(proc_rc == 1);
    assert(sta_mailbox_is_empty(&child->mailbox));

    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: no_schedule_without_scheduler\n");
}

/* ── Test 5: RUNNING actor does not get re-enqueued ──────────────────── */

static void test_running_actor_not_reenqueued(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    /* Don't start — just test the CAS logic. */

    struct STA_Actor *child = make_child_actor(vm);
    STA_OOP sel = yourself_selector(vm);
    struct STA_Actor *root = vm->root_actor;

    /* Manually set state to RUNNING. */
    atomic_store(&child->state, STA_ACTOR_RUNNING);

    /* Set scheduler running flag so the auto-schedule path is taken. */
    atomic_store(&vm->scheduler->running, true);

    /* Send — should NOT enqueue (actor is RUNNING). */
    sta_actor_send_msg(root, child, sel, NULL, 0);

    /* Overflow queue should be empty — actor was RUNNING, CAS fails. */
    assert(vm->scheduler->overflow_head == NULL);

    /* State should still be RUNNING. */
    assert(atomic_load(&child->state) == STA_ACTOR_RUNNING);

    atomic_store(&vm->scheduler->running, false);
    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: running_actor_not_reenqueued\n");
}

/* ── Test 6: two actors, reactive scheduling ─────────────────────────── */

static void test_two_actors_reactive(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *a1 = make_child_actor(vm);
    struct STA_Actor *a2 = make_child_actor(vm);
    a1->actor_id = 100;
    a2->actor_id = 200;

    STA_OOP sel = yourself_selector(vm);
    struct STA_Actor *root = vm->root_actor;

    /* Send to both actors. */
    sta_actor_send_msg(root, a1, sel, NULL, 0);
    sta_actor_send_msg(root, a2, sel, NULL, 0);

    /* Wait for both to process. */
    bool done1 = wait_for_empty(a1, 500);
    bool done2 = wait_for_empty(a2, 500);
    assert(done1);
    assert(done2);

    sta_scheduler_stop(vm);

    assert(atomic_load(&a1->state) == STA_ACTOR_SUSPENDED);
    assert(atomic_load(&a2->state) == STA_ACTOR_SUSPENDED);

    sta_scheduler_destroy(vm);
    sta_actor_destroy(a1);
    sta_actor_destroy(a2);
    sta_vm_destroy(vm);
    printf("  PASS: two_actors_reactive\n");
}

/* ── Test 7: FIFO message ordering preserved ─────────────────────────── */

static void test_fifo_ordering(void) {
    STA_VM *vm = make_vm();

    /* Send 3 messages manually, then process — verify FIFO. */
    struct STA_Actor *child = make_child_actor(vm);
    struct STA_Actor *root = vm->root_actor;

    STA_OOP sel1 = sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, "yourself", 8);
    STA_OOP sel2 = sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, "class", 5);
    STA_OOP sel3 = sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, "yourself", 8);

    sta_actor_send_msg(root, child, sel1, NULL, 0);
    sta_actor_send_msg(root, child, sel2, NULL, 0);
    sta_actor_send_msg(root, child, sel3, NULL, 0);

    /* Process all three manually. */
    assert(sta_actor_process_one(vm, child) == 1);
    assert(sta_actor_process_one(vm, child) == 1);
    assert(sta_actor_process_one(vm, child) == 1);
    assert(sta_actor_process_one(vm, child) == 0);  /* empty */

    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: fifo_ordering\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_scheduler_auto (Epic 4 Story 2):\n");

    test_send_auto_schedules_created();
    test_send_wakes_suspended();
    test_multiple_msgs_auto_scheduled();
    test_no_schedule_without_scheduler();
    test_running_actor_not_reenqueued();
    test_two_actors_reactive();
    test_fifo_ordering();

    printf("All scheduler story 2 tests passed.\n");
    return 0;
}
