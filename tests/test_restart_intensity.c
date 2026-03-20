/* tests/test_restart_intensity.c
 * Phase 2 Epic 6, Story 5: Restart intensity limiting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "actor/actor.h"
#include "actor/supervisor.h"
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
                            const char *source) {
    STA_CompileResult r = sta_compile_method(
        source, cls, NULL, 0,
        &g_vm->symbol_table, &g_vm->immutable_space,
        &g_vm->root_actor->heap,
        g_vm->specials[SPC_SMALLTALK]);
    if (r.had_error) {
        fprintf(stderr, "compile error: %s\nsource: %s\n", r.error_msg, source);
    }
    assert(!r.had_error);

    STA_OOP md = sta_class_method_dict(cls);
    assert(md != 0);
    STA_OOP selector = intern(selector_str);
    sta_method_dict_insert(&g_vm->root_actor->heap, md, selector, r.method);
}

/* Create a supervisor actor with Object as behavior class. */
static struct STA_Actor *make_supervisor(uint32_t actor_id,
                                          uint32_t max_restarts,
                                          uint32_t max_seconds) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *sup = sta_actor_create(g_vm, 16384, 2048);
    assert(sup);
    sta_supervisor_init(sup, max_restarts, max_seconds);
    sup->behavior_class = obj_cls;
    STA_ObjHeader *obj_h = sta_heap_alloc(&sup->heap, STA_CLS_OBJECT, 0);
    assert(obj_h);
    sup->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    (void)actor_id;  /* IDs are now opaque, auto-assigned */
    atomic_store_explicit(&sup->state, STA_ACTOR_SUSPENDED, memory_order_relaxed);
    return sup;
}

/* Add a child and set up its behavior_obj. */
static struct STA_Actor *add_child(struct STA_Actor *sup,
                                     STA_RestartStrategy strategy) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls, strategy);
    assert(child);
    STA_ObjHeader *obj_h = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(obj_h);
    child->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    return child;
}

/* Drain any pending messages (e.g. #initialize from a restart) then
 * send #crash and process until the exception fires. */
static void crash_and_notify(struct STA_Actor *child, struct STA_Actor *sup) {
    /* Drain pending messages (e.g. #initialize after restart). */
    while (!sta_mailbox_is_empty(&child->mailbox)) {
        atomic_store_explicit(&child->state, STA_ACTOR_RUNNING,
                              memory_order_relaxed);
        sta_actor_process_one_preemptible(g_vm, child);
    }

    /* Send crash message and process it. */
    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&child->mailbox, msg);

    atomic_store_explicit(&child->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, child);
    assert(rc == STA_ACTOR_MSG_EXCEPTION);

    /* Process supervisor notification. */
    assert(!sta_mailbox_is_empty(&sup->mailbox));
    atomic_store_explicit(&sup->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    rc = sta_actor_process_one_preemptible(g_vm, sup);
    assert(rc == STA_ACTOR_MSG_PROCESSED);
}

/* ── Test: rapid crash loop exceeds intensity ────────────────────────────── */

/* Common setup: install crash and initialize methods on Object. */
static void install_common_methods(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");
    install_method(obj_cls, "initialize", "initialize\n  ^self");
}

static void test_rapid_crash_exceeds_intensity(void) {
    setup();
    install_common_methods();

    /* Supervisor with max_restarts=3, max_seconds=5. */
    struct STA_Actor *sup = make_supervisor(1, 3, 5);
    struct STA_Actor *child = add_child(sup, STA_RESTART_RESTART);

    /* Crash 1: restart_count → 1 (allowed) */
    crash_and_notify(child, sup);
    child = sup->sup_data->children->current_actor;
    assert(child != NULL);  /* Restarted */

    /* Crash 2: restart_count → 2 (allowed) */
    crash_and_notify(child, sup);
    child = sup->sup_data->children->current_actor;
    assert(child != NULL);  /* Restarted */

    /* Crash 3: restart_count → 3 (allowed, == max_restarts) */
    crash_and_notify(child, sup);
    child = sup->sup_data->children->current_actor;
    assert(child != NULL);  /* Restarted */

    /* Crash 4: restart_count → 4 (> max_restarts=3) → intensity exceeded */
    crash_and_notify(child, sup);

    /* All children should be terminated (NULL). */
    STA_ChildSpec *spec = sup->sup_data->children;
    assert(spec->current_actor == NULL);

    /* Supervisor should be TERMINATED. */
    uint32_t state = atomic_load_explicit(&sup->state, memory_order_acquire);
    assert(state == STA_ACTOR_TERMINATED);

    sta_supervisor_data_destroy(sup->sup_data);
    sup->sup_data = NULL;
    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_rapid_crash_exceeds_intensity\n");
}

/* ── Test: window reset allows restarts after timeout ────────────────────── */

static void test_window_reset(void) {
    setup();
    install_common_methods();

    /* Supervisor with max_restarts=2, max_seconds=0 (tiny window).
     * A window of 0 seconds means any time > 0ns resets the window. */
    struct STA_Actor *sup = make_supervisor(1, 2, 0);
    struct STA_Actor *child = add_child(sup, STA_RESTART_RESTART);

    /* Crash 1: restart_count → 1 */
    crash_and_notify(child, sup);
    child = sup->sup_data->children->current_actor;
    assert(child != NULL);

    /* Sleep briefly so the window elapses (0 seconds). */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000 }; /* 1 ms */
    nanosleep(&ts, NULL);

    /* Crash 2: window elapsed → restart_count reset to 0 → incremented to 1 */
    crash_and_notify(child, sup);
    child = sup->sup_data->children->current_actor;
    assert(child != NULL);  /* Should restart (counter was reset) */

    /* Verify restart_count is 1 (reset + 1 increment). */
    assert(sup->sup_data->restart_count == 1);

    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_window_reset\n");
}

/* ── Test: multiple children — restarts from different children count ─────── */

static void test_multiple_children_intensity(void) {
    setup();
    install_common_methods();

    /* Supervisor with max_restarts=2, max_seconds=5.
     * 3 children, all RESTART. */
    struct STA_Actor *sup = make_supervisor(1, 2, 5);
    struct STA_Actor *child1 = add_child(sup, STA_RESTART_RESTART);
    struct STA_Actor *child2 = add_child(sup, STA_RESTART_RESTART);
    struct STA_Actor *child3 = add_child(sup, STA_RESTART_RESTART);

    /* Walk specs: prepended order is child3, child2, child1. */
    STA_ChildSpec *spec3 = sup->sup_data->children;
    STA_ChildSpec *spec2 = spec3->next;
    STA_ChildSpec *spec1 = spec2->next;
    assert(spec1->current_actor == child1);
    assert(spec2->current_actor == child2);
    assert(spec3->current_actor == child3);

    /* Crash child1: restart_count → 1 */
    crash_and_notify(child1, sup);
    assert(spec1->current_actor != NULL);
    child1 = spec1->current_actor;

    /* Crash child2: restart_count → 2 (different child, same counter) */
    crash_and_notify(child2, sup);
    assert(spec2->current_actor != NULL);
    child2 = spec2->current_actor;

    /* Crash child3: restart_count → 3 (> max_restarts=2) → EXCEEDED */
    crash_and_notify(child3, sup);

    /* All children should be NULL. */
    assert(spec1->current_actor == NULL);
    assert(spec2->current_actor == NULL);
    assert(spec3->current_actor == NULL);

    /* Supervisor should be TERMINATED. */
    uint32_t state = atomic_load_explicit(&sup->state, memory_order_acquire);
    assert(state == STA_ACTOR_TERMINATED);

    sta_supervisor_data_destroy(sup->sup_data);
    sup->sup_data = NULL;
    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_multiple_children_intensity\n");
}

/* ── Test: all children terminated when intensity exceeded ────────────────── */

static void test_all_children_terminated(void) {
    setup();
    install_common_methods();

    /* Supervisor with max_restarts=1, max_seconds=5.
     * 3 children: one crashes, one is healthy. */
    struct STA_Actor *sup = make_supervisor(1, 1, 5);
    struct STA_Actor *crasher = add_child(sup, STA_RESTART_RESTART);
    struct STA_Actor *healthy1 = add_child(sup, STA_RESTART_RESTART);
    struct STA_Actor *healthy2 = add_child(sup, STA_RESTART_RESTART);

    /* Crash crasher: restart_count → 1 (OK, == max_restarts) */
    crash_and_notify(crasher, sup);
    STA_ChildSpec *crasher_spec = sup->sup_data->children->next->next;
    crasher = crasher_spec->current_actor;
    assert(crasher != NULL);

    /* Crash again: restart_count → 2 (> 1) → intensity exceeded.
     * ALL children (including healthy1 and healthy2) should be terminated. */
    crash_and_notify(crasher, sup);

    /* Verify ALL child specs have NULL actors. */
    STA_ChildSpec *spec = sup->sup_data->children;
    while (spec) {
        assert(spec->current_actor == NULL);
        spec = spec->next;
    }

    /* healthy1 and healthy2 should NOT be accessible anymore (destroyed). */
    (void)healthy1;
    (void)healthy2;

    sta_supervisor_data_destroy(sup->sup_data);
    sup->sup_data = NULL;
    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_all_children_terminated\n");
}

/* ── Test: escalation sent to grandparent ────────────────────────────────── */

static void test_intensity_escalation_to_grandparent(void) {
    setup();
    install_common_methods();

    /* Grandparent → parent → worker.
     * Parent has max_restarts=1, max_seconds=5. */
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *gp = make_supervisor(1, 10, 10);

    struct STA_Actor *parent = sta_supervisor_add_child(
        gp, obj_cls, STA_RESTART_RESTART);
    assert(parent);
    STA_ObjHeader *p_obj = sta_heap_alloc(&parent->heap, STA_CLS_OBJECT, 0);
    assert(p_obj);
    parent->behavior_obj = (STA_OOP)(uintptr_t)p_obj;
    sta_supervisor_init(parent, 1, 5);

    struct STA_Actor *worker = add_child(parent, STA_RESTART_RESTART);

    /* First crash: restart_count → 1 (OK, == max_restarts) */
    crash_and_notify(worker, parent);
    worker = parent->sup_data->children->current_actor;
    assert(worker != NULL);

    /* Second crash: restart_count → 2 (> 1) → intensity exceeded.
     * Parent should terminate children, escalate to grandparent. */
    uint32_t old_parent_id = parent->actor_id;
    crash_and_notify(worker, parent);

    /* Parent should be TERMINATED. */
    uint32_t parent_state = atomic_load_explicit(&parent->state,
                                                   memory_order_acquire);
    assert(parent_state == STA_ACTOR_TERMINATED);

    /* Grandparent should have received an escalation notification. */
    assert(!sta_mailbox_is_empty(&gp->mailbox));

    /* Process grandparent notification — should RESTART the parent. */
    atomic_store_explicit(&gp->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, gp);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Grandparent's child spec should have a new actor (restarted parent). */
    STA_ChildSpec *gp_spec = gp->sup_data->children;
    assert(gp_spec->current_actor != NULL);
    assert(gp_spec->current_actor->actor_id != old_parent_id);

    sta_actor_destroy(gp);
    teardown();
    printf("  PASS: test_intensity_escalation_to_grandparent\n");
}

/* ── Test: supervisor transitions to TERMINATED ──────────────────────────── */

static void test_supervisor_terminated_on_exceeded(void) {
    setup();
    install_common_methods();

    /* max_restarts=0 → first restart attempt immediately exceeds. */
    struct STA_Actor *sup = make_supervisor(1, 0, 5);
    struct STA_Actor *child = add_child(sup, STA_RESTART_RESTART);

    /* First crash: restart_count → 1 (> 0) → immediately exceeded. */
    crash_and_notify(child, sup);

    /* Supervisor should be TERMINATED. */
    uint32_t state = atomic_load_explicit(&sup->state, memory_order_acquire);
    assert(state == STA_ACTOR_TERMINATED);

    /* Child should be NULL. */
    assert(sup->sup_data->children->current_actor == NULL);

    sta_supervisor_data_destroy(sup->sup_data);
    sup->sup_data = NULL;
    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_supervisor_terminated_on_exceeded\n");
}

/* ── Test: max_restarts=0 edge case ──────────────────────────────────────── */

static void test_max_restarts_zero(void) {
    setup();
    install_common_methods();

    /* max_restarts=0 → no restarts allowed at all. */
    struct STA_Actor *sup = make_supervisor(1, 0, 5);
    struct STA_Actor *child = add_child(sup, STA_RESTART_RESTART);

    /* Very first crash: immediately exceeds intensity. */
    crash_and_notify(child, sup);

    uint32_t state = atomic_load_explicit(&sup->state, memory_order_acquire);
    assert(state == STA_ACTOR_TERMINATED);
    assert(sup->sup_data->children->current_actor == NULL);

    sta_supervisor_data_destroy(sup->sup_data);
    sup->sup_data = NULL;
    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_max_restarts_zero\n");
}

/* ── Test: STOP and ESCALATE do NOT increment restart counter ────────────── */

static void test_stop_escalate_no_counter_increment(void) {
    setup();
    install_common_methods();

    /* Supervisor with max_restarts=1 and three children of different strategies. */
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *gp = make_supervisor(1, 10, 10);
    struct STA_Actor *sup = sta_supervisor_add_child(gp, obj_cls,
                                                       STA_RESTART_RESTART);
    assert(sup);
    STA_ObjHeader *s_obj = sta_heap_alloc(&sup->heap, STA_CLS_OBJECT, 0);
    assert(s_obj);
    sup->behavior_obj = (STA_OOP)(uintptr_t)s_obj;
    sta_supervisor_init(sup, 1, 5);

    struct STA_Actor *stop_child = add_child(sup, STA_RESTART_STOP);
    struct STA_Actor *escalate_child = add_child(sup, STA_RESTART_ESCALATE);
    struct STA_Actor *restart_child_actor = add_child(sup, STA_RESTART_RESTART);

    /* Crash STOP child — should NOT increment restart_count. */
    crash_and_notify(stop_child, sup);
    assert(sup->sup_data->restart_count == 0);

    /* Crash ESCALATE child — should NOT increment restart_count. */
    crash_and_notify(escalate_child, sup);
    assert(sup->sup_data->restart_count == 0);
    /* Note: grandparent now has an escalation notification in its mailbox.
     * Do NOT process it — the grandparent would RESTART sup, destroying
     * the current sup and its children. We only care about the counter. */

    /* Crash RESTART child — should increment restart_count to 1 (== max_restarts). */
    crash_and_notify(restart_child_actor, sup);
    assert(sup->sup_data->restart_count == 1);

    /* Supervisor should still be alive (1 == max_restarts, not exceeded). */
    uint32_t state = atomic_load_explicit(&sup->state, memory_order_acquire);
    assert(state != STA_ACTOR_TERMINATED);

    sta_actor_destroy(gp);
    teardown();
    printf("  PASS: test_stop_escalate_no_counter_increment\n");
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_restart_intensity:\n");
    test_rapid_crash_exceeds_intensity();
    test_window_reset();
    test_multiple_children_intensity();
    test_all_children_terminated();
    test_intensity_escalation_to_grandparent();
    test_supervisor_terminated_on_exceeded();
    test_max_restarts_zero();
    test_stop_escalate_no_counter_increment();
    printf("All restart intensity tests passed.\n");
    return 0;
}
