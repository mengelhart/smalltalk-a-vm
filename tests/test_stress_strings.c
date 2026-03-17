/* tests/test_stress_strings.c
 * Phase 1.5 Batch 5: String/Character pipeline stress tests.
 * Exercises byte-indexable path under sustained load.
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

/* ── Test 14: String reverse + case round-trip ───────────────────────── */

static void test_string_reverse(void) {
    assert_eval("'Hello World' reversed", "'dlroW olleH'");
}

static void test_string_reverse_roundtrip(void) {
    assert_eval("'Hello World' reversed reversed", "'Hello World'");
}

static void test_string_case_roundtrip(void) {
    assert_eval("'Hello World' asUppercase asLowercase", "'hello world'");
}

/* ── Test 15: String comparison boundary cases ───────────────────────── */

static void test_string_lt_basic(void) {
    assert_eval("'aaa' < 'aab'", "true");
    assert_eval("'aab' < 'aaa'", "false");
}

static void test_string_lt_length(void) {
    assert_eval("'aaa' < 'aaaa'", "true");
    assert_eval("'aaaa' < 'aaa'", "false");
}

static void test_string_lt_empty(void) {
    assert_eval("'' < 'a'", "true");
    assert_eval("'a' < ''", "false");
}

static void test_string_eq_empty(void) {
    assert_eval("'' = ''", "true");
}

/* ── Test 16: Character classification sweep ─────────────────────────── */

static void test_char_classify_letters(void) {
    /* 52 letters: A-Z (26) + a-z (26) */
    assert_eval(
        "| count i ch | count := 0. i := 0. "
        "[i <= 127] whileTrue: [ "
        "  ch := Character value: i. "
        "  ch isLetter ifTrue: [count := count + 1]. "
        "  i := i + 1]. "
        "count", "52");
}

static void test_char_classify_digits(void) {
    /* 10 digits: 0-9 */
    assert_eval(
        "| count i ch | count := 0. i := 0. "
        "[i <= 127] whileTrue: [ "
        "  ch := Character value: i. "
        "  ch isDigit ifTrue: [count := count + 1]. "
        "  i := i + 1]. "
        "count", "10");
}

static void test_char_classify_separators(void) {
    /* 5 separators: space(32), tab(9), lf(10), cr(13), ff(12) */
    assert_eval(
        "| count i ch | count := 0. i := 0. "
        "[i <= 127] whileTrue: [ "
        "  ch := Character value: i. "
        "  ch isSeparator ifTrue: [count := count + 1]. "
        "  i := i + 1]. "
        "count", "5");
}

/* ── Test 17: String hash distribution ───────────────────────────────── */

static void test_hash_distribution(void) {
    /* Hash 10 strings, count distinct values.
     * Expect at least 8 distinct out of 10. */
    assert_eval(
        "| hashes h count i j unique | "
        "hashes := Array new: 10. "
        "hashes at: 1 put: 'a' hash. "
        "hashes at: 2 put: 'b' hash. "
        "hashes at: 3 put: 'ab' hash. "
        "hashes at: 4 put: 'ba' hash. "
        "hashes at: 5 put: 'hello' hash. "
        "hashes at: 6 put: 'world' hash. "
        "hashes at: 7 put: 'foo' hash. "
        "hashes at: 8 put: 'bar' hash. "
        "hashes at: 9 put: 'test' hash. "
        "hashes at: 10 put: 'hash' hash. "
        "count := 0. i := 1. "
        "[i <= 10] whileTrue: [ "
        "  unique := true. j := 1. "
        "  [j < i and: [unique]] whileTrue: [ "
        "    (hashes at: j) = (hashes at: i) ifTrue: [unique := false]. "
        "    j := j + 1]. "
        "  unique ifTrue: [count := count + 1]. "
        "  i := i + 1]. "
        "count >= 8", "true");
}

/* ── Test 18: printString:base: sweep ────────────────────────────────── */

static void test_base_hex_255(void) {
    assert_eval("255 printString: 16", "'FF'");
}

static void test_base_binary_255(void) {
    assert_eval("255 printString: 2", "'11111111'");
}

static void test_base_octal_255(void) {
    assert_eval("255 printString: 8", "'377'");
}

static void test_base_hex_zero(void) {
    assert_eval("0 printString: 16", "'0'");
}

static void test_base_binary_one(void) {
    assert_eval("1 printString: 2", "'1'");
}

static void test_base_hex_negative(void) {
    assert_eval("-42 printString: 16", "'-2A'");
}

static void test_base_hex_16(void) {
    assert_eval("16 printString: 16", "'10'");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);
    printf("test_stress_strings (Phase 1.5 Batch 5):\n");

    /* String reverse + case */
    RUN(test_string_reverse);
    RUN(test_string_reverse_roundtrip);
    RUN(test_string_case_roundtrip);

    /* String comparisons */
    RUN(test_string_lt_basic);
    RUN(test_string_lt_length);
    RUN(test_string_lt_empty);
    RUN(test_string_eq_empty);

    /* Character classification */
    RUN(test_char_classify_letters);
    RUN(test_char_classify_digits);
    RUN(test_char_classify_separators);

    /* Hash distribution */
    RUN(test_hash_distribution);

    /* Base conversion sweep */
    RUN(test_base_hex_255);
    RUN(test_base_binary_255);
    RUN(test_base_octal_255);
    RUN(test_base_hex_zero);
    RUN(test_base_binary_one);
    RUN(test_base_hex_negative);
    RUN(test_base_hex_16);

    sta_vm_destroy(vm);
    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
