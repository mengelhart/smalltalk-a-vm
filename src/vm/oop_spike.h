/* src/vm/oop_spike.h
 * Phase 0 spike: OOP tagging and object header layout.
 * NOT the permanent implementation — see ADR 007 for decisions.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── OOP ──────────────────────────────────────────────────── */
typedef uintptr_t STA_OOP;

/* Tag scheme — bit 0 discriminates SmallInt from heap pointer.
 * Heap pointers are always 8-byte aligned (low 3 bits zero).
 * SmallInt: bit 0 = 1, value = bits 63:1 (63-bit signed).       */
#define STA_IS_SMALLINT(oop)    ((oop) & 1u)
#define STA_IS_HEAP(oop)        (!STA_IS_SMALLINT(oop))
#define STA_SMALLINT_VAL(oop)   ((intptr_t)(oop) >> 1)
#define STA_SMALLINT_OOP(n)     (((STA_OOP)(uintptr_t)(intptr_t)(n) << 1) | 1u)

/* Well-known immediate constants.
 * nil/true/false are heap objects in the shared immutable region.
 * Their addresses are stored in a VM-global table; no special tags. */
#define STA_OOP_ZERO     STA_SMALLINT_OOP(0)

/* ── ObjHeader ───────────────────────────────────────────── */

/* gc_flags bits */
#define STA_GC_WHITE        0x00u   /* not yet visited */
#define STA_GC_GREY         0x01u   /* discovered, not yet scanned */
#define STA_GC_BLACK        0x02u   /* fully scanned */
#define STA_GC_COLOR_MASK   0x03u
#define STA_GC_FORWARDED    0x04u   /* object copied; header IS new address */
#define STA_GC_REMEMBERED   0x08u   /* old-space obj refs young-space obj */
#define STA_GC_RESERVED     0xF0u   /* must be zero */

/* obj_flags bits */
#define STA_OBJ_IMMUTABLE   0x01u   /* writes to slots are a runtime error */
#define STA_OBJ_PINNED      0x02u   /* must not be moved by compacting GC */
#define STA_OBJ_ACTOR_LOCAL 0x04u   /* must not escape this actor's heap */
#define STA_OBJ_SHARED_IMM  0x08u   /* lives in shared immutable region */
#define STA_OBJ_FINALIZE    0x10u   /* registered for finalization */
#define STA_OBJ_RESERVED    0xE0u   /* must be zero */

typedef struct {
    uint32_t class_index;  /* index into global class table */
    uint32_t size;         /* payload size in OOP-sized words (8 bytes each) */
    uint8_t  gc_flags;
    uint8_t  obj_flags;
    uint16_t reserved;     /* must be zero; available for future use */
} STA_ObjHeader;

/* Derived sizes and offsets — validated in spike tests */
#define STA_HEADER_SIZE     sizeof(STA_ObjHeader)   /* expect 12 */
#define STA_HEADER_ALIGNED  16u                     /* allocation unit */

/* Payload pointer: first OOP-sized word after the header,
 * at offset STA_HEADER_ALIGNED (16) from the start of the allocation. */
static inline STA_OOP *sta_payload(STA_ObjHeader *h) {
    return (STA_OOP *)((char *)h + STA_HEADER_ALIGNED);
}

/* Total allocation size for an object with `n` payload words.
 * Always a multiple of 8 (one OOP). */
static inline size_t sta_alloc_size(uint32_t n) {
    return STA_HEADER_ALIGNED + (size_t)n * sizeof(STA_OOP);
}
