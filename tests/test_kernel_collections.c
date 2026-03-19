/* tests/test_kernel_collections.c
 * Test: Collection family, String kernel methods + whileTrue: verification.
 * Phase 1, Epic 9, Stories 8-12.
 *
 * Clean-block limitation: blocks passed as arguments (e.g. to do:, collect:)
 * can only reference self, their own args/temps, and literals. They CANNOT
 * capture mutable outer temps. This limits do: to side-effect-free blocks
 * but collect:, select:, reject:, detect:ifNone: all work because their
 * blocks only use the block arg. Phase 2 closures will lift this restriction.
 */
#include <sta/vm.h>
#include "vm/interpreter.h"
#include "vm/primitive_table.h"
#include "vm/method_dict.h"
#include "vm/symbol_table.h"
#include "vm/special_objects.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/frame.h"
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/kernel_load.h"
#include "compiler/compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_VM *g_vm;

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);

    sta_heap_init(&g_vm->heap, 4 * 1024 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 4 * 1024 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 512);
    sta_class_table_init(&g_vm->class_table);
    sta_stack_slab_init(&g_vm->slab, 64 * 1024);

    sta_special_objects_bind(g_vm->specials);
    sta_primitive_table_init();

    STA_BootstrapResult br = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space, &g_vm->symbol_table, &g_vm->class_table);
    assert(br.status == 0);

    int rc = sta_kernel_load_all(g_vm, KERNEL_DIR);
    if (rc != STA_OK) {
        fprintf(stderr, "kernel_load error: %s\n", sta_vm_last_error(g_vm));
    }
    assert(rc == STA_OK);
}

/* Helper: evaluate an expression and return the result OOP. */
static STA_OOP eval(const char *source) {
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);
    STA_CompileResult cr = sta_compile_expression(
        source, &g_vm->symbol_table, &g_vm->immutable_space, &g_vm->heap, sysdict);
    if (cr.had_error) {
        fprintf(stderr, "eval compile error: %s\n  source: %s\n",
                cr.error_msg, source);
        assert(!cr.had_error);
    }
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    return sta_interpret(g_vm, cr.method, nil_oop, NULL, 0);
}

/* ── Story 8: whileTrue: with mutable outer temp ─────────────────────── */

static void test_while_true_mutable_temp(void) {
    STA_OOP r = eval(
        "| sum i | "
        "sum := 0. "
        "i := 1. "
        "[i <= 5] whileTrue: [sum := sum + i. i := i + 1]. "
        "sum"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 15);
}

static void test_while_true_counter(void) {
    STA_OOP r = eval(
        "| i | "
        "i := 0. "
        "[i < 10] whileTrue: [i := i + 1]. "
        "i"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 10);
}

/* ── Story 9: Collection / SequenceableCollection / Array ────────────── */

static void test_array_collect_identity(void) {
    STA_OOP r = eval(
        "| arr result | "
        "arr := Array new: 3. "
        "arr at: 1 put: 10. "
        "arr at: 2 put: 20. "
        "arr at: 3 put: 30. "
        "result := arr collect: [:each | each]. "
        "result at: 2"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 20);
}

static void test_array_collect_double(void) {
    STA_OOP r = eval(
        "| arr result | "
        "arr := Array new: 3. "
        "arr at: 1 put: 1. "
        "arr at: 2 put: 2. "
        "arr at: 3 put: 3. "
        "result := arr collect: [:each | each * 2]. "
        "result at: 2"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 4);
}

static void test_array_collect_size(void) {
    STA_OOP r = eval(
        "| arr | "
        "arr := Array new: 5. "
        "arr at: 1 put: 0. arr at: 2 put: 0. arr at: 3 put: 0. "
        "arr at: 4 put: 0. arr at: 5 put: 0. "
        "(arr collect: [:each | each + 1]) size"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 5);
}

static void test_array_select(void) {
    STA_OOP r = eval(
        "| arr result | "
        "arr := Array new: 4. "
        "arr at: 1 put: 1. "
        "arr at: 2 put: 3. "
        "arr at: 3 put: 2. "
        "arr at: 4 put: 5. "
        "result := arr select: [:each | each > 2]. "
        "result size"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 2);
}

static void test_array_select_values(void) {
    STA_OOP r = eval(
        "| arr result | "
        "arr := Array new: 4. "
        "arr at: 1 put: 1. "
        "arr at: 2 put: 3. "
        "arr at: 3 put: 2. "
        "arr at: 4 put: 5. "
        "result := arr select: [:each | each > 2]. "
        "result at: 1"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 3);
}

static void test_array_reject(void) {
    STA_OOP r = eval(
        "| arr result | "
        "arr := Array new: 4. "
        "arr at: 1 put: 1. "
        "arr at: 2 put: 3. "
        "arr at: 3 put: 2. "
        "arr at: 4 put: 5. "
        "result := arr reject: [:each | each > 2]. "
        "result size"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 2);
}

static void test_array_detect_found(void) {
    STA_OOP r = eval(
        "| arr | "
        "arr := Array new: 3. "
        "arr at: 1 put: 1. "
        "arr at: 2 put: 5. "
        "arr at: 3 put: 3. "
        "arr detect: [:each | each > 4] ifNone: [0]"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 5);
}

static void test_array_detect_not_found(void) {
    STA_OOP r = eval(
        "| arr | "
        "arr := Array new: 3. "
        "arr at: 1 put: 1. "
        "arr at: 2 put: 2. "
        "arr at: 3 put: 3. "
        "arr detect: [:each | each > 10] ifNone: [99]"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 99);
}

static void test_array_is_empty(void) {
    STA_OOP r = eval("(Array new: 0) isEmpty");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_array_not_empty(void) {
    STA_OOP r = eval("(Array new: 3) notEmpty");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_array_includes_true(void) {
    STA_OOP r = eval(
        "| arr | "
        "arr := Array new: 3. "
        "arr at: 1 put: 10. "
        "arr at: 2 put: 20. "
        "arr at: 3 put: 30. "
        "arr includes: 20"
    );
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_array_includes_false(void) {
    STA_OOP r = eval(
        "| arr | "
        "arr := Array new: 3. "
        "arr at: 1 put: 10. "
        "arr at: 2 put: 20. "
        "arr at: 3 put: 30. "
        "arr includes: 99"
    );
    assert(r == sta_spc_get(SPC_FALSE));
}

static void test_array_first(void) {
    STA_OOP r = eval(
        "| arr | "
        "arr := Array new: 3. "
        "arr at: 1 put: 42. "
        "arr at: 2 put: 0. "
        "arr at: 3 put: 0. "
        "arr first"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 42);
}

static void test_array_last(void) {
    STA_OOP r = eval(
        "| arr | "
        "arr := Array new: 3. "
        "arr at: 1 put: 0. "
        "arr at: 2 put: 0. "
        "arr at: 3 put: 77. "
        "arr last"
    );
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 77);
}

/* ── Story 10: String ────────────────────────────────────────────────── */

static void test_string_size(void) {
    STA_OOP r = eval("'hello' size");
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 5);
}

static void test_string_size_empty(void) {
    STA_OOP r = eval("'' size");
    assert(STA_IS_SMALLINT(r));
    assert(STA_SMALLINT_VAL(r) == 0);
}

static void test_string_is_empty(void) {
    STA_OOP r = eval("'' isEmpty");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_string_not_empty(void) {
    STA_OOP r = eval("'abc' notEmpty");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_string_printstring(void) {
    STA_OOP r = eval("'hello' printString");
    assert(r != sta_spc_get(SPC_NIL));
    assert(!STA_IS_SMALLINT(r));
}

/* ── Story 12: Integration — full kernel smoke tests ─────────────────── */

static void test_integration_max(void) {
    STA_OOP r = eval("5 max: 3");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 5);
}

static void test_integration_abs(void) {
    STA_OOP r = eval("-3 abs");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 3);
}

static void test_integration_factorial(void) {
    STA_OOP r = eval("5 factorial");
    assert(STA_IS_SMALLINT(r) && STA_SMALLINT_VAL(r) == 120);
}

static void test_integration_isnil(void) {
    STA_OOP r = eval("nil isNil");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_integration_true_not(void) {
    STA_OOP r = eval("true not");
    assert(r == sta_spc_get(SPC_FALSE));
}

static void test_integration_equality(void) {
    STA_OOP r = eval("3 = 3");
    assert(r == sta_spc_get(SPC_TRUE));
}

static void test_integration_inequality(void) {
    STA_OOP r = eval("3 ~= 4");
    assert(r == sta_spc_get(SPC_TRUE));
}

/* ── main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_kernel_collections:\n");
    setup();

    printf("\n  -- Story 8: whileTrue: inlining --\n");
    RUN(test_while_true_mutable_temp);
    RUN(test_while_true_counter);

    printf("\n  -- Story 9: Collection / Array --\n");
    RUN(test_array_collect_identity);
    RUN(test_array_collect_double);
    RUN(test_array_collect_size);
    RUN(test_array_select);
    RUN(test_array_select_values);
    RUN(test_array_reject);
    RUN(test_array_detect_found);
    RUN(test_array_detect_not_found);
    RUN(test_array_is_empty);
    RUN(test_array_not_empty);
    RUN(test_array_includes_true);
    RUN(test_array_includes_false);
    RUN(test_array_first);
    RUN(test_array_last);

    printf("\n  -- Story 10: String --\n");
    RUN(test_string_size);
    RUN(test_string_size_empty);
    RUN(test_string_is_empty);
    RUN(test_string_not_empty);
    RUN(test_string_printstring);

    printf("\n  -- Story 12: Integration --\n");
    RUN(test_integration_max);
    RUN(test_integration_abs);
    RUN(test_integration_factorial);
    RUN(test_integration_isnil);
    RUN(test_integration_true_not);
    RUN(test_integration_equality);
    RUN(test_integration_inequality);

    printf("\n  %d/%d tests passed.\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
