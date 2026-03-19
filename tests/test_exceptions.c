/* tests/test_exceptions.c
 * Integration tests for Epic 8: Exception Handling.
 * Tests on:do:, signal, ensure:, doesNotUnderstand:, and combinations.
 *
 * Phase 1 limitation: blocks evaluated from C primitives (on:do:, ensure:)
 * create new frames and do NOT share outer temp variables. Tests are
 * structured to avoid outer variable capture in blocks.
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
#include "vm/frame.h"
#include "vm/handler.h"
#include "vm/oop.h"
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_VM *g_vm;

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);

    sta_heap_init(&g_vm->heap, 4 * 1024 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 4 * 1024 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 256);
    sta_class_table_init(&g_vm->class_table);
    sta_stack_slab_init(&g_vm->slab, 64 * 1024);

    sta_special_objects_bind(g_vm->specials);
    sta_primitive_table_init();

    STA_BootstrapResult br = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space, &g_vm->symbol_table, &g_vm->class_table);
    assert(br.status == 0);

    /* Reset handler chain to clean state. */
    sta_handler_set_top(NULL);
}

/* Compile an expression, install as Object>>doIt, execute, return result. */
static STA_OOP eval(const char *source) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);

    STA_CompileResult cr = sta_compile_expression(
        source, &g_vm->symbol_table, &g_vm->immutable_space, &g_vm->heap, sysdict);
    if (cr.had_error) {
        fprintf(stderr, "COMPILE ERROR: %s\n  source: %s\n",
                cr.error_msg, source);
    }
    assert(!cr.had_error);

    /* Install as doIt on Object. */
    STA_OOP sel = sta_symbol_intern(&g_vm->immutable_space, &g_vm->symbol_table, "doIt", 4);
    assert(sel != 0);
    STA_OOP md = sta_class_method_dict(obj_cls);
    (void)sta_method_dict_insert(&g_vm->heap, md, sel, cr.method);

    STA_ObjHeader *recv_h = sta_heap_alloc(&g_vm->heap, STA_CLS_OBJECT, 0);
    assert(recv_h);
    STA_OOP receiver = (STA_OOP)(uintptr_t)recv_h;

    return sta_interpret(g_vm, cr.method, receiver, NULL, 0);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Story 2: on:do: — no exception (body completes normally)              */
/* ═══════════════════════════════════════════════════════════════════════ */

static void test_on_do_normal_completion(void) {
    /* [42] on: Error do: [:e | -1]  →  42 */
    STA_OOP result = eval("[42] on: Error do: [:e | 0 - 1]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 42);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Story 3: signal caught by on:do:                                      */
/* ═══════════════════════════════════════════════════════════════════════ */

static void test_signal_caught(void) {
    /* [Error new signal] on: Error do: [:e | 99]  →  99 */
    STA_OOP result = eval("[Error new signal] on: Error do: [:e | 99]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 99);
}

static void test_signal_superclass_match(void) {
    /* Error new signal caught by on: Exception do: (superclass match) */
    STA_OOP result = eval(
        "[Error new signal] on: Exception do: [:e | 77]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 77);
}

static void test_signal_with_message_text(void) {
    STA_OOP result = eval(
        "[(Exception new messageText: 'boom') signal] "
        "  on: Exception do: [:e | e messageText]");
    assert(result != sta_spc_get(SPC_NIL));
    assert(STA_IS_HEAP(result));
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Story 4: Exception class hierarchy                                    */
/* ═══════════════════════════════════════════════════════════════════════ */

static void test_exception_accessors(void) {
    STA_OOP result = eval(
        "(Exception new messageText: 'test') messageText");
    assert(result != sta_spc_get(SPC_NIL));
    assert(STA_IS_HEAP(result));
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Story 5: doesNotUnderstand: — real Smalltalk method                   */
/* ═══════════════════════════════════════════════════════════════════════ */

static void test_dnu_creates_mnu(void) {
    STA_OOP result = eval(
        "[3 unknownMessage] "
        "  on: MessageNotUnderstood "
        "  do: [:e | e receiver]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 3);
}

static void test_dnu_message_object(void) {
    STA_OOP result = eval(
        "[3 unknownMessage] "
        "  on: MessageNotUnderstood "
        "  do: [:e | e message]");
    assert(result != sta_spc_get(SPC_NIL));
    assert(STA_IS_HEAP(result));
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Story 6: ensure: (normal-completion only)                             */
/* ═══════════════════════════════════════════════════════════════════════ */

static void test_ensure_normal(void) {
    STA_OOP result = eval("[42] ensure: [99]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 42);
}

static void test_ensure_body_result_preserved(void) {
    STA_OOP r1 = eval("[7] ensure: [0]");
    assert(STA_IS_SMALLINT(r1));
    assert(STA_SMALLINT_VAL(r1) == 7);

    STA_OOP r2 = eval("[3 + 4] ensure: [100]");
    assert(STA_IS_SMALLINT(r2));
    assert(STA_SMALLINT_VAL(r2) == 7);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Story 7: Integration tests                                            */
/* ═══════════════════════════════════════════════════════════════════════ */

static void test_nested_handlers_inner_catches(void) {
    STA_OOP result = eval(
        "[[Error new signal] on: Error do: [:e | 11]] "
        "  on: Exception do: [:e | 22]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 11);
}

static void test_on_do_with_ensure_phase1(void) {
    STA_OOP result = eval(
        "[[Error new signal] ensure: [0]] "
        "  on: Error do: [:e | 55]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 55);
}

static void test_normal_ensure_with_on_do(void) {
    STA_OOP result = eval(
        "[[42] ensure: [99]] "
        "  on: Error do: [:e | 0 - 1]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 42);
}

/* ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    setup();
    printf("test_exceptions:\n");

    /* Story 2: on:do: normal completion */
    RUN(test_on_do_normal_completion);

    /* Story 3: signal and catching */
    RUN(test_signal_caught);
    RUN(test_signal_superclass_match);
    RUN(test_signal_with_message_text);

    /* Story 4: Exception accessors */
    RUN(test_exception_accessors);

    /* Story 5: doesNotUnderstand: */
    RUN(test_dnu_creates_mnu);
    RUN(test_dnu_message_object);

    /* Story 6: ensure: */
    RUN(test_ensure_normal);
    RUN(test_ensure_body_result_preserved);

    /* Story 7: Integration */
    RUN(test_nested_handlers_inner_catches);
    RUN(test_on_do_with_ensure_phase1);
    RUN(test_normal_ensure_with_on_do);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
