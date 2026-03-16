/* src/compiler/parser.c
 * Recursive descent parser for Smalltalk method syntax.
 * See parser.h for documentation.
 */
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void parser_error(STA_Parser *p, const char *msg) {
    if (p->had_error) return; /* keep first error */
    p->had_error = true;
    snprintf(p->error_msg, sizeof(p->error_msg), "line %u: %s",
             p->current.line, msg);
}

static void parser_error_at(STA_Parser *p, uint32_t line, const char *msg) {
    if (p->had_error) return;
    p->had_error = true;
    snprintf(p->error_msg, sizeof(p->error_msg), "line %u: %s", line, msg);
}

static void advance_token(STA_Parser *p) {
    p->previous = p->current;
    p->current = sta_scanner_next(&p->scanner);
}

static bool check(const STA_Parser *p, STA_TokenType type) {
    return p->current.type == type;
}

static bool match(STA_Parser *p, STA_TokenType type) {
    if (!check(p, type)) return false;
    advance_token(p);
    return true;
}

static bool consume(STA_Parser *p, STA_TokenType type, const char *msg) {
    if (check(p, type)) {
        advance_token(p);
        return true;
    }
    parser_error(p, msg);
    return false;
}

/* Copy n bytes from src into a new null-terminated string. */
static char *copy_str(const char *src, uint32_t len) {
    char *s = malloc(len + 1);
    if (s) {
        memcpy(s, src, len);
        s[len] = '\0';
    }
    return s;
}

/* Copy token text to a new string. */
static char *token_str(const STA_Token *t) {
    return copy_str(t->start, t->length);
}

static STA_AstNode *alloc_node(STA_NodeType type, uint32_t line) {
    STA_AstNode *n = calloc(1, sizeof(STA_AstNode));
    if (n) {
        n->type = type;
        n->line = line;
    }
    return n;
}

/* ── Forward declarations ────────────────────────────────────────────── */

static STA_AstNode *parse_expression(STA_Parser *p);
static STA_AstNode *parse_keyword_expr(STA_Parser *p);
static STA_AstNode *parse_binary_expr(STA_Parser *p);
static STA_AstNode *parse_unary_expr(STA_Parser *p);
static STA_AstNode *parse_primary(STA_Parser *p);

/* ── Dynamic array helpers ───────────────────────────────────────────── */

/* Simple growable pointer array. */
typedef struct {
    void  **items;
    uint32_t count;
    uint32_t capacity;
} PtrArray;

static void ptr_array_init(PtrArray *a) {
    a->items = NULL;
    a->count = 0;
    a->capacity = 0;
}

static bool ptr_array_push(PtrArray *a, void *item) {
    if (a->count == a->capacity) {
        uint32_t new_cap = a->capacity ? a->capacity * 2 : 4;
        void **new_items = realloc(a->items, new_cap * sizeof(void *));
        if (!new_items) return false;
        a->items = new_items;
        a->capacity = new_cap;
    }
    a->items[a->count++] = item;
    return true;
}

/* ── Literal parsing ─────────────────────────────────────────────────── */

static STA_AstNode *parse_literal(STA_Parser *p);

static STA_AstNode *parse_literal_array(STA_Parser *p) {
    /* TOKEN_HASH_PAREN already consumed. */
    uint32_t line = p->previous.line;
    PtrArray elems;
    ptr_array_init(&elems);

    while (!check(p, TOKEN_RPAREN) && !check(p, TOKEN_EOF) && !p->had_error) {
        /* Inside literal arrays, tokens are treated as literals.
         * Identifiers like true/false/nil and symbols are allowed. */
        STA_AstNode *elem = NULL;

        if (check(p, TOKEN_INTEGER) || check(p, TOKEN_FLOAT) ||
            check(p, TOKEN_STRING) || check(p, TOKEN_CHAR_LITERAL) ||
            check(p, TOKEN_SYMBOL_LITERAL) || check(p, TOKEN_HASH_PAREN)) {
            elem = parse_literal(p);
        } else if (check(p, TOKEN_IDENTIFIER)) {
            /* true, false, nil, or treated as symbol */
            advance_token(p);
            const char *txt = p->previous.start;
            uint32_t len = p->previous.length;
            if (len == 3 && memcmp(txt, "nil", 3) == 0) {
                elem = alloc_node(NODE_LITERAL_NIL, p->previous.line);
            } else if (len == 4 && memcmp(txt, "true", 4) == 0) {
                elem = alloc_node(NODE_LITERAL_BOOL, p->previous.line);
                if (elem) elem->as.boolean.value = true;
            } else if (len == 5 && memcmp(txt, "false", 5) == 0) {
                elem = alloc_node(NODE_LITERAL_BOOL, p->previous.line);
                if (elem) elem->as.boolean.value = false;
            } else {
                /* Bare identifier in literal array → symbol */
                elem = alloc_node(NODE_LITERAL_SYMBOL, p->previous.line);
                if (elem) elem->as.variable.name = token_str(&p->previous);
            }
        } else if (check(p, TOKEN_LPAREN)) {
            /* Nested literal array: #(1 (2 3) 4) — parens in inner arrays */
            advance_token(p);
            uint32_t inner_line = p->previous.line;
            PtrArray inner;
            ptr_array_init(&inner);
            while (!check(p, TOKEN_RPAREN) && !check(p, TOKEN_EOF) && !p->had_error) {
                STA_AstNode *ie = parse_literal(p);
                if (!ie) break;
                ptr_array_push(&inner, ie);
            }
            if (!consume(p, TOKEN_RPAREN, "expected ')' in nested literal array")) {
                for (uint32_t i = 0; i < inner.count; i++)
                    sta_ast_free(inner.items[i]);
                free(inner.items);
                break;
            }
            elem = alloc_node(NODE_LITERAL_ARRAY, inner_line);
            if (elem) {
                elem->as.array.elements = (STA_AstNode **)inner.items;
                elem->as.array.count = inner.count;
            }
        } else if (check(p, TOKEN_BINARY_SELECTOR)) {
            /* Binary selector as symbol in literal array: #(+ -) */
            advance_token(p);
            elem = alloc_node(NODE_LITERAL_SYMBOL, p->previous.line);
            if (elem) elem->as.variable.name = token_str(&p->previous);
        } else {
            parser_error(p, "unexpected token in literal array");
            break;
        }

        if (!elem) break;
        ptr_array_push(&elems, elem);
    }

    if (!consume(p, TOKEN_RPAREN, "expected ')' after literal array")) {
        for (uint32_t i = 0; i < elems.count; i++)
            sta_ast_free(elems.items[i]);
        free(elems.items);
        return NULL;
    }

    STA_AstNode *node = alloc_node(NODE_LITERAL_ARRAY, line);
    if (node) {
        node->as.array.elements = (STA_AstNode **)elems.items;
        node->as.array.count = elems.count;
    }
    return node;
}

static STA_AstNode *parse_literal(STA_Parser *p) {
    if (match(p, TOKEN_INTEGER)) {
        STA_AstNode *n = alloc_node(NODE_LITERAL_INT, p->previous.line);
        if (n) {
            char *s = token_str(&p->previous);
            n->as.integer.value = strtoll(s, NULL, 10);
            free(s);
        }
        return n;
    }

    if (match(p, TOKEN_FLOAT)) {
        STA_AstNode *n = alloc_node(NODE_LITERAL_FLOAT, p->previous.line);
        if (n) {
            char *s = token_str(&p->previous);
            n->as.floatval.value = strtod(s, NULL);
            free(s);
        }
        return n;
    }

    if (match(p, TOKEN_STRING)) {
        STA_AstNode *n = alloc_node(NODE_LITERAL_STRING, p->previous.line);
        if (n) {
            /* Strip quotes and unescape embedded quotes. */
            const char *src = p->previous.start + 1; /* skip opening ' */
            uint32_t src_len = p->previous.length - 2; /* skip both quotes */
            /* Count actual length (embedded '' becomes ') */
            uint32_t actual = 0;
            for (uint32_t i = 0; i < src_len; i++) {
                actual++;
                if (src[i] == '\'' && i + 1 < src_len && src[i + 1] == '\'')
                    i++; /* skip second quote */
            }
            char *buf = malloc(actual + 1);
            if (buf) {
                uint32_t j = 0;
                for (uint32_t i = 0; i < src_len; i++) {
                    buf[j++] = src[i];
                    if (src[i] == '\'' && i + 1 < src_len && src[i + 1] == '\'')
                        i++;
                }
                buf[j] = '\0';
            }
            n->as.variable.name = buf;
        }
        return n;
    }

    if (match(p, TOKEN_SYMBOL_LITERAL)) {
        STA_AstNode *n = alloc_node(NODE_LITERAL_SYMBOL, p->previous.line);
        if (n) {
            const char *src = p->previous.start;
            uint32_t len = p->previous.length;
            /* Skip leading # */
            src++; len--;
            /* If it's a string-form symbol #'...', strip quotes and unescape. */
            if (len >= 2 && src[0] == '\'') {
                src++; len -= 2; /* skip both quotes */
                uint32_t actual = 0;
                for (uint32_t i = 0; i < len; i++) {
                    actual++;
                    if (src[i] == '\'' && i + 1 < len && src[i + 1] == '\'')
                        i++;
                }
                char *buf = malloc(actual + 1);
                if (buf) {
                    uint32_t j = 0;
                    for (uint32_t i = 0; i < len; i++) {
                        buf[j++] = src[i];
                        if (src[i] == '\'' && i + 1 < len && src[i + 1] == '\'')
                            i++;
                    }
                    buf[j] = '\0';
                }
                n->as.variable.name = buf;
            } else {
                n->as.variable.name = copy_str(src, len);
            }
        }
        return n;
    }

    if (match(p, TOKEN_CHAR_LITERAL)) {
        STA_AstNode *n = alloc_node(NODE_LITERAL_CHAR, p->previous.line);
        if (n) {
            /* Token is $X — character is at position 1. */
            n->as.character.value = p->previous.start[1];
        }
        return n;
    }

    if (match(p, TOKEN_HASH_PAREN)) {
        return parse_literal_array(p);
    }

    return NULL; /* not a literal */
}

/* ── Block parsing ───────────────────────────────────────────────────── */

static STA_AstNode *parse_block(STA_Parser *p) {
    /* TOKEN_LBRACKET already consumed. */
    uint32_t line = p->previous.line;
    p->block_depth++;

    PtrArray args;
    ptr_array_init(&args);

    /* Block arguments: [:x :y | ...] */
    if (check(p, TOKEN_COLON)) {
        while (match(p, TOKEN_COLON)) {
            if (!consume(p, TOKEN_IDENTIFIER, "expected block argument name")) {
                p->block_depth--;
                for (uint32_t i = 0; i < args.count; i++) free(args.items[i]);
                free(args.items);
                return NULL;
            }
            ptr_array_push(&args, token_str(&p->previous));
        }
        if (!consume(p, TOKEN_VBAR, "expected '|' after block arguments")) {
            p->block_depth--;
            for (uint32_t i = 0; i < args.count; i++) free(args.items[i]);
            free(args.items);
            return NULL;
        }
    }

    /* Block temporaries: [ | temp1 temp2 | ...] */
    PtrArray temps;
    ptr_array_init(&temps);
    if (check(p, TOKEN_VBAR)) {
        advance_token(p);
        while (check(p, TOKEN_IDENTIFIER)) {
            advance_token(p);
            ptr_array_push(&temps, token_str(&p->previous));
        }
        if (!consume(p, TOKEN_VBAR, "expected '|' after block temporaries")) {
            p->block_depth--;
            for (uint32_t i = 0; i < args.count; i++) free(args.items[i]);
            free(args.items);
            for (uint32_t i = 0; i < temps.count; i++) free(temps.items[i]);
            free(temps.items);
            return NULL;
        }
    }

    /* Block body: statements. */
    PtrArray body;
    ptr_array_init(&body);

    while (!check(p, TOKEN_RBRACKET) && !check(p, TOKEN_EOF) && !p->had_error) {
        STA_AstNode *stmt = NULL;

        /* Check for return inside block — reject non-local return. */
        if (check(p, TOKEN_RETURN)) {
            parser_error(p, "non-local return (^) inside block is not supported in Phase 1");
            break;
        }

        stmt = parse_expression(p);
        if (!stmt) break;
        ptr_array_push(&body, stmt);

        if (!match(p, TOKEN_PERIOD)) break; /* optional trailing period */
    }

    if (!consume(p, TOKEN_RBRACKET, "expected ']' after block body")) {
        p->block_depth--;
        for (uint32_t i = 0; i < args.count; i++) free(args.items[i]);
        free(args.items);
        for (uint32_t i = 0; i < temps.count; i++) free(temps.items[i]);
        free(temps.items);
        for (uint32_t i = 0; i < body.count; i++) sta_ast_free(body.items[i]);
        free(body.items);
        return NULL;
    }

    p->block_depth--;

    STA_AstNode *node = alloc_node(NODE_BLOCK, line);
    if (node) {
        node->as.method.selector = NULL;
        node->as.method.args = (char **)args.items;
        node->as.method.arg_count = args.count;
        node->as.method.temps = (char **)temps.items;
        node->as.method.temp_count = temps.count;
        node->as.method.body = (STA_AstNode **)body.items;
        node->as.method.body_count = body.count;
    }
    return node;
}

/* ── Expression parsing (standard Smalltalk precedence) ──────────────── */

static STA_AstNode *parse_primary(STA_Parser *p) {
    /* Identifiers: self, super, thisContext, true, false, nil, or variable. */
    if (check(p, TOKEN_IDENTIFIER)) {
        advance_token(p);
        const char *txt = p->previous.start;
        uint32_t len = p->previous.length;
        uint32_t line = p->previous.line;

        if (len == 4 && memcmp(txt, "self", 4) == 0)
            return alloc_node(NODE_SELF, line);
        if (len == 5 && memcmp(txt, "super", 5) == 0) {
            STA_AstNode *n = alloc_node(NODE_VARIABLE, line);
            if (n) n->as.variable.name = copy_str("super", 5);
            return n;
        }
        if (len == 3 && memcmp(txt, "nil", 3) == 0)
            return alloc_node(NODE_LITERAL_NIL, line);
        if (len == 4 && memcmp(txt, "true", 4) == 0) {
            STA_AstNode *n = alloc_node(NODE_LITERAL_BOOL, line);
            if (n) n->as.boolean.value = true;
            return n;
        }
        if (len == 5 && memcmp(txt, "false", 5) == 0) {
            STA_AstNode *n = alloc_node(NODE_LITERAL_BOOL, line);
            if (n) n->as.boolean.value = false;
            return n;
        }
        if (len == 11 && memcmp(txt, "thisContext", 11) == 0) {
            STA_AstNode *n = alloc_node(NODE_VARIABLE, line);
            if (n) n->as.variable.name = copy_str("thisContext", 11);
            return n;
        }

        STA_AstNode *n = alloc_node(NODE_VARIABLE, line);
        if (n) n->as.variable.name = copy_str(txt, len);
        return n;
    }

    /* Literals. */
    if (check(p, TOKEN_INTEGER) || check(p, TOKEN_FLOAT) ||
        check(p, TOKEN_STRING) || check(p, TOKEN_SYMBOL_LITERAL) ||
        check(p, TOKEN_CHAR_LITERAL) || check(p, TOKEN_HASH_PAREN)) {
        return parse_literal(p);
    }

    /* Block. */
    if (match(p, TOKEN_LBRACKET)) {
        return parse_block(p);
    }

    /* Parenthesised expression. */
    if (match(p, TOKEN_LPAREN)) {
        STA_AstNode *expr = parse_expression(p);
        if (!expr) return NULL;
        if (!consume(p, TOKEN_RPAREN, "expected ')' after expression")) {
            sta_ast_free(expr);
            return NULL;
        }
        return expr;
    }

    /* Negative number literal: when we see BINARY_SELECTOR "-" followed by
     * a number, this was already handled by the scanner. But handle a bare
     * return token or unexpected. */

    parser_error(p, "expected expression");
    return NULL;
}

static STA_AstNode *parse_unary_expr(STA_Parser *p) {
    STA_AstNode *receiver = parse_primary(p);
    if (!receiver || p->had_error) return receiver;

    /* Check if receiver is "super" — affects sends. */
    bool is_super = (receiver->type == NODE_VARIABLE &&
                     receiver->as.variable.name &&
                     strcmp(receiver->as.variable.name, "super") == 0);

    while (check(p, TOKEN_IDENTIFIER)) {
        /* Make sure it's not a keyword (has colon) — that's for keyword_expr. */
        /* Also not true/false/nil/self/super — those start new expressions. */
        const char *txt = p->current.start;
        uint32_t len = p->current.length;

        /* Peek: is this identifier actually a keyword? */
        /* In the token stream, keywords are separate (TOKEN_KEYWORD). */
        /* TOKEN_IDENTIFIER here means it's a unary selector. */

        /* Check for pseudo-variables that shouldn't be unary sends. */
        if ((len == 4 && memcmp(txt, "self", 4) == 0) ||
            (len == 5 && memcmp(txt, "super", 5) == 0) ||
            (len == 3 && memcmp(txt, "nil", 3) == 0) ||
            (len == 4 && memcmp(txt, "true", 4) == 0) ||
            (len == 5 && memcmp(txt, "false", 5) == 0) ||
            (len == 11 && memcmp(txt, "thisContext", 11) == 0)) {
            break;
        }

        advance_token(p);
        uint32_t line = p->previous.line;
        char *sel = token_str(&p->previous);

        STA_NodeType send_type = is_super ? NODE_SUPER_SEND : NODE_SEND;
        STA_AstNode *send = alloc_node(send_type, line);
        if (send) {
            send->as.send.receiver = receiver;
            send->as.send.selector = sel;
            send->as.send.args = NULL;
            send->as.send.arg_count = 0;
        }
        receiver = send;
        is_super = false; /* only first send is super */
    }

    return receiver;
}

static STA_AstNode *parse_binary_expr(STA_Parser *p) {
    STA_AstNode *left = parse_unary_expr(p);
    if (!left || p->had_error) return left;

    while (check(p, TOKEN_BINARY_SELECTOR) || check(p, TOKEN_VBAR)) {
        /* TOKEN_VBAR (|) is also a valid binary selector in expression
         * context. Temp declarations are already consumed before we
         * reach expression parsing. */

        /* Check if left is a super variable for super sends. */
        bool is_super = (left->type == NODE_VARIABLE &&
                         left->as.variable.name &&
                         strcmp(left->as.variable.name, "super") == 0);

        advance_token(p);
        uint32_t line = p->previous.line;
        char *sel = token_str(&p->previous);

        STA_AstNode *right = parse_unary_expr(p);
        if (!right) {
            free(sel);
            sta_ast_free(left);
            return NULL;
        }

        STA_NodeType send_type = is_super ? NODE_SUPER_SEND : NODE_SEND;
        STA_AstNode *send = alloc_node(send_type, line);
        if (send) {
            send->as.send.receiver = left;
            send->as.send.selector = sel;
            send->as.send.args = malloc(sizeof(STA_AstNode *));
            send->as.send.args[0] = right;
            send->as.send.arg_count = 1;
        }
        left = send;
    }

    return left;
}

static STA_AstNode *parse_keyword_expr(STA_Parser *p) {
    STA_AstNode *receiver = parse_binary_expr(p);
    if (!receiver || p->had_error) return receiver;

    if (!check(p, TOKEN_KEYWORD)) return receiver;

    /* Check if receiver is "super". */
    bool is_super = (receiver->type == NODE_VARIABLE &&
                     receiver->as.variable.name &&
                     strcmp(receiver->as.variable.name, "super") == 0);

    /* Assemble keyword selector from multiple keyword tokens. */
    PtrArray args;
    ptr_array_init(&args);
    uint32_t sel_len = 0;
    uint32_t sel_cap = 64;
    char *selector = malloc(sel_cap);
    if (!selector) { sta_ast_free(receiver); return NULL; }
    selector[0] = '\0';
    uint32_t line = p->current.line;

    while (check(p, TOKEN_KEYWORD)) {
        advance_token(p);
        uint32_t kw_len = p->previous.length;
        if (sel_len + kw_len >= sel_cap) {
            sel_cap = (sel_len + kw_len) * 2;
            char *new_sel = realloc(selector, sel_cap);
            if (!new_sel) { free(selector); sta_ast_free(receiver); return NULL; }
            selector = new_sel;
        }
        memcpy(selector + sel_len, p->previous.start, kw_len);
        sel_len += kw_len;
        selector[sel_len] = '\0';

        STA_AstNode *arg = parse_binary_expr(p);
        if (!arg) {
            free(selector);
            sta_ast_free(receiver);
            for (uint32_t i = 0; i < args.count; i++)
                sta_ast_free(args.items[i]);
            free(args.items);
            return NULL;
        }
        ptr_array_push(&args, arg);
    }

    STA_NodeType send_type = is_super ? NODE_SUPER_SEND : NODE_SEND;
    STA_AstNode *send = alloc_node(send_type, line);
    if (send) {
        send->as.send.receiver = receiver;
        send->as.send.selector = selector;
        send->as.send.args = (STA_AstNode **)args.items;
        send->as.send.arg_count = args.count;
    }
    return send;
}

/* Parse a cascade: after a message send, ';' means send another message
 * to the same receiver. */
static STA_AstNode *parse_cascade_or_expr(STA_Parser *p) {
    /* First, parse the full expression up to a possible cascade. */
    /* We need to split into: receiver + first message + cascade messages.
     * The challenge is that the first message is already parsed as a send.
     * If we see ';', we restructure into a CASCADE node. */
    STA_AstNode *expr = parse_keyword_expr(p);
    if (!expr || p->had_error) return expr;

    if (!check(p, TOKEN_SEMICOLON)) return expr;

    /* expr must be a send (NODE_SEND or NODE_SUPER_SEND). */
    if (expr->type != NODE_SEND && expr->type != NODE_SUPER_SEND) {
        parser_error(p, "cascade (;) requires a message send before it");
        sta_ast_free(expr);
        return NULL;
    }

    /* Build cascade: extract receiver from first send, add first message. */
    STA_AstNode *receiver = expr->as.send.receiver;
    expr->as.send.receiver = NULL; /* detach */

    PtrArray msgs;
    ptr_array_init(&msgs);

    /* Add first message. */
    STA_CascadeMsg first;
    first.selector = expr->as.send.selector;
    first.args = expr->as.send.args;
    first.arg_count = expr->as.send.arg_count;
    expr->as.send.selector = NULL;
    expr->as.send.args = NULL;
    free(expr); /* free the send node shell only */

    /* Allocate messages array with enough space. */
    STA_CascadeMsg *msg_array = NULL;
    uint32_t msg_count = 1;
    uint32_t msg_cap = 4;
    msg_array = malloc(msg_cap * sizeof(STA_CascadeMsg));
    if (!msg_array) return NULL;
    msg_array[0] = first;

    while (match(p, TOKEN_SEMICOLON)) {
        if (p->had_error) break;

        char *sel = NULL;
        PtrArray cargs;
        ptr_array_init(&cargs);

        if (check(p, TOKEN_IDENTIFIER)) {
            /* Unary cascade message. */
            advance_token(p);
            sel = token_str(&p->previous);
        } else if (check(p, TOKEN_BINARY_SELECTOR)) {
            /* Binary cascade message. */
            advance_token(p);
            sel = token_str(&p->previous);
            STA_AstNode *arg = parse_unary_expr(p);
            if (!arg) { free(sel); break; }
            ptr_array_push(&cargs, arg);
        } else if (check(p, TOKEN_KEYWORD)) {
            /* Keyword cascade message. */
            uint32_t sl = 0, sc = 64;
            sel = malloc(sc);
            if (!sel) break;
            sel[0] = '\0';
            while (check(p, TOKEN_KEYWORD)) {
                advance_token(p);
                uint32_t kl = p->previous.length;
                if (sl + kl >= sc) {
                    sc = (sl + kl) * 2;
                    char *ns = realloc(sel, sc);
                    if (!ns) { free(sel); sel = NULL; break; }
                    sel = ns;
                }
                memcpy(sel + sl, p->previous.start, kl);
                sl += kl;
                sel[sl] = '\0';

                STA_AstNode *arg = parse_binary_expr(p);
                if (!arg) break;
                ptr_array_push(&cargs, arg);
            }
            if (!sel) break;
        } else {
            parser_error(p, "expected message after cascade ';'");
            break;
        }

        if (msg_count == msg_cap) {
            msg_cap *= 2;
            STA_CascadeMsg *new_arr = realloc(msg_array, msg_cap * sizeof(STA_CascadeMsg));
            if (!new_arr) { free(sel); break; }
            msg_array = new_arr;
        }
        msg_array[msg_count].selector = sel;
        msg_array[msg_count].args = (STA_AstNode **)cargs.items;
        msg_array[msg_count].arg_count = cargs.count;
        msg_count++;
    }

    STA_AstNode *cascade = alloc_node(NODE_CASCADE, receiver->line);
    if (cascade) {
        cascade->as.cascade.receiver = receiver;
        cascade->as.cascade.messages = msg_array;
        cascade->as.cascade.msg_count = msg_count;
    }
    return cascade;
}

static STA_AstNode *parse_expression(STA_Parser *p) {
    if (p->had_error) return NULL;

    /* Check for assignment: identifier followed by ':=' */
    if (check(p, TOKEN_IDENTIFIER)) {
        /* Peek ahead for := */
        STA_Scanner saved = p->scanner;
        STA_Token saved_current = p->current;

        advance_token(p);
        STA_Token ident = p->previous;

        if (check(p, TOKEN_ASSIGN)) {
            advance_token(p); /* consume := */
            uint32_t line = ident.line;

            STA_AstNode *value = parse_expression(p);
            if (!value) return NULL;

            STA_AstNode *var = alloc_node(NODE_VARIABLE, line);
            if (var) var->as.variable.name = token_str(&ident);

            STA_AstNode *assign = alloc_node(NODE_ASSIGN, line);
            if (assign) {
                assign->as.assign.variable = var;
                assign->as.assign.value = value;
            }
            return assign;
        }

        /* Not an assignment — restore scanner state and re-parse. */
        p->scanner = saved;
        p->current = saved_current;
    }

    return parse_cascade_or_expr(p);
}

/* ── Statement parsing ───────────────────────────────────────────────── */

static STA_AstNode *parse_statement(STA_Parser *p) {
    if (p->had_error) return NULL;

    /* Return statement. */
    if (match(p, TOKEN_RETURN)) {
        uint32_t line = p->previous.line;

        /* Check for non-local return in block. */
        if (p->block_depth > 0) {
            parser_error_at(p, line,
                "non-local return (^) inside block is not supported in Phase 1");
            return NULL;
        }

        STA_AstNode *expr = parse_expression(p);
        if (!expr) return NULL;

        STA_AstNode *ret = alloc_node(NODE_RETURN, line);
        if (ret) ret->as.ret.expr = expr;
        return ret;
    }

    return parse_expression(p);
}

/* ── Method header parsing ───────────────────────────────────────────── */

/* Returns the method node with selector and args filled in.
 * Body/temps parsed afterward. */
static STA_AstNode *parse_method_header(STA_Parser *p) {
    uint32_t line = p->current.line;

    /* Keyword method header: keyword+ identifier+ */
    if (check(p, TOKEN_KEYWORD)) {
        uint32_t sel_len = 0, sel_cap = 64;
        char *selector = malloc(sel_cap);
        if (!selector) return NULL;
        selector[0] = '\0';

        PtrArray args;
        ptr_array_init(&args);

        while (check(p, TOKEN_KEYWORD)) {
            advance_token(p);
            uint32_t kl = p->previous.length;
            if (sel_len + kl >= sel_cap) {
                sel_cap = (sel_len + kl) * 2;
                char *ns = realloc(selector, sel_cap);
                if (!ns) { free(selector); return NULL; }
                selector = ns;
            }
            memcpy(selector + sel_len, p->previous.start, kl);
            sel_len += kl;
            selector[sel_len] = '\0';

            if (!consume(p, TOKEN_IDENTIFIER, "expected argument name after keyword")) {
                free(selector);
                for (uint32_t i = 0; i < args.count; i++) free(args.items[i]);
                free(args.items);
                return NULL;
            }
            ptr_array_push(&args, token_str(&p->previous));
        }

        STA_AstNode *method = alloc_node(NODE_METHOD, line);
        if (method) {
            method->as.method.selector = selector;
            method->as.method.args = (char **)args.items;
            method->as.method.arg_count = args.count;
        }
        return method;
    }

    /* Binary method header: BINARY_SELECTOR IDENTIFIER
     * Also accept TOKEN_VBAR as a binary selector (|). */
    if (check(p, TOKEN_BINARY_SELECTOR) || check(p, TOKEN_VBAR)) {
        advance_token(p);
        char *selector = token_str(&p->previous);

        if (!consume(p, TOKEN_IDENTIFIER, "expected argument name after binary selector")) {
            free(selector);
            return NULL;
        }

        PtrArray args;
        ptr_array_init(&args);
        ptr_array_push(&args, token_str(&p->previous));

        STA_AstNode *method = alloc_node(NODE_METHOD, line);
        if (method) {
            method->as.method.selector = selector;
            method->as.method.args = (char **)args.items;
            method->as.method.arg_count = args.count;
        }
        return method;
    }

    /* Unary method header: IDENTIFIER */
    if (check(p, TOKEN_IDENTIFIER)) {
        advance_token(p);
        char *selector = token_str(&p->previous);

        STA_AstNode *method = alloc_node(NODE_METHOD, line);
        if (method) {
            method->as.method.selector = selector;
            method->as.method.args = NULL;
            method->as.method.arg_count = 0;
        }
        return method;
    }

    parser_error(p, "expected method header");
    return NULL;
}

/* ── Temporary variables ─────────────────────────────────────────────── */

static bool parse_temps(STA_Parser *p, char ***out_temps, uint32_t *out_count) {
    *out_temps = NULL;
    *out_count = 0;

    if (!check(p, TOKEN_VBAR)) return true;
    advance_token(p); /* consume | */

    PtrArray temps;
    ptr_array_init(&temps);

    while (check(p, TOKEN_IDENTIFIER)) {
        advance_token(p);
        ptr_array_push(&temps, token_str(&p->previous));
    }

    if (!consume(p, TOKEN_VBAR, "expected '|' after temporary variables")) {
        for (uint32_t i = 0; i < temps.count; i++) free(temps.items[i]);
        free(temps.items);
        return false;
    }

    *out_temps = (char **)temps.items;
    *out_count = temps.count;
    return true;
}

/* ── Public API ──────────────────────────────────────────────────────── */

STA_AstNode *sta_parse_method(const char *source, STA_Parser *parser) {
    sta_scanner_init(&parser->scanner, source);
    parser->had_error = false;
    parser->error_msg[0] = '\0';
    parser->block_depth = 0;

    /* Prime the current token. */
    parser->current = sta_scanner_next(&parser->scanner);

    /* Method header. */
    STA_AstNode *method = parse_method_header(parser);
    if (!method || parser->had_error) {
        sta_ast_free(method);
        return NULL;
    }

    /* Temporaries. */
    char **temps = NULL;
    uint32_t temp_count = 0;
    if (!parse_temps(parser, &temps, &temp_count)) {
        sta_ast_free(method);
        return NULL;
    }
    method->as.method.temps = temps;
    method->as.method.temp_count = temp_count;

    /* Body: list of statements separated by periods. */
    PtrArray body;
    ptr_array_init(&body);

    while (!check(parser, TOKEN_EOF) && !parser->had_error) {
        STA_AstNode *stmt = parse_statement(parser);
        if (!stmt) break;
        ptr_array_push(&body, stmt);

        if (!match(parser, TOKEN_PERIOD)) break;

        /* Allow trailing period before EOF. */
        if (check(parser, TOKEN_EOF)) break;
    }

    if (parser->had_error) {
        for (uint32_t i = 0; i < body.count; i++)
            sta_ast_free(body.items[i]);
        free(body.items);
        sta_ast_free(method);
        return NULL;
    }

    method->as.method.body = (STA_AstNode **)body.items;
    method->as.method.body_count = body.count;

    return method;
}

const char *sta_parser_error(const STA_Parser *parser) {
    if (parser->had_error) return parser->error_msg;
    return NULL;
}
