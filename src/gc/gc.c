/* src/gc/gc.c
 * Per-actor Cheney semi-space copying garbage collector.
 * Phase 2 Epic 5 — see gc.h for interface documentation.
 *
 * Algorithm:
 *   1. Allocate to-space (same size as from-space).
 *   2. Walk roots: stack frames, handler chain, actor OOP fields.
 *   3. Copy each reachable object from from-space to to-space.
 *   4. Install forwarding pointer in from-space copy.
 *   5. Cheney scan: advance scan pointer through to-space, updating
 *      OOP slots to follow forwarding pointers.
 *   6. When scan catches up to alloc, GC is complete.
 *   7. Free old from-space; to-space becomes the active heap.
 */
#include "gc.h"
#include "vm/oop.h"
#include "vm/heap.h"
#include "vm/frame.h"
#include "vm/handler.h"
#include "vm/immutable_space.h"
#include "vm/class_table.h"
#include "vm/format.h"
#include "vm/compiled_method.h"
#include "vm/interpreter.h"   /* STA_CLASS_SLOT_FORMAT */
#include "vm/special_objects.h"
#include "vm/vm_state.h"
#include "actor/actor.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Side table for zero-payload forwarding ────────────────────────────── */
/* Objects with size==0 have no payload word to store a forwarding pointer.
 * We use a small hash map allocated per GC cycle for these rare objects. */

#define FWD_TABLE_INITIAL_CAP  16u

typedef struct {
    STA_OOP from;  /* 0 = empty */
    STA_OOP to;
} FwdEntry;

typedef struct {
    FwdEntry *entries;
    uint32_t  capacity;
    uint32_t  count;
} FwdTable;

static int fwd_table_init(FwdTable *ft) {
    ft->capacity = FWD_TABLE_INITIAL_CAP;
    ft->count = 0;
    ft->entries = calloc(ft->capacity, sizeof(FwdEntry));
    return ft->entries ? 0 : -1;
}

static void fwd_table_destroy(FwdTable *ft) {
    free(ft->entries);
    ft->entries = NULL;
}

static inline uint32_t fwd_hash(STA_OOP key) {
    uintptr_t v = (uintptr_t)key;
    v ^= v >> 16;
    v *= 0x45d9f3bu;
    v ^= v >> 16;
    return (uint32_t)v;
}

static STA_OOP fwd_table_get(FwdTable *ft, STA_OOP from) {
    uint32_t mask = ft->capacity - 1;
    uint32_t idx = fwd_hash(from) & mask;
    for (uint32_t i = 0; i < ft->capacity; i++) {
        uint32_t pos = (idx + i) & mask;
        if (ft->entries[pos].from == 0) return 0;
        if (ft->entries[pos].from == from) return ft->entries[pos].to;
    }
    return 0;
}

static int fwd_table_grow(FwdTable *ft);

static int fwd_table_put(FwdTable *ft, STA_OOP from, STA_OOP to) {
    if (ft->count * 100 >= ft->capacity * 70) {
        if (fwd_table_grow(ft) != 0) return -1;
    }
    uint32_t mask = ft->capacity - 1;
    uint32_t idx = fwd_hash(from) & mask;
    for (uint32_t i = 0; i < ft->capacity; i++) {
        uint32_t pos = (idx + i) & mask;
        if (ft->entries[pos].from == 0) {
            ft->entries[pos].from = from;
            ft->entries[pos].to = to;
            ft->count++;
            return 0;
        }
        if (ft->entries[pos].from == from) {
            ft->entries[pos].to = to;
            return 0;
        }
    }
    return -1;
}

static int fwd_table_grow(FwdTable *ft) {
    uint32_t old_cap = ft->capacity;
    FwdEntry *old = ft->entries;
    uint32_t new_cap = old_cap * 2;
    FwdEntry *ne = calloc(new_cap, sizeof(FwdEntry));
    if (!ne) return -1;
    ft->entries = ne;
    ft->capacity = new_cap;
    ft->count = 0;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].from != 0) {
            fwd_table_put(ft, old[i].from, old[i].to);
        }
    }
    free(old);
    return 0;
}

/* ── GCState ───────────────────────────────────────────────────────────── */

typedef struct {
    /* From-space (old heap) */
    char *from_base;
    char *from_limit;

    /* To-space (new heap) */
    char *to_base;
    char *to_alloc;   /* bump pointer — next free byte in to-space */
    char *to_limit;

    /* Immutable space bounds (for skip check) */
    const char *imm_base;
    const char *imm_limit;

    /* Side table for zero-payload objects */
    FwdTable fwd_table;

    /* Class table for format queries */
    STA_ClassTable *class_table;

    /* Counters */
    uint32_t objects_copied;
    size_t   bytes_copied;
} GCState;

/* ── Address classification ────────────────────────────────────────────── */

static inline bool in_from_space(GCState *gc, const void *ptr) {
    return (const char *)ptr >= gc->from_base &&
           (const char *)ptr < gc->from_limit;
}

static inline bool in_to_space(GCState *gc, const void *ptr) {
    return (const char *)ptr >= gc->to_base &&
           (const char *)ptr < gc->to_limit;
}

static inline bool in_immutable(GCState *gc, const void *ptr) {
    return (const char *)ptr >= gc->imm_base &&
           (const char *)ptr < gc->imm_limit;
}

/* ── Scannable slot count ──────────────────────────────────────────────── */
/* Determines how many OOP-sized payload slots should be scanned as
 * pointer fields. Byte-indexable objects only scan instVar slots;
 * CompiledMethods only scan header + literals (not bytecodes). */

static uint32_t gc_scannable_slots(STA_ObjHeader *h, STA_ClassTable *ct) {
    STA_OOP cls = sta_class_table_get(ct, h->class_index);
    if (cls == 0) return h->size;  /* fallback: scan all */

    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)cls;
    STA_OOP fmt = sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];

    uint8_t fmt_type = STA_FORMAT_TYPE(fmt);

    if (fmt_type == STA_FMT_VARIABLE_BYTE) {
        return STA_FORMAT_INST_VARS(fmt);
    }
    if (fmt_type == STA_FMT_COMPILED_METHOD) {
        /* CompiledMethods should be in immutable space and never in
         * from-space, but handle correctly just in case. */
        if (h->size == 0) return 0;
        STA_OOP header_word = sta_payload(h)[0];
        uint32_t num_lits = (uint32_t)STA_METHOD_NUM_LITERALS(header_word);
        uint32_t scan = 1 + num_lits;
        return (scan < h->size) ? scan : h->size;
    }

    return h->size;
}

/* ── gc_copy — copy one object from from-space to to-space ─────────────── */

static STA_OOP gc_copy(GCState *gc, STA_OOP oop) {
    /* Skip immediates. */
    if (STA_IS_SMALLINT(oop) || STA_IS_CHAR(oop)) return oop;

    /* Skip null/nil (OOP 0). */
    if (oop == 0) return oop;

    /* Must be a heap pointer. */
    if (!STA_IS_HEAP(oop)) return oop;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)oop;

    /* Skip immutable space objects — they never move. */
    if (in_immutable(gc, h)) return oop;

    /* Skip objects already in to-space. */
    if (in_to_space(gc, h)) return oop;

    /* Object not in from-space — external reference, leave unchanged. */
    if (!in_from_space(gc, h)) return oop;

    /* Already forwarded? Follow the forwarding pointer. */
    if (h->gc_flags & STA_GC_FORWARDED) {
        if (h->size == 0) {
            /* Zero-payload: forwarding address in side table. */
            return fwd_table_get(&gc->fwd_table, oop);
        }
        /* Forwarding address stored in first payload word. */
        return sta_payload(h)[0];
    }

    /* Copy the object to to-space. */
    size_t raw = sta_alloc_size(h->size);
    size_t alloc_bytes = (raw + 15u) & ~(size_t)15u;

    if (gc->to_alloc + alloc_bytes > gc->to_limit) {
        /* To-space overflow — should not happen in a correct GC cycle.
         * Caller must have sized to-space >= from-space. */
        fprintf(stderr, "GC FATAL: to-space overflow\n");
        return oop;
    }

    STA_ObjHeader *new_h = (STA_ObjHeader *)gc->to_alloc;
    gc->to_alloc += alloc_bytes;

    /* memcpy entire object (header + payload). */
    memcpy(new_h, h, raw);

    /* Clear gc_flags in to-space copy (fresh). */
    new_h->gc_flags = STA_GC_WHITE;

    STA_OOP new_oop = (STA_OOP)(uintptr_t)new_h;

    /* Install forwarding pointer in from-space copy. */
    h->gc_flags |= STA_GC_FORWARDED;
    if (h->size == 0) {
        /* Zero-payload: use side table. */
        fwd_table_put(&gc->fwd_table, oop, new_oop);
    } else {
        /* Overwrite first payload word with new address. */
        sta_payload(h)[0] = new_oop;
    }

    gc->objects_copied++;
    gc->bytes_copied += alloc_bytes;

    return new_oop;
}

/* ── gc_scan_object — scan one object's payload, updating OOP slots ────── */

static void gc_scan_object(GCState *gc, STA_ObjHeader *obj) {
    uint32_t scan_count = gc_scannable_slots(obj, gc->class_table);
    STA_OOP *slots = sta_payload(obj);

    for (uint32_t i = 0; i < scan_count; i++) {
        slots[i] = gc_copy(gc, slots[i]);
    }
}

/* ── gc_scan_roots — enumerate and copy all roots for an actor ─────────── */

/* Visitor callback for sta_frame_gc_roots. */
static void gc_root_visitor(STA_OOP *slot, void *ctx) {
    GCState *gc = (GCState *)ctx;
    *slot = gc_copy(gc, *slot);
}

static void gc_scan_roots(GCState *gc, struct STA_Actor *actor) {
    /* 1. Stack frames — walk all frames via the existing GC root walker.
     *
     * saved_frame must be set to the innermost (most recently pushed) frame
     * before GC triggers. The interpreter sets this before any allocation
     * that could trigger GC (wired in Story 2). Tests set it manually.
     *
     * sta_frame_gc_roots walks from innermost → bottom via sender chain,
     * visiting method, receiver, context, args, temps, and expression stack. */
    if (actor->saved_frame) {
        sta_frame_gc_roots(actor->saved_frame, &actor->slab,
                           gc_root_visitor, gc);
    }

    /* 2. Handler chain — walk handler_top linked list. */
    STA_HandlerEntry *he = actor->handler_top;
    while (he) {
        he->exception_class = gc_copy(gc, he->exception_class);
        he->handler_block = gc_copy(gc, he->handler_block);
        if (he->is_ensure) {
            he->ensure_block = gc_copy(gc, he->ensure_block);
        }
        he = he->prev;
    }

    /* 3. Actor struct OOP fields. */
    actor->behavior_class = gc_copy(gc, actor->behavior_class);
    actor->behavior_obj = gc_copy(gc, actor->behavior_obj);
    actor->signaled_exception = gc_copy(gc, actor->signaled_exception);
}

/* ── sta_gc_collect — main entry point ─────────────────────────────────── */

int sta_gc_collect(struct STA_VM *vm, struct STA_Actor *actor) {
    STA_Heap *heap = &actor->heap;

    /* From-space is the current heap region. */
    GCState gc;
    memset(&gc, 0, sizeof(gc));

    gc.from_base  = heap->base;
    gc.from_limit = heap->base + heap->capacity;
    gc.class_table = &vm->class_table;

    /* Immutable space bounds for skip check. */
    gc.imm_base  = (const char *)sta_immutable_space_base(&vm->immutable_space);
    gc.imm_limit = gc.imm_base + sta_immutable_space_used(&vm->immutable_space);

    /* Allocate to-space — same size as from-space. */
    size_t to_capacity = heap->capacity;
    char *to_base = aligned_alloc(16, to_capacity);
    if (!to_base) return -1;
    memset(to_base, 0, to_capacity);

    gc.to_base  = to_base;
    gc.to_alloc = to_base;
    gc.to_limit = to_base + to_capacity;

    /* Initialize side table for zero-payload forwarding. */
    if (fwd_table_init(&gc.fwd_table) != 0) {
        free(to_base);
        return -1;
    }

    /* Phase 1: Copy roots into to-space. */
    gc_scan_roots(&gc, actor);

    /* Phase 2: Cheney scan — advance through to-space copying referents. */
    char *scan = gc.to_base;
    while (scan < gc.to_alloc) {
        STA_ObjHeader *obj = (STA_ObjHeader *)scan;
        gc_scan_object(&gc, obj);

        /* Advance scan past this object. */
        size_t raw = sta_alloc_size(obj->size);
        size_t alloc_bytes = (raw + 15u) & ~(size_t)15u;
        scan += alloc_bytes;
    }

    /* Phase 3: Update the actor's heap to use to-space. */
    size_t survived = (size_t)(gc.to_alloc - gc.to_base);

    /* Free old from-space. */
    free(heap->base);

    /* Install to-space as the new heap. */
    heap->base     = to_base;
    heap->capacity = to_capacity;
    heap->used     = survived;

    /* Clean up side table. */
    fwd_table_destroy(&gc.fwd_table);

    return 0;
}
