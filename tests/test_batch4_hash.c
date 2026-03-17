/* tests/test_batch4_hash.c
 * Phase 1.5 Batch 4, Story 2: Hash protocol.
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

/* ── SmallInteger hash ────────────────────────────────────────────────── */

static void test_smallint_hash(void) {
    assert_eval("42 hash", "42");
}

static void test_zero_hash(void) {
    assert_eval("0 hash", "0");
}

static void test_negative_hash(void) {
    /* SmallInteger hash is the value itself (via prim 36). */
    assert_eval("-1 hash", "-1");
}

/* ── String hash ──────────────────────────────────────────────────────── */

static void test_string_hash_consistent(void) {
    assert_eval("'hello' hash = 'hello' hash", "true");
}

static void test_string_hash_different(void) {
    assert_eval("'hello' hash = 'world' hash", "false");
}

static void test_empty_string_hash(void) {
    assert_eval("'' hash", "0");
}

/* ── Symbol hash = String hash ────────────────────────────────────────── */

static void test_symbol_string_hash_equal(void) {
    assert_eval("#hello hash = 'hello' hash", "true");
}

/* ── Character hash ───────────────────────────────────────────────────── */

static void test_char_hash(void) {
    assert_eval("$A hash", "65");
}

static void test_char_hash_space(void) {
    assert_eval("$  hash", "32");
}

/* ── Association hash ─────────────────────────────────────────────────── */

static void test_association_hash(void) {
    assert_eval(
        "| a | a := Association new. a key: #x. a hash = #x hash", "true");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);
    printf("test_batch4_hash (Phase 1.5 Batch 4, Story 2):\n");

    RUN(test_smallint_hash);
    RUN(test_zero_hash);
    RUN(test_negative_hash);
    RUN(test_string_hash_consistent);
    RUN(test_string_hash_different);
    RUN(test_empty_string_hash);
    RUN(test_symbol_string_hash_equal);
    RUN(test_char_hash);
    RUN(test_char_hash_space);
    RUN(test_association_hash);

    sta_vm_destroy(vm);
    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
