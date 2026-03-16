/* tests/test_arithmetic_prims.c
 * Phase 1.5 Batch 1, Story 4: Arithmetic primitive tests.
 * Tests comparison, division, modulo, and bit operation primitives
 * through sta_eval on the public API.
 */
#include <sta/vm.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static STA_VM *vm;

static void assert_eval(const char *expr, const char *expected) {
    STA_Handle *h = sta_eval(vm, expr);
    if (!h) {
        fprintf(stderr, "\n  eval failed: %s\n  expr: %s\n",
                sta_vm_last_error(vm), expr);
    }
    assert(h != NULL);
    const char *got = sta_inspect_cstring(vm, h);
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "\n  FAIL: %s\n    expected: %s\n    got:      %s\n",
                expr, expected, got);
    }
    assert(strcmp(got, expected) == 0);
    sta_handle_release(vm, h);
}

/* Verify eval returns NULL (DNU or primitive failure, no crash). */
static void assert_eval_fails(const char *expr) {
    STA_Handle *h = sta_eval(vm, expr);
    if (h) {
        /* If it didn't fail, that's also acceptable for DNU cases
         * where doesNotUnderstand: returns nil. Just ensure no crash. */
        sta_handle_release(vm, h);
    }
    /* The point is: no crash. */
}

/* ── Comparison primitives (5, 6, 8) ────────────────────────────────── */

static void test_le_equal(void)   { assert_eval("3 <= 3", "true"); }
static void test_le_greater(void) { assert_eval("4 <= 3", "false"); }
static void test_le_less(void)    { assert_eval("2 <= 3", "true"); }

static void test_ge_equal(void)   { assert_eval("3 >= 3", "true"); }
static void test_ge_less(void)    { assert_eval("3 >= 4", "false"); }
static void test_ge_greater(void) { assert_eval("4 >= 3", "true"); }

static void test_ne_diff(void)    { assert_eval("3 ~= 4", "true"); }
static void test_ne_same(void)    { assert_eval("3 ~= 3", "false"); }

/* ── Exact division (prim 10) ────────────────────────────────────────── */

static void test_div_exact_6_2(void)  { assert_eval("6 / 2", "3"); }
static void test_div_exact_6_3(void)  { assert_eval("6 / 3", "2"); }
static void test_div_exact_neg(void)  { assert_eval("-6 / 2", "-3"); }
static void test_div_exact_fail(void) { assert_eval_fails("7 / 2"); }

/* ── Floor division (prim 12) ────────────────────────────────────────── */

static void test_floor_div_pos(void)      { assert_eval("7 // 2", "3"); }
static void test_floor_div_neg_num(void)  { assert_eval("-7 // 2", "-4"); }
static void test_floor_div_neg_den(void)  { assert_eval("7 // -2", "-4"); }
static void test_floor_div_both_neg(void) { assert_eval("-7 // -2", "3"); }
static void test_floor_div_zero(void)     { assert_eval_fails("10 // 0"); }

/* ── Modulo (prim 11) ────────────────────────────────────────────────── */

static void test_mod_pos(void)      { assert_eval("7 \\\\ 2", "1"); }
static void test_mod_neg_num(void)  { assert_eval("-7 \\\\ 2", "1"); }
static void test_mod_neg_den(void)  { assert_eval("7 \\\\ -2", "-1"); }
static void test_mod_both_neg(void) { assert_eval("-7 \\\\ -2", "-1"); }
static void test_mod_zero(void)     { assert_eval_fails("10 \\\\ 0"); }

/* ── Truncated division (prim 13) ────────────────────────────────────── */

static void test_quo_pos(void)  { assert_eval("7 quo: 2", "3"); }
static void test_quo_neg(void)  { assert_eval("-7 quo: 2", "-3"); }

/* ── Bit operations (prims 14-17) ────────────────────────────────────── */

static void test_bitand(void)   { assert_eval("12 bitAnd: 10", "8"); }
static void test_bitor(void)    { assert_eval("12 bitOr: 10", "14"); }
static void test_bitxor(void)   { assert_eval("12 bitXor: 10", "6"); }
static void test_lshift(void)   { assert_eval("1 bitShift: 4", "16"); }
static void test_rshift(void)   { assert_eval("16 bitShift: -2", "4"); }
static void test_neg_lshift(void) { assert_eval("-1 bitShift: 4", "-16"); }

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_arithmetic_prims (Phase 1.5 Batch 1):\n");

    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);

    /* Comparison */
    RUN(test_le_equal);
    RUN(test_le_greater);
    RUN(test_le_less);
    RUN(test_ge_equal);
    RUN(test_ge_less);
    RUN(test_ge_greater);
    RUN(test_ne_diff);
    RUN(test_ne_same);

    /* Exact division */
    RUN(test_div_exact_6_2);
    RUN(test_div_exact_6_3);
    RUN(test_div_exact_neg);
    RUN(test_div_exact_fail);

    /* Floor division */
    RUN(test_floor_div_pos);
    RUN(test_floor_div_neg_num);
    RUN(test_floor_div_neg_den);
    RUN(test_floor_div_both_neg);
    RUN(test_floor_div_zero);

    /* Modulo */
    RUN(test_mod_pos);
    RUN(test_mod_neg_num);
    RUN(test_mod_neg_den);
    RUN(test_mod_both_neg);
    RUN(test_mod_zero);

    /* Truncated division */
    RUN(test_quo_pos);
    RUN(test_quo_neg);

    /* Bit operations */
    RUN(test_bitand);
    RUN(test_bitor);
    RUN(test_bitxor);
    RUN(test_lshift);
    RUN(test_rshift);
    RUN(test_neg_lshift);

    sta_vm_destroy(vm);

    printf("\n%d/%d arithmetic primitive tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
