/* tests/test_actor_dispatch.c
 * Unit tests for sta_actor_process_one — Phase 2 Epic 3 Story 5.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "actor/actor.h"
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
#include <sta/vm.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(fn) do { \
    printf("  %-55s", #fn); \
    tests_run++; \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

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

/* Compile and install a method on a class.
 * selector_str is the method selector (e.g. "ping", "setValue:"). */
static void install_method(STA_OOP cls, const char *selector_str,
                            const char *source,
                            const char **ivars, uint32_t ivar_count) {
    STA_CompileResult r = sta_compile_method(
        source, cls, ivars, ivar_count,
        &g_vm->symbol_table, &g_vm->immutable_space,
        &g_vm->root_actor->heap,
        g_vm->specials[SPC_SMALLTALK]);
    if (r.had_error) {
        fprintf(stderr, "compile error: %s\nsource: %s\n", r.error_msg, source);
    }
    assert(!r.had_error);
    assert(r.method != 0);

    STA_OOP md = sta_class_method_dict(cls);
    assert(md != 0);

    STA_OOP selector = intern(selector_str);
    sta_method_dict_insert(&g_vm->root_actor->heap, md, selector, r.method);
}

/* Create an actor with a given class as its behavior. Allocate a
 * behavior_obj (instance of that class) on the actor's heap. */
static struct STA_Actor *make_behavior_actor(STA_OOP cls, uint32_t inst_vars) {
    struct STA_Actor *a = sta_actor_create(g_vm, 16384, 2048);
    assert(a != NULL);
    a->behavior_class = cls;

    /* Allocate an instance of cls on the actor's heap. */
    uint32_t cls_idx = sta_class_table_index_of(&g_vm->class_table, cls);
    assert(cls_idx != 0);
    STA_ObjHeader *obj_h = sta_heap_alloc(&a->heap, cls_idx, inst_vars);
    assert(obj_h != NULL);
    /* Initialize all instVars to nil. */
    STA_OOP nil_oop = g_vm->specials[SPC_NIL];
    STA_OOP *slots = sta_payload(obj_h);
    for (uint32_t i = 0; i < inst_vars; i++)
        slots[i] = nil_oop;

    a->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    a->state = STA_ACTOR_READY;
    sta_actor_register(a);
    return a;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

static void test_process_empty_returns_zero(void) {
    /* Create actor with Object as behavior. */
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *a = make_behavior_actor(obj_cls, 0);

    int rc = sta_actor_process_one(g_vm, a);
    assert(rc == 0);  /* no message */

    sta_actor_terminate(a);
}

static void test_process_simple_method(void) {
    /* Use Object class. Install a method that returns self. */
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    install_method(obj_cls, "ping", "ping ^self", NULL, 0);

    struct STA_Actor *a = make_behavior_actor(obj_cls, 0);

    /* Send a message from the root actor. */
    STA_OOP sel = intern("ping");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    sta_mailbox_enqueue(&a->mailbox, msg);

    int rc = sta_actor_process_one(g_vm, a);
    assert(rc == 1);  /* message processed */

    /* Mailbox should be empty now. */
    assert(sta_mailbox_is_empty(&a->mailbox));

    sta_actor_terminate(a);
}

static void test_process_with_args(void) {
    /* Use Association class (has key and value instVars). */
    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ASSOCIATION);
    const char *ivars[] = {"key", "value"};

    /* Install a method: setValue: anObj → sets value instVar. */
    install_method(assoc_cls, "setValue:", "setValue: anObj value := anObj", ivars, 2);

    struct STA_Actor *a = make_behavior_actor(assoc_cls, 2);

    /* Send setValue: 42 */
    STA_OOP sel = intern("setValue:");
    STA_OOP arg = STA_SMALLINT_OOP(42);

    /* Allocate the args on the actor's heap (simulating deep copy). */
    STA_ObjHeader *args_h = sta_heap_alloc(&a->heap, STA_CLS_ARRAY, 1);
    sta_payload(args_h)[0] = arg;

    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, sta_payload(args_h), 1, 0);
    sta_mailbox_enqueue(&a->mailbox, msg);

    int rc = sta_actor_process_one(g_vm, a);
    assert(rc == 1);

    /* Verify: the behavior_obj's value instVar should be 42. */
    STA_ObjHeader *beh_h = (STA_ObjHeader *)(uintptr_t)a->behavior_obj;
    assert(sta_payload(beh_h)[1] == STA_SMALLINT_OOP(42));  /* slot 1 = value */

    sta_actor_terminate(a);
}

static void test_process_fifo_order(void) {
    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ASSOCIATION);
    const char *ivars[] = {"key", "value"};

    /* Install setKey: */
    install_method(assoc_cls, "setKey:", "setKey: anObj key := anObj", ivars, 2);
    /* setValue: already installed from previous test. */

    struct STA_Actor *a = make_behavior_actor(assoc_cls, 2);

    /* Send setKey: 1, then setValue: 2 */
    STA_OOP sel_key = intern("setKey:");
    STA_OOP sel_val = intern("setValue:");

    STA_ObjHeader *args1_h = sta_heap_alloc(&a->heap, STA_CLS_ARRAY, 1);
    sta_payload(args1_h)[0] = STA_SMALLINT_OOP(1);
    STA_MailboxMsg *msg1 = sta_mailbox_msg_create(sel_key, sta_payload(args1_h), 1, 0);
    sta_mailbox_enqueue(&a->mailbox, msg1);

    STA_ObjHeader *args2_h = sta_heap_alloc(&a->heap, STA_CLS_ARRAY, 1);
    sta_payload(args2_h)[0] = STA_SMALLINT_OOP(2);
    STA_MailboxMsg *msg2 = sta_mailbox_msg_create(sel_val, sta_payload(args2_h), 1, 0);
    sta_mailbox_enqueue(&a->mailbox, msg2);

    /* Process in order. */
    assert(sta_actor_process_one(g_vm, a) == 1);
    assert(sta_actor_process_one(g_vm, a) == 1);
    assert(sta_actor_process_one(g_vm, a) == 0);  /* empty */

    /* Check state: key=1, value=2. */
    STA_ObjHeader *beh_h = (STA_ObjHeader *)(uintptr_t)a->behavior_obj;
    assert(sta_payload(beh_h)[0] == STA_SMALLINT_OOP(1));
    assert(sta_payload(beh_h)[1] == STA_SMALLINT_OOP(2));

    sta_actor_terminate(a);
}

static void test_process_state_mutation(void) {
    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ASSOCIATION);
    const char *ivars[] = {"key", "value"};

    /* Install increment: adds arg to value. */
    install_method(assoc_cls, "increment:",
        "increment: n value := value + n",
        ivars, 2);

    struct STA_Actor *a = make_behavior_actor(assoc_cls, 2);

    /* Set initial value to 10. */
    STA_ObjHeader *beh_h = (STA_ObjHeader *)(uintptr_t)a->behavior_obj;
    sta_payload(beh_h)[1] = STA_SMALLINT_OOP(10);

    /* Send increment: 5 three times. */
    STA_OOP sel = intern("increment:");
    for (int i = 0; i < 3; i++) {
        STA_ObjHeader *args_h = sta_heap_alloc(&a->heap, STA_CLS_ARRAY, 1);
        sta_payload(args_h)[0] = STA_SMALLINT_OOP(5);
        STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, sta_payload(args_h), 1, 0);
        sta_mailbox_enqueue(&a->mailbox, msg);
    }

    for (int i = 0; i < 3; i++) {
        assert(sta_actor_process_one(g_vm, a) == 1);
    }

    /* value should be 10 + 5 + 5 + 5 = 25. */
    assert(sta_payload(beh_h)[1] == STA_SMALLINT_OOP(25));

    sta_actor_terminate(a);
}

static void test_method_not_found_returns_error(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *a = make_behavior_actor(obj_cls, 0);

    /* Send a selector that doesn't exist. */
    STA_OOP sel = intern("nonExistentMethod");
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, NULL, 0, 0);
    sta_mailbox_enqueue(&a->mailbox, msg);

    int rc = sta_actor_process_one(g_vm, a);
    assert(rc == -1);  /* method not found */

    sta_actor_terminate(a);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_actor_dispatch:\n");
    setup();

    RUN(test_process_empty_returns_zero);
    RUN(test_process_simple_method);
    RUN(test_process_with_args);
    RUN(test_process_fifo_order);
    RUN(test_process_state_mutation);
    RUN(test_method_not_found_returns_error);

    teardown();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
