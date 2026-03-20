/* src/actor/deep_copy.c
 * Cross-actor deep copy engine — Phase 2 Epic 3.
 * See deep_copy.h and ADR 008.
 */
#include "deep_copy.h"
#include "vm/class_table.h"
#include "vm/format.h"
#include "vm/interpreter.h"  /* STA_CLASS_SLOT_FORMAT */
#include "gc/gc.h"           /* sta_heap_alloc_gc */
#include "actor.h"           /* STA_Actor */
#include <stdlib.h>
#include <string.h>

/* ── Visited set (open-addressing hash map) ──────────────────────────── */

/* Maps source OOP → target OOP. Used for:
 *  1. Cycle detection: if we encounter an object already in the set,
 *     return the previously created copy (no infinite loop).
 *  2. Shared structure preservation: if two slots point to the same
 *     object, the copies also share (not duplicated). */

#define VISITED_INITIAL_CAPACITY  64u
#define VISITED_LOAD_FACTOR_NUM   70u  /* grow when count*100 >= cap*70 */
#define VISITED_LOAD_FACTOR_DEN  100u

typedef struct {
    STA_OOP src;    /* 0 = empty slot */
    STA_OOP dst;
} VisitedEntry;

typedef struct {
    VisitedEntry *entries;
    uint32_t      capacity;
    uint32_t      count;
} VisitedSet;

static int visited_init(VisitedSet *vs) {
    vs->capacity = VISITED_INITIAL_CAPACITY;
    vs->count = 0;
    vs->entries = calloc(vs->capacity, sizeof(VisitedEntry));
    return vs->entries ? 0 : -1;
}

static void visited_destroy(VisitedSet *vs) {
    free(vs->entries);
    vs->entries = NULL;
}

/* Hash function for OOP keys. Heap pointers are 16-byte aligned, so
 * shift right to use the significant bits. */
static inline uint32_t oop_hash(STA_OOP key) {
    uintptr_t v = (uintptr_t)key;
    /* FNV-1a-style mixing. */
    v ^= v >> 16;
    v *= 0x45d9f3bu;
    v ^= v >> 16;
    return (uint32_t)v;
}

/* Look up src in the visited set. Returns the target OOP if found, 0 if not. */
static STA_OOP visited_get(VisitedSet *vs, STA_OOP src) {
    uint32_t mask = vs->capacity - 1;
    uint32_t idx = oop_hash(src) & mask;
    for (uint32_t i = 0; i < vs->capacity; i++) {
        uint32_t pos = (idx + i) & mask;
        if (vs->entries[pos].src == 0) return 0;       /* empty — not found */
        if (vs->entries[pos].src == src) return vs->entries[pos].dst;
    }
    return 0; /* table full — shouldn't happen with load factor */
}

static int visited_grow(VisitedSet *vs);

/* Insert src→dst mapping. Returns 0 on success, -1 on alloc failure. */
static int visited_put(VisitedSet *vs, STA_OOP src, STA_OOP dst) {
    /* Grow if at load factor threshold. */
    if (vs->count * VISITED_LOAD_FACTOR_DEN >=
        vs->capacity * VISITED_LOAD_FACTOR_NUM) {
        if (visited_grow(vs) != 0) return -1;
    }

    uint32_t mask = vs->capacity - 1;
    uint32_t idx = oop_hash(src) & mask;
    for (uint32_t i = 0; i < vs->capacity; i++) {
        uint32_t pos = (idx + i) & mask;
        if (vs->entries[pos].src == 0) {
            vs->entries[pos].src = src;
            vs->entries[pos].dst = dst;
            vs->count++;
            return 0;
        }
        if (vs->entries[pos].src == src) {
            /* Already present — update (shouldn't happen in normal use). */
            vs->entries[pos].dst = dst;
            return 0;
        }
    }
    return -1; /* should never reach here */
}

static int visited_grow(VisitedSet *vs) {
    uint32_t old_cap = vs->capacity;
    VisitedEntry *old_entries = vs->entries;

    uint32_t new_cap = old_cap * 2;
    VisitedEntry *new_entries = calloc(new_cap, sizeof(VisitedEntry));
    if (!new_entries) return -1;

    vs->entries = new_entries;
    vs->capacity = new_cap;
    vs->count = 0;

    /* Re-insert all existing entries. */
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old_entries[i].src != 0) {
            visited_put(vs, old_entries[i].src, old_entries[i].dst);
        }
    }

    free(old_entries);
    return 0;
}

/* ── Deep copy core ──────────────────────────────────────────────────── */

/* Context passed through the recursive copy. */
typedef struct {
    STA_Heap       *target;
    STA_ClassTable *class_table;
    VisitedSet      visited;
    bool            failed;   /* set on allocation failure */
    /* GC-aware mode: when vm and actor are non-NULL, use
     * sta_heap_alloc_gc instead of raw sta_heap_alloc. */
    struct STA_VM    *vm;
    struct STA_Actor *actor;
} CopyCtx;

/* Forward declaration. */
static STA_OOP copy_oop(CopyCtx *ctx, STA_OOP src);

/* Determine how many OOP-sized payload slots to scan for pointers.
 * For byte-indexable objects, only scan instVar slots — the variable
 * byte region is raw data, not OOPs.
 * For CompiledMethod, only scan header + literal slots — bytecodes
 * are raw data.
 * For all other objects, all slots are OOPs. */
static uint32_t scannable_slots(STA_ObjHeader *h, STA_ClassTable *ct) {
    STA_OOP cls = sta_class_table_get(ct, h->class_index);
    if (cls == 0) return h->size;  /* fallback: scan all */

    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)cls;
    STA_OOP fmt = sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];

    uint8_t fmt_type = STA_FORMAT_TYPE(fmt);

    if (fmt_type == STA_FMT_VARIABLE_BYTE) {
        /* Byte-indexable: only instVars are OOP slots. */
        return STA_FORMAT_INST_VARS(fmt);
    }
    if (fmt_type == STA_FMT_COMPILED_METHOD) {
        /* CompiledMethod: header word (slot 0) + literal slots.
         * The header word at slot 0 encodes numLiterals. We need to
         * read it to know how many literal slots follow.
         * Format: slot 0 = header, slots 1..numLiterals = literals,
         * rest = bytecodes (raw data). */
        if (h->size == 0) return 0;
        STA_OOP header_word = sta_payload(h)[0];
        /* numLiterals is in bits 9:0 of the header value. */
        uint32_t num_lits = (uint32_t)(STA_SMALLINT_VAL(header_word)) & 0x3FFu;
        /* Scan slot 0 (header) + literal slots. */
        uint32_t scan = 1 + num_lits;
        return (scan < h->size) ? scan : h->size;
    }

    /* All other types: all slots are OOPs. */
    return h->size;
}

static STA_OOP copy_heap_object(CopyCtx *ctx, STA_ObjHeader *src_h) {
    /* Allocate the copy on the target heap.
     * Use GC-aware path when vm/actor are available. */
    STA_ObjHeader *dst_h;
    if (ctx->vm && ctx->actor) {
        dst_h = sta_heap_alloc_gc(ctx->vm, ctx->actor,
                                   src_h->class_index, src_h->size);
    } else {
        dst_h = sta_heap_alloc(ctx->target,
                                src_h->class_index, src_h->size);
    }
    if (!dst_h) {
        ctx->failed = true;
        return 0;
    }

    STA_OOP dst_oop = (STA_OOP)(uintptr_t)dst_h;
    STA_OOP src_oop = (STA_OOP)(uintptr_t)src_h;

    /* Register in visited set BEFORE recursing (handles cycles). */
    if (visited_put(&ctx->visited, src_oop, dst_oop) != 0) {
        ctx->failed = true;
        return 0;
    }

    /* Copy reserved field (byte padding for byte-indexable objects). */
    dst_h->reserved = src_h->reserved;

    /* Determine which slots are OOP pointers vs raw data. */
    uint32_t scan_count = scannable_slots(src_h, ctx->class_table);

    STA_OOP *src_slots = sta_payload(src_h);
    STA_OOP *dst_slots = sta_payload(dst_h);

    /* Deep copy OOP slots (recursively). */
    for (uint32_t i = 0; i < scan_count; i++) {
        dst_slots[i] = copy_oop(ctx, src_slots[i]);
        if (ctx->failed) return 0;
    }

    /* memcpy raw data slots (bytes after instVars, or bytecodes). */
    if (scan_count < src_h->size) {
        memcpy(&dst_slots[scan_count], &src_slots[scan_count],
               (size_t)(src_h->size - scan_count) * sizeof(STA_OOP));
    }

    return dst_oop;
}

static STA_OOP copy_oop(CopyCtx *ctx, STA_OOP src) {
    if (ctx->failed) return 0;

    /* Immediates pass through unchanged. */
    if (STA_IS_SMALLINT(src) || STA_IS_CHAR(src)) return src;

    /* Null/nil OOP (0) passes through. */
    if (src == 0) return 0;

    /* Heap object — check flags. */
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)src;

    /* Immutable objects are shared by pointer — no copy.
     * This includes Symbols, class objects, compiled methods in
     * immutable space, nil/true/false, etc. */
    if (h->obj_flags & STA_OBJ_IMMUTABLE) return src;

    /* Check visited set (cycle detection + shared structure). */
    STA_OOP cached = visited_get(&ctx->visited, src);
    if (cached != 0) return cached;

    /* Mutable heap object — deep copy. */
    return copy_heap_object(ctx, h);
}

/* ── Public API ──────────────────────────────────────────────────────── */

STA_OOP sta_deep_copy(STA_OOP root,
                       STA_Heap *source,
                       STA_Heap *target,
                       STA_ClassTable *class_table) {
    (void)source;  /* Source heap pointer reserved for future validation;
                    * currently we just follow OOP pointers directly. */

    /* Immediates need no work. */
    if (STA_IS_SMALLINT(root) || STA_IS_CHAR(root) || root == 0) {
        return root;
    }

    /* Immutable: share by pointer. */
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)root;
    if (h->obj_flags & STA_OBJ_IMMUTABLE) return root;

    /* Set up copy context. */
    CopyCtx ctx;
    ctx.target = target;
    ctx.class_table = class_table;
    ctx.failed = false;
    ctx.vm = NULL;
    ctx.actor = NULL;

    if (visited_init(&ctx.visited) != 0) return 0;

    STA_OOP result = copy_heap_object(&ctx, h);

    visited_destroy(&ctx.visited);

    return ctx.failed ? 0 : result;
}

STA_OOP sta_deep_copy_gc(STA_OOP root,
                          STA_Heap *source,
                          struct STA_VM *vm,
                          struct STA_Actor *target_actor,
                          STA_ClassTable *class_table) {
    (void)source;

    if (STA_IS_SMALLINT(root) || STA_IS_CHAR(root) || root == 0)
        return root;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)root;
    if (h->obj_flags & STA_OBJ_IMMUTABLE) return root;

    CopyCtx ctx;
    ctx.target = &target_actor->heap;
    ctx.class_table = class_table;
    ctx.failed = false;
    ctx.vm = vm;
    ctx.actor = target_actor;

    if (visited_init(&ctx.visited) != 0) return 0;

    STA_OOP result = copy_heap_object(&ctx, h);

    visited_destroy(&ctx.visited);

    return ctx.failed ? 0 : result;
}
