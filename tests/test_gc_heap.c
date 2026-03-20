/* tests/test_gc_heap.c
 * Phase 2 Epic 5, Story 2: Heap integration — GC triggers on allocation failure.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

static STA_VM *create_test_vm(void) {
    STA_VM *vm = calloc(1, sizeof(STA_VM));
    assert(vm);
    assert(sta_immutable_space_init(&vm->immutable_space, 64 * 1024) == 0);
    sta_class_table_init(&vm->class_table);
    sta_special_objects_bind(vm->specials);
    sta_special_objects_init();

    STA_ObjHeader *nil_h = sta_immutable_alloc(&vm->immutable_space,
                                                STA_CLS_UNDEFINEDOBJ, 0);
    STA_ObjHeader *true_h = sta_immutable_alloc(&vm->immutable_space,
                                                 STA_CLS_TRUE, 0);
    STA_ObjHeader *false_h = sta_immutable_alloc(&vm->immutable_space,
                                                  STA_CLS_FALSE, 0);
    vm->specials[SPC_NIL]   = (STA_OOP)(uintptr_t)nil_h;
    vm->specials[SPC_TRUE]  = (STA_OOP)(uintptr_t)true_h;
    vm->specials[SPC_FALSE] = (STA_OOP)(uintptr_t)false_h;

    /* Array class — variable OOP, 0 instVars. */
    STA_ObjHeader *array_cls = sta_immutable_alloc(&vm->immutable_space,
                                                    STA_CLS_CLASS, 4);
    STA_OOP *cls_slots = sta_payload(array_cls);
    cls_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    cls_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    cls_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(0, STA_FMT_VARIABLE_OOP);
    cls_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_ARRAY,
                        (STA_OOP)(uintptr_t)array_cls);

    /* Association class — normal, 2 instVars. */
    STA_ObjHeader *assoc_cls = sta_immutable_alloc(&vm->immutable_space,
                                                    STA_CLS_CLASS, 4);
    STA_OOP *assoc_slots = sta_payload(assoc_cls);
    assoc_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    assoc_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    assoc_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(2, STA_FMT_NORMAL);
    assoc_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_ASSOCIATION,
                        (STA_OOP)(uintptr_t)assoc_cls);

    /* Object class — normal, 0 instVars. */
    STA_ObjHeader *obj_cls = sta_immutable_alloc(&vm->immutable_space,
                                                  STA_CLS_CLASS, 4);
    STA_OOP *obj_slots = sta_payload(obj_cls);
    obj_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    obj_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    obj_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(0, STA_FMT_NORMAL);
    obj_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_OBJECT,
                        (STA_OOP)(uintptr_t)obj_cls);

    return vm;
}

static void destroy_test_vm(STA_VM *vm) {
    sta_immutable_space_deinit(&vm->immutable_space);
    free(vm);
}

/* ── Test 1: GC triggers automatically on allocation failure ───────────── */

static void test_alloc_gc_triggers(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = sta_actor_create(vm, 128, 4096);
    assert(actor);
    STA_Heap *heap = &actor->heap;

    /* Allocate until the heap is full using plain sta_heap_alloc. */
    STA_ObjHeader *live = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(live);
    sta_payload(live)[0] = STA_SMALLINT_OOP(42);
    sta_payload(live)[1] = STA_SMALLINT_OOP(99);
    actor->behavior_obj = (STA_OOP)(uintptr_t)live;

    /* Fill with garbage. */
    while (sta_heap_alloc(heap, STA_CLS_ARRAY, 1)) {}

    /* Heap is now full. Normal alloc fails. */
    assert(sta_heap_alloc(heap, STA_CLS_ARRAY, 1) == NULL);

    /* GC-aware alloc should trigger GC and succeed. */
    STA_ObjHeader *new_obj = sta_heap_alloc_gc(vm, actor,
                                                STA_CLS_ARRAY, 1);
    assert(new_obj != NULL);
    assert(new_obj->class_index == STA_CLS_ARRAY);
    assert(new_obj->size == 1);

    /* The live object should still be accessible via behavior_obj. */
    STA_ObjHeader *surviving = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(sta_payload(surviving)[0] == STA_SMALLINT_OOP(42));
    assert(sta_payload(surviving)[1] == STA_SMALLINT_OOP(99));

    sta_actor_destroy(actor);
    destroy_test_vm(vm);
}

/* ── Test 2: Repeated fill → GC → fill cycles ─────────────────────────── */

static void test_repeated_gc_cycles(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = sta_actor_create(vm, 256, 4096);
    assert(actor);

    /* Root: an Association that we keep updating. */
    STA_ObjHeader *root = sta_heap_alloc(&actor->heap, STA_CLS_ASSOCIATION, 2);
    assert(root);
    sta_payload(root)[0] = STA_SMALLINT_OOP(0);
    sta_payload(root)[1] = STA_SMALLINT_OOP(0);
    actor->behavior_obj = (STA_OOP)(uintptr_t)root;

    for (int cycle = 1; cycle <= 5; cycle++) {
        /* Fill with garbage. */
        while (sta_heap_alloc(&actor->heap, STA_CLS_ARRAY, 1)) {}

        /* Allocate via GC-aware path. */
        STA_ObjHeader *obj = sta_heap_alloc_gc(vm, actor,
                                                STA_CLS_ASSOCIATION, 2);
        assert(obj);
        sta_payload(obj)[0] = STA_SMALLINT_OOP(cycle);
        sta_payload(obj)[1] = STA_SMALLINT_OOP(cycle * 10);

        /* Update root to reference the new object. */
        STA_ObjHeader *r = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
        sta_payload(r)[0] = (STA_OOP)(uintptr_t)obj;
        sta_payload(r)[1] = STA_SMALLINT_OOP(cycle);
    }

    /* Verify the chain is intact after 5 GC cycles. */
    STA_ObjHeader *r = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(sta_payload(r)[1] == STA_SMALLINT_OOP(5));
    STA_OOP child_oop = sta_payload(r)[0];
    assert(STA_IS_HEAP(child_oop));
    STA_ObjHeader *child = (STA_ObjHeader *)(uintptr_t)child_oop;
    assert(sta_payload(child)[0] == STA_SMALLINT_OOP(5));
    assert(sta_payload(child)[1] == STA_SMALLINT_OOP(50));

    sta_actor_destroy(actor);
    destroy_test_vm(vm);
}

/* ── Test 3: Heap growth — many long-lived objects trigger growth ───────── */

static void test_heap_growth(void) {
    STA_VM *vm = create_test_vm();
    /* Start with a tiny 128-byte heap. */
    struct STA_Actor *actor = sta_actor_create(vm, 128, 4096);
    assert(actor);

    size_t initial_capacity = actor->heap.capacity;

    /* Build a chain of live objects that exceeds the initial heap size.
     * Each Array(2) is 32 bytes. 128 / 32 = 4 objects max.
     * We'll create 8 nodes to force growth.
     *
     * Structure: behavior_obj → head (Array[1])
     *            head[0] → latest node
     *            node[0] = SmallInt(i), node[1] → previous node
     *
     * GC safety: after each sta_heap_alloc_gc, ALL C locals pointing
     * into the heap are stale. Re-read from the rooted graph. */
    STA_ObjHeader *chain_head = sta_heap_alloc_gc(vm, actor,
                                                   STA_CLS_ARRAY, 1);
    assert(chain_head);
    sta_payload(chain_head)[0] = STA_SMALLINT_OOP(0);
    actor->behavior_obj = (STA_OOP)(uintptr_t)chain_head;

    for (int i = 1; i <= 8; i++) {
        /* Re-read the current "latest node" from the rooted graph
         * BEFORE allocating, so we can pass it to the new node. */
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
        STA_OOP prev_oop = sta_payload(h)[0];

        STA_ObjHeader *node = sta_heap_alloc_gc(vm, actor,
                                                 STA_CLS_ARRAY, 2);
        assert(node);

        /* Re-read prev_oop from rooted graph — it may have moved during GC. */
        h = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
        prev_oop = sta_payload(h)[0];

        sta_payload(node)[0] = STA_SMALLINT_OOP(i);
        sta_payload(node)[1] = prev_oop;

        /* Update head to point to this new node. */
        sta_payload(h)[0] = (STA_OOP)(uintptr_t)node;
    }

    /* Heap should have grown. */
    assert(actor->heap.capacity > initial_capacity);

    /* Walk the chain backwards to verify integrity. */
    STA_ObjHeader *head = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    STA_OOP node_oop = sta_payload(head)[0];
    int count = 0;
    while (STA_IS_HEAP(node_oop) && node_oop != 0) {
        STA_ObjHeader *n = (STA_ObjHeader *)(uintptr_t)node_oop;
        assert(n->class_index == STA_CLS_ARRAY);
        count++;
        if (n->size < 2) break;
        node_oop = sta_payload(n)[1];
    }
    assert(count == 8);

    sta_actor_destroy(actor);
    destroy_test_vm(vm);
}

/* ── Test 4: GC-aware alloc succeeds when heap not full (no GC needed) ── */

static void test_alloc_gc_no_gc_needed(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = sta_actor_create(vm, 1024, 4096);
    assert(actor);

    size_t used_before = actor->heap.used;

    /* Heap has plenty of space — should allocate without GC. */
    STA_ObjHeader *obj = sta_heap_alloc_gc(vm, actor,
                                            STA_CLS_ASSOCIATION, 2);
    assert(obj);
    assert(obj->class_index == STA_CLS_ASSOCIATION);
    assert(actor->heap.used == used_before + 32);

    sta_actor_destroy(actor);
    destroy_test_vm(vm);
}

/* ── Test 5: Heap never shrinks below initial size ─────────────────────── */

static void test_heap_min_size(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = sta_actor_create(vm, 128, 4096);
    assert(actor);

    /* GC on nearly empty heap — should keep 128 byte capacity. */
    STA_ObjHeader *obj = sta_heap_alloc(&actor->heap, STA_CLS_ARRAY, 1);
    assert(obj);
    actor->behavior_obj = (STA_OOP)(uintptr_t)obj;

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);
    assert(actor->heap.capacity >= 128);

    sta_actor_destroy(actor);
    destroy_test_vm(vm);
}

/* ── Test 6: References survive through growth ─────────────────────────── */

static void test_refs_survive_growth(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = sta_actor_create(vm, 128, 4096);
    assert(actor);

    /* Create an object, root it, then force growth by allocating
     * more live data than fits in 128 bytes. */
    STA_ObjHeader *parent = sta_heap_alloc_gc(vm, actor, STA_CLS_ARRAY, 3);
    assert(parent);
    sta_payload(parent)[0] = STA_SMALLINT_OOP(111);
    sta_payload(parent)[1] = STA_SMALLINT_OOP(222);
    sta_payload(parent)[2] = vm->specials[SPC_NIL];  /* immutable ref */
    actor->behavior_obj = (STA_OOP)(uintptr_t)parent;

    /* Allocate children that force the heap beyond 128 bytes. */
    STA_ObjHeader *child1 = sta_heap_alloc_gc(vm, actor, STA_CLS_ARRAY, 2);
    assert(child1);
    sta_payload(child1)[0] = STA_SMALLINT_OOP(333);
    sta_payload(child1)[1] = STA_SMALLINT_OOP(444);

    /* Re-read parent (may have moved during GC/grow). */
    parent = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    sta_payload(parent)[2] = (STA_OOP)(uintptr_t)child1;

    STA_ObjHeader *child2 = sta_heap_alloc_gc(vm, actor, STA_CLS_ARRAY, 2);
    assert(child2);
    sta_payload(child2)[0] = STA_SMALLINT_OOP(555);
    sta_payload(child2)[1] = STA_SMALLINT_OOP(666);

    /* Re-read parent again. */
    parent = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    sta_payload(parent)[1] = (STA_OOP)(uintptr_t)child2;

    /* Force a final GC to compact. */
    while (sta_heap_alloc(&actor->heap, STA_CLS_ARRAY, 1)) {}
    STA_ObjHeader *final = sta_heap_alloc_gc(vm, actor, STA_CLS_ARRAY, 1);
    assert(final);

    /* Verify the entire tree survived. */
    parent = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(sta_payload(parent)[0] == STA_SMALLINT_OOP(111));

    STA_OOP c2_oop = sta_payload(parent)[1];
    assert(STA_IS_HEAP(c2_oop));
    STA_ObjHeader *c2 = (STA_ObjHeader *)(uintptr_t)c2_oop;
    assert(sta_payload(c2)[0] == STA_SMALLINT_OOP(555));
    assert(sta_payload(c2)[1] == STA_SMALLINT_OOP(666));

    STA_OOP c1_oop = sta_payload(parent)[2];
    assert(STA_IS_HEAP(c1_oop));
    STA_ObjHeader *c1 = (STA_ObjHeader *)(uintptr_t)c1_oop;
    assert(sta_payload(c1)[0] == STA_SMALLINT_OOP(333));
    assert(sta_payload(c1)[1] == STA_SMALLINT_OOP(444));

    sta_actor_destroy(actor);
    destroy_test_vm(vm);
}

/* ── Test 7: Multiple actors — GC on one doesn't affect the other ──────── */

static void test_independent_actor_gc(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *a1 = sta_actor_create(vm, 256, 4096);
    struct STA_Actor *a2 = sta_actor_create(vm, 256, 4096);
    assert(a1 && a2);

    /* Put objects on both heaps. */
    STA_ObjHeader *obj1 = sta_heap_alloc(&a1->heap, STA_CLS_ASSOCIATION, 2);
    assert(obj1);
    sta_payload(obj1)[0] = STA_SMALLINT_OOP(1);
    sta_payload(obj1)[1] = STA_SMALLINT_OOP(2);
    a1->behavior_obj = (STA_OOP)(uintptr_t)obj1;

    STA_ObjHeader *obj2 = sta_heap_alloc(&a2->heap, STA_CLS_ASSOCIATION, 2);
    assert(obj2);
    sta_payload(obj2)[0] = STA_SMALLINT_OOP(3);
    sta_payload(obj2)[1] = STA_SMALLINT_OOP(4);
    a2->behavior_obj = (STA_OOP)(uintptr_t)obj2;

    /* Fill a1 and GC it. */
    while (sta_heap_alloc(&a1->heap, STA_CLS_ARRAY, 1)) {}
    int rc = sta_gc_collect(vm, a1);
    assert(rc == 0);

    /* a1's object moved. */
    STA_ObjHeader *new1 = (STA_ObjHeader *)(uintptr_t)a1->behavior_obj;
    assert(sta_payload(new1)[0] == STA_SMALLINT_OOP(1));

    /* a2's object is untouched (same address). */
    assert(a2->behavior_obj == (STA_OOP)(uintptr_t)obj2);
    assert(sta_payload(obj2)[0] == STA_SMALLINT_OOP(3));

    sta_actor_destroy(a1);
    sta_actor_destroy(a2);
    destroy_test_vm(vm);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_gc_heap (Phase 2 Epic 5, Story 2):\n");

    RUN(test_alloc_gc_triggers);
    RUN(test_repeated_gc_cycles);
    RUN(test_heap_growth);
    RUN(test_alloc_gc_no_gc_needed);
    RUN(test_heap_min_size);
    RUN(test_refs_survive_growth);
    RUN(test_independent_actor_gc);

    printf("\n  %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
