/* tests/test_new_methods.c
 * Interpreter-level tests for Behavior>>new, basicNew, basicNew:, new:
 * and Object>>initialize (Phase 1, Epic 5.4).
 * Exercises the full message dispatch path for object creation.
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

/* ── Test 1: Association new via interpreter ────────────────────────────── */
/* Equivalent Smalltalk: Association new */

static void test_association_new(void) {
    printf("  Association new via interpreter...");

    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    STA_OOP new_sel = sym("new");

    /* Method: push Association, send #new, return top. */
    STA_OOP lits[] = { assoc_cls, new_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,    0x00,   /* push Association class   */
        OP_SEND,         0x01,   /* send #new (lit 1)        */
        OP_RETURN_TOP,   0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
        lits, 2, bc, sizeof(bc));
    assert(method);

    STA_StackSlab *slab = sta_stack_slab_create(65536);
    STA_OOP result = sta_interpret(slab, g_heap, g_ct, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);

    /* Result should be a new Association. */
    assert(STA_IS_HEAP(result));
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_ASSOCIATION);
    assert(h->size == 2);

    /* Both slots (key, value) should be nil. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP *slots = sta_payload(h);
    assert(slots[0] == nil_oop);
    assert(slots[1] == nil_oop);

    sta_stack_slab_destroy(slab);
    printf(" ok\n");
}

/* ── Test 2: Array new: 10 via interpreter ─────────────────────────────── */
/* Equivalent Smalltalk: Array new: 10 */

static void test_array_new_size(void) {
    printf("  Array new: 10 via interpreter...");

    STA_OOP arr_cls = sta_class_table_get(g_ct, STA_CLS_ARRAY);
    STA_OOP newSize_sel = sym("new:");

    /* Method: push Array, push 10, send #new:, return top. */
    STA_OOP lits[] = { arr_cls, newSize_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,      0x00,   /* push Array class       */
        OP_PUSH_SMALLINT,  0x0A,   /* push 10                */
        OP_SEND,           0x01,   /* send #new: (lit 1)     */
        OP_RETURN_TOP,     0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
        lits, 2, bc, sizeof(bc));
    assert(method);

    STA_StackSlab *slab = sta_stack_slab_create(65536);
    STA_OOP result = sta_interpret(slab, g_heap, g_ct, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);

    /* Result should be a 10-element Array. */
    assert(STA_IS_HEAP(result));
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_ARRAY);
    assert(h->size == 10);

    /* All elements should be nil. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < 10; i++) {
        assert(slots[i] == nil_oop);
    }

    sta_stack_slab_destroy(slab);
    printf(" ok\n");
}

/* ── Test 3: Object new via interpreter ────────────────────────────────── */
/* Equivalent Smalltalk: Object new */

static void test_object_new(void) {
    printf("  Object new via interpreter...");

    STA_OOP obj_cls = sta_class_table_get(g_ct, STA_CLS_OBJECT);
    STA_OOP new_sel = sym("new");

    STA_OOP lits[] = { obj_cls, new_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,    0x00,
        OP_SEND,         0x01,
        OP_RETURN_TOP,   0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
        lits, 2, bc, sizeof(bc));
    assert(method);

    STA_StackSlab *slab = sta_stack_slab_create(65536);
    STA_OOP result = sta_interpret(slab, g_heap, g_ct, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);

    /* Object has 0 ivars. */
    assert(STA_IS_HEAP(result));
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_OBJECT);
    assert(h->size == 0);

    sta_stack_slab_destroy(slab);
    printf(" ok\n");
}

/* ── Test 4: Association new returns a distinct object each time ────────── */
/* Equivalent Smalltalk: | a b | a := Association new. b := Association new. a == b → false */

static void test_new_returns_distinct(void) {
    printf("  new returns distinct objects...");

    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    STA_OOP new_sel = sym("new");
    STA_OOP eq_sel  = sym("==");

    /* push Association, send #new, push Association, send #new, send #==, return top */
    STA_OOP lits[] = { assoc_cls, new_sel, eq_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,    0x00,   /* push Association        */
        OP_SEND,         0x01,   /* send #new → obj a       */
        OP_PUSH_LIT,    0x00,   /* push Association        */
        OP_SEND,         0x01,   /* send #new → obj b       */
        OP_SEND,         0x02,   /* send #== (a == b)       */
        OP_RETURN_TOP,   0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
        lits, 3, bc, sizeof(bc));
    assert(method);

    STA_StackSlab *slab = sta_stack_slab_create(65536);
    STA_OOP result = sta_interpret(slab, g_heap, g_ct, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);

    assert(result == sta_spc_get(SPC_FALSE));

    sta_stack_slab_destroy(slab);
    printf(" ok\n");
}

/* ── Test 5: #class on a new object returns its class ──────────────────── */
/* Equivalent Smalltalk: Association new class == Association → true */

static void test_new_object_class(void) {
    printf("  new object class is correct...");

    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    STA_OOP new_sel   = sym("new");
    STA_OOP class_sel = sym("class");
    STA_OOP eq_sel    = sym("==");

    /* push Association, send #new, send #class, push Association, send #== */
    STA_OOP lits[] = { assoc_cls, new_sel, class_sel, eq_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,    0x00,   /* push Association          */
        OP_SEND,         0x01,   /* send #new → new obj       */
        OP_SEND,         0x02,   /* send #class → Association */
        OP_PUSH_LIT,    0x00,   /* push Association          */
        OP_SEND,         0x03,   /* send #== → true           */
        OP_RETURN_TOP,   0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
        lits, 4, bc, sizeof(bc));
    assert(method);

    STA_StackSlab *slab = sta_stack_slab_create(65536);
    STA_OOP result = sta_interpret(slab, g_heap, g_ct, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);

    assert(result == sta_spc_get(SPC_TRUE));

    sta_stack_slab_destroy(slab);
    printf(" ok\n");
}

/* ── Test 6: ByteArray new: 8 via interpreter ──────────────────────────── */

static void test_bytearray_new_size(void) {
    printf("  ByteArray new: 8 via interpreter...");

    STA_OOP ba_cls = sta_class_table_get(g_ct, STA_CLS_BYTEARRAY);
    STA_OOP newSize_sel = sym("new:");

    STA_OOP lits[] = { ba_cls, newSize_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,      0x00,
        OP_PUSH_SMALLINT,  0x08,
        OP_SEND,           0x01,
        OP_RETURN_TOP,     0x00,
    };
    STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
        lits, 2, bc, sizeof(bc));
    assert(method);

    STA_StackSlab *slab = sta_stack_slab_create(65536);
    STA_OOP result = sta_interpret(slab, g_heap, g_ct, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);

    assert(STA_IS_HEAP(result));
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_BYTEARRAY);
    /* 8 bytes = 1 word, 0 padding. */
    assert(h->size == 1);
    assert(STA_BYTE_PADDING(h) == 0);

    sta_stack_slab_destroy(slab);
    printf(" ok\n");
}

/* ── Test 7: Methods are findable via respondsTo: ──────────────────────── */

static void test_responds_to_new(void) {
    printf("  classes respond to #new and #basicNew...");

    STA_OOP assoc_cls = sta_class_table_get(g_ct, STA_CLS_ASSOCIATION);
    STA_OOP responds_sel = sym("respondsTo:");
    STA_OOP new_sel      = sym("new");
    STA_OOP basicNew_sel = sym("basicNew");

    /* Association respondsTo: #new → true */
    {
        STA_OOP lits[] = { assoc_cls, new_sel, responds_sel };
        uint8_t bc[] = {
            OP_PUSH_LIT, 0x00,   /* push Association        */
            OP_PUSH_LIT, 0x01,   /* push #new               */
            OP_SEND,      0x02,   /* send #respondsTo:       */
            OP_RETURN_TOP, 0x00,
        };
        STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
            lits, 3, bc, sizeof(bc));
        assert(method);

        STA_StackSlab *slab = sta_stack_slab_create(65536);
        STA_OOP result = sta_interpret(slab, g_heap, g_ct, method,
                                        STA_SMALLINT_OOP(0), NULL, 0);
        assert(result == sta_spc_get(SPC_TRUE));
        sta_stack_slab_destroy(slab);
    }

    /* Association respondsTo: #basicNew → true */
    {
        STA_OOP lits[] = { assoc_cls, basicNew_sel, responds_sel };
        uint8_t bc[] = {
            OP_PUSH_LIT, 0x00,
            OP_PUSH_LIT, 0x01,
            OP_SEND,      0x02,
            OP_RETURN_TOP, 0x00,
        };
        STA_OOP method = sta_compiled_method_create(g_sp, 0, 0, 0,
            lits, 3, bc, sizeof(bc));
        assert(method);

        STA_StackSlab *slab = sta_stack_slab_create(65536);
        STA_OOP result = sta_interpret(slab, g_heap, g_ct, method,
                                        STA_SMALLINT_OOP(0), NULL, 0);
        assert(result == sta_spc_get(SPC_TRUE));
        sta_stack_slab_destroy(slab);
    }

    printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_new_methods:\n");
    setup();

    test_association_new();
    test_array_new_size();
    test_object_new();
    test_new_returns_distinct();
    test_new_object_class();
    test_bytearray_new_size();
    test_responds_to_new();

    teardown();
    printf("  all new method tests passed.\n");
    return 0;
}
