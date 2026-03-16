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
#include "bootstrap/bootstrap.h"
#include "bootstrap/kernel_load.h"
#include "compiler/compiler.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_ImmutableSpace *imm;
static STA_SymbolTable    *syms;
static STA_Heap           *heap;
static STA_ClassTable     *ct;
static STA_StackSlab      *slab;

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static void setup(void) {
    heap = sta_heap_create(4 * 1024 * 1024);
    imm  = sta_immutable_space_create(4 * 1024 * 1024);
    syms = sta_symbol_table_create(512);
    ct   = sta_class_table_create();
    slab = sta_stack_slab_create(64 * 1024);

    STA_BootstrapResult br = sta_bootstrap(heap, imm, syms, ct);
    assert(br.status == 0);
}

/* Helper: evaluate an expression and return the result OOP. */
static STA_OOP eval(const char *source) {
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);
    STA_CompileResult cr = sta_compile_expression(
        source, syms, imm, heap, sysdict);
    if (cr.had_error) {
        fprintf(stderr, "eval compile error: %s\n  source: %s\n",
                cr.error_msg, source);
        assert(!cr.had_error);
    }
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    return sta_interpret(slab, heap, ct, cr.method, nil_oop, NULL, 0);
}

/* ── Tests ─────────────────────────────────────────────────────────────── */

static void test_kernel_load(void) {
    int rc = sta_kernel_load_all(KERNEL_DIR);
    if (rc != STA_OK) {
        fprintf(stderr, "kernel_load error: %s\n", sta_vm_last_error(NULL));
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
    /* printString returns a Symbol (strings are interned as symbols in Phase 1). */
    STA_OOP result = eval("nil printString");
    size_t len;
    const char *str = sta_symbol_get_bytes(result, &len);
    assert(len == 3);
    assert(memcmp(str, "nil", 3) == 0);
}

static void test_true_printString(void) {
    STA_OOP result = eval("true printString");
    size_t len;
    const char *str = sta_symbol_get_bytes(result, &len);
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
    /* For non-SmallInteger objects, = defaults to == (identity). */
    STA_OOP result = eval("true = true");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_false_printString(void) {
    STA_OOP result = eval("false printString");
    size_t len;
    const char *str = sta_symbol_get_bytes(result, &len);
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

    sta_stack_slab_destroy(slab);
    sta_class_table_destroy(ct);
    sta_heap_destroy(heap);
    sta_symbol_table_destroy(syms);
    sta_immutable_space_destroy(imm);
    return (tests_passed == tests_run) ? 0 : 1;
}
