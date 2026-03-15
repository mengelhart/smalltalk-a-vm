/* src/vm/format.h
 * Class format field encoding and decoding.
 * Phase 1 — permanent. See bytecode spec §11.2 (kernel class hierarchy).
 *
 * The format field is stored as a SmallInt OOP in class slot 2.
 * Bits 0-7: instVarCount (0–255), bits 8-10: formatType (0–7).
 */
#pragma once
#include "oop.h"
#include <stdbool.h>

/* ── Format types — object shape ─────────────────────────────────────── */

#define STA_FMT_NORMAL          0u  /* Fixed-size, all OOP slots           */
#define STA_FMT_VARIABLE_OOP    1u  /* Indexable OOP slots (Array-like)    */
#define STA_FMT_VARIABLE_BYTE   2u  /* Indexable byte slots (String-like)  */
#define STA_FMT_VARIABLE_WORD   3u  /* Indexable word slots (future use)   */
#define STA_FMT_WEAK            4u  /* Weak references (future use)        */
#define STA_FMT_IMMEDIATE       5u  /* Tagged immediate, no heap instances */
#define STA_FMT_COMPILED_METHOD 6u  /* Special CompiledMethod layout       */

/* ── Encode / decode ─────────────────────────────────────────────────── */

/* Encode instVarCount (0-255) and formatType (0-7) into a SmallInt OOP.
 * Bits 0-7: instVarCount, bits 8-10: formatType. */
#define STA_FORMAT_ENCODE(instVars, fmtType) \
    STA_SMALLINT_OOP((intptr_t)(((uint32_t)(fmtType) << 8) | (uint32_t)(instVars)))

/* Decode helpers. */
#define STA_FORMAT_INST_VARS(fmt) ((uint8_t)(STA_SMALLINT_VAL(fmt) & 0xFFu))
#define STA_FORMAT_TYPE(fmt)      ((uint8_t)((STA_SMALLINT_VAL(fmt) >> 8) & 0x7u))

/* ── Query helpers ───────────────────────────────────────────────────── */

/* True if the class has variable-size indexable slots (Array, String, etc.). */
static inline bool sta_format_is_indexable(STA_OOP fmt) {
    uint8_t t = STA_FORMAT_TYPE(fmt);
    return t == STA_FMT_VARIABLE_OOP  || t == STA_FMT_VARIABLE_BYTE
        || t == STA_FMT_VARIABLE_WORD || t == STA_FMT_WEAK;
}

/* True if indexable slots are byte-sized (ByteArray, String). */
static inline bool sta_format_is_bytes(STA_OOP fmt) {
    return STA_FORMAT_TYPE(fmt) == STA_FMT_VARIABLE_BYTE;
}

/* True if instances can be heap-allocated via basicNew / basicNew:.
 * False for tagged immediates (SmallInteger, Float, Character) and
 * CompiledMethod (which has a special creation path). */
static inline bool sta_format_is_instantiable(STA_OOP fmt) {
    uint8_t t = STA_FORMAT_TYPE(fmt);
    return t != STA_FMT_IMMEDIATE && t != STA_FMT_COMPILED_METHOD;
}
