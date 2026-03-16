/* tests/test_batch1_integration.c
 * Phase 1.5 Batch 1, Story 8: Integration tests.
 * Tests SmallInteger printString, even/odd, gcd:, lcm:,
 * to:do:, timesRepeat:, and combined operations.
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

/* ── printString (prim 200) ──────────────────────────────────────────── */

static void test_printstring_zero(void)     { assert_eval("0 printString", "'0'"); }
static void test_printstring_pos(void)      { assert_eval("42 printString", "'42'"); }
static void test_printstring_neg(void)      { assert_eval("-7 printString", "'-7'"); }
static void test_printstring_factorial(void) {
    assert_eval("10 factorial printString", "'3628800'");
}

/* ── even / odd ──────────────────────────────────────────────────────── */

static void test_even_true(void)  { assert_eval("12 even", "true"); }
static void test_even_false(void) { assert_eval("7 even", "false"); }
static void test_odd_true(void)   { assert_eval("7 odd", "true"); }
static void test_odd_false(void)  { assert_eval("12 odd", "false"); }
static void test_zero_even(void)  { assert_eval("0 even", "true"); }

/* ── gcd: ─────────────────────────────────────────────────────────── */

static void test_gcd_basic(void)     { assert_eval("12 gcd: 8", "4"); }
static void test_gcd_coprime(void)   {
    /* Consecutive Fibonacci numbers are coprime — 24 tail calls. */
    assert_eval("46368 gcd: 28657", "1");
}
static void test_gcd_zero(void)      { assert_eval("42 gcd: 0", "42"); }
static void test_gcd_negative(void)  { assert_eval("-12 gcd: 8", "4"); }

/* ── lcm: ─────────────────────────────────────────────────────────── */

static void test_lcm_basic(void) { assert_eval("12 lcm: 8", "24"); }
static void test_lcm_zero(void)  { assert_eval("0 lcm: 5", "0"); }

/* ── to:do: ───────────────────────────────────────────────────────── */

/* to:do: with a non-capturing block — just runs without crash.
 * The block [:i | i + 1] doesn't capture outer temps. */
static void test_to_do_runs(void) {
    /* to:do: has no explicit ^ return, so it returns self (the receiver). */
    assert_eval("1 to: 5 do: [:i | i + 1]", "1");
}

/* ── timesRepeat: ──────────────────────────────────────────────────── */

static void test_times_repeat_runs(void) {
    /* timesRepeat: runs without crash. Return value is nil (whileTrue: result). */
    assert_eval("3 timesRepeat: [1 + 1]", "nil");
}

/* ── whileTrue: with mutable temp (inlined, so it works) ───────────── */

static void test_while_true_sum(void) {
    /* This uses only inlined whileTrue: with outer temp access. */
    assert_eval("| sum | sum := 0. [sum < 55] whileTrue: [sum := sum + 1]. sum", "55");
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_batch1_integration (Phase 1.5 Batch 1):\n");

    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);

    /* printString */
    RUN(test_printstring_zero);
    RUN(test_printstring_pos);
    RUN(test_printstring_neg);
    RUN(test_printstring_factorial);

    /* even / odd */
    RUN(test_even_true);
    RUN(test_even_false);
    RUN(test_odd_true);
    RUN(test_odd_false);
    RUN(test_zero_even);

    /* gcd: */
    RUN(test_gcd_basic);
    RUN(test_gcd_coprime);
    RUN(test_gcd_zero);
    RUN(test_gcd_negative);

    /* lcm: */
    RUN(test_lcm_basic);
    RUN(test_lcm_zero);

    /* to:do: */
    RUN(test_to_do_runs);

    /* timesRepeat: */
    RUN(test_times_repeat_runs);

    /* whileTrue: sum */
    RUN(test_while_true_sum);

    sta_vm_destroy(vm);

    printf("\n%d/%d batch 1 integration tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
