/* src/vm/special_selectors.h
 * Bootstrap interning of special selectors and interpreter-referenced symbols.
 * Phase 1 — permanent. See bytecode spec §10.7 and §11.4.
 *
 * Called once during bootstrap to:
 *   1. Intern all 32 special selectors from §10.7.
 *   2. Allocate an Array of 32 Symbol OOPs in immutable space.
 *   3. Store the Array at SPC_SPECIAL_SELECTORS (index 4).
 *   4. Intern and store individual symbols at SPC_DOES_NOT_UNDERSTAND (6),
 *      SPC_CANNOT_RETURN (7), SPC_MUST_BE_BOOLEAN (8), SPC_STARTUP (9),
 *      SPC_SHUTDOWN (10), SPC_RUN (11).
 */
#pragma once
#include "symbol_table.h"
#include "immutable_space.h"

/* Intern all special selectors and populate the special object table.
 * Returns 0 on success, -1 on allocation failure. */
int sta_intern_special_selectors(STA_ImmutableSpace *sp, STA_SymbolTable *st);
