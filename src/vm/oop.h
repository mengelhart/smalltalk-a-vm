/* src/vm/oop.h
 * Production OOP typedef and STA_ObjHeader definition.
 * Phase 1 — permanent, not spike. See ADR 007 (header layout, OOP tagging)
 * and the Character representation addendum in ADR 007.
 *
 * Tag scheme (bits 1:0 of every OOP value):
 *   00 — heap pointer  (16-byte aligned; bits 3:0 always zero in a live pointer)
 *   01 — SmallInt      (63-bit signed; value in bits 63:1)
 *   10 — Character     (62-bit Unicode code point; value in bits 63:2)
 *   11 — reserved      (used as snapshot-heap tag in image files; never live)
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

/* ── OOP type ───────────────────────────────────────────────────────────── */
typedef uintptr_t STA_OOP;

/* ── Tag predicates ─────────────────────────────────────────────────────── */
/* Note: also true for tag 11 (reserved/snapshot); callers handling raw
 * image data must check STA_IS_HEAP first or use the relocation table. */
#define STA_IS_SMALLINT(oop)    (((oop) & 1u) != 0u)
#define STA_IS_CHAR(oop)        (((oop) & 3u) == 2u)
#define STA_IS_HEAP(oop)        (((oop) & 3u) == 0u)
#define STA_IS_IMMEDIATE(oop)   (!STA_IS_HEAP(oop))

/* ── SmallInt encoding (bit 0 = 1; value in bits 63:1, 63-bit signed) ──── */
#define STA_SMALLINT_VAL(oop)   ((intptr_t)(oop) >> 1)
#define STA_SMALLINT_OOP(n)     (((STA_OOP)(uintptr_t)(intptr_t)(n) << 1) | 1u)

/* ── Character encoding (bits 1:0 = 10; code point in bits 63:2) ────────── */
#define STA_CHAR_VAL(oop)       ((uint32_t)((oop) >> 2))
#define STA_CHAR_OOP(cp)        (((STA_OOP)(uint32_t)(cp) << 2) | 2u)

/* ── Object header ──────────────────────────────────────────────────────── */

/*
 * Layout verified by _Static_assert in oop.c:
 *
 *   field         offset   size
 *   class_index    0        4
 *   size           4        4
 *   gc_flags       8        1
 *   obj_flags      9        1
 *   reserved      10        2
 *   (struct total)          12 bytes
 *   (allocation unit)       16 bytes  ← 4-byte implicit gap
 *
 * The first payload word is at offset 16 from the allocation base,
 * ensuring 8-byte (and 16-byte) alignment on arm64.
 */
typedef struct {
    uint32_t class_index;  /* index into global class table              */
    uint32_t size;         /* payload size in OOP-sized words (8 bytes)  */
    uint8_t  gc_flags;     /* GC color, forwarding, remembered-set       */
    uint8_t  obj_flags;    /* immutable, pinned, actor-local, etc.       */
    uint16_t reserved;     /* must be zero; available for future use     */
} STA_ObjHeader;           /* sizeof = 12; allocation unit = 16          */

/* ── reserved field: byte-padding for byte-indexable objects ──────────────── */
/* Bits 2:0 of reserved store the number of unused trailing bytes (0-7) in
 * byte-indexable objects (ByteArray, String). This lets the runtime recover
 * the exact byte count: exact_bytes = (size - instSize) * 8 - padding.
 * Bits 15:3 remain zero and available for future use. */
#define STA_BYTE_PADDING_MASK   0x07u
#define STA_BYTE_PADDING(h)     ((h)->reserved & STA_BYTE_PADDING_MASK)

/* ── Allocation constants ────────────────────────────────────────────────── */
#define STA_HEADER_SIZE   12u   /* sizeof(STA_ObjHeader)                  */
#define STA_ALLOC_UNIT    16u   /* allocation unit; payload starts here   */

/* ── gc_flags bits (ADR 007) ────────────────────────────────────────────── */
#define STA_GC_WHITE        0x00u  /* not yet visited                     */
#define STA_GC_GREY         0x01u  /* discovered, children not yet scanned*/
#define STA_GC_BLACK        0x02u  /* fully scanned                       */
#define STA_GC_COLOR_MASK   0x03u  /* tri-color mask (bits 1:0)           */
#define STA_GC_FORWARDED    0x04u  /* object copied; header stores new addr*/
#define STA_GC_REMEMBERED   0x08u  /* old-space obj refs young-space obj  */
/* bits 7:4 reserved — must be zero; debug builds should assert this      */

/* ── obj_flags bits (ADR 007) ───────────────────────────────────────────── */
#define STA_OBJ_IMMUTABLE   0x01u  /* writes to slots are a runtime error */
#define STA_OBJ_PINNED      0x02u  /* must not be moved by compacting GC  */
#define STA_OBJ_ACTOR_LOCAL 0x04u  /* must not escape this actor's heap   */
#define STA_OBJ_SHARED_IMM  0x08u  /* lives in shared immutable region    */
#define STA_OBJ_FINALIZE    0x10u  /* registered for finalization callback */
/* bits 7:5 reserved — must be zero; debug builds should assert this      */

/* ── Allocation helpers ─────────────────────────────────────────────────── */

/* Total bytes to allocate for an object with n OOP-sized payload slots.
 * n == 0 → 16 bytes (header + padding, no payload).
 * The result is always a multiple of 8. */
static inline size_t sta_alloc_size(uint32_t n) {
    return STA_ALLOC_UNIT + (size_t)n * sizeof(STA_OOP);
}

/* Pointer to the first payload slot of a heap object.
 * The header occupies bytes 0–11; bytes 12–15 are implicit allocation padding.
 * Payload begins at offset 16 — 8-byte aligned and 16-byte aligned. */
static inline STA_OOP *sta_payload(STA_ObjHeader *h) {
    return (STA_OOP *)((char *)h + STA_ALLOC_UNIT);
}
