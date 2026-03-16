/* src/image/image.c
 * Production image writer — see image.h for documentation.
 * Phase 1 — permanent. See ADR 012 (format spec + amendments).
 */

#include "image.h"
#include "../vm/special_objects.h"
#include "../vm/interpreter.h"
#include "../vm/format.h"
#include "../vm/compiled_method.h"
#include <sta/vm.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Static assertions (format-locked by ADR 012) ──────────────────────── */

_Static_assert(sizeof(STA_ImageHeader)    == 48, "STA_ImageHeader must be 48 bytes");
_Static_assert(sizeof(STA_ObjRecord)      == 16, "STA_ObjRecord must be 16 bytes");
_Static_assert(sizeof(STA_ImmutableEntry) == 10, "STA_ImmutableEntry must be 10 bytes");
_Static_assert(sizeof(STA_RelocEntry)     ==  8, "STA_RelocEntry must be 8 bytes");
_Static_assert(sizeof(void *)             ==  8, "ptr_width must be 8 (arm64)");

/* ── Utilities ──────────────────────────────────────────────────────────── */

/* Write exactly n bytes; return false on error. */
static bool fwrite_exact(const void *buf, size_t n, FILE *f) {
    return fwrite(buf, 1, n, f) == n;
}

/* Detect host endianness at runtime. */
static uint8_t host_endian(void) {
    uint16_t v = 1;
    return (*(uint8_t *)&v == 1) ? STA_IMAGE_ENDIAN_LITTLE : STA_IMAGE_ENDIAN_BIG;
}

/* ── Pointer → object_id hash table ─────────────────────────────────────── */

/* Open-addressing hash table: uintptr_t key → uint32_t object_id value.
 * Power-of-two capacity, linear probing, 70% load factor growth. */

#define ID_MAP_EMPTY UINTPTR_MAX   /* sentinel: slot is empty */

typedef struct {
    uintptr_t *keys;     /* object header addresses (cast from STA_ObjHeader *) */
    uint32_t  *values;   /* corresponding object_ids */
    uint32_t   capacity; /* always a power of two */
    uint32_t   count;    /* number of occupied slots */
} IdMap;

static int id_map_init(IdMap *m, uint32_t initial_capacity) {
    /* Round up to power of two. */
    uint32_t cap = 16;
    while (cap < initial_capacity) cap *= 2;

    m->keys   = malloc(cap * sizeof(uintptr_t));
    m->values = malloc(cap * sizeof(uint32_t));
    if (!m->keys || !m->values) {
        free(m->keys);
        free(m->values);
        return -1;
    }
    for (uint32_t i = 0; i < cap; i++) m->keys[i] = ID_MAP_EMPTY;
    m->capacity = cap;
    m->count    = 0;
    return 0;
}

static void id_map_destroy(IdMap *m) {
    free(m->keys);
    free(m->values);
}

static int id_map_grow(IdMap *m) {
    uint32_t new_cap = m->capacity * 2;
    uintptr_t *new_keys = malloc(new_cap * sizeof(uintptr_t));
    uint32_t  *new_vals = malloc(new_cap * sizeof(uint32_t));
    if (!new_keys || !new_vals) {
        free(new_keys);
        free(new_vals);
        return -1;
    }
    for (uint32_t i = 0; i < new_cap; i++) new_keys[i] = ID_MAP_EMPTY;

    uint32_t mask = new_cap - 1;
    for (uint32_t i = 0; i < m->capacity; i++) {
        if (m->keys[i] == ID_MAP_EMPTY) continue;
        uint32_t idx = (uint32_t)(m->keys[i] >> 4) & mask;
        while (new_keys[idx] != ID_MAP_EMPTY) idx = (idx + 1) & mask;
        new_keys[idx] = m->keys[i];
        new_vals[idx] = m->values[i];
    }

    free(m->keys);
    free(m->values);
    m->keys     = new_keys;
    m->values   = new_vals;
    m->capacity = new_cap;
    return 0;
}

/* Insert a key→value pair. Key must not already exist. */
static int id_map_put(IdMap *m, uintptr_t key, uint32_t value) {
    if ((m->count + 1) * 10 > m->capacity * 7) {
        if (id_map_grow(m) != 0) return -1;
    }
    uint32_t mask = m->capacity - 1;
    uint32_t idx = (uint32_t)(key >> 4) & mask;
    while (m->keys[idx] != ID_MAP_EMPTY) idx = (idx + 1) & mask;
    m->keys[idx]   = key;
    m->values[idx] = value;
    m->count++;
    return 0;
}

/* Look up a key. Returns UINT32_MAX if not found. */
static uint32_t id_map_get(const IdMap *m, uintptr_t key) {
    uint32_t mask = m->capacity - 1;
    uint32_t idx = (uint32_t)(key >> 4) & mask;
    for (;;) {
        if (m->keys[idx] == ID_MAP_EMPTY) return UINT32_MAX;
        if (m->keys[idx] == key) return m->values[idx];
        idx = (idx + 1) & mask;
    }
}

/* ── Object registration entry ──────────────────────────────────────────── */

typedef struct {
    STA_ObjHeader *hdr;
    uint32_t       object_id;
    bool           is_immutable;
} ObjReg;

/* ── Dynamic array helpers ──────────────────────────────────────────────── */

typedef struct {
    ObjReg   *data;
    uint32_t  count;
    uint32_t  capacity;
} ObjRegArray;

static int objreg_push(ObjRegArray *a, ObjReg entry) {
    if (a->count == a->capacity) {
        uint32_t new_cap = a->capacity ? a->capacity * 2 : 64;
        ObjReg *tmp = realloc(a->data, new_cap * sizeof(ObjReg));
        if (!tmp) return -1;
        a->data     = tmp;
        a->capacity = new_cap;
    }
    a->data[a->count++] = entry;
    return 0;
}

typedef struct {
    STA_ObjHeader **data;
    uint32_t        count;
    uint32_t        capacity;
} WorkList;

static int worklist_push(WorkList *w, STA_ObjHeader *hdr) {
    if (w->count == w->capacity) {
        uint32_t new_cap = w->capacity ? w->capacity * 2 : 256;
        STA_ObjHeader **tmp = realloc(w->data, new_cap * sizeof(STA_ObjHeader *));
        if (!tmp) return -1;
        w->data     = tmp;
        w->capacity = new_cap;
    }
    w->data[w->count++] = hdr;
    return 0;
}

static STA_ObjHeader *worklist_pop(WorkList *w) {
    return w->data[--w->count];
}

typedef struct {
    STA_RelocEntry *data;
    uint32_t        count;
    uint32_t        capacity;
} RelocArray;

static int reloc_push(RelocArray *a, STA_RelocEntry entry) {
    if (a->count == a->capacity) {
        uint32_t new_cap = a->capacity ? a->capacity * 2 : 256;
        STA_RelocEntry *tmp = realloc(a->data, new_cap * sizeof(STA_RelocEntry));
        if (!tmp) return -1;
        a->data     = tmp;
        a->capacity = new_cap;
    }
    a->data[a->count++] = entry;
    return 0;
}

/* Immutable name entry — for the immutable section of the file. */
typedef struct {
    uint32_t object_id;
    uint32_t stable_key;
    const char *name;
    uint16_t name_len;
} ImmutableName;

typedef struct {
    ImmutableName *data;
    uint32_t       count;
    uint32_t       capacity;
} ImmutableNameArray;

static int imm_name_push(ImmutableNameArray *a, ImmutableName entry) {
    if (a->count == a->capacity) {
        uint32_t new_cap = a->capacity ? a->capacity * 2 : 64;
        ImmutableName *tmp = realloc(a->data, new_cap * sizeof(ImmutableName));
        if (!tmp) return -1;
        a->data     = tmp;
        a->capacity = new_cap;
    }
    a->data[a->count++] = entry;
    return 0;
}

/* ── Determine OOP-scannable slots in a payload ──────────────────────────── */

/* Returns the number of payload slots that contain OOPs (as opposed to raw
 * bytes). Byte-indexable objects (String, Symbol, ByteArray) have zero
 * scannable slots. CompiledMethods have header + literals as scannable.
 * Normal and OOP-variable objects have all slots scannable. */
static uint32_t scannable_oop_slots(const STA_ObjHeader *hdr,
                                     STA_ClassTable *class_table) {
    uint32_t ci = hdr->class_index;

    /* For metaclasses (ci >= STA_CLS_RESERVED_COUNT or ci == STA_CLS_METACLASS),
     * and other heap-allocated classes: all 4 slots are OOPs. */

    /* Look up the class to get its format. */
    STA_OOP class_oop = sta_class_table_get(class_table, ci);
    if (class_oop == 0) {
        /* Unknown class — assume all slots are OOPs (safe default). */
        return hdr->size;
    }

    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)class_oop;
    STA_OOP fmt = sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];
    uint8_t fmt_type = STA_FORMAT_TYPE(fmt);

    switch (fmt_type) {
    case STA_FMT_VARIABLE_BYTE:
        /* Byte-indexable: entire payload is raw bytes/data, not OOPs.
         * Symbol has instVarCount=1 but slot 0 is the precomputed FNV-1a
         * hash stored as a raw uint32_t — not a real OOP reference.
         * String and ByteArray have instVarCount=0.
         * No OOP-containing slots to scan. */
        return 0;

    case STA_FMT_COMPILED_METHOD:
        /* CompiledMethod: payload[0] is header (SmallInt), payload[1..nLits]
         * are literal OOPs, rest is bytecodes. */
        if (hdr->size == 0) return 0;
        {
            STA_OOP mhdr = sta_payload((STA_ObjHeader *)hdr)[0];
            uint8_t num_lits = STA_METHOD_NUM_LITERALS(mhdr);
            return 1 + (uint32_t)num_lits;  /* header word + literals */
        }

    default:
        /* Normal, variable-OOP, or other: all slots are OOPs. */
        return hdr->size;
    }
}

/* ── Encode a live OOP for the snapshot file ─────────────────────────────── */

static uint64_t encode_oop(const IdMap *map, STA_OOP oop) {
    /* Null pointer (e.g. empty class table slot) — store as 0 verbatim. */
    if (oop == 0) return 0;
    /* SmallInt: bit 0 = 1 — store verbatim */
    if (STA_IS_SMALLINT(oop)) return (uint64_t)oop;
    /* Character immediate: bits 1:0 = 10 — store verbatim */
    if (STA_IS_CHAR(oop)) return (uint64_t)oop;
    /* Heap pointer: look up object_id */
    uint32_t id = id_map_get(map, (uintptr_t)oop);
    assert(id != UINT32_MAX && "encode_oop: unregistered heap object");
    return STA_SNAP_ENCODE(id);
}

/* ── Check if a pointer is in immutable space ────────────────────────────── */

static bool is_in_immutable_space(const STA_ImmutableSpace *sp,
                                   const STA_ObjHeader *hdr) {
    const char *base = (const char *)sta_immutable_space_base(sp);
    size_t used = sta_immutable_space_used(sp);
    const char *ptr = (const char *)hdr;
    return ptr >= base && ptr < base + used;
}

/* ── Production image writer ─────────────────────────────────────────────── */

int sta_image_save_to_file(
    const char *path,
    STA_Heap *heap,
    STA_ImmutableSpace *immutable_space,
    STA_SymbolTable *symbol_table,
    STA_ClassTable *class_table)
{
    int rc = STA_OK;

    /* ── Transient data structures ──────────────────────────────────────── */
    IdMap            id_map   = {0};
    ObjRegArray      objects  = {0};
    WorkList         worklist = {0};
    RelocArray       relocs   = {0};
    ImmutableNameArray imm_names = {0};
    FILE            *f        = NULL;

    if (id_map_init(&id_map, 1024) != 0) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }

    /* ── Phase A: Build root Array and walk all reachable objects ────────── */

    /* Construct the root Array (3-slot, allocated in immutable space).
     * root[0] = special object table (as an Array of 32 OOPs)
     * root[1] = class table snapshot (as an Array)
     * root[2] = globals SystemDictionary */

    /* Build special objects Array. */
    STA_ObjHeader *spc_arr = sta_immutable_alloc(immutable_space, STA_CLS_ARRAY,
                                                  STA_SPECIAL_OBJECTS_COUNT);
    if (!spc_arr) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
    {
        STA_OOP *slots = sta_payload(spc_arr);
        for (uint32_t i = 0; i < STA_SPECIAL_OBJECTS_COUNT; i++) {
            slots[i] = sta_spc_get(i);
        }
    }

    /* Build class table Array. */
    uint32_t ct_capacity = sta_class_table_capacity(class_table);
    STA_ObjHeader *ct_arr = sta_immutable_alloc(immutable_space, STA_CLS_ARRAY,
                                                 ct_capacity);
    if (!ct_arr) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
    {
        STA_OOP *slots = sta_payload(ct_arr);
        for (uint32_t i = 0; i < ct_capacity; i++) {
            slots[i] = sta_class_table_get(class_table, i);
        }
    }

    /* Get globals SystemDictionary. */
    STA_OOP globals_oop = sta_spc_get(SPC_SMALLTALK);

    /* Build root Array. */
    STA_ObjHeader *root_arr = sta_immutable_alloc(immutable_space, STA_CLS_ARRAY,
                                                   STA_IMAGE_ROOT_COUNT);
    if (!root_arr) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
    {
        STA_OOP *slots = sta_payload(root_arr);
        slots[STA_IMAGE_ROOT_SPECIAL_OBJECTS] = (STA_OOP)(uintptr_t)spc_arr;
        slots[STA_IMAGE_ROOT_CLASS_TABLE]     = (STA_OOP)(uintptr_t)ct_arr;
        slots[STA_IMAGE_ROOT_GLOBALS]         = globals_oop;
    }

    /* Register the root Array as object_id 0 and begin graph walk. */
    {
        ObjReg root_reg = {
            .hdr = root_arr,
            .object_id = 0,
            .is_immutable = true,
        };
        if (objreg_push(&objects, root_reg) != 0) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
        if (id_map_put(&id_map, (uintptr_t)root_arr, 0) != 0) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
        if (worklist_push(&worklist, root_arr) != 0) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
    }

    /* Graph walk: BFS/DFS worklist. */
    while (worklist.count > 0) {
        STA_ObjHeader *hdr = worklist_pop(&worklist);
        STA_OOP *payload = sta_payload(hdr);
        uint32_t oop_slots = scannable_oop_slots(hdr, class_table);

        for (uint32_t s = 0; s < oop_slots; s++) {
            STA_OOP oop = payload[s];
            if (STA_IS_SMALLINT(oop)) continue;
            if (STA_IS_CHAR(oop)) continue;
            /* oop is a heap pointer (bits 1:0 = 00) */
            if (oop == 0) continue;  /* null pointer — skip */

            STA_ObjHeader *ref = (STA_ObjHeader *)(uintptr_t)oop;

            /* Already registered? */
            if (id_map_get(&id_map, (uintptr_t)ref) != UINT32_MAX) continue;

            /* Register new object. */
            uint32_t new_id = objects.count;
            bool is_imm = is_in_immutable_space(immutable_space, ref);
            ObjReg reg = { .hdr = ref, .object_id = new_id, .is_immutable = is_imm };
            if (objreg_push(&objects, reg) != 0) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
            if (id_map_put(&id_map, (uintptr_t)ref, new_id) != 0) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
            if (worklist_push(&worklist, ref) != 0) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
        }
    }

    /* ── Build immutable name entries ───────────────────────────────────── */
    /* Only nil, true, false, and interned symbols need names. */

    STA_OOP nil_oop   = sta_spc_get(SPC_NIL);
    STA_OOP true_oop  = sta_spc_get(SPC_TRUE);
    STA_OOP false_oop = sta_spc_get(SPC_FALSE);

    for (uint32_t i = 0; i < objects.count; i++) {
        ObjReg *reg = &objects.data[i];
        if (!reg->is_immutable) continue;

        const char *name = NULL;
        uint16_t name_len = 0;

        STA_OOP obj_oop = (STA_OOP)(uintptr_t)reg->hdr;

        if (obj_oop == nil_oop) {
            name = "nil"; name_len = 3;
        } else if (obj_oop == true_oop) {
            name = "true"; name_len = 4;
        } else if (obj_oop == false_oop) {
            name = "false"; name_len = 5;
        } else if (reg->hdr->class_index == STA_CLS_SYMBOL) {
            /* Symbol — use its string content as the name. */
            size_t slen;
            name = sta_symbol_get_bytes(obj_oop, &slen);
            name_len = (uint16_t)slen;
        } else {
            /* Other immutable objects (character table, etc.) — no name entry.
             * They are serialised as regular objects with payload + immutable flag. */
            continue;
        }

        ImmutableName imm = {
            .object_id  = reg->object_id,
            .stable_key = sta_fnv1a(name, name_len),
            .name       = name,
            .name_len   = name_len,
        };
        if (imm_name_push(&imm_names, imm) != 0) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
    }

    /* ── Phase B: Build relocation table ─────────────────────────────────── */

    for (uint32_t i = 0; i < objects.count; i++) {
        STA_ObjHeader *hdr = objects.data[i].hdr;
        STA_OOP *payload = sta_payload(hdr);
        uint32_t oid = objects.data[i].object_id;
        uint32_t oop_slots = scannable_oop_slots(hdr, class_table);

        for (uint32_t s = 0; s < oop_slots; s++) {
            STA_OOP oop = payload[s];
            if (STA_IS_SMALLINT(oop)) continue;
            if (STA_IS_CHAR(oop)) continue;
            if (oop == 0) continue;
            /* Heap pointer — needs reloc entry */
            STA_RelocEntry rel = { .object_id = oid, .slot_index = s };
            if (reloc_push(&relocs, rel) != 0) { rc = STA_ERR_IMAGE_OOM; goto cleanup; }
        }
    }

    /* ── Phase C: Class name table for indices 32+ ───────────────────────── */
    /* Check if any classes exist at indices >= 32 that are not metaclasses.
     * After bootstrap + kernel load, indices 32+ are all metaclasses (assigned
     * during bootstrap by create_class). These have stable construction order,
     * so no name table is needed for Phase 1.
     * TODO: When file-in class creation (prim 122) assigns indices beyond the
     * bootstrap range, emit a class name table for those indices. */

    /* ── Phase D: Write file ─────────────────────────────────────────────── */

    /* Compute immutable section size. */
    uint64_t imm_section_size = 0;
    for (uint32_t i = 0; i < imm_names.count; i++) {
        imm_section_size += sizeof(STA_ImmutableEntry) + imm_names.data[i].name_len;
    }

    /* Compute data section size. */
    uint64_t data_section_size = 0;
    for (uint32_t i = 0; i < objects.count; i++) {
        data_section_size += sizeof(STA_ObjRecord) +
                             (uint64_t)objects.data[i].hdr->size * sizeof(uint64_t);
    }

    uint64_t reloc_section_size = (uint64_t)relocs.count * sizeof(STA_RelocEntry);

    uint64_t imm_offset   = sizeof(STA_ImageHeader);
    uint64_t data_offset  = imm_offset + imm_section_size;
    uint64_t reloc_offset = data_offset + data_section_size;
    uint64_t total_size   = reloc_offset + reloc_section_size;

    /* Open output file. */
    f = fopen(path, "wb");
    if (!f) { rc = STA_ERR_IMAGE_IO; goto cleanup; }

    /* Write header. */
    STA_ImageHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, STA_IMAGE_MAGIC, STA_IMAGE_MAGIC_LEN);
    hdr.version                  = STA_IMAGE_VERSION;
    hdr.endian                   = host_endian();
    hdr.ptr_width                = (uint8_t)sizeof(void *);
    hdr.object_count             = objects.count;
    hdr.immutable_count          = imm_names.count;
    hdr.immutable_section_offset = imm_offset;
    hdr.data_section_offset      = data_offset;
    hdr.reloc_section_offset     = reloc_offset;
    hdr.file_size                = total_size;

    if (!fwrite_exact(&hdr, sizeof(hdr), f)) { rc = STA_ERR_IMAGE_IO; goto cleanup; }

    /* Write immutable section. */
    for (uint32_t i = 0; i < imm_names.count; i++) {
        ImmutableName *n = &imm_names.data[i];
        STA_ImmutableEntry ent;
        ent.stable_key   = n->stable_key;
        ent.name_len     = n->name_len;
        ent.immutable_id = n->object_id;
        if (!fwrite_exact(&ent, sizeof(ent), f)) { rc = STA_ERR_IMAGE_IO; goto cleanup; }
        if (n->name_len > 0) {
            if (!fwrite_exact(n->name, n->name_len, f)) { rc = STA_ERR_IMAGE_IO; goto cleanup; }
        }
    }

    /* Write data section. */
    for (uint32_t i = 0; i < objects.count; i++) {
        ObjReg *reg = &objects.data[i];
        STA_ObjHeader *ohdr = reg->hdr;
        STA_OOP *payload = sta_payload(ohdr);

        uint32_t class_key = ohdr->class_index;
        if (reg->is_immutable) {
            class_key |= STA_CLASS_KEY_IMMUT_FLAG;
        }

        STA_ObjRecord rec;
        rec.object_id = reg->object_id;
        rec.class_key = class_key;
        rec.size      = ohdr->size;
        rec.gc_flags  = ohdr->gc_flags;
        rec.obj_flags = ohdr->obj_flags;
        rec.reserved  = ohdr->reserved;
        if (!fwrite_exact(&rec, sizeof(rec), f)) { rc = STA_ERR_IMAGE_IO; goto cleanup; }

        /* Write encoded payload words.
         * OOP-containing slots are encoded; byte/raw slots are written verbatim. */
        uint32_t oop_slots_w = scannable_oop_slots(ohdr, class_table);
        for (uint32_t s = 0; s < ohdr->size; s++) {
            uint64_t encoded;
            if (s < oop_slots_w) {
                encoded = encode_oop(&id_map, payload[s]);
            } else {
                /* Raw data word (bytecodes, byte data) — write verbatim. */
                encoded = (uint64_t)payload[s];
            }
            if (!fwrite_exact(&encoded, sizeof(encoded), f)) {
                rc = STA_ERR_IMAGE_IO; goto cleanup;
            }
        }
    }

    /* Write relocation table. */
    if (relocs.count > 0) {
        if (!fwrite_exact(relocs.data, relocs.count * sizeof(STA_RelocEntry), f)) {
            rc = STA_ERR_IMAGE_IO; goto cleanup;
        }
    }

cleanup:
    if (f) fclose(f);
    id_map_destroy(&id_map);
    free(objects.data);
    free(worklist.data);
    free(relocs.data);
    free(imm_names.data);
    return rc;
}

/* ── Read helper ─────────────────────────────────────────────────────────── */

static bool fread_exact(void *buf, size_t n, FILE *f) {
    return fread(buf, 1, n, f) == n;
}

/* ── Production image loader ─────────────────────────────────────────────── */

int sta_image_load_from_file(
    const char *path,
    STA_Heap *heap,
    STA_ImmutableSpace *immutable_space,
    STA_SymbolTable *symbol_table,
    STA_ClassTable *class_table)
{
    FILE *f = fopen(path, "rb");
    if (!f) return STA_ERR_IMAGE_IO;

    int rc = STA_OK;
    STA_ObjHeader **id_table = NULL;
    STA_ObjRecord  *records  = NULL;
    STA_RelocEntry *relocs   = NULL;
    uint8_t        *name_buf = NULL;

    /* Immutable name info — saved from pass 2 for use in pass 6. */
    typedef struct { uint32_t id; char name[256]; uint16_t name_len; } ImmInfo;
    ImmInfo *imm_info = NULL;

    /* ── Pass 1: Read and validate header ─────────────────────────────────*/
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

    uint32_t total_objects   = hdr.object_count;
    uint32_t immutable_count = hdr.immutable_count;

    /* ── Allocate id → address table ──────────────────────────────────────*/
    id_table = calloc(total_objects, sizeof(STA_ObjHeader *));
    if (!id_table) { rc = STA_ERR_IMAGE_OOM; goto done; }

    /* ── Read all object records (we need them for passes 3-4) ────────────*/
    records = calloc(total_objects, sizeof(STA_ObjRecord));
    if (!records) { rc = STA_ERR_IMAGE_OOM; goto done; }

    /* ── Pass 2: Process immutable section ────────────────────────────────*/
    /* We just read the names and store them; the actual objects will be
     * allocated in pass 3 from the data section. After pass 5 we use the
     * immutable entries to identify nil/true/false and symbols. */

    imm_info = calloc(immutable_count > 0 ? immutable_count : 1, sizeof(ImmInfo));
    if (!imm_info) { rc = STA_ERR_IMAGE_OOM; goto done; }

    if (fseek(f, (long)hdr.immutable_section_offset, SEEK_SET) != 0) {
        rc = STA_ERR_IMAGE_IO; goto done;
    }
    name_buf = malloc(65536);
    if (!name_buf) { rc = STA_ERR_IMAGE_OOM; goto done; }

    for (uint32_t i = 0; i < immutable_count; i++) {
        STA_ImmutableEntry ent;
        if (!fread_exact(&ent, sizeof(ent), f)) { rc = STA_ERR_IMAGE_IO; goto done; }
        if (ent.name_len > 0) {
            if (!fread_exact(name_buf, ent.name_len, f)) { rc = STA_ERR_IMAGE_IO; goto done; }
        }
        if (ent.immutable_id >= total_objects) { rc = STA_ERR_IMAGE_CORRUPT; goto done; }

        imm_info[i].id = ent.immutable_id;
        imm_info[i].name_len = ent.name_len;
        if (ent.name_len < sizeof(imm_info[i].name)) {
            memcpy(imm_info[i].name, name_buf, ent.name_len);
            imm_info[i].name[ent.name_len] = '\0';
        }
    }

    /* ── Pass 3: Read object records and allocate all objects ──────────────*/
    if (fseek(f, (long)hdr.data_section_offset, SEEK_SET) != 0) {
        rc = STA_ERR_IMAGE_IO; goto done;
    }

    for (uint32_t i = 0; i < total_objects; i++) {
        if (!fread_exact(&records[i], sizeof(STA_ObjRecord), f)) {
            rc = STA_ERR_IMAGE_IO; goto done;
        }
        /* Skip payload for now. */
        long payload_bytes = (long)records[i].size * (long)sizeof(uint64_t);
        if (fseek(f, payload_bytes, SEEK_CUR) != 0) {
            rc = STA_ERR_IMAGE_IO; goto done;
        }

        uint32_t oid = records[i].object_id;
        if (oid >= total_objects) { rc = STA_ERR_IMAGE_CORRUPT; goto done; }

        uint32_t class_key = records[i].class_key;
        bool is_immutable = (class_key & STA_CLASS_KEY_IMMUT_FLAG) != 0;
        uint32_t class_index = class_key & ~STA_CLASS_KEY_IMMUT_FLAG;

        STA_ObjHeader *nhdr;
        if (is_immutable) {
            nhdr = sta_immutable_alloc(immutable_space, class_index, records[i].size);
        } else {
            nhdr = sta_heap_alloc(heap, class_index, records[i].size);
        }
        if (!nhdr) { rc = STA_ERR_IMAGE_OOM; goto done; }

        nhdr->gc_flags = records[i].gc_flags;
        nhdr->obj_flags = records[i].obj_flags;
        nhdr->reserved = records[i].reserved;

        id_table[oid] = nhdr;
    }

    /* ── Pass 4: Fill payload words (raw encoded values) ──────────────────*/
    if (fseek(f, (long)hdr.data_section_offset, SEEK_SET) != 0) {
        rc = STA_ERR_IMAGE_IO; goto done;
    }

    for (uint32_t i = 0; i < total_objects; i++) {
        /* Skip record header. */
        if (fseek(f, (long)sizeof(STA_ObjRecord), SEEK_CUR) != 0) {
            rc = STA_ERR_IMAGE_IO; goto done;
        }

        uint32_t oid  = records[i].object_id;
        uint32_t size = records[i].size;
        STA_ObjHeader *nhdr = id_table[oid];
        uint64_t *payload = (uint64_t *)sta_payload(nhdr);

        for (uint32_t s = 0; s < size; s++) {
            uint64_t encoded;
            if (!fread_exact(&encoded, sizeof(encoded), f)) {
                rc = STA_ERR_IMAGE_IO; goto done;
            }
            payload[s] = encoded;
        }
    }

    /* ── Pass 5: Apply relocations ───────────────────────────────────────*/
    if (fseek(f, (long)hdr.reloc_section_offset, SEEK_SET) != 0) {
        rc = STA_ERR_IMAGE_IO; goto done;
    }

    uint64_t reloc_count = (hdr.file_size - hdr.reloc_section_offset)
                           / sizeof(STA_RelocEntry);
    if (reloc_count > 0) {
        relocs = malloc((size_t)reloc_count * sizeof(STA_RelocEntry));
        if (!relocs) { rc = STA_ERR_IMAGE_OOM; goto done; }

        for (uint64_t i = 0; i < reloc_count; i++) {
            if (!fread_exact(&relocs[i], sizeof(STA_RelocEntry), f)) {
                rc = STA_ERR_IMAGE_IO; goto done;
            }
        }

        for (uint64_t i = 0; i < reloc_count; i++) {
            uint32_t oid  = relocs[i].object_id;
            uint32_t slot = relocs[i].slot_index;

            if (oid >= total_objects || id_table[oid] == NULL) {
                rc = STA_ERR_IMAGE_CORRUPT; goto done;
            }

            uint64_t *payload = (uint64_t *)sta_payload(id_table[oid]);
            uint64_t encoded = payload[slot];

            if (!STA_SNAP_IS_HEAP(encoded)) {
                rc = STA_ERR_IMAGE_CORRUPT; goto done;
            }

            uint32_t ref_id = STA_SNAP_GET_ID(encoded);
            if (ref_id >= total_objects || id_table[ref_id] == NULL) {
                rc = STA_ERR_IMAGE_CORRUPT; goto done;
            }

            /* Patch: replace encoded snapshot OOP with live heap pointer. */
            payload[slot] = (uint64_t)(uintptr_t)id_table[ref_id];
        }
    }

    /* ── Pass 6: Rebuild runtime tables ──────────────────────────────────*/

    /* 6a. Extract root Array (object_id 0). */
    if (id_table[0] == NULL) { rc = STA_ERR_IMAGE_CORRUPT; goto done; }
    STA_ObjHeader *root = id_table[0];
    if (root->size < STA_IMAGE_ROOT_COUNT) { rc = STA_ERR_IMAGE_CORRUPT; goto done; }

    STA_OOP *root_slots = sta_payload(root);
    STA_ObjHeader *spc_arr_h = (STA_ObjHeader *)(uintptr_t)root_slots[STA_IMAGE_ROOT_SPECIAL_OBJECTS];
    STA_ObjHeader *ct_arr_h  = (STA_ObjHeader *)(uintptr_t)root_slots[STA_IMAGE_ROOT_CLASS_TABLE];
    STA_OOP globals = root_slots[STA_IMAGE_ROOT_GLOBALS];

    if (!spc_arr_h || !ct_arr_h) { rc = STA_ERR_IMAGE_CORRUPT; goto done; }

    /* 6b. Restore special object table. */
    sta_special_objects_init();
    {
        STA_OOP *spc_slots = sta_payload(spc_arr_h);
        uint32_t spc_count = spc_arr_h->size;
        if (spc_count > STA_SPECIAL_OBJECTS_COUNT) spc_count = STA_SPECIAL_OBJECTS_COUNT;
        for (uint32_t i = 0; i < spc_count; i++) {
            sta_spc_set(i, spc_slots[i]);
        }
    }

    /* 6c. Restore class table. */
    {
        STA_OOP *ct_slots = sta_payload(ct_arr_h);
        uint32_t ct_count = ct_arr_h->size;
        for (uint32_t i = 1; i < ct_count; i++) {  /* skip index 0 (invalid) */
            if (ct_slots[i] != 0) {
                sta_class_table_set(class_table, i, ct_slots[i]);
            }
        }
    }

    /* 6d. Restore globals. */
    sta_spc_set(SPC_SMALLTALK, globals);

    /* 6e. Rebuild symbol table index. */
    /* Walk all restored objects; register any Symbol objects. */
    for (uint32_t i = 0; i < total_objects; i++) {
        STA_ObjHeader *obj = id_table[i];
        if (!obj) continue;
        uint32_t ci = obj->class_index & ~STA_CLASS_KEY_IMMUT_FLAG;
        if (ci == STA_CLS_SYMBOL) {
            STA_OOP sym = (STA_OOP)(uintptr_t)obj;
            if (sta_symbol_table_register(symbol_table, sym) != 0) {
                rc = STA_ERR_IMAGE_OOM; goto done;
            }
        }
    }

done:
    if (f) fclose(f);
    free(id_table);
    free(records);
    free(relocs);
    free(name_buf);
    free(imm_info);
    return rc;
}
