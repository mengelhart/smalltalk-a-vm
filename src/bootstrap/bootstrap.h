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
#include "../vm/heap.h"
#include "../vm/immutable_space.h"
#include "../vm/symbol_table.h"
#include "../vm/class_table.h"

/* ── Format field encoding (class slot 2) ─────────────────────────────── */

/* Format types — object shape. */
#define STA_FMT_NORMAL          0u  /* Fixed-size, all OOP slots           */
#define STA_FMT_VARIABLE_OOP    1u  /* Indexable OOP slots (Array-like)    */
#define STA_FMT_VARIABLE_BYTE   2u  /* Indexable byte slots (String-like)  */
#define STA_FMT_VARIABLE_WORD   3u  /* Indexable word slots (future use)   */
#define STA_FMT_WEAK            4u  /* Weak references (future use)        */
#define STA_FMT_IMMEDIATE       5u  /* Tagged immediate, no heap instances */
#define STA_FMT_COMPILED_METHOD 6u  /* Special CompiledMethod layout       */

/* Encode instVarCount (0-255) and formatType (0-7) into a SmallInt OOP.
 * Bits 0-7: instVarCount, bits 8-10: formatType. */
#define STA_FORMAT_ENCODE(instVars, fmtType) \
    STA_SMALLINT_OOP((intptr_t)(((uint32_t)(fmtType) << 8) | (uint32_t)(instVars)))

/* Decode helpers. */
#define STA_FORMAT_INST_VARS(fmt) ((uint8_t)(STA_SMALLINT_VAL(fmt) & 0xFFu))
#define STA_FORMAT_TYPE(fmt)      ((uint8_t)((STA_SMALLINT_VAL(fmt) >> 8) & 0x7u))

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
