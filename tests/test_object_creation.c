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
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* ── Shared test infrastructure ────────────────────────────────────────── */

static STA_VM *g_vm;

static STA_OOP sym(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space, &g_vm->symbol_table, name, strlen(name));
}

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);

    sta_heap_init(&g_vm->heap, 1024 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 1024 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 512);
    sta_class_table_init(&g_vm->class_table);
    sta_stack_slab_init(&g_vm->slab, 65536);

    sta_special_objects_bind(g_vm->specials);
    sta_primitive_table_init();

    STA_BootstrapResult r = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space, &g_vm->symbol_table, &g_vm->class_table);
    assert(r.status == 0);
}

static void teardown(void) {
    sta_stack_slab_deinit(&g_vm->slab);
    sta_class_table_deinit(&g_vm->class_table);
    sta_heap_deinit(&g_vm->heap);
    sta_symbol_table_deinit(&g_vm->symbol_table);
    sta_immutable_space_deinit(&g_vm->immutable_space);
    free(g_vm);
}

static STA_OOP run(STA_OOP method) {
    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    return result;
}

/* ── Test 1: Create array, store a value, retrieve it ──────────────────── */
static void test_array_store_retrieve(void) {
    printf("  Array new: 3, at:put:, at:...");

    STA_OOP arr_cls   = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP newS_sel  = sym("new:");
    STA_OOP atPut_sel = sym("at:put:");
    STA_OOP at_sel    = sym("at:");

    STA_OOP lits[] = { arr_cls, newS_sel, atPut_sel, at_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,       0x00,
        OP_PUSH_SMALLINT,  0x03,
        OP_SEND,           0x01,
        OP_DUP,            0x00,
        OP_PUSH_ONE,       0x00,
        OP_PUSH_SMALLINT,  0x2A,
        OP_SEND,           0x02,
        OP_POP,            0x00,
        OP_PUSH_ONE,       0x00,
        OP_SEND,           0x03,
        OP_RETURN_TOP,     0x00,
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 1, 0,
        lits, 4, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == STA_SMALLINT_OOP(42));
    printf(" ok\n");
}

/* ── Test 2: Create array, check its size via #size send ───────────────── */
static void test_array_size(void) {
    printf("  (Array new: 10) size...");

    STA_OOP arr_cls  = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP newS_sel = sym("new:");
    STA_OOP size_sel = sym("size");

    STA_OOP lits[] = { arr_cls, newS_sel, size_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,       0x00,
        OP_PUSH_SMALLINT,  0x0A,
        OP_SEND,           0x01,
        OP_SEND,           0x02,
        OP_RETURN_TOP,     0x00,
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 3, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == STA_SMALLINT_OOP(10));
    printf(" ok\n");
}

/* ── Test 3: Create two objects, store in array, retrieve both ──────────── */
static void test_store_two_objects_in_array(void) {
    printf("  store two new objects in array, verify distinct...");

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ASSOCIATION);
    STA_OOP arr_cls   = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP new_sel   = sym("new");
    STA_OOP newS_sel  = sym("new:");
    STA_OOP atPut_sel = sym("at:put:");
    STA_OOP at_sel    = sym("at:");
    STA_OOP eq_sel    = sym("==");

    STA_OOP lits[] = { assoc_cls, new_sel, arr_cls, newS_sel,
                        atPut_sel, at_sel, eq_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,           0x00,
        OP_SEND,               0x01,
        OP_STORE_TEMP,         0x00,
        OP_PUSH_LIT,           0x00,
        OP_SEND,               0x01,
        OP_STORE_TEMP,         0x01,
        OP_PUSH_LIT,           0x02,
        OP_PUSH_TWO,           0x00,
        OP_SEND,               0x03,
        OP_STORE_TEMP,         0x02,
        OP_PUSH_ONE,           0x00,
        OP_PUSH_TEMP,          0x00,
        OP_SEND,               0x04,
        OP_POP,                0x00,
        OP_PUSH_TEMP,          0x02,
        OP_PUSH_TWO,           0x00,
        OP_PUSH_TEMP,          0x01,
        OP_SEND,               0x04,
        OP_POP,                0x00,
        OP_PUSH_TEMP,          0x02,
        OP_PUSH_ONE,           0x00,
        OP_SEND,               0x05,
        OP_PUSH_TEMP,          0x02,
        OP_PUSH_TWO,           0x00,
        OP_SEND,               0x05,
        OP_SEND,               0x06,
        OP_RETURN_TOP,         0x00,
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 3, 0,
        lits, 7, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == sta_spc_get(SPC_FALSE));
    printf(" ok\n");
}

/* ── Test 4: Create object, verify class, verify class identity ────────── */
static void test_new_object_class_identity(void) {
    printf("  Association new class == Association...");

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ASSOCIATION);
    STA_OOP new_sel   = sym("new");
    STA_OOP class_sel = sym("class");
    STA_OOP eq_sel    = sym("==");

    STA_OOP lits[] = { assoc_cls, new_sel, class_sel, eq_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,    0x00,
        OP_SEND,         0x01,
        OP_SEND,         0x02,
        OP_PUSH_LIT,    0x00,
        OP_SEND,         0x03,
        OP_RETURN_TOP,   0x00,
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 4, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == sta_spc_get(SPC_TRUE));
    printf(" ok\n");
}

/* ── Test 5: Array new: with store and size check combined ─────────────── */
static void test_array_store_and_size(void) {
    printf("  array store + size arithmetic...");

    STA_OOP arr_cls   = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP newS_sel  = sym("new:");
    STA_OOP atPut_sel = sym("at:put:");
    STA_OOP at_sel    = sym("at:");
    STA_OOP size_sel  = sym("size");
    STA_OOP plus_sel  = sym("+");

    STA_OOP lits[] = { arr_cls, newS_sel, atPut_sel, at_sel,
                        size_sel, plus_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,           0x00,
        OP_PUSH_SMALLINT,      0x05,
        OP_SEND,               0x01,
        OP_STORE_TEMP,         0x00,
        OP_PUSH_SMALLINT,      0x03,
        OP_PUSH_SMALLINT,      0x63,
        OP_SEND,               0x02,
        OP_POP,                0x00,
        OP_PUSH_TEMP,          0x00,
        OP_PUSH_SMALLINT,      0x03,
        OP_SEND,               0x03,
        OP_PUSH_TEMP,          0x00,
        OP_SEND,               0x04,
        OP_SEND,               0x05,
        OP_RETURN_TOP,         0x00,
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 1, 0,
        lits, 6, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == STA_SMALLINT_OOP(104));
    printf(" ok\n");
}

/* ── Test 6: Object new responds to #yourself ──────────────────────────── */
static void test_new_object_yourself(void) {
    printf("  Object new yourself identity...");

    STA_OOP obj_cls      = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP new_sel      = sym("new");
    STA_OOP yourself_sel = sym("yourself");
    STA_OOP eq_sel       = sym("==");

    STA_OOP lits[] = { obj_cls, new_sel, yourself_sel, eq_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,    0x00,
        OP_SEND,         0x01,
        OP_STORE_TEMP,   0x00,
        OP_SEND,         0x02,
        OP_PUSH_TEMP,    0x00,
        OP_SEND,         0x03,
        OP_RETURN_TOP,   0x00,
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 1, 0,
        lits, 4, bc, sizeof(bc));
    assert(method);

    STA_OOP result = run(method);
    assert(result == sta_spc_get(SPC_TRUE));
    printf(" ok\n");
}

/* ── Test 7: Chain of creates — no corruption ──────────────────────────── */
static void test_chain_of_creates(void) {
    printf("  chain of creates, no corruption...");

    STA_OOP arr_cls   = sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAY);
    STA_OOP newS_sel  = sym("new:");
    STA_OOP atPut_sel = sym("at:put:");
    STA_OOP at_sel    = sym("at:");
    STA_OOP plus_sel  = sym("+");

    STA_OOP lits[] = { arr_cls, newS_sel, atPut_sel, at_sel, plus_sel };
    uint8_t bc[] = {
        OP_PUSH_LIT,       0x00,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x01,
        OP_STORE_TEMP,      0x00,
        OP_PUSH_ONE,        0x00,
        OP_PUSH_SMALLINT,   0x0A,
        OP_SEND,            0x02,
        OP_POP,             0x00,
        OP_PUSH_LIT,       0x00,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x01,
        OP_STORE_TEMP,      0x01,
        OP_PUSH_ONE,        0x00,
        OP_PUSH_SMALLINT,   0x14,
        OP_SEND,            0x02,
        OP_POP,             0x00,
        OP_PUSH_LIT,       0x00,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x01,
        OP_STORE_TEMP,      0x02,
        OP_PUSH_ONE,        0x00,
        OP_PUSH_SMALLINT,   0x1E,
        OP_SEND,            0x02,
        OP_POP,             0x00,
        OP_PUSH_TEMP,       0x00,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x03,
        OP_PUSH_TEMP,       0x01,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x03,
        OP_SEND,            0x04,
        OP_PUSH_TEMP,       0x02,
        OP_PUSH_ONE,        0x00,
        OP_SEND,            0x03,
        OP_SEND,            0x04,
        OP_RETURN_TOP,      0x00,
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 3, 0,
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
