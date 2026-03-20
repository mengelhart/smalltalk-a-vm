/* tests/test_eval_integration.c
 * Phase 2 Epic 4 Story 6: sta_eval integration with scheduler.
 *
 * Verifies that sta_eval works correctly whether the scheduler is
 * running or not, and that concurrent sta_eval + scheduled actors
 * don't corrupt each other.
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

/* ── Test 1: sta_eval without scheduler (backward compat) ────────────── */

static void test_eval_no_scheduler(void) {
    STA_VM *vm = make_vm();

    /* No scheduler — sta_eval should work as always. */
    STA_Handle *r = sta_eval(vm, "3 + 4");
    assert(r != NULL);
    assert(strcmp(sta_inspect_cstring(vm, r), "7") == 0);
    sta_handle_release(vm, r);

    r = sta_eval(vm, "true ifTrue: [42] ifFalse: [0]");
    assert(r != NULL);
    assert(strcmp(sta_inspect_cstring(vm, r), "42") == 0);
    sta_handle_release(vm, r);

    sta_vm_destroy(vm);
    printf("  PASS: eval_no_scheduler\n");
}

/* ── Test 2: sta_eval with scheduler initialized but not started ──────── */

static void test_eval_scheduler_init_not_started(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    /* Scheduler initialized but not started. */

    STA_Handle *r = sta_eval(vm, "10 factorial");
    assert(r != NULL);
    assert(strcmp(sta_inspect_cstring(vm, r), "3628800") == 0);
    sta_handle_release(vm, r);

    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
    printf("  PASS: eval_scheduler_init_not_started\n");
}

/* ── Test 3: sta_eval with scheduler running (no actors) ─────────────── */

static void test_eval_scheduler_running_no_actors(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Scheduler running but no actors — sta_eval on main thread. */
    STA_Handle *r = sta_eval(vm, "3 + 4");
    assert(r != NULL);
    assert(strcmp(sta_inspect_cstring(vm, r), "7") == 0);
    sta_handle_release(vm, r);

    r = sta_eval(vm, "| i | i := 0. [i < 100] whileTrue: [i := i + 1]. i");
    assert(r != NULL);
    assert(strcmp(sta_inspect_cstring(vm, r), "100") == 0);
    sta_handle_release(vm, r);

    sta_scheduler_stop(vm);
    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
    printf("  PASS: eval_scheduler_running_no_actors\n");
}

/* ── Test 4: sta_eval concurrent with scheduled actors ───────────────── */

static void test_eval_concurrent_with_actors(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Start a long-running actor. */
    struct STA_Actor *child = make_child_actor(vm);
    child->actor_id = 500;
    install_method(vm, child,
        "work | i | i := 0. [i < 5000] whileTrue: [i := i + 1]. ^ i",
        "work");
    STA_OOP sel = intern(vm, "work");
    sta_actor_send_msg(vm->root_actor, child, sel, NULL, 0);

    /* While the actor is running, do sta_eval on the main thread. */
    for (int i = 0; i < 5; i++) {
        STA_Handle *r = sta_eval(vm, "3 + 4");
        assert(r != NULL);
        assert(strcmp(sta_inspect_cstring(vm, r), "7") == 0);
        sta_handle_release(vm, r);
    }

    /* Wait for actor to finish. */
    for (int i = 0; i < 200; i++) {
        if (atomic_load(&child->state) == STA_ACTOR_SUSPENDED) break;
        usleep(5000);
    }

    sta_scheduler_stop(vm);

    assert(atomic_load(&child->state) == STA_ACTOR_SUSPENDED);
    assert(sta_mailbox_is_empty(&child->mailbox));

    sta_scheduler_destroy(vm);
    sta_actor_destroy(child);
    sta_vm_destroy(vm);
    printf("  PASS: eval_concurrent_with_actors\n");
}

/* ── Test 5: multiple sta_eval calls with scheduler ──────────────────── */

static void test_multiple_evals_with_scheduler(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Multiple actors running. */
    struct STA_Actor *actors[3];
    STA_OOP sel = intern(vm, "yourself");
    for (int i = 0; i < 3; i++) {
        actors[i] = make_child_actor(vm);
        actors[i]->actor_id = (uint32_t)(600 + i);
        for (int j = 0; j < 5; j++) {
            sta_actor_send_msg(vm->root_actor, actors[i], sel, NULL, 0);
        }
    }

    /* Interleave sta_eval calls. */
    const char *exprs[] = {
        "1 + 1", "2 * 3", "10 - 7", "4 + 5", "100 // 10"
    };
    const char *expected[] = {
        "2", "6", "3", "9", "10"
    };
    for (int i = 0; i < 5; i++) {
        STA_Handle *r = sta_eval(vm, exprs[i]);
        assert(r != NULL);
        const char *s = sta_inspect_cstring(vm, r);
        assert(strcmp(s, expected[i]) == 0);
        sta_handle_release(vm, r);
    }

    /* Wait for actors. */
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 200; j++) {
            if (atomic_load(&actors[i]->state) == STA_ACTOR_SUSPENDED) break;
            usleep(1000);
        }
    }

    sta_scheduler_stop(vm);

    for (int i = 0; i < 3; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
    }

    sta_scheduler_destroy(vm);
    for (int i = 0; i < 3; i++) {
        sta_actor_destroy(actors[i]);
    }
    sta_vm_destroy(vm);
    printf("  PASS: multiple_evals_with_scheduler\n");
}

/* ── Test 6: root actor is not scheduled ─────────────────────────────── */

static void test_root_actor_not_scheduled(void) {
    STA_VM *vm = make_vm();

    int rc = sta_scheduler_init(vm, 2);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Root actor should be in READY state (set during vm_create). */
    uint32_t state = atomic_load(&vm->root_actor->state);
    assert(state == STA_ACTOR_READY);

    /* Sending a message to root actor should NOT auto-schedule it,
     * because root_actor is for sta_eval, not for the scheduler. */
    /* Actually, the auto-schedule CAS checks CREATED/SUSPENDED.
     * Root actor is READY, so CAS fails — it won't be enqueued. */

    /* sta_eval works fine. */
    STA_Handle *r = sta_eval(vm, "42");
    assert(r != NULL);
    assert(strcmp(sta_inspect_cstring(vm, r), "42") == 0);
    sta_handle_release(vm, r);

    /* Root actor state unchanged. */
    state = atomic_load(&vm->root_actor->state);
    assert(state == STA_ACTOR_READY);

    usleep(10000);  /* Let scheduler threads spin briefly. */

    sta_scheduler_stop(vm);
    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
    printf("  PASS: root_actor_not_scheduled\n");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_eval_integration (Epic 4 Story 6):\n");

    test_eval_no_scheduler();
    test_eval_scheduler_init_not_started();
    test_eval_scheduler_running_no_actors();
    test_eval_concurrent_with_actors();
    test_multiple_evals_with_scheduler();
    test_root_actor_not_scheduled();

    printf("All eval integration tests passed.\n");
    return 0;
}
