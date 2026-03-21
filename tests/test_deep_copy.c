/* tests/test_deep_copy.c
 * Unit tests for the cross-actor deep copy engine — Phase 2 Epic 3 Story 3.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "actor/deep_copy.h"
#include "actor/actor.h"
#include "vm/vm_state.h"
#include "vm/heap.h"
#include "vm/oop.h"
#include "vm/class_table.h"
#include "vm/format.h"
#include "vm/interpreter.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include "vm/special_objects.h"
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

/* ── Helpers ─────────────────────────────────────────────────────────── */

/* Use a fully bootstrapped VM so class table has correct format info. */
static STA_VM *g_vm;

static void setup(void) {
    STA_VMConfig cfg = {0};
    g_vm = sta_vm_create(&cfg);
    if (!g_vm) {
        fprintf(stderr, "sta_vm_create failed: %s\n", sta_vm_last_error(NULL));
    }
    assert(g_vm != NULL);
}

static void teardown(void) {
    sta_vm_destroy(g_vm);
    g_vm = NULL;
}

/* Create an actor with a fresh heap. */
static struct STA_Actor *make_actor(size_t heap_size) {
    struct STA_Actor *a = sta_actor_create(g_vm, heap_size, 512);
    assert(a != NULL);
    sta_actor_register(a);
    return a;
}

/* Allocate an Array of given size on a heap, fill with SmallInts. */
static STA_OOP make_array(STA_Heap *heap, int count) {
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_ARRAY, (uint32_t)count);
    assert(h != NULL);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < count; i++)
        slots[i] = STA_SMALLINT_OOP(i + 1);
    return (STA_OOP)(uintptr_t)h;
}

/* Allocate a mutable String on a heap. */
static STA_OOP make_string(STA_Heap *heap, const char *text) {
    size_t len = strlen(text);
    uint32_t var_words = ((uint32_t)len + (uint32_t)(sizeof(STA_OOP) - 1))
                         / (uint32_t)sizeof(STA_OOP);
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_STRING, var_words);
    assert(h != NULL);
    h->reserved = (uint16_t)(var_words * sizeof(STA_OOP) - (uint32_t)len);
    memset(sta_payload(h), 0, var_words * sizeof(STA_OOP));
    memcpy(sta_payload(h), text, len);
    return (STA_OOP)(uintptr_t)h;
}

/* Check if oop is on the given heap. */
static bool on_heap(STA_Heap *heap, STA_OOP oop) {
    if (STA_IS_IMMEDIATE(oop) || oop == 0) return false;
    uintptr_t addr = (uintptr_t)oop;
    uintptr_t base = (uintptr_t)heap->base;
    return addr >= base && addr < base + heap->capacity;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

static void test_copy_smallint(void) {
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *dst = make_actor(4096);

    STA_OOP result = sta_deep_copy(STA_SMALLINT_OOP(42),
                                    &src->heap, &dst->heap,
                                    &g_vm->class_table);
    assert(result == STA_SMALLINT_OOP(42));

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_character(void) {
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *dst = make_actor(4096);

    STA_OOP ch = STA_CHAR_OOP('A');
    STA_OOP result = sta_deep_copy(ch, &src->heap, &dst->heap,
                                    &g_vm->class_table);
    assert(result == ch);

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_symbol_shared(void) {
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *dst = make_actor(4096);

    /* Intern a symbol — lives in immutable space, has IMMUTABLE flag. */
    STA_OOP sym = sta_symbol_intern(&g_vm->immutable_space,
                                     &g_vm->symbol_table,
                                     "hello", 5);
    assert(sym != 0);

    STA_OOP result = sta_deep_copy(sym, &src->heap, &dst->heap,
                                    &g_vm->class_table);
    /* Symbol is immutable — shared by pointer, same OOP. */
    assert(result == sym);

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_nil_shared(void) {
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *dst = make_actor(4096);

    STA_OOP nil_oop = g_vm->specials[SPC_NIL];
    STA_OOP result = sta_deep_copy(nil_oop, &src->heap, &dst->heap,
                                    &g_vm->class_table);
    /* nil is immutable — shared by pointer. */
    assert(result == nil_oop);

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_mutable_array(void) {
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *dst = make_actor(4096);

    STA_OOP arr = make_array(&src->heap, 5);
    size_t dst_used_before = sta_heap_used(&dst->heap);

    STA_OOP copy = sta_deep_copy(arr, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0);
    assert(copy != arr);  /* Different address */
    assert(on_heap(&dst->heap, copy));  /* Lives on target heap */
    assert(!on_heap(&src->heap, copy));

    /* Verify contents. */
    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)copy;
    assert(copy_h->class_index == STA_CLS_ARRAY);
    assert(copy_h->size == 5);
    STA_OOP *copy_slots = sta_payload(copy_h);
    for (int i = 0; i < 5; i++) {
        assert(copy_slots[i] == STA_SMALLINT_OOP(i + 1));
    }

    /* Target heap should have grown. */
    assert(sta_heap_used(&dst->heap) > dst_used_before);

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_array_with_symbol(void) {
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *dst = make_actor(4096);

    /* Array containing a Symbol (immutable) and a SmallInt. */
    STA_OOP sym = sta_symbol_intern(&g_vm->immutable_space,
                                     &g_vm->symbol_table,
                                     "test", 4);
    STA_ObjHeader *h = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 2);
    STA_OOP *slots = sta_payload(h);
    slots[0] = sym;
    slots[1] = STA_SMALLINT_OOP(99);
    STA_OOP arr = (STA_OOP)(uintptr_t)h;

    STA_OOP copy = sta_deep_copy(arr, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0 && copy != arr);

    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)copy;
    STA_OOP *copy_slots = sta_payload(copy_h);
    /* Symbol shared by pointer. */
    assert(copy_slots[0] == sym);
    /* SmallInt passed through. */
    assert(copy_slots[1] == STA_SMALLINT_OOP(99));

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_shared_structure(void) {
    struct STA_Actor *src = make_actor(8192);
    struct STA_Actor *dst = make_actor(8192);

    /* Create shared sub-object C, then A and B both point to C. */
    STA_OOP c = make_array(&src->heap, 1);  /* C = #(1) */

    STA_ObjHeader *ah = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    sta_payload(ah)[0] = c;
    STA_OOP a = (STA_OOP)(uintptr_t)ah;

    STA_ObjHeader *bh = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    sta_payload(bh)[0] = c;
    STA_OOP b = (STA_OOP)(uintptr_t)bh;

    /* Outer array: #(a b) */
    STA_ObjHeader *outer_h = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 2);
    sta_payload(outer_h)[0] = a;
    sta_payload(outer_h)[1] = b;
    STA_OOP outer = (STA_OOP)(uintptr_t)outer_h;

    STA_OOP copy = sta_deep_copy(outer, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0);

    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)copy;
    STA_OOP copy_a = sta_payload(copy_h)[0];
    STA_OOP copy_b = sta_payload(copy_h)[1];

    STA_ObjHeader *copy_a_h = (STA_ObjHeader *)(uintptr_t)copy_a;
    STA_ObjHeader *copy_b_h = (STA_ObjHeader *)(uintptr_t)copy_b;

    STA_OOP copy_c_from_a = sta_payload(copy_a_h)[0];
    STA_OOP copy_c_from_b = sta_payload(copy_b_h)[0];

    /* Both point to the SAME copy of C (shared structure preserved). */
    assert(copy_c_from_a == copy_c_from_b);
    /* But it's NOT the original C. */
    assert(copy_c_from_a != c);
    assert(on_heap(&dst->heap, copy_c_from_a));

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_cycle(void) {
    struct STA_Actor *src = make_actor(8192);
    struct STA_Actor *dst = make_actor(8192);

    /* A points to B, B points to A — a cycle. */
    STA_ObjHeader *ah = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    STA_ObjHeader *bh = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    STA_OOP a = (STA_OOP)(uintptr_t)ah;
    STA_OOP b = (STA_OOP)(uintptr_t)bh;
    sta_payload(ah)[0] = b;
    sta_payload(bh)[0] = a;

    STA_OOP copy_a = sta_deep_copy(a, &src->heap, &dst->heap,
                                    &g_vm->class_table);
    assert(copy_a != 0);
    assert(copy_a != a);

    STA_ObjHeader *ca_h = (STA_ObjHeader *)(uintptr_t)copy_a;
    STA_OOP copy_b = sta_payload(ca_h)[0];
    assert(copy_b != 0);
    assert(copy_b != b);
    assert(copy_b != copy_a);

    STA_ObjHeader *cb_h = (STA_ObjHeader *)(uintptr_t)copy_b;
    /* B' should point back to A'. */
    assert(sta_payload(cb_h)[0] == copy_a);

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_mutable_string(void) {
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *dst = make_actor(4096);

    STA_OOP str = make_string(&src->heap, "hello");

    STA_OOP copy = sta_deep_copy(str, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0);
    assert(copy != str);
    assert(on_heap(&dst->heap, copy));

    /* Verify byte content. */
    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)copy;
    assert(copy_h->class_index == STA_CLS_STRING);
    STA_ObjHeader *orig_h = (STA_ObjHeader *)(uintptr_t)str;
    assert(copy_h->size == orig_h->size);
    assert(copy_h->reserved == orig_h->reserved);

    /* Byte content should match. */
    char *orig_bytes = (char *)sta_payload(orig_h);
    char *copy_bytes = (char *)sta_payload(copy_h);
    uint32_t byte_count = copy_h->size * (uint32_t)sizeof(STA_OOP)
                          - STA_BYTE_PADDING(copy_h);
    assert(memcmp(orig_bytes, copy_bytes, byte_count) == 0);

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_array_containing_string(void) {
    struct STA_Actor *src = make_actor(8192);
    struct STA_Actor *dst = make_actor(8192);

    STA_OOP str = make_string(&src->heap, "world");
    STA_ObjHeader *ah = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 2);
    sta_payload(ah)[0] = str;
    sta_payload(ah)[1] = STA_SMALLINT_OOP(42);
    STA_OOP arr = (STA_OOP)(uintptr_t)ah;

    STA_OOP copy = sta_deep_copy(arr, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0);

    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)copy;
    STA_OOP copy_str = sta_payload(copy_h)[0];
    assert(copy_str != str);  /* String was mutable, so copied. */
    assert(on_heap(&dst->heap, copy_str));
    assert(sta_payload(copy_h)[1] == STA_SMALLINT_OOP(42));

    /* Verify string bytes. */
    STA_ObjHeader *cs_h = (STA_ObjHeader *)(uintptr_t)copy_str;
    assert(cs_h->class_index == STA_CLS_STRING);
    uint32_t bc = cs_h->size * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(cs_h);
    assert(bc == 5);
    assert(memcmp(sta_payload(cs_h), "world", 5) == 0);

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_across_two_actor_heaps(void) {
    struct STA_Actor *actor_a = make_actor(8192);
    struct STA_Actor *actor_b = make_actor(8192);

    /* Build a small graph on actor A's heap. */
    STA_OOP str = make_string(&actor_a->heap, "cross-actor");
    STA_OOP arr = make_array(&actor_a->heap, 3);
    STA_ObjHeader *arr_h = (STA_ObjHeader *)(uintptr_t)arr;
    sta_payload(arr_h)[0] = str;
    sta_payload(arr_h)[1] = STA_SMALLINT_OOP(100);
    sta_payload(arr_h)[2] = g_vm->specials[SPC_TRUE];  /* immutable */

    /* Copy from A to B. */
    STA_OOP copy = sta_deep_copy(arr, &actor_a->heap, &actor_b->heap,
                                  &g_vm->class_table);
    assert(copy != 0);
    assert(on_heap(&actor_b->heap, copy));

    STA_ObjHeader *ch = (STA_ObjHeader *)(uintptr_t)copy;
    STA_OOP *cs = sta_payload(ch);

    /* String was mutable → copied onto B's heap. */
    assert(cs[0] != str);
    assert(on_heap(&actor_b->heap, cs[0]));

    /* SmallInt pass-through. */
    assert(cs[1] == STA_SMALLINT_OOP(100));

    /* true is immutable → shared. */
    assert(cs[2] == g_vm->specials[SPC_TRUE]);

    /* Mutating the copy on B doesn't affect A. */
    sta_payload(ch)[1] = STA_SMALLINT_OOP(999);
    assert(sta_payload(arr_h)[1] == STA_SMALLINT_OOP(100));

    sta_actor_terminate(actor_a);
    sta_actor_terminate(actor_b);
}

static void test_copy_zero_oop(void) {
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *dst = make_actor(4096);

    STA_OOP result = sta_deep_copy(0, &src->heap, &dst->heap,
                                    &g_vm->class_table);
    assert(result == 0);

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_deeply_nested(void) {
    struct STA_Actor *src = make_actor(32768);
    struct STA_Actor *dst = make_actor(32768);

    /* Build a chain: arr[0] -> arr[0] -> ... -> SmallInt, 20 levels deep. */
    STA_OOP inner = STA_SMALLINT_OOP(42);
    for (int i = 0; i < 20; i++) {
        STA_ObjHeader *h = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
        assert(h != NULL);
        sta_payload(h)[0] = inner;
        inner = (STA_OOP)(uintptr_t)h;
    }

    STA_OOP copy = sta_deep_copy(inner, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0);
    assert(copy != inner);

    /* Walk down 20 levels — should end at SmallInt 42. */
    STA_OOP current = copy;
    for (int i = 0; i < 20; i++) {
        assert(!STA_IS_IMMEDIATE(current) && current != 0);
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)current;
        assert(h->class_index == STA_CLS_ARRAY);
        assert(h->size == 1);
        current = sta_payload(h)[0];
    }
    assert(current == STA_SMALLINT_OOP(42));

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_mixed_mutable_immutable(void) {
    struct STA_Actor *src = make_actor(8192);
    struct STA_Actor *dst = make_actor(8192);

    STA_OOP sym = sta_symbol_intern(&g_vm->immutable_space,
                                     &g_vm->symbol_table,
                                     "mixTest", 7);
    STA_OOP str = make_string(&src->heap, "mutable");
    STA_OOP nil_oop = g_vm->specials[SPC_NIL];
    STA_OOP true_oop = g_vm->specials[SPC_TRUE];

    STA_ObjHeader *ah = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 5);
    STA_OOP *slots = sta_payload(ah);
    slots[0] = sym;                     /* immutable → shared */
    slots[1] = str;                     /* mutable → copied */
    slots[2] = STA_SMALLINT_OOP(7);     /* immediate → pass-through */
    slots[3] = nil_oop;                 /* immutable → shared */
    slots[4] = true_oop;               /* immutable → shared */
    STA_OOP arr = (STA_OOP)(uintptr_t)ah;

    STA_OOP copy = sta_deep_copy(arr, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0);

    STA_OOP *cs = sta_payload((STA_ObjHeader *)(uintptr_t)copy);
    assert(cs[0] == sym);               /* shared */
    assert(cs[1] != str);               /* copied */
    assert(on_heap(&dst->heap, cs[1]));
    assert(cs[2] == STA_SMALLINT_OOP(7));
    assert(cs[3] == nil_oop);           /* shared */
    assert(cs[4] == true_oop);          /* shared */

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

static void test_copy_empty_array(void) {
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *dst = make_actor(4096);

    STA_ObjHeader *h = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 0);
    STA_OOP arr = (STA_OOP)(uintptr_t)h;

    STA_OOP copy = sta_deep_copy(arr, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0);
    assert(copy != arr);
    STA_ObjHeader *ch = (STA_ObjHeader *)(uintptr_t)copy;
    assert(ch->class_index == STA_CLS_ARRAY);
    assert(ch->size == 0);

    sta_actor_terminate(src);
    sta_actor_terminate(dst);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_deep_copy:\n");
    setup();

    RUN(test_copy_smallint);
    RUN(test_copy_character);
    RUN(test_copy_symbol_shared);
    RUN(test_copy_nil_shared);
    RUN(test_copy_mutable_array);
    RUN(test_copy_array_with_symbol);
    RUN(test_copy_shared_structure);
    RUN(test_copy_cycle);
    RUN(test_copy_mutable_string);
    RUN(test_copy_array_containing_string);
    RUN(test_copy_across_two_actor_heaps);
    RUN(test_copy_zero_oop);
    RUN(test_copy_deeply_nested);
    RUN(test_copy_mixed_mutable_immutable);
    RUN(test_copy_empty_array);

    teardown();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
