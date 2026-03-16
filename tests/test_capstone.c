/* tests/test_capstone.c
 * Phase 1 capstone smoke test — all tests go through the public API.
 * Uses only sta_vm_create, sta_eval, sta_inspect_cstring, sta_vm_destroy,
 * sta_vm_save_image, sta_handle_release, sta_vm_last_error.
 *
 * Phase 1, Epic 11, Story 10.
 */
#include <sta/vm.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static STA_VM *vm;

/* Helper: eval + inspect, returning the inspect string.
 * Asserts that eval succeeded. */
static const char *eval_str(const char *expr) {
    STA_Handle *h = sta_eval(vm, expr);
    if (!h) {
        fprintf(stderr, "eval failed: %s\n  expr: %s\n",
                sta_vm_last_error(vm), expr);
    }
    assert(h != NULL);
    const char *s = sta_inspect_cstring(vm, h);
    sta_handle_release(vm, h);
    return s;
}

/* Helper: eval + inspect, assert the result equals expected string. */
static void assert_eval(const char *expr, const char *expected) {
    const char *got = eval_str(expr);
    if (strcmp(got, expected) != 0) {
        fprintf(stderr, "\n  FAIL: %s\n    expected: %s\n    got:      %s\n",
                expr, expected, got);
    }
    assert(strcmp(got, expected) == 0);
}

/* ── Arithmetic ────────────────────────────────────────────────────── */

static void test_add(void)  { assert_eval("3 + 4", "7"); }
static void test_sub(void)  { assert_eval("100 - 58", "42"); }
static void test_mul(void)  { assert_eval("6 * 7", "42"); }
static void test_expr(void) { assert_eval("(4 * 5) + 2", "22"); }

static void test_factorial(void) {
    assert_eval("10 factorial", "3628800");
}

/* ── Boolean ───────────────────────────────────────────────────────── */

static void test_if_true(void) {
    assert_eval("true ifTrue: [42] ifFalse: [0]", "42");
}

static void test_if_false(void) {
    assert_eval("false ifTrue: [42] ifFalse: [0]", "0");
}

static void test_gt_true(void)  { assert_eval("3 > 2", "true"); }
static void test_gt_false(void) { assert_eval("1 > 2", "false"); }
static void test_lt(void)       { assert_eval("2 < 5", "true"); }

/* ── Blocks ────────────────────────────────────────────────────────── */

static void test_block_value(void) {
    assert_eval("[42] value", "42");
}

static void test_block_value_arg(void) {
    assert_eval("[:x | x * 2] value: 21", "42");
}

/* ── Collections ───────────────────────────────────────────────────── */

static void test_array_size(void) {
    assert_eval("#(10 20 30) size", "3");
}

static void test_array_at(void) {
    assert_eval("#(10 20 30) at: 2", "20");
}

/* ── Identity and class ────────────────────────────────────────────── */

static void test_nil_isNil(void)    { assert_eval("nil isNil", "true"); }
static void test_nil_inspect(void)  { assert_eval("nil", "nil"); }
static void test_true_inspect(void) { assert_eval("true", "true"); }

/* ── Exceptions ────────────────────────────────────────────────────── */

static void test_dnu_caught(void) {
    assert_eval(
        "[42 frobnicate] on: MessageNotUnderstood do: [:e | 99]", "99");
}

/* ── Error cases ───────────────────────────────────────────────────── */

static void test_compile_error(void) {
    STA_Handle *h = sta_eval(vm, "3 +");
    assert(h == NULL);
    const char *err = sta_vm_last_error(vm);
    assert(err != NULL);
    assert(strlen(err) > 0);
}

/* ── Image round-trip through public API ───────────────────────────── */

static void test_image_roundtrip(void) {
    const char *img = "/tmp/sta_test_capstone.stai";

    /* Save image. */
    int rc = sta_vm_save_image(vm, img);
    assert(rc == STA_OK);

    /* Destroy current VM. */
    sta_vm_destroy(vm);
    vm = NULL;

    /* Create new VM from saved image. */
    STA_VMConfig cfg2 = { .image_path = img };
    vm = sta_vm_create(&cfg2);
    assert(vm != NULL);

    /* Verify interpreter works after image load. */
    assert_eval("3 + 4", "7");
    assert_eval("true ifTrue: [42] ifFalse: [0]", "42");

    unlink(img);
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_capstone (Phase 1):\n");

    /* Create VM. */
    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);

    /* Arithmetic */
    RUN(test_add);
    RUN(test_sub);
    RUN(test_mul);
    RUN(test_expr);
    RUN(test_factorial);

    /* Boolean */
    RUN(test_if_true);
    RUN(test_if_false);
    RUN(test_gt_true);
    RUN(test_gt_false);
    RUN(test_lt);

    /* Blocks */
    RUN(test_block_value);
    RUN(test_block_value_arg);

    /* Collections */
    RUN(test_array_size);
    RUN(test_array_at);

    /* Identity and class */
    RUN(test_nil_isNil);
    RUN(test_nil_inspect);
    RUN(test_true_inspect);

    /* Exceptions */
    RUN(test_dnu_caught);

    /* Error cases */
    RUN(test_compile_error);

    /* Image round-trip (destroys and recreates VM) */
    RUN(test_image_roundtrip);

    /* Cleanup. */
    sta_vm_destroy(vm);

    printf("\n%d/%d capstone tests passed.\n", tests_passed, tests_run);
    printf("Phase 1 — Minimal Live Kernel: COMPLETE\n");
    return tests_passed == tests_run ? 0 : 1;
}
