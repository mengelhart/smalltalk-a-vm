/* tests/test_supervision_tree.c
 * Phase 2 Epic 6, Story 6: Root supervisor and supervision tree.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>

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

static void install_common_methods(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");
    install_method(obj_cls, "initialize", "initialize\n  ^self");
    install_method(obj_cls, "noop", "noop\n  ^self");
}

/* Set up a child with behavior_obj. */
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

/* Drain pending messages, send crash, process crash and supervisor notification. */
static void crash_and_notify(struct STA_Actor *child, struct STA_Actor *sup) {
    while (!sta_mailbox_is_empty(&child->mailbox)) {
        atomic_store_explicit(&child->state, STA_ACTOR_RUNNING,
                              memory_order_relaxed);
        sta_actor_process_one_preemptible(g_vm, child);
    }

    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&child->mailbox, msg);

    atomic_store_explicit(&child->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, child);
    assert(rc == STA_ACTOR_MSG_EXCEPTION);

    assert(!sta_mailbox_is_empty(&sup->mailbox));
    atomic_store_explicit(&sup->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    rc = sta_actor_process_one_preemptible(g_vm, sup);
    assert(rc == STA_ACTOR_MSG_PROCESSED);
}

/* Event counter for testing. */
static int g_event_count;
static STA_EventType g_last_event_type;
static char g_last_event_msg[256];

static void event_counter(STA_VM *vm, const STA_Event *event, void *ctx) {
    (void)vm; (void)ctx;
    g_event_count++;
    g_last_event_type = event->type;
    if (event->message) {
        snprintf(g_last_event_msg, sizeof(g_last_event_msg), "%s",
                 event->message);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
 * 6a: Root supervisor exists and is properly configured
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_root_supervisor_exists(void) {
    setup();

    assert(g_vm->root_supervisor != NULL);
    assert(g_vm->root_supervisor->sup_data != NULL);
    assert(g_vm->root_supervisor->supervisor == NULL);

    /* Verify generous defaults. */
    assert(g_vm->root_supervisor->sup_data->max_restarts == 10);
    assert(g_vm->root_supervisor->sup_data->max_seconds == 10);

    /* Root supervisor should have a behavior obj. */
    assert(g_vm->root_supervisor->behavior_obj != 0);

    /* Root supervisor should be SUSPENDED (not running). */
    uint32_t state = atomic_load_explicit(&g_vm->root_supervisor->state,
                                            memory_order_acquire);
    assert(state == STA_ACTOR_SUSPENDED);

    teardown();
    printf("  PASS: test_root_supervisor_exists\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * 6b: sta_vm_spawn_supervised
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_spawn_supervised(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *child = sta_vm_spawn_supervised(
        g_vm, obj_cls, STA_RESTART_RESTART);
    assert(child != NULL);

    /* Child should be a child of the root supervisor. */
    assert(child->supervisor == g_vm->root_supervisor);

    /* Root supervisor should have the child in its spec list. */
    STA_ChildSpec *spec = g_vm->root_supervisor->sup_data->children;
    assert(spec != NULL);
    assert(spec->current_actor == child);
    assert(spec->strategy == STA_RESTART_RESTART);

    teardown();
    printf("  PASS: test_spawn_supervised\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Multi-level tree structure
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_multi_level_tree(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    /* Create a sub-supervisor under root. */
    struct STA_Actor *sub_sup = sta_vm_spawn_supervised(
        g_vm, obj_cls, STA_RESTART_RESTART);
    assert(sub_sup);
    sta_supervisor_init(sub_sup, 3, 5);

    /* Add workers under the sub-supervisor. */
    struct STA_Actor *w1 = add_child(sub_sup, STA_RESTART_RESTART);
    struct STA_Actor *w2 = add_child(sub_sup, STA_RESTART_RESTART);

    /* Verify tree structure. */
    assert(sub_sup->supervisor == g_vm->root_supervisor);
    assert(w1->supervisor == sub_sup);
    assert(w2->supervisor == sub_sup);

    /* Root → sub_sup (1 child of root), sub_sup → w1, w2 (2 children). */
    assert(g_vm->root_supervisor->sup_data->child_count == 1);
    assert(sub_sup->sup_data->child_count == 2);

    teardown();
    printf("  PASS: test_multi_level_tree\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Worker crash → parent supervisor restarts → root unaware
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_worker_crash_no_root_escalation(void) {
    setup();
    install_common_methods();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    /* Register event callback. */
    g_event_count = 0;
    sta_event_register(g_vm, event_counter, NULL);

    /* sub_sup under root with RESTART strategy. */
    struct STA_Actor *sub_sup = sta_vm_spawn_supervised(
        g_vm, obj_cls, STA_RESTART_RESTART);
    assert(sub_sup);
    STA_ObjHeader *ss_obj = sta_heap_alloc(&sub_sup->heap, STA_CLS_OBJECT, 0);
    assert(ss_obj);
    sub_sup->behavior_obj = (STA_OOP)(uintptr_t)ss_obj;
    sta_supervisor_init(sub_sup, 5, 5);

    struct STA_Actor *worker = add_child(sub_sup, STA_RESTART_RESTART);
    uint32_t old_worker_id = worker->actor_id;

    /* Crash worker → sub_sup restarts it. */
    crash_and_notify(worker, sub_sup);

    /* Worker should be restarted with a new actor_id. */
    STA_ChildSpec *w_spec = sub_sup->sup_data->children;
    assert(w_spec->current_actor != NULL);
    assert(w_spec->current_actor->actor_id != old_worker_id);

    /* Root supervisor should NOT have been notified (no event fired for
     * non-root supervisor handling). */
    assert(sta_mailbox_is_empty(&g_vm->root_supervisor->mailbox));

    /* Event count should be 0 — root supervisor was not involved. */
    assert(g_event_count == 0);

    sta_event_unregister(g_vm, event_counter, NULL);
    teardown();
    printf("  PASS: test_worker_crash_no_root_escalation\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Sub-supervisor exceeds intensity → escalates to root → event fired →
 * root stays alive → other subtrees unaffected
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_sub_supervisor_escalates_to_root(void) {
    setup();
    install_common_methods();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    g_event_count = 0;
    sta_event_register(g_vm, event_counter, NULL);

    /* Create two sub-supervisors under root. */
    struct STA_Actor *sup_a = sta_vm_spawn_supervised(
        g_vm, obj_cls, STA_RESTART_RESTART);
    assert(sup_a);
    STA_ObjHeader *a_obj = sta_heap_alloc(&sup_a->heap, STA_CLS_OBJECT, 0);
    assert(a_obj);
    sup_a->behavior_obj = (STA_OOP)(uintptr_t)a_obj;
    sta_supervisor_init(sup_a, 1, 5);  /* Low intensity: max_restarts=1 */

    struct STA_Actor *sup_b = sta_vm_spawn_supervised(
        g_vm, obj_cls, STA_RESTART_RESTART);
    assert(sup_b);
    STA_ObjHeader *b_obj = sta_heap_alloc(&sup_b->heap, STA_CLS_OBJECT, 0);
    assert(b_obj);
    sup_b->behavior_obj = (STA_OOP)(uintptr_t)b_obj;
    sta_supervisor_init(sup_b, 5, 5);

    struct STA_Actor *worker_b = add_child(sup_b, STA_RESTART_RESTART);

    /* Add a worker to sup_a and crash it twice to exceed intensity. */
    struct STA_Actor *worker_a = add_child(sup_a, STA_RESTART_RESTART);

    /* First crash: restart_count=1 (OK, == max_restarts=1) */
    crash_and_notify(worker_a, sup_a);
    worker_a = sup_a->sup_data->children->current_actor;
    assert(worker_a != NULL);

    /* Second crash: restart_count=2 (> 1) → intensity exceeded.
     * sup_a terminates children, escalates to root. */
    crash_and_notify(worker_a, sup_a);

    /* sup_a should be TERMINATED. */
    uint32_t sup_a_state = atomic_load_explicit(&sup_a->state,
                                                  memory_order_acquire);
    assert(sup_a_state == STA_ACTOR_TERMINATED);

    /* Root supervisor should have the escalation notification. */
    assert(!sta_mailbox_is_empty(&g_vm->root_supervisor->mailbox));

    /* Process root supervisor notification. */
    atomic_store_explicit(&g_vm->root_supervisor->state, STA_ACTOR_RUNNING,
                          memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, g_vm->root_supervisor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Root should fire STA_EVT_ACTOR_CRASH event. */
    assert(g_event_count >= 1);
    assert(g_last_event_type == STA_EVT_ACTOR_CRASH);

    /* Root supervisor should still be alive (NOT TERMINATED). */
    uint32_t root_state = atomic_load_explicit(&g_vm->root_supervisor->state,
                                                 memory_order_acquire);
    assert(root_state != STA_ACTOR_TERMINATED);

    /* sup_b and its worker should be unaffected. */
    STA_ChildSpec *b_spec = sup_b->sup_data->children;
    assert(b_spec->current_actor == worker_b);

    sta_event_unregister(g_vm, event_counter, NULL);
    teardown();
    printf("  PASS: test_sub_supervisor_escalates_to_root\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Root supervisor intensity exceeded → fires event, resets, stays alive
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_root_intensity_exceeded(void) {
    setup();
    install_common_methods();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    g_event_count = 0;
    sta_event_register(g_vm, event_counter, NULL);

    /* Override root supervisor's max_restarts to something low for testing. */
    g_vm->root_supervisor->sup_data->max_restarts = 1;
    g_vm->root_supervisor->sup_data->max_seconds = 5;

    /* Create two sub-supervisors, each with low intensity. */
    struct STA_Actor *sup1 = sta_vm_spawn_supervised(
        g_vm, obj_cls, STA_RESTART_RESTART);
    assert(sup1);
    STA_ObjHeader *s1_obj = sta_heap_alloc(&sup1->heap, STA_CLS_OBJECT, 0);
    assert(s1_obj);
    sup1->behavior_obj = (STA_OOP)(uintptr_t)s1_obj;
    sta_supervisor_init(sup1, 0, 5);  /* max_restarts=0: first crash exceeds */

    struct STA_Actor *sup2 = sta_vm_spawn_supervised(
        g_vm, obj_cls, STA_RESTART_RESTART);
    assert(sup2);
    STA_ObjHeader *s2_obj = sta_heap_alloc(&sup2->heap, STA_CLS_OBJECT, 0);
    assert(s2_obj);
    sup2->behavior_obj = (STA_OOP)(uintptr_t)s2_obj;
    sta_supervisor_init(sup2, 0, 5);

    /* Add worker to sup1, crash it → sup1 exceeds → escalates to root. */
    struct STA_Actor *w1 = add_child(sup1, STA_RESTART_RESTART);
    crash_and_notify(w1, sup1);

    /* Process root notification → root RESTARTs sup1 (restart_count=1). */
    atomic_store_explicit(&g_vm->root_supervisor->state, STA_ACTOR_RUNNING,
                          memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, g_vm->root_supervisor);

    /* Now do same with sup2 → root restart_count=2 > max_restarts=1. */
    struct STA_Actor *w2 = add_child(sup2, STA_RESTART_RESTART);
    crash_and_notify(w2, sup2);

    /* Process root notification → intensity exceeded. */
    atomic_store_explicit(&g_vm->root_supervisor->state, STA_ACTOR_RUNNING,
                          memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, g_vm->root_supervisor);

    /* Root should have fired the "root intensity exceeded" event. */
    assert(g_event_count >= 1);
    /* Check for the intensity exceeded message. */
    int found_intensity = (strstr(g_last_event_msg, "intensity") != NULL);
    assert(found_intensity);

    /* Root supervisor must still be alive. */
    uint32_t root_state = atomic_load_explicit(&g_vm->root_supervisor->state,
                                                 memory_order_acquire);
    assert(root_state != STA_ACTOR_TERMINATED);

    /* Counters should have been reset. */
    assert(g_vm->root_supervisor->sup_data->restart_count == 0);

    sta_event_unregister(g_vm, event_counter, NULL);
    teardown();
    printf("  PASS: test_root_intensity_exceeded\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * sta_vm_destroy cleanly tears down entire tree (ASan validates)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_clean_teardown(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    /* Build a 3-level tree: root → sup → workers. */
    struct STA_Actor *sup = sta_vm_spawn_supervised(
        g_vm, obj_cls, STA_RESTART_RESTART);
    assert(sup);
    sta_supervisor_init(sup, 3, 5);

    add_child(sup, STA_RESTART_RESTART);
    add_child(sup, STA_RESTART_RESTART);
    add_child(sup, STA_RESTART_RESTART);

    /* Also spawn direct children of root. */
    sta_vm_spawn_supervised(g_vm, obj_cls, STA_RESTART_STOP);
    sta_vm_spawn_supervised(g_vm, obj_cls, STA_RESTART_RESTART);

    /* Teardown — ASan will catch leaks or double-frees. */
    teardown();
    printf("  PASS: test_clean_teardown\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Existing tests still pass (sta_vm_create/destroy with root supervisor)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_vm_create_destroy_idempotent(void) {
    /* Create and destroy multiple VMs to ensure no leaks across instances. */
    for (int i = 0; i < 3; i++) {
        STA_VMConfig cfg = {0};
        STA_VM *vm = sta_vm_create(&cfg);
        assert(vm != NULL);
        assert(vm->root_supervisor != NULL);
        sta_vm_destroy(vm);
    }
    printf("  PASS: test_vm_create_destroy_idempotent\n");
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_supervision_tree:\n");
    test_root_supervisor_exists();
    test_spawn_supervised();
    test_multi_level_tree();
    test_worker_crash_no_root_escalation();
    test_sub_supervisor_escalates_to_root();
    test_root_intensity_exceeded();
    test_clean_teardown();
    test_vm_create_destroy_idempotent();
    printf("All supervision tree tests passed.\n");
    return 0;
}
