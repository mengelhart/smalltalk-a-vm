/* src/image/image.h
 * Production image format structs and writer.
 * Phase 1 — permanent. See ADR 012 (format spec + amendments).
 *
 * The image file format is locked at version 1 by ADR 012.
 * Do not change struct layouts or section order.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "../vm/oop.h"
#include "../vm/heap.h"
#include "../vm/immutable_space.h"
#include "../vm/class_table.h"
#include "../vm/symbol_table.h"

/* ── Error codes ────────────────────────────────────────────────────────── */

#define STA_ERR_IMAGE_MAGIC     (-1)   /* bad magic bytes */
#define STA_ERR_IMAGE_VERSION   (-2)   /* unsupported version */
#define STA_ERR_IMAGE_ENDIAN    (-3)   /* host/image endian mismatch */
#define STA_ERR_IMAGE_PTRWIDTH  (-4)   /* ptr_width != 8 */
#define STA_ERR_IMAGE_IO        (-5)   /* fread/fwrite/fseek failure */
#define STA_ERR_IMAGE_OOM       (-6)   /* malloc returned NULL */
#define STA_ERR_IMAGE_CORRUPT   (-7)   /* structural inconsistency */

/* ── File format header ─────────────────────────────────────────────────── */

#define STA_IMAGE_MAGIC "\x53\x54\x41\x01"   /* "STA\x01" */
#define STA_IMAGE_MAGIC_LEN 4u
#define STA_IMAGE_VERSION   1u

/* Endian tag stored in the image. */
#define STA_IMAGE_ENDIAN_LITTLE 0u
#define STA_IMAGE_ENDIAN_BIG    1u

typedef struct __attribute__((packed)) {
    uint8_t  magic[4];              /* "STA\x01" */
    uint16_t version;               /* must equal STA_IMAGE_VERSION */
    uint8_t  endian;                /* 0=little, 1=big — write native */
    uint8_t  ptr_width;             /* must be 8 on arm64 */
    uint32_t object_count;          /* total objects including immutables */
    uint32_t immutable_count;       /* shared immutable objects (name-keyed) */
    uint64_t immutable_section_offset; /* file offset to immutable section */
    uint64_t data_section_offset;   /* file offset to object data section */
    uint64_t reloc_section_offset;  /* file offset to reloc table */
    uint64_t file_size;             /* total file size in bytes */
} STA_ImageHeader;
/* sizeof: 4+2+1+1+4+4+8+8+8+8 = 48 bytes */

/* ── Snapshot OOP encoding ───────────────────────────────────────────────── */

/* In the snapshot file, OOPs are encoded as follows:
 *
 *   SmallInt (bit 0 = 1): stored verbatim — no fixup needed.
 *   Character imm (bits 1:0 = 10): stored verbatim — no fixup needed.
 *   Heap pointers: stored as (object_id << 2) | STA_SNAP_HEAP_TAG.
 *
 * STA_SNAP_HEAP_TAG = 0x3 (bits 1:0 = 11) is the reserved tag in the
 * live OOP encoding (ADR 007). Disambiguation is via the relocation table,
 * not tag bits alone.
 */
#define STA_SNAP_HEAP_TAG    UINT64_C(0x3)
#define STA_SNAP_IS_HEAP(v)  (((v) & 0x3u) == STA_SNAP_HEAP_TAG)
#define STA_SNAP_GET_ID(v)   ((uint32_t)((v) >> 2))
#define STA_SNAP_ENCODE(id)  (((uint64_t)(id) << 2) | STA_SNAP_HEAP_TAG)

/* ── Object record (written per object in the data section) ─────────────── */

/* class_key high bit: set if object came from immutable space */
#define STA_CLASS_KEY_IMMUT_FLAG  0x80000000u

typedef struct __attribute__((packed)) {
    uint32_t object_id;   /* sequential, 0-based */
    uint32_t class_key;   /* class_index; high bit = immutable space origin */
    uint32_t size;        /* payload words (matches STA_ObjHeader.size) */
    uint8_t  gc_flags;
    uint8_t  obj_flags;
    uint16_t reserved;    /* must be zero */
    /* followed by: size * sizeof(uint64_t) bytes of encoded OOP payload */
} STA_ObjRecord;
/* sizeof: 4+4+4+1+1+2 = 16 bytes fixed, then payload */

/* ── Immutable section entry ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t stable_key;    /* FNV-1a hash of the name string */
    uint16_t name_len;      /* byte length of name (no null terminator) */
    uint32_t immutable_id;  /* object ID assigned to this immutable */
    /* followed by: name_len bytes of UTF-8 name */
} STA_ImmutableEntry;
/* sizeof: 4+2+4 = 10 bytes fixed, then name bytes */

/* ── Relocation table entry ──────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint32_t object_id;   /* which object contains the heap-pointer slot */
    uint32_t slot_index;  /* which payload word (0-based) */
} STA_RelocEntry;
/* sizeof: 8 bytes */

/* ── Root table constants (ADR 012 amendment) ────────────────────────────── */

#define STA_IMAGE_ROOT_SPECIAL_OBJECTS  0
#define STA_IMAGE_ROOT_CLASS_TABLE      1
#define STA_IMAGE_ROOT_GLOBALS          2
#define STA_IMAGE_ROOT_COUNT            3

/* ── FNV-1a hash (stable key for immutable names) ───────────────────────── */

static inline uint32_t sta_fnv1a(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/* ── Writer API (internal — not the public sta_vm_save_image) ────────────── */

/* Save the entire live object graph to a binary image file.
 * Constructs the root Array, walks all reachable objects, and writes
 * the image in ADR 012 format.
 *
 * Returns STA_OK (0) on success, or a STA_ERR_IMAGE_* code on failure. */
int sta_image_save_to_file(
    const char *path,
    STA_Heap *heap,
    STA_ImmutableSpace *immutable_space,
    STA_SymbolTable *symbol_table,
    STA_ClassTable *class_table);

/* ── Loader API (internal — not the public sta_vm_load_image) ────────────── */

/* Load an image file into the given runtime state.
 * Allocates objects in heap and immutable_space. Rebuilds the special
 * object table, class table, symbol table index, and globals.
 *
 * After a successful load + sta_primitive_table_init(), the runtime is
 * in exactly the same state as after bootstrap + kernel load.
 *
 * Returns STA_OK (0) on success, or a STA_ERR_IMAGE_* code on failure. */
int sta_image_load_from_file(
    const char *path,
    STA_Heap *heap,
    STA_ImmutableSpace *immutable_space,
    STA_SymbolTable *symbol_table,
    STA_ClassTable *class_table);
