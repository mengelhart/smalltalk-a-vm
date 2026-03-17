/* tests/test_batch4_symbol.c
 * Phase 1.5 Batch 4, Stories 1+4: Symbol/String interning prims 91-93,
 * Symbol protocol (printString, asSymbol, concatenation).
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

/* ── Story 1: Prim 91 — String>>asSymbol ──────────────────────────────── */

static void test_string_asSymbol(void) {
    assert_eval("'hello' asSymbol", "#hello");
}

static void test_asSymbol_identity(void) {
    assert_eval("'hello' asSymbol == 'hello' asSymbol", "true");
}

static void test_asSymbol_class(void) {
    /* Verify class identity by checking == against the Symbol class. */
    assert_eval("'hello' asSymbol class == Symbol", "true");
}

/* ── Story 1: Prim 92 — Symbol>>asString ──────────────────────────────── */

static void test_symbol_asString(void) {
    assert_eval("#hello asString", "'hello'");
}

static void test_asString_not_identity(void) {
    assert_eval("#hello asString == #hello asString", "false");
}

static void test_asString_class(void) {
    assert_eval("#hello asString class == String", "true");
}

static void test_asString_value_equal(void) {
    assert_eval("'hello' asSymbol asString = 'hello'", "true");
}

/* ── Story 1: Prim 93 — Symbol class>>intern: ─────────────────────────── */

static void test_symbol_intern(void) {
    assert_eval("(Symbol intern: 'world') == #world", "true");
}

/* ── Story 4: Symbol protocol ─────────────────────────────────────────── */

static void test_symbol_printString(void) {
    assert_eval("#hello printString", "'#hello'");
}

static void test_symbol_asSymbol(void) {
    assert_eval("#hello asSymbol == #hello", "true");
}

static void test_symbol_concat(void) {
    assert_eval("#hello , ' world'", "'hello world'");
}

static void test_symbol_concat_class(void) {
    assert_eval("(#hello , ' world') class == String", "true");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);
    printf("test_batch4_symbol (Phase 1.5 Batch 4, Stories 1+4):\n");

    /* Story 1: Interning primitives */
    RUN(test_string_asSymbol);
    RUN(test_asSymbol_identity);
    RUN(test_asSymbol_class);
    RUN(test_symbol_asString);
    RUN(test_asString_not_identity);
    RUN(test_asString_class);
    RUN(test_asString_value_equal);
    RUN(test_symbol_intern);

    /* Story 4: Symbol protocol */
    RUN(test_symbol_printString);
    RUN(test_symbol_asSymbol);
    RUN(test_symbol_concat);
    RUN(test_symbol_concat_class);

    sta_vm_destroy(vm);
    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
