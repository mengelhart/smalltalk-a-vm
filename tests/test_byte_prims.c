/* tests/test_byte_prims.c
 * Phase 1.5 Batch 2, Story 4: Byte-indexable primitive tests.
 * Tests ByteArray byte access, String character access,
 * Character value round-trip.
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

static void assert_eval_fails(const char *expr) {
    STA_Handle *h = sta_eval(vm, expr);
    if (h) sta_handle_release(vm, h);
    /* No crash is the success criterion. */
}

/* ── ByteArray prims 60-62 ──────────────────────────────────────────── */

static void test_bytearray_at_put_at(void) {
    assert_eval(
        "| ba | ba := ByteArray new: 4. "
        "ba at: 1 put: 65. ba at: 2 put: 66. "
        "ba at: 3 put: 67. ba at: 4 put: 68. ba at: 1", "65");
}

static void test_bytearray_at_all(void) {
    assert_eval(
        "| ba | ba := ByteArray new: 3. "
        "ba at: 1 put: 10. ba at: 2 put: 20. ba at: 3 put: 30. ba at: 3", "30");
}

static void test_bytearray_size(void) {
    assert_eval("(ByteArray new: 4) size", "4");
}

static void test_bytearray_bounds_low(void) {
    assert_eval_fails("(ByteArray new: 4) at: 0");
}

static void test_bytearray_bounds_high(void) {
    assert_eval_fails("(ByteArray new: 4) at: 5");
}

/* ── String prims 63-64 ─────────────────────────────────────────────── */

static void test_string_size(void) {
    assert_eval("'hello' size", "5");
}

static void test_string_at_first(void) {
    /* $h has code point 104 */
    assert_eval("'hello' at: 1", "$h");
}

static void test_string_at_last(void) {
    assert_eval("'hello' at: 5", "$o");
}

static void test_string_at_bounds_low(void) {
    assert_eval_fails("'hello' at: 0");
}

static void test_string_at_bounds_high(void) {
    assert_eval_fails("'hello' at: 6");
}

/* ── Character prims 94-95 ──────────────────────────────────────────── */

static void test_char_value_A(void) {
    assert_eval("$A value", "65");
}

static void test_char_value_a(void) {
    assert_eval("$a value", "97");
}

static void test_char_value_0(void) {
    assert_eval("$0 value", "48");
}

static void test_char_value_create(void) {
    assert_eval("Character value: 65", "$A");
}

static void test_char_roundtrip(void) {
    assert_eval("(Character value: 72) value", "72");
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_byte_prims (Phase 1.5 Batch 2):\n");

    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);

    /* ByteArray */
    RUN(test_bytearray_at_put_at);
    RUN(test_bytearray_at_all);
    RUN(test_bytearray_size);
    RUN(test_bytearray_bounds_low);
    RUN(test_bytearray_bounds_high);

    /* String */
    RUN(test_string_size);
    RUN(test_string_at_first);
    RUN(test_string_at_last);
    RUN(test_string_at_bounds_low);
    RUN(test_string_at_bounds_high);

    /* Character */
    RUN(test_char_value_A);
    RUN(test_char_value_a);
    RUN(test_char_value_0);
    RUN(test_char_value_create);
    RUN(test_char_roundtrip);

    sta_vm_destroy(vm);

    printf("\n%d/%d byte primitive tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
