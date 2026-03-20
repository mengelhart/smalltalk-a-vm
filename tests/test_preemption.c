/* tests/test_preemption.c
 * Phase 2 Epic 4 Story 3: Reduction-based preemption.
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

/* Compile a method and install it on the actor's behavior class.
 * selector_name: the method's selector as a C string. */
static void install_method(STA_VM *vm, struct STA_Actor *actor,
                           const char *src, const char *selector_name) {
    STA_OOP cls = actor->behavior_class;
    STA_OOP md = sta_class_method_dict(cls);

    /* If no method dict, create one. */
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
    if (cr.had_error) {
        fprintf(stderr, "Compile error: %s\n", cr.error_msg);
    }
    assert(cr.method != 0);

    STA_OOP sel = sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table,
        selector_name, strlen(selector_name));
    assert(sel != 0);

    int rc = sta_method_dict_insert(&vm->root_actor->heap, md, sel, cr.method);
    assert(rc == 0);
}

/* Look up a method on the actor's behavior class hierarchy. */
static STA_OOP lookup_method(STA_VM *vm, struct STA_Actor *actor,
                              const char *name) {
    STA_OOP sel = sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, name, strlen(name));
    STA_ObjHeader *beh_h = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    uint32_t cls_idx = beh_h->class_index;
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP cls = sta_class_table_get(&vm->class_table, cls_idx);
    while (cls != 0 && cls != nil_oop) {
        STA_OOP md = sta_class_method_dict(cls);
        if (md != 0) {
            STA_OOP method = sta_method_dict_lookup(md, sel);
            if (method != 0) return method;
        }
        cls = sta_class_superclass(cls);
    }
    return 0;
}

/* ── Test 1: interpret_actor preempts on long loop ───────────────────── */

static void test_preempt_long_loop(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *child = make_child_actor(vm);

    install_method(vm, child,
        "longLoop\n"
        "  | i |\n"
        "  i := 0.\n"
        "  [i < 5000] whileTrue: [i := i + 1].\n"
        "  ^ i",
        "longLoop");

    STA_OOP method = lookup_method(vm, child, "longLoop");
    assert(method != 0);

    /* Run with preemption. */
    int rc = sta_interpret_actor(vm, child, method, child->behavior_obj,
                                  NULL, 0);
    /* With 5000 iterations and REDUCTION_QUOTA=1000, should preempt. */
    assert(rc == STA_INTERPRET_PREEMPTED);
    assert(child->saved_frame != NULL);

    /* Resume until completion. */
    int preemptions = 1;
    while (rc == STA_INTERPRET_PREEMPTED) {
        rc = sta_interpret_resume(vm, child);
        if (rc == STA_INTERPRET_PREEMPTED) preemptions++;
    }
    assert(rc == STA_INTERPRET_COMPLETED);
    assert(child->saved_frame == NULL);
    assert(preemptions >= 2);  /* should preempt multiple times */

    printf("  PASS: preempt_long_loop (preemptions=%d)\n", preemptions);

    sta_actor_destroy(child);
    sta_vm_destroy(vm);
}

/* ── Test 2: short method completes without preemption ───────────────── */

static void test_short_method_no_preempt(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *child = make_child_actor(vm);

    /* Install a trivial method. */
    install_method(vm, child, "quick ^ 42", "quick");

    STA_OOP method = lookup_method(vm, child, "quick");
    assert(method != 0);

    int rc = sta_interpret_actor(vm, child, method, child->behavior_obj,
                                  NULL, 0);
    assert(rc == STA_INTERPRET_COMPLETED);
    assert(child->saved_frame == NULL);

    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: short_method_no_preempt\n");
}

/* ── Test 3: process_one_preemptible handles preemption ──────────────── */

static void test_process_one_preemptible(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *child = make_child_actor(vm);

    install_method(vm, child,
        "longLoop\n"
        "  | i |\n"
        "  i := 0.\n"
        "  [i < 5000] whileTrue: [i := i + 1].\n"
        "  ^ i",
        "longLoop");

    /* Send a longLoop message. */
    STA_OOP sel = sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, "longLoop", 8);
    struct STA_Actor *root = vm->root_actor;
    sta_actor_send_msg(root, child, sel, NULL, 0);

    /* Process with preemption. */
    int rc = sta_actor_process_one_preemptible(vm, child);
    assert(rc == STA_ACTOR_MSG_PREEMPTED);
    assert(child->saved_frame != NULL);

    /* Resume until done. */
    int preemptions = 1;
    while (rc == STA_ACTOR_MSG_PREEMPTED) {
        rc = sta_actor_process_one_preemptible(vm, child);
        if (rc == STA_ACTOR_MSG_PREEMPTED) preemptions++;
    }
    assert(rc == STA_ACTOR_MSG_PROCESSED);
    assert(child->saved_frame == NULL);

    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: process_one_preemptible (preemptions=%d)\n", preemptions);
}

/* ── Test 4: scheduler preempts and re-enqueues ──────────────────────── */

static void test_scheduler_preemption(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 1);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    struct STA_Actor *child = make_child_actor(vm);
    child->actor_id = 99;

    install_method(vm, child,
        "longLoop\n"
        "  | i |\n"
        "  i := 0.\n"
        "  [i < 5000] whileTrue: [i := i + 1].\n"
        "  ^ i",
        "longLoop");

    /* Send message — auto-schedules. */
    STA_OOP sel = sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, "longLoop", 8);
    struct STA_Actor *root = vm->root_actor;
    sta_actor_send_msg(root, child, sel, NULL, 0);

    /* Wait for completion — only check atomic state to avoid TSan races
     * on non-atomic saved_frame. saved_frame is checked after stop (which
     * joins all threads, establishing happens-before). */
    for (int i = 0; i < 200; i++) {
        if (atomic_load(&child->state) == STA_ACTOR_SUSPENDED) break;
        usleep(5000);
    }

    sta_scheduler_stop(vm);

    /* After stop/join, all worker threads are done — safe to read. */
    assert(sta_mailbox_is_empty(&child->mailbox));
    assert(child->saved_frame == NULL);
    uint32_t state = atomic_load(&child->state);
    assert(state == STA_ACTOR_SUSPENDED);

    /* Verify the scheduler ran the actor multiple times (preemption
     * caused re-enqueue). */
    assert(vm->scheduler->workers[0].actors_run >= 2);

    printf("  PASS: scheduler_preemption (actors_run=%llu)\n",
           (unsigned long long)vm->scheduler->workers[0].actors_run);

    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
}

/* ── Test 5: sta_eval is NOT preempted (backward compat) ─────────────── */

static void test_sta_eval_not_preempted(void) {
    STA_VM *vm = make_vm();

    /* Run a long computation via sta_eval — should complete without
     * preemption since sta_eval uses the non-preemptible path. */
    STA_Handle *result = sta_eval(vm,
        "| i | i := 0. [i < 5000] whileTrue: [i := i + 1]. i");
    assert(result != NULL);

    const char *s = sta_inspect_cstring(vm, result);
    assert(strcmp(s, "5000") == 0);

    sta_handle_release(vm, result);
    sta_vm_destroy(vm);
    printf("  PASS: sta_eval_not_preempted\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_preemption (Epic 4 Story 3):\n");

    test_preempt_long_loop();
    test_short_method_no_preempt();
    test_process_one_preemptible();
    test_scheduler_preemption();
    test_sta_eval_not_preempted();

    printf("All preemption tests passed.\n");
    return 0;
}
