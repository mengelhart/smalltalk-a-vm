/* tests/test_closure_integration.c
 * Phase 2.5 Epic 1, Story 1: Mutable capture validation tests.
 *
 * Exercises closure patterns that were impossible in Phase 1.5:
 * to:do: with accumulators, do: with side effects, nested captures,
 * collection operations with captured state, NLR in iteration, etc.
 *
 * Uses only the public API (sta_vm_create, sta_eval, sta_inspect_cstring).
 * Complements test_closures.c with patterns targeting real kernel usage.
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

/* ── Test 1: to:do: with Array at:put: ────────────────────────────────── */

static void test_to_do_collect_into_array(void) {
    assert_eval(
        "| a | a := Array new: 5. "
        "1 to: 5 do: [:i | a at: i put: i * i]. "
        "a at: 3",
        "9");
}

/* ── Test 2: OrderedCollection do: with captured counter ──────────────── */

static void test_do_with_counter(void) {
    assert_eval(
        "| oc count | "
        "oc := OrderedCollection new. "
        "oc add: 10. oc add: 20. oc add: 30. "
        "count := 0. "
        "oc do: [:each | count := count + each]. "
        "count",
        "60");
}

/* ── Test 3: collect: captures outer variable ─────────────────────────── */

static void test_collect_with_outer_capture(void) {
    assert_eval(
        "| factor | "
        "factor := 10. "
        "#(1 2 3) collect: [:each | each * factor]",
        "#(10 20 30)");
}

/* ── Test 4: select: with captured threshold ──────────────────────────── */

static void test_select_with_capture(void) {
    assert_eval(
        "| threshold | "
        "threshold := 15. "
        "#(10 20 30 5) select: [:each | each > threshold]",
        "#(20 30)");
}

/* ── Test 5: inject:into: closure ─────────────────────────────────────── */

static void test_inject_into_closure(void) {
    assert_eval(
        "#(1 2 3 4 5) inject: 0 into: [:sum :each | sum + each]",
        "15");
}

/* ── Test 6: Nested closure capture ───────────────────────────────────── */

static void test_nested_closure_capture(void) {
    /* Inner block captures both the outer block's arg and the method's var. */
    assert_eval(
        "| x inner | "
        "x := 1. "
        "inner := [:a | a + x]. "
        "inner value: 100",
        "101");
}

/* ── Test 7: Nested blocks — block returning block that captures ──────── */

static void test_block_returning_block(void) {
    /* A block that returns another block which captures the first's arg. */
    assert_eval(
        "| maker adder | "
        "maker := [:n | [:x | x + n]]. "
        "adder := maker value: 10. "
        "adder value: 32",
        "42");
}

/* ── Test 8: detect:ifNone: with captured state ───────────────────────── */

static void test_detect_ifNone_with_capture(void) {
    assert_eval(
        "| target | "
        "target := 42. "
        "#(10 42 30) detect: [:each | each = target] ifNone: [0]",
        "42");
}

/* ── Test 9: detect:ifNone: — noneBlock fires ─────────────────────────── */

static void test_detect_ifNone_not_found(void) {
    assert_eval(
        "#(10 20 30) detect: [:each | each = 99] ifNone: [-1]",
        "-1");
}

/* ── Test 10: Closure survives method return (first-class block) ──────── */

static void test_closure_first_class(void) {
    assert_eval("| block | block := [:x | x + 1]. block value: 41", "42");
}

/* ── Test 11: ensure: with captured mutation ───────────────────────────── */

static void test_ensure_with_captured_mutation(void) {
    assert_eval(
        "| log | "
        "log := OrderedCollection new. "
        "[log add: 'body'] ensure: [log add: 'ensure']. "
        "log size",
        "2");
}

/* ── Test 12: Multiple closures sharing same captured variable ────────── */

static void test_multiple_closures_shared_var(void) {
    assert_eval(
        "| x inc dec | "
        "x := 10. "
        "inc := [x := x + 1]. "
        "dec := [x := x - 1]. "
        "inc value. inc value. inc value. dec value. x",
        "12");
}

/* ── Test 13: to:do: nested with outer accumulator ────────────────────── */

static void test_nested_to_do(void) {
    assert_eval(
        "| total | "
        "total := 0. "
        "1 to: 3 do: [:i | "
        "    1 to: 3 do: [:j | "
        "        total := total + (i * j)]]. "
        "total",
        "36");
}

/* ── Test 14: Factorial via to:do: with accumulator ───────────────────── */

static void test_factorial_via_to_do(void) {
    assert_eval(
        "| result | "
        "result := 1. "
        "1 to: 10 do: [:i | result := result * i]. "
        "result",
        "3628800");
}

/* ── Story 3: Collection + iteration integration tests ─────────────── */

/* ── Test 15: OrderedCollection add via to:do: ─────────────────────── */

static void test_oc_add_via_to_do(void) {
    assert_eval(
        "| oc | "
        "oc := OrderedCollection new. "
        "1 to: 20 do: [:i | oc add: i]. "
        "oc size",
        "20");
}

/* ── Test 16: OrderedCollection collect: ───────────────────────────── */

static void test_oc_collect(void) {
    assert_eval(
        "| oc result | "
        "oc := OrderedCollection new. "
        "1 to: 5 do: [:i | oc add: i]. "
        "result := oc collect: [:each | each * each]. "
        "result at: 3",
        "9");
}

/* ── Test 17: Nested iteration with capture ────────────────────────── */

static void test_nested_iteration_capture(void) {
    assert_eval(
        "| total | "
        "total := 0. "
        "1 to: 3 do: [:i | "
        "    1 to: 3 do: [:j | "
        "        total := total + (i * j)]]. "
        "total",
        "36");
}

/* ── Test 18: select: + size ───────────────────────────────────────── */

static void test_oc_select_size(void) {
    assert_eval(
        "| oc | "
        "oc := OrderedCollection new. "
        "1 to: 20 do: [:i | oc add: i]. "
        "(oc select: [:each | each even]) size",
        "10");
}

/* ── Test 19: inject:into: over OrderedCollection ──────────────────── */

static void test_oc_inject_into(void) {
    assert_eval(
        "| oc | "
        "oc := OrderedCollection new. "
        "1 to: 10 do: [:i | oc add: i]. "
        "oc inject: 0 into: [:sum :each | sum + each]",
        "55");
}

/* ── Test 20: detect:ifNone: ───────────────────────────────────────── */

static void test_oc_detect_ifNone(void) {
    assert_eval(
        "| oc | "
        "oc := OrderedCollection new. "
        "oc add: 'a'. oc add: 'b'. oc add: 'c'. "
        "oc detect: [:each | each = 'b'] ifNone: ['not found']",
        "'b'");
}

/* ── Test 21: Factorial via to:do: with accumulator ────────────────── */

static void test_factorial_accumulator(void) {
    assert_eval(
        "| result | "
        "result := 1. "
        "1 to: 10 do: [:i | result := result * i]. "
        "result",
        "3628800");
}

/* ── Test 22: Chained collection operations ────────────────────────── */

static void test_chained_collection_ops(void) {
    assert_eval(
        "| oc | "
        "oc := OrderedCollection new. "
        "1 to: 10 do: [:i | oc add: i]. "
        "(oc select: [:each | each > 5]) inject: 0 into: [:sum :each | sum + each]",
        "40");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_closure_integration (Phase 2.5 Epic 1):\n");

    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);

    printf("\n  -- Story 1: Mutable capture validation --\n");
    RUN(test_to_do_collect_into_array);
    RUN(test_do_with_counter);
    RUN(test_collect_with_outer_capture);
    RUN(test_select_with_capture);
    RUN(test_inject_into_closure);
    RUN(test_nested_closure_capture);
    RUN(test_block_returning_block);
    RUN(test_detect_ifNone_with_capture);
    RUN(test_detect_ifNone_not_found);
    RUN(test_closure_first_class);
    RUN(test_ensure_with_captured_mutation);
    RUN(test_multiple_closures_shared_var);
    RUN(test_nested_to_do);
    RUN(test_factorial_via_to_do);

    printf("\n  -- Story 3: Collection + iteration integration --\n");
    RUN(test_oc_add_via_to_do);
    RUN(test_oc_collect);
    RUN(test_nested_iteration_capture);
    RUN(test_oc_select_size);
    RUN(test_oc_inject_into);
    RUN(test_oc_detect_ifNone);
    RUN(test_factorial_accumulator);
    RUN(test_chained_collection_ops);

    sta_vm_destroy(vm);

    printf("\n%d/%d closure integration tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
