/* src/compiler/scanner.h
 * Smalltalk method source scanner (lexer).
 * Phase 1 — permanent. See bytecode spec §5.
 *
 * Pull model: caller calls sta_scanner_next() to consume one token at a time.
 * Input: null-terminated C string (method source).
 * Each token carries: type, start pointer, length, line number.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* ── Token types ─────────────────────────────────────────────────────── */

typedef enum {
    TOKEN_IDENTIFIER,       /* starts with letter, contains letters/digits    */
    TOKEN_KEYWORD,          /* identifier ending in colon (e.g. at:)          */
    TOKEN_INTEGER,          /* decimal digits, optional leading minus         */
    TOKEN_FLOAT,            /* digits.digits (no scientific notation Phase 1) */
    TOKEN_STRING,           /* 'single quoted', '' for embedded quote         */
    TOKEN_SYMBOL_LITERAL,   /* #foo, #at:put:, #+, #'string symbol'          */
    TOKEN_CHAR_LITERAL,     /* $A, $  (space char)                           */
    TOKEN_HASH_PAREN,       /* #( for literal array start                    */
    TOKEN_BINARY_SELECTOR,  /* + - * / < > = ~ & @ % , ? !                   */
    TOKEN_ASSIGN,           /* :=                                            */
    TOKEN_RETURN,           /* ^                                             */
    TOKEN_LPAREN,           /* (                                             */
    TOKEN_RPAREN,           /* )                                             */
    TOKEN_LBRACKET,         /* [                                             */
    TOKEN_RBRACKET,         /* ]                                             */
    TOKEN_PERIOD,           /* .                                             */
    TOKEN_SEMICOLON,        /* ; (cascade separator)                         */
    TOKEN_COLON,            /* : (standalone, for block args [:x | ...])     */
    TOKEN_VBAR,             /* | (for temp declarations and block arg sep)   */
    TOKEN_BANG,             /* ! (chunk separator for file-in)               */
    TOKEN_EOF,              /* end of input                                  */
    TOKEN_ERROR,            /* invalid character or unterminated string       */
} STA_TokenType;

/* ── Token ───────────────────────────────────────────────────────────── */

typedef struct {
    STA_TokenType type;
    const char   *start;    /* pointer into source string                    */
    uint32_t      length;   /* byte length of the token text                 */
    uint32_t      line;     /* 1-based line number                           */
} STA_Token;

/* ── Scanner state ───────────────────────────────────────────────────── */

typedef struct {
    const char *source;     /* original source string (null-terminated)      */
    const char *current;    /* current position in source                    */
    uint32_t    line;       /* current line number (1-based)                 */
} STA_Scanner;

/* ── API ─────────────────────────────────────────────────────────────── */

/* Initialize scanner with source string. */
void sta_scanner_init(STA_Scanner *scanner, const char *source);

/* Consume and return the next token. */
STA_Token sta_scanner_next(STA_Scanner *scanner);

/* Peek at the next token without consuming it. */
STA_Token sta_scanner_peek(STA_Scanner *scanner);
