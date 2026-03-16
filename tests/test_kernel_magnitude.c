/* tests/test_kernel_magnitude.c
 * Test: Magnitude, Number, SmallInteger, Association kernel methods.
 * Phase 1, Epic 9, Story 7.
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

    int rc = sta_kernel_load_all(KERNEL_DIR);
    if (rc != STA_OK) {
        fprintf(stderr, "kernel_load error: %s\n", sta_vm_last_error(NULL));
    }
    assert(rc == STA_OK);
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

/* ── Magnitude tests ──────────────────────────────────────────────────── */

static void test_max_5_3(void) {
    STA_OOP r = eval("5 max: 3");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 5);
}

static void test_min_3_5(void) {
    STA_OOP r = eval("3 min: 5");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 3);
}

static void test_between_true(void) {
    STA_OOP r = eval("4 between: 1 and: 10");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_between_false(void) {
    STA_OOP r = eval("7 between: 8 and: 10");
    assert(r == sta_spc_get(SPC_FALSE));
}

static void test_gte_equal(void) {
    STA_OOP r = eval("3 >= 3");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_gte_less(void) {
    STA_OOP r = eval("3 >= 4");
    assert(r == sta_spc_get(SPC_FALSE));
}

static void test_lte_equal(void) {
    STA_OOP r = eval("3 <= 3");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_lte_greater(void) {
    STA_OOP r = eval("4 <= 3");
    assert(r == sta_spc_get(SPC_FALSE));
}

/* ── Number tests ─────────────────────────────────────────────────────── */

static void test_abs_negative(void) {
    STA_OOP r = eval("-3 abs");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 3);
}

static void test_abs_zero(void) {
    STA_OOP r = eval("0 abs");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 0);
}

static void test_abs_positive(void) {
    STA_OOP r = eval("5 abs");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 5);
}

static void test_isZero_true(void) {
    STA_OOP r = eval("0 isZero");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_isZero_false(void) {
    STA_OOP r = eval("5 isZero");
    assert(r == sta_spc_get(SPC_FALSE));
}

static void test_sign_positive(void) {
    STA_OOP r = eval("5 sign");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 1);
}

static void test_sign_negative(void) {
    STA_OOP r = eval("-3 sign");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == -1);
}

static void test_sign_zero(void) {
    STA_OOP r = eval("0 sign");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 0);
}

static void test_negated(void) {
    STA_OOP r = eval("5 negated");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == -5);
}

static void test_positive_true(void) {
    STA_OOP r = eval("5 positive");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_negative_true(void) {
    STA_OOP r = eval("-3 negative");
    assert(r == sta_spc_get(SPC_TRUE));
}

/* ── SmallInteger tests ───────────────────────────────────────────────── */

static void test_factorial_0(void) {
    STA_OOP r = eval("0 factorial");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 1);
}

static void test_factorial_1(void) {
    STA_OOP r = eval("1 factorial");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 1);
}

static void test_factorial_5(void) {
    STA_OOP r = eval("5 factorial");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 120);
}

static void test_nested_send_factorial(void) {
    /* This was the original crash case: nested non-primitive send
     * inside a binary expression corrupted the expression stack. */
    STA_OOP r = eval("1 * ((1 - 1) factorial)");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 1);
}

/* ── Association tests ────────────────────────────────────────────────── */

static void test_association_key_value(void) {
    /* Create an Association, set key: 'a' value: 42, verify. */
    STA_OOP r;

    /* Association new creates one, key:value: sets both. */
    r = eval("| a | a := Association new. a key: #a value: 42. a key");
    /* key should be the symbol #a */
    size_t len;
    const char *str = sta_symbol_get_bytes(r, &len);
    assert(len == 1 && memcmp(str, "a", 1) == 0);

    r = eval("| a | a := Association new. a key: #a value: 42. a value");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 42);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    setup();
    printf("test_kernel_magnitude:\n");

    /* Magnitude */
    RUN(test_max_5_3);
    RUN(test_min_3_5);
    RUN(test_between_true);
    RUN(test_between_false);
    RUN(test_gte_equal);
    RUN(test_gte_less);
    RUN(test_lte_equal);
    RUN(test_lte_greater);

    /* Number */
    RUN(test_abs_negative);
    RUN(test_abs_zero);
    RUN(test_abs_positive);
    RUN(test_isZero_true);
    RUN(test_isZero_false);
    RUN(test_sign_positive);
    RUN(test_sign_negative);
    RUN(test_sign_zero);
    RUN(test_negated);
    RUN(test_positive_true);
    RUN(test_negative_true);

    /* SmallInteger */
    RUN(test_factorial_0);
    RUN(test_factorial_1);
    RUN(test_factorial_5);
    RUN(test_nested_send_factorial);

    /* Association */
    RUN(test_association_key_value);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);

    sta_stack_slab_destroy(slab);
    sta_class_table_destroy(ct);
    sta_heap_destroy(heap);
    sta_symbol_table_destroy(syms);
    sta_immutable_space_destroy(imm);
    return (tests_passed == tests_run) ? 0 : 1;
}
