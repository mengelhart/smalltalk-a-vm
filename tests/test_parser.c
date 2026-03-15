/* tests/test_parser.c
 * Parser tests for Phase 1 Epic 7a.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "compiler/parser.h"
#include "compiler/ast.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-55s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

/* ── Tests ───────────────────────────────────────────────────────────── */

static void test_unary_method(void) {
    /* "foo ^42" → METHOD(#foo, body=[RETURN(INT(42))]) */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^42", &p);
    assert(m);
    assert(m->type == NODE_METHOD);
    assert(strcmp(m->as.method.selector, "foo") == 0);
    assert(m->as.method.arg_count == 0);
    assert(m->as.method.body_count == 1);
    assert(m->as.method.body[0]->type == NODE_RETURN);
    assert(m->as.method.body[0]->as.ret.expr->type == NODE_LITERAL_INT);
    assert(m->as.method.body[0]->as.ret.expr->as.integer.value == 42);
    sta_ast_free(m);
}

static void test_binary_method(void) {
    /* "+ other ^self" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("+ other ^self", &p);
    assert(m);
    assert(m->type == NODE_METHOD);
    assert(strcmp(m->as.method.selector, "+") == 0);
    assert(m->as.method.arg_count == 1);
    assert(strcmp(m->as.method.args[0], "other") == 0);
    assert(m->as.method.body_count == 1);
    assert(m->as.method.body[0]->type == NODE_RETURN);
    assert(m->as.method.body[0]->as.ret.expr->type == NODE_SELF);
    sta_ast_free(m);
}

static void test_keyword_method(void) {
    /* "at: index put: value | temp | ^temp" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("at: index put: value | temp | ^temp", &p);
    assert(m);
    assert(strcmp(m->as.method.selector, "at:put:") == 0);
    assert(m->as.method.arg_count == 2);
    assert(strcmp(m->as.method.args[0], "index") == 0);
    assert(strcmp(m->as.method.args[1], "value") == 0);
    assert(m->as.method.temp_count == 1);
    assert(strcmp(m->as.method.temps[0], "temp") == 0);
    assert(m->as.method.body_count == 1);
    assert(m->as.method.body[0]->type == NODE_RETURN);
    STA_AstNode *var = m->as.method.body[0]->as.ret.expr;
    assert(var->type == NODE_VARIABLE);
    assert(strcmp(var->as.variable.name, "temp") == 0);
    sta_ast_free(m);
}

static void test_precedence_unary_binds_tighter(void) {
    /* "foo ^a + b size"
     * Correct: a + (b size)
     * AST: SEND(VAR(a), #+, [SEND(VAR(b), #size)]) */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^a + b size", &p);
    assert(m);
    STA_AstNode *ret = m->as.method.body[0];
    assert(ret->type == NODE_RETURN);
    STA_AstNode *send = ret->as.ret.expr;
    assert(send->type == NODE_SEND);
    assert(strcmp(send->as.send.selector, "+") == 0);
    /* receiver is VAR(a) */
    assert(send->as.send.receiver->type == NODE_VARIABLE);
    assert(strcmp(send->as.send.receiver->as.variable.name, "a") == 0);
    /* arg is SEND(VAR(b), #size) */
    assert(send->as.send.arg_count == 1);
    STA_AstNode *arg = send->as.send.args[0];
    assert(arg->type == NODE_SEND);
    assert(strcmp(arg->as.send.selector, "size") == 0);
    assert(arg->as.send.receiver->type == NODE_VARIABLE);
    assert(strcmp(arg->as.send.receiver->as.variable.name, "b") == 0);
    sta_ast_free(m);
}

static void test_precedence_binary_tighter_than_keyword(void) {
    /* "foo ^a at: b + c"
     * Correct: a at: (b + c)
     * AST: SEND(VAR(a), #at:, [SEND(VAR(b), #+, [VAR(c)])]) */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^a at: b + c", &p);
    assert(m);
    STA_AstNode *send = m->as.method.body[0]->as.ret.expr;
    assert(send->type == NODE_SEND);
    assert(strcmp(send->as.send.selector, "at:") == 0);
    assert(send->as.send.receiver->type == NODE_VARIABLE);
    assert(strcmp(send->as.send.receiver->as.variable.name, "a") == 0);
    assert(send->as.send.arg_count == 1);
    STA_AstNode *arg = send->as.send.args[0];
    assert(arg->type == NODE_SEND);
    assert(strcmp(arg->as.send.selector, "+") == 0);
    sta_ast_free(m);
}

static void test_paren_override(void) {
    /* "foo ^(a + b) size"
     * AST: SEND(SEND(VAR(a), #+, [VAR(b)]), #size) */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^(a + b) size", &p);
    assert(m);
    STA_AstNode *outer = m->as.method.body[0]->as.ret.expr;
    assert(outer->type == NODE_SEND);
    assert(strcmp(outer->as.send.selector, "size") == 0);
    STA_AstNode *inner = outer->as.send.receiver;
    assert(inner->type == NODE_SEND);
    assert(strcmp(inner->as.send.selector, "+") == 0);
    sta_ast_free(m);
}

static void test_cascade(void) {
    /* "foo ^x foo; bar" → CASCADE(VAR(x), [(#foo,[]), (#bar,[])]) */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^x foo; bar", &p);
    assert(m);
    STA_AstNode *cascade = m->as.method.body[0]->as.ret.expr;
    assert(cascade->type == NODE_CASCADE);
    assert(cascade->as.cascade.receiver->type == NODE_VARIABLE);
    assert(strcmp(cascade->as.cascade.receiver->as.variable.name, "x") == 0);
    assert(cascade->as.cascade.msg_count == 2);
    assert(strcmp(cascade->as.cascade.messages[0].selector, "foo") == 0);
    assert(cascade->as.cascade.messages[0].arg_count == 0);
    assert(strcmp(cascade->as.cascade.messages[1].selector, "bar") == 0);
    assert(cascade->as.cascade.messages[1].arg_count == 0);
    sta_ast_free(m);
}

static void test_block(void) {
    /* "foo ^[:x | x + 1]" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^[:x | x + 1]", &p);
    assert(m);
    STA_AstNode *ret = m->as.method.body[0]->as.ret.expr;
    assert(ret->type == NODE_BLOCK);
    assert(ret->as.method.arg_count == 1);
    assert(strcmp(ret->as.method.args[0], "x") == 0);
    assert(ret->as.method.body_count == 1);
    STA_AstNode *send = ret->as.method.body[0];
    assert(send->type == NODE_SEND);
    assert(strcmp(send->as.send.selector, "+") == 0);
    sta_ast_free(m);
}

static void test_assignment(void) {
    /* "foo x := 42" → ASSIGN(VAR(x), INT(42)) */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo x := 42", &p);
    assert(m);
    assert(m->as.method.body_count == 1);
    STA_AstNode *assign = m->as.method.body[0];
    assert(assign->type == NODE_ASSIGN);
    assert(assign->as.assign.variable->type == NODE_VARIABLE);
    assert(strcmp(assign->as.assign.variable->as.variable.name, "x") == 0);
    assert(assign->as.assign.value->type == NODE_LITERAL_INT);
    assert(assign->as.assign.value->as.integer.value == 42);
    sta_ast_free(m);
}

static void test_literal_array(void) {
    /* "foo ^#(1 2 3)" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^#(1 2 3)", &p);
    assert(m);
    STA_AstNode *arr = m->as.method.body[0]->as.ret.expr;
    assert(arr->type == NODE_LITERAL_ARRAY);
    assert(arr->as.array.count == 3);
    assert(arr->as.array.elements[0]->type == NODE_LITERAL_INT);
    assert(arr->as.array.elements[0]->as.integer.value == 1);
    assert(arr->as.array.elements[1]->as.integer.value == 2);
    assert(arr->as.array.elements[2]->as.integer.value == 3);
    sta_ast_free(m);
}

static void test_string_literal(void) {
    /* "foo ^'hello'" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^'hello'", &p);
    assert(m);
    STA_AstNode *str = m->as.method.body[0]->as.ret.expr;
    assert(str->type == NODE_LITERAL_STRING);
    assert(strcmp(str->as.variable.name, "hello") == 0);
    sta_ast_free(m);
}

static void test_symbol_literal(void) {
    /* "foo ^#foo" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^#bar", &p);
    assert(m);
    STA_AstNode *sym = m->as.method.body[0]->as.ret.expr;
    assert(sym->type == NODE_LITERAL_SYMBOL);
    assert(strcmp(sym->as.variable.name, "bar") == 0);
    sta_ast_free(m);
}

static void test_character_literal(void) {
    /* "foo ^$A" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^$A", &p);
    assert(m);
    STA_AstNode *ch = m->as.method.body[0]->as.ret.expr;
    assert(ch->type == NODE_LITERAL_CHAR);
    assert(ch->as.character.value == 'A');
    sta_ast_free(m);
}

static void test_true_false_nil(void) {
    STA_Parser p;

    STA_AstNode *m1 = sta_parse_method("foo ^true", &p);
    assert(m1);
    assert(m1->as.method.body[0]->as.ret.expr->type == NODE_LITERAL_BOOL);
    assert(m1->as.method.body[0]->as.ret.expr->as.boolean.value == true);
    sta_ast_free(m1);

    STA_AstNode *m2 = sta_parse_method("foo ^false", &p);
    assert(m2);
    assert(m2->as.method.body[0]->as.ret.expr->type == NODE_LITERAL_BOOL);
    assert(m2->as.method.body[0]->as.ret.expr->as.boolean.value == false);
    sta_ast_free(m2);

    STA_AstNode *m3 = sta_parse_method("foo ^nil", &p);
    assert(m3);
    assert(m3->as.method.body[0]->as.ret.expr->type == NODE_LITERAL_NIL);
    sta_ast_free(m3);
}

static void test_self(void) {
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^self", &p);
    assert(m);
    assert(m->as.method.body[0]->as.ret.expr->type == NODE_SELF);
    sta_ast_free(m);
}

static void test_super(void) {
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^super bar", &p);
    assert(m);
    STA_AstNode *send = m->as.method.body[0]->as.ret.expr;
    assert(send->type == NODE_SUPER_SEND);
    assert(strcmp(send->as.send.selector, "bar") == 0);
    sta_ast_free(m);
}

static void test_nested_send(void) {
    /* "foo ^self foo: (bar baz: 42)" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^self foo: (bar baz: 42)", &p);
    assert(m);
    STA_AstNode *outer = m->as.method.body[0]->as.ret.expr;
    assert(outer->type == NODE_SEND);
    assert(strcmp(outer->as.send.selector, "foo:") == 0);
    assert(outer->as.send.receiver->type == NODE_SELF);
    assert(outer->as.send.arg_count == 1);
    STA_AstNode *inner = outer->as.send.args[0];
    assert(inner->type == NODE_SEND);
    assert(strcmp(inner->as.send.selector, "baz:") == 0);
    assert(inner->as.send.receiver->type == NODE_VARIABLE);
    assert(strcmp(inner->as.send.receiver->as.variable.name, "bar") == 0);
    assert(inner->as.send.arg_count == 1);
    assert(inner->as.send.args[0]->type == NODE_LITERAL_INT);
    assert(inner->as.send.args[0]->as.integer.value == 42);
    sta_ast_free(m);
}

static void test_non_local_return_in_block_rejected(void) {
    /* "foo [:x | ^x]" → parse error */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo [:x | ^x]", &p);
    assert(m == NULL);
    assert(p.had_error);
    assert(strstr(p.error_msg, "non-local return") != NULL);
}

static void test_empty_method_body(void) {
    /* "foo" → METHOD(#foo, body=[]) — no implicit ^self from parser */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo", &p);
    assert(m);
    assert(m->type == NODE_METHOD);
    assert(strcmp(m->as.method.selector, "foo") == 0);
    assert(m->as.method.body_count == 0);
    sta_ast_free(m);
}

static void test_multiple_statements(void) {
    /* "foo a := 1. b := 2. ^a" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo a := 1. b := 2. ^a", &p);
    assert(m);
    assert(m->as.method.body_count == 3);
    assert(m->as.method.body[0]->type == NODE_ASSIGN);
    assert(m->as.method.body[1]->type == NODE_ASSIGN);
    assert(m->as.method.body[2]->type == NODE_RETURN);
    sta_ast_free(m);
}

static void test_chained_unary(void) {
    /* "foo ^a b c" → SEND(SEND(SEND(VAR(a), #b), #c)) — no, that's
     * SEND(SEND(VAR(a), #b), #c) — three chained unary sends. */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^a b c", &p);
    assert(m);
    STA_AstNode *outer = m->as.method.body[0]->as.ret.expr;
    assert(outer->type == NODE_SEND);
    assert(strcmp(outer->as.send.selector, "c") == 0);
    STA_AstNode *inner = outer->as.send.receiver;
    assert(inner->type == NODE_SEND);
    assert(strcmp(inner->as.send.selector, "b") == 0);
    assert(inner->as.send.receiver->type == NODE_VARIABLE);
    assert(strcmp(inner->as.send.receiver->as.variable.name, "a") == 0);
    sta_ast_free(m);
}

static void test_float_literal(void) {
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^3.14", &p);
    assert(m);
    STA_AstNode *f = m->as.method.body[0]->as.ret.expr;
    assert(f->type == NODE_LITERAL_FLOAT);
    assert(f->as.floatval.value > 3.13 && f->as.floatval.value < 3.15);
    sta_ast_free(m);
}

static void test_block_no_args(void) {
    /* "foo ^[42]" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^[42]", &p);
    assert(m);
    STA_AstNode *block = m->as.method.body[0]->as.ret.expr;
    assert(block->type == NODE_BLOCK);
    assert(block->as.method.arg_count == 0);
    assert(block->as.method.body_count == 1);
    assert(block->as.method.body[0]->type == NODE_LITERAL_INT);
    sta_ast_free(m);
}

static void test_block_with_temps(void) {
    /* "foo ^[:x | | t | t := x. t]" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^[:x | | t | t := x. t]", &p);
    assert(m);
    STA_AstNode *block = m->as.method.body[0]->as.ret.expr;
    assert(block->type == NODE_BLOCK);
    assert(block->as.method.arg_count == 1);
    assert(block->as.method.temp_count == 1);
    assert(strcmp(block->as.method.temps[0], "t") == 0);
    assert(block->as.method.body_count == 2);
    sta_ast_free(m);
}

static void test_cascade_keyword(void) {
    /* "foo ^dict at: 1 put: 2; at: 3 put: 4" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^dict at: 1 put: 2; at: 3 put: 4", &p);
    assert(m);
    STA_AstNode *cascade = m->as.method.body[0]->as.ret.expr;
    assert(cascade->type == NODE_CASCADE);
    assert(cascade->as.cascade.msg_count == 2);
    assert(strcmp(cascade->as.cascade.messages[0].selector, "at:put:") == 0);
    assert(cascade->as.cascade.messages[0].arg_count == 2);
    assert(strcmp(cascade->as.cascade.messages[1].selector, "at:put:") == 0);
    assert(cascade->as.cascade.messages[1].arg_count == 2);
    sta_ast_free(m);
}

static void test_negative_integer(void) {
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^-42", &p);
    assert(m);
    STA_AstNode *lit = m->as.method.body[0]->as.ret.expr;
    assert(lit->type == NODE_LITERAL_INT);
    assert(lit->as.integer.value == -42);
    sta_ast_free(m);
}

static void test_super_keyword_send(void) {
    /* "foo ^super at: 1 put: 2" */
    STA_Parser p;
    STA_AstNode *m = sta_parse_method("foo ^super at: 1 put: 2", &p);
    assert(m);
    STA_AstNode *send = m->as.method.body[0]->as.ret.expr;
    assert(send->type == NODE_SUPER_SEND);
    assert(strcmp(send->as.send.selector, "at:put:") == 0);
    sta_ast_free(m);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_parser:\n");
    RUN(test_unary_method);
    RUN(test_binary_method);
    RUN(test_keyword_method);
    RUN(test_precedence_unary_binds_tighter);
    RUN(test_precedence_binary_tighter_than_keyword);
    RUN(test_paren_override);
    RUN(test_cascade);
    RUN(test_block);
    RUN(test_assignment);
    RUN(test_literal_array);
    RUN(test_string_literal);
    RUN(test_symbol_literal);
    RUN(test_character_literal);
    RUN(test_true_false_nil);
    RUN(test_self);
    RUN(test_super);
    RUN(test_nested_send);
    RUN(test_non_local_return_in_block_rejected);
    RUN(test_empty_method_body);
    RUN(test_multiple_statements);
    RUN(test_chained_unary);
    RUN(test_float_literal);
    RUN(test_block_no_args);
    RUN(test_block_with_temps);
    RUN(test_cascade_keyword);
    RUN(test_negative_integer);
    RUN(test_super_keyword_send);
    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
