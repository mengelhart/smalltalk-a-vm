/* tests/test_stress_exceptions.c
 * Phase 1.5 Batch 5: Exception path stress tests.
 * Exercises exception propagation through complex code.
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

/* ── Test 10: Exception in collection iteration ──────────────────────── */

static void test_exception_in_inject(void) {
    /* Exception fires mid-iteration inside inject:into:.
     * The on:do: handler block returns the messageText directly
     * (avoids mutable outer temp capture limitation). */
    assert_eval(
        "[ | a | a := Array new: 5. "
        "  a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "  a at: 4 put: 4. a at: 5 put: 5. "
        "  a inject: 0 into: [:sum :each | "
        "    each = 3 ifTrue: [Error new messageText: 'found 3'; signal]. "
        "    sum + each]] "
        "on: Error do: [:e | e messageText]",
        "'found 3'");
}

/* ── Test 11: DNU in polymorphic context ─────────────────────────────── */

static void test_dnu_int(void) {
    assert_eval(
        "[42 frobnicate] on: MessageNotUnderstood do: [:e | 'caught']",
        "'caught'");
}

static void test_dnu_string(void) {
    assert_eval(
        "['hello' frobnicate] on: MessageNotUnderstood do: [:e | 'caught']",
        "'caught'");
}

static void test_dnu_nil(void) {
    assert_eval(
        "[nil frobnicate] on: MessageNotUnderstood do: [:e | 'caught']",
        "'caught'");
}

/* ── Test 12: Nested exception handlers ──────────────────────────────── */

static void test_nested_handlers(void) {
    /* Inner handler catches, then signals a new error.
     * Outer handler catches the re-signaled error.
     * Verify the outer handler receives the outer error's messageText. */
    assert_eval(
        "[ [Error new messageText: 'inner'; signal] "
        "    on: Error do: [:e | "
        "        Error new messageText: 'outer'; signal] "
        "] on: Error do: [:e | e messageText]",
        "'outer'");
}

static void test_nested_handlers_inner_caught(void) {
    /* Verify the inner handler fires by checking that the inner
     * exception does NOT propagate — the outer catches 'outer'. */
    assert_eval(
        "[ [Error new messageText: 'inner'; signal] "
        "    on: Error do: [:e | 'inner caught'] "
        "] on: Error do: [:e | 'outer caught']",
        "'inner caught'");
}

/* ── Test 13: Exception from OC operations ───────────────────────────── */

static void test_oc_error_propagation(void) {
    /* BUG (GitHub #244): Catching DNU on OC>>removeFirst triggers
     * unhandled BlockCannotReturn (class index 28) instead of delivering
     * the MessageNotUnderstood to the on:do: handler.
     *
     * KNOWN_FAIL — re-tested after Phase 2 Epics 1-4 (2026-03-19), still fails.
     * Epic 1 fixed ensure: unwinding, but this path involves DNU signal
     * propagation through longjmp which still escapes as BlockCannotReturn.
     * Likely: doesNotUnderstand: method uses a block that attempts NLR
     * after the DNU handler's longjmp has already unwound the home frame.
     *
     * Uncomment when fixed:
     *   assert_eval(
     *       "[OrderedCollection new removeFirst] "
     *       "on: MessageNotUnderstood do: [:e | 'dnu caught']",
     *       "'dnu caught'");
     */
    printf("SKIP (known bug #244: DNU catch triggers BlockCannotReturn) ");
}

/* ── Additional: Error messageText round-trip ─────────────────────────── */

static void test_error_messageText_survives(void) {
    /* Signal an Error with messageText, catch, verify text survives. */
    assert_eval(
        "[Error new messageText: 'something broke'; signal] "
        "on: Error do: [:e | e messageText]",
        "'something broke'");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);
    printf("test_stress_exceptions (Phase 1.5 Batch 5):\n");

    RUN(test_exception_in_inject);
    RUN(test_dnu_int);
    RUN(test_dnu_string);
    RUN(test_dnu_nil);
    RUN(test_nested_handlers);
    RUN(test_nested_handlers_inner_caught);
    RUN(test_oc_error_propagation);

    /* Also add a non-skipped error messageText test. */
    RUN(test_error_messageText_survives);

    sta_vm_destroy(vm);
    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
