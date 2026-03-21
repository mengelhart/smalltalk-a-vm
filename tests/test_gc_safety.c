/* tests/test_gc_safety.c
 * Phase 2 Epic 5, Story 4: GC safety audit — verify all allocation
 * sites are safe under GC pressure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "actor/actor.h"
#include "actor/mailbox.h"
#include "actor/mailbox_msg.h"
#include "actor/deep_copy.h"
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
    sta_actor_register(a);
    return a;
}

static void send_msg(struct STA_Actor *actor, const char *sel_name,
                      STA_OOP *args, uint8_t nargs) {
    STA_OOP sel = intern(sel_name);
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, args, nargs, 0);
    assert(msg);
    assert(sta_mailbox_enqueue(&actor->mailbox, msg) == 0);
}

/* ── Test 1: GC during basicNew primitive (prim 31) ────────────────────── */

static void test_gc_during_basicNew(void) {
    setup();

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    /* Method that calls basicNew in a loop to trigger GC. */
    install_method(obj_cls, "manyNew",
        "manyNew\n"
        "  | a b c d e f g h |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  d := Object new.\n"
        "  e := Object new.\n"
        "  f := Object new.\n"
        "  g := Object new.\n"
        "  h := Object new.\n"
        "  ^h",
        NULL, 0);

    /* Tiny heap — forces GC multiple times during the 8 allocations. */
    struct STA_Actor *actor = make_gc_actor(obj_cls, 0, 128);

    send_msg(actor, "manyNew", NULL, 0);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    sta_actor_terminate(actor);
    teardown();
}

/* ── Test 2: GC during shallowCopy (prim 41) ──────────────────────────── */

static void test_gc_during_shallowCopy(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    install_method(assoc_cls, "copyMany",
        "copyMany\n"
        "  | a b c d |\n"
        "  a := self shallowCopy.\n"
        "  b := self shallowCopy.\n"
        "  c := self shallowCopy.\n"
        "  d := self shallowCopy.\n"
        "  ^d",
        (const char *[]){"key", "value"}, 2);

    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 256);

    send_msg(actor, "copyMany", NULL, 0);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    sta_actor_terminate(actor);
    teardown();
}

/* ── Test 3: Deep copy with large payload to small-heap actor ──────────── */

static void test_gc_during_deep_copy(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Simple method that stores args. */
    install_method(assoc_cls, "setKey:",
        "setKey: v\n"
        "  key := v",
        (const char *[]){"key", "value"}, 2);

    /* Create a sender actor with a large object graph. */
    struct STA_Actor *sender = make_gc_actor(assoc_cls, 2, 4096);

    /* Create a complex mutable object on the sender's heap:
     * an Array holding several Associations. */
    STA_ObjHeader *arr = sta_heap_alloc(&sender->heap, STA_CLS_ARRAY, 5);
    assert(arr);
    for (int i = 0; i < 5; i++) {
        STA_ObjHeader *a = sta_heap_alloc(&sender->heap, STA_CLS_ASSOCIATION, 2);
        assert(a);
        sta_payload(a)[0] = STA_SMALLINT_OOP(i * 10);
        sta_payload(a)[1] = STA_SMALLINT_OOP(i * 10 + 1);
        sta_payload(arr)[i] = (STA_OOP)(uintptr_t)a;
    }

    /* Heap sized for the deep copy (5 Associations + 1 Array + behavior_obj
     * + args array ≈ 288 bytes). 512 gives headroom. Deep copy heap growth
     * is deferred — see GitHub #295. This test verifies GC works during
     * the actor's subsequent message processing, not mid-deep-copy growth. */
    struct STA_Actor *receiver = make_gc_actor(assoc_cls, 2, 512);

    /* Send message with the array as argument.
     * sta_actor_send_msg deep-copies args to receiver's heap. */
    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr;
    int rc = sta_actor_send_msg(g_vm, sender, receiver->actor_id,
                                 intern("setKey:"), &arr_oop, 1);
    assert(rc == 0);

    /* Process the message. */
    rc = sta_actor_process_one(g_vm, receiver);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* Verify the receiver's key holds a valid copied array. */
    STA_ObjHeader *beh = (STA_ObjHeader *)(uintptr_t)receiver->behavior_obj;
    STA_OOP key_val = sta_payload(beh)[0];
    assert(STA_IS_HEAP(key_val));
    STA_ObjHeader *copied_arr = (STA_ObjHeader *)(uintptr_t)key_val;
    assert(copied_arr->class_index == STA_CLS_ARRAY);
    assert(copied_arr->size == 5);

    /* Check first and last elements. */
    STA_OOP first = sta_payload(copied_arr)[0];
    assert(STA_IS_HEAP(first));
    STA_ObjHeader *first_h = (STA_ObjHeader *)(uintptr_t)first;
    assert(sta_payload(first_h)[0] == STA_SMALLINT_OOP(0));

    STA_OOP last = sta_payload(copied_arr)[4];
    assert(STA_IS_HEAP(last));
    STA_ObjHeader *last_h = (STA_ObjHeader *)(uintptr_t)last;
    assert(sta_payload(last_h)[0] == STA_SMALLINT_OOP(40));

    sta_actor_terminate(sender);
    sta_actor_terminate(receiver);
    teardown();
}

/* ── Test 4: GC during closure allocation (OP_CLOSURE_COPY) ────────────── */

static void test_gc_during_closure(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Method with a capturing closure that forces allocation. */
    install_method(assoc_cls, "closureGC",
        "closureGC\n"
        "  | sum i |\n"
        "  sum := 0.\n"
        "  i := 0.\n"
        "  [i < 10] whileTrue: [\n"
        "    sum := sum + i.\n"
        "    key := Object new.\n"
        "    i := i + 1\n"
        "  ].\n"
        "  ^sum",
        (const char *[]){"key", "value"}, 2);

    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 256);

    send_msg(actor, "closureGC", NULL, 0);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    sta_actor_terminate(actor);
    teardown();
}

/* ── Test 5: Sustained GC pressure — many allocations across methods ──── */

static void test_gc_sustained_pressure(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Method that allocates heavily — Array new: forces basicNew:. */
    install_method(assoc_cls, "pressure",
        "pressure\n"
        "  | a b c d e f g h |\n"
        "  a := Array new: 3.\n"
        "  b := Array new: 3.\n"
        "  c := Array new: 3.\n"
        "  d := Array new: 3.\n"
        "  e := Array new: 3.\n"
        "  f := Array new: 3.\n"
        "  g := Array new: 3.\n"
        "  h := Array new: 3.\n"
        "  key := h.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    /* Tiny heap — forces multiple GC cycles during the 8 Array allocs. */
    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 256);

    send_msg(actor, "pressure", NULL, 0);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* key should hold the last Array. */
    STA_ObjHeader *beh = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    STA_OOP key_val = sta_payload(beh)[0];
    assert(STA_IS_HEAP(key_val));
    STA_ObjHeader *arr = (STA_ObjHeader *)(uintptr_t)key_val;
    assert(arr->class_index == STA_CLS_ARRAY);
    assert(arr->size == 3);

    sta_actor_terminate(actor);
    teardown();
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_gc_safety (Phase 2 Epic 5, Story 4):\n");

    RUN(test_gc_during_basicNew);
    RUN(test_gc_during_shallowCopy);
    RUN(test_gc_during_deep_copy);
    RUN(test_gc_during_closure);
    RUN(test_gc_sustained_pressure);

    printf("\n  %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
