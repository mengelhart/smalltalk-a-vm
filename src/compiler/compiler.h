/* src/compiler/compiler.h
 * Top-level Smalltalk method compiler.
 * Phase 1 — permanent. See bytecode spec §5.
 *
 * Single entry point that chains: scanner → parser → codegen → CompiledMethod.
 */
#pragma once
#include <stdbool.h>
#include "../vm/oop.h"
#include "../vm/immutable_space.h"
#include "../vm/heap.h"
#include "../vm/symbol_table.h"
#include "../vm/class_table.h"

/* ── Compiler result ─────────────────────────────────────────────────── */

typedef struct {
    STA_OOP     method;       /* CompiledMethod OOP, or 0 on failure */
    bool        had_error;
    char        error_msg[512];
} STA_CompileResult;

/* ── API ─────────────────────────────────────────────────────────────── */

/* Compile a method from source text.
 *
 * source:          Full method source (header + body), e.g. "foo: x ^x + 1"
 * class_oop:       Class this method belongs to (for instvar resolution, owner class)
 * instvar_names:   Array of instance variable names (may be NULL if none)
 * instvar_count:   Number of instance variables
 * symbol_table:    Symbol table for interning
 * immutable_space: Immutable space for CompiledMethod allocation
 * heap:            Heap for temporary objects (block descriptors, arrays)
 * system_dict:     SystemDictionary OOP for global resolution (0 if none)
 *
 * Returns result with method OOP on success, error message on failure. */
STA_CompileResult sta_compile_method(
    const char *source,
    STA_OOP class_oop,
    const char **instvar_names,
    uint32_t instvar_count,
    STA_SymbolTable *symbol_table,
    STA_ImmutableSpace *immutable_space,
    STA_Heap *heap,
    STA_OOP system_dict);

/* Compile a workspace expression.
 * Wraps the expression in a synthetic "doIt ^<expression>" method. */
STA_CompileResult sta_compile_expression(
    const char *source,
    STA_SymbolTable *symbol_table,
    STA_ImmutableSpace *immutable_space,
    STA_Heap *heap,
    STA_OOP system_dict);
