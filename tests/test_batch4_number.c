/* tests/test_batch4_number.c
 * Phase 1.5 Batch 4, Stories 3+5: Number protocol expansion,
 * SmallInteger base conversion, gcd:/lcm: validation.
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

/* ── Story 3: Number protocol ─────────────────────────────────────────── */

static void test_isNumber_true(void) {
    assert_eval("5 isNumber", "true");
}

static void test_isNumber_false(void) {
    assert_eval("'hello' isNumber", "false");
}

static void test_isInteger_true(void) {
    assert_eval("5 isInteger", "true");
}

static void test_isInteger_false_on_object(void) {
    assert_eval("nil isInteger", "false");
}

static void test_isZero_true(void) {
    assert_eval("0 isZero", "true");
}

static void test_isZero_false(void) {
    assert_eval("5 isZero", "false");
}

static void test_positive_true(void) {
    assert_eval("5 positive", "true");
}

static void test_positive_false(void) {
    assert_eval("-5 positive", "false");
}

static void test_positive_zero(void) {
    assert_eval("0 positive", "true");
}

static void test_negative_true(void) {
    assert_eval("-3 negative", "true");
}

static void test_negative_false(void) {
    assert_eval("3 negative", "false");
}

static void test_sign_positive(void) {
    assert_eval("7 sign", "1");
}

static void test_sign_negative(void) {
    assert_eval("-3 sign", "-1");
}

static void test_sign_zero(void) {
    assert_eval("0 sign", "0");
}

static void test_between_and_true(void) {
    assert_eval("5 between: 1 and: 10", "true");
}

static void test_between_and_false(void) {
    assert_eval("15 between: 1 and: 10", "false");
}

static void test_between_and_edge(void) {
    assert_eval("10 between: 1 and: 10", "true");
}

/* ── Story 3: SmallInteger base conversion ────────────────────────────── */

static void test_printString_hex(void) {
    assert_eval("255 printString: 16", "'FF'");
}

static void test_printString_binary(void) {
    assert_eval("8 printString: 2", "'1000'");
}

static void test_printString_octal(void) {
    assert_eval("10 printString: 8", "'12'");
}

static void test_printString_negative_hex(void) {
    assert_eval("-255 printString: 16", "'-FF'");
}

static void test_printString_zero(void) {
    assert_eval("0 printString: 16", "'0'");
}

static void test_printStringHex(void) {
    assert_eval("255 printStringHex", "'FF'");
}

static void test_printStringOctal(void) {
    assert_eval("10 printStringOctal", "'12'");
}

static void test_printStringBinary(void) {
    assert_eval("8 printStringBinary", "'1000'");
}

static void test_asInteger(void) {
    assert_eval("42 asInteger", "42");
}

/* ── Story 5: gcd: and lcm: ──────────────────────────────────────────── */

static void test_gcd_basic(void) {
    assert_eval("12 gcd: 8", "4");
}

static void test_gcd_coprime(void) {
    assert_eval("46368 gcd: 28657", "1");
}

static void test_lcm_basic(void) {
    assert_eval("12 lcm: 8", "24");
}

static void test_lcm_coprime(void) {
    assert_eval("5 lcm: 7", "35");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);
    printf("test_batch4_number (Phase 1.5 Batch 4, Stories 3+5):\n");

    /* Story 3: Number protocol */
    RUN(test_isNumber_true);
    RUN(test_isNumber_false);
    RUN(test_isInteger_true);
    RUN(test_isInteger_false_on_object);
    RUN(test_isZero_true);
    RUN(test_isZero_false);
    RUN(test_positive_true);
    RUN(test_positive_false);
    RUN(test_positive_zero);
    RUN(test_negative_true);
    RUN(test_negative_false);
    RUN(test_sign_positive);
    RUN(test_sign_negative);
    RUN(test_sign_zero);
    RUN(test_between_and_true);
    RUN(test_between_and_false);
    RUN(test_between_and_edge);

    /* Story 3: Base conversion */
    RUN(test_printString_hex);
    RUN(test_printString_binary);
    RUN(test_printString_octal);
    RUN(test_printString_negative_hex);
    RUN(test_printString_zero);
    RUN(test_printStringHex);
    RUN(test_printStringOctal);
    RUN(test_printStringBinary);
    RUN(test_asInteger);

    /* Story 5: gcd:/lcm: */
    RUN(test_gcd_basic);
    RUN(test_gcd_coprime);
    RUN(test_lcm_basic);
    RUN(test_lcm_coprime);

    sta_vm_destroy(vm);
    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
