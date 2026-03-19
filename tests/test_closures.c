/* tests/test_closures.c
 * Phase 2 Epic 1: Full closure tests — captured variables, NLR, ensure:.
 * Uses only the public API (sta_vm_create, sta_eval, sta_inspect_cstring).
 */
#include <sta/vm.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    fflush(stdout); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static STA_VM *vm;

static const char *eval_str(const char *expr) {
    STA_Handle *h = sta_eval(vm, expr);
    if (!h) {
        fprintf(stderr, "\neval failed: %s\n  expr: %s\n",
                sta_vm_last_error(vm), expr);
    }
    assert(h != NULL);
    const char *s = sta_inspect_cstring(vm, h);
    sta_handle_release(vm, h);
    return s;
}

static void assert_eval(const char *expr, const char *expected) {
    const char *got = eval_str(expr);
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "\n  FAIL: %s\n    expected: %s\n    got:      %s\n",
                expr, expected, got);
    }
    assert(strcmp(got, expected) == 0);
}

/* ── Mutable capture tests ─────────────────────────────────────────── */

static void test_capture_read_outer_var(void) {
    assert_eval("| x b | x := 42. b := [x]. b value", "42");
}

static void test_capture_mutate_outer_var(void) {
    assert_eval("| x | x := 1. [x := x + 10] value. x", "11");
}

static void test_shared_mutation(void) {
    assert_eval(
        "| x inc read | x := 0. "
        "inc := [x := x + 1]. "
        "read := [x]. "
        "inc value. inc value. inc value. read value",
        "3");
}

static void test_to_do_mutable_accumulator(void) {
    assert_eval(
        "| sum | sum := 0. 1 to: 10 do: [:i | sum := sum + i]. sum",
        "55");
}

static void test_collect_with_captured_state(void) {
    assert_eval(
        "| count | count := 0. "
        "#(10 20 30) collect: [:each | count := count + 1. each + count]",
        "#(11 22 33)");
}

static void test_select_with_captured_threshold(void) {
    assert_eval(
        "| threshold | threshold := 3. "
        "#(1 2 3 4 5) select: [:each | each > threshold]",
        "#(4 5)");
}

/* ── Non-local return tests ────────────────────────────────────────── */

static void test_nlr_from_block(void) {
    /* ^ inside a block returns from the enclosing method (doIt).
     * The NLR unwinds through `block value` back to doIt. */
    assert_eval("| block | block := [^42]. block value. 99", "42");
}

static void test_nlr_skips_remaining(void) {
    /* Code after the NLR block invocation is not executed. */
    assert_eval("| x | x := 1. [^x] value. x := 999. x", "1");
}

static void test_nlr_through_send(void) {
    /* NLR through a regular message send chain. */
    assert_eval(
        "| block | block := [^42]. block value + 100",
        "42");
}

/* ── ensure: tests ─────────────────────────────────────────────────── */

static void test_ensure_normal_completion(void) {
    assert_eval(
        "| result | result := 0. "
        "[result := 42] ensure: [result := result + 1]. result",
        "43");
}

static void test_ensure_body_result_preserved(void) {
    assert_eval("[42] ensure: [99]", "42");
}

static void test_ensure_during_exception(void) {
    /* ensure: block must fire even when an exception is caught.
     * Use Error new signal to trigger a real exception. */
    assert_eval(
        "| flag | flag := 0. "
        "[[flag := 1. Error new signal] ensure: [flag := flag + 10]] "
        "on: Error do: [:e | flag]",
        "11");
}

static void test_ensure_plus_on_do(void) {
    /* ensure: fires even when on:do: catches the exception. */
    assert_eval(
        "| log | log := 0. "
        "[[Error new signal] ensure: [log := 99]] on: Error do: [:e | log]",
        "99");
}

/* ── Regression tests ──────────────────────────────────────────────── */

static void test_clean_block_still_works(void) {
    assert_eval("[3 + 4] value", "7");
    assert_eval("[:x | x * 2] value: 21", "42");
}

static void test_inject_into_no_regression(void) {
    assert_eval("#(1 2 3 4 5) inject: 0 into: [:sum :each | sum + each]", "15");
}

static void test_existing_arithmetic(void) {
    assert_eval("3 + 4", "7");
    assert_eval("10 factorial", "3628800");
}

static void test_existing_boolean(void) {
    assert_eval("true ifTrue: [42] ifFalse: [0]", "42");
    assert_eval("false ifTrue: [42] ifFalse: [0]", "0");
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_closures:\n");

    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);

    /* Mutable capture */
    RUN(test_capture_read_outer_var);
    RUN(test_capture_mutate_outer_var);
    RUN(test_shared_mutation);
    RUN(test_to_do_mutable_accumulator);
    RUN(test_collect_with_captured_state);
    RUN(test_select_with_captured_threshold);

    /* Non-local return */
    RUN(test_nlr_from_block);
    RUN(test_nlr_skips_remaining);
    RUN(test_nlr_through_send);

    /* ensure: */
    RUN(test_ensure_normal_completion);
    RUN(test_ensure_body_result_preserved);
    RUN(test_ensure_during_exception);
    RUN(test_ensure_plus_on_do);

    /* Regression */
    RUN(test_clean_block_still_works);
    RUN(test_inject_into_no_regression);
    RUN(test_existing_arithmetic);
    RUN(test_existing_boolean);

    sta_vm_destroy(vm);

    printf("\n%d/%d closure tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
