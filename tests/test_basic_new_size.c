/* tests/test_basic_new_size.c
 * Tests for primitive 32 — basicNew: (Phase 1, Epic 5.3).
 * Exercises variable-size instance allocation for both pointer-indexable
 * and byte-indexable classes.
 */
#include "vm/format.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/symbol_table.h"
#include "vm/interpreter.h"
#include "vm/primitive_table.h"
#include "vm/special_objects.h"
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

/* ── Shared test infrastructure ────────────────────────────────────────── */

static STA_VM *g_vm;
static STA_ExecContext g_ctx;

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);
    sta_heap_init(&g_vm->heap, 512 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 512 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 512);
    sta_class_table_init(&g_vm->class_table);
    sta_special_objects_bind(g_vm->specials);

    STA_BootstrapResult r = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space,
                                           &g_vm->symbol_table, &g_vm->class_table);
    assert(r.status == 0);
    sta_primitive_table_init();
    g_ctx.vm = g_vm;
    g_ctx.actor = NULL;
}

static void teardown(void) {
    sta_class_table_deinit(&g_vm->class_table);
    sta_heap_deinit(&g_vm->heap);
    sta_symbol_table_deinit(&g_vm->symbol_table);
    sta_immutable_space_deinit(&g_vm->immutable_space);
    sta_special_objects_bind(NULL);
    free(g_vm);
}

/* Helper: call prim 32 with receiver = class OOP, arg = size. */
static int call_basic_new_size(STA_OOP class_oop, intptr_t size, STA_OOP *result) {
    STA_PrimFn prim = sta_primitives[32];
    assert(prim != NULL);
    STA_OOP args[2] = { class_oop, STA_SMALLINT_OOP(size) };
    return prim(&g_ctx, args, 1, result);
}

/* ── Test 1: Array new: 5 — 5 pointer slots, all nil ──────────────────── */

static void test_array_new_5(void) {
    printf("  Array new: 5...");
    STA_OOP arr_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP result;
    int rc = call_basic_new_size(arr_cls, 5, &result);
    assert(rc == STA_PRIM_SUCCESS);
    assert(STA_IS_HEAP(result));

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_ARRAY);
    assert(h->size == 5);

    /* All slots should be nil. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < 5; i++) {
        assert(slots[i] == nil_oop);
    }
    printf(" ok\n");
}

/* ── Test 2: Array new: 0 — valid empty array ─────────────────────────── */

static void test_array_new_0(void) {
    printf("  Array new: 0...");
    STA_OOP arr_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP result;
    int rc = call_basic_new_size(arr_cls, 0, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_ARRAY);
    assert(h->size == 0);
    printf(" ok\n");
}

/* ── Test 3: ByteArray new: 16 — exact word boundary ──────────────────── */

static void test_bytearray_new_16(void) {
    printf("  ByteArray new: 16...");
    STA_OOP ba_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_BYTEARRAY);
    STA_OOP result;
    int rc = call_basic_new_size(ba_cls, 16, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_BYTEARRAY);
    /* 16 bytes = 2 words, 0 padding. */
    assert(h->size == 2);
    assert(STA_BYTE_PADDING(h) == 0);

    /* Recover exact byte count: size * 8 - padding = 16. */
    uint32_t exact = h->size * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
    assert(exact == 16);

    /* All bytes should be zero. */
    uint8_t *bytes = (uint8_t *)sta_payload(h);
    for (int i = 0; i < 16; i++) {
        assert(bytes[i] == 0);
    }
    printf(" ok\n");
}

/* ── Test 4: ByteArray new: 7 — non-aligned, 1 byte padding ──────────── */

static void test_bytearray_new_7(void) {
    printf("  ByteArray new: 7...");
    STA_OOP ba_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_BYTEARRAY);
    STA_OOP result;
    int rc = call_basic_new_size(ba_cls, 7, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_BYTEARRAY);
    /* 7 bytes → ceil(7/8) = 1 word, padding = 8 - 7 = 1. */
    assert(h->size == 1);
    assert(STA_BYTE_PADDING(h) == 1);

    uint32_t exact = h->size * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
    assert(exact == 7);
    printf(" ok\n");
}

/* ── Test 5: ByteArray new: 0 — valid empty byte array ────────────────── */

static void test_bytearray_new_0(void) {
    printf("  ByteArray new: 0...");
    STA_OOP ba_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_BYTEARRAY);
    STA_OOP result;
    int rc = call_basic_new_size(ba_cls, 0, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_BYTEARRAY);
    assert(h->size == 0);
    assert(STA_BYTE_PADDING(h) == 0);
    printf(" ok\n");
}

/* ── Test 6: String new: 10 — 10 bytes, byte-indexable ────────────────── */

static void test_string_new_10(void) {
    printf("  String new: 10...");
    STA_OOP str_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_STRING);
    STA_OOP result;
    int rc = call_basic_new_size(str_cls, 10, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_STRING);
    /* 10 bytes → ceil(10/8) = 2 words, padding = 16 - 10 = 6. */
    assert(h->size == 2);
    assert(STA_BYTE_PADDING(h) == 6);

    uint32_t exact = h->size * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
    assert(exact == 10);

    /* All bytes should be zero. */
    uint8_t *bytes = (uint8_t *)sta_payload(h);
    for (int i = 0; i < 10; i++) {
        assert(bytes[i] == 0);
    }
    printf(" ok\n");
}

/* ── Test 7: ByteArray new: 1 — minimal non-empty ─────────────────────── */

static void test_bytearray_new_1(void) {
    printf("  ByteArray new: 1...");
    STA_OOP ba_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_BYTEARRAY);
    STA_OOP result;
    int rc = call_basic_new_size(ba_cls, 1, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    /* 1 byte → 1 word, padding = 7. */
    assert(h->size == 1);
    assert(STA_BYTE_PADDING(h) == 7);

    uint32_t exact = h->size * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
    assert(exact == 1);
    printf(" ok\n");
}

/* ── Test 8: ByteArray new: 8 — exact word boundary ───────────────────── */

static void test_bytearray_new_8(void) {
    printf("  ByteArray new: 8...");
    STA_OOP ba_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_BYTEARRAY);
    STA_OOP result;
    int rc = call_basic_new_size(ba_cls, 8, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->size == 1);
    assert(STA_BYTE_PADDING(h) == 0);

    uint32_t exact = h->size * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
    assert(exact == 8);
    printf(" ok\n");
}

/* ── Test 9: Object new: 5 fails (not indexable) ──────────────────────── */

static void test_object_new_size_fails(void) {
    printf("  Object new: 5 fails...");
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP result;
    int rc = call_basic_new_size(obj_cls, 5, &result);
    assert(rc == STA_PRIM_BAD_RECEIVER);
    printf(" ok\n");
}

/* ── Test 10: Array new: -1 fails (negative size) ─────────────────────── */

static void test_array_negative_size_fails(void) {
    printf("  Array new: -1 fails...");
    STA_OOP arr_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP result;
    int rc = call_basic_new_size(arr_cls, -1, &result);
    assert(rc == STA_PRIM_OUT_OF_RANGE);
    printf(" ok\n");
}

/* ── Test 11: SmallInteger new: fails (immediate) ─────────────────────── */

static void test_smallint_new_size_fails(void) {
    printf("  SmallInteger new: 3 fails...");
    STA_OOP si_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_SMALLINTEGER);
    STA_OOP result;
    int rc = call_basic_new_size(si_cls, 3, &result);
    assert(rc == STA_PRIM_BAD_RECEIVER);
    printf(" ok\n");
}

/* ── Test 12: Verify class_index on pointer- and byte-indexable objects ── */

static void test_class_index_correct(void) {
    printf("  class_index correct on all created objects...");
    STA_OOP result;

    STA_OOP arr_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    assert(call_basic_new_size(arr_cls, 3, &result) == STA_PRIM_SUCCESS);
    assert(((STA_ObjHeader *)(uintptr_t)result)->class_index == STA_CLS_ARRAY);

    STA_OOP ba_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_BYTEARRAY);
    assert(call_basic_new_size(ba_cls, 5, &result) == STA_PRIM_SUCCESS);
    assert(((STA_ObjHeader *)(uintptr_t)result)->class_index == STA_CLS_BYTEARRAY);

    STA_OOP str_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_STRING);
    assert(call_basic_new_size(str_cls, 20, &result) == STA_PRIM_SUCCESS);
    assert(((STA_ObjHeader *)(uintptr_t)result)->class_index == STA_CLS_STRING);

    STA_OOP sym_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_SYMBOL);
    assert(call_basic_new_size(sym_cls, 4, &result) == STA_PRIM_SUCCESS);
    assert(((STA_ObjHeader *)(uintptr_t)result)->class_index == STA_CLS_SYMBOL);

    printf(" ok\n");
}

/* ── Test 13: Large allocation ────────────────────────────────────────── */

static void test_large_array(void) {
    printf("  Array new: 1000...");
    STA_OOP arr_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP result;
    int rc = call_basic_new_size(arr_cls, 1000, &result);
    assert(rc == STA_PRIM_SUCCESS);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->size == 1000);

    /* All slots should be nil. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < 1000; i++) {
        assert(slots[i] == nil_oop);
    }
    printf(" ok\n");
}

/* ── Test 14: Allocation failure ──────────────────────────────────────── */

static void test_allocation_failure(void) {
    printf("  allocation failure on full heap...");
    STA_Heap saved_heap = g_vm->heap;
    sta_heap_init(&g_vm->heap, 256);

    STA_OOP arr_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP result;
    /* Request a huge array that can't fit in 256 bytes. */
    int rc = call_basic_new_size(arr_cls, 1000, &result);
    assert(rc == STA_PRIM_NO_MEMORY);

    sta_heap_deinit(&g_vm->heap);
    g_vm->heap = saved_heap;
    printf(" ok\n");
}

/* ── Test 15: Non-SmallInt argument fails ─────────────────────────────── */

static void test_non_smallint_arg_fails(void) {
    printf("  non-SmallInt argument fails...");
    STA_PrimFn prim = sta_primitives[32];
    STA_OOP arr_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    /* Pass a heap OOP (the class itself) as the size argument. */
    STA_OOP args[2] = { arr_cls, arr_cls };
    STA_OOP result;
    int rc = prim(&g_ctx, args, 1, &result);
    assert(rc == STA_PRIM_BAD_ARGUMENT);
    printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_basic_new_size:\n");
    setup();

    /* Pointer-indexable tests. */
    test_array_new_5();
    test_array_new_0();

    /* Byte-indexable tests. */
    test_bytearray_new_16();
    test_bytearray_new_7();
    test_bytearray_new_0();
    test_string_new_10();
    test_bytearray_new_1();
    test_bytearray_new_8();

    /* Failure cases. */
    test_object_new_size_fails();
    test_array_negative_size_fails();
    test_smallint_new_size_fails();
    test_non_smallint_arg_fails();

    /* Verification. */
    test_class_index_correct();
    test_large_array();
    test_allocation_failure();

    teardown();
    printf("  all basicNew: tests passed.\n");
    return 0;
}
