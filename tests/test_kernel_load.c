/* tests/test_kernel_load.c
 * Test: kernel .st file loading and exercising kernel methods.
 * Phase 1, Epic 9, Story 6.
 */
#include <sta/vm.h>
#include "vm/interpreter.h"
#include "vm/primitive_table.h"
#include "vm/method_dict.h"
#include "vm/symbol_table.h"
#include "vm/special_objects.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/frame.h"
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/kernel_load.h"
#include "compiler/compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_VM *g_vm;

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);

    sta_heap_init(&g_vm->heap, 4 * 1024 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 4 * 1024 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 512);
    sta_class_table_init(&g_vm->class_table);
    sta_stack_slab_init(&g_vm->slab, 64 * 1024);

    sta_special_objects_bind(g_vm->specials);
    sta_primitive_table_init();

    STA_BootstrapResult br = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space, &g_vm->symbol_table, &g_vm->class_table);
    assert(br.status == 0);
}

/* Helper: evaluate an expression and return the result OOP. */
static STA_OOP eval(const char *source) {
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);
    STA_CompileResult cr = sta_compile_expression(
        source, &g_vm->symbol_table, &g_vm->immutable_space, &g_vm->heap, sysdict);
    if (cr.had_error) {
        fprintf(stderr, "eval compile error: %s\n  source: %s\n",
                cr.error_msg, source);
        assert(!cr.had_error);
    }
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    return sta_interpret(g_vm, cr.method, nil_oop, NULL, 0);
}

/* Helper: extract bytes from a String or Symbol OOP. */
static const char *string_or_symbol_bytes(STA_OOP oop, size_t *out_len) {
    if (STA_IS_IMMEDIATE(oop)) return NULL;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)oop;
    if (h->class_index == STA_CLS_SYMBOL) {
        return sta_symbol_get_bytes(oop, out_len);
    }
    /* String: byte-indexable, no instVars. */
    uint32_t var_words = h->size;
    uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP)
                          - STA_BYTE_PADDING(h);
    *out_len = byte_count;
    return (const char *)sta_payload(h);
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

static void test_kernel_load(void) {
    int rc = sta_kernel_load_all(g_vm, KERNEL_DIR);
    if (rc != STA_OK) {
        fprintf(stderr, "kernel_load error: %s\n", sta_vm_last_error(g_vm));
    }
    assert(rc == STA_OK);
}

static void test_nil_isNil(void) {
    STA_OOP result = eval("nil isNil");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_nil_notNil(void) {
    STA_OOP result = eval("nil notNil");
    assert(result == sta_spc_get(SPC_FALSE));
}

static void test_true_not(void) {
    STA_OOP result = eval("true not");
    assert(result == sta_spc_get(SPC_FALSE));
}

static void test_false_not(void) {
    STA_OOP result = eval("false not");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_true_and_false(void) {
    STA_OOP result = eval("true & false");
    assert(result == sta_spc_get(SPC_FALSE));
}

static void test_false_or_true(void) {
    STA_OOP result = eval("false | true");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_nil_printString(void) {
    STA_OOP result = eval("nil printString");
    size_t len;
    const char *str = string_or_symbol_bytes(result, &len);
    assert(len == 3);
    assert(memcmp(str, "nil", 3) == 0);
}

static void test_true_printString(void) {
    STA_OOP result = eval("true printString");
    size_t len;
    const char *str = string_or_symbol_bytes(result, &len);
    assert(len == 4);
    assert(memcmp(str, "true", 4) == 0);
}

static void test_integer_isNil(void) {
    STA_OOP result = eval("42 isNil");
    assert(result == sta_spc_get(SPC_FALSE));
}

static void test_integer_notNil(void) {
    STA_OOP result = eval("42 notNil");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_integer_equality(void) {
    STA_OOP result = eval("3 = 3");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_integer_inequality(void) {
    STA_OOP result = eval("3 ~= 4");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_nil_ifNil(void) {
    STA_OOP result = eval("nil ifNil: [42]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 42);
}

static void test_nil_ifNotNil(void) {
    STA_OOP result = eval("nil ifNotNil: [42]");
    assert(result == sta_spc_get(SPC_NIL));
}

static void test_nil_ifNil_ifNotNil(void) {
    STA_OOP result = eval("nil ifNil: [99] ifNotNil: [0]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 99);
}

static void test_object_equals_identity(void) {
    STA_OOP result = eval("true = true");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_false_printString(void) {
    STA_OOP result = eval("false printString");
    size_t len;
    const char *str = string_or_symbol_bytes(result, &len);
    assert(len == 5);
    assert(memcmp(str, "false", 5) == 0);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    setup();
    printf("test_kernel_load:\n");

    RUN(test_kernel_load);
    RUN(test_nil_isNil);
    RUN(test_nil_notNil);
    RUN(test_true_not);
    RUN(test_false_not);
    RUN(test_true_and_false);
    RUN(test_false_or_true);
    RUN(test_nil_printString);
    RUN(test_true_printString);
    RUN(test_false_printString);
    RUN(test_integer_isNil);
    RUN(test_integer_notNil);
    RUN(test_integer_equality);
    RUN(test_integer_inequality);
    RUN(test_nil_ifNil);
    RUN(test_nil_ifNotNil);
    RUN(test_nil_ifNil_ifNotNil);
    RUN(test_object_equals_identity);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);

    sta_stack_slab_deinit(&g_vm->slab);
    sta_class_table_deinit(&g_vm->class_table);
    sta_heap_deinit(&g_vm->heap);
    sta_symbol_table_deinit(&g_vm->symbol_table);
    sta_immutable_space_deinit(&g_vm->immutable_space);
    free(g_vm);
    return (tests_passed == tests_run) ? 0 : 1;
}
