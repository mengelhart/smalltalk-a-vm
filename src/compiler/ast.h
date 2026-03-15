/* src/compiler/ast.h
 * AST node types for the Smalltalk method parser.
 * Phase 1 — permanent. See bytecode spec §5.
 *
 * Nodes are malloc-allocated and short-lived — they exist only during
 * compilation of a single method, then freed via sta_ast_free().
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Node types ──────────────────────────────────────────────────────── */

typedef enum {
    NODE_METHOD,
    NODE_RETURN,
    NODE_ASSIGN,
    NODE_SEND,
    NODE_SUPER_SEND,
    NODE_CASCADE,
    NODE_VARIABLE,
    NODE_LITERAL_INT,
    NODE_LITERAL_FLOAT,
    NODE_LITERAL_STRING,
    NODE_LITERAL_SYMBOL,
    NODE_LITERAL_CHAR,
    NODE_LITERAL_ARRAY,
    NODE_LITERAL_BOOL,
    NODE_LITERAL_NIL,
    NODE_BLOCK,
    NODE_SELF,
} STA_NodeType;

/* Forward declaration. */
typedef struct STA_AstNode STA_AstNode;

/* ── Cascade message entry ───────────────────────────────────────────── */

typedef struct {
    char        *selector;    /* selector string (owned)       */
    STA_AstNode **args;       /* argument nodes (owned array)  */
    uint32_t     arg_count;
} STA_CascadeMsg;

/* ── AST node ────────────────────────────────────────────────────────── */

struct STA_AstNode {
    STA_NodeType type;
    uint32_t     line;        /* source line for error reporting */

    union {
        /* NODE_METHOD */
        struct {
            char        *selector;    /* method selector (owned) */
            char       **args;        /* argument names (owned array of owned strings) */
            uint32_t     arg_count;
            char       **temps;       /* temporary names (owned array of owned strings) */
            uint32_t     temp_count;
            STA_AstNode **body;       /* statement nodes (owned array) */
            uint32_t     body_count;
        } method;

        /* NODE_RETURN */
        struct {
            STA_AstNode *expr;        /* expression to return (owned) */
        } ret;

        /* NODE_ASSIGN */
        struct {
            STA_AstNode *variable;    /* NODE_VARIABLE (owned) */
            STA_AstNode *value;       /* expression (owned) */
        } assign;

        /* NODE_SEND, NODE_SUPER_SEND */
        struct {
            STA_AstNode  *receiver;   /* receiver expression (owned) */
            char         *selector;   /* message selector (owned) */
            STA_AstNode **args;       /* argument nodes (owned array) */
            uint32_t      arg_count;
        } send;

        /* NODE_CASCADE */
        struct {
            STA_AstNode     *receiver;   /* receiver expression (owned) */
            STA_CascadeMsg  *messages;   /* array of cascade messages (owned) */
            uint32_t         msg_count;
        } cascade;

        /* NODE_VARIABLE, NODE_LITERAL_STRING, NODE_LITERAL_SYMBOL */
        struct {
            char *name;               /* variable/string/symbol name (owned) */
        } variable;

        /* NODE_LITERAL_INT */
        struct {
            int64_t value;
        } integer;

        /* NODE_LITERAL_FLOAT */
        struct {
            double value;
        } floatval;

        /* NODE_LITERAL_CHAR */
        struct {
            char value;
        } character;

        /* NODE_LITERAL_ARRAY */
        struct {
            STA_AstNode **elements;   /* literal nodes (owned array) */
            uint32_t      count;
        } array;

        /* NODE_LITERAL_BOOL */
        struct {
            bool value;
        } boolean;

        /* NODE_SELF, NODE_LITERAL_NIL — no extra data */
    } as;
};

/* ── API ─────────────────────────────────────────────────────────────── */

/* Recursively free an AST node and all children. NULL-safe. */
void sta_ast_free(STA_AstNode *node);
