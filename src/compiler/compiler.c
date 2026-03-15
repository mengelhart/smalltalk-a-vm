/* src/compiler/compiler.c
 * Top-level compiler — see compiler.h for documentation.
 */
#include "compiler.h"
#include "parser.h"
#include "codegen.h"
#include "../vm/special_objects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

STA_CompileResult sta_compile_method(
    const char *source,
    STA_OOP class_oop,
    const char **instvar_names,
    uint32_t instvar_count,
    STA_SymbolTable *symbol_table,
    STA_ImmutableSpace *immutable_space,
    STA_Heap *heap,
    STA_OOP system_dict)
{
    STA_CompileResult result;
    memset(&result, 0, sizeof(result));

    /* Step 1: Parse. */
    STA_Parser parser;
    STA_AstNode *ast = sta_parse_method(source, &parser);
    if (!ast) {
        result.had_error = true;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "parse error: %s",
                 parser.had_error ? parser.error_msg : "unknown");
        return result;
    }

    /* Step 2: Codegen. */
    STA_CodegenContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.class_oop = class_oop;
    ctx.instvar_names = instvar_names;
    ctx.instvar_count = instvar_count;
    ctx.symbol_table = symbol_table;
    ctx.immutable_space = immutable_space;
    ctx.heap = heap;
    ctx.system_dict = system_dict;
    ctx.had_error = false;

    STA_OOP method = sta_codegen(ast, &ctx);

    sta_ast_free(ast);

    if (method == 0 || ctx.had_error) {
        result.had_error = true;
        snprintf(result.error_msg, sizeof(result.error_msg),
                 "codegen error: %s", ctx.error_msg);
        return result;
    }

    result.method = method;
    return result;
}

STA_CompileResult sta_compile_expression(
    const char *source,
    STA_SymbolTable *symbol_table,
    STA_ImmutableSpace *immutable_space,
    STA_Heap *heap,
    STA_OOP system_dict)
{
    /* Wrap expression in a doIt method.
     * Single expression:        doIt ^<source>
     * Multi-statement:          doIt <stmts>. ^<last>
     * With temps (| x y | ...): doIt | x y | <stmts>. ^<last>
     */
    size_t src_len = strlen(source);

    /* Find the last '.' that is NOT inside a string literal or block. */
    int last_dot = -1;
    int depth = 0;       /* nesting depth for [] */
    bool in_string = false;
    for (size_t i = 0; i < src_len; i++) {
        char c = source[i];
        if (in_string) {
            if (c == '\'' && i + 1 < src_len && source[i + 1] == '\'') {
                i++; /* skip escaped quote */
            } else if (c == '\'') {
                in_string = false;
            }
        } else {
            if (c == '\'') {
                in_string = true;
            } else if (c == '[') {
                depth++;
            } else if (c == ']') {
                depth--;
            } else if (c == '.' && depth == 0) {
                last_dot = (int)i;
            }
        }
    }

    /* "doIt " (5) + source + "^" (1) + NUL */
    size_t buf_len = 5 + src_len + 2;
    char *buf = malloc(buf_len);
    if (!buf) {
        STA_CompileResult result;
        memset(&result, 0, sizeof(result));
        result.had_error = true;
        snprintf(result.error_msg, sizeof(result.error_msg), "out of memory");
        return result;
    }

    if (last_dot < 0) {
        /* Single expression — simple case. */
        snprintf(buf, buf_len, "doIt ^%s", source);
    } else {
        /* Multi-statement: copy up to and including last '.', then ^rest. */
        /* Skip trailing whitespace after last dot. */
        int rest_start = last_dot + 1;
        while (rest_start < (int)src_len && source[rest_start] == ' ')
            rest_start++;
        snprintf(buf, buf_len, "doIt %.*s ^%s",
                 last_dot + 1, source, source + rest_start);
    }

    /* Use Object as the class (no instvars). */
    STA_CompileResult result = sta_compile_method(
        buf,
        0, /* no class */
        NULL, 0,
        symbol_table, immutable_space, heap, system_dict);

    free(buf);
    return result;
}
