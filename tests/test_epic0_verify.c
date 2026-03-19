/* tests/test_epic0_verify.c
 * Verification tests for Epic 0: Per-Instance VM State.
 * Tests that the migration of globals to STA_VM introduced no regressions.
 */
#include <sta/vm.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static STA_VMConfig default_config(void) {
    STA_VMConfig c = {
        .scheduler_threads = 1,
        .initial_heap_bytes = 4 * 1024 * 1024,
        .image_path = NULL
    };
    return c;
}

static void assert_eval(STA_VM *vm, const char *expr, const char *expected) {
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

/* ── Test 1: VM create → destroy → create cycle ─────────────────────── */

static void test_create_destroy_create(void) {
    STA_VMConfig cfg = default_config();

    /* First VM: eval 3 + 4 = 7 */
    STA_VM *vm1 = sta_vm_create(&cfg);
    assert(vm1 != NULL);
    assert_eval(vm1, "3 + 4", "7");
    sta_vm_destroy(vm1);

    /* Second VM: same eval must work on fresh state */
    STA_VM *vm2 = sta_vm_create(&cfg);
    assert(vm2 != NULL);
    assert_eval(vm2, "3 + 4", "7");
    sta_vm_destroy(vm2);
}

/* ── Test 2: Handle table growth through public API ──────────────────── */

static void test_handle_table_growth(void) {
    STA_VMConfig cfg = default_config();
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);

    /* 200 evals without releasing — forces multiple slab allocations. */
    STA_Handle *handles[200];
    for (int i = 0; i < 200; i++) {
        handles[i] = sta_eval(vm, "1 + 1");
        assert(handles[i] != NULL);
    }

    /* All 200 handles are still valid and inspectable. */
    for (int i = 0; i < 200; i++) {
        const char *s = sta_inspect_cstring(vm, handles[i]);
        assert(strcmp(s, "2") == 0);
    }

    /* Release all. */
    for (int i = 0; i < 200; i++) {
        sta_handle_release(vm, handles[i]);
    }

    sta_vm_destroy(vm);
}

/* ── Test 3: Handle slot reuse through public API ────────────────────── */

static void test_handle_slot_reuse(void) {
    STA_VMConfig cfg = default_config();
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);

    for (int i = 0; i < 10; i++) {
        STA_Handle *h = sta_eval(vm, "5 * 5");
        assert(h != NULL);
        const char *s = sta_inspect_cstring(vm, h);
        assert(strcmp(s, "25") == 0);
        sta_handle_release(vm, h);
    }

    sta_vm_destroy(vm);
}

/* ── Test 4: Nested on:do: with VM-based handler state ───────────────── */

static void test_nested_on_do_inner_catches(void) {
    STA_VMConfig cfg = default_config();
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);

    /* Inner handler catches Error; outer is not reached. */
    assert_eval(vm,
        "[[Error new signal] on: Error do: [:e | 99]] "
        "on: Exception do: [:e | 0 - 1]",
        "99");

    sta_vm_destroy(vm);
}

static void test_nested_on_do_outer_catches(void) {
    STA_VMConfig cfg = default_config();
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);

    /* Inner handler doesn't match (MNU); outer catches Exception. */
    assert_eval(vm,
        "[[Error new signal] on: MessageNotUnderstood do: [:e | 0 - 1]] "
        "on: Exception do: [:e | 55]",
        "55");

    sta_vm_destroy(vm);
}

/* ── Test 5: Exception after create/destroy/create ───────────────────── */

static void test_exception_after_recreate(void) {
    STA_VMConfig cfg = default_config();

    /* Create and destroy a first VM. */
    STA_VM *vm1 = sta_vm_create(&cfg);
    assert(vm1 != NULL);
    assert_eval(vm1, "1 + 1", "2");
    sta_vm_destroy(vm1);

    /* Second VM: exception handling must work on fresh handler state. */
    STA_VM *vm2 = sta_vm_create(&cfg);
    assert(vm2 != NULL);
    assert_eval(vm2,
        "[Error new signal] on: Error do: [:e | 42]",
        "42");
    sta_vm_destroy(vm2);
}

/* ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_epic0_verify:\n");

    RUN(test_create_destroy_create);
    RUN(test_handle_table_growth);
    RUN(test_handle_slot_reuse);
    RUN(test_nested_on_do_inner_catches);
    RUN(test_nested_on_do_outer_catches);
    RUN(test_exception_after_recreate);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
