/* tests/test_object_memory_prims.c
 * Tests for object and memory primitives 33–41 (Phase 1, Epic 6).
 */
#include "vm/primitive_table.h"
#include "vm/special_objects.h"
#include "vm/immutable_space.h"
#include "vm/symbol_table.h"
#include "vm/heap.h"
#include "vm/class_table.h"
#include "vm/format.h"
#include "vm/interpreter.h"
#include "vm/compiled_method.h"
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* ── Shared test infrastructure ────────────────────────────────────────── */

static STA_VM *g_vm;
static STA_ExecContext g_ctx;

static STA_OOP intern(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space, &g_vm->symbol_table, name, strlen(name));
}

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);

    sta_heap_init(&g_vm->heap, 512 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 512 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 512);
    sta_class_table_init(&g_vm->class_table);
    sta_stack_slab_init(&g_vm->slab, 65536);

    sta_special_objects_bind(g_vm->specials);
    sta_primitive_table_init();

    STA_BootstrapResult r = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space, &g_vm->symbol_table, &g_vm->class_table);
    assert(r.status == 0);
    g_ctx.vm = g_vm;
    g_ctx.actor = NULL;
}

static void teardown(void) {
    sta_stack_slab_deinit(&g_vm->slab);
    sta_class_table_deinit(&g_vm->class_table);
    sta_heap_deinit(&g_vm->heap);
    sta_symbol_table_deinit(&g_vm->symbol_table);
    sta_immutable_space_deinit(&g_vm->immutable_space);
    free(g_vm);
}

/* Helper: create an Array of given size with values [10, 20, 30, ...]. */
static STA_OOP make_array(uint32_t count) {
    STA_ObjHeader *h = sta_heap_alloc(&g_vm->heap, STA_CLS_ARRAY, count);
    assert(h);
    STA_OOP *s = sta_payload(h);
    for (uint32_t i = 0; i < count; i++) {
        s[i] = STA_SMALLINT_OOP((intptr_t)(i + 1) * 10);
    }
    return (STA_OOP)(uintptr_t)h;
}

/* Helper: create a ByteArray of given byte count. */
static STA_OOP make_bytearray(uint32_t byte_count) {
    /* Use basicNew: prim to create properly. */
    STA_OOP cls = sta_class_table_get(&g_vm->class_table, STA_CLS_BYTEARRAY);
    STA_OOP args[] = { cls, STA_SMALLINT_OOP((intptr_t)byte_count) };
    STA_OOP result;
    int rc = sta_primitives[32](&g_ctx, args, 1, &result);
    assert(rc == 0);
    return result;
}

/* Helper: create a String of given byte count. */
static STA_OOP make_string(uint32_t byte_count) {
    STA_OOP cls = sta_class_table_get(&g_vm->class_table, STA_CLS_STRING);
    STA_OOP args[] = { cls, STA_SMALLINT_OOP((intptr_t)byte_count) };
    STA_OOP result;
    int rc = sta_primitives[32](&g_ctx, args, 1, &result);
    assert(rc == 0);
    return result;
}

/* Helper: create an Association (key, value). */
static STA_OOP make_association(STA_OOP key, STA_OOP value) {
    STA_ObjHeader *h = sta_heap_alloc(&g_vm->heap, STA_CLS_ASSOCIATION, 2);
    assert(h);
    sta_payload(h)[0] = key;
    sta_payload(h)[1] = value;
    return (STA_OOP)(uintptr_t)h;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Story 1: Indexed access primitives (prims 33–35)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_basic_at_array(void) {
    printf("  basicAt: Array round-trip...");
    STA_OOP arr = make_array(3);
    STA_OOP result;

    /* at: 1 → 10, at: 2 → 20, at: 3 → 30 */
    STA_OOP args[] = { arr, STA_SMALLINT_OOP(1) };
    assert(sta_primitives[33](&g_ctx, args, 1, &result) == 0);
    assert(result == STA_SMALLINT_OOP(10));

    args[1] = STA_SMALLINT_OOP(2);
    assert(sta_primitives[33](&g_ctx, args, 1, &result) == 0);
    assert(result == STA_SMALLINT_OOP(20));

    args[1] = STA_SMALLINT_OOP(3);
    assert(sta_primitives[33](&g_ctx, args, 1, &result) == 0);
    assert(result == STA_SMALLINT_OOP(30));
    printf(" ok\n");
}

static void test_basic_at_bounds(void) {
    printf("  basicAt: bounds checking...");
    STA_OOP arr = make_array(3);
    STA_OOP result;

    /* at: 0 → fail code 2 */
    STA_OOP args[] = { arr, STA_SMALLINT_OOP(0) };
    assert(sta_primitives[33](&g_ctx, args, 1, &result) == 2);

    /* at: 4 → fail code 2 */
    args[1] = STA_SMALLINT_OOP(4);
    assert(sta_primitives[33](&g_ctx, args, 1, &result) == 2);

    /* at: -1 → fail code 2 */
    args[1] = STA_SMALLINT_OOP(-1);
    assert(sta_primitives[33](&g_ctx, args, 1, &result) == 2);
    printf(" ok\n");
}

static void test_basic_at_put_array(void) {
    printf("  basicAt:put: Array round-trip...");
    STA_OOP arr = make_array(3);
    STA_OOP result;

    /* put: 99 at: 2 */
    STA_OOP args[] = { arr, STA_SMALLINT_OOP(2), STA_SMALLINT_OOP(99) };
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 0);
    assert(result == STA_SMALLINT_OOP(99));

    /* Read back */
    STA_OOP args_at[] = { arr, STA_SMALLINT_OOP(2) };
    assert(sta_primitives[33](&g_ctx, args_at, 1, &result) == 0);
    assert(result == STA_SMALLINT_OOP(99));
    printf(" ok\n");
}

static void test_basic_at_bytearray(void) {
    printf("  basicAt:/basicAt:put: ByteArray...");
    STA_OOP ba = make_bytearray(5);
    STA_OOP result;

    /* Write bytes: [65, 0, 255, 128, 1] */
    STA_OOP args[] = { ba, STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(65) };
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 0);

    args[1] = STA_SMALLINT_OOP(3); args[2] = STA_SMALLINT_OOP(255);
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 0);

    args[1] = STA_SMALLINT_OOP(4); args[2] = STA_SMALLINT_OOP(128);
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 0);

    /* Read back */
    STA_OOP args_at[] = { ba, STA_SMALLINT_OOP(1) };
    assert(sta_primitives[33](&g_ctx, args_at, 1, &result) == 0);
    assert(result == STA_SMALLINT_OOP(65));

    args_at[1] = STA_SMALLINT_OOP(3);
    assert(sta_primitives[33](&g_ctx, args_at, 1, &result) == 0);
    assert(result == STA_SMALLINT_OOP(255));

    /* Byte value out of range: -1 → fail code 5 */
    args[1] = STA_SMALLINT_OOP(1); args[2] = STA_SMALLINT_OOP(-1);
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 5);

    /* Byte value out of range: 256 → fail code 5 */
    args[2] = STA_SMALLINT_OOP(256);
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 5);
    printf(" ok\n");
}

static void test_basic_at_string(void) {
    printf("  basicAt:/basicAt:put: String (byte-indexable)...");
    STA_OOP str = make_string(3);
    STA_OOP result;

    /* Write 'A', 'B', 'C' */
    STA_OOP args[] = { str, STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(65) };
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 0);
    args[1] = STA_SMALLINT_OOP(2); args[2] = STA_SMALLINT_OOP(66);
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 0);
    args[1] = STA_SMALLINT_OOP(3); args[2] = STA_SMALLINT_OOP(67);
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 0);

    /* Read back */
    STA_OOP args_at[] = { str, STA_SMALLINT_OOP(2) };
    assert(sta_primitives[33](&g_ctx, args_at, 1, &result) == 0);
    assert(result == STA_SMALLINT_OOP(66)); /* 'B' */
    printf(" ok\n");
}

static void test_basic_at_nonindexable(void) {
    printf("  basicAt: non-indexable fails...");
    STA_OOP assoc = make_association(STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(2));
    STA_OOP result;

    STA_OOP args[] = { assoc, STA_SMALLINT_OOP(1) };
    assert(sta_primitives[33](&g_ctx, args, 1, &result) == 1); /* fail: not indexable */
    printf(" ok\n");
}

static void test_basic_at_put_immutable(void) {
    printf("  basicAt:put: immutable fails...");
    /* Create an immutable array via the immutable space. */
    STA_ObjHeader *h = sta_immutable_alloc(&g_vm->immutable_space, STA_CLS_ARRAY, 3);
    assert(h);
    STA_OOP arr = (STA_OOP)(uintptr_t)h;
    STA_OOP result;

    STA_OOP args[] = { arr, STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(99) };
    assert(sta_primitives[34](&g_ctx, args, 2, &result) == 4); /* fail: immutable */
    printf(" ok\n");
}

static void test_basic_at_arg_not_smallint(void) {
    printf("  basicAt: arg not SmallInteger...");
    STA_OOP arr = make_array(3);
    STA_OOP result;

    STA_OOP args[] = { arr, STA_CHAR_OOP('A') };
    assert(sta_primitives[33](&g_ctx, args, 1, &result) == 3); /* fail: arg not SmallInt */
    printf(" ok\n");
}

static void test_basic_size_array(void) {
    printf("  basicSize Array...");
    STA_OOP arr = make_array(5);
    STA_OOP result;

    STA_OOP args[] = { arr };
    assert(sta_primitives[35](&g_ctx, args, 0, &result) == 0);
    assert(result == STA_SMALLINT_OOP(5));
    printf(" ok\n");
}

static void test_basic_size_bytearray(void) {
    printf("  basicSize ByteArray...");
    STA_OOP ba = make_bytearray(13);
    STA_OOP result;

    STA_OOP args[] = { ba };
    assert(sta_primitives[35](&g_ctx, args, 0, &result) == 0);
    assert(result == STA_SMALLINT_OOP(13));
    printf(" ok\n");
}

static void test_basic_size_nonindexable(void) {
    printf("  basicSize non-indexable = 0...");
    STA_OOP assoc = make_association(STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(2));
    STA_OOP result;

    STA_OOP args[] = { assoc };
    assert(sta_primitives[35](&g_ctx, args, 0, &result) == 0);
    assert(result == STA_SMALLINT_OOP(0));
    printf(" ok\n");
}

static void test_basic_size_string(void) {
    printf("  basicSize String = char count...");
    STA_OOP str = make_string(7);
    STA_OOP result;

    STA_OOP args[] = { str };
    assert(sta_primitives[35](&g_ctx, args, 0, &result) == 0);
    assert(result == STA_SMALLINT_OOP(7));
    printf(" ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Story 2: Hash primitives (prims 36, 40)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_hash_smallint(void) {
    printf("  hash SmallInteger == value...");
    STA_OOP result;
    STA_OOP args[] = { STA_SMALLINT_OOP(42) };
    assert(sta_primitives[36](&g_ctx, args, 0, &result) == 0);
    assert(result == STA_SMALLINT_OOP(42));

    args[0] = STA_SMALLINT_OOP(0);
    assert(sta_primitives[36](&g_ctx, args, 0, &result) == 0);
    assert(result == STA_SMALLINT_OOP(0));
    printf(" ok\n");
}

static void test_hash_symbol(void) {
    printf("  hash Symbol == FNV-1a...");
    STA_OOP sym_oop = intern("hello");
    assert(sym_oop != 0);

    uint32_t fnv = sta_symbol_get_hash(sym_oop);
    STA_OOP expected = STA_SMALLINT_OOP((intptr_t)(fnv & 0x3FFFFFFFu));

    STA_OOP result;
    STA_OOP args[] = { sym_oop };
    assert(sta_primitives[36](&g_ctx, args, 0, &result) == 0);
    assert(result == expected);
    printf(" ok\n");
}

static void test_identity_hash_stable(void) {
    printf("  identityHash stable across calls...");
    STA_OOP arr = make_array(3);
    STA_OOP result1, result2;

    STA_OOP args[] = { arr };
    assert(sta_primitives[40](&g_ctx, args, 0, &result1) == 0);
    assert(sta_primitives[40](&g_ctx, args, 0, &result2) == 0);
    assert(result1 == result2);
    printf(" ok\n");
}

static void test_identity_hash_distribution(void) {
    printf("  identityHash distribution (<5%% collision)...");
    #define N_OBJS 100
    STA_OOP hashes[N_OBJS];

    for (int i = 0; i < N_OBJS; i++) {
        STA_OOP arr = make_array(1);
        STA_OOP args[] = { arr };
        assert(sta_primitives[40](&g_ctx, args, 0, &hashes[i]) == 0);
    }

    int collisions = 0;
    for (int i = 0; i < N_OBJS; i++) {
        for (int j = i + 1; j < N_OBJS; j++) {
            if (hashes[i] == hashes[j]) collisions++;
        }
    }

    /* With 100 objects, expect < 5 collisions (< 5% of 100). */
    assert(collisions < 5);
    printf(" ok (%d collisions)\n", collisions);
    #undef N_OBJS
}

/* ══════════════════════════════════════════════════════════════════════════
 * Story 3: Reflective access primitives (prims 38–39)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_inst_var_at_association(void) {
    printf("  instVarAt: Association...");
    STA_OOP key = STA_SMALLINT_OOP(42);
    STA_OOP val = STA_SMALLINT_OOP(99);
    STA_OOP assoc = make_association(key, val);
    STA_OOP result;

    /* instVarAt: 1 → key */
    STA_OOP args[] = { assoc, STA_SMALLINT_OOP(1) };
    assert(sta_primitives[38](&g_ctx, args, 1, &result) == 0);
    assert(result == key);

    /* instVarAt: 2 → value */
    args[1] = STA_SMALLINT_OOP(2);
    assert(sta_primitives[38](&g_ctx, args, 1, &result) == 0);
    assert(result == val);
    printf(" ok\n");
}

static void test_inst_var_at_put_roundtrip(void) {
    printf("  instVarAt:put: round-trip...");
    STA_OOP assoc = make_association(STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(2));
    STA_OOP result;

    /* Write new value at slot 2. */
    STA_OOP args[] = { assoc, STA_SMALLINT_OOP(2), STA_SMALLINT_OOP(77) };
    assert(sta_primitives[39](&g_ctx, args, 2, &result) == 0);
    assert(result == STA_SMALLINT_OOP(77));

    /* Read back. */
    STA_OOP args_at[] = { assoc, STA_SMALLINT_OOP(2) };
    assert(sta_primitives[38](&g_ctx, args_at, 1, &result) == 0);
    assert(result == STA_SMALLINT_OOP(77));
    printf(" ok\n");
}

static void test_inst_var_at_bounds(void) {
    printf("  instVarAt: bounds checking...");
    STA_OOP assoc = make_association(STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(2));
    STA_OOP result;

    /* Index 0 fails. */
    STA_OOP args[] = { assoc, STA_SMALLINT_OOP(0) };
    assert(sta_primitives[38](&g_ctx, args, 1, &result) == 2);

    /* Index 3 fails (Association has 2 instVars). */
    args[1] = STA_SMALLINT_OOP(3);
    assert(sta_primitives[38](&g_ctx, args, 1, &result) == 2);
    printf(" ok\n");
}

static void test_inst_var_at_array_fails(void) {
    printf("  instVarAt: Array fails (0 named instVars)...");
    STA_OOP arr = make_array(3);
    STA_OOP result;

    STA_OOP args[] = { arr, STA_SMALLINT_OOP(1) };
    assert(sta_primitives[38](&g_ctx, args, 1, &result) == 2);
    printf(" ok\n");
}

static void test_inst_var_at_put_immutable(void) {
    printf("  instVarAt:put: immutable fails...");
    STA_ObjHeader *h = sta_immutable_alloc(&g_vm->immutable_space, STA_CLS_ASSOCIATION, 2);
    assert(h);
    STA_OOP assoc = (STA_OOP)(uintptr_t)h;
    STA_OOP result;

    STA_OOP args[] = { assoc, STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(99) };
    assert(sta_primitives[39](&g_ctx, args, 2, &result) == 4);
    printf(" ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Story 4: become: (prim 37)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_become_associations(void) {
    printf("  become: swaps two Associations...");
    STA_OOP a = make_association(STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(2));
    STA_OOP b = make_association(STA_SMALLINT_OOP(3), STA_SMALLINT_OOP(4));
    STA_OOP result;

    STA_OOP args[] = { a, b };
    assert(sta_primitives[37](&g_ctx, args, 1, &result) == 0);

    STA_ObjHeader *ha = (STA_ObjHeader *)(uintptr_t)a;
    STA_ObjHeader *hb = (STA_ObjHeader *)(uintptr_t)b;
    assert(sta_payload(ha)[0] == STA_SMALLINT_OOP(3));
    assert(sta_payload(ha)[1] == STA_SMALLINT_OOP(4));
    assert(sta_payload(hb)[0] == STA_SMALLINT_OOP(1));
    assert(sta_payload(hb)[1] == STA_SMALLINT_OOP(2));
    printf(" ok\n");
}

static void test_become_immutable_fails(void) {
    printf("  become: immutable fails...");
    STA_ObjHeader *h = sta_immutable_alloc(&g_vm->immutable_space, STA_CLS_ASSOCIATION, 2);
    assert(h);
    STA_OOP imm = (STA_OOP)(uintptr_t)h;
    STA_OOP mut = make_association(STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(2));
    STA_OOP result;

    STA_OOP args[] = { imm, mut };
    assert(sta_primitives[37](&g_ctx, args, 1, &result) == 1);

    args[0] = mut; args[1] = imm;
    assert(sta_primitives[37](&g_ctx, args, 1, &result) == 1);
    printf(" ok\n");
}

static void test_become_smallint_fails(void) {
    printf("  become: SmallInteger fails...");
    STA_OOP result;
    STA_OOP args[] = { STA_SMALLINT_OOP(5), STA_SMALLINT_OOP(6) };
    assert(sta_primitives[37](&g_ctx, args, 1, &result) == 1);
    printf(" ok\n");
}

static void test_become_different_size_fails(void) {
    printf("  become: different-sized fails...");
    STA_OOP arr2 = make_array(2);
    STA_OOP arr5 = make_array(5);
    STA_OOP result;

    STA_OOP args[] = { arr2, arr5 };
    assert(sta_primitives[37](&g_ctx, args, 1, &result) == 6);
    printf(" ok\n");
}

static void test_become_yourself_noop(void) {
    printf("  become: yourself is no-op...");
    STA_OOP assoc = make_association(STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(2));
    STA_OOP result;

    STA_OOP args[] = { assoc, assoc };
    assert(sta_primitives[37](&g_ctx, args, 1, &result) == 0);
    assert(result == assoc);

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)assoc;
    assert(sta_payload(h)[0] == STA_SMALLINT_OOP(1));
    assert(sta_payload(h)[1] == STA_SMALLINT_OOP(2));
    printf(" ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Story 5: shallowCopy (prim 41)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_shallow_copy_array(void) {
    printf("  shallowCopy Array: equal but not identical...");
    STA_OOP arr = make_array(3);
    STA_OOP result;

    STA_OOP args[] = { arr };
    assert(sta_primitives[41](&g_ctx, args, 0, &result) == 0);
    assert(result != arr);

    STA_ObjHeader *orig_h = (STA_ObjHeader *)(uintptr_t)arr;
    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)result;
    assert(copy_h->class_index == orig_h->class_index);
    assert(copy_h->size == orig_h->size);
    for (uint32_t i = 0; i < orig_h->size; i++) {
        assert(sta_payload(copy_h)[i] == sta_payload(orig_h)[i]);
    }
    printf(" ok\n");
}

static void test_shallow_copy_independent(void) {
    printf("  shallowCopy: modify copy doesn't affect original...");
    STA_OOP arr = make_array(3);
    STA_OOP result;

    STA_OOP args[] = { arr };
    assert(sta_primitives[41](&g_ctx, args, 0, &result) == 0);

    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)result;
    sta_payload(copy_h)[0] = STA_SMALLINT_OOP(999);

    STA_ObjHeader *orig_h = (STA_ObjHeader *)(uintptr_t)arr;
    assert(sta_payload(orig_h)[0] == STA_SMALLINT_OOP(10));
    printf(" ok\n");
}

static void test_shallow_copy_smallint(void) {
    printf("  shallowCopy SmallInteger returns self...");
    STA_OOP result;
    STA_OOP args[] = { STA_SMALLINT_OOP(42) };
    assert(sta_primitives[41](&g_ctx, args, 0, &result) == 0);
    assert(result == STA_SMALLINT_OOP(42));
    printf(" ok\n");
}

static void test_shallow_copy_immutable_produces_mutable(void) {
    printf("  shallowCopy of immutable is mutable...");
    STA_ObjHeader *h = sta_immutable_alloc(&g_vm->immutable_space, STA_CLS_ARRAY, 3);
    assert(h);
    STA_OOP imm_arr = (STA_OOP)(uintptr_t)h;
    STA_OOP result;

    STA_OOP args[] = { imm_arr };
    assert(sta_primitives[41](&g_ctx, args, 0, &result) == 0);

    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)result;
    assert(!(copy_h->obj_flags & STA_OBJ_IMMUTABLE));
    printf(" ok\n");
}

/* ══════════════════════════════════════════════════════════════════════════
 * Story 6: Bootstrap installation — interpreter-level tests
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_send_hash_to_smallint(void) {
    printf("  send #hash to SmallInteger...");
    STA_OOP hash_sel = intern("hash");
    STA_OOP lits[] = { hash_sel, STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_SMALLINT, 42,
        OP_SEND, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 2, bc, sizeof(bc));
    assert(method);

    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(42));
    printf(" ok\n");
}

static void test_send_shallow_copy_to_array(void) {
    printf("  send #shallowCopy to Array...");
    STA_OOP arr = make_array(3);
    STA_OOP copy_sel = intern("shallowCopy");
    STA_OOP lits[] = { arr, copy_sel, STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_LIT, 0,
        OP_SEND, 1,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 3, bc, sizeof(bc));
    assert(method);

    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result != arr);
    assert(STA_IS_HEAP(result));
    STA_ObjHeader *rh = (STA_ObjHeader *)(uintptr_t)result;
    assert(rh->class_index == STA_CLS_ARRAY);
    assert(rh->size == 3);
    printf(" ok\n");
}

static void test_send_inst_var_at_to_association(void) {
    printf("  send #instVarAt: 1 to Association...");
    STA_OOP assoc = make_association(STA_SMALLINT_OOP(42), STA_SMALLINT_OOP(99));
    STA_OOP ivar_sel = intern("instVarAt:");
    STA_OOP lits[] = { assoc, ivar_sel, STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_LIT, 0,
        OP_PUSH_SMALLINT, 1,
        OP_SEND, 1,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 3, bc, sizeof(bc));
    assert(method);

    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(42));
    printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_object_memory_prims:\n");
    setup();

    /* Story 1: Indexed access (prims 33–35) */
    test_basic_at_array();
    test_basic_at_bounds();
    test_basic_at_put_array();
    test_basic_at_bytearray();
    test_basic_at_string();
    test_basic_at_nonindexable();
    test_basic_at_put_immutable();
    test_basic_at_arg_not_smallint();
    test_basic_size_array();
    test_basic_size_bytearray();
    test_basic_size_nonindexable();
    test_basic_size_string();

    /* Story 2: Hash (prims 36, 40) */
    test_hash_smallint();
    test_hash_symbol();
    test_identity_hash_stable();
    test_identity_hash_distribution();

    /* Story 3: Reflective access (prims 38–39) */
    test_inst_var_at_association();
    test_inst_var_at_put_roundtrip();
    test_inst_var_at_bounds();
    test_inst_var_at_array_fails();
    test_inst_var_at_put_immutable();

    /* Story 4: become: (prim 37) */
    test_become_associations();
    test_become_immutable_fails();
    test_become_smallint_fails();
    test_become_different_size_fails();
    test_become_yourself_noop();

    /* Story 5: shallowCopy (prim 41) */
    test_shallow_copy_array();
    test_shallow_copy_independent();
    test_shallow_copy_smallint();
    test_shallow_copy_immutable_produces_mutable();

    /* Story 6: Bootstrap installation — interpreter-level */
    test_send_hash_to_smallint();
    test_send_shallow_copy_to_array();
    test_send_inst_var_at_to_association();

    teardown();
    printf("All object_memory_prims tests passed.\n");
    return 0;
}
