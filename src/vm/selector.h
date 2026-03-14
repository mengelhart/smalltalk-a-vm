/* src/vm/selector.h
 * Selector arity helper — determines argument count from a selector symbol.
 * Phase 1 — permanent. See bytecode spec §4.10.
 *
 * Binary detection: first byte is a special character.
 * Keyword detection: symbol contains ':'.
 * Unary: neither.
 */
#pragma once
#include "oop.h"
#include "symbol_table.h"
#include <stdint.h>

/* Return the arity (number of arguments) of a selector symbol.
 *   Unary:   0
 *   Binary:  1
 *   Keyword: number of colons */
uint8_t sta_selector_arity(STA_OOP selector);
