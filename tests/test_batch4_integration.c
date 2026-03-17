/* tests/test_batch4_integration.c
 * Phase 1.5 Batch 4, Story 6: Integration tests combining
 * Batch 4 features with earlier batches.
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

/* ── Integration: hex printString exercises \\, //, String at:, etc. ───── */

static void test_hex_printString(void) {
    assert_eval("255 printString: 16", "'FF'");
}

/* ── Integration: symbol round-trip ───────────────────────────────────── */

static void test_symbol_roundtrip(void) {
    assert_eval("'hello' asSymbol asString = 'hello'", "true");
}

/* ── Integration: hash consistency across String/Symbol ───────────────── */

static void test_hash_consistency(void) {
    assert_eval("'hello' hash = 'hello' asSymbol hash", "true");
}

/* ── Integration: OC inject:into: with Batch 1 arithmetic ─────────────── */

static void test_oc_inject(void) {
    assert_eval(
        "| a | a := OrderedCollection new. "
        "a add: 10. a add: 20. a add: 30. "
        "a inject: 0 into: [:sum :each | sum + each]", "60");
}

/* ── Integration: between:and: ────────────────────────────────────────── */

static void test_between_and(void) {
    assert_eval("42 between: 1 and: 100", "true");
}

/* ── Integration: sign ────────────────────────────────────────────────── */

static void test_negative_sign(void) {
    assert_eval("-7 sign", "-1");
}

/* ── Integration: string printString after codegen fix ────────────────── */

static void test_string_printString(void) {
    assert_eval("'hi' printString", "'''hi'''");
}

/* ── Integration: isNumber polymorphism ────────────────────────────────── */

static void test_isNumber_polymorphic(void) {
    assert_eval("42 isNumber", "true");
}

static void test_isNumber_nil(void) {
    assert_eval("nil isNumber", "false");
}

/* ── Integration: printString base with larger values ─────────────────── */

static void test_hex_large(void) {
    assert_eval("4095 printString: 16", "'FFF'");
}

static void test_binary_15(void) {
    assert_eval("15 printString: 2", "'1111'");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);
    printf("test_batch4_integration (Phase 1.5 Batch 4, Story 6):\n");

    RUN(test_hex_printString);
    RUN(test_symbol_roundtrip);
    RUN(test_hash_consistency);
    RUN(test_oc_inject);
    RUN(test_between_and);
    RUN(test_negative_sign);
    RUN(test_string_printString);
    RUN(test_isNumber_polymorphic);
    RUN(test_isNumber_nil);
    RUN(test_hex_large);
    RUN(test_binary_15);

    sta_vm_destroy(vm);
    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
