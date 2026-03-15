/* tests/test_codegen.c
 * Codegen unit tests — verify emitted bytecodes match §5 reference compilations.
 */
#include "compiler/codegen.h"
#include "compiler/parser.h"
#include "compiler/compiler.h"
#include "vm/interpreter.h"
#include "vm/compiled_method.h"
#include "vm/immutable_space.h"
#include "vm/heap.h"
#include "vm/symbol_table.h"
#include "vm/class_table.h"
#include "vm/special_objects.h"
#include "vm/oop.h"
#include "bootstrap/bootstrap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

/* ── Shared test infrastructure ──────────────────────────────────────── */

static STA_Heap *heap;
static STA_ImmutableSpace *imm;
static STA_SymbolTable *syms;
static STA_ClassTable *ct;

static void setup(void) {
    heap = sta_heap_create(1024 * 1024);
    imm  = sta_immutable_space_create(1024 * 1024);
    syms = sta_symbol_table_create(256);
    ct   = sta_class_table_create();

    STA_BootstrapResult br = sta_bootstrap(heap, imm, syms, ct);
    assert(br.status == 0);
}

static STA_OOP compile(const char *source, STA_OOP class_oop,
                        const char **ivar_names, uint32_t ivar_count) {
    if (class_oop == 0)
        class_oop = sta_class_table_get(ct, STA_CLS_OBJECT);

    STA_CompileResult r = sta_compile_method(source, class_oop,
        ivar_names, ivar_count, syms, imm, heap, 0);
    if (r.had_error) {
        fprintf(stderr, "COMPILE ERROR: %s\n  source: %s\n",
                r.error_msg, source);
    }
    assert(!r.had_error);
    assert(r.method != 0);
    return r.method;
}

static void assert_bytecodes(const char *source,
                              const char **ivar_names, uint32_t ivar_count,
                              const uint8_t *expected, uint32_t expected_len) {
    STA_OOP method = compile(source, 0, ivar_names, ivar_count);
    const uint8_t *bc = sta_method_bytecodes(method);
    uint32_t bc_len = sta_method_bytecode_count(method);

    if (bc_len < expected_len) {
        fprintf(stderr, "\nBytecode length mismatch: got %u, expected %u\n"
                "  source: %s\n", bc_len, expected_len, source);
        fprintf(stderr, "  got:      ");
        for (uint32_t i = 0; i < bc_len; i++)
            fprintf(stderr, "%02x ", bc[i]);
        fprintf(stderr, "\n  expected: ");
        for (uint32_t i = 0; i < expected_len; i++)
            fprintf(stderr, "%02x ", expected[i]);
        fprintf(stderr, "\n");
        assert(0);
    }

    for (uint32_t i = 0; i < expected_len; i++) {
        if (bc[i] != expected[i]) {
            fprintf(stderr, "\nBytecode mismatch at offset %u: got 0x%02x, "
                    "expected 0x%02x\n  source: %s\n",
                    i, bc[i], expected[i], source);
            fprintf(stderr, "  got:      ");
            for (uint32_t j = 0; j < bc_len; j++)
                fprintf(stderr, "%02x ", bc[j]);
            fprintf(stderr, "\n  expected: ");
            for (uint32_t j = 0; j < expected_len; j++)
                fprintf(stderr, "%02x ", expected[j]);
            fprintf(stderr, "\n");
            assert(0);
        }
    }
}

/* ── §5.1 Literals ───────────────────────────────────────────────────── */

static void test_return_42(void) {
    uint8_t expected[] = {
        OP_PUSH_SMALLINT, 42, OP_RETURN_TOP, 0x00,
    };
    assert_bytecodes("answer ^42", NULL, 0, expected, sizeof(expected));
}

static void test_return_zero(void) {
    uint8_t expected[] = { OP_PUSH_ZERO, 0x00, OP_RETURN_TOP, 0x00 };
    assert_bytecodes("answer ^0", NULL, 0, expected, sizeof(expected));
}

static void test_return_one(void) {
    uint8_t expected[] = { OP_PUSH_ONE, 0x00, OP_RETURN_TOP, 0x00 };
    assert_bytecodes("answer ^1", NULL, 0, expected, sizeof(expected));
}

static void test_return_two(void) {
    uint8_t expected[] = { OP_PUSH_TWO, 0x00, OP_RETURN_TOP, 0x00 };
    assert_bytecodes("answer ^2", NULL, 0, expected, sizeof(expected));
}

static void test_return_minus_one(void) {
    uint8_t expected[] = { OP_PUSH_MINUS_ONE, 0x00, OP_RETURN_TOP, 0x00 };
    assert_bytecodes("answer ^-1", NULL, 0, expected, sizeof(expected));
}

static void test_return_nil(void) {
    uint8_t expected[] = { OP_RETURN_NIL, 0x00 };
    assert_bytecodes("answer ^nil", NULL, 0, expected, sizeof(expected));
}

static void test_return_true(void) {
    uint8_t expected[] = { OP_RETURN_TRUE, 0x00 };
    assert_bytecodes("answer ^true", NULL, 0, expected, sizeof(expected));
}

static void test_return_false(void) {
    uint8_t expected[] = { OP_RETURN_FALSE, 0x00 };
    assert_bytecodes("answer ^false", NULL, 0, expected, sizeof(expected));
}

static void test_return_self(void) {
    uint8_t expected[] = { OP_RETURN_SELF, 0x00 };
    assert_bytecodes("yourself", NULL, 0, expected, sizeof(expected));
}

static void test_return_self_explicit(void) {
    uint8_t expected[] = { OP_RETURN_SELF, 0x00 };
    assert_bytecodes("answer ^self", NULL, 0, expected, sizeof(expected));
}

static void test_return_large_smallint(void) {
    uint8_t expected[] = {
        OP_WIDE, 0x03, OP_PUSH_SMALLINT, 0xE8,
        OP_RETURN_TOP, 0x00,
    };
    assert_bytecodes("answer ^1000", NULL, 0, expected, sizeof(expected));
}

static void test_return_string(void) {
    STA_OOP method = compile("answer ^'hello'", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_LIT);
    assert(bc[2] == OP_RETURN_TOP);
}

static void test_return_symbol(void) {
    STA_OOP method = compile("answer ^#foo", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_LIT);
    assert(bc[2] == OP_RETURN_TOP);
}

/* ── §5.2 Variable access ───────────────────────────────────────────── */

static void test_push_temp(void) {
    uint8_t expected[] = { OP_PUSH_TEMP, 0, OP_RETURN_TOP, 0x00 };
    assert_bytecodes("foo: x ^x", NULL, 0, expected, sizeof(expected));
}

static void test_push_temp_second(void) {
    uint8_t expected[] = { OP_PUSH_TEMP, 1, OP_RETURN_TOP, 0x00 };
    assert_bytecodes("foo: a bar: b ^b", NULL, 0, expected, sizeof(expected));
}

static void test_push_instvar(void) {
    const char *ivars[] = { "x", "y" };
    uint8_t expected[] = { OP_PUSH_INSTVAR, 0, OP_RETURN_TOP, 0x00 };
    assert_bytecodes("answer ^x", ivars, 2, expected, sizeof(expected));
}

static void test_push_instvar_second(void) {
    const char *ivars[] = { "x", "y" };
    uint8_t expected[] = { OP_PUSH_INSTVAR, 1, OP_RETURN_TOP, 0x00 };
    assert_bytecodes("answer ^y", ivars, 2, expected, sizeof(expected));
}

/* ── §5.3 Assignment ─────────────────────────────────────────────────── */

static void test_assign_temp(void) {
    uint8_t expected[] = {
        OP_PUSH_SMALLINT, 42, OP_POP_STORE_TEMP, 0,
        OP_PUSH_TEMP, 0, OP_RETURN_TOP, 0x00,
    };
    assert_bytecodes("answer | x | x := 42. ^x", NULL, 0,
                      expected, sizeof(expected));
}

static void test_assign_instvar(void) {
    const char *ivars[] = { "x" };
    uint8_t expected[] = {
        OP_PUSH_SMALLINT, 10, OP_POP_STORE_INSTVAR, 0,
        OP_PUSH_INSTVAR, 0, OP_RETURN_TOP, 0x00,
    };
    assert_bytecodes("answer x := 10. ^x", ivars, 1,
                      expected, sizeof(expected));
}

/* ── §5.4 Message sends ──────────────────────────────────────────────── */

static void test_unary_send(void) {
    STA_OOP method = compile("answer ^self yourself", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_RECEIVER);
    assert(bc[2] == OP_SEND);
    assert(bc[4] == OP_RETURN_TOP);
}

static void test_binary_send(void) {
    STA_OOP method = compile("answer: a ^a + 1", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_TEMP); assert(bc[1] == 0);
    assert(bc[2] == OP_PUSH_ONE);
    assert(bc[4] == OP_SEND);
    assert(bc[6] == OP_RETURN_TOP);
}

static void test_keyword_send(void) {
    STA_OOP method = compile("at: i ^self at: i", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_RECEIVER);
    assert(bc[2] == OP_PUSH_TEMP); assert(bc[3] == 0);
    assert(bc[4] == OP_SEND);
    assert(bc[6] == OP_RETURN_TOP);
}

static void test_super_send(void) {
    STA_OOP method = compile("answer ^super yourself", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_RECEIVER);
    assert(bc[2] == OP_SEND_SUPER);
    assert(bc[4] == OP_RETURN_TOP);
}

/* ── §5.5 Cascade ────────────────────────────────────────────────────── */

static void test_cascade(void) {
    STA_OOP method = compile("answer: x ^x yourself; yourself", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_TEMP); assert(bc[1] == 0);
    assert(bc[2] == OP_DUP);
    assert(bc[4] == OP_SEND);
    assert(bc[6] == OP_POP);
    assert(bc[8] == OP_SEND);
    assert(bc[10] == OP_RETURN_TOP);
}

/* ── §5.6 Control structure inlining ──────────────────────────────────── */

static void test_if_true(void) {
    STA_OOP method = compile("answer ^true ifTrue: [42]", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_TRUE);
    assert(bc[2] == OP_JUMP_FALSE);
    assert(bc[4] == OP_PUSH_SMALLINT); assert(bc[5] == 42);
    assert(bc[6] == OP_JUMP);
    assert(bc[8] == OP_PUSH_NIL);
    assert(bc[10] == OP_RETURN_TOP);
}

static void test_if_true_if_false(void) {
    STA_OOP method = compile("answer ^true ifTrue: [1] ifFalse: [2]",
                              0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_TRUE);
    assert(bc[2] == OP_JUMP_FALSE);
    assert(bc[4] == OP_PUSH_ONE);
    assert(bc[6] == OP_JUMP);
    assert(bc[8] == OP_PUSH_TWO);
    assert(bc[10] == OP_RETURN_TOP);
}

static void test_while_true(void) {
    STA_OOP method = compile(
        "answer | x | x := 0. [x < 10] whileTrue: [x := x + 1]. ^x",
        0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    /* x := 0 */
    assert(bc[0] == OP_PUSH_ZERO);
    assert(bc[2] == OP_POP_STORE_TEMP); assert(bc[3] == 0);
    /* condition: x < 10 */
    assert(bc[4] == OP_PUSH_TEMP); assert(bc[5] == 0);
    assert(bc[6] == OP_PUSH_SMALLINT); assert(bc[7] == 10);
    assert(bc[8] == OP_SEND); /* #< */
    assert(bc[10] == OP_JUMP_FALSE);
    /* body: x := x + 1 */
    assert(bc[12] == OP_PUSH_TEMP); assert(bc[13] == 0);
    assert(bc[14] == OP_PUSH_ONE);
    assert(bc[16] == OP_SEND); /* #+ */
    assert(bc[18] == OP_POP_STORE_TEMP); assert(bc[19] == 0);
    assert(bc[20] == OP_JUMP_BACK);
    /* whileTrue: result = nil (for value context), then POP */
    assert(bc[22] == OP_PUSH_NIL);
    assert(bc[24] == OP_POP);
    /* ^x */
    assert(bc[26] == OP_PUSH_TEMP); assert(bc[27] == 0);
    assert(bc[28] == OP_RETURN_TOP);
}

/* ── §5.8 Temp indexing ──────────────────────────────────────────────── */

static void test_temp_indexing(void) {
    uint8_t expected[] = { OP_PUSH_TEMP, 3, OP_RETURN_TOP, 0x00 };
    assert_bytecodes("foo: a bar: b | x y | ^y", NULL, 0,
                      expected, sizeof(expected));
}

/* ── §5.9 Clean block ────────────────────────────────────────────────── */

static void test_clean_block(void) {
    STA_OOP method = compile("answer ^[42]", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_BLOCK_COPY);
    assert(bc[2] == OP_PUSH_SMALLINT); assert(bc[3] == 42);
    assert(bc[4] == OP_RETURN_TOP);
    assert(bc[6] == OP_RETURN_TOP);
}

/* ── §5.11 Tail position ─────────────────────────────────────────────── */

static void test_tail_send(void) {
    STA_OOP method = compile("answer ^self yourself", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_RECEIVER);
    assert(bc[2] == OP_SEND);
    assert(bc[4] == OP_RETURN_TOP);
}

/* ── Nested sends ────────────────────────────────────────────────────── */

static void test_nested_sends(void) {
    STA_OOP method = compile("answer: a ^(a + 1) * 2", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_TEMP); assert(bc[1] == 0);
    assert(bc[2] == OP_PUSH_ONE);
    assert(bc[4] == OP_SEND);
    assert(bc[6] == OP_PUSH_TWO);
    assert(bc[8] == OP_SEND);
    assert(bc[10] == OP_RETURN_TOP);
}

/* ── Header correctness ──────────────────────────────────────────────── */

static void test_header_args(void) {
    STA_OOP method = compile("foo: a bar: b ^a", 0, NULL, 0);
    STA_OOP hdr = sta_method_header(method);
    assert(STA_METHOD_NUM_ARGS(hdr) == 2);
}

static void test_header_temps(void) {
    STA_OOP method = compile("answer | x y z | ^x", 0, NULL, 0);
    STA_OOP hdr = sta_method_header(method);
    assert(STA_METHOD_NUM_ARGS(hdr) == 0);
    assert(STA_METHOD_NUM_TEMPS(hdr) == 3);
}

static void test_header_literals(void) {
    STA_OOP method = compile("answer ^self yourself", 0, NULL, 0);
    STA_OOP hdr = sta_method_header(method);
    assert(STA_METHOD_NUM_LITERALS(hdr) >= 2);
}

/* ── Literal array ───────────────────────────────────────────────────── */

static void test_literal_array(void) {
    STA_OOP method = compile("answer ^#(1 2 3)", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_LIT);
    assert(bc[2] == OP_RETURN_TOP);
    uint8_t lit_idx = bc[1];
    STA_OOP arr = sta_method_literal(method, lit_idx);
    assert(STA_IS_HEAP(arr));
    STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)arr;
    assert(ah->class_index == STA_CLS_ARRAY);
    STA_OOP *elems = sta_payload(ah);
    assert(STA_SMALLINT_VAL(elems[0]) == 1);
    assert(STA_SMALLINT_VAL(elems[1]) == 2);
    assert(STA_SMALLINT_VAL(elems[2]) == 3);
}

/* ── Multiple statements ─────────────────────────────────────────────── */

static void test_multiple_statements(void) {
    STA_OOP method = compile("answer | x | x := 10. ^x * x", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_SMALLINT); assert(bc[1] == 10);
    assert(bc[2] == OP_POP_STORE_TEMP); assert(bc[3] == 0);
    assert(bc[4] == OP_PUSH_TEMP); assert(bc[5] == 0);
    assert(bc[6] == OP_PUSH_TEMP); assert(bc[7] == 0);
    assert(bc[8] == OP_SEND);
    assert(bc[10] == OP_RETURN_TOP);
}

/* ── and: / or: inlining ─────────────────────────────────────────────── */

static void test_and_inline(void) {
    STA_OOP method = compile("answer ^true and: [false]", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_TRUE);
    assert(bc[2] == OP_JUMP_FALSE);
    assert(bc[4] == OP_PUSH_FALSE);
    assert(bc[6] == OP_JUMP);
    assert(bc[8] == OP_PUSH_FALSE);
    assert(bc[10] == OP_RETURN_TOP);
}

static void test_or_inline(void) {
    STA_OOP method = compile("answer ^false or: [true]", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_FALSE);
    assert(bc[2] == OP_JUMP_TRUE);
    assert(bc[4] == OP_PUSH_TRUE);
    assert(bc[6] == OP_JUMP);
    assert(bc[8] == OP_PUSH_TRUE);
    assert(bc[10] == OP_RETURN_TOP);
}

/* ── Character literal ───────────────────────────────────────────────── */

static void test_char_literal(void) {
    STA_OOP method = compile("answer ^$A", 0, NULL, 0);
    const uint8_t *bc = sta_method_bytecodes(method);
    assert(bc[0] == OP_PUSH_LIT);
    assert(bc[2] == OP_RETURN_TOP);
    STA_OOP ch = sta_method_literal(method, bc[1]);
    assert(STA_IS_CHAR(ch));
    assert(STA_CHAR_VAL(ch) == 'A');
}

/* ── Owner class in last literal ─────────────────────────────────────── */

static void test_owner_class_last_literal(void) {
    STA_OOP obj_cls = sta_class_table_get(ct, STA_CLS_OBJECT);
    STA_OOP method = compile("answer ^42", obj_cls, NULL, 0);
    STA_OOP hdr = sta_method_header(method);
    uint8_t nlits = STA_METHOD_NUM_LITERALS(hdr);
    assert(nlits >= 1);
    STA_OOP last_lit = sta_method_literal(method, nlits - 1);
    assert(last_lit == obj_cls);
}

/* ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    setup();
    printf("test_codegen:\n");

    RUN(test_return_42);
    RUN(test_return_zero);
    RUN(test_return_one);
    RUN(test_return_two);
    RUN(test_return_minus_one);
    RUN(test_return_nil);
    RUN(test_return_true);
    RUN(test_return_false);
    RUN(test_return_self);
    RUN(test_return_self_explicit);
    RUN(test_return_large_smallint);
    RUN(test_return_string);
    RUN(test_return_symbol);
    RUN(test_push_temp);
    RUN(test_push_temp_second);
    RUN(test_push_instvar);
    RUN(test_push_instvar_second);
    RUN(test_assign_temp);
    RUN(test_assign_instvar);
    RUN(test_unary_send);
    RUN(test_binary_send);
    RUN(test_keyword_send);
    RUN(test_super_send);
    RUN(test_cascade);
    RUN(test_if_true);
    RUN(test_if_true_if_false);
    RUN(test_while_true);
    RUN(test_temp_indexing);
    RUN(test_clean_block);
    RUN(test_tail_send);
    RUN(test_nested_sends);
    RUN(test_header_args);
    RUN(test_header_temps);
    RUN(test_header_literals);
    RUN(test_literal_array);
    RUN(test_multiple_statements);
    RUN(test_and_inline);
    RUN(test_or_inline);
    RUN(test_char_literal);
    RUN(test_owner_class_last_literal);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
