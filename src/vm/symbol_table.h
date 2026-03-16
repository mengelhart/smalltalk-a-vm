/* src/vm/symbol_table.h
 * Symbol object layout, FNV-1a hash, and symbol interning table.
 * Phase 1 — permanent. See ADR 012 (image format, FNV-1a hash).
 *
 * Symbol object layout (class_index = STA_CLS_SYMBOL, 7):
 *   Payload slot 0: precomputed FNV-1a hash (stored as STA_OOP-sized value)
 *   Slots 1+: raw UTF-8 bytes, packed, NUL-terminated for C convenience
 *   header.size = 1 + ceil((byte_length + 1) / 8)
 *     i.e. hash word + enough 8-byte words to hold bytes + NUL terminator
 *
 * Concurrency (Phase 1 → Phase 2 contract):
 *   Phase 1 is single-threaded. The lookup path is designed for lock-free
 *   reads (no mid-resize observation by readers). In Phase 2 the write path
 *   will acquire a lock; the backing array pointer will be swapped atomically
 *   so readers never see a torn state. Do not add locks in Phase 1.
 */
#pragma once
#include "oop.h"
#include <stddef.h>
#include <stdint.h>

/* ── Symbol table (opaque) ─────────────────────────────────────────────── */
typedef struct STA_SymbolTable STA_SymbolTable;

/* Create a symbol table with the given initial capacity (number of slots).
 * Returns NULL on allocation failure. */
STA_SymbolTable *sta_symbol_table_create(uint32_t initial_capacity);

/* Destroy the symbol table and free its backing memory.
 * Does NOT free the Symbol objects themselves (they live in immutable space). */
void sta_symbol_table_destroy(STA_SymbolTable *st);

/* ── FNV-1a hash ───────────────────────────────────────────────────────── */

/* 32-bit FNV-1a hash. Same algorithm as ADR 012 image format.
 * Offset basis: 2166136261, prime: 16777619. */
uint32_t sta_symbol_hash(const char *utf8, size_t len);

/* ── Interning ─────────────────────────────────────────────────────────── */

/* Forward-declare STA_ImmutableSpace (defined in immutable_space.h). */
typedef struct STA_ImmutableSpace STA_ImmutableSpace;

/* Intern a symbol. Idempotent — returns the canonical Symbol OOP for the
 * given bytes. Allocates in immutable space on first intern.
 * Returns 0 on allocation failure. */
STA_OOP sta_symbol_intern(STA_ImmutableSpace *sp, STA_SymbolTable *st,
                          const char *utf8, size_t len);

/* Look up a symbol without allocating. Returns 0 if not found. */
STA_OOP sta_symbol_lookup(const STA_SymbolTable *st,
                          const char *utf8, size_t len);

/* Register an already-allocated Symbol object into the hash table index.
 * Used by the image loader to rebuild the symbol table after loading
 * Symbol objects from an image file. The Symbol must already exist in
 * immutable space with correct hash and bytes.
 * Returns 0 on success, -1 on allocation failure (table growth). */
int sta_symbol_table_register(STA_SymbolTable *st, STA_OOP symbol);

/* ── Symbol accessors ──────────────────────────────────────────────────── */

/* Extract the precomputed FNV-1a hash from a Symbol object (payload slot 0). */
uint32_t sta_symbol_get_hash(STA_OOP symbol);

/* Return a pointer to the UTF-8 bytes inside the Symbol object.
 * Sets *out_len to the byte length (excluding NUL terminator) if non-NULL. */
const char *sta_symbol_get_bytes(STA_OOP symbol, size_t *out_len);
