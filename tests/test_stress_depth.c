/* tests/test_stress_depth.c
 * Phase 1.5 Batch 5: Deep call chain stress tests.
 * Exercises stack to depths individual tests never reach.
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

/* ── Test 5: Nested Array printString (3 levels) ─────────────────────── */

static void test_nested_array_printString(void) {
    assert_eval(
        "| inner mid outer | "
        "inner := Array new: 2. inner at: 1 put: 1. inner at: 2 put: 2. "
        "mid := Array new: 2. mid at: 1 put: inner. mid at: 2 put: 3. "
        "outer := Array new: 2. outer at: 1 put: mid. outer at: 2 put: 4. "
        "outer printString",
        "'#(#(#(1 2) 3) 4)'");
}

/* ── Test 6: Recursive factorial (20! = 2432902008176640000) ──────────── */

static void test_factorial_20(void) {
    assert_eval("20 factorial", "2432902008176640000");
}

static void test_factorial_20_printString(void) {
    assert_eval("20 factorial printString", "'2432902008176640000'");
}

/* ── Test 7: Deep inject:into: chain ──────────────────────────────────── */

static void test_inject_10(void) {
    assert_eval(
        "| a i | a := Array new: 10. i := 1. "
        "[i <= 10] whileTrue: [a at: i put: i. i := i + 1]. "
        "a inject: 0 into: [:sum :each | sum + each]", "55");
}

static void test_inject_100(void) {
    /* Build 100-element array via whileTrue:, sum via inject:into:.
     * Sum of 1..100 = 5050. */
    assert_eval(
        "| a i | a := Array new: 100. i := 1. "
        "[i <= 100] whileTrue: [a at: i put: i. i := i + 1]. "
        "a inject: 0 into: [:sum :each | sum + each]", "5050");
}

/* ── Test 8: String concatenation chain (allocation pressure) ─────────── */

static void test_string_concat_50(void) {
    assert_eval(
        "| result i | result := ''. i := 1. "
        "[i <= 50] whileTrue: [result := result , 'a'. i := i + 1]. "
        "result size", "50");
}

/* ── Test 9: OrderedCollection grow under pressure ────────────────────── */

static void test_oc_grow_100(void) {
    assert_eval(
        "| oc i | oc := OrderedCollection new. i := 1. "
        "[i <= 100] whileTrue: [oc add: i. i := i + 1]. "
        "oc size", "100");
}

static void test_oc_grow_100_first_last(void) {
    assert_eval(
        "| oc i | oc := OrderedCollection new. i := 1. "
        "[i <= 100] whileTrue: [oc add: i. i := i + 1]. "
        "oc at: 1", "1");
    assert_eval(
        "| oc i | oc := OrderedCollection new. i := 1. "
        "[i <= 100] whileTrue: [oc add: i. i := i + 1]. "
        "oc at: 100", "100");
}

static void test_oc_collect_100(void) {
    assert_eval(
        "| oc i result | oc := OrderedCollection new. i := 1. "
        "[i <= 100] whileTrue: [oc add: i. i := i + 1]. "
        "result := oc collect: [:e | e * 2]. "
        "result size", "100");
}

static void test_oc_collect_100_values(void) {
    assert_eval(
        "| oc i result | oc := OrderedCollection new. i := 1. "
        "[i <= 100] whileTrue: [oc add: i. i := i + 1]. "
        "result := oc collect: [:e | e * 2]. "
        "result at: 1", "2");
    assert_eval(
        "| oc i result | oc := OrderedCollection new. i := 1. "
        "[i <= 100] whileTrue: [oc add: i. i := i + 1]. "
        "result := oc collect: [:e | e * 2]. "
        "result at: 100", "200");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);
    printf("test_stress_depth (Phase 1.5 Batch 5):\n");

    RUN(test_nested_array_printString);
    RUN(test_factorial_20);
    RUN(test_factorial_20_printString);
    RUN(test_inject_10);
    RUN(test_inject_100);
    RUN(test_string_concat_50);
    RUN(test_oc_grow_100);
    RUN(test_oc_grow_100_first_last);
    RUN(test_oc_collect_100);
    RUN(test_oc_collect_100_values);

    sta_vm_destroy(vm);
    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
