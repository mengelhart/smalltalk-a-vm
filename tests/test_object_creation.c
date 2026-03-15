/* tests/test_object_creation.c
 * End-to-end smoke tests for object creation (Phase 1, Epic 5.5).
 * Each test hand-assembles a CompiledMethod that exercises multiple
 * operations through the interpreter — creation, mutation, inspection.
 */
#include "vm/format.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/symbol_table.h"
#include "vm/interpreter.h"
#include "vm/compiled_method.h"
#include "vm/primitive_table.h"
#include "vm/special_objects.h"
#include "vm/frame.h"
#include "bootstrap/bootstrap.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* ── Shared test infrastructure ────────────────────────────────────────── */

static STA_ImmutableSpace *g_sp;
static STA_SymbolTable    *g_st;
static STA_Heap           *g_heap;
static STA_ClassTable     *g_ct;

static STA_OOP sym(const char *name) {
    return sta_symbol_intern(g_sp, g_st, name, strlen(name));
}

static void setup(void) {
    g_sp   = sta_immutable_space_create(1024 * 1024);
    g_st   = sta_symbol_table_create(512);
    g_heap = sta_heap_create(1024 * 1024);
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

static STA_OOP run(STA_OOP method) {
    STA_StackSlab *slab = sta_stack_slab_create(65536);
    STA_OOP result = sta_interpret(slab, g_heap, g_ct, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    sta_stack_slab_destroy(slab);
    return result;
}

/* ── Test 1: Create array, store a value, retrieve it ──────────────────── */
/*
 * Equivalent Smalltalk:
 *   | arr |
 *   arr := Array new: 3.
 *   arr at: 1 put: 42.
 *   arr at: 1           "→ 42"
 */
static void test_array_store_retrieve(void) {
    printf("  Array new: 3, at:put:, at:...");

    STA_OOP arr_cls   = sta_class_table_get(g_ct, STA_CLS_ARRAY);
    STA_OOP newS_sel  = sym("new:");
    STA_OOP atPut_sel = sym("at:put:");
    STA_OOP at_sel    = sym("at:");

    STA_OOP lits[] = { arr_cls, newS_sel, atPut_sel, at_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,       0x00,   /* push Array                    */
        OP_PUSH_SMALLINT,  0x03,   /* push 3                        */
        OP_SEND,           0x01,   /* send #new: → arr              */
        OP_DUP,            0x00,   /* dup arr (for at:put:)         */
        OP_PUSH_ONE,       0x00,   /* push 1                        */
        OP_PUSH_SMALLINT,  0x2A,   /* push 42                       */
        OP_SEND,           0x02,   /* send #at:put: → 42 (discard)  */
        OP_POP,            0x00,   /* pop at:put: result            */
        OP_PUSH_ONE,       0x00,   /* push 1                        */
        OP_SEND,           0x03,   /* send #at: → 42                */
        OP_RETURN_TOP,     0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 1, 0,
        lits, 4, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == STA_SMALLINT_OOP(42));
    printf(" ok\n");
}

/* ── Test 2: Create array, check its size via #size send ───────────────── */
/*
 *   Array new: 10 → arr. arr size  "→ 10"
 */
static void test_array_size(void) {
    printf("  (Array new: 10) size...");

    STA_OOP arr_cls  = sta_class_table_get(g_ct, STA_CLS_ARRAY);
    STA_OOP newS_sel = sym("new:");
    STA_OOP size_sel = sym("size");

    STA_OOP lits[] = { arr_cls, newS_sel, size_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,       0x00,   /* push Array        */
        OP_PUSH_SMALLINT,  0x0A,   /* push 10           */
        OP_SEND,           0x01,   /* send #new:        */
        OP_SEND,           0x02,   /* send #size → 10   */
        OP_RETURN_TOP,     0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
        lits, 3, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == STA_SMALLINT_OOP(10));
    printf(" ok\n");
}

/* ── Test 3: Create two objects, store in array, retrieve both ──────────── */
/*
 *   | a b arr |
 *   a := Association new.
 *   b := Association new.
 *   arr := Array new: 2.
 *   arr at: 1 put: a.
 *   arr at: 2 put: b.
 *   (arr at: 1) == (arr at: 2)   "→ false (distinct objects)"
 */
static void test_store_two_objects_in_array(void) {
    printf("  store two new objects in array, verify distinct...");

    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    STA_OOP arr_cls   = sta_class_table_get(g_ct, STA_CLS_ARRAY);
    STA_OOP new_sel   = sym("new");
    STA_OOP newS_sel  = sym("new:");
    STA_OOP atPut_sel = sym("at:put:");
    STA_OOP at_sel    = sym("at:");
    STA_OOP eq_sel    = sym("==");

    STA_OOP lits[] = { assoc_cls, new_sel, arr_cls, newS_sel,
                        atPut_sel, at_sel, eq_sel };
    uint8_t bc[] = {
        /* a := Association new  →  temp 0 */
        OP_PUSH_LIT,           0x00,   /* push Association           */
        OP_SEND,               0x01,   /* send #new → a              */
        OP_STORE_TEMP,         0x00,   /* temp 0 = a                 */

        /* b := Association new  →  temp 1 */
        OP_PUSH_LIT,           0x00,   /* push Association           */
        OP_SEND,               0x01,   /* send #new → b              */
        OP_STORE_TEMP,         0x01,   /* temp 1 = b                 */

        /* arr := Array new: 2  →  temp 2 */
        OP_PUSH_LIT,           0x02,   /* push Array                 */
        OP_PUSH_TWO,           0x00,   /* push 2                     */
        OP_SEND,               0x03,   /* send #new: → arr           */
        OP_STORE_TEMP,         0x02,   /* temp 2 = arr               */

        /* arr at: 1 put: a */
        OP_PUSH_ONE,           0x00,   /* push 1                     */
        OP_PUSH_TEMP,          0x00,   /* push a                     */
        OP_SEND,               0x04,   /* send #at:put:              */
        OP_POP,                0x00,   /* discard result             */

        /* arr at: 2 put: b */
        OP_PUSH_TEMP,          0x02,   /* push arr                   */
        OP_PUSH_TWO,           0x00,   /* push 2                     */
        OP_PUSH_TEMP,          0x01,   /* push b                     */
        OP_SEND,               0x04,   /* send #at:put:              */
        OP_POP,                0x00,   /* discard result             */

        /* (arr at: 1) == (arr at: 2) */
        OP_PUSH_TEMP,          0x02,   /* push arr                   */
        OP_PUSH_ONE,           0x00,   /* push 1                     */
        OP_SEND,               0x05,   /* send #at: → a              */
        OP_PUSH_TEMP,          0x02,   /* push arr                   */
        OP_PUSH_TWO,           0x00,   /* push 2                     */
        OP_SEND,               0x05,   /* send #at: → b              */
        OP_SEND,               0x06,   /* send #== → false           */
        OP_RETURN_TOP,         0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 3, 0,
        lits, 7, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == sta_spc_get(SPC_FALSE));
    printf(" ok\n");
}

/* ── Test 4: Create object, verify class, verify class identity ────────── */
/*
 *   Association new class == Association  "→ true"
 *   Verified through the full interpreter dispatch chain.
 */
static void test_new_object_class_identity(void) {
    printf("  Association new class == Association...");

    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    STA_OOP new_sel   = sym("new");
    STA_OOP class_sel = sym("class");
    STA_OOP eq_sel    = sym("==");

    STA_OOP lits[] = { assoc_cls, new_sel, class_sel, eq_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,    0x00,   /* push Association          */
        OP_SEND,         0x01,   /* send #new                 */
        OP_SEND,         0x02,   /* send #class               */
        OP_PUSH_LIT,    0x00,   /* push Association          */
        OP_SEND,         0x03,   /* send #==                  */
        OP_RETURN_TOP,   0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
        lits, 4, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == sta_spc_get(SPC_TRUE));
    printf(" ok\n");
}

/* ── Test 5: Array new: with store and size check combined ─────────────── */
/*
 *   | arr |
 *   arr := Array new: 5.
 *   arr at: 3 put: 99.
 *   arr at: 3           "→ 99"
 *   ... and arr size = 5
 *
 *   Returns (arr at: 3) + (arr size) = 99 + 5 = 104
 */
static void test_array_store_and_size(void) {
    printf("  array store + size arithmetic...");

    STA_OOP arr_cls   = sta_class_table_get(g_ct, STA_CLS_ARRAY);
    STA_OOP newS_sel  = sym("new:");
    STA_OOP atPut_sel = sym("at:put:");
    STA_OOP at_sel    = sym("at:");
    STA_OOP size_sel  = sym("size");
    STA_OOP plus_sel  = sym("+");

    STA_OOP lits[] = { arr_cls, newS_sel, atPut_sel, at_sel,
                        size_sel, plus_sel };
    uint8_t bc[] = {
        /* arr := Array new: 5  →  temp 0 */
        OP_PUSH_LIT,           0x00,
        OP_PUSH_SMALLINT,      0x05,
        OP_SEND,               0x01,   /* #new: */
        OP_STORE_TEMP,         0x00,   /* temp 0 = arr */

        /* arr at: 3 put: 99 */
        OP_PUSH_SMALLINT,      0x03,
        OP_PUSH_SMALLINT,      0x63,   /* 99 */
        OP_SEND,               0x02,   /* #at:put: */
        OP_POP,                0x00,

        /* (arr at: 3) → 99 on stack */
        OP_PUSH_TEMP,          0x00,
        OP_PUSH_SMALLINT,      0x03,
        OP_SEND,               0x03,   /* #at: → 99 */

        /* arr size → 5 on stack */
        OP_PUSH_TEMP,          0x00,
        OP_SEND,               0x04,   /* #size → 5 */

        /* 99 + 5 → 104 */
        OP_SEND,               0x05,   /* #+ → 104 */
        OP_RETURN_TOP,         0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 1, 0,
        lits, 6, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == STA_SMALLINT_OOP(104));
    printf(" ok\n");
}

/* ── Test 6: Object new responds to #yourself ──────────────────────────── */
/*
 *   | obj |
 *   obj := Object new.
 *   obj yourself == obj  "→ true"
 */
static void test_new_object_yourself(void) {
    printf("  Object new yourself identity...");

    STA_OOP obj_cls      = sta_class_table_get(g_ct, STA_CLS_OBJECT);
    STA_OOP new_sel      = sym("new");
    STA_OOP yourself_sel = sym("yourself");
    STA_OOP eq_sel       = sym("==");

    STA_OOP lits[] = { obj_cls, new_sel, yourself_sel, eq_sel };
    uint8_t bc[] = {
        /* obj := Object new  →  temp 0 */
        OP_PUSH_LIT,    0x00,
        OP_SEND,         0x01,   /* #new */
        OP_STORE_TEMP,   0x00,   /* temp 0 = obj */

        /* obj yourself */
        OP_SEND,         0x02,   /* #yourself → same obj */

        /* result == obj */
        OP_PUSH_TEMP,    0x00,   /* push obj */
        OP_SEND,         0x03,   /* #== → true */
        OP_RETURN_TOP,   0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 1, 0,
        lits, 4, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == sta_spc_get(SPC_TRUE));
    printf(" ok\n");
}

/* ── Test 7: Chain of creates — no corruption ──────────────────────────── */
/*
 *   | a1 a2 a3 |
 *   a1 := Array new: 1. a1 at: 1 put: 10.
 *   a2 := Array new: 1. a2 at: 1 put: 20.
 *   a3 := Array new: 1. a3 at: 1 put: 30.
 *   (a1 at: 1) + (a2 at: 1) + (a3 at: 1)  "→ 60"
 */
static void test_chain_of_creates(void) {
    printf("  chain of creates, no corruption...");

    STA_OOP arr_cls   = sta_class_table_get(g_ct, STA_CLS_ARRAY);
    STA_OOP newS_sel  = sym("new:");
    STA_OOP atPut_sel = sym("at:put:");
    STA_OOP at_sel    = sym("at:");
    STA_OOP plus_sel  = sym("+");

    STA_OOP lits[] = { arr_cls, newS_sel, atPut_sel, at_sel, plus_sel };
    uint8_t bc[] = {
        /* a1 := Array new: 1 → temp 0 */
        OP_PUSH_LIT,       0x00,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x01,   /* #new: */
        OP_STORE_TEMP,      0x00,

        /* a1 at: 1 put: 10 */
        OP_PUSH_ONE,        0x00,
        OP_PUSH_SMALLINT,   0x0A,
        OP_SEND,            0x02,   /* #at:put: */
        OP_POP,             0x00,

        /* a2 := Array new: 1 → temp 1 */
        OP_PUSH_LIT,       0x00,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x01,
        OP_STORE_TEMP,      0x01,

        /* a2 at: 1 put: 20 */
        OP_PUSH_ONE,        0x00,
        OP_PUSH_SMALLINT,   0x14,
        OP_SEND,            0x02,
        OP_POP,             0x00,

        /* a3 := Array new: 1 → temp 2 */
        OP_PUSH_LIT,       0x00,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x01,
        OP_STORE_TEMP,      0x02,

        /* a3 at: 1 put: 30 */
        OP_PUSH_ONE,        0x00,
        OP_PUSH_SMALLINT,   0x1E,
        OP_SEND,            0x02,
        OP_POP,             0x00,

        /* (a1 at: 1) */
        OP_PUSH_TEMP,       0x00,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x03,   /* #at: → 10 */

        /* (a2 at: 1) */
        OP_PUSH_TEMP,       0x01,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x03,   /* #at: → 20 */

        /* 10 + 20 */
        OP_SEND,            0x04,   /* #+ → 30 */

        /* (a3 at: 1) */
        OP_PUSH_TEMP,       0x02,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x03,   /* #at: → 30 */

        /* 30 + 30 */
        OP_SEND,            0x04,   /* #+ → 60 */
        OP_RETURN_TOP,      0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 3, 0,
        lits, 5, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == STA_SMALLINT_OOP(60));
    printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_object_creation (end-to-end):\n");
    setup();

    test_array_store_retrieve();
    test_array_size();
    test_store_two_objects_in_array();
    test_new_object_class_identity();
    test_array_store_and_size();
    test_new_object_yourself();
    test_chain_of_creates();

    teardown();
    printf("  all end-to-end object creation tests passed.\n");
    return 0;
}
