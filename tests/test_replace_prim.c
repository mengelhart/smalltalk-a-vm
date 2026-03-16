/* tests/test_replace_prim.c
 * Phase 1.5 Batch 3, Story 2: Prim 54 replaceFrom:to:with:startingAt: tests.
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
}

/* ── Array bulk copy ─────────────────────────────────────────────────── */

static void test_replace_array(void) {
    assert_eval(
        "| a b | "
        "a := Array new: 5. "
        "a at: 1 put: 10. a at: 2 put: 20. a at: 3 put: 30. "
        "a at: 4 put: 40. a at: 5 put: 50. "
        "b := Array new: 3. b at: 1 put: 77. b at: 2 put: 88. b at: 3 put: 99. "
        "a replaceFrom: 2 to: 4 with: b startingAt: 1. "
        "a at: 2", "77");
}

static void test_replace_array_end(void) {
    assert_eval(
        "| a b | "
        "a := Array new: 5. "
        "a at: 1 put: 10. a at: 2 put: 20. a at: 3 put: 30. "
        "a at: 4 put: 40. a at: 5 put: 50. "
        "b := Array new: 3. b at: 1 put: 77. b at: 2 put: 88. b at: 3 put: 99. "
        "a replaceFrom: 2 to: 4 with: b startingAt: 1. "
        "a at: 4", "99");
}

static void test_replace_array_untouched(void) {
    assert_eval(
        "| a b | "
        "a := Array new: 5. "
        "a at: 1 put: 10. a at: 2 put: 20. a at: 3 put: 30. "
        "a at: 4 put: 40. a at: 5 put: 50. "
        "b := Array new: 3. b at: 1 put: 77. b at: 2 put: 88. b at: 3 put: 99. "
        "a replaceFrom: 2 to: 4 with: b startingAt: 1. "
        "a at: 5", "50");
}

/* ── Self-overlap ────────────────────────────────────────────────────── */

static void test_replace_self_overlap(void) {
    /* [1,2,3,4,5] replaceFrom:2 to:4 with:self startingAt:1 → [1,1,2,3,5] */
    assert_eval(
        "| a | "
        "a := Array new: 5. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "a at: 4 put: 4. a at: 5 put: 5. "
        "a replaceFrom: 2 to: 4 with: a startingAt: 1. "
        "a at: 3", "2");
}

/* ── Bounds checking ─────────────────────────────────────────────────── */

static void test_replace_bounds_low(void) {
    assert_eval_fails(
        "| a b | a := Array new: 5. b := Array new: 3. "
        "a replaceFrom: 0 to: 2 with: b startingAt: 1");
}

static void test_replace_bounds_high(void) {
    assert_eval_fails(
        "| a b | a := Array new: 5. b := Array new: 3. "
        "a replaceFrom: 1 to: 6 with: b startingAt: 1");
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_replace_prim (Phase 1.5 Batch 3):\n");

    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);

    RUN(test_replace_array);
    RUN(test_replace_array_end);
    RUN(test_replace_array_untouched);
    RUN(test_replace_self_overlap);
    RUN(test_replace_bounds_low);
    RUN(test_replace_bounds_high);

    sta_vm_destroy(vm);

    printf("\n%d/%d replace primitive tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
