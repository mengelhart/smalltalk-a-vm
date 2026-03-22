/* tests/test_gc_mailbox.c
 * Phase 2.5 Epic 0, Story 2: GC with queued mailbox messages.
 * Verifies that queued message OOPs survive GC and heap growth.
 * See #341.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#include "vm/oop.h"
#include "vm/heap.h"
#include "vm/format.h"
#include "vm/immutable_space.h"
#include "vm/class_table.h"
#include "vm/special_objects.h"
#include "vm/frame.h"
#include "vm/handler.h"
#include "vm/compiled_method.h"
#include "vm/interpreter.h"
#include "vm/vm_state.h"
#include "actor/actor.h"
#include "actor/mailbox.h"
#include "actor/mailbox_msg.h"
#include "gc/gc.h"

/* ── Test helpers ──────────────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(fn) do { \
    printf("  %-55s", #fn); \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
    tests_run++; \
} while (0)

/* Create a minimal VM for GC+mailbox testing (no kernel bootstrap).
 * Mirrors test_gc.c's create_test_vm. */
static STA_VM *create_test_vm(void) {
    STA_VM *vm = calloc(1, sizeof(STA_VM));
    assert(vm);

    assert(sta_immutable_space_init(&vm->immutable_space, 64 * 1024) == 0);
    sta_class_table_init(&vm->class_table);
    sta_special_objects_bind(vm->specials);
    sta_special_objects_init();

    /* nil, true, false */
    STA_ObjHeader *nil_h = sta_immutable_alloc(&vm->immutable_space,
                                                STA_CLS_UNDEFINEDOBJ, 0);
    STA_ObjHeader *true_h = sta_immutable_alloc(&vm->immutable_space,
                                                 STA_CLS_TRUE, 0);
    STA_ObjHeader *false_h = sta_immutable_alloc(&vm->immutable_space,
                                                  STA_CLS_FALSE, 0);
    vm->specials[SPC_NIL]   = (STA_OOP)(uintptr_t)nil_h;
    vm->specials[SPC_TRUE]  = (STA_OOP)(uintptr_t)true_h;
    vm->specials[SPC_FALSE] = (STA_OOP)(uintptr_t)false_h;

    /* Array class (9) — variable OOP, 0 instVars. */
    STA_ObjHeader *array_cls = sta_immutable_alloc(&vm->immutable_space,
                                                    STA_CLS_CLASS, 4);
    STA_OOP *cls_slots = sta_payload(array_cls);
    cls_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    cls_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    cls_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(0, STA_FMT_VARIABLE_OOP);
    cls_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_ARRAY,
                        (STA_OOP)(uintptr_t)array_cls);

    /* Object class (2) — normal, 0 instVars. */
    STA_ObjHeader *obj_cls = sta_immutable_alloc(&vm->immutable_space,
                                                  STA_CLS_CLASS, 4);
    STA_OOP *obj_cls_slots = sta_payload(obj_cls);
    obj_cls_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    obj_cls_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    obj_cls_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(0, STA_FMT_NORMAL);
    obj_cls_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_OBJECT,
                        (STA_OOP)(uintptr_t)obj_cls);

    /* Association class (14) — normal, 2 instVars. */
    STA_ObjHeader *assoc_cls = sta_immutable_alloc(&vm->immutable_space,
                                                    STA_CLS_CLASS, 4);
    STA_OOP *assoc_cls_slots = sta_payload(assoc_cls);
    assoc_cls_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    assoc_cls_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    assoc_cls_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(2, STA_FMT_NORMAL);
    assoc_cls_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_ASSOCIATION,
                        (STA_OOP)(uintptr_t)assoc_cls);

    /* String class (8) — byte-indexable, 0 instVars. */
    STA_ObjHeader *str_cls = sta_immutable_alloc(&vm->immutable_space,
                                                  STA_CLS_CLASS, 4);
    STA_OOP *str_cls_slots = sta_payload(str_cls);
    str_cls_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    str_cls_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    str_cls_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(0, STA_FMT_VARIABLE_BYTE);
    str_cls_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_STRING,
                        (STA_OOP)(uintptr_t)str_cls);

    return vm;
}

static void destroy_test_vm(STA_VM *vm) {
    sta_immutable_space_deinit(&vm->immutable_space);
    free(vm);
}

static struct STA_Actor *create_test_actor(STA_VM *vm, size_t heap_size) {
    struct STA_Actor *actor = sta_actor_create(vm, heap_size, 4096);
    assert(actor);
    sta_actor_register(actor);
    return actor;
}

static bool on_heap(STA_Heap *heap, STA_OOP oop) {
    if (STA_IS_IMMEDIATE(oop) || oop == 0) return false;
    uintptr_t addr = (uintptr_t)oop;
    uintptr_t base = (uintptr_t)heap->base;
    return addr >= base && addr < base + heap->capacity;
}

/* Allocate an Association on the given heap with SmallInt key/value. */
static STA_OOP make_assoc(STA_Heap *heap, int key, int value) {
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(h);
    sta_payload(h)[0] = STA_SMALLINT_OOP(key);
    sta_payload(h)[1] = STA_SMALLINT_OOP(value);
    return (STA_OOP)(uintptr_t)h;
}

/* Allocate an Array on the given heap, slots filled with SmallInts 1..count. */
static STA_OOP make_array(STA_Heap *heap, int count) {
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_ARRAY, (uint32_t)count);
    assert(h);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < count; i++)
        slots[i] = STA_SMALLINT_OOP(i + 1);
    return (STA_OOP)(uintptr_t)h;
}

/* Create a mailbox message with the given args (already on the actor's heap).
 * The args array is malloc'd (args_owned=true), matching production behavior.
 * selector is a SmallInt stand-in (immutable, GC-safe). */
static void enqueue_msg(STA_Mailbox *mb, STA_OOP *args, uint8_t nargs) {
    STA_OOP *copied = NULL;
    if (nargs > 0) {
        copied = malloc(nargs * sizeof(STA_OOP));
        assert(copied);
        memcpy(copied, args, nargs * sizeof(STA_OOP));
    }
    STA_MailboxMsg *msg = sta_mailbox_msg_create(
        STA_SMALLINT_OOP(1),  /* selector stand-in */
        copied, nargs, 0);
    assert(msg);
    msg->args_owned = (copied != NULL);
    int rc = sta_mailbox_enqueue(mb, msg);
    assert(rc == 0);
}

/* Fill the heap with garbage objects (unrooted, will be collected). */
static void fill_heap_with_garbage(STA_Heap *heap) {
    while (sta_heap_alloc(heap, STA_CLS_ARRAY, 2) != NULL) {
        /* keep allocating until heap is full */
    }
}

/* ── Test 1: Basic — queued message OOPs survive GC ───────────────────── */

static void test_gc_mailbox_basic(void) {
    STA_VM *vm = create_test_vm();
    /* 1024-byte heap: enough for several objects + room to fill with garbage. */
    struct STA_Actor *actor = create_test_actor(vm, 1024);
    STA_Heap *heap = &actor->heap;

    /* Allocate 5 Associations on the actor's heap. */
    STA_OOP objs[5];
    for (int i = 0; i < 5; i++) {
        objs[i] = make_assoc(heap, i * 10, i * 10 + 1);
    }

    /* Queue 5 messages, each with one mutable heap arg. */
    for (int i = 0; i < 5; i++) {
        STA_OOP args[1] = { objs[i] };
        enqueue_msg(&actor->mailbox, args, 1);
    }

    /* Fill the rest of the heap with garbage to force GC to do real work. */
    fill_heap_with_garbage(heap);

    /* Trigger GC. */
    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    /* Dequeue messages and verify each arg OOP is valid. */
    for (int i = 0; i < 5; i++) {
        STA_MailboxMsg *msg = sta_mailbox_dequeue(&actor->mailbox);
        assert(msg != NULL);
        assert(msg->arg_count == 1);

        STA_OOP arg = msg->args[0];
        assert(STA_IS_HEAP(arg));
        assert(on_heap(heap, arg));

        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)arg;
        assert(h->class_index == STA_CLS_ASSOCIATION);
        assert(h->size == 2);
        assert(sta_payload(h)[0] == STA_SMALLINT_OOP(i * 10));
        assert(sta_payload(h)[1] == STA_SMALLINT_OOP(i * 10 + 1));

        sta_mailbox_msg_destroy(msg);
    }

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 2: Immutable/immediate args unchanged after GC ──────────────── */

static void test_gc_mailbox_immutable_args(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 512);

    STA_OOP nil_oop = vm->specials[SPC_NIL];
    STA_OOP true_oop = vm->specials[SPC_TRUE];

    /* Create an immutable symbol-like object. */
    STA_ObjHeader *sym = sta_immutable_alloc(&vm->immutable_space,
                                              STA_CLS_SYMBOL, 1);
    STA_OOP sym_oop = (STA_OOP)(uintptr_t)sym;

    /* Message with all-immutable/immediate args. */
    STA_OOP args[4] = {
        STA_SMALLINT_OOP(42),
        nil_oop,
        true_oop,
        sym_oop,
    };
    enqueue_msg(&actor->mailbox, args, 4);

    /* GC should not touch these. */
    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&actor->mailbox);
    assert(msg != NULL);
    assert(msg->args[0] == STA_SMALLINT_OOP(42));
    assert(msg->args[1] == nil_oop);
    assert(msg->args[2] == true_oop);
    assert(msg->args[3] == sym_oop);

    sta_mailbox_msg_destroy(msg);
    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 3: Deep object graph in args survives GC ────────────────────── */

static void test_gc_mailbox_deep_graph(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 2048);
    STA_Heap *heap = &actor->heap;

    /* Build: Array[0] → Assoc(10,20), Array[1] → Assoc(30,40).
     * Two levels of reference: Array → Association → SmallInts. */
    STA_OOP assoc1 = make_assoc(heap, 10, 20);
    STA_OOP assoc2 = make_assoc(heap, 30, 40);

    STA_ObjHeader *arr_h = sta_heap_alloc(heap, STA_CLS_ARRAY, 2);
    assert(arr_h);
    sta_payload(arr_h)[0] = assoc1;
    sta_payload(arr_h)[1] = assoc2;
    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr_h;

    /* Queue message with the array as arg. */
    STA_OOP args[1] = { arr_oop };
    enqueue_msg(&actor->mailbox, args, 1);

    fill_heap_with_garbage(heap);

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    /* Verify the entire graph is intact and traversable. */
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&actor->mailbox);
    assert(msg != NULL);

    STA_OOP new_arr = msg->args[0];
    assert(STA_IS_HEAP(new_arr));
    assert(on_heap(heap, new_arr));

    STA_ObjHeader *new_arr_h = (STA_ObjHeader *)(uintptr_t)new_arr;
    assert(new_arr_h->class_index == STA_CLS_ARRAY);
    assert(new_arr_h->size == 2);

    /* Follow references to the Associations. */
    STA_OOP ref1 = sta_payload(new_arr_h)[0];
    STA_OOP ref2 = sta_payload(new_arr_h)[1];
    assert(STA_IS_HEAP(ref1));
    assert(STA_IS_HEAP(ref2));
    assert(on_heap(heap, ref1));
    assert(on_heap(heap, ref2));

    STA_ObjHeader *a1 = (STA_ObjHeader *)(uintptr_t)ref1;
    assert(a1->class_index == STA_CLS_ASSOCIATION);
    assert(sta_payload(a1)[0] == STA_SMALLINT_OOP(10));
    assert(sta_payload(a1)[1] == STA_SMALLINT_OOP(20));

    STA_ObjHeader *a2 = (STA_ObjHeader *)(uintptr_t)ref2;
    assert(a2->class_index == STA_CLS_ASSOCIATION);
    assert(sta_payload(a2)[0] == STA_SMALLINT_OOP(30));
    assert(sta_payload(a2)[1] == STA_SMALLINT_OOP(40));

    sta_mailbox_msg_destroy(msg);
    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 4: Multiple GC cycles with queued messages ──────────────────── */

static void test_gc_mailbox_multiple_cycles(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 1024);
    STA_Heap *heap = &actor->heap;

    /* Round 1: queue 2 messages, GC. */
    STA_OOP a1 = make_assoc(heap, 1, 2);
    STA_OOP a2 = make_assoc(heap, 3, 4);
    enqueue_msg(&actor->mailbox, &a1, 1);
    enqueue_msg(&actor->mailbox, &a2, 1);
    fill_heap_with_garbage(heap);
    assert(sta_gc_collect(vm, actor) == 0);

    /* Round 2: queue 2 more, GC again. */
    STA_OOP a3 = make_assoc(heap, 5, 6);
    STA_OOP a4 = make_assoc(heap, 7, 8);
    enqueue_msg(&actor->mailbox, &a3, 1);
    enqueue_msg(&actor->mailbox, &a4, 1);
    fill_heap_with_garbage(heap);
    assert(sta_gc_collect(vm, actor) == 0);

    /* Round 3: one more GC with no new messages. */
    fill_heap_with_garbage(heap);
    assert(sta_gc_collect(vm, actor) == 0);

    /* All 4 messages should survive with correct data. */
    int expected_keys[] = { 1, 3, 5, 7 };
    int expected_vals[] = { 2, 4, 6, 8 };
    for (int i = 0; i < 4; i++) {
        STA_MailboxMsg *msg = sta_mailbox_dequeue(&actor->mailbox);
        assert(msg != NULL);
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)msg->args[0];
        assert(h->class_index == STA_CLS_ASSOCIATION);
        assert(sta_payload(h)[0] == STA_SMALLINT_OOP(expected_keys[i]));
        assert(sta_payload(h)[1] == STA_SMALLINT_OOP(expected_vals[i]));
        sta_mailbox_msg_destroy(msg);
    }

    /* Queue should be empty now. */
    assert(sta_mailbox_dequeue(&actor->mailbox) == NULL);

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 5: Heap growth updates queued message OOPs ──────────────────── */

static void test_gc_mailbox_growth(void) {
    STA_VM *vm = create_test_vm();
    /* Start with a very small heap (256 bytes) so growth is easy to trigger. */
    struct STA_Actor *actor = create_test_actor(vm, 256);
    STA_Heap *heap = &actor->heap;

    /* Allocate an object and queue it. */
    STA_OOP assoc = make_assoc(heap, 77, 88);
    enqueue_msg(&actor->mailbox, &assoc, 1);

    /* Record the old heap region. */
    char *old_base = heap->base;
    size_t old_cap = heap->capacity;

    /* Fill heap to force GC, then growth on the retry allocation. */
    fill_heap_with_garbage(heap);

    /* Use sta_heap_alloc_gc which runs GC then grows if needed. */
    STA_ObjHeader *big = sta_heap_alloc_gc(vm, actor, STA_CLS_ARRAY, 10);
    assert(big != NULL);

    /* If the heap grew, it moved. Verify the queued message OOP was updated. */
    if (heap->base != old_base || heap->capacity != old_cap) {
        /* Heap moved — the queued arg must point into the NEW heap region. */
        STA_MailboxMsg *msg = sta_mailbox_dequeue(&actor->mailbox);
        assert(msg != NULL);

        STA_OOP arg = msg->args[0];
        assert(STA_IS_HEAP(arg));
        assert(on_heap(heap, arg));

        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)arg;
        assert(h->class_index == STA_CLS_ASSOCIATION);
        assert(sta_payload(h)[0] == STA_SMALLINT_OOP(77));
        assert(sta_payload(h)[1] == STA_SMALLINT_OOP(88));

        sta_mailbox_msg_destroy(msg);
    } else {
        /* Heap didn't move (GC freed enough). Still verify correctness. */
        STA_MailboxMsg *msg = sta_mailbox_dequeue(&actor->mailbox);
        assert(msg != NULL);
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)msg->args[0];
        assert(h->class_index == STA_CLS_ASSOCIATION);
        assert(sta_payload(h)[0] == STA_SMALLINT_OOP(77));
        assert(sta_payload(h)[1] == STA_SMALLINT_OOP(88));
        sta_mailbox_msg_destroy(msg);
    }

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 6: GC with empty mailbox — no crash ─────────────────────────── */

static void test_gc_mailbox_empty(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 256);
    STA_Heap *heap = &actor->heap;

    /* Allocate a rooted object so GC has something to do. */
    STA_ObjHeader *obj = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(obj);
    sta_payload(obj)[0] = STA_SMALLINT_OOP(1);
    sta_payload(obj)[1] = STA_SMALLINT_OOP(2);
    actor->behavior_obj = (STA_OOP)(uintptr_t)obj;

    fill_heap_with_garbage(heap);

    /* GC with empty mailbox should not crash. */
    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    /* Rooted object survived. */
    STA_ObjHeader *new_obj = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(sta_payload(new_obj)[0] == STA_SMALLINT_OOP(1));
    assert(sta_payload(new_obj)[1] == STA_SMALLINT_OOP(2));

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_gc_mailbox (Phase 2.5 Epic 0, Story 2):\n");

    RUN(test_gc_mailbox_basic);
    RUN(test_gc_mailbox_immutable_args);
    RUN(test_gc_mailbox_deep_graph);
    RUN(test_gc_mailbox_multiple_cycles);
    RUN(test_gc_mailbox_growth);
    RUN(test_gc_mailbox_empty);

    printf("\n  %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
