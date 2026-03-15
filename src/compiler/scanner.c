/* src/compiler/scanner.c
 * Smalltalk method source scanner — see scanner.h for documentation.
 */
#include "scanner.h"
#include <ctype.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int is_at_end(const STA_Scanner *s) {
    return *s->current == '\0';
}

static char peek(const STA_Scanner *s) {
    return *s->current;
}

static char peek_next(const STA_Scanner *s) {
    if (is_at_end(s)) return '\0';
    return s->current[1];
}

static char advance(STA_Scanner *s) {
    char c = *s->current;
    s->current++;
    if (c == '\n') s->line++;
    return c;
}

static int is_binary_char(char c) {
    return c == '+' || c == '-' || c == '*' || c == '/' ||
           c == '<' || c == '>' || c == '=' || c == '~' ||
           c == '&' || c == '@' || c == '%' || c == ',' ||
           c == '?' || c == '!';
}

static int is_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

static STA_Token make_token(STA_TokenType type, const char *start,
                            uint32_t length, uint32_t line) {
    STA_Token t;
    t.type   = type;
    t.start  = start;
    t.length = length;
    t.line   = line;
    return t;
}

static STA_Token error_token(const char *message, uint32_t line) {
    STA_Token t;
    t.type   = TOKEN_ERROR;
    t.start  = message;
    t.length = (uint32_t)strlen(message);
    t.line   = line;
    return t;
}

/* ── Whitespace and comments ─────────────────────────────────────────── */

static void skip_whitespace_and_comments(STA_Scanner *s) {
    for (;;) {
        char c = peek(s);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance(s);
        } else if (c == '"') {
            /* Comment: skip until closing double quote. */
            advance(s); /* consume opening " */
            while (!is_at_end(s) && peek(s) != '"') {
                advance(s);
            }
            if (!is_at_end(s)) {
                advance(s); /* consume closing " */
            }
            /* If unterminated, we'll just stop here — next scan will
             * hit EOF or produce something reasonable. */
        } else {
            return;
        }
    }
}

/* ── Token scanning ──────────────────────────────────────────────────── */

static STA_Token scan_identifier_or_keyword(STA_Scanner *s) {
    const char *start = s->current;
    uint32_t line = s->line;

    while (is_letter(peek(s)) || is_digit(peek(s))) {
        advance(s);
    }

    /* Check for keyword: identifier followed by colon (not :=). */
    if (peek(s) == ':' && peek_next(s) != '=') {
        advance(s); /* consume the colon */
        return make_token(TOKEN_KEYWORD, start,
                          (uint32_t)(s->current - start), line);
    }

    return make_token(TOKEN_IDENTIFIER, start,
                      (uint32_t)(s->current - start), line);
}

static STA_Token scan_number(STA_Scanner *s, int negative) {
    const char *start = negative ? s->current - 1 : s->current;
    uint32_t line = s->line;

    while (is_digit(peek(s))) {
        advance(s);
    }

    /* Check for float: digits followed by '.' followed by digit. */
    if (peek(s) == '.' && is_digit(peek_next(s))) {
        advance(s); /* consume '.' */
        while (is_digit(peek(s))) {
            advance(s);
        }
        return make_token(TOKEN_FLOAT, start,
                          (uint32_t)(s->current - start), line);
    }

    return make_token(TOKEN_INTEGER, start,
                      (uint32_t)(s->current - start), line);
}

static STA_Token scan_string(STA_Scanner *s) {
    const char *start = s->current;
    uint32_t line = s->line;

    advance(s); /* consume opening quote */

    for (;;) {
        if (is_at_end(s)) {
            return error_token("unterminated string", line);
        }
        if (peek(s) == '\'') {
            advance(s); /* consume closing quote */
            /* Check for embedded quote ('') */
            if (peek(s) == '\'') {
                advance(s); /* consume second quote — embedded, continue */
            } else {
                break; /* end of string */
            }
        } else {
            advance(s);
        }
    }

    return make_token(TOKEN_STRING, start,
                      (uint32_t)(s->current - start), line);
}

static STA_Token scan_symbol_after_hash(STA_Scanner *s) {
    /* s->current is right after '#'. */
    const char *hash_start = s->current - 1;
    uint32_t line = s->line;
    char c = peek(s);

    if (c == '(') {
        advance(s); /* consume ( */
        return make_token(TOKEN_HASH_PAREN, hash_start, 2, line);
    }

    if (c == '\'') {
        /* #'string symbol' */
        advance(s); /* consume opening quote */
        for (;;) {
            if (is_at_end(s)) {
                return error_token("unterminated symbol string", line);
            }
            if (peek(s) == '\'') {
                advance(s);
                if (peek(s) == '\'') {
                    advance(s); /* embedded quote */
                } else {
                    break;
                }
            } else {
                advance(s);
            }
        }
        return make_token(TOKEN_SYMBOL_LITERAL, hash_start,
                          (uint32_t)(s->current - hash_start), line);
    }

    if (is_letter(c)) {
        /* #identifier or #keyword:keyword: */
        while (is_letter(peek(s)) || is_digit(peek(s))) {
            advance(s);
        }
        /* Consume keyword colons: #at:put: */
        while (peek(s) == ':') {
            advance(s); /* consume colon */
            /* After colon, may have more identifier chars for next keyword piece */
            while (is_letter(peek(s)) || is_digit(peek(s))) {
                advance(s);
            }
        }
        return make_token(TOKEN_SYMBOL_LITERAL, hash_start,
                          (uint32_t)(s->current - hash_start), line);
    }

    if (is_binary_char(c)) {
        /* #+ #- #>= etc. — one or two binary chars */
        advance(s);
        if (is_binary_char(peek(s))) {
            advance(s);
        }
        return make_token(TOKEN_SYMBOL_LITERAL, hash_start,
                          (uint32_t)(s->current - hash_start), line);
    }

    return error_token("invalid symbol literal", line);
}

static STA_Token scan_binary_selector(STA_Scanner *s) {
    const char *start = s->current;
    uint32_t line = s->line;

    advance(s); /* consume first char */

    /* Binary selectors are one or two special characters. */
    if (is_binary_char(peek(s))) {
        advance(s);
    }

    return make_token(TOKEN_BINARY_SELECTOR, start,
                      (uint32_t)(s->current - start), line);
}

static STA_Token scan_token(STA_Scanner *s) {
    skip_whitespace_and_comments(s);

    if (is_at_end(s)) {
        return make_token(TOKEN_EOF, s->current, 0, s->line);
    }

    char c = peek(s);

    /* Identifiers and keywords. */
    if (is_letter(c)) {
        return scan_identifier_or_keyword(s);
    }

    /* Numbers. */
    if (is_digit(c)) {
        return scan_number(s, 0);
    }

    /* Minus: could be negative number or binary selector. */
    if (c == '-') {
        /* Negative number: minus immediately before digit with no space. */
        if (is_digit(peek_next(s))) {
            const char *start = s->current;
            uint32_t line = s->line;
            advance(s); /* consume '-' */
            /* But was there a space or identifier before us that means
             * this is a binary op? We check: if the token before this
             * was the very start of the scan (no prior token), or the
             * character just before '-' is a space/delimiter, then it's
             * a negative number. But the scanner doesn't track prior
             * tokens — instead, we follow standard Smalltalk lexing:
             * minus immediately before digit with no space IS a negative
             * number literal. The parser can override if needed.
             *
             * Actually, the standard rule is: leading minus immediately
             * before digit (no space) is negative literal. Our scanner
             * position is right at '-' and peek_next is a digit, so this
             * is correct.
             */
            (void)start;
            (void)line;
            return scan_number(s, 1);
        }
        return scan_binary_selector(s);
    }

    /* String literal. */
    if (c == '\'') {
        return scan_string(s);
    }

    /* Character literal: $X */
    if (c == '$') {
        const char *start = s->current;
        uint32_t line = s->line;
        advance(s); /* consume $ */
        if (is_at_end(s)) {
            return error_token("unterminated character literal", line);
        }
        advance(s); /* consume the character */
        return make_token(TOKEN_CHAR_LITERAL, start,
                          (uint32_t)(s->current - start), line);
    }

    /* Hash: symbol literal or literal array. */
    if (c == '#') {
        advance(s); /* consume # */
        return scan_symbol_after_hash(s);
    }

    /* Colon: could be := (assign) or standalone : (block arg). */
    if (c == ':') {
        advance(s); /* consume : */
        if (peek(s) == '=') {
            advance(s); /* consume = */
            return make_token(TOKEN_ASSIGN, s->current - 2, 2, s->line);
        }
        return make_token(TOKEN_COLON, s->current - 1, 1, s->line);
    }

    /* Single-character tokens. */
    {
        const char *start = s->current;
        uint32_t line = s->line;
        advance(s);

        switch (c) {
            case '^': return make_token(TOKEN_RETURN,    start, 1, line);
            case '(': return make_token(TOKEN_LPAREN,    start, 1, line);
            case ')': return make_token(TOKEN_RPAREN,    start, 1, line);
            case '[': return make_token(TOKEN_LBRACKET,  start, 1, line);
            case ']': return make_token(TOKEN_RBRACKET,  start, 1, line);
            case '.': return make_token(TOKEN_PERIOD,    start, 1, line);
            case ';': return make_token(TOKEN_SEMICOLON, start, 1, line);
            case '|': return make_token(TOKEN_VBAR,      start, 1, line);
            default:  break;
        }

        /* Binary selector characters. */
        if (is_binary_char(c)) {
            /* We already advanced past c. Check for two-char binary. */
            if (is_binary_char(peek(s))) {
                advance(s);
            }
            return make_token(TOKEN_BINARY_SELECTOR, start,
                              (uint32_t)(s->current - start), line);
        }

        return error_token("unexpected character", line);
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

void sta_scanner_init(STA_Scanner *scanner, const char *source) {
    scanner->source  = source;
    scanner->current = source;
    scanner->line    = 1;
}

STA_Token sta_scanner_next(STA_Scanner *scanner) {
    return scan_token(scanner);
}

STA_Token sta_scanner_peek(STA_Scanner *scanner) {
    /* Save state, scan, restore. */
    const char *saved_current = scanner->current;
    uint32_t saved_line = scanner->line;
    STA_Token t = scan_token(scanner);
    scanner->current = saved_current;
    scanner->line    = saved_line;
    return t;
}
