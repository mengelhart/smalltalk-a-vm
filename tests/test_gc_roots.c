/* tests/test_gc_roots.c
 * Phase 2 Epic 5, Story 3: Root enumeration correctness under GC.
 * Tests GC during interpreter execution with real Smalltalk methods.
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
#include "gc/gc.h"
#include <sta/vm.h>

/* ── Test helpers ──────────────────────────────────────────────────────── */

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

/* Create an actor with a tiny heap so GC triggers easily.
 * heap_size should be small (e.g. 256-512 bytes). */
static struct STA_Actor *make_gc_actor(STA_OOP cls, uint32_t inst_vars,
                                        size_t heap_size) {
    struct STA_Actor *a = sta_actor_create(g_vm, heap_size, 4096);
    assert(a != NULL);
    a->behavior_class = cls;

    uint32_t cls_idx = sta_class_table_index_of(&g_vm->class_table, cls);
    assert(cls_idx != 0);
    STA_ObjHeader *obj_h = sta_heap_alloc(&a->heap, cls_idx, inst_vars);
    assert(obj_h != NULL);
    STA_OOP nil_oop = g_vm->specials[SPC_NIL];
    STA_OOP *slots = sta_payload(obj_h);
    for (uint32_t i = 0; i < inst_vars; i++)
        slots[i] = nil_oop;

    a->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    a->state = STA_ACTOR_READY;
    return a;
}

static void send_msg(struct STA_Actor *actor, const char *sel_name,
                      STA_OOP *args, uint8_t nargs) {
    STA_OOP sel = intern(sel_name);
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, args, nargs, 0);
    assert(msg);
    int rc = sta_mailbox_enqueue(&actor->mailbox, msg);
    assert(rc == 0);
}

/* ── Test 1: Allocating method forces GC, frame locals survive ─────────── */

static void test_gc_during_allocation(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    /* Method that allocates multiple objects — on a tiny heap this
     * will trigger GC via basicNew. */
    install_method(obj_cls, "allocMany",
        "allocMany\n"
        "  | a b c d |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  d := Object new.\n"
        "  ^d",
        NULL, 0);

    /* Actor with 256-byte heap — 4 Objects = 64 bytes for headers,
     * plus the behavior_obj. Should need GC. */
    struct STA_Actor *actor = make_gc_actor(obj_cls, 0, 256);

    send_msg(actor, "allocMany", NULL, 0);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    sta_actor_terminate(actor);
    teardown();
}

/* ── Test 2: Deep call stack (recursive method), GC during execution ───── */

static void test_gc_deep_stack(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    const char *ivars[] = {"count"};

    /* Use Association class which has key/value instVars.
     * Install a method that recurses, allocating at each level. */
    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    install_method(assoc_cls, "countDown:",
        "countDown: n\n"
        "  | obj |\n"
        "  n = 0 ifTrue: [^self].\n"
        "  obj := Object new.\n"
        "  self countDown: n - 1.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    /* Small heap — recursive allocation + frame overhead will force GC. */
    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 512);

    STA_OOP arg = STA_SMALLINT_OOP(10);
    send_msg(actor, "countDown:", &arg, 1);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    sta_actor_terminate(actor);
    teardown();
}

/* ── Test 3: Actor retains state across GC cycles ──────────────────────── */

static void test_gc_state_retained(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Store a value, allocate garbage, then read the value back. */
    install_method(assoc_cls, "setKey:",
        "setKey: v\n"
        "  key := v",
        (const char *[]){"key", "value"}, 2);

    install_method(assoc_cls, "getKey",
        "getKey\n"
        "  ^key",
        (const char *[]){"key", "value"}, 2);

    install_method(assoc_cls, "allocGarbage",
        "allocGarbage\n"
        "  | a b c d e f |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  d := Object new.\n"
        "  e := Object new.\n"
        "  f := Object new.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 256);

    /* Set key to 42. */
    STA_OOP arg = STA_SMALLINT_OOP(42);
    send_msg(actor, "setKey:", &arg, 1);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Allocate lots of garbage (forces GC). */
    send_msg(actor, "allocGarbage", NULL, 0);
    rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Verify key is still 42. The behavior_obj (Association) survived GC. */
    STA_ObjHeader *beh = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    STA_OOP key_val = sta_payload(beh)[0];  /* slot 0 = key */
    assert(key_val == STA_SMALLINT_OOP(42));

    sta_actor_terminate(actor);
    teardown();
}

/* ── Test 4: Multiple messages, each triggering GC ─────────────────────── */

static void test_gc_across_messages(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    install_method(assoc_cls, "increment",
        "increment\n"
        "  | garbage |\n"
        "  garbage := Object new.\n"
        "  garbage := Object new.\n"
        "  garbage := Object new.\n"
        "  key := key + 1",
        (const char *[]){"key", "value"}, 2);

    install_method(assoc_cls, "setKey:",
        "setKey: v\n"
        "  key := v",
        (const char *[]){"key", "value"}, 2);

    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 256);

    /* Initialize key to 0. */
    STA_OOP zero = STA_SMALLINT_OOP(0);
    send_msg(actor, "setKey:", &zero, 1);
    sta_actor_process_one(g_vm, actor);

    /* Send 10 increment messages. Each allocates garbage and may trigger GC. */
    for (int i = 0; i < 10; i++) {
        send_msg(actor, "increment", NULL, 0);
        int rc = sta_actor_process_one(g_vm, actor);
        assert(rc == STA_ACTOR_MSG_PROCESSED);
    }

    /* key should be 10. */
    STA_ObjHeader *beh = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    STA_OOP key_val = sta_payload(beh)[0];
    assert(key_val == STA_SMALLINT_OOP(10));

    sta_actor_terminate(actor);
    teardown();
}

/* ── Test 5: Expression stack values survive GC ────────────────────────── */

static void test_gc_expression_stack(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    /* Method that allocates several objects and then does arithmetic.
     * The intermediate values on the expression stack must survive GC. */
    install_method(obj_cls, "addResults",
        "addResults\n"
        "  | a b c |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  ^3 + 4 + 5",
        NULL, 0);

    /* 3 + 4 + 5 = 12. The Object new calls force GC pressure. */
    struct STA_Actor *actor = make_gc_actor(obj_cls, 0, 256);

    send_msg(actor, "addResults", NULL, 0);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    sta_actor_terminate(actor);
    teardown();
}

/* ── Test 6: GC with loop iteration (whileTrue:) ──────────────────────── */

static void test_gc_loop_allocation(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Loop that allocates an object each iteration.
     * On a tiny heap, GC will trigger multiple times. */
    install_method(assoc_cls, "allocLoop:",
        "allocLoop: n\n"
        "  | i |\n"
        "  i := 0.\n"
        "  [i < n] whileTrue: [\n"
        "    key := Object new.\n"
        "    i := i + 1\n"
        "  ].\n"
        "  ^i",
        (const char *[]){"key", "value"}, 2);

    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 256);

    STA_OOP arg = STA_SMALLINT_OOP(20);
    send_msg(actor, "allocLoop:", &arg, 1);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* After 20 iterations of allocating+GC, the actor should still be fine. */
    sta_actor_terminate(actor);
    teardown();
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_gc_roots (Phase 2 Epic 5, Story 3):\n");

    RUN(test_gc_during_allocation);
    RUN(test_gc_deep_stack);
    RUN(test_gc_state_retained);
    RUN(test_gc_across_messages);
    RUN(test_gc_expression_stack);
    RUN(test_gc_loop_allocation);

    printf("\n  %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
