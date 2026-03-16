/* tests/test_batch3_collections.c
 * Phase 1.5 Batch 3, Stories 6+8: Collection, Array, OrderedCollection tests.
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

/* ── inject:into: ────────────────────────────────────────────────────── */

static void test_inject_sum(void) {
    assert_eval(
        "| a | a := Array new: 5. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "a at: 4 put: 4. a at: 5 put: 5. "
        "a inject: 0 into: [:sum :each | sum + each]", "15");
}

/* ── anySatisfy: / allSatisfy: ───────────────────────────────────────── */

static void test_anySatisfy_true(void) {
    assert_eval(
        "| a | a := Array new: 5. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "a at: 4 put: 4. a at: 5 put: 5. "
        "a anySatisfy: [:e | e > 3]", "true");
}

static void test_allSatisfy_true(void) {
    assert_eval(
        "| a | a := Array new: 5. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "a at: 4 put: 4. a at: 5 put: 5. "
        "a allSatisfy: [:e | e > 0]", "true");
}

static void test_allSatisfy_false(void) {
    assert_eval(
        "| a | a := Array new: 5. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "a at: 4 put: 4. a at: 5 put: 5. "
        "a allSatisfy: [:e | e > 3]", "false");
}

/* ── indexOf: ─────────────────────────────────────────────────────── */

static void test_indexOf_found(void) {
    assert_eval(
        "| a | a := Array new: 3. "
        "a at: 1 put: 10. a at: 2 put: 20. a at: 3 put: 30. "
        "a indexOf: 20", "2");
}

static void test_indexOf_not_found(void) {
    assert_eval(
        "| a | a := Array new: 3. "
        "a at: 1 put: 10. a at: 2 put: 20. a at: 3 put: 30. "
        "a indexOf: 99", "0");
}

/* ── copyWith: / copyWithout: ────────────────────────────────────────── */

static void test_copyWith(void) {
    assert_eval(
        "| a b | a := Array new: 3. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "b := a copyWith: 4. b at: 4", "4");
}

static void test_copyWith_size(void) {
    assert_eval(
        "| a b | a := Array new: 3. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "b := a copyWith: 4. b size", "4");
}

static void test_copyWithout(void) {
    assert_eval(
        "| a b | a := Array new: 5. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "a at: 4 put: 2. a at: 5 put: 1. "
        "b := a copyWithout: 2. b size", "3");
}

/* ── Array printString ───────────────────────────────────────────────── */

static void test_array_printString_empty(void) {
    assert_eval("(Array new: 0) printString", "'#()'");
}

static void test_array_printString_flat(void) {
    assert_eval(
        "| a | a := Array new: 3. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "a printString", "'#(1 2 3)'");
}

static void test_array_printString_nested(void) {
    assert_eval(
        "| inner outer | "
        "inner := Array new: 2. inner at: 1 put: 1. inner at: 2 put: 2. "
        "outer := Array new: 3. "
        "outer at: 1 put: inner. outer at: 2 put: 'hi'. outer at: 3 put: 42. "
        "outer printString", "'#(#(1 2) ''hi'' 42)'");
}

/* ── asArray ─────────────────────────────────────────────────────────── */

static void test_array_asArray(void) {
    assert_eval(
        "| a | a := Array new: 3. "
        "a at: 1 put: 1. a at: 2 put: 2. a at: 3 put: 3. "
        "a asArray == a", "true");
}

/* ── OrderedCollection ───────────────────────────────────────────────── */

static void test_oc_add_size(void) {
    assert_eval(
        "| oc | oc := OrderedCollection new. "
        "oc add: 10. oc add: 20. oc add: 30. oc size", "3");
}

static void test_oc_at(void) {
    assert_eval(
        "| oc | oc := OrderedCollection new. "
        "oc add: 10. oc add: 20. oc add: 30. oc at: 2", "20");
}

static void test_oc_removeFirst(void) {
    assert_eval(
        "| oc | oc := OrderedCollection new. "
        "oc add: 10. oc add: 20. oc add: 30. oc removeFirst", "10");
}

static void test_oc_after_removeFirst(void) {
    assert_eval(
        "| oc | oc := OrderedCollection new. "
        "oc add: 10. oc add: 20. oc add: 30. oc removeFirst. oc at: 1", "20");
}

static void test_oc_removeLast(void) {
    assert_eval(
        "| oc | oc := OrderedCollection new. "
        "oc add: 10. oc add: 20. oc add: 30. oc removeLast", "30");
}

static void test_oc_grow(void) {
    /* Default capacity 4, adding 10 elements forces at least one grow.
     * Use whileTrue: (inlined) to avoid clean-block limitation. */
    assert_eval(
        "| oc i | oc := OrderedCollection new. i := 1. "
        "[i <= 10] whileTrue: [oc add: i. i := i + 1]. "
        "oc size", "10");
}

static void test_oc_grow_at_first(void) {
    assert_eval(
        "| oc i | oc := OrderedCollection new. i := 1. "
        "[i <= 10] whileTrue: [oc add: i. i := i + 1]. "
        "oc at: 10", "10");
}

static void test_oc_addFirst(void) {
    assert_eval(
        "| oc | oc := OrderedCollection new. "
        "oc add: 1. oc add: 2. oc add: 3. "
        "oc addFirst: 99. oc at: 1", "99");
}

static void test_oc_addFirst_size(void) {
    assert_eval(
        "| oc | oc := OrderedCollection new. "
        "oc add: 1. oc add: 2. oc add: 3. "
        "oc addFirst: 99. oc size", "4");
}

static void test_oc_printString(void) {
    assert_eval(
        "| oc | oc := OrderedCollection new. "
        "oc add: 10. oc add: 20. oc add: 30. "
        "oc printString", "'an OrderedCollection(10 20 30)'");
}

static void test_oc_collect(void) {
    /* collect: inherited from SequenceableCollection, uses OC's at: */
    assert_eval(
        "| oc result | oc := OrderedCollection new. "
        "oc add: 1. oc add: 2. oc add: 3. "
        "result := oc collect: [:e | e * 2]. "
        "result at: 2", "4");
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_batch3_collections (Phase 1.5 Batch 3):\n");

    STA_VMConfig config = {0};
    vm = sta_vm_create(&config);
    assert(vm != NULL);

    /* inject:into: */
    RUN(test_inject_sum);

    /* anySatisfy: / allSatisfy: */
    RUN(test_anySatisfy_true);
    RUN(test_allSatisfy_true);
    RUN(test_allSatisfy_false);

    /* indexOf: */
    RUN(test_indexOf_found);
    RUN(test_indexOf_not_found);

    /* copyWith: / copyWithout: */
    RUN(test_copyWith);
    RUN(test_copyWith_size);
    RUN(test_copyWithout);

    /* Array printString */
    RUN(test_array_printString_empty);
    RUN(test_array_printString_flat);
    RUN(test_array_printString_nested);

    /* asArray */
    RUN(test_array_asArray);

    /* OrderedCollection */
    RUN(test_oc_add_size);
    RUN(test_oc_at);
    RUN(test_oc_removeFirst);
    RUN(test_oc_after_removeFirst);
    RUN(test_oc_removeLast);
    RUN(test_oc_grow);
    RUN(test_oc_grow_at_first);
    RUN(test_oc_addFirst);
    RUN(test_oc_addFirst_size);
    RUN(test_oc_printString);
    RUN(test_oc_collect);

    sta_vm_destroy(vm);

    printf("\n%d/%d batch 3 collection tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
