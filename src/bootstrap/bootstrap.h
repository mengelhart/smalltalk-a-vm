/* src/bootstrap/bootstrap.h
 * Kernel bootstrap — creates the Smalltalk object system from scratch.
 * Phase 1 — permanent. See Epic 4 spec, Blue Book chapters 3–5.
 *
 * Runs once to create the complete metaclass hierarchy, kernel classes,
 * special objects, character table, global dictionary, and installs
 * primitive methods. After this call the interpreter can execute
 * Smalltalk expressions using real class objects with real method
 * dictionaries.
 */
#pragma once
#include "../vm/oop.h"
#include "../vm/format.h"
#include "../vm/heap.h"
#include "../vm/immutable_space.h"
#include "../vm/symbol_table.h"
#include "../vm/class_table.h"

/* ── Bootstrap result ─────────────────────────────────────────────────── */

typedef struct STA_BootstrapResult {
    int         status;   /* 0 = success, non-zero = error              */
    const char *error;    /* human-readable error on failure            */
} STA_BootstrapResult;

/* ── Bootstrap entry point ────────────────────────────────────────────── */

/* Run the full bootstrap sequence.
 * Creates the complete Smalltalk kernel object system.
 * After this call, the interpreter can execute Smalltalk expressions
 * using real class objects with real method dictionaries.
 *
 * Must be called exactly once, before any bytecode execution.
 * All parameters must be initialized but empty (no prior content).
 *
 * Calls sta_special_objects_init() and sta_primitive_table_init()
 * internally — callers must NOT call them beforehand. */
STA_BootstrapResult sta_bootstrap(
    STA_Heap *heap,
    STA_ImmutableSpace *immutable_space,
    STA_SymbolTable *symbol_table,
    STA_ClassTable *class_table);
