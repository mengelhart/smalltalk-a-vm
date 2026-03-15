/* src/compiler/codegen.h
 * Bytecode generator — walks AST and emits CompiledMethod objects.
 * Phase 1 — permanent. See bytecode spec §5.
 *
 * Input: AST (from parser) + compilation context (class, symbol table,
 *        class table for globals, heap, immutable space).
 * Output: CompiledMethod OOP allocated in immutable space.
 */
#pragma once
#include "ast.h"
#include "../vm/oop.h"
#include "../vm/immutable_space.h"
#include "../vm/heap.h"
#include "../vm/symbol_table.h"
#include "../vm/class_table.h"

/* ── Compilation context ─────────────────────────────────────────────── */

typedef struct {
    /* Class being compiled for (instvar resolution, owner class). */
    STA_OOP          class_oop;

    /* Instance variable names for this class (NULL-terminated array).
     * Used to resolve variable references to OP_PUSH_INSTVAR indices. */
    const char     **instvar_names;
    uint32_t         instvar_count;

    /* Symbol table for interning selector symbols and literal symbols. */
    STA_SymbolTable *symbol_table;
    STA_ImmutableSpace *immutable_space;

    /* Heap for creating block descriptor objects. */
    STA_Heap        *heap;

    /* SystemDictionary OOP — for resolving global variable names
     * to Association objects in the literal frame. 0 if no globals. */
    STA_OOP          system_dict;

    /* Error output. */
    bool             had_error;
    char             error_msg[256];
} STA_CodegenContext;

/* ── API ─────────────────────────────────────────────────────────────── */

/* Generate bytecodes from a parsed method AST.
 * Returns a CompiledMethod OOP on success, 0 on failure.
 * On failure, ctx->error_msg contains the message. */
STA_OOP sta_codegen(STA_AstNode *method_ast, STA_CodegenContext *ctx);
