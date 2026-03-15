/* src/vm/primitive_table.h
 * Primitive function table — Phase 1 kernel primitives.
 * Phase 1 — permanent. See bytecode spec §8.
 *
 * Primitive function signature:
 *   int fn(STA_OOP *args, uint8_t nargs, STA_OOP *result)
 *   args[0] = receiver, args[1..nargs] = arguments.
 *   Returns 0 on success (result written), non-zero failure code.
 *
 * Failure codes (§8.1):
 *   0 = success
 *   1 = receiver wrong type
 *   2 = argument wrong type
 *   3 = argument out of range
 *   4 = insufficient memory
 *   5 = primitive not available
 */
#pragma once
#include "oop.h"
#include "heap.h"
#include "class_table.h"
#include "frame.h"
#include "symbol_table.h"
#include "immutable_space.h"
#include <stdint.h>

/* ── Primitive function pointer type ───────────────────────────────────── */

typedef int (*STA_PrimFn)(STA_OOP *args, uint8_t nargs, STA_OOP *result);

/* ── Primitive table (256 entries for header-addressable range) ─────────── */

#define STA_PRIM_TABLE_SIZE 256u

extern STA_PrimFn sta_primitives[STA_PRIM_TABLE_SIZE];

/* ── Initialization ────────────────────────────────────────────────────── */

/* Fill table with NULL, then register Phase 1 kernel primitives.
 * Must be called before the first bytecode executes. */
void sta_primitive_table_init(void);

/* ── Failure codes ─────────────────────────────────────────────────────── */

#define STA_PRIM_SUCCESS          0
#define STA_PRIM_BAD_RECEIVER     1
#define STA_PRIM_BAD_ARGUMENT     2
#define STA_PRIM_OUT_OF_RANGE     3
#define STA_PRIM_NO_MEMORY        4
#define STA_PRIM_NOT_AVAILABLE    5

/* ── Class table context for primitives ───────────────────────────────── */

/* Set the class table used by primitives that need class lookup
 * (e.g. #class, #respondsTo:). Called by bootstrap. */
void sta_primitive_set_class_table(STA_ClassTable *ct);

/* Set the heap used by allocation primitives (e.g. #basicNew, #basicNew:).
 * Called by bootstrap after heap is ready. */
void sta_primitive_set_heap(STA_Heap *heap);

/* Set the stack slab used by exception primitives (on:do:, ensure:).
 * Called by the interpreter at dispatch loop entry. */
void sta_primitive_set_slab(STA_StackSlab *slab);

/* Set the symbol table used by class-creation primitive.
 * Called by bootstrap after symbol table is ready. */
void sta_primitive_set_symbol_table(STA_SymbolTable *st);

/* Set the immutable space used by class-creation primitive.
 * Called by bootstrap after immutable space is ready. */
void sta_primitive_set_immutable_space(STA_ImmutableSpace *sp);
