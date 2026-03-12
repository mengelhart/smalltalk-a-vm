/* src/image/image_spike.h
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: image save/load for a closed-world subset.
 * See docs/spikes/spike-006-image.md and ADR 012 (to be written).
 *
 * Hard rules:
 *   - No scheduler, no GC, no libuv included or required.
 *   - Only <stdint.h>, <stddef.h>, <stdbool.h>, <stdio.h> included here.
 *   - All libc I/O types (FILE *) confined to image_spike.c.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "src/vm/oop_spike.h"

/* ── Error codes ────────────────────────────────────────────────────────── */

#define STA_OK                    0
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
    uint32_t object_count;          /* total heap objects (excl. immutables) */
    uint32_t immutable_count;       /* shared immutable objects */
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
 *   nil/true/false and other heap ptrs: stored as:
 *       (uint64_t)((object_id << 2) | STA_SNAP_HEAP_TAG)
 *
 * STA_SNAP_HEAP_TAG = 0x3 (bits 1:0 = 11) is the currently-reserved tag
 * in the live OOP encoding (ADR 007), so it cannot be confused with a live
 * heap pointer or SmallInt. Safe to use in snapshot files only.
 */
#define STA_SNAP_HEAP_TAG    UINT64_C(0x3)
#define STA_SNAP_IS_HEAP(v)  (((v) & 0x3u) == STA_SNAP_HEAP_TAG)
#define STA_SNAP_GET_ID(v)   ((uint32_t)((v) >> 2))
#define STA_SNAP_ENCODE(id)  (((uint64_t)(id) << 2) | STA_SNAP_HEAP_TAG)

/* ── Object record (written per object in the data section) ─────────────── */

/* class_key high bit: 0 = actor-local class index, 1 = immutable class key */
#define STA_CLASS_KEY_IMMUT_FLAG  0x80000000u

typedef struct __attribute__((packed)) {
    uint32_t object_id;   /* sequential, 0-based */
    uint32_t class_key;   /* class_index or FNV-1a key (flag in high bit) */
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

/* ── Snapshot context ────────────────────────────────────────────────────── */

/* Maximum objects for the spike (production uses dynamic allocation). */
#define STA_SNAP_MAX_OBJECTS    4096u
#define STA_SNAP_MAX_IMMUTABLES  256u
#define STA_SNAP_MAX_RELOCS    16384u

/* A named immutable known at snapshot time. */
typedef struct {
    uint32_t      stable_key;
    uint32_t      object_id;
    const char   *name;       /* points into caller-owned memory */
    uint16_t      name_len;
    STA_ObjHeader *hdr;       /* runtime header pointer */
} STA_ImmutableReg;

/* Object registration entry during save. */
typedef struct {
    STA_ObjHeader *hdr;
    uint32_t       object_id;
    bool           is_immutable;
} STA_ObjEntry;

/* Top-level snapshot context — stack-allocated in spike tests. */
typedef struct {
    /* Object registry */
    STA_ObjEntry     objects[STA_SNAP_MAX_OBJECTS];
    uint32_t         object_count;

    /* Immutable registry */
    STA_ImmutableReg immutables[STA_SNAP_MAX_IMMUTABLES];
    uint32_t         immutable_count;

    /* Relocation table */
    STA_RelocEntry   relocs[STA_SNAP_MAX_RELOCS];
    uint32_t         reloc_count;
} STA_SnapCtx;

/* ── Actor snapshot density struct (Step 9) ──────────────────────────────── */

/* Represents STA_ActorIo (from Spike 005, 144 bytes) extended with snapshot
 * fields. snapshot_id enables stable object references across save/load.
 * STA_ACTOR_QUIESCED flag uses sched_flags bit 3 (0x08), not a new field. */
#define STA_ACTOR_QUIESCED  0x08u   /* sched_flags bit: actor stopped for snap */

typedef struct {
    /* All fields from STA_ActorIo (Spike 005, 144 bytes) */
    uint32_t         class_index;       /*  4 */
    uint32_t         actor_id;          /*  4 */
    void            *mbox_tail;         /*  8 */
    void            *mbox_head;         /*  8 */
    uint8_t          mbox_stub[16];     /* 16 */
    uint32_t         mbox_count;        /*  4 */
    uint32_t         mbox_limit;        /*  4 */
    _Atomic uint32_t reductions;        /*  4 */
    _Atomic uint32_t sched_flags;       /*  4 */
    void            *heap_base;         /*  8 */
    void            *heap_bump;         /*  8 */
    void            *heap_limit_ptr;    /*  8 */
    void            *supervisor;        /*  8 */
    uint32_t         restart_strategy;  /*  4 */
    uint32_t         restart_count;     /*  4 */
    uint64_t         capability_token;  /*  8 */
    void            *next_runnable;     /*  8 */
    uint32_t         home_thread;       /*  4 */
    uint32_t         _pad0;             /*  4 */
    void            *stack_base;        /*  8 */
    void            *stack_top;         /*  8 */
    _Atomic uint32_t io_state;          /*  4 */
    int32_t          io_result;         /*  4 */
    /* Spike 006 addition */
    uint32_t         snapshot_id;       /*  4 — actor's object ID in snapshot */
    uint32_t         _pad1;             /*  4 — alignment padding */
} STA_ActorSnap;
/* Expected sizeof: 144 + 8 = 152 bytes.
 * Creation cost: 152 + 128 (nursery) + 16 (identity) = 296 bytes.
 * Headroom vs 300-byte target: 4 bytes. */

/* ── FNV-1a hash (stable key for immutable names) ───────────────────────── */

static inline uint32_t sta_fnv1a(const char *s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h;
}

/* ── Public API ─────────────────────────────────────────────────────────── */

/* Initialise a snapshot context. Must be called before any registration. */
void sta_snap_ctx_init(STA_SnapCtx *ctx);

/* Register a shared immutable object. name must remain valid for the lifetime
 * of ctx. Returns the assigned object_id. */
uint32_t sta_snap_register_immutable(STA_SnapCtx *ctx, STA_ObjHeader *hdr,
                                     const char *name, uint16_t name_len);

/* Register an actor-local heap object. Returns the assigned object_id. */
uint32_t sta_snap_register_object(STA_SnapCtx *ctx, STA_ObjHeader *hdr);

/* Look up the object_id for a registered header. Returns UINT32_MAX if not
 * found. */
uint32_t sta_snap_lookup_id(const STA_SnapCtx *ctx, const STA_ObjHeader *hdr);

/* Encode all registered objects and write to path.
 * Populates the relocation table as a side-effect.
 * Returns STA_OK or a STA_ERR_IMAGE_* code. */
int sta_image_save(STA_SnapCtx *ctx, const char *path);

/* ── Restore context ─────────────────────────────────────────────────────── */

/* Callback type: given an immutable name (not null-terminated) and its FNV-1a
 * key, return the runtime STA_ObjHeader * for that symbol/class.
 * The spike implementation uses a small linear table of name→header pairs.
 * Returns NULL if not found (image load aborts with STA_ERR_IMAGE_CORRUPT). */
typedef STA_ObjHeader *(*STA_ImmutableResolver)(uint32_t stable_key,
                                                const char *name,
                                                uint16_t name_len,
                                                void *userdata);

/* Load an image from path. Allocates fresh memory for all non-immutable
 * objects (malloc for spike; pool allocator in Phase 1).
 *
 * resolver is called for each immutable entry to map name → runtime address.
 * userdata is passed through to resolver unchanged.
 *
 * On success, *root_out receives the runtime pointer for object_id 0 (the
 * root of the serialized graph), and returns STA_OK.
 * On failure, returns a STA_ERR_IMAGE_* code and *root_out is unchanged. */
int sta_image_load(const char *path,
                   STA_ImmutableResolver resolver, void *userdata,
                   STA_ObjHeader **root_out);
