/* src/compiler/parser.h
 * Recursive descent parser for Smalltalk method syntax.
 * Phase 1 — permanent. See bytecode spec §5.
 *
 * Input: scanner (token stream from scanner.h).
 * Output: AST (heap-allocated tree from ast.h).
 *
 * Standard Smalltalk precedence: unary > binary > keyword.
 * Clean blocks only — non-local return (^ inside block) is rejected.
 * No error recovery in Phase 1 — first error stops parsing.
 */
#pragma once
#include "scanner.h"
#include "ast.h"

/* ── Parser state ────────────────────────────────────────────────────── */

typedef struct {
    STA_Scanner  scanner;
    STA_Token    current;     /* most recently consumed token   */
    STA_Token    previous;    /* token before current           */
    bool         had_error;
    char         error_msg[256];
    int          block_depth; /* >0 means inside a block body   */
} STA_Parser;

/* ── API ─────────────────────────────────────────────────────────────── */

/* Parse a complete method definition from source.
 * Returns the root NODE_METHOD on success, NULL on error.
 * On error, parser->error_msg contains the message. */
STA_AstNode *sta_parse_method(const char *source, STA_Parser *parser);

/* Return the error message from the last failed parse, or NULL. */
const char *sta_parser_error(const STA_Parser *parser);
