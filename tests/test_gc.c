/* tests/test_gc.c
 * Phase 2 Epic 5, Story 1: Per-actor GC infrastructure tests.
 * Tests the Cheney semi-space copying collector in isolation.
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

/* Create a minimal VM+actor for GC testing.
 * The actor gets a small heap (heap_size bytes) so we can fill it quickly. */
static STA_VM *create_test_vm(void) {
    STA_VM *vm = calloc(1, sizeof(STA_VM));
    assert(vm);

    /* Immutable space for nil/true/false and class objects. */
    assert(sta_immutable_space_init(&vm->immutable_space, 64 * 1024) == 0);

    /* Class table. */
    sta_class_table_init(&vm->class_table);

    /* Special objects — bind to VM. */
    sta_special_objects_bind(vm->specials);
    sta_special_objects_init();

    /* Allocate nil, true, false in immutable space. */
    STA_ObjHeader *nil_h = sta_immutable_alloc(&vm->immutable_space,
                                                STA_CLS_UNDEFINEDOBJ, 0);
    STA_ObjHeader *true_h = sta_immutable_alloc(&vm->immutable_space,
                                                 STA_CLS_TRUE, 0);
    STA_ObjHeader *false_h = sta_immutable_alloc(&vm->immutable_space,
                                                  STA_CLS_FALSE, 0);
    vm->specials[SPC_NIL]   = (STA_OOP)(uintptr_t)nil_h;
    vm->specials[SPC_TRUE]  = (STA_OOP)(uintptr_t)true_h;
    vm->specials[SPC_FALSE] = (STA_OOP)(uintptr_t)false_h;

    /* Create a minimal class for Array (class_index 9) so format queries work.
     * Class object has 4 slots: superclass, methodDict, format, name.
     * format: STA_FMT_VARIABLE_OOP, 0 instVars. */
    STA_ObjHeader *array_cls = sta_immutable_alloc(&vm->immutable_space,
                                                    STA_CLS_CLASS, 4);
    STA_OOP *cls_slots = sta_payload(array_cls);
    cls_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    cls_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    cls_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(0, STA_FMT_VARIABLE_OOP);
    cls_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_ARRAY,
                        (STA_OOP)(uintptr_t)array_cls);

    /* Minimal Object class (class_index 2) — normal, 0 instVars. */
    STA_ObjHeader *obj_cls = sta_immutable_alloc(&vm->immutable_space,
                                                  STA_CLS_CLASS, 4);
    STA_OOP *obj_cls_slots = sta_payload(obj_cls);
    obj_cls_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    obj_cls_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    obj_cls_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(0, STA_FMT_NORMAL);
    obj_cls_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_OBJECT,
                        (STA_OOP)(uintptr_t)obj_cls);

    /* Association class (class_index 14) — normal, 2 instVars. */
    STA_ObjHeader *assoc_cls = sta_immutable_alloc(&vm->immutable_space,
                                                    STA_CLS_CLASS, 4);
    STA_OOP *assoc_cls_slots = sta_payload(assoc_cls);
    assoc_cls_slots[STA_CLASS_SLOT_SUPERCLASS] = vm->specials[SPC_NIL];
    assoc_cls_slots[STA_CLASS_SLOT_METHODDICT] = 0;
    assoc_cls_slots[STA_CLASS_SLOT_FORMAT] = STA_FORMAT_ENCODE(2, STA_FMT_NORMAL);
    assoc_cls_slots[STA_CLASS_SLOT_NAME] = 0;
    sta_class_table_set(&vm->class_table, STA_CLS_ASSOCIATION,
                        (STA_OOP)(uintptr_t)assoc_cls);

    /* String class (class_index 8) — byte-indexable, 0 instVars. */
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

/* ── Test 1: Basic GC — allocate, trigger GC, survivors correct ────────── */

static void test_gc_basic_survive(void) {
    STA_VM *vm = create_test_vm();
    /* 256-byte heap: enough for a few small objects. */
    struct STA_Actor *actor = create_test_actor(vm, 256);
    STA_Heap *heap = &actor->heap;

    /* Allocate an Association (2 slots = 32 bytes total). */
    STA_ObjHeader *obj = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(obj);
    STA_OOP *slots = sta_payload(obj);
    slots[0] = STA_SMALLINT_OOP(42);
    slots[1] = STA_SMALLINT_OOP(99);
    STA_OOP obj_oop = (STA_OOP)(uintptr_t)obj;

    /* Root the object via behavior_obj so GC can find it. */
    actor->behavior_obj = obj_oop;

    size_t used_before = heap->used;
    assert(used_before > 0);

    /* Trigger GC. */
    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    /* behavior_obj should now point into the new heap (to-space). */
    STA_ObjHeader *new_obj = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(new_obj != obj);  /* Must have moved. */
    assert(new_obj->class_index == STA_CLS_ASSOCIATION);
    assert(new_obj->size == 2);
    STA_OOP *new_slots = sta_payload(new_obj);
    assert(new_slots[0] == STA_SMALLINT_OOP(42));
    assert(new_slots[1] == STA_SMALLINT_OOP(99));

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 2: Dead objects are not copied ───────────────────────────────── */

static void test_gc_dead_not_copied(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 512);
    STA_Heap *heap = &actor->heap;

    /* Allocate several objects — only root one of them. */
    STA_ObjHeader *live = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(live);
    sta_payload(live)[0] = STA_SMALLINT_OOP(1);
    sta_payload(live)[1] = STA_SMALLINT_OOP(2);

    /* Dead objects — not rooted. */
    STA_ObjHeader *dead1 = sta_heap_alloc(heap, STA_CLS_ARRAY, 3);
    assert(dead1);
    STA_ObjHeader *dead2 = sta_heap_alloc(heap, STA_CLS_ARRAY, 5);
    assert(dead2);

    size_t used_before = heap->used;

    /* Root only the Association. */
    actor->behavior_obj = (STA_OOP)(uintptr_t)live;

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    /* After GC, only the live object should survive.
     * used should be just the size of the one Association (32 bytes). */
    size_t used_after = heap->used;
    assert(used_after < used_before);

    /* Live object data intact. */
    STA_ObjHeader *new_live = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(sta_payload(new_live)[0] == STA_SMALLINT_OOP(1));
    assert(sta_payload(new_live)[1] == STA_SMALLINT_OOP(2));

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 3: Forwarding pointers — A references B, both survive ────────── */

static void test_gc_forwarding(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 512);
    STA_Heap *heap = &actor->heap;

    /* Create B first (child). */
    STA_ObjHeader *b = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(b);
    sta_payload(b)[0] = STA_SMALLINT_OOP(100);
    sta_payload(b)[1] = STA_SMALLINT_OOP(200);
    STA_OOP b_oop = (STA_OOP)(uintptr_t)b;

    /* Create A (parent) referencing B. */
    STA_ObjHeader *a = sta_heap_alloc(heap, STA_CLS_ARRAY, 2);
    assert(a);
    sta_payload(a)[0] = b_oop;
    sta_payload(a)[1] = STA_SMALLINT_OOP(999);
    STA_OOP a_oop = (STA_OOP)(uintptr_t)a;

    /* Root A via behavior_obj. */
    actor->behavior_obj = a_oop;

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    /* A should have moved. */
    STA_ObjHeader *new_a = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(new_a->class_index == STA_CLS_ARRAY);
    assert(new_a->size == 2);

    /* A's slot[0] should point to B's new location. */
    STA_OOP new_b_oop = sta_payload(new_a)[0];
    assert(STA_IS_HEAP(new_b_oop));
    STA_ObjHeader *new_b = (STA_ObjHeader *)(uintptr_t)new_b_oop;
    assert(new_b->class_index == STA_CLS_ASSOCIATION);
    assert(sta_payload(new_b)[0] == STA_SMALLINT_OOP(100));
    assert(sta_payload(new_b)[1] == STA_SMALLINT_OOP(200));

    /* A's slot[1] should be unchanged SmallInt. */
    assert(sta_payload(new_a)[1] == STA_SMALLINT_OOP(999));

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 4: SmallInt and nil references unchanged after GC ────────────── */

static void test_gc_immediates_unchanged(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 256);
    STA_Heap *heap = &actor->heap;

    STA_OOP nil_oop = vm->specials[SPC_NIL];
    STA_OOP true_oop = vm->specials[SPC_TRUE];

    /* Array holding immediates and immutable refs. */
    STA_ObjHeader *arr = sta_heap_alloc(heap, STA_CLS_ARRAY, 4);
    assert(arr);
    sta_payload(arr)[0] = STA_SMALLINT_OOP(42);
    sta_payload(arr)[1] = nil_oop;
    sta_payload(arr)[2] = true_oop;
    sta_payload(arr)[3] = STA_CHAR_OOP('A');

    actor->behavior_obj = (STA_OOP)(uintptr_t)arr;

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    STA_ObjHeader *new_arr = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(sta_payload(new_arr)[0] == STA_SMALLINT_OOP(42));
    assert(sta_payload(new_arr)[1] == nil_oop);
    assert(sta_payload(new_arr)[2] == true_oop);
    assert(sta_payload(new_arr)[3] == STA_CHAR_OOP('A'));

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 5: Immutable space references unchanged ──────────────────────── */

static void test_gc_immutable_refs(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 256);
    STA_Heap *heap = &actor->heap;

    /* Create an immutable object (simulating a Symbol). */
    STA_ObjHeader *sym = sta_immutable_alloc(&vm->immutable_space,
                                              STA_CLS_SYMBOL, 1);
    assert(sym);
    STA_OOP sym_oop = (STA_OOP)(uintptr_t)sym;

    /* Array referencing the immutable symbol. */
    STA_ObjHeader *arr = sta_heap_alloc(heap, STA_CLS_ARRAY, 1);
    assert(arr);
    sta_payload(arr)[0] = sym_oop;

    actor->behavior_obj = (STA_OOP)(uintptr_t)arr;

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    STA_ObjHeader *new_arr = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    /* The symbol reference should be unchanged (immutable, not copied). */
    assert(sta_payload(new_arr)[0] == sym_oop);

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 6: Zero-payload object (identity object) ────────────────────── */

static void test_gc_zero_payload(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 256);
    STA_Heap *heap = &actor->heap;

    /* Zero-slot object (identity object, like basicNew on 0-instVar class). */
    STA_ObjHeader *id_obj = sta_heap_alloc(heap, STA_CLS_OBJECT, 0);
    assert(id_obj);
    assert(id_obj->size == 0);
    STA_OOP id_oop = (STA_OOP)(uintptr_t)id_obj;

    /* An array pointing to the identity object. */
    STA_ObjHeader *arr = sta_heap_alloc(heap, STA_CLS_ARRAY, 1);
    assert(arr);
    sta_payload(arr)[0] = id_oop;

    actor->behavior_obj = (STA_OOP)(uintptr_t)arr;

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    STA_ObjHeader *new_arr = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    STA_OOP new_id_oop = sta_payload(new_arr)[0];
    assert(STA_IS_HEAP(new_id_oop));
    STA_ObjHeader *new_id = (STA_ObjHeader *)(uintptr_t)new_id_oop;
    assert(new_id->class_index == STA_CLS_OBJECT);
    assert(new_id->size == 0);
    /* Must have moved (different address). */
    assert(new_id != id_obj);

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 7: Bytes survived — measure reclaimed ────────────────────────── */

static void test_gc_bytes_measurement(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 1024);
    STA_Heap *heap = &actor->heap;

    /* Allocate: 1 live (32 bytes) + many dead objects. */
    STA_ObjHeader *live = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(live);
    sta_payload(live)[0] = STA_SMALLINT_OOP(1);
    sta_payload(live)[1] = STA_SMALLINT_OOP(2);
    actor->behavior_obj = (STA_OOP)(uintptr_t)live;

    /* Fill up with garbage. */
    while (sta_heap_alloc(heap, STA_CLS_ARRAY, 2) != NULL) {
        /* keep allocating dead objects */
    }

    size_t used_before = heap->used;
    assert(used_before > 32);

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    size_t used_after = heap->used;
    /* Only the live Association (32 bytes) should remain. */
    assert(used_after == 32);
    assert(used_after < used_before);

    /* Data intact. */
    STA_ObjHeader *new_live = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(sta_payload(new_live)[0] == STA_SMALLINT_OOP(1));
    assert(sta_payload(new_live)[1] == STA_SMALLINT_OOP(2));

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 8: Circular reference — A → B → A ───────────────────────────── */

static void test_gc_circular_ref(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 512);
    STA_Heap *heap = &actor->heap;

    STA_ObjHeader *a = sta_heap_alloc(heap, STA_CLS_ARRAY, 1);
    STA_ObjHeader *b = sta_heap_alloc(heap, STA_CLS_ARRAY, 1);
    assert(a && b);

    /* Create cycle: A[0] = B, B[0] = A. */
    sta_payload(a)[0] = (STA_OOP)(uintptr_t)b;
    sta_payload(b)[0] = (STA_OOP)(uintptr_t)a;

    actor->behavior_obj = (STA_OOP)(uintptr_t)a;

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    /* Both should survive and the cycle should be preserved. */
    STA_ObjHeader *new_a = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    STA_OOP new_b_oop = sta_payload(new_a)[0];
    assert(STA_IS_HEAP(new_b_oop));
    STA_ObjHeader *new_b = (STA_ObjHeader *)(uintptr_t)new_b_oop;

    /* B[0] should point back to new A. */
    assert(sta_payload(new_b)[0] == actor->behavior_obj);

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 9: Byte-indexable object (String) — raw bytes copied, not scanned */

static void test_gc_byte_object(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 512);
    STA_Heap *heap = &actor->heap;

    /* Allocate a String-like byte-indexable object: "hello" = 5 bytes.
     * 5 bytes → 1 OOP word, padding = 3. */
    STA_ObjHeader *str = sta_heap_alloc(heap, STA_CLS_STRING, 1);
    assert(str);
    str->reserved = 3;  /* 8 - 5 = 3 padding bytes */
    memcpy(sta_payload(str), "hello\0\0\0", 8);

    actor->behavior_obj = (STA_OOP)(uintptr_t)str;

    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);

    STA_ObjHeader *new_str = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(new_str->class_index == STA_CLS_STRING);
    assert(new_str->size == 1);
    assert(new_str->reserved == 3);
    assert(memcmp(sta_payload(new_str), "hello\0\0\0", 8) == 0);

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 10: Multiple GC cycles ──────────────────────────────────────── */

static void test_gc_multiple_cycles(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 256);
    STA_Heap *heap = &actor->heap;

    /* Cycle 1: allocate, GC. */
    STA_ObjHeader *obj1 = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(obj1);
    sta_payload(obj1)[0] = STA_SMALLINT_OOP(1);
    sta_payload(obj1)[1] = STA_SMALLINT_OOP(2);
    actor->behavior_obj = (STA_OOP)(uintptr_t)obj1;

    /* Fill with garbage. */
    while (sta_heap_alloc(heap, STA_CLS_ARRAY, 1)) {}

    assert(sta_gc_collect(vm, actor) == 0);
    STA_ObjHeader *after1 = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(sta_payload(after1)[0] == STA_SMALLINT_OOP(1));

    /* Cycle 2: allocate more on the new heap, GC again. */
    STA_ObjHeader *obj2 = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(obj2);
    sta_payload(obj2)[0] = STA_SMALLINT_OOP(3);
    sta_payload(obj2)[1] = STA_SMALLINT_OOP(4);

    /* Make obj1 point to obj2. */
    after1 = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    sta_payload(after1)[1] = (STA_OOP)(uintptr_t)obj2;

    /* Fill with garbage again. */
    while (sta_heap_alloc(heap, STA_CLS_ARRAY, 1)) {}

    assert(sta_gc_collect(vm, actor) == 0);

    /* Both should survive. */
    STA_ObjHeader *after2 = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    assert(sta_payload(after2)[0] == STA_SMALLINT_OOP(1));
    STA_OOP ref2 = sta_payload(after2)[1];
    assert(STA_IS_HEAP(ref2));
    STA_ObjHeader *new_obj2 = (STA_ObjHeader *)(uintptr_t)ref2;
    assert(sta_payload(new_obj2)[0] == STA_SMALLINT_OOP(3));
    assert(sta_payload(new_obj2)[1] == STA_SMALLINT_OOP(4));

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 11: gc_flags cleared after GC ───────────────────────────────── */

static void test_gc_flags_cleared(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 256);
    STA_Heap *heap = &actor->heap;

    STA_ObjHeader *obj = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(obj);
    sta_payload(obj)[0] = STA_SMALLINT_OOP(7);
    sta_payload(obj)[1] = STA_SMALLINT_OOP(8);
    actor->behavior_obj = (STA_OOP)(uintptr_t)obj;

    assert(sta_gc_collect(vm, actor) == 0);

    STA_ObjHeader *new_obj = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    /* gc_flags should be WHITE (0) — no forwarded flag, no color bits. */
    assert(new_obj->gc_flags == STA_GC_WHITE);

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Test 12: Empty heap — GC on empty heap is a no-op ─────────────────── */

static void test_gc_empty_heap(void) {
    STA_VM *vm = create_test_vm();
    struct STA_Actor *actor = create_test_actor(vm, 256);

    /* No objects allocated, no roots. */
    int rc = sta_gc_collect(vm, actor);
    assert(rc == 0);
    assert(actor->heap.used == 0);

    sta_actor_terminate(actor);
    destroy_test_vm(vm);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_gc (Phase 2 Epic 5, Story 1):\n");

    RUN(test_gc_basic_survive);
    RUN(test_gc_dead_not_copied);
    RUN(test_gc_forwarding);
    RUN(test_gc_immediates_unchanged);
    RUN(test_gc_immutable_refs);
    RUN(test_gc_zero_payload);
    RUN(test_gc_bytes_measurement);
    RUN(test_gc_circular_ref);
    RUN(test_gc_byte_object);
    RUN(test_gc_multiple_cycles);
    RUN(test_gc_flags_cleared);
    RUN(test_gc_empty_heap);

    printf("\n  %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
