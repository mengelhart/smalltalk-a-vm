/* tests/test_restart.c
 * Phase 2 Epic 6, Story 3: Restart strategy — RESTART.
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
#include "scheduler/scheduler.h"
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
static struct STA_Actor *make_supervisor(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *sup = sta_actor_create(g_vm, 16384, 2048);
    assert(sup);
    sta_supervisor_init(sup, 3, 5);
    sup->behavior_class = obj_cls;
    STA_ObjHeader *obj_h = sta_heap_alloc(&sup->heap, STA_CLS_OBJECT, 0);
    assert(obj_h);
    sup->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    atomic_store_explicit(&sup->state, STA_ACTOR_SUSPENDED, memory_order_relaxed);
    return sup;
}

/* ── Test: crash → supervisor restarts child with new actor_id ────────── */

static void test_restart_new_actor(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");

    struct STA_Actor *sup = make_supervisor();

    /* Add a child with RESTART strategy. */
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls,
                                                        STA_RESTART_RESTART);
    assert(child);
    STA_ObjHeader *child_obj = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(child_obj);
    child->behavior_obj = (STA_OOP)(uintptr_t)child_obj;
    child->supervisor = sup;

    uint32_t old_id = child->actor_id;

    /* Send crash message and process it. */
    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&child->mailbox, msg);

    atomic_store_explicit(&child->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, child);
    assert(rc == STA_ACTOR_MSG_EXCEPTION);

    /* Supervisor should have the notification. Process it. */
    assert(!sta_mailbox_is_empty(&sup->mailbox));
    atomic_store_explicit(&sup->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    rc = sta_actor_process_one_preemptible(g_vm, sup);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Child spec should point to a NEW actor. */
    STA_ChildSpec *spec = sup->sup_data->children;
    assert(spec != NULL);
    assert(spec->current_actor != NULL);
    assert(spec->current_actor->actor_id != old_id);

    /* New actor should have an initialize message in its mailbox. */
    assert(!sta_mailbox_is_empty(&spec->current_actor->mailbox));

    sta_actor_terminate(sup);
    teardown();
    printf("  PASS: test_restart_new_actor\n");
}

/* ── Test: restarted actor has fresh heap ─────────────────────────────── */

static void test_restart_fresh_heap(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");

    struct STA_Actor *sup = make_supervisor();
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls,
                                                        STA_RESTART_RESTART);
    assert(child);
    STA_ObjHeader *child_obj = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(child_obj);
    child->behavior_obj = (STA_OOP)(uintptr_t)child_obj;

    /* Allocate some objects on the child's heap to dirty it. */
    sta_heap_alloc(&child->heap, STA_CLS_ARRAY, 5);
    sta_heap_alloc(&child->heap, STA_CLS_ARRAY, 10);
    size_t old_used = child->heap.used;
    assert(old_used > 0);

    /* Crash the child. */
    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&child->mailbox, msg);
    atomic_store_explicit(&child->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, child);

    /* Process supervisor notification → restart. */
    atomic_store_explicit(&sup->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, sup);

    /* New actor should have a fresh heap. */
    struct STA_Actor *new_child = sup->sup_data->children->current_actor;
    assert(new_child != NULL);
    /* Fresh heap should have minimal usage (just the behavior_obj). */
    assert(new_child->heap.used < old_used);

    sta_actor_terminate(sup);
    teardown();
    printf("  PASS: test_restart_fresh_heap\n");
}

/* ── Test: old actor is fully freed (ASan validates) ──────────────────── */

static void test_restart_old_actor_freed(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");

    struct STA_Actor *sup = make_supervisor();
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls,
                                                        STA_RESTART_RESTART);
    assert(child);
    STA_ObjHeader *child_obj = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(child_obj);
    child->behavior_obj = (STA_OOP)(uintptr_t)child_obj;

    /* Crash and restart. ASan will catch use-after-free if old actor leaks. */
    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&child->mailbox, msg);
    atomic_store_explicit(&child->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, child);

    atomic_store_explicit(&sup->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, sup);

    /* Verify new actor exists and is functional. */
    struct STA_Actor *new_child = sup->sup_data->children->current_actor;
    assert(new_child != NULL);

    sta_actor_terminate(sup);
    teardown();
    printf("  PASS: test_restart_old_actor_freed\n");
}

/* ── Test: restarted actor can receive and process messages ───────────── */

static void test_restart_processes_messages(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");
    install_method(obj_cls, "noop", "noop\n  ^self");

    struct STA_Actor *sup = make_supervisor();
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls,
                                                        STA_RESTART_RESTART);
    assert(child);
    STA_ObjHeader *child_obj = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(child_obj);
    child->behavior_obj = (STA_OOP)(uintptr_t)child_obj;

    /* Crash the child. */
    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&child->mailbox, msg);
    atomic_store_explicit(&child->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, child);

    /* Process restart. */
    atomic_store_explicit(&sup->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    sta_actor_process_one_preemptible(g_vm, sup);

    struct STA_Actor *new_child = sup->sup_data->children->current_actor;
    assert(new_child);

    /* Drain the #initialize message first. */
    atomic_store_explicit(&new_child->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, new_child);
    /* initialize may succeed or return error (Object doesn't have it) — either is fine. */
    (void)rc;

    /* Send a regular message to the new actor. */
    STA_OOP noop_sel = intern("noop");
    STA_MailboxMsg *noop_msg = sta_mailbox_msg_create(noop_sel, NULL, 0, 0);
    assert(noop_msg);
    sta_mailbox_enqueue(&new_child->mailbox, noop_msg);

    atomic_store_explicit(&new_child->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    rc = sta_actor_process_one_preemptible(g_vm, new_child);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    sta_actor_terminate(sup);
    teardown();
    printf("  PASS: test_restart_processes_messages\n");
}

/* ── Test: scheduler crash → restart → new actor scheduled ────────────── */

static void test_scheduler_restart(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash", "crash\n  Error new signal");
    install_method(obj_cls, "noop", "noop\n  ^self");

    struct STA_Actor *sup = make_supervisor();
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls,
                                                        STA_RESTART_RESTART);
    assert(child);
    STA_ObjHeader *child_obj = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(child_obj);
    child->behavior_obj = (STA_OOP)(uintptr_t)child_obj;
    atomic_store_explicit(&child->state, STA_ACTOR_SUSPENDED, memory_order_relaxed);

    /* Save the old actor_id — the old actor pointer becomes dangling after
     * the supervisor destroys it, and malloc may reuse the address. */
    uint32_t old_child_id = child->actor_id;

    /* Start scheduler. */
    sta_scheduler_init(g_vm, 2);
    sta_scheduler_start(g_vm);

    /* Send crash message. */
    STA_OOP crash_sel = intern("crash");
    sta_actor_send_msg(g_vm, g_vm->root_actor, child->actor_id, crash_sel, NULL, 0);

    /* Wait for the scheduler to process crash → notify → restart.
     * Sleep long enough, then stop the scheduler so all threads are joined
     * before we inspect shared state. No polling of child spec from the
     * main thread — that would race with the scheduler thread's writes. */
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 200000000 };  /* 200 ms */
    nanosleep(&ts, NULL);

    sta_scheduler_stop(g_vm);

    /* Scheduler stopped — all worker threads joined, safe to read. */
    STA_ChildSpec *spec = sup->sup_data->children;
    assert(spec != NULL);
    assert(spec->current_actor != NULL);
    assert(spec->current_actor->actor_id != old_child_id);

    sta_scheduler_destroy(g_vm);
    sta_actor_terminate(sup);
    teardown();
    printf("  PASS: test_scheduler_restart\n");
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_restart:\n");
    test_restart_new_actor();
    test_restart_fresh_heap();
    test_restart_old_actor_freed();
    test_restart_processes_messages();
    test_scheduler_restart();
    printf("All restart tests passed.\n");
    return 0;
}
