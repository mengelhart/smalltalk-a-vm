/* src/vm/special_objects.h
 * Special object table — 32-entry OOP array.
 * Phase 1 — permanent. See bytecode spec section 11.1.
 *
 * The interpreter and runtime reference well-known objects (nil, true, false,
 * Smalltalk dictionary, special selectors, etc.) by constant index into this
 * table. The table is populated during bootstrap and never resized.
 *
 * All 32 entries must be populated before the first bytecode executes.
 */
#pragma once
#include "oop.h"
#include <stdint.h>

/* ── Table size ────────────────────────────────────────────────────────── */
#define STA_SPECIAL_OBJECTS_COUNT  32u

/* ── Index constants (bytecode spec section 11.1) ──────────────────────── */
#define SPC_NIL                    0u  /* nil — unique UndefinedObject       */
#define SPC_TRUE                   1u  /* true — unique True                 */
#define SPC_FALSE                  2u  /* false — unique False               */
#define SPC_SMALLTALK              3u  /* Smalltalk — SystemDictionary       */
#define SPC_SPECIAL_SELECTORS      4u  /* Array of special selectors         */
#define SPC_CHARACTER_TABLE        5u  /* Array of 256 ASCII Characters      */
#define SPC_DOES_NOT_UNDERSTAND    6u  /* #doesNotUnderstand: symbol          */
#define SPC_CANNOT_RETURN          7u  /* #cannotReturn: symbol               */
#define SPC_MUST_BE_BOOLEAN        8u  /* #mustBeBoolean symbol               */
#define SPC_STARTUP                9u  /* #startUp symbol                     */
#define SPC_SHUTDOWN              10u  /* #shutDown symbol                    */
#define SPC_RUN                   11u  /* #run symbol                         */
#define SPC_CHILD_FAILED_REASON   12u  /* #childFailed:reason: symbol (Epic 6) */
/* Indices 13–31: reserved for future well-known objects, initialized to 0. */

/* ── The table ─────────────────────────────────────────────────────────── */

/* Global pointer to the active VM's specials array.
 * In Phase 2 this pointer is set to &vm->specials[0] at VM creation.
 * All callers use sta_spc_get/set which dereference this pointer. */
extern STA_OOP *sta_special_objects;

/* ── Bootstrap API ─────────────────────────────────────────────────────── */

/* Initialize all 32 entries to 0 (null OOP). Call once at bootstrap start.
 * If sta_special_objects points to a VM specials array, that array is zeroed.
 * If sta_special_objects is NULL, uses the internal fallback array. */
void sta_special_objects_init(void);

/* Bind the global pointer to a VM's specials array. */
void sta_special_objects_bind(STA_OOP *specials_array);

/* Set the special object at index. idx must be < STA_SPECIAL_OBJECTS_COUNT. */
static inline void sta_spc_set(uint32_t idx, STA_OOP oop) {
    sta_special_objects[idx] = oop;
}

/* Get the special object at index. Returns 0 for uninitialized entries. */
static inline STA_OOP sta_spc_get(uint32_t idx) {
    return sta_special_objects[idx];
}
