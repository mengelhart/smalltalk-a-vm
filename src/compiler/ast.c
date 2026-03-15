/* src/compiler/ast.c
 * AST node memory management — see ast.h for documentation.
 */
#include "ast.h"
#include <stdlib.h>

void sta_ast_free(STA_AstNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_METHOD:
            free(node->as.method.selector);
            for (uint32_t i = 0; i < node->as.method.arg_count; i++)
                free(node->as.method.args[i]);
            free(node->as.method.args);
            for (uint32_t i = 0; i < node->as.method.temp_count; i++)
                free(node->as.method.temps[i]);
            free(node->as.method.temps);
            for (uint32_t i = 0; i < node->as.method.body_count; i++)
                sta_ast_free(node->as.method.body[i]);
            free(node->as.method.body);
            break;

        case NODE_RETURN:
            sta_ast_free(node->as.ret.expr);
            break;

        case NODE_ASSIGN:
            sta_ast_free(node->as.assign.variable);
            sta_ast_free(node->as.assign.value);
            break;

        case NODE_SEND:
        case NODE_SUPER_SEND:
            sta_ast_free(node->as.send.receiver);
            free(node->as.send.selector);
            for (uint32_t i = 0; i < node->as.send.arg_count; i++)
                sta_ast_free(node->as.send.args[i]);
            free(node->as.send.args);
            break;

        case NODE_CASCADE:
            sta_ast_free(node->as.cascade.receiver);
            for (uint32_t i = 0; i < node->as.cascade.msg_count; i++) {
                STA_CascadeMsg *m = &node->as.cascade.messages[i];
                free(m->selector);
                for (uint32_t j = 0; j < m->arg_count; j++)
                    sta_ast_free(m->args[j]);
                free(m->args);
            }
            free(node->as.cascade.messages);
            break;

        case NODE_VARIABLE:
        case NODE_LITERAL_STRING:
        case NODE_LITERAL_SYMBOL:
            free(node->as.variable.name);
            break;

        case NODE_LITERAL_ARRAY:
            for (uint32_t i = 0; i < node->as.array.count; i++)
                sta_ast_free(node->as.array.elements[i]);
            free(node->as.array.elements);
            break;

        case NODE_BLOCK: {
            /* Block reuses method struct layout for args/temps/body. */
            for (uint32_t i = 0; i < node->as.method.arg_count; i++)
                free(node->as.method.args[i]);
            free(node->as.method.args);
            for (uint32_t i = 0; i < node->as.method.temp_count; i++)
                free(node->as.method.temps[i]);
            free(node->as.method.temps);
            for (uint32_t i = 0; i < node->as.method.body_count; i++)
                sta_ast_free(node->as.method.body[i]);
            free(node->as.method.body);
            break;
        }

        case NODE_LITERAL_INT:
        case NODE_LITERAL_FLOAT:
        case NODE_LITERAL_CHAR:
        case NODE_LITERAL_BOOL:
        case NODE_LITERAL_NIL:
        case NODE_SELF:
            /* No owned memory. */
            break;
    }

    free(node);
}
