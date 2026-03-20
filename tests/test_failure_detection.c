/* tests/test_failure_detection.c
 * Phase 2 Epic 6, Story 2: Failure detection and notification.
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
#include "vm/compiled_method.h"
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

/* Create an actor with Object as its behavior class. */
static struct STA_Actor *make_actor(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *a = sta_actor_create(g_vm, 16384, 2048);
    assert(a != NULL);
    a->behavior_class = obj_cls;

    /* Allocate an Object instance on the actor's heap. */
    STA_ObjHeader *obj_h = sta_heap_alloc(&a->heap, STA_CLS_OBJECT, 0);
    assert(obj_h != NULL);
    a->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    a->actor_id = 42;
    atomic_store_explicit(&a->state, STA_ACTOR_READY, memory_order_relaxed);
    return a;
}

/* ── Test: actor crash transitions to TERMINATED ──────────────────────── */

static void test_crash_terminates_actor(void) {
    setup();

    /* Install a method that signals an unhandled Error. */
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash",
                   "crash\n  Error new signal");

    struct STA_Actor *a = make_actor();

    /* Send 'crash' message. */
    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&a->mailbox, msg);

    /* Process via preemptible path — should catch the exception. */
    atomic_store_explicit(&a->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, a);
    assert(rc == STA_ACTOR_MSG_EXCEPTION);

    /* Actor should be TERMINATED. */
    uint32_t state = atomic_load_explicit(&a->state, memory_order_relaxed);
    assert(state == STA_ACTOR_TERMINATED);

    sta_actor_destroy(a);
    teardown();
    printf("  PASS: test_crash_terminates_actor\n");
}

/* ── Test: supervisor receives childFailed:reason: ────────────────────── */

static void test_supervisor_receives_notification(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash",
                   "crash\n  Error new signal");

    /* Create supervisor. */
    struct STA_Actor *sup = sta_actor_create(g_vm, 16384, 2048);
    assert(sup);
    sta_supervisor_init(sup, 3, 5);
    sup->actor_id = 1;
    atomic_store_explicit(&sup->state, STA_ACTOR_SUSPENDED, memory_order_relaxed);

    /* Create worker with supervisor link. */
    struct STA_Actor *worker = make_actor();
    worker->supervisor = sup;
    worker->actor_id = 42;

    /* Send 'crash' to worker. */
    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&worker->mailbox, msg);

    /* Process — should crash and notify supervisor. */
    atomic_store_explicit(&worker->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, worker);
    assert(rc == STA_ACTOR_MSG_EXCEPTION);

    /* Supervisor should have a message in its mailbox. */
    assert(!sta_mailbox_is_empty(&sup->mailbox));

    STA_MailboxMsg *notif = sta_mailbox_dequeue(&sup->mailbox);
    assert(notif != NULL);

    /* Verify selector is childFailed:reason: */
    STA_OOP expected_sel = sta_spc_get(SPC_CHILD_FAILED_REASON);
    assert(notif->selector == expected_sel);

    /* Verify args: [0] = actor_id as SmallInt, [1] = error symbol */
    assert(notif->arg_count == 2);
    assert(notif->args != NULL);
    STA_OOP id_oop = notif->args[0];
    assert(STA_IS_SMALLINT(id_oop));
    assert(STA_SMALLINT_VAL(id_oop) == 42);

    /* args[1] should be a Symbol (the exception class name). */
    STA_OOP reason = notif->args[1];
    assert(STA_IS_HEAP(reason));
    STA_ObjHeader *reason_h = (STA_ObjHeader *)(uintptr_t)reason;
    assert(reason_h->class_index == STA_CLS_SYMBOL);

    sta_mailbox_msg_destroy(notif);
    /* Detach worker from sup to avoid double-free. */
    worker->supervisor = NULL;
    sta_actor_destroy(worker);
    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_supervisor_receives_notification\n");
}

/* ── Test: failed actor's remaining messages are drained ──────────────── */

static void test_drain_remaining_messages(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash",
                   "crash\n  Error new signal");
    install_method(obj_cls, "noop",
                   "noop\n  ^self");

    struct STA_Actor *a = make_actor();

    /* Enqueue 3 messages: crash, noop, noop. */
    STA_OOP crash_sel = intern("crash");
    STA_OOP noop_sel = intern("noop");

    STA_MailboxMsg *m1 = sta_mailbox_msg_create(crash_sel, NULL, 0, 0);
    STA_MailboxMsg *m2 = sta_mailbox_msg_create(noop_sel, NULL, 0, 0);
    STA_MailboxMsg *m3 = sta_mailbox_msg_create(noop_sel, NULL, 0, 0);
    assert(m1 && m2 && m3);

    sta_mailbox_enqueue(&a->mailbox, m1);
    sta_mailbox_enqueue(&a->mailbox, m2);
    sta_mailbox_enqueue(&a->mailbox, m3);

    assert(sta_mailbox_count(&a->mailbox) == 3);

    /* Process first message — should crash. */
    atomic_store_explicit(&a->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, a);
    assert(rc == STA_ACTOR_MSG_EXCEPTION);

    /* Remaining messages should be drained. */
    assert(sta_mailbox_is_empty(&a->mailbox));

    sta_actor_destroy(a);
    teardown();
    printf("  PASS: test_drain_remaining_messages\n");
}

/* ── Test: NULL supervisor — no crash, just logged ────────────────────── */

static void test_null_supervisor_no_crash(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash",
                   "crash\n  Error new signal");

    struct STA_Actor *a = make_actor();
    assert(a->supervisor == NULL);

    STA_OOP sel = intern("crash");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    assert(msg);
    sta_mailbox_enqueue(&a->mailbox, msg);

    /* Should not crash the process — just terminate the actor. */
    atomic_store_explicit(&a->state, STA_ACTOR_RUNNING, memory_order_relaxed);
    int rc = sta_actor_process_one_preemptible(g_vm, a);
    assert(rc == STA_ACTOR_MSG_EXCEPTION);

    uint32_t state = atomic_load_explicit(&a->state, memory_order_relaxed);
    assert(state == STA_ACTOR_TERMINATED);

    sta_actor_destroy(a);
    teardown();
    printf("  PASS: test_null_supervisor_no_crash\n");
}

/* ── Test: multi-threaded crash with scheduler ────────────────────────── */

static void test_scheduler_crash(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "crash",
                   "crash\n  Error new signal");

    /* Create supervisor. */
    struct STA_Actor *sup = sta_actor_create(g_vm, 16384, 2048);
    assert(sup);
    sta_supervisor_init(sup, 3, 5);
    sup->actor_id = 1;
    /* Supervisor needs behavior to be schedulable but won't process. */
    sup->behavior_class = obj_cls;
    STA_ObjHeader *sup_obj = sta_heap_alloc(&sup->heap, STA_CLS_OBJECT, 0);
    assert(sup_obj);
    sup->behavior_obj = (STA_OOP)(uintptr_t)sup_obj;
    /* Set to RUNNING so auto-schedule CAS in notify_supervisor fails.
     * This prevents the scheduler from processing the notification before
     * the test can inspect the supervisor's mailbox. */
    atomic_store_explicit(&sup->state, STA_ACTOR_RUNNING, memory_order_relaxed);

    /* Create worker. */
    struct STA_Actor *worker = make_actor();
    worker->supervisor = sup;
    worker->actor_id = 99;
    /* Set to SUSPENDED so auto-schedule CAS in send_msg will enqueue it. */
    atomic_store_explicit(&worker->state, STA_ACTOR_SUSPENDED, memory_order_relaxed);

    /* Start scheduler. */
    sta_scheduler_init(g_vm, 2);
    sta_scheduler_start(g_vm);

    /* Send crash message — auto-schedule should pick it up. */
    STA_OOP sel = intern("crash");
    sta_actor_send_msg(g_vm->root_actor, worker, sel, NULL, 0);

    /* Wait for worker to terminate. */
    for (int i = 0; i < 200; i++) {
        uint32_t s = atomic_load_explicit(&worker->state, memory_order_acquire);
        if (s == STA_ACTOR_TERMINATED) break;
        struct timespec ts = { .tv_nsec = 5000000 }; /* 5ms */
        nanosleep(&ts, NULL);
    }

    sta_scheduler_stop(g_vm);

    uint32_t ws = atomic_load_explicit(&worker->state, memory_order_relaxed);
    assert(ws == STA_ACTOR_TERMINATED);

    /* Supervisor should have the notification. */
    assert(!sta_mailbox_is_empty(&sup->mailbox));

    sta_scheduler_destroy(g_vm);
    worker->supervisor = NULL;
    sta_actor_destroy(worker);
    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_scheduler_crash\n");
}

/* ── Test: childFailed:reason: selector is interned ───────────────────── */

static void test_selector_interned(void) {
    setup();

    STA_OOP sel = sta_spc_get(SPC_CHILD_FAILED_REASON);
    assert(sel != 0);

    /* Verify it's a Symbol. */
    assert(STA_IS_HEAP(sel));
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)sel;
    assert(h->class_index == STA_CLS_SYMBOL);

    teardown();
    printf("  PASS: test_selector_interned\n");
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_failure_detection:\n");
    test_selector_interned();
    test_crash_terminates_actor();
    test_null_supervisor_no_crash();
    test_supervisor_receives_notification();
    test_drain_remaining_messages();
    test_scheduler_crash();
    printf("All failure detection tests passed.\n");
    return 0;
}
