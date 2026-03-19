/* src/vm/symbol_table.c
 * Symbol interning and FNV-1a hash — see symbol_table.h for documentation.
 */
#include "symbol_table.h"
#include "immutable_space.h"
#include "class_table.h"
#include <stdlib.h>
#include <string.h>

/* ── FNV-1a (32-bit) ───────────────────────────────────────────────────── */

#define FNV_OFFSET_BASIS  2166136261u
#define FNV_PRIME         16777619u

uint32_t sta_symbol_hash(const char *utf8, size_t len) {
    uint32_t h = FNV_OFFSET_BASIS;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)utf8[i];
        h *= FNV_PRIME;
    }
    return h;
}

/* ── Table lifecycle ───────────────────────────────────────────────────── */

static uint32_t round_up_pow2(uint32_t v) {
    if (v == 0) return 16;
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;
    return v;
}

int sta_symbol_table_init(STA_SymbolTable *st, uint32_t initial_capacity) {
    uint32_t cap = round_up_pow2(initial_capacity);
    st->slots = calloc(cap, sizeof(STA_OOP));
    if (!st->slots) return -1;
    st->capacity = cap;
    st->count    = 0;
    return 0;
}

void sta_symbol_table_deinit(STA_SymbolTable *st) {
    if (!st) return;
    free(st->slots);
    st->slots = NULL;
    st->capacity = 0;
    st->count = 0;
}

STA_SymbolTable *sta_symbol_table_create(uint32_t initial_capacity) {
    STA_SymbolTable *st = malloc(sizeof(*st));
    if (!st) return NULL;

    if (sta_symbol_table_init(st, initial_capacity) != 0) {
        free(st);
        return NULL;
    }
    return st;
}

void sta_symbol_table_destroy(STA_SymbolTable *st) {
    if (!st) return;
    sta_symbol_table_deinit(st);
    free(st);
}

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* Compare the bytes of an existing Symbol object against a candidate string. */
static int symbol_bytes_equal(STA_OOP sym, const char *utf8, size_t len) {
    size_t sym_len;
    const char *sym_bytes = sta_symbol_get_bytes(sym, &sym_len);
    if (sym_len != len) return 0;
    return memcmp(sym_bytes, utf8, len) == 0;
}

/* Probe the table for a matching symbol. Returns the slot index.
 * If found, slots[index] is the symbol OOP.
 * If not found, slots[index] is 0 (the first empty slot). */
static uint32_t probe(const STA_SymbolTable *st,
                      const char *utf8, size_t len, uint32_t hash) {
    uint32_t mask = st->capacity - 1;
    uint32_t idx  = hash & mask;

    for (;;) {
        STA_OOP slot = st->slots[idx];
        if (slot == 0) return idx;                /* empty — not found       */
        if (symbol_bytes_equal(slot, utf8, len))  /* match — found           */
            return idx;
        idx = (idx + 1) & mask;                   /* linear probe            */
    }
}

/* Grow the table by doubling capacity and rehashing all entries. */
static int table_grow(STA_SymbolTable *st) {
    uint32_t new_cap = st->capacity * 2;
    STA_OOP *new_slots = calloc(new_cap, sizeof(STA_OOP));
    if (!new_slots) return -1;

    uint32_t new_mask = new_cap - 1;

    for (uint32_t i = 0; i < st->capacity; i++) {
        STA_OOP sym = st->slots[i];
        if (sym == 0) continue;

        uint32_t h   = sta_symbol_get_hash(sym);
        uint32_t idx = h & new_mask;
        while (new_slots[idx] != 0)
            idx = (idx + 1) & new_mask;
        new_slots[idx] = sym;
    }

    free(st->slots);
    st->slots    = new_slots;
    st->capacity = new_cap;
    return 0;
}

/* Allocate a Symbol object in immutable space.
 * Layout: slot 0 = hash, slots 1+ = packed UTF-8 bytes (NUL-terminated). */
static STA_OOP alloc_symbol(STA_ImmutableSpace *sp,
                            const char *utf8, size_t len, uint32_t hash) {
    /* Number of 8-byte words needed for bytes + NUL terminator.
     * For empty symbols (len=0), allocate 0 byte words so that
     * byte-aware size (prim 53) correctly returns 0. */
    uint32_t byte_words = (len == 0) ? 0 : (uint32_t)((len + 1 + 7) / 8);
    uint32_t nwords     = 1 + byte_words;   /* slot 0 = hash */

    STA_ObjHeader *h = sta_immutable_alloc(sp, STA_CLS_SYMBOL, nwords);
    if (!h) return 0;

    /* Set byte padding so size primitive returns the character count.
     * padding = (byte_words * 8) - len. */
    h->reserved = (uint16_t)((byte_words * 8 - len) & STA_BYTE_PADDING_MASK);

    STA_OOP *payload = sta_payload(h);

    /* Slot 0: precomputed hash (stored in the low 32 bits of an OOP word). */
    payload[0] = (STA_OOP)hash;

    /* Slots 1+: raw bytes. Payload was zeroed by sta_immutable_alloc,
     * so the NUL terminator and trailing padding are already zero. */
    memcpy(&payload[1], utf8, len);

    return (STA_OOP)(uintptr_t)h;
}

/* ── Public API ────────────────────────────────────────────────────────── */

STA_OOP sta_symbol_lookup(const STA_SymbolTable *st,
                          const char *utf8, size_t len) {
    uint32_t hash = sta_symbol_hash(utf8, len);
    uint32_t idx  = probe(st, utf8, len, hash);
    return st->slots[idx];
}

STA_OOP sta_symbol_intern(STA_ImmutableSpace *sp, STA_SymbolTable *st,
                          const char *utf8, size_t len) {
    uint32_t hash = sta_symbol_hash(utf8, len);
    uint32_t idx  = probe(st, utf8, len, hash);

    /* Already interned — return canonical OOP. */
    if (st->slots[idx] != 0) return st->slots[idx];

    /* Grow if load factor would exceed 70%. */
    if ((st->count + 1) * 10 > st->capacity * 7) {
        if (table_grow(st) != 0) return 0;
        /* Re-probe after growth (slot indices changed). */
        idx = probe(st, utf8, len, hash);
    }

    STA_OOP sym = alloc_symbol(sp, utf8, len, hash);
    if (sym == 0) return 0;

    st->slots[idx] = sym;
    st->count++;
    return sym;
}

int sta_symbol_table_register(STA_SymbolTable *st, STA_OOP symbol) {
    size_t len;
    const char *bytes = sta_symbol_get_bytes(symbol, &len);
    uint32_t hash = sta_symbol_get_hash(symbol);

    uint32_t idx = probe(st, bytes, len, hash);
    if (st->slots[idx] != 0) return 0;  /* already registered */

    if ((st->count + 1) * 10 > st->capacity * 7) {
        if (table_grow(st) != 0) return -1;
        idx = probe(st, bytes, len, hash);
    }

    st->slots[idx] = symbol;
    st->count++;
    return 0;
}

uint32_t sta_symbol_get_hash(STA_OOP symbol) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)symbol;
    STA_OOP *payload = sta_payload(h);
    return (uint32_t)payload[0];
}

const char *sta_symbol_get_bytes(STA_OOP symbol, size_t *out_len) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)symbol;
    STA_OOP *payload = sta_payload(h);
    const char *bytes = (const char *)&payload[1];
    if (out_len) {
        *out_len = strlen(bytes);
    }
    return bytes;
}
