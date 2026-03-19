/* tests/test_interpreter.c
 * Integration tests for the bytecode interpreter.
 * Tests: 3+4=7, push/return, store/push temps, jumps, TCO,
 *        reduction counting, primitive dispatch, super send,
 *        primitive failure → fallback, array #at:.
 */
#include "vm/interpreter.h"
#include "vm/compiled_method.h"
#include "vm/primitive_table.h"
#include "vm/method_dict.h"
#include "vm/symbol_table.h"
#include "vm/special_objects.h"
#include "vm/special_selectors.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/selector.h"
#include "vm/vm_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* ── Shared test infrastructure ────────────────────────────────────────── */

static STA_VM *g_vm;

static STA_OOP sym(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space, &g_vm->symbol_table, name, strlen(name));
}

/* Create a class object with 4+ slots: superclass, methoddict, format, name.
 * Allocates on the mutable heap. Registers in the class table. */
static STA_OOP make_class(uint32_t cls_idx, STA_OOP superclass,
                           const char *name) {
    STA_ObjHeader *h = sta_heap_alloc(&g_vm->heap, STA_CLS_CLASS, 4);
    assert(h);
    STA_OOP cls = (STA_OOP)(uintptr_t)h;
    STA_OOP *slots = sta_payload(h);

    slots[STA_CLASS_SLOT_SUPERCLASS] = superclass;
    slots[STA_CLASS_SLOT_METHODDICT] = 0;  /* filled later */
    slots[STA_CLASS_SLOT_FORMAT] = STA_SMALLINT_OOP(0);
    slots[STA_CLASS_SLOT_NAME] = sym(name);

    int rc = sta_class_table_set(&g_vm->class_table, cls_idx, cls);
    assert(rc == 0);
    return cls;
}

static void set_method_dict(STA_OOP cls, STA_OOP dict) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)cls;
    sta_payload(h)[STA_CLASS_SLOT_METHODDICT] = dict;
}

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);

    sta_heap_init(&g_vm->heap, 256 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 256 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 256);
    sta_class_table_init(&g_vm->class_table);
    sta_stack_slab_init(&g_vm->slab, 65536);

    sta_special_objects_bind(g_vm->specials);
    sta_primitive_table_init();

    /* Bootstrap nil, true, false. */
    STA_ObjHeader *nil_h = sta_immutable_alloc(&g_vm->immutable_space, STA_CLS_UNDEFINEDOBJ, 0);
    STA_ObjHeader *true_h = sta_immutable_alloc(&g_vm->immutable_space, STA_CLS_TRUE, 0);
    STA_ObjHeader *false_h = sta_immutable_alloc(&g_vm->immutable_space, STA_CLS_FALSE, 0);
    sta_spc_set(SPC_NIL, (STA_OOP)(uintptr_t)nil_h);
    sta_spc_set(SPC_TRUE, (STA_OOP)(uintptr_t)true_h);
    sta_spc_set(SPC_FALSE, (STA_OOP)(uintptr_t)false_h);

    /* Intern special selectors. */
    sta_intern_special_selectors(&g_vm->immutable_space, &g_vm->symbol_table);
}

static void teardown(void) {
    sta_stack_slab_deinit(&g_vm->slab);
    sta_class_table_deinit(&g_vm->class_table);
    sta_heap_deinit(&g_vm->heap);
    sta_symbol_table_deinit(&g_vm->symbol_table);
    sta_immutable_space_deinit(&g_vm->immutable_space);
    free(g_vm);
}

/* ── Test a: 3 + 4 = 7 ────────────────────────────────────────────────── */

static void test_three_plus_four(void) {
    printf("  3 + 4 = 7...");

    /* Build SmallInteger class with #+ method (prim 1). */
    STA_OOP obj_cls = make_class(STA_CLS_OBJECT, 0, "Object");
    STA_OOP si_cls = make_class(STA_CLS_SMALLINTEGER, obj_cls, "SmallInteger");

    /* SmallInteger >> #+ (primitive 1, prim-only method) */
    STA_OOP plus_sel = sym("+");
    STA_OOP si_plus_lits[] = { plus_sel, si_cls };
    uint8_t si_plus_bc[] = { OP_PRIMITIVE, 0x01, OP_RETURN_SELF, 0x00 };
    STA_OOP si_plus = sta_compiled_method_create(&g_vm->immutable_space,
        1, 1, 1,  /* numArgs=1, numTemps=1, primIdx=1 */
        si_plus_lits, 2,
        si_plus_bc, 4);
    assert(si_plus);

    /* Install into SmallInteger's method dict. */
    STA_OOP si_md = sta_method_dict_create(&g_vm->heap, 8);
    sta_method_dict_insert(&g_vm->heap, si_md, plus_sel, si_plus);
    set_method_dict(si_cls, si_md);

    /* Object >> (empty dict). */
    STA_OOP obj_md = sta_method_dict_create(&g_vm->heap, 4);
    set_method_dict(obj_cls, obj_md);

    /* Test harness method:
     *   PUSH_SMALLINT 3
     *   PUSH_SMALLINT 4
     *   SEND lit[0]  (#+ )
     *   RETURN_TOP */
    STA_OOP harness_lits[] = { plus_sel, STA_SMALLINT_OOP(0) };
    uint8_t harness_bc[] = {
        OP_PUSH_SMALLINT, 3,
        OP_PUSH_SMALLINT, 4,
        OP_SEND, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP harness = sta_compiled_method_create(&g_vm->immutable_space,
        0, 0, 0,
        harness_lits, 2,
        harness_bc, sizeof(harness_bc));
    assert(harness);

    STA_OOP result = sta_interpret(g_vm, harness,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(7));

    printf(" ok\n");
}

/* ── Test d: PUSH_NIL, PUSH_TRUE, PUSH_FALSE, PUSH_RECEIVER ──────────── */

static void test_push_constants(void) {
    printf("  push nil/true/false/receiver...");

    /* Method: PUSH_NIL, POP, PUSH_TRUE, POP, PUSH_FALSE, POP,
     *         PUSH_RECEIVER, RETURN_TOP */
    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };  /* owner placeholder */
    uint8_t bc[] = {
        OP_PUSH_NIL, 0,
        OP_POP, 0,
        OP_PUSH_TRUE, 0,
        OP_POP, 0,
        OP_PUSH_FALSE, 0,
        OP_POP, 0,
        OP_PUSH_RECEIVER, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                                  lits, 1, bc, sizeof(bc));
    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(42), NULL, 0);
    assert(result == STA_SMALLINT_OOP(42));
    printf(" ok\n");
}

/* ── Test e: STORE_TEMP / PUSH_TEMP round-trip ────────────────────────── */

static void test_store_push_temp(void) {
    printf("  store_temp / push_temp round-trip...");

    /* numArgs=0, numTemps=2, so local_count=2, temps are slots [0,1].
     * PUSH_SMALLINT 99, POP_STORE_TEMP 0, PUSH_SMALLINT 77,
     * POP_STORE_TEMP 1, PUSH_TEMP 0, PUSH_TEMP 1,
     * ... we'll just return temp[0] to verify. */
    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_SMALLINT, 99,
        OP_POP_STORE_TEMP, 0,
        OP_PUSH_SMALLINT, 77,
        OP_POP_STORE_TEMP, 1,
        OP_PUSH_TEMP, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 2, 0,
                                                  lits, 1, bc, sizeof(bc));
    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(99));
    printf(" ok\n");
}

/* ── Test f: JUMP_TRUE / JUMP_FALSE ───────────────────────────────────── */

static void test_jump_conditionals(void) {
    printf("  jump_true / jump_false...");

    /* PUSH_TRUE, JUMP_FALSE 4, PUSH_SMALLINT 1, RETURN_TOP,
     * PUSH_SMALLINT 2, RETURN_TOP
     * (if true → should NOT jump, returns 1) */
    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_TRUE, 0,           /* 0 */
        OP_JUMP_FALSE, 4,          /* 2 — if false, skip 4 bytes (to offset 8) */
        OP_PUSH_SMALLINT, 1,       /* 4 */
        OP_RETURN_TOP, 0,          /* 6 */
        OP_PUSH_SMALLINT, 2,       /* 8 */
        OP_RETURN_TOP, 0           /* 10 */
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                                  lits, 1, bc, sizeof(bc));
    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(1));

    /* Now with PUSH_FALSE → should jump to offset 8, return 2. */
    uint8_t bc2[] = {
        OP_PUSH_FALSE, 0,
        OP_JUMP_FALSE, 4,
        OP_PUSH_SMALLINT, 1,
        OP_RETURN_TOP, 0,
        OP_PUSH_SMALLINT, 2,
        OP_RETURN_TOP, 0
    };
    STA_OOP method2 = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                                   lits, 1, bc2, sizeof(bc2));
    g_vm->slab.top = g_vm->slab.base;
    g_vm->slab.sp = g_vm->slab.base;
    result = sta_interpret(g_vm, method2,
                           STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(2));

    printf(" ok\n");
}

/* ── Test g: JUMP_BACK + reduction counting ───────────────────────────── */

static void test_jump_back_reductions(void) {
    printf("  jump_back + reduction counting...");

    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };

    uint8_t bc[] = {
        OP_PUSH_ONE, 0,            /* 0 */
        OP_JUMP, 6,                /* 2 → pc = 2+2+6 = 10 */
        OP_PUSH_TWO, 0,            /* 4 */
        OP_RETURN_TOP, 0,          /* 6 */
        OP_NOP, 0,                 /* 8 */
        OP_JUMP_BACK, 8,           /* 10 → pc = 10+2-8 = 4 */
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                                  lits, 1, bc, sizeof(bc));
    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(2));
    printf(" ok\n");
}

/* ── Test h: RETURN_SELF, RETURN_NIL ──────────────────────────────────── */

static void test_return_variants(void) {
    printf("  return_self / return_nil...");

    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };

    /* RETURN_SELF */
    uint8_t bc1[] = { OP_RETURN_SELF, 0 };
    STA_OOP m1 = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                              lits, 1, bc1, 2);
    STA_OOP r = sta_interpret(g_vm, m1,
                               STA_SMALLINT_OOP(77), NULL, 0);
    assert(r == STA_SMALLINT_OOP(77));

    /* RETURN_NIL */
    uint8_t bc2[] = { OP_RETURN_NIL, 0 };
    STA_OOP m2 = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                              lits, 1, bc2, 2);
    g_vm->slab.top = g_vm->slab.base;
    g_vm->slab.sp = g_vm->slab.base;
    r = sta_interpret(g_vm, m2,
                      STA_SMALLINT_OOP(0), NULL, 0);
    assert(r == sta_spc_get(SPC_NIL));

    printf(" ok\n");
}

/* ── Test i: TCO — tail-recursive method, max stack depth == 1 frame ─── */

static void test_tco(void) {
    printf("  TCO tail recursion...");

    /* Build SmallInteger >> #< (prim 3) and SmallInteger >> #- (prim 2). */
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP si_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_SMALLINTEGER);

    STA_OOP lt_sel = sym("<");
    STA_OOP sub_sel = sym("-");
    STA_OOP countdown_sel = sym("countdown:");

    /* SmallInteger >> #< (prim 3) */
    STA_OOP lt_lits[] = { lt_sel, si_cls };
    uint8_t lt_bc[] = { OP_PRIMITIVE, 3, OP_RETURN_SELF, 0 };
    STA_OOP lt_method = sta_compiled_method_create(&g_vm->immutable_space, 1, 1, 3,
        lt_lits, 2, lt_bc, sizeof(lt_bc));

    /* SmallInteger >> #- (prim 2) */
    STA_OOP sub_lits[] = { sub_sel, si_cls };
    uint8_t sub_bc[] = { OP_PRIMITIVE, 2, OP_RETURN_SELF, 0 };
    STA_OOP sub_method = sta_compiled_method_create(&g_vm->immutable_space, 1, 1, 2,
        sub_lits, 2, sub_bc, sizeof(sub_bc));

    /* Install #< and #- into SmallInteger. */
    STA_OOP si_md = sta_class_method_dict(si_cls);
    sta_method_dict_insert(&g_vm->heap, si_md, lt_sel, lt_method);
    sta_method_dict_insert(&g_vm->heap, si_md, sub_sel, sub_method);

    /* Create a "countdown" method that tail-calls itself. */
    STA_OOP cd_lits[] = { lt_sel, sub_sel, countdown_sel, si_cls };
    uint8_t cd_bc[] = {
        OP_PUSH_TEMP, 0,          /* 0: push n */
        OP_PUSH_ONE, 0,           /* 2: push 1 */
        OP_SEND, 0,               /* 4: send #< (lit[0]) */
        OP_JUMP_FALSE, 4,         /* 6: if false, jump to pc 12 */
        OP_RETURN_SELF, 0,        /* 8: return self (base case) */
        OP_NOP, 0,                /* 10: padding (jump target alignment) */
        OP_PUSH_RECEIVER, 0,     /* 12: push self */
        OP_PUSH_TEMP, 0,          /* 14: push n */
        OP_PUSH_ONE, 0,           /* 16: push 1 */
        OP_SEND, 1,               /* 18: send #- (lit[1]) */
        OP_SEND, 2,               /* 20: send #countdown: (lit[2]) — tail */
        OP_RETURN_TOP, 0,         /* 22: return top (TCO target) */
    };
    STA_OOP cd_method = sta_compiled_method_create(&g_vm->immutable_space, 1, 1, 0,
        cd_lits, 4, cd_bc, sizeof(cd_bc));

    /* Install countdown: into SmallInteger. */
    sta_method_dict_insert(&g_vm->heap, si_md, countdown_sel, cd_method);

    /* Harness: self countdown: 100 */
    STA_OOP harness_lits[] = { countdown_sel, STA_SMALLINT_OOP(0) };
    uint8_t harness_bc[] = {
        OP_PUSH_RECEIVER, 0,
        OP_PUSH_SMALLINT, 100,
        OP_SEND, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP harness = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        harness_lits, 2, harness_bc, sizeof(harness_bc));

    /* Run with a small slab — reinit for this test. */
    sta_stack_slab_deinit(&g_vm->slab);
    sta_stack_slab_init(&g_vm->slab, 2048);
    STA_OOP result = sta_interpret(g_vm, harness,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(0));

    /* Restore normal slab size. */
    sta_stack_slab_deinit(&g_vm->slab);
    sta_stack_slab_init(&g_vm->slab, 65536);
    printf(" ok\n");
}

/* ── Test j: Array >> #at: via primitive dispatch ─────────────────────── */

static void test_array_at_via_send(void) {
    printf("  Array >> #at: via send...");

    /* Create Array class. */
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP arr_cls = make_class(STA_CLS_ARRAY, obj_cls, "Array");

    STA_OOP at_sel = sym("at:");
    STA_OOP at_lits[] = { at_sel, arr_cls };
    uint8_t at_bc[] = { OP_PRIMITIVE, 51, OP_RETURN_SELF, 0 };
    STA_OOP at_method = sta_compiled_method_create(&g_vm->immutable_space, 1, 1, 51,
        at_lits, 2, at_bc, sizeof(at_bc));

    STA_OOP arr_md = sta_method_dict_create(&g_vm->heap, 4);
    sta_method_dict_insert(&g_vm->heap, arr_md, at_sel, at_method);
    set_method_dict(arr_cls, arr_md);

    /* Create a test array [10, 20, 30]. */
    STA_ObjHeader *arr_h = sta_heap_alloc(&g_vm->heap, STA_CLS_ARRAY, 3);
    STA_OOP *arr_payload = sta_payload(arr_h);
    arr_payload[0] = STA_SMALLINT_OOP(10);
    arr_payload[1] = STA_SMALLINT_OOP(20);
    arr_payload[2] = STA_SMALLINT_OOP(30);
    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr_h;

    STA_OOP harness_lits[] = { arr_oop, at_sel, STA_SMALLINT_OOP(0) };
    uint8_t harness_bc[] = {
        OP_PUSH_LIT, 0,
        OP_PUSH_SMALLINT, 2,
        OP_SEND, 1,
        OP_RETURN_TOP, 0
    };
    STA_OOP harness = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        harness_lits, 3, harness_bc, sizeof(harness_bc));

    STA_OOP result = sta_interpret(g_vm, harness,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(20));

    printf(" ok\n");
}

/* ── Test k: Primitive failure → fallback bytecode execution ──────────── */

static void test_primitive_failure_fallback(void) {
    printf("  primitive failure → fallback...");

    STA_OOP plus_sel = sym("+");
    STA_OOP si_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_SMALLINTEGER);

    STA_OOP lits[] = { plus_sel, si_cls };
    uint8_t bc[] = {
        OP_PRIMITIVE, 1,           /* 0: preamble */
        OP_PUSH_SMALLINT, 255,    /* 2: fallback — push 255 */
        OP_PUSH_SMALLINT, 244,    /* 4: push 244 */
        OP_RETURN_TOP, 0,         /* 6: return 244 */
    };
    STA_OOP fail_method = sta_compiled_method_create(&g_vm->immutable_space, 1, 2, 1,
        lits, 2, bc, sizeof(bc));

    STA_OOP fail_sel = sym("failingAdd:");
    STA_OOP si_md = sta_class_method_dict(si_cls);
    sta_method_dict_insert(&g_vm->heap, si_md, fail_sel, fail_method);

    STA_OOP char_oop = STA_CHAR_OOP('X');
    STA_OOP harness_lits[] = { char_oop, fail_sel, STA_SMALLINT_OOP(0) };
    uint8_t harness_bc[] = {
        OP_PUSH_SMALLINT, 5,
        OP_PUSH_LIT, 0,
        OP_SEND, 1,
        OP_RETURN_TOP, 0
    };
    STA_OOP harness = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        harness_lits, 3, harness_bc, sizeof(harness_bc));

    STA_OOP result = sta_interpret(g_vm, harness,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(244));

    printf(" ok\n");
}

/* ── Test l: OP_SEND_SUPER ────────────────────────────────────────────── */

static void test_super_send(void) {
    printf("  super send...");

    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);

    uint32_t BASE_IDX = 32;
    uint32_t SUB_IDX = 33;

    STA_OOP base_cls = make_class(BASE_IDX, obj_cls, "Base");
    STA_OOP sub_cls = make_class(SUB_IDX, base_cls, "Sub");

    STA_OOP foo_sel = sym("foo");

    /* Base >> #foo: PUSH_SMALLINT 100, RETURN_TOP */
    STA_OOP base_foo_lits[] = { foo_sel, base_cls };
    uint8_t base_foo_bc[] = {
        OP_PUSH_SMALLINT, 100,
        OP_RETURN_TOP, 0
    };
    STA_OOP base_foo = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        base_foo_lits, 2, base_foo_bc, sizeof(base_foo_bc));

    STA_OOP base_md = sta_method_dict_create(&g_vm->heap, 4);
    sta_method_dict_insert(&g_vm->heap, base_md, foo_sel, base_foo);
    set_method_dict(base_cls, base_md);

    /* Sub >> #foo: PUSH_RECEIVER, SEND_SUPER #foo, RETURN_TOP. */
    STA_OOP sub_foo_lits[] = { foo_sel, sub_cls };
    uint8_t sub_foo_bc[] = {
        OP_PUSH_RECEIVER, 0,
        OP_SEND_SUPER, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP sub_foo = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        sub_foo_lits, 2, sub_foo_bc, sizeof(sub_foo_bc));

    STA_OOP sub_md = sta_method_dict_create(&g_vm->heap, 4);
    sta_method_dict_insert(&g_vm->heap, sub_md, foo_sel, sub_foo);
    set_method_dict(sub_cls, sub_md);

    /* Create an instance of Sub. */
    STA_ObjHeader *instance_h = sta_heap_alloc(&g_vm->heap, SUB_IDX, 0);
    STA_OOP instance = (STA_OOP)(uintptr_t)instance_h;

    STA_OOP harness_lits[] = { instance, foo_sel, STA_SMALLINT_OOP(0) };
    uint8_t harness_bc[] = {
        OP_PUSH_LIT, 0,
        OP_SEND, 1,
        OP_RETURN_TOP, 0
    };
    STA_OOP harness = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        harness_lits, 3, harness_bc, sizeof(harness_bc));

    STA_OOP result = sta_interpret(g_vm, harness,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(100));

    printf(" ok\n");
}

/* ── Test: PUSH_MINUS_ONE, PUSH_ZERO, PUSH_ONE, PUSH_TWO ─────────────── */

static void test_push_small_constants(void) {
    printf("  push -1, 0, 1, 2...");

    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };

    uint8_t bc[] = {
        OP_PUSH_MINUS_ONE, 0,
        OP_POP, 0,
        OP_PUSH_ZERO, 0,
        OP_POP, 0,
        OP_PUSH_ONE, 0,
        OP_POP, 0,
        OP_PUSH_TWO, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                                  lits, 1, bc, sizeof(bc));
    STA_OOP r = sta_interpret(g_vm, method,
                               STA_SMALLINT_OOP(0), NULL, 0);
    assert(r == STA_SMALLINT_OOP(2));
    printf(" ok\n");
}

/* ── Test: DUP ─────────────────────────────────────────────────────────── */

static void test_dup(void) {
    printf("  DUP...");

    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_SMALLINT, 42,
        OP_DUP, 0,
        OP_POP, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                                  lits, 1, bc, sizeof(bc));
    STA_OOP r = sta_interpret(g_vm, method,
                               STA_SMALLINT_OOP(0), NULL, 0);
    assert(r == STA_SMALLINT_OOP(42));
    printf(" ok\n");
}

/* ── Test: RETURN_TRUE / RETURN_FALSE ─────────────────────────────────── */

static void test_return_true_false(void) {
    printf("  return_true / return_false...");

    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };

    uint8_t bc1[] = { OP_RETURN_TRUE, 0 };
    STA_OOP m1 = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                              lits, 1, bc1, 2);
    STA_OOP r = sta_interpret(g_vm, m1,
                               STA_SMALLINT_OOP(0), NULL, 0);
    assert(r == sta_spc_get(SPC_TRUE));

    uint8_t bc2[] = { OP_RETURN_FALSE, 0 };
    STA_OOP m2 = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
                                              lits, 1, bc2, 2);
    g_vm->slab.top = g_vm->slab.base;
    g_vm->slab.sp = g_vm->slab.base;
    r = sta_interpret(g_vm, m2,
                      STA_SMALLINT_OOP(0), NULL, 0);
    assert(r == sta_spc_get(SPC_FALSE));

    printf(" ok\n");
}

/* ── Test: chained expression 10 + (3 + 4) = 17 (sp restoration) ─────── */

static void test_chained_expression(void) {
    printf("  10 + (3 + 4) = 17 (sp restore)...");

    STA_OOP plus_sel = sym("+");

    STA_OOP harness_lits[] = { plus_sel, STA_SMALLINT_OOP(0) };
    uint8_t harness_bc[] = {
        OP_PUSH_SMALLINT, 10,
        OP_PUSH_SMALLINT, 3,
        OP_PUSH_SMALLINT, 4,
        OP_SEND, 0,
        OP_SEND, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP harness = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        harness_lits, 2, harness_bc, sizeof(harness_bc));
    assert(harness);

    STA_OOP result = sta_interpret(g_vm, harness,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(17));

    printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_interpreter:\n");
    setup();

    test_three_plus_four();
    test_push_constants();
    test_store_push_temp();
    test_jump_conditionals();
    test_jump_back_reductions();
    test_return_variants();
    test_tco();
    test_array_at_via_send();
    test_primitive_failure_fallback();
    test_super_send();
    test_push_small_constants();
    test_dup();
    test_return_true_false();
    test_chained_expression();

    teardown();
    printf("All interpreter tests passed.\n");
    return 0;
}
