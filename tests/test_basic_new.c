/* tests/test_basic_new.c
 * Tests for primitive 31 — basicNew (Phase 1, Epic 5.2).
 * Exercises fixed-size instance allocation through the primitive table.
 */
#include "vm/format.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/symbol_table.h"
#include "vm/interpreter.h"
#include "vm/primitive_table.h"
#include "vm/special_objects.h"
#include "bootstrap/bootstrap.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* ── Shared test infrastructure ────────────────────────────────────────── */

static STA_ImmutableSpace *g_sp;
static STA_SymbolTable    *g_st;
static STA_Heap           *g_heap;
static STA_ClassTable     *g_ct;

static void setup(void) {
    g_sp   = sta_immutable_space_create(512 * 1024);
    g_st   = sta_symbol_table_create(512);
    g_heap = sta_heap_create(512 * 1024);
    g_ct   = sta_class_table_create();

    STA_BootstrapResult r = sta_bootstrap(g_heap, g_sp, g_st, g_ct);
    assert(r.status == 0);
}

static void teardown(void) {
    sta_class_table_destroy(g_ct);
    sta_heap_destroy(g_heap);
    sta_symbol_table_destroy(g_st);
    sta_immutable_space_destroy(g_sp);
}

/* Helper: call prim 31 with receiver = class OOP. */
static int call_basic_new(STA_OOP class_oop, STA_OOP *result) {
    STA_PrimFn prim = sta_primitives[31];
    assert(prim != NULL);
    STA_OOP args[1] = { class_oop };
    return prim(args, 0, result);
}

/* ── Test 1: Object basicNew — 0 instance variables ────────────────────── */

static void test_object_basic_new(void) {
    printf("  Object basicNew...");
    STA_OOP obj_cls = sta_class_table_get(g_ct, STA_CLS_OBJECT);
    STA_OOP result;
    int rc = call_basic_new(obj_cls, &result);
    assert(rc == STA_PRIM_SUCCESS);

    /* Verify it's a heap object. */
    assert(STA_IS_HEAP(result));

    /* Verify class_index. */
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_OBJECT);

    /* Object has 0 inst vars. */
    assert(h->size == 0);

    printf(" ok\n");
}

/* ── Test 2: Association basicNew — 2 instance variables, both nil ──────── */

static void test_association_basic_new(void) {
    printf("  Association basicNew...");
    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    STA_OOP result;
    int rc = call_basic_new(assoc_cls, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_ASSOCIATION);
    assert(h->size == 2);

    /* Both slots should be nil. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP *slots = sta_payload(h);
    assert(slots[0] == nil_oop);
    assert(slots[1] == nil_oop);

    printf(" ok\n");
}

/* ── Test 3: BlockClosure basicNew — 4 instance variables, all nil ──────── */

static void test_blockclosure_basic_new(void) {
    printf("  BlockClosure basicNew...");
    STA_OOP bc_cls = sta_class_table_get(g_ct, STA_CLS_BLOCKCLOSURE);
    STA_OOP result;
    int rc = call_basic_new(bc_cls, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_BLOCKCLOSURE);
    assert(h->size == 4);

    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < 4; i++) {
        assert(slots[i] == nil_oop);
    }

    printf(" ok\n");
}

/* ── Test 4: Message basicNew — 3 instance variables ───────────────────── */

static void test_message_basic_new(void) {
    printf("  Message basicNew...");
    STA_OOP msg_cls = sta_class_table_get(g_ct, STA_CLS_MESSAGE);
    STA_OOP result;
    int rc = call_basic_new(msg_cls, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_MESSAGE);
    assert(h->size == 3);

    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < 3; i++) {
        assert(slots[i] == nil_oop);
    }

    printf(" ok\n");
}

/* ── Test 5: Array basicNew fails (indexable class) ────────────────────── */

static void test_array_basic_new_fails(void) {
    printf("  Array basicNew fails...");
    STA_OOP arr_cls = sta_class_table_get(g_ct, STA_CLS_ARRAY);
    STA_OOP result;
    int rc = call_basic_new(arr_cls, &result);
    assert(rc == STA_PRIM_BAD_RECEIVER);
    printf(" ok\n");
}

/* ── Test 6: String basicNew fails (indexable byte class) ──────────────── */

static void test_string_basic_new_fails(void) {
    printf("  String basicNew fails...");
    STA_OOP str_cls = sta_class_table_get(g_ct, STA_CLS_STRING);
    STA_OOP result;
    int rc = call_basic_new(str_cls, &result);
    assert(rc == STA_PRIM_BAD_RECEIVER);
    printf(" ok\n");
}

/* ── Test 7: SmallInteger basicNew fails (immediate) ───────────────────── */

static void test_smallint_basic_new_fails(void) {
    printf("  SmallInteger basicNew fails...");
    STA_OOP si_cls = sta_class_table_get(g_ct, STA_CLS_SMALLINTEGER);
    STA_OOP result;
    int rc = call_basic_new(si_cls, &result);
    assert(rc == STA_PRIM_BAD_RECEIVER);
    printf(" ok\n");
}

/* ── Test 8: Float basicNew fails (immediate) ──────────────────────────── */

static void test_float_basic_new_fails(void) {
    printf("  Float basicNew fails...");
    STA_OOP flt_cls = sta_class_table_get(g_ct, STA_CLS_FLOAT);
    STA_OOP result;
    int rc = call_basic_new(flt_cls, &result);
    assert(rc == STA_PRIM_BAD_RECEIVER);
    printf(" ok\n");
}

/* ── Test 9: CompiledMethod basicNew fails ─────────────────────────────── */

static void test_compiled_method_basic_new_fails(void) {
    printf("  CompiledMethod basicNew fails...");
    STA_OOP cm_cls = sta_class_table_get(g_ct, STA_CLS_COMPILEDMETHOD);
    STA_OOP result;
    int rc = call_basic_new(cm_cls, &result);
    assert(rc == STA_PRIM_BAD_RECEIVER);
    printf(" ok\n");
}

/* ── Test 10: Multiple allocations produce distinct objects ────────────── */

static void test_multiple_allocations_distinct(void) {
    printf("  multiple allocations distinct...");
    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    STA_OOP a, b;
    assert(call_basic_new(assoc_cls, &a) == STA_PRIM_SUCCESS);
    assert(call_basic_new(assoc_cls, &b) == STA_PRIM_SUCCESS);
    assert(a != b);

    /* Both should have correct class_index. */
    STA_ObjHeader *ha = (STA_ObjHeader *)(uintptr_t)a;
    STA_ObjHeader *hb = (STA_ObjHeader *)(uintptr_t)b;
    assert(ha->class_index == STA_CLS_ASSOCIATION);
    assert(hb->class_index == STA_CLS_ASSOCIATION);

    printf(" ok\n");
}

/* ── Test 11: Allocation failure on tiny heap ──────────────────────────── */

static void test_allocation_failure(void) {
    printf("  allocation failure on full heap...");

    /* Create a tiny heap and bootstrap-free primitive context. */
    STA_Heap *tiny = sta_heap_create(128);  /* just 128 bytes */
    assert(tiny != NULL);

    /* Save and swap the global heap. */
    sta_primitive_set_heap(tiny);

    /* Association needs 2 slots = 16 + 16 = 32 bytes per object.
     * 128 bytes should fit a few, then fail. */
    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    STA_OOP result;
    int rc;
    int count = 0;
    for (int i = 0; i < 100; i++) {
        rc = call_basic_new(assoc_cls, &result);
        if (rc != STA_PRIM_SUCCESS) break;
        count++;
    }
    assert(rc == STA_PRIM_NO_MEMORY);
    assert(count > 0);  /* should have allocated at least one */

    /* Restore original heap. */
    sta_primitive_set_heap(g_heap);
    sta_heap_destroy(tiny);

    printf(" ok (allocated %d before failure)\n", count);
}

/* ── Test 12: class_table_index_of works for all kernel classes ────────── */

static void test_class_table_index_of(void) {
    printf("  class_table_index_of...");

    /* Spot-check a few. */
    STA_OOP obj_cls = sta_class_table_get(g_ct, STA_CLS_OBJECT);
    assert(sta_class_table_index_of(g_ct, obj_cls) == STA_CLS_OBJECT);

    STA_OOP arr_cls = sta_class_table_get(g_ct, STA_CLS_ARRAY);
    assert(sta_class_table_index_of(g_ct, arr_cls) == STA_CLS_ARRAY);

    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    assert(sta_class_table_index_of(g_ct, assoc_cls) == STA_CLS_ASSOCIATION);

    STA_OOP si_cls = sta_class_table_get(g_ct, STA_CLS_SMALLINTEGER);
    assert(sta_class_table_index_of(g_ct, si_cls) == STA_CLS_SMALLINTEGER);

    /* Not-found case. */
    assert(sta_class_table_index_of(g_ct, 0) == 0);
    assert(sta_class_table_index_of(g_ct, 0xDEADBEEF) == 0);

    printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_basic_new:\n");
    setup();

    test_object_basic_new();
    test_association_basic_new();
    test_blockclosure_basic_new();
    test_message_basic_new();
    test_array_basic_new_fails();
    test_string_basic_new_fails();
    test_smallint_basic_new_fails();
    test_float_basic_new_fails();
    test_compiled_method_basic_new_fails();
    test_multiple_allocations_distinct();
    test_allocation_failure();
    test_class_table_index_of();

    teardown();
    printf("  all basicNew tests passed.\n");
    return 0;
}
