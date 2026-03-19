/* src/vm/compiled_method.h
 * CompiledMethod object layout, header encoding, and accessors.
 * Phase 1 — permanent. See bytecode spec §4.1–§4.9.
 *
 * A CompiledMethod is a Smalltalk object (class_index = STA_CLS_COMPILEDMETHOD, 12).
 * Lives in immutable space (write-once after compilation, per ADR 004).
 *
 * Object layout (contiguous in immutable space):
 *   STA_ObjHeader           (standard 12-byte header + 4-byte pad = 16 bytes)
 *   payload[0]              method header word (tagged SmallInt)
 *   payload[1..numLiterals] literal frame (OOP slots)
 *   remaining bytes         bytecode array (raw bytes, not OOP-containing)
 *
 * Method header bit layout (SmallInt tag in bit 0):
 *   bits 1–8:   numArgs        (8 bits, 0–255)
 *   bits 9–16:  numTemps       (8 bits, 0–255)
 *   bits 17–24: numLiterals    (8 bits, 0–255)
 *   bits 25–32: primitiveIndex (8 bits, 0–255)
 *   bit 33:     hasPrimitive   (1 bit)
 *   bit 34:     largeFrame     (1 bit)
 *   bits 35–62: reserved       (must be 0)
 */
#pragma once
#include "oop.h"
#include "immutable_space.h"
#include <stdint.h>

/* ── Method header decoding macros ─────────────────────────────────────── */

/* All macros take a tagged SmallInt OOP (the header word). */
#define STA_METHOD_NUM_ARGS(h)      ((uint8_t)(((h) >> 1) & 0xFFu))
#define STA_METHOD_NUM_TEMPS(h)     ((uint8_t)(((h) >> 9) & 0xFFu))
#define STA_METHOD_NUM_LITERALS(h)  ((uint8_t)(((h) >> 17) & 0xFFu))
#define STA_METHOD_PRIM_INDEX(h)    ((uint8_t)(((h) >> 25) & 0xFFu))
#define STA_METHOD_HAS_PRIM(h)      ((uint8_t)(((h) >> 33) & 1u))
#define STA_METHOD_LARGE_FRAME(h)   ((uint8_t)(((h) >> 34) & 1u))

/* ── Method header encoding ────────────────────────────────────────────── */

/* Build a tagged SmallInt OOP from the header fields. */
#define STA_METHOD_HEADER(nArgs, nTemps, nLits, primIdx, hasPrim, largeFr) \
    ((STA_OOP)(                                         \
        ((STA_OOP)(uint8_t)(nArgs)       <<  1) |       \
        ((STA_OOP)(uint8_t)(nTemps)      <<  9) |       \
        ((STA_OOP)(uint8_t)(nLits)       << 17) |       \
        ((STA_OOP)(uint8_t)(primIdx)     << 25) |       \
        ((STA_OOP)((hasPrim) ? 1u : 0u)  << 33) |       \
        ((STA_OOP)((largeFr) ? 1u : 0u)  << 34) |       \
        1u  /* SmallInt tag */                          \
    ))

/* ── Accessors ─────────────────────────────────────────────────────────── */

/* Return the method header word (tagged SmallInt in payload[0]). */
static inline STA_OOP sta_method_header(STA_OOP method) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)method;
    return sta_payload(h)[0];
}

/* Return a literal from the literal frame. Index is 0-based. */
static inline STA_OOP sta_method_literal(STA_OOP method, uint8_t index) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)method;
    return sta_payload(h)[1 + index];  /* payload[0] is header */
}

/* Return a pointer to the bytecode array. */
static inline const uint8_t *sta_method_bytecodes(STA_OOP method) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)method;
    STA_OOP header = sta_payload(h)[0];
    uint8_t num_lits = STA_METHOD_NUM_LITERALS(header);
    /* Bytecodes start after header word + numLiterals OOP slots. */
    return (const uint8_t *)&sta_payload(h)[1 + num_lits];
}

/* Return the number of bytecode bytes. */
static inline uint32_t sta_method_bytecode_count(STA_OOP method) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)method;
    STA_OOP header = sta_payload(h)[0];
    uint8_t num_lits = STA_METHOD_NUM_LITERALS(header);
    /* Total payload words minus header word minus literal slots,
     * converted from OOP-sized words to bytes. */
    uint32_t oop_slots_used = 1 + (uint32_t)num_lits;
    uint32_t remaining_words = h->size - oop_slots_used;
    return remaining_words * (uint32_t)sizeof(STA_OOP);
}

/* ── Builder (test/bootstrap use only) ─────────────────────────────────── */

/* Create a CompiledMethod in immutable space.
 * Sets hasPrimitive = (primIndex != 0). largeFrame = 0.
 * Returns 0 on allocation failure. */
STA_OOP sta_compiled_method_create(STA_ImmutableSpace *sp,
    uint8_t numArgs, uint8_t numTemps, uint8_t primIndex,
    const STA_OOP *literals, uint8_t numLiterals,
    const uint8_t *bytecodes, uint32_t numBytecodes);

/* Create a CompiledMethod with an explicit header word.
 * Used when the codegen needs to set specific header flags (e.g.,
 * largeFrame/needsContext). Returns 0 on allocation failure. */
STA_OOP sta_compiled_method_create_with_header(STA_ImmutableSpace *sp,
    STA_OOP header,
    const STA_OOP *literals, uint8_t numLiterals,
    const uint8_t *bytecodes, uint32_t numBytecodes);
