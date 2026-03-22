/* tests/test_smallint_correctness.c
 * Regression tests for SmallInteger correctness fixes (#243, #244, #339).
 *
 * 6a: SmallInteger = with non-SmallInt arg should return false (#243)
 * 6b: Catching DNU via on:do: should not trigger BlockCannotReturn (#244)
 * 6c: SmallInteger >> / should signal ZeroDivide on zero divisor (#339)
 */
#include "compiler/compiler.h"
#include "vm/interpreter.h"
#include "vm/compiled_method.h"
#include "vm/immutable_space.h"
#include "vm/heap.h"
#include "vm/symbol_table.h"
#include "vm/class_table.h"
#include "vm/special_objects.h"
#include "vm/method_dict.h"
#include "vm/oop.h"
#include "vm/vm_state.h"
#include "vm/primitive_table.h"
#include "bootstrap/bootstrap.h"
#include "actor/actor.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sta/vm.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static STA_VM *g_vm;

static void setup(void) {
    STA_VMConfig cfg = {0};
    g_vm = sta_vm_create(&cfg);
    assert(g_vm != NULL);
}

static void teardown(void) {
    sta_vm_destroy(g_vm);
    g_vm = NULL;
}

static STA_OOP eval(const char *source) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);

    STA_CompileResult cr = sta_compile_expression(
        source, &g_vm->symbol_table, &g_vm->immutable_space,
        &g_vm->root_actor->heap, sysdict);
    if (cr.had_error) {
        fprintf(stderr, "COMPILE ERROR: %s\n  source: %s\n",
                cr.error_msg, source);
    }
    assert(!cr.had_error);

    STA_OOP sel = sta_symbol_intern(&g_vm->immutable_space,
                                     &g_vm->symbol_table, "doIt", 4);
    STA_OOP md = sta_class_method_dict(obj_cls);
    (void)sta_method_dict_insert(&g_vm->root_actor->heap, md, sel, cr.method);

    STA_ObjHeader *recv_h = sta_heap_alloc(&g_vm->root_actor->heap,
                                             STA_CLS_OBJECT, 0);
    assert(recv_h);
    return sta_interpret(g_vm, cr.method, (STA_OOP)(uintptr_t)recv_h, NULL, 0);
}

/* ── 6a: SmallInteger = with non-SmallInt arg ─────────────────────────── */

static void test_eq_with_string_returns_false(void) {
    STA_OOP result = eval("3 = 'hello'");
    assert(result == sta_spc_get(SPC_FALSE));
}

static void test_eq_with_nil_returns_false(void) {
    STA_OOP result = eval("42 = nil");
    assert(result == sta_spc_get(SPC_FALSE));
}

static void test_ne_with_string_returns_true(void) {
    STA_OOP result = eval("3 ~= 'hello'");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_eq_with_same_value(void) {
    STA_OOP result = eval("5 = 5");
    assert(result == sta_spc_get(SPC_TRUE));
}

/* ── 6b: DNU caught via on:do: ────────────────────────────────────────── */

static void test_dnu_on_do_catches_cleanly(void) {
    STA_OOP result = eval(
        "[3 nonExistentMessage] "
        "  on: MessageNotUnderstood "
        "  do: [:e | e]");
    /* Should catch the MNU exception, not trigger BlockCannotReturn. */
    assert(result != 0);
    assert(STA_IS_HEAP(result));
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_MESSAGENOTUNDERSTOOD);
}

static void test_dnu_on_do_returns_handler_value(void) {
    STA_OOP result = eval(
        "[3 nonExistentMessage] "
        "  on: MessageNotUnderstood "
        "  do: [:e | 99]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 99);
}

/* ── 6c: SmallInteger / ZeroDivide ────────────────────────────────────── */

static void test_zero_divide_caught(void) {
    STA_OOP result = eval(
        "[1 / 0] on: ZeroDivide do: [:e | e]");
    assert(result != 0);
    assert(STA_IS_HEAP(result));
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)result;
    assert(h->class_index == STA_CLS_ZERODIVIDE);
}

static void test_exact_division_works(void) {
    STA_OOP result = eval("10 / 2");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 5);
}

static void test_negative_division_works(void) {
    STA_OOP result = eval("-12 / 3");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == -4);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_smallint_correctness:\n");
    setup();

    /* 6a: SmallInteger = with non-SmallInt arg */
    RUN(test_eq_with_string_returns_false);
    RUN(test_eq_with_nil_returns_false);
    RUN(test_ne_with_string_returns_true);
    RUN(test_eq_with_same_value);

    /* 6b: DNU caught via on:do: */
    RUN(test_dnu_on_do_catches_cleanly);
    RUN(test_dnu_on_do_returns_handler_value);

    /* 6c: ZeroDivide */
    RUN(test_zero_divide_caught);
    RUN(test_exact_division_works);
    RUN(test_negative_division_works);

    teardown();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
