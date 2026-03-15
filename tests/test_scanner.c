/* tests/test_scanner.c
 * Scanner tests for Phase 1 Epic 7a.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Internal header — tests may access src/ headers per project convention. */
#include "compiler/scanner.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

#define ASSERT_TOKEN(scanner, expected_type) do { \
    STA_Token _t = sta_scanner_next(&(scanner)); \
    assert(_t.type == (expected_type)); \
    (void)_t; \
} while(0)

#define ASSERT_TOKEN_VAL(scanner, expected_type, expected_text) do { \
    STA_Token _t = sta_scanner_next(&(scanner)); \
    assert(_t.type == (expected_type)); \
    assert(_t.length == (uint32_t)strlen(expected_text)); \
    assert(memcmp(_t.start, expected_text, _t.length) == 0); \
    (void)_t; \
} while(0)

/* ── Tests ───────────────────────────────────────────────────────────── */

static void test_empty_input(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "");
    STA_Token t = sta_scanner_next(&s);
    assert(t.type == TOKEN_EOF);
}

static void test_single_identifier(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "foo");
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "foo");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_keyword_token(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "at:");
    ASSERT_TOKEN_VAL(s, TOKEN_KEYWORD, "at:");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_keywords_separate(void) {
    /* "at:put:" scans as two keyword tokens: "at:" and "put:" */
    STA_Scanner s;
    sta_scanner_init(&s, "at:put:");
    /* Actually, the scanner should scan "at:" then "put:" as separate
     * keywords only if there's a space or identifier between them.
     * "at:put:" with no space: scanner reads "at" sees ':', consumes it
     * as keyword "at:", then reads "put" sees ':', consumes as "put:".
     */
    ASSERT_TOKEN_VAL(s, TOKEN_KEYWORD, "at:");
    ASSERT_TOKEN_VAL(s, TOKEN_KEYWORD, "put:");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_integer(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "42");
    ASSERT_TOKEN_VAL(s, TOKEN_INTEGER, "42");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_float(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "3.14");
    ASSERT_TOKEN_VAL(s, TOKEN_FLOAT, "3.14");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_string(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "'hello'");
    ASSERT_TOKEN_VAL(s, TOKEN_STRING, "'hello'");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_string_with_embedded_quote(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "'it''s'");
    STA_Token t = sta_scanner_next(&s);
    assert(t.type == TOKEN_STRING);
    /* Token text includes the outer quotes: 'it''s' */
    assert(t.length == 7);
    assert(memcmp(t.start, "'it''s'", 7) == 0);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_comment_skipping(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "\"ignored\" 42");
    ASSERT_TOKEN_VAL(s, TOKEN_INTEGER, "42");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_negative_number(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "-42");
    ASSERT_TOKEN_VAL(s, TOKEN_INTEGER, "-42");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_minus_as_binary(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "x - 42");
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "x");
    ASSERT_TOKEN_VAL(s, TOKEN_BINARY_SELECTOR, "-");
    ASSERT_TOKEN_VAL(s, TOKEN_INTEGER, "42");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_binary_selectors(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "+ - >= ~=");
    ASSERT_TOKEN_VAL(s, TOKEN_BINARY_SELECTOR, "+");
    ASSERT_TOKEN_VAL(s, TOKEN_BINARY_SELECTOR, "-");
    ASSERT_TOKEN_VAL(s, TOKEN_BINARY_SELECTOR, ">=");
    ASSERT_TOKEN_VAL(s, TOKEN_BINARY_SELECTOR, "~=");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_symbol_identifier(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "#foo");
    ASSERT_TOKEN_VAL(s, TOKEN_SYMBOL_LITERAL, "#foo");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_symbol_binary(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "#+");
    ASSERT_TOKEN_VAL(s, TOKEN_SYMBOL_LITERAL, "#+");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_symbol_keyword(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "#at:put:");
    ASSERT_TOKEN_VAL(s, TOKEN_SYMBOL_LITERAL, "#at:put:");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_symbol_string(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "#'hello world'");
    ASSERT_TOKEN_VAL(s, TOKEN_SYMBOL_LITERAL, "#'hello world'");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_literal_array(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "#( 1 2 3 )");
    ASSERT_TOKEN_VAL(s, TOKEN_HASH_PAREN, "#(");
    ASSERT_TOKEN_VAL(s, TOKEN_INTEGER, "1");
    ASSERT_TOKEN_VAL(s, TOKEN_INTEGER, "2");
    ASSERT_TOKEN_VAL(s, TOKEN_INTEGER, "3");
    ASSERT_TOKEN(s, TOKEN_RPAREN);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_character_literal(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "$A");
    ASSERT_TOKEN_VAL(s, TOKEN_CHAR_LITERAL, "$A");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_character_space(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "$ ");
    STA_Token t = sta_scanner_next(&s);
    assert(t.type == TOKEN_CHAR_LITERAL);
    assert(t.length == 2);
    assert(t.start[1] == ' ');
}

static void test_assign(void) {
    STA_Scanner s;
    sta_scanner_init(&s, ":=");
    ASSERT_TOKEN_VAL(s, TOKEN_ASSIGN, ":=");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_return(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "^");
    ASSERT_TOKEN(s, TOKEN_RETURN);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_parens(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "()");
    ASSERT_TOKEN(s, TOKEN_LPAREN);
    ASSERT_TOKEN(s, TOKEN_RPAREN);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_brackets(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "[]");
    ASSERT_TOKEN(s, TOKEN_LBRACKET);
    ASSERT_TOKEN(s, TOKEN_RBRACKET);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_period(void) {
    STA_Scanner s;
    sta_scanner_init(&s, ".");
    ASSERT_TOKEN(s, TOKEN_PERIOD);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_semicolon(void) {
    STA_Scanner s;
    sta_scanner_init(&s, ";");
    ASSERT_TOKEN(s, TOKEN_SEMICOLON);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_colon_standalone(void) {
    STA_Scanner s;
    sta_scanner_init(&s, ":");
    ASSERT_TOKEN(s, TOKEN_COLON);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_vbar(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "|");
    ASSERT_TOKEN(s, TOKEN_VBAR);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_method_header_keywords(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "at: index put: value");
    ASSERT_TOKEN_VAL(s, TOKEN_KEYWORD, "at:");
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "index");
    ASSERT_TOKEN_VAL(s, TOKEN_KEYWORD, "put:");
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "value");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_line_tracking(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "foo\nbar\nbaz");
    STA_Token t1 = sta_scanner_next(&s);
    assert(t1.line == 1);
    STA_Token t2 = sta_scanner_next(&s);
    assert(t2.line == 2);
    STA_Token t3 = sta_scanner_next(&s);
    assert(t3.line == 3);
}

static void test_peek(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "foo bar");
    STA_Token p1 = sta_scanner_peek(&s);
    assert(p1.type == TOKEN_IDENTIFIER);
    assert(p1.length == 3);
    /* Peek doesn't consume — next should give same token. */
    STA_Token n1 = sta_scanner_next(&s);
    assert(n1.type == TOKEN_IDENTIFIER);
    assert(n1.length == 3);
    assert(n1.start == p1.start);
    /* Now "bar" should be next. */
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "bar");
}

static void test_full_method_scan(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "at: index put: value | temp | ^temp := value");
    ASSERT_TOKEN_VAL(s, TOKEN_KEYWORD, "at:");
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "index");
    ASSERT_TOKEN_VAL(s, TOKEN_KEYWORD, "put:");
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "value");
    ASSERT_TOKEN(s, TOKEN_VBAR);
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "temp");
    ASSERT_TOKEN(s, TOKEN_VBAR);
    ASSERT_TOKEN(s, TOKEN_RETURN);
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "temp");
    ASSERT_TOKEN(s, TOKEN_ASSIGN);
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "value");
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_block_scan(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "[:x | x + 1]");
    ASSERT_TOKEN(s, TOKEN_LBRACKET);
    ASSERT_TOKEN(s, TOKEN_COLON);
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "x");
    ASSERT_TOKEN(s, TOKEN_VBAR);
    ASSERT_TOKEN_VAL(s, TOKEN_IDENTIFIER, "x");
    ASSERT_TOKEN_VAL(s, TOKEN_BINARY_SELECTOR, "+");
    ASSERT_TOKEN_VAL(s, TOKEN_INTEGER, "1");
    ASSERT_TOKEN(s, TOKEN_RBRACKET);
    ASSERT_TOKEN(s, TOKEN_EOF);
}

static void test_error_token(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "`");
    STA_Token t = sta_scanner_next(&s);
    assert(t.type == TOKEN_ERROR);
}

static void test_unterminated_string(void) {
    STA_Scanner s;
    sta_scanner_init(&s, "'unterminated");
    STA_Token t = sta_scanner_next(&s);
    assert(t.type == TOKEN_ERROR);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_scanner:\n");
    RUN(test_empty_input);
    RUN(test_single_identifier);
    RUN(test_keyword_token);
    RUN(test_keywords_separate);
    RUN(test_integer);
    RUN(test_float);
    RUN(test_string);
    RUN(test_string_with_embedded_quote);
    RUN(test_comment_skipping);
    RUN(test_negative_number);
    RUN(test_minus_as_binary);
    RUN(test_binary_selectors);
    RUN(test_symbol_identifier);
    RUN(test_symbol_binary);
    RUN(test_symbol_keyword);
    RUN(test_symbol_string);
    RUN(test_literal_array);
    RUN(test_character_literal);
    RUN(test_character_space);
    RUN(test_assign);
    RUN(test_return);
    RUN(test_parens);
    RUN(test_brackets);
    RUN(test_period);
    RUN(test_semicolon);
    RUN(test_colon_standalone);
    RUN(test_vbar);
    RUN(test_method_header_keywords);
    RUN(test_line_tracking);
    RUN(test_peek);
    RUN(test_full_method_scan);
    RUN(test_block_scan);
    RUN(test_error_token);
    RUN(test_unterminated_string);
    printf("  %d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
