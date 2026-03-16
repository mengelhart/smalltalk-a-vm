/* tests/test_batch2_integration.c
 * Phase 1.5 Batch 2, Story 8: Integration tests.
 * Tests Character protocol, String operations, ByteArray,
 * polymorphic dispatch, concatenation chains.
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

/* ── Character protocol ─────────────────────────────────────────────── */

static void test_char_isLetter(void)    { assert_eval("$A isLetter", "true"); }
static void test_char_isDigit(void)     { assert_eval("$1 isDigit", "true"); }
static void test_char_isUppercase(void) { assert_eval("$A isUppercase", "true"); }
static void test_char_isLowercase(void) { assert_eval("$a isLowercase", "true"); }

static void test_char_asLowercase(void) {
    assert_eval("$A asLowercase value", "97");
}
static void test_char_asUppercase(void) {
    assert_eval("$a asUppercase value", "65");
}

static void test_char_asString(void) {
    assert_eval("$A asString size", "1");
}

static void test_char_printString(void) {
    assert_eval("$A printString size", "2");
}

/* ── String operations ──────────────────────────────────────────────── */

static void test_string_size(void) {
    assert_eval("'hello' size", "5");
}

static void test_string_reversed(void) {
    assert_eval("'hello' reversed", "'olleh'");
}

static void test_string_asUppercase(void) {
    assert_eval("'hello' asUppercase", "'HELLO'");
}

static void test_string_asLowercase(void) {
    assert_eval("'HELLO' asLowercase", "'hello'");
}

static void test_string_concat(void) {
    assert_eval("'hello', ' world'", "'hello world'");
}

static void test_string_copyFromTo(void) {
    assert_eval("'hello' copyFrom: 2 to: 4", "'ell'");
}

static void test_string_includes_yes(void) {
    assert_eval("'hello' includes: $l", "true");
}

static void test_string_includes_no(void) {
    assert_eval("'hello' includes: $z", "false");
}

static void test_string_equal_yes(void) {
    assert_eval("'hello' = 'hello'", "true");
}

static void test_string_equal_no(void) {
    assert_eval("'hello' = 'world'", "false");
}

static void test_string_isEmpty_yes(void) {
    assert_eval("'' isEmpty", "true");
}

static void test_string_isEmpty_no(void) {
    assert_eval("'hello' isEmpty", "false");
}

/* ── Polymorphic printString ─────────────────────────────────────────── */

static void test_int_printString(void) {
    assert_eval("42 printString", "'42'");
}

static void test_string_printString(void) {
    /* Smalltalk-80: 'hi' printString => '''hi''' (6 chars: ''hi'').
     * inspect wraps in quotes: '''hi''' */
    assert_eval("'hi' printString", "'''hi'''");
}

/* ── ByteArray ──────────────────────────────────────────────────────── */

static void test_bytearray_asString(void) {
    assert_eval(
        "| ba | ba := ByteArray new: 3. "
        "ba at: 1 put: 72. ba at: 2 put: 69. ba at: 3 put: 76. "
        "ba asString", "'HEL'");
}

static void test_bytearray_printString(void) {
    assert_eval(
        "| ba | ba := ByteArray new: 3. "
        "ba at: 1 put: 72. ba at: 2 put: 69. ba at: 3 put: 76. "
        "ba printString", "'#[72 69 76]'");
}

/* ── Concatenation chain ─────────────────────────────────────────────── */

static void test_concat_chain(void) {
    assert_eval("'a', 'b', 'c', 'd'", "'abcd'");
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_batch2_integration (Phase 1.5 Batch 2):\n");

    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);

    /* Character protocol */
    RUN(test_char_isLetter);
    RUN(test_char_isDigit);
    RUN(test_char_isUppercase);
    RUN(test_char_isLowercase);
    RUN(test_char_asLowercase);
    RUN(test_char_asUppercase);
    RUN(test_char_asString);
    RUN(test_char_printString);

    /* String operations */
    RUN(test_string_size);
    RUN(test_string_reversed);
    RUN(test_string_asUppercase);
    RUN(test_string_asLowercase);
    RUN(test_string_concat);
    RUN(test_string_copyFromTo);
    RUN(test_string_includes_yes);
    RUN(test_string_includes_no);
    RUN(test_string_equal_yes);
    RUN(test_string_equal_no);
    RUN(test_string_isEmpty_yes);
    RUN(test_string_isEmpty_no);

    /* Polymorphic printString */
    RUN(test_int_printString);
    RUN(test_string_printString);

    /* ByteArray */
    RUN(test_bytearray_asString);
    RUN(test_bytearray_printString);

    /* Concatenation chain */
    RUN(test_concat_chain);

    sta_vm_destroy(vm);

    printf("\n%d/%d batch 2 integration tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
