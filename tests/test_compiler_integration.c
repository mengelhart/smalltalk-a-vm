/* tests/test_compiler_integration.c
 * End-to-end integration: compile source → install → execute → verify.
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
#include "vm/oop.h"
#include "bootstrap/bootstrap.h"
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

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_Heap *heap;
static STA_ImmutableSpace *imm;
static STA_SymbolTable *syms;
static STA_ClassTable *ct;
static STA_StackSlab *slab;

static void setup(void) {
    heap = sta_heap_create(4 * 1024 * 1024);
    imm  = sta_immutable_space_create(4 * 1024 * 1024);
    syms = sta_symbol_table_create(256);
    ct   = sta_class_table_create();
    slab = sta_stack_slab_create(64 * 1024);

    STA_BootstrapResult br = sta_bootstrap(heap, imm, syms, ct);
    assert(br.status == 0);
}

/* Compile a method, install it on Object, send the message, return result. */
static STA_OOP compile_install_run(const char *source, const char *selector_str,
                                    STA_OOP receiver, STA_OOP *args,
                                    uint8_t nargs) {
    STA_OOP obj_cls = sta_class_table_get(ct, STA_CLS_OBJECT);

    STA_CompileResult cr = sta_compile_method(source, obj_cls,
        NULL, 0, syms, imm, heap, 0);
    if (cr.had_error) {
        fprintf(stderr, "COMPILE ERROR: %s\n  source: %s\n",
                cr.error_msg, source);
    }
    assert(!cr.had_error);

    /* Install in Object's method dictionary. */
    STA_OOP sel = sta_symbol_intern(imm, syms, selector_str,
                                     strlen(selector_str));
    assert(sel != 0);
    STA_OOP md = sta_class_method_dict(obj_cls);
    int rc = sta_method_dict_insert(heap, md, sel, cr.method);
    assert(rc == 0);

    /* Execute via interpreter. */
    STA_OOP result = sta_interpret(slab, heap, ct, cr.method,
                                    receiver, args, nargs);
    return result;
}

/* Helper: create a simple Object instance. */
static STA_OOP make_object(void) {
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_OBJECT, 0);
    assert(h != NULL);
    return (STA_OOP)(uintptr_t)h;
}

/* ── Test cases ──────────────────────────────────────────────────────── */

/* (a) answer ^3 + 4 → expect SmallInt 7 */
static void test_three_plus_four(void) {
    STA_OOP receiver = make_object();
    STA_OOP result = compile_install_run("answer ^3 + 4", "answer",
                                          receiver, NULL, 0);
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 7);
}

/* (c) answer | x | x := 10. ^x * x → expect SmallInt 100 */
static void test_temp_multiply(void) {
    STA_OOP receiver = make_object();
    STA_OOP result = compile_install_run(
        "answer | x | x := 10. ^x * x", "answer",
        receiver, NULL, 0);
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 100);
}

/* (f) answer ^4 > 3 ifTrue: [42] ifFalse: [0] → expect SmallInt 42 */
static void test_if_true_if_false(void) {
    STA_OOP receiver = make_object();
    STA_OOP result = compile_install_run(
        "answer ^4 > 3 ifTrue: [42] ifFalse: [0]", "answer",
        receiver, NULL, 0);
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 42);
}

/* (f variant) answer ^2 > 3 ifTrue: [42] ifFalse: [0] → expect SmallInt 0 */
static void test_if_false_branch(void) {
    STA_OOP receiver = make_object();
    STA_OOP result = compile_install_run(
        "answer2 ^2 > 3 ifTrue: [42] ifFalse: [0]", "answer2",
        receiver, NULL, 0);
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 0);
}

/* (g) whileTrue loop: sum 1..10 → expect SmallInt 55 */
static void test_while_true_sum(void) {
    STA_OOP receiver = make_object();
    STA_OOP result = compile_install_run(
        "answer | sum i | sum := 0. i := 1. "
        "[i < 11] whileTrue: [sum := sum + i. i := i + 1]. ^sum",
        "answer", receiver, NULL, 0);
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 55);
}

/* (h) answer ^self yourself → expect the receiver back */
static void test_yourself(void) {
    STA_OOP receiver = make_object();
    STA_OOP result = compile_install_run(
        "answerSelf ^self yourself", "answerSelf",
        receiver, NULL, 0);
    assert(result == receiver);
}

/* Keyword method with arg: answer: a ^a + 1 */
static void test_keyword_arg(void) {
    STA_OOP receiver = make_object();
    STA_OOP obj_cls = sta_class_table_get(ct, STA_CLS_OBJECT);

    STA_CompileResult cr = sta_compile_method("answer: a ^a + 1", obj_cls,
        NULL, 0, syms, imm, heap, 0);
    assert(!cr.had_error);

    STA_OOP sel = sta_symbol_intern(imm, syms, "answer:", 7);
    STA_OOP md = sta_class_method_dict(obj_cls);
    sta_method_dict_insert(heap, md, sel, cr.method);

    STA_OOP args[1] = { STA_SMALLINT_OOP(9) };
    STA_OOP result = sta_interpret(slab, heap, ct, cr.method,
                                    receiver, args, 1);
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 10);
}

/* Implicit return self from empty body. */
static void test_implicit_self(void) {
    STA_OOP receiver = make_object();
    STA_OOP result = compile_install_run("noop", "noop", receiver, NULL, 0);
    assert(result == receiver);
}

/* Boolean inlining: ifTrue: with false condition → nil */
static void test_if_true_nil(void) {
    STA_OOP receiver = make_object();
    STA_OOP result = compile_install_run(
        "answerNil ^false ifTrue: [42]", "answerNil",
        receiver, NULL, 0);
    assert(result == sta_spc_get(SPC_NIL));
}

/* Cascades executed through interpreter. */
static void test_cascade_yourself(void) {
    STA_OOP receiver = make_object();
    STA_OOP result = compile_install_run(
        "cascade ^self yourself; yourself", "cascade",
        receiver, NULL, 0);
    assert(result == receiver);
}

/* ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    setup();
    printf("test_compiler_integration:\n");

    RUN(test_three_plus_four);
    RUN(test_temp_multiply);
    RUN(test_if_true_if_false);
    RUN(test_if_false_branch);
    RUN(test_while_true_sum);
    RUN(test_yourself);
    RUN(test_keyword_arg);
    RUN(test_implicit_self);
    RUN(test_if_true_nil);
    RUN(test_cascade_yourself);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
