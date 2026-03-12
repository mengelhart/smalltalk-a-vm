/* src/image/image_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: image save/load for a closed-world subset.
 * See docs/spikes/spike-006-image.md and ADR 012 (to be written).
 */

#include "src/image/image_spike.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

/* ── Static assertions ──────────────────────────────────────────────────── */

static_assert(sizeof(STA_ImageHeader)    == 48,  "STA_ImageHeader must be 48 bytes");
static_assert(sizeof(STA_ObjRecord)      == 16,  "STA_ObjRecord must be 16 bytes");
static_assert(sizeof(STA_ImmutableEntry) == 10,  "STA_ImmutableEntry must be 10 bytes");
static_assert(sizeof(STA_RelocEntry)     ==  8,  "STA_RelocEntry must be 8 bytes");
static_assert(sizeof(STA_ActorSnap)      == 152, "STA_ActorSnap must be 152 bytes");
static_assert(sizeof(void *)             ==  8,  "ptr_width must be 8 (arm64)");

/* ── Utilities ──────────────────────────────────────────────────────────── */

/* Detect host endianness at runtime. */
static uint8_t host_endian(void) {
    uint16_t v = 1;
    return (*(uint8_t *)&v == 1) ? STA_IMAGE_ENDIAN_LITTLE : STA_IMAGE_ENDIAN_BIG;
}

/* Write exactly n bytes; return false on error. */
static bool fwrite_exact(const void *buf, size_t n, FILE *f) {
    return fwrite(buf, 1, n, f) == n;
}

/* Read exactly n bytes; return false on error or EOF. */
static bool fread_exact(void *buf, size_t n, FILE *f) {
    return fread(buf, 1, n, f) == n;
}

/* Encode a live OOP for the snapshot file.
 * SmallInts and character immediates are stored verbatim.
 * Heap pointers are stored as (object_id << 2) | STA_SNAP_HEAP_TAG. */
static uint64_t encode_oop(const STA_SnapCtx *ctx, STA_OOP oop) {
    /* SmallInt: bit 0 = 1 — store verbatim */
    if (STA_IS_SMALLINT(oop)) {
        return (uint64_t)oop;
    }
    /* Character immediate: bits 1:0 = 10 — store verbatim */
    if ((oop & 0x3u) == 0x2u) {
        return (uint64_t)oop;
    }
    /* nil encodes as the null pointer; it must be a registered immutable */
    /* Heap pointer: look up object_id */
    STA_ObjHeader *hdr = (STA_ObjHeader *)(uintptr_t)oop;
    uint32_t id = sta_snap_lookup_id(ctx, hdr);
    assert(id != UINT32_MAX && "encode_oop: unregistered heap object");
    return STA_SNAP_ENCODE(id);
}

/* ── Context API ────────────────────────────────────────────────────────── */

void sta_snap_ctx_init(STA_SnapCtx *ctx) {
    memset(ctx, 0, sizeof(*ctx));
}

uint32_t sta_snap_register_immutable(STA_SnapCtx *ctx, STA_ObjHeader *hdr,
                                     const char *name, uint16_t name_len) {
    assert(ctx->immutable_count < STA_SNAP_MAX_IMMUTABLES);
    assert(ctx->object_count < STA_SNAP_MAX_OBJECTS);

    uint32_t oid = ctx->object_count++;

    /* Object table entry */
    STA_ObjEntry *e = &ctx->objects[oid];
    e->hdr          = hdr;
    e->object_id    = oid;
    e->is_immutable = true;

    /* Immutable registry */
    STA_ImmutableReg *r = &ctx->immutables[ctx->immutable_count++];
    r->stable_key = sta_fnv1a(name, name_len);
    r->object_id  = oid;
    r->name       = name;
    r->name_len   = name_len;
    r->hdr        = hdr;

    return oid;
}

uint32_t sta_snap_register_object(STA_SnapCtx *ctx, STA_ObjHeader *hdr) {
    assert(ctx->object_count < STA_SNAP_MAX_OBJECTS);

    uint32_t oid = ctx->object_count++;

    STA_ObjEntry *e = &ctx->objects[oid];
    e->hdr          = hdr;
    e->object_id    = oid;
    e->is_immutable = false;

    return oid;
}

uint32_t sta_snap_lookup_id(const STA_SnapCtx *ctx, const STA_ObjHeader *hdr) {
    for (uint32_t i = 0; i < ctx->object_count; i++) {
        if (ctx->objects[i].hdr == hdr) {
            return ctx->objects[i].object_id;
        }
    }
    return UINT32_MAX;
}

/* ── Save ────────────────────────────────────────────────────────────────── */

int sta_image_save(STA_SnapCtx *ctx, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return STA_ERR_IMAGE_IO;

    int rc = STA_OK;

    /* ── Pass 1: build relocation table ─────────────────────────────────── */
    ctx->reloc_count = 0;

    for (uint32_t i = 0; i < ctx->object_count; i++) {
        STA_ObjHeader *hdr = ctx->objects[i].hdr;
        STA_OOP       *payload = sta_payload(hdr);

        for (uint32_t s = 0; s < hdr->size; s++) {
            STA_OOP oop = payload[s];
            /* Skip SmallInts and character immediates */
            if (STA_IS_SMALLINT(oop))       continue;
            if ((oop & 0x3u) == 0x2u)       continue;
            /* Heap pointer — needs a reloc entry */
            assert(ctx->reloc_count < STA_SNAP_MAX_RELOCS);
            STA_RelocEntry *rel = &ctx->relocs[ctx->reloc_count++];
            rel->object_id  = ctx->objects[i].object_id;
            rel->slot_index = s;
        }
    }

    /* ── Compute section offsets ──────────────────────────────────────────
     *
     * File layout:
     *   [0]                    STA_ImageHeader (48 bytes)
     *   [immutable_offset]     immutable section entries
     *   [data_offset]          object data records
     *   [reloc_offset]         reloc table
     */

    /* Compute immutable section size */
    uint64_t imm_size = 0;
    for (uint32_t i = 0; i < ctx->immutable_count; i++) {
        imm_size += sizeof(STA_ImmutableEntry) + ctx->immutables[i].name_len;
    }

    /* Compute data section size */
    uint64_t data_size = 0;
    for (uint32_t i = 0; i < ctx->object_count; i++) {
        data_size += sizeof(STA_ObjRecord) +
                     (uint64_t)ctx->objects[i].hdr->size * sizeof(uint64_t);
    }

    uint64_t reloc_size = (uint64_t)ctx->reloc_count * sizeof(STA_RelocEntry);

    uint64_t imm_offset   = sizeof(STA_ImageHeader);
    uint64_t data_offset  = imm_offset  + imm_size;
    uint64_t reloc_offset = data_offset + data_size;
    uint64_t total_size   = reloc_offset + reloc_size;

    /* ── Write header ─────────────────────────────────────────────────────*/
    STA_ImageHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, STA_IMAGE_MAGIC, STA_IMAGE_MAGIC_LEN);
    hdr.version                  = STA_IMAGE_VERSION;
    hdr.endian                   = host_endian();
    hdr.ptr_width                = (uint8_t)sizeof(void *);
    hdr.object_count             = ctx->object_count;
    hdr.immutable_count          = ctx->immutable_count;
    hdr.immutable_section_offset = imm_offset;
    hdr.data_section_offset      = data_offset;
    hdr.reloc_section_offset     = reloc_offset;
    hdr.file_size                = total_size;

    if (!fwrite_exact(&hdr, sizeof(hdr), f)) { rc = STA_ERR_IMAGE_IO; goto done; }

    /* ── Write immutable section ──────────────────────────────────────────*/
    for (uint32_t i = 0; i < ctx->immutable_count; i++) {
        STA_ImmutableReg *r = &ctx->immutables[i];
        STA_ImmutableEntry ent;
        ent.stable_key   = r->stable_key;
        ent.name_len     = r->name_len;
        ent.immutable_id = r->object_id;
        if (!fwrite_exact(&ent, sizeof(ent), f)) { rc = STA_ERR_IMAGE_IO; goto done; }
        if (!fwrite_exact(r->name, r->name_len, f)) { rc = STA_ERR_IMAGE_IO; goto done; }
    }

    /* ── Write data section ───────────────────────────────────────────────*/
    for (uint32_t i = 0; i < ctx->object_count; i++) {
        STA_ObjHeader *ohdr = ctx->objects[i].hdr;
        STA_OOP       *payload = sta_payload(ohdr);

        /* Determine class_key */
        uint32_t class_key = ohdr->class_index;
        if (ctx->objects[i].is_immutable) {
            class_key |= STA_CLASS_KEY_IMMUT_FLAG;
        }

        STA_ObjRecord rec;
        rec.object_id = ctx->objects[i].object_id;
        rec.class_key = class_key;
        rec.size      = ohdr->size;
        rec.gc_flags  = ohdr->gc_flags;
        rec.obj_flags = ohdr->obj_flags;
        rec.reserved  = 0;
        if (!fwrite_exact(&rec, sizeof(rec), f)) { rc = STA_ERR_IMAGE_IO; goto done; }

        /* Write encoded payload words */
        for (uint32_t s = 0; s < ohdr->size; s++) {
            uint64_t encoded = encode_oop(ctx, payload[s]);
            if (!fwrite_exact(&encoded, sizeof(encoded), f)) {
                rc = STA_ERR_IMAGE_IO; goto done;
            }
        }
    }

    /* ── Write relocation table ───────────────────────────────────────────*/
    for (uint32_t i = 0; i < ctx->reloc_count; i++) {
        if (!fwrite_exact(&ctx->relocs[i], sizeof(STA_RelocEntry), f)) {
            rc = STA_ERR_IMAGE_IO; goto done;
        }
    }

done:
    fclose(f);
    return rc;
}

/* ── Load ────────────────────────────────────────────────────────────────── */

int sta_image_load(const char *path,
                   STA_ImmutableResolver resolver, void *userdata,
                   STA_ObjHeader **root_out) {
    FILE *f = fopen(path, "rb");
    if (!f) return STA_ERR_IMAGE_IO;

    int rc = STA_OK;

    /* id → runtime address table (malloc; freed at end or on error) */
    STA_ObjHeader **id_table = NULL;
    /* Buffers for object records and reloc entries */
    STA_ObjRecord  *records  = NULL;
    STA_RelocEntry *relocs   = NULL;
    uint8_t        *name_buf = NULL; /* reused for each immutable name */

    /* ── Read and validate header ─────────────────────────────────────────*/
    STA_ImageHeader hdr;
    if (!fread_exact(&hdr, sizeof(hdr), f)) { rc = STA_ERR_IMAGE_IO; goto done; }

    if (memcmp(hdr.magic, STA_IMAGE_MAGIC, STA_IMAGE_MAGIC_LEN) != 0) {
        rc = STA_ERR_IMAGE_MAGIC; goto done;
    }
    if (hdr.version != STA_IMAGE_VERSION) {
        rc = STA_ERR_IMAGE_VERSION; goto done;
    }
    if (hdr.endian != host_endian()) {
        rc = STA_ERR_IMAGE_ENDIAN; goto done;
    }
    if (hdr.ptr_width != 8) {
        rc = STA_ERR_IMAGE_PTRWIDTH; goto done;
    }

    uint32_t total_objects    = hdr.object_count;
    uint32_t immutable_count  = hdr.immutable_count;

    /* ── Allocate id → address table ──────────────────────────────────────*/
    id_table = calloc(total_objects, sizeof(STA_ObjHeader *));
    if (!id_table) { rc = STA_ERR_IMAGE_OOM; goto done; }

    /* ── Read object records (cache them; we need two passes) ─────────────*/
    records = calloc(total_objects, sizeof(STA_ObjRecord));
    if (!records) { rc = STA_ERR_IMAGE_OOM; goto done; }

    /* ── Pass 1: process immutable section ───────────────────────────────*/
    if (fseek(f, (long)hdr.immutable_section_offset, SEEK_SET) != 0) {
        rc = STA_ERR_IMAGE_IO; goto done;
    }
    /* Max name length we expect in the spike. */
    name_buf = malloc(65536);
    if (!name_buf) { rc = STA_ERR_IMAGE_OOM; goto done; }

    for (uint32_t i = 0; i < immutable_count; i++) {
        STA_ImmutableEntry ent;
        if (!fread_exact(&ent, sizeof(ent), f)) { rc = STA_ERR_IMAGE_IO; goto done; }
        if (!fread_exact(name_buf, ent.name_len, f)) { rc = STA_ERR_IMAGE_IO; goto done; }

        STA_ObjHeader *resolved = resolver(ent.stable_key,
                                           (const char *)name_buf,
                                           ent.name_len, userdata);
        if (!resolved) { rc = STA_ERR_IMAGE_CORRUPT; goto done; }

        if (ent.immutable_id >= total_objects) {
            rc = STA_ERR_IMAGE_CORRUPT; goto done;
        }
        id_table[ent.immutable_id] = resolved;
    }

    /* ── Pass 2: allocate non-immutable objects ───────────────────────────*/
    if (fseek(f, (long)hdr.data_section_offset, SEEK_SET) != 0) {
        rc = STA_ERR_IMAGE_IO; goto done;
    }

    for (uint32_t i = 0; i < total_objects; i++) {
        if (!fread_exact(&records[i], sizeof(STA_ObjRecord), f)) {
            rc = STA_ERR_IMAGE_IO; goto done;
        }
        /* Skip payload bytes for now; we'll re-read on pass 3 */
        long payload_bytes = (long)records[i].size * (long)sizeof(uint64_t);
        if (fseek(f, payload_bytes, SEEK_CUR) != 0) {
            rc = STA_ERR_IMAGE_IO; goto done;
        }

        uint32_t oid = records[i].object_id;
        if (oid >= total_objects) { rc = STA_ERR_IMAGE_CORRUPT; goto done; }

        /* Immutables are already in id_table; skip allocation */
        if (id_table[oid] != NULL) continue;

        /* Allocate fresh memory */
        size_t alloc = sta_alloc_size(records[i].size);
        void *mem = malloc(alloc);
        if (!mem) { rc = STA_ERR_IMAGE_OOM; goto done; }
        memset(mem, 0, alloc);

        STA_ObjHeader *nhdr = (STA_ObjHeader *)mem;
        uint32_t class_key = records[i].class_key;
        nhdr->class_index = class_key & ~STA_CLASS_KEY_IMMUT_FLAG;
        nhdr->size        = records[i].size;
        nhdr->gc_flags    = records[i].gc_flags;
        nhdr->obj_flags   = records[i].obj_flags;
        nhdr->reserved    = 0;

        id_table[oid] = nhdr;
    }

    /* ── Pass 3: fill payload words (raw encoded values) ─────────────────*/
    /* We store encoded OOP arrays temporarily in the payload, then patch */
    if (fseek(f, (long)hdr.data_section_offset, SEEK_SET) != 0) {
        rc = STA_ERR_IMAGE_IO; goto done;
    }

    for (uint32_t i = 0; i < total_objects; i++) {
        /* Re-read record (we already have records[] cached) */
        /* Skip fixed record header (already in records[]) */
        if (fseek(f, (long)sizeof(STA_ObjRecord), SEEK_CUR) != 0) {
            rc = STA_ERR_IMAGE_IO; goto done;
        }

        uint32_t oid  = records[i].object_id;
        uint32_t size = records[i].size;

        /* Read encoded payload into the object's payload area directly */
        STA_ObjHeader *nhdr    = id_table[oid];
        uint64_t      *payload = (uint64_t *)sta_payload(nhdr);

        for (uint32_t s = 0; s < size; s++) {
            uint64_t encoded;
            if (!fread_exact(&encoded, sizeof(encoded), f)) {
                rc = STA_ERR_IMAGE_IO; goto done;
            }
            payload[s] = encoded; /* stored as-is; reloc pass will fix up */
        }
    }

    /* ── Pass 4: read reloc table ─────────────────────────────────────────*/
    if (fseek(f, (long)hdr.reloc_section_offset, SEEK_SET) != 0) {
        rc = STA_ERR_IMAGE_IO; goto done;
    }

    uint64_t reloc_count = (hdr.file_size - hdr.reloc_section_offset)
                           / sizeof(STA_RelocEntry);
    relocs = malloc(reloc_count * sizeof(STA_RelocEntry));
    if (!relocs) { rc = STA_ERR_IMAGE_OOM; goto done; }

    for (uint64_t i = 0; i < reloc_count; i++) {
        if (!fread_exact(&relocs[i], sizeof(STA_RelocEntry), f)) {
            rc = STA_ERR_IMAGE_IO; goto done;
        }
    }

    /* ── Pass 5: apply relocations ───────────────────────────────────────*/
    for (uint64_t i = 0; i < reloc_count; i++) {
        uint32_t oid   = relocs[i].object_id;
        uint32_t slot  = relocs[i].slot_index;

        if (oid >= total_objects || id_table[oid] == NULL) {
            rc = STA_ERR_IMAGE_CORRUPT; goto done;
        }

        uint64_t *payload = (uint64_t *)sta_payload(id_table[oid]);
        uint64_t  encoded = payload[slot];

        if (!STA_SNAP_IS_HEAP(encoded)) {
            rc = STA_ERR_IMAGE_CORRUPT; goto done;
        }

        uint32_t ref_id = STA_SNAP_GET_ID(encoded);
        if (ref_id >= total_objects || id_table[ref_id] == NULL) {
            rc = STA_ERR_IMAGE_CORRUPT; goto done;
        }

        /* Patch: replace encoded snapshot OOP with live heap pointer */
        payload[slot] = (uint64_t)(uintptr_t)id_table[ref_id];
    }

    /* Root is object_id 0 */
    if (id_table[0] == NULL) { rc = STA_ERR_IMAGE_CORRUPT; goto done; }
    *root_out = id_table[0];

done:
    free(id_table);
    free(records);
    free(relocs);
    free(name_buf);
    fclose(f);
    return rc;
}
