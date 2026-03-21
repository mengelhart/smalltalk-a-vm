/* tests/test_stop_escalate.c
 * Phase 2 Epic 6, Story 4: STOP and ESCALATE restart strategies.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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
static struct STA_Actor *make_supervisor(uint32_t actor_id) {
    (void)actor_id;  /* IDs are now opaque, auto-assigned */
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *sup = sta_actor_create(g_vm, 16384, 2048);
    assert(sup);
    sta_supervisor_init(sup, 3, 5);
    sup->behavior_class = obj_cls;
    STA_ObjHeader *obj_h = sta_heap_alloc(&sup->heap, STA_CLS_OBJECT, 0);
    assert(obj_h);
    sup->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    atomic_store_explicit(&sup->state, STA_ACTOR_SUSPENDED, memory_order_relaxed);
    sta_actor_register(sup);
    return sup;
}

/* Helper: crash a child and process supervisor notification. */
static void crash_child(struct STA_Actor *child, struct STA_Actor *sup) {
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

/* ══════════════════════════════════════════════════════════════════════════
 * STOP tests
 * ══════════════════════════════════════════════════════════════════════════ */

/* STOP: crash → supervisor does NOT restart → current_actor == NULL */
static void test_stop_no_restart(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");

    struct STA_Actor *sup = make_supervisor(1);
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls,
                                                        STA_RESTART_STOP);
    assert(child);
    STA_ObjHeader *child_obj = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(child_obj);
    child->behavior_obj = (STA_OOP)(uintptr_t)child_obj;

    crash_child(child, sup);

    /* Child spec should have NULL actor — not restarted. */
    STA_ChildSpec *spec = sup->sup_data->children;
    assert(spec != NULL);
    assert(spec->current_actor == NULL);
    assert(spec->strategy == STA_RESTART_STOP);

    sta_actor_terminate(sup);
    teardown();
    printf("  PASS: test_stop_no_restart\n");
}

/* STOP child + RESTART child: RESTART still works after STOP. */
static void test_stop_other_children_restart(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");

    struct STA_Actor *sup = make_supervisor(1);

    /* Add RESTART child first, then STOP child.
     * Children are prepended, so STOP is at head. */
    struct STA_Actor *restart_child_actor = sta_supervisor_add_child(
        sup, obj_cls, STA_RESTART_RESTART);
    assert(restart_child_actor);
    STA_ObjHeader *r_obj = sta_heap_alloc(&restart_child_actor->heap,
                                            STA_CLS_OBJECT, 0);
    assert(r_obj);
    restart_child_actor->behavior_obj = (STA_OOP)(uintptr_t)r_obj;

    struct STA_Actor *stop_child_actor = sta_supervisor_add_child(
        sup, obj_cls, STA_RESTART_STOP);
    assert(stop_child_actor);
    STA_ObjHeader *s_obj = sta_heap_alloc(&stop_child_actor->heap,
                                            STA_CLS_OBJECT, 0);
    assert(s_obj);
    stop_child_actor->behavior_obj = (STA_OOP)(uintptr_t)s_obj;

    /* Crash the STOP child. */
    crash_child(stop_child_actor, sup);

    /* STOP child spec should be NULL. */
    STA_ChildSpec *stop_spec = sup->sup_data->children;  /* head = STOP */
    assert(stop_spec->strategy == STA_RESTART_STOP);
    assert(stop_spec->current_actor == NULL);

    /* Now crash the RESTART child. */
    uint32_t old_restart_id = restart_child_actor->actor_id;
    crash_child(restart_child_actor, sup);

    /* RESTART child spec should have a new actor. */
    STA_ChildSpec *restart_spec = stop_spec->next;
    assert(restart_spec->strategy == STA_RESTART_RESTART);
    assert(restart_spec->current_actor != NULL);
    assert(restart_spec->current_actor->actor_id != old_restart_id);

    sta_actor_terminate(sup);
    teardown();
    printf("  PASS: test_stop_other_children_restart\n");
}

/* STOP: destroyed actor is freed (ASan validates no leaks). */
static void test_stop_actor_freed(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");

    struct STA_Actor *sup = make_supervisor(1);
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls,
                                                        STA_RESTART_STOP);
    assert(child);
    STA_ObjHeader *child_obj = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(child_obj);
    child->behavior_obj = (STA_OOP)(uintptr_t)child_obj;

    /* Allocate some objects to make the heap non-trivial. */
    sta_heap_alloc(&child->heap, STA_CLS_ARRAY, 10);

    crash_child(child, sup);

    /* ASan will catch use-after-free or leaks. */
    assert(sup->sup_data->children->current_actor == NULL);

    sta_actor_terminate(sup);
    teardown();
    printf("  PASS: test_stop_actor_freed\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * ESCALATE tests
 * ══════════════════════════════════════════════════════════════════════════ */

/* Three-level tree: grandparent → parent → worker.
 * Worker crashes with ESCALATE on parent → parent forwards to grandparent. */
static void test_escalate_to_grandparent(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");

    /* Build tree: grandparent → parent → worker. */
    struct STA_Actor *gp = make_supervisor(1);
    struct STA_Actor *parent = sta_supervisor_add_child(gp, obj_cls,
                                                          STA_RESTART_RESTART);
    assert(parent);
    STA_ObjHeader *p_obj = sta_heap_alloc(&parent->heap, STA_CLS_OBJECT, 0);
    assert(p_obj);
    parent->behavior_obj = (STA_OOP)(uintptr_t)p_obj;

    /* Make parent a supervisor too. */
    sta_supervisor_init(parent, 3, 5);

    /* Add worker to parent with ESCALATE strategy. */
    struct STA_Actor *worker = sta_supervisor_add_child(parent, obj_cls,
                                                          STA_RESTART_ESCALATE);
    assert(worker);
    STA_ObjHeader *w_obj = sta_heap_alloc(&worker->heap, STA_CLS_OBJECT, 0);
    assert(w_obj);
    worker->behavior_obj = (STA_OOP)(uintptr_t)w_obj;

    /* Save parent's actor_id — parent pointer becomes dangling after
     * grandparent destroys it during RESTART. */
    uint32_t old_parent_id = parent->actor_id;

    /* Crash the worker — parent processes failure with ESCALATE. */
    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&worker->mailbox, msg);

    atomic_store_explicit(&worker->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, worker);
    assert(rc == STA_ACTOR_MSG_EXCEPTION);

    /* Parent should have notification from worker. Process it (ESCALATE). */
    assert(!sta_mailbox_is_empty(&parent->mailbox));
    atomic_store_explicit(&parent->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    rc = sta_actor_process_one_preemptible(g_vm, parent);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Parent should have destroyed the worker and set NULL. */
    STA_ChildSpec *parent_spec = parent->sup_data->children;
    assert(parent_spec->current_actor == NULL);

    /* Grandparent should now have a childFailed:reason: notification
     * with the PARENT's actor_id (not the worker's). */
    assert(!sta_mailbox_is_empty(&gp->mailbox));

    /* Process grandparent notification — grandparent RESTARTs the parent.
     * After this, the old parent is destroyed (dangling pointer). */
    atomic_store_explicit(&gp->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    rc = sta_actor_process_one_preemptible(g_vm, gp);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Grandparent's child spec for parent should have RESTART strategy.
     * Since we used RESTART, the grandparent should have created a new actor. */
    STA_ChildSpec *gp_spec = gp->sup_data->children;
    assert(gp_spec->strategy == STA_RESTART_RESTART);
    assert(gp_spec->current_actor != NULL);
    /* The new actor should have a different ID from the old parent. */
    assert(gp_spec->current_actor->actor_id != old_parent_id);

    sta_actor_terminate(gp);
    teardown();
    printf("  PASS: test_escalate_to_grandparent\n");
}

/* ESCALATE with no grandparent: log, no crash. */
static void test_escalate_no_grandparent(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");

    /* Supervisor with no supervisor of its own. */
    struct STA_Actor *sup = make_supervisor(1);
    assert(sup->supervisor == NULL);

    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls,
                                                        STA_RESTART_ESCALATE);
    assert(child);
    STA_ObjHeader *child_obj = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(child_obj);
    child->behavior_obj = (STA_OOP)(uintptr_t)child_obj;

    /* Crash the child — supervisor should ESCALATE but has no grandparent.
     * Should log and not crash. */
    crash_child(child, sup);

    /* Child should be destroyed. */
    STA_ChildSpec *spec = sup->sup_data->children;
    assert(spec->current_actor == NULL);

    sta_actor_terminate(sup);
    teardown();
    printf("  PASS: test_escalate_no_grandparent\n");
}

/* Grandparent can apply its own strategy (RESTART) to a supervisor
 * that escalated. */
static void test_escalate_grandparent_restarts_parent(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");
    install_method(obj_cls, "noop", "noop\n  ^self");

    /* Grandparent (RESTART strategy for parent) → Parent → Worker (ESCALATE). */
    struct STA_Actor *gp = make_supervisor(1);

    struct STA_Actor *parent = sta_supervisor_add_child(gp, obj_cls,
                                                          STA_RESTART_RESTART);
    assert(parent);
    STA_ObjHeader *p_obj = sta_heap_alloc(&parent->heap, STA_CLS_OBJECT, 0);
    assert(p_obj);
    parent->behavior_obj = (STA_OOP)(uintptr_t)p_obj;
    sta_supervisor_init(parent, 3, 5);

    struct STA_Actor *worker = sta_supervisor_add_child(parent, obj_cls,
                                                          STA_RESTART_ESCALATE);
    assert(worker);
    STA_ObjHeader *w_obj = sta_heap_alloc(&worker->heap, STA_CLS_OBJECT, 0);
    assert(w_obj);
    worker->behavior_obj = (STA_OOP)(uintptr_t)w_obj;

    uint32_t old_parent_id = parent->actor_id;

    /* Crash worker → parent ESCALATES → grandparent RESTARTS parent. */
    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&worker->mailbox, msg);

    atomic_store_explicit(&worker->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, worker);

    /* Parent processes ESCALATE. */
    atomic_store_explicit(&parent->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, parent);

    /* Grandparent processes and RESTARTs parent. */
    atomic_store_explicit(&gp->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, gp);

    /* Grandparent should have a new actor for the parent slot. */
    STA_ChildSpec *gp_spec = gp->sup_data->children;
    assert(gp_spec->current_actor != NULL);
    assert(gp_spec->current_actor->actor_id != old_parent_id);

    /* New parent should have #initialize in its mailbox. */
    assert(!sta_mailbox_is_empty(&gp_spec->current_actor->mailbox));

    sta_actor_terminate(gp);
    teardown();
    printf("  PASS: test_escalate_grandparent_restarts_parent\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Mixed strategies test
 * ══════════════════════════════════════════════════════════════════════════ */

/* Supervisor with 3 children: RESTART, STOP, ESCALATE.
 * Each crashes in sequence. Verify each strategy applied correctly. */
static void test_mixed_strategies(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");

    /* Grandparent to receive ESCALATE notifications. */
    struct STA_Actor *gp = make_supervisor(1);

    /* Parent supervisor with 3 children (different strategies). */
    struct STA_Actor *parent = sta_supervisor_add_child(gp, obj_cls,
                                                          STA_RESTART_RESTART);
    assert(parent);
    STA_ObjHeader *p_obj = sta_heap_alloc(&parent->heap, STA_CLS_OBJECT, 0);
    assert(p_obj);
    parent->behavior_obj = (STA_OOP)(uintptr_t)p_obj;
    sta_supervisor_init(parent, 3, 5);

    /* Add children in order: RESTART, STOP, ESCALATE.
     * Linked list is prepended, so order in list is: ESCALATE, STOP, RESTART. */
    struct STA_Actor *child_restart = sta_supervisor_add_child(
        parent, obj_cls, STA_RESTART_RESTART);
    assert(child_restart);
    STA_ObjHeader *cr_obj = sta_heap_alloc(&child_restart->heap,
                                             STA_CLS_OBJECT, 0);
    assert(cr_obj);
    child_restart->behavior_obj = (STA_OOP)(uintptr_t)cr_obj;

    struct STA_Actor *child_stop = sta_supervisor_add_child(
        parent, obj_cls, STA_RESTART_STOP);
    assert(child_stop);
    STA_ObjHeader *cs_obj = sta_heap_alloc(&child_stop->heap,
                                             STA_CLS_OBJECT, 0);
    assert(cs_obj);
    child_stop->behavior_obj = (STA_OOP)(uintptr_t)cs_obj;

    struct STA_Actor *child_escalate = sta_supervisor_add_child(
        parent, obj_cls, STA_RESTART_ESCALATE);
    assert(child_escalate);
    STA_ObjHeader *ce_obj = sta_heap_alloc(&child_escalate->heap,
                                             STA_CLS_OBJECT, 0);
    assert(ce_obj);
    child_escalate->behavior_obj = (STA_OOP)(uintptr_t)ce_obj;

    uint32_t old_restart_id = child_restart->actor_id;

    /* Walk the child spec list (prepended order: ESCALATE → STOP → RESTART). */
    STA_ChildSpec *escalate_spec = parent->sup_data->children;
    assert(escalate_spec->strategy == STA_RESTART_ESCALATE);
    STA_ChildSpec *stop_spec = escalate_spec->next;
    assert(stop_spec->strategy == STA_RESTART_STOP);
    STA_ChildSpec *restart_spec = stop_spec->next;
    assert(restart_spec->strategy == STA_RESTART_RESTART);

    /* ── 1. Crash RESTART child ──────────────────────────────────────── */
    crash_child(child_restart, parent);

    assert(restart_spec->current_actor != NULL);
    assert(restart_spec->current_actor->actor_id != old_restart_id);

    /* ── 2. Crash STOP child ─────────────────────────────────────────── */
    crash_child(child_stop, parent);

    assert(stop_spec->current_actor == NULL);

    /* Verify RESTART + STOP results before ESCALATE (which destroys parent). */
    assert(restart_spec->current_actor != NULL);
    assert(stop_spec->current_actor == NULL);
    assert(escalate_spec->current_actor == child_escalate);

    /* ── 3. Crash ESCALATE child ─────────────────────────────────────── */
    /* After this, the grandparent RESTARTs the parent, which destroys the
     * old parent and all its child specs. Do NOT reference them after. */
    uint32_t old_parent_id = parent->actor_id;

    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&child_escalate->mailbox, msg);

    atomic_store_explicit(&child_escalate->state, STA_ACTOR_RUNNING,
                          memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, child_escalate);
    assert(rc == STA_ACTOR_MSG_EXCEPTION);

    /* Parent processes ESCALATE notification → forwards to grandparent. */
    assert(!sta_mailbox_is_empty(&parent->mailbox));
    atomic_store_explicit(&parent->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    rc = sta_actor_process_one_preemptible(g_vm, parent);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Grandparent receives the escalated notification and RESTARTs parent.
     * This destroys the old parent actor (and its sup_data/child specs). */
    assert(!sta_mailbox_is_empty(&gp->mailbox));
    atomic_store_explicit(&gp->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    rc = sta_actor_process_one_preemptible(g_vm, gp);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Grandparent's child spec: old parent was restarted with new actor. */
    STA_ChildSpec *gp_spec = gp->sup_data->children;
    assert(gp_spec->current_actor != NULL);
    assert(gp_spec->current_actor->actor_id != old_parent_id);

    sta_actor_terminate(gp);
    teardown();
    printf("  PASS: test_mixed_strategies\n");
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_stop_escalate:\n");

    /* STOP tests */
    test_stop_no_restart();
    test_stop_other_children_restart();
    test_stop_actor_freed();

    /* ESCALATE tests */
    test_escalate_to_grandparent();
    test_escalate_no_grandparent();
    test_escalate_grandparent_restarts_parent();

    /* Mixed strategies */
    test_mixed_strategies();

    printf("All stop/escalate tests passed.\n");
    return 0;
}
