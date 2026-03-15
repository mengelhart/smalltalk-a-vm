/* src/vm/primitive_table.c
 * Primitive function table — see primitive_table.h for documentation.
 */
#include "primitive_table.h"
#include "format.h"
#include "handler.h"
#include "interpreter.h"
#include "method_dict.h"
#include "special_objects.h"
#include "symbol_table.h"
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdio.h>

STA_PrimFn sta_primitives[STA_PRIM_TABLE_SIZE];

/* Class table pointer for primitives that need class lookup. */
static STA_ClassTable *g_prim_class_table;

void sta_primitive_set_class_table(STA_ClassTable *ct) {
    g_prim_class_table = ct;
}

/* Heap pointer for allocation primitives. */
static STA_Heap *g_prim_heap;

void sta_primitive_set_heap(STA_Heap *heap) {
    g_prim_heap = heap;
}

/* Stack slab pointer for exception primitives (on:do:, ensure:). */
static STA_StackSlab *g_prim_slab;

void sta_primitive_set_slab(STA_StackSlab *slab) {
    g_prim_slab = slab;
}

/* ── SmallInteger arithmetic primitives ────────────────────────────────── */

/* Prim 1: SmallInteger >> #+ */
static int prim_smallint_add(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    intptr_t r;
    if (__builtin_add_overflow(a, b, &r)) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_SMALLINT_OOP(r);
    return STA_PRIM_SUCCESS;
}

/* Prim 2: SmallInteger >> #- */
static int prim_smallint_sub(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    intptr_t r;
    if (__builtin_sub_overflow(a, b, &r)) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_SMALLINT_OOP(r);
    return STA_PRIM_SUCCESS;
}

/* Prim 3: SmallInteger >> #< */
static int prim_smallint_lt(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    *result = (a < b) ? sta_spc_get(SPC_TRUE) : sta_spc_get(SPC_FALSE);
    return STA_PRIM_SUCCESS;
}

/* Prim 4: SmallInteger >> #> */
static int prim_smallint_gt(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    *result = (a > b) ? sta_spc_get(SPC_TRUE) : sta_spc_get(SPC_FALSE);
    return STA_PRIM_SUCCESS;
}

/* Prim 7: SmallInteger >> #= */
static int prim_smallint_eq(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = (args[0] == args[1]) ? sta_spc_get(SPC_TRUE) : sta_spc_get(SPC_FALSE);
    return STA_PRIM_SUCCESS;
}

/* Prim 9: SmallInteger >> #* */
static int prim_smallint_mul(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    intptr_t r;
    if (__builtin_mul_overflow(a, b, &r)) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_SMALLINT_OOP(r);
    return STA_PRIM_SUCCESS;
}

/* ── Object primitives ─────────────────────────────────────────────────── */

/* Prim 29: Object >> #== (identity) */
static int prim_identity(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    *result = (args[0] == args[1]) ? sta_spc_get(SPC_TRUE) : sta_spc_get(SPC_FALSE);
    return STA_PRIM_SUCCESS;
}

/* Prim 30: Object >> #class */
static int prim_class(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!g_prim_class_table) return STA_PRIM_NOT_AVAILABLE;
    uint32_t cls_idx;
    if (STA_IS_SMALLINT(args[0]))  cls_idx = STA_CLS_SMALLINTEGER;
    else if (STA_IS_CHAR(args[0])) cls_idx = STA_CLS_CHARACTER;
    else                           cls_idx = ((STA_ObjHeader *)(uintptr_t)args[0])->class_index;
    *result = sta_class_table_get(g_prim_class_table, cls_idx);
    return (*result != 0) ? STA_PRIM_SUCCESS : STA_PRIM_NOT_AVAILABLE;
}

/* Prim 42: Object >> #yourself */
static int prim_yourself(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    *result = args[0];
    return STA_PRIM_SUCCESS;
}

/* Prim 120: Object >> #respondsTo: */
static int prim_responds_to(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!g_prim_class_table) return STA_PRIM_NOT_AVAILABLE;
    STA_OOP receiver = args[0];
    STA_OOP selector = args[1];

    uint32_t cls_idx;
    if (STA_IS_SMALLINT(receiver))  cls_idx = STA_CLS_SMALLINTEGER;
    else if (STA_IS_CHAR(receiver)) cls_idx = STA_CLS_CHARACTER;
    else                            cls_idx = ((STA_ObjHeader *)(uintptr_t)receiver)->class_index;

    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP cls = sta_class_table_get(g_prim_class_table, cls_idx);
    while (cls != 0 && cls != nil_oop) {
        STA_OOP md = sta_class_method_dict(cls);
        if (md != 0) {
            if (sta_method_dict_lookup(md, selector) != 0) {
                *result = sta_spc_get(SPC_TRUE);
                return STA_PRIM_SUCCESS;
            }
        }
        cls = sta_class_superclass(cls);
    }
    *result = sta_spc_get(SPC_FALSE);
    return STA_PRIM_SUCCESS;
}

/* Prim 121: Object >> #doesNotUnderstand: (minimal — returns nil) */
static int prim_dnu(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs; (void)args;
    *result = sta_spc_get(SPC_NIL);
    return STA_PRIM_SUCCESS;
}

/* ── Object creation primitives ────────────────────────────────────────── */

/* Prim 31: Behavior >> #basicNew (fixed-size instance allocation) */
static int prim_basic_new(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!g_prim_class_table || !g_prim_heap) return STA_PRIM_NOT_AVAILABLE;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;

    /* Read the class's format field (slot 2). */
    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];

    /* Reject non-instantiable classes (immediate, compiled method). */
    if (!sta_format_is_instantiable(fmt)) return STA_PRIM_BAD_RECEIVER;

    /* Reject indexable classes — must use basicNew: instead. */
    if (sta_format_is_indexable(fmt)) return STA_PRIM_BAD_RECEIVER;

    /* Find this class's class table index. */
    uint32_t cls_idx = sta_class_table_index_of(g_prim_class_table, args[0]);
    if (cls_idx == 0) return STA_PRIM_BAD_RECEIVER;

    /* Allocate: header + instSize OOP-sized slots, all zeroed. */
    uint8_t inst_size = STA_FORMAT_INST_VARS(fmt);
    STA_ObjHeader *obj = sta_heap_alloc(g_prim_heap, cls_idx, inst_size);
    if (!obj) return STA_PRIM_NO_MEMORY;

    /* Initialize all slots to nil. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP *slots = sta_payload(obj);
    for (uint8_t i = 0; i < inst_size; i++) {
        slots[i] = nil_oop;
    }

    *result = (STA_OOP)(uintptr_t)obj;
    return STA_PRIM_SUCCESS;
}

/* Prim 32: Behavior >> #basicNew: (variable-size instance allocation) */
static int prim_basic_new_size(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!g_prim_class_table || !g_prim_heap) return STA_PRIM_NOT_AVAILABLE;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;

    intptr_t requested = STA_SMALLINT_VAL(args[1]);
    if (requested < 0) return STA_PRIM_OUT_OF_RANGE;

    /* Read the class's format field (slot 2). */
    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];

    /* Must be indexable and instantiable. */
    if (!sta_format_is_instantiable(fmt)) return STA_PRIM_BAD_RECEIVER;
    if (!sta_format_is_indexable(fmt)) return STA_PRIM_BAD_RECEIVER;

    /* Find this class's class table index. */
    uint32_t cls_idx = sta_class_table_index_of(g_prim_class_table, args[0]);
    if (cls_idx == 0) return STA_PRIM_BAD_RECEIVER;

    uint8_t inst_size = STA_FORMAT_INST_VARS(fmt);
    bool is_bytes = sta_format_is_bytes(fmt);
    uint32_t var_words;
    uint8_t byte_padding = 0;

    if (is_bytes) {
        /* Byte-indexable: round up to OOP-sized words. */
        var_words = ((uint32_t)requested + (uint32_t)(sizeof(STA_OOP) - 1))
                    / (uint32_t)sizeof(STA_OOP);
        byte_padding = (uint8_t)(var_words * sizeof(STA_OOP) - (uint32_t)requested);
    } else {
        /* Pointer-indexable: one word per element. */
        var_words = (uint32_t)requested;
    }

    uint32_t total_words = (uint32_t)inst_size + var_words;
    STA_ObjHeader *obj = sta_heap_alloc(g_prim_heap, cls_idx, total_words);
    if (!obj) return STA_PRIM_NO_MEMORY;

    /* Store byte padding in reserved field. */
    if (is_bytes) {
        obj->reserved = byte_padding;
    }

    /* Initialize all OOP-sized slots to nil. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP *slots = sta_payload(obj);
    for (uint32_t i = 0; i < total_words; i++) {
        slots[i] = nil_oop;
    }

    /* For byte-indexable objects, zero the byte region instead of nil.
     * The slots after inst_size hold raw bytes, not OOP values. */
    if (is_bytes && var_words > 0) {
        memset(&slots[inst_size], 0, var_words * sizeof(STA_OOP));
    }

    *result = (STA_OOP)(uintptr_t)obj;
    return STA_PRIM_SUCCESS;
}

/* ── Helper: get class format OOP for a heap object ────────────────────── */

/* Look up the receiver's class via class_index, read its format field.
 * Returns 0 if class table is unavailable or class not found. */
static STA_OOP get_receiver_format(STA_ObjHeader *h) {
    if (!g_prim_class_table) return 0;
    STA_OOP cls = sta_class_table_get(g_prim_class_table, h->class_index);
    if (cls == 0) return 0;
    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)cls;
    return sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];
}

/* ── Object and memory primitives (§8.5, prims 33–41) ─────────────────── */

/* Prim 33: Object >> #basicAt: */
static int prim_basic_at(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 1; /* not indexable */
    if (!STA_IS_SMALLINT(args[1])) return 3; /* arg not SmallInteger */

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = get_receiver_format(h);
    if (fmt == 0) return 1;

    if (!sta_format_is_indexable(fmt)) return 1; /* not indexable */

    uint8_t inst_vars = STA_FORMAT_INST_VARS(fmt);
    intptr_t idx = STA_SMALLINT_VAL(args[1]);

    if (sta_format_is_bytes(fmt)) {
        /* Byte-indexable: compute exact byte count. */
        uint32_t var_words = h->size - inst_vars;
        uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP)
                              - STA_BYTE_PADDING(h);
        if (idx < 1 || (uint32_t)idx > byte_count) return 2;
        uint8_t *bytes = (uint8_t *)&sta_payload(h)[inst_vars];
        *result = STA_SMALLINT_OOP(bytes[idx - 1]);
    } else {
        /* Pointer-indexable. */
        uint32_t var_count = h->size - inst_vars;
        if (idx < 1 || (uint32_t)idx > var_count) return 2;
        *result = sta_payload(h)[inst_vars + idx - 1];
    }
    return STA_PRIM_SUCCESS;
}

/* Prim 34: Object >> #basicAt:put: */
static int prim_basic_at_put(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 1;
    if (!STA_IS_SMALLINT(args[1])) return 3;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (h->obj_flags & STA_OBJ_IMMUTABLE) return 4;

    STA_OOP fmt = get_receiver_format(h);
    if (fmt == 0) return 1;
    if (!sta_format_is_indexable(fmt)) return 1;

    uint8_t inst_vars = STA_FORMAT_INST_VARS(fmt);
    intptr_t idx = STA_SMALLINT_VAL(args[1]);

    if (sta_format_is_bytes(fmt)) {
        uint32_t var_words = h->size - inst_vars;
        uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP)
                              - STA_BYTE_PADDING(h);
        if (idx < 1 || (uint32_t)idx > byte_count) return 2;
        if (!STA_IS_SMALLINT(args[2])) return 3;
        intptr_t val = STA_SMALLINT_VAL(args[2]);
        if (val < 0 || val > 255) return 5;
        uint8_t *bytes = (uint8_t *)&sta_payload(h)[inst_vars];
        bytes[idx - 1] = (uint8_t)val;
        *result = args[2];
    } else {
        uint32_t var_count = h->size - inst_vars;
        if (idx < 1 || (uint32_t)idx > var_count) return 2;
        sta_payload(h)[inst_vars + idx - 1] = args[2];
        *result = args[2];
    }
    return STA_PRIM_SUCCESS;
}

/* Prim 35: Object >> #basicSize */
static int prim_basic_size(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) {
        *result = STA_SMALLINT_OOP(0);
        return STA_PRIM_SUCCESS;
    }

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = get_receiver_format(h);
    if (fmt == 0) {
        *result = STA_SMALLINT_OOP(0);
        return STA_PRIM_SUCCESS;
    }

    if (!sta_format_is_indexable(fmt)) {
        *result = STA_SMALLINT_OOP(0);
        return STA_PRIM_SUCCESS;
    }

    uint8_t inst_vars = STA_FORMAT_INST_VARS(fmt);
    if (sta_format_is_bytes(fmt)) {
        uint32_t var_words = h->size - inst_vars;
        uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP)
                              - STA_BYTE_PADDING(h);
        *result = STA_SMALLINT_OOP((intptr_t)byte_count);
    } else {
        uint32_t var_count = h->size - inst_vars;
        *result = STA_SMALLINT_OOP((intptr_t)var_count);
    }
    return STA_PRIM_SUCCESS;
}

/* Prim 36: Object >> #hash */
static int prim_hash(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_SMALLINT(args[0])) {
        /* SmallInteger: hash is the value itself. */
        *result = args[0];
        return STA_PRIM_SUCCESS;
    }
    if (STA_IS_CHAR(args[0])) {
        /* Character: hash is the code point. */
        *result = STA_SMALLINT_OOP((intptr_t)STA_CHAR_VAL(args[0]));
        return STA_PRIM_SUCCESS;
    }

    /* Heap object. Symbols use FNV-1a from slot 0. */
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (h->class_index == STA_CLS_SYMBOL) {
        uint32_t fnv = sta_symbol_get_hash(args[0]);
        /* Mask to positive SmallInteger range. */
        *result = STA_SMALLINT_OOP((intptr_t)(fnv & 0x3FFFFFFFu));
        return STA_PRIM_SUCCESS;
    }

    /* Other heap objects: use identity hash (prim 40 logic). */
    uintptr_t addr = (uintptr_t)args[0];
    intptr_t hash = (intptr_t)((addr >> 4) & 0x3FFFFFFFu);
    *result = STA_SMALLINT_OOP(hash);
    return STA_PRIM_SUCCESS;
}

/* Prim 37: Object >> #become: */
static int prim_become(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 1;
    if (STA_IS_IMMEDIATE(args[1])) return 1;

    /* become: yourself is a no-op. */
    if (args[0] == args[1]) {
        *result = args[0];
        return STA_PRIM_SUCCESS;
    }

    STA_ObjHeader *a = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_ObjHeader *b = (STA_ObjHeader *)(uintptr_t)args[1];

    if (a->obj_flags & STA_OBJ_IMMUTABLE) return 1;
    if (b->obj_flags & STA_OBJ_IMMUTABLE) return 1;

    /* Both must have same total allocation size. */
    size_t size_a = sta_alloc_size(a->size);
    size_t size_b = sta_alloc_size(b->size);
    if (size_a != size_b) return 6;

    /* Swap via temp buffer.
     * TODO: Phase 2 — add cross-actor check. */
    size_t total = size_a;
    /* Use stack buffer for small objects, heap for larger. */
    uint8_t stack_buf[256];
    uint8_t *tmp = (total <= sizeof(stack_buf)) ? stack_buf : (uint8_t *)malloc(total);
    if (!tmp) return 6;

    memcpy(tmp, a, total);
    memcpy(a, b, total);
    memcpy(b, tmp, total);

    if (tmp != stack_buf) free(tmp);

    *result = args[0];
    return STA_PRIM_SUCCESS;
}

/* Prim 38: Object >> #instVarAt: */
static int prim_inst_var_at(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 2; /* no named instVars */
    if (!STA_IS_SMALLINT(args[1])) return 3;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = get_receiver_format(h);
    if (fmt == 0) return 2;

    uint8_t inst_var_count = STA_FORMAT_INST_VARS(fmt);
    intptr_t idx = STA_SMALLINT_VAL(args[1]);

    if (idx < 1 || (uint32_t)idx > inst_var_count) return 2;

    *result = sta_payload(h)[idx - 1];
    return STA_PRIM_SUCCESS;
}

/* Prim 39: Object >> #instVarAt:put: */
static int prim_inst_var_at_put(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 2;
    if (!STA_IS_SMALLINT(args[1])) return 3;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (h->obj_flags & STA_OBJ_IMMUTABLE) return 4;

    STA_OOP fmt = get_receiver_format(h);
    if (fmt == 0) return 2;

    uint8_t inst_var_count = STA_FORMAT_INST_VARS(fmt);
    intptr_t idx = STA_SMALLINT_VAL(args[1]);

    if (idx < 1 || (uint32_t)idx > inst_var_count) return 2;

    sta_payload(h)[idx - 1] = args[2];
    *result = args[2];
    return STA_PRIM_SUCCESS;
}

/* Prim 40: Object >> #identityHash */
static int prim_identity_hash(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_SMALLINT(args[0])) {
        /* SmallInteger: derived from value. */
        *result = STA_SMALLINT_OOP((intptr_t)(STA_SMALLINT_VAL(args[0]) & 0x3FFFFFFFu));
        return STA_PRIM_SUCCESS;
    }
    if (STA_IS_CHAR(args[0])) {
        /* Character: derived from code point. */
        *result = STA_SMALLINT_OOP((intptr_t)(STA_CHAR_VAL(args[0]) & 0x3FFFFFFFu));
        return STA_PRIM_SUCCESS;
    }

    /* Heap objects: address-derived hash.
     * IMPORTANT: Phase 1 uses non-moving GC, so address is stable.
     * Phase 2 (compacting GC) will require a stored hash field. */
    uintptr_t addr = (uintptr_t)args[0];
    intptr_t hash = (intptr_t)((addr >> 4) & 0x3FFFFFFFu);
    *result = STA_SMALLINT_OOP(hash);
    return STA_PRIM_SUCCESS;
}

/* Prim 41: Object >> #shallowCopy */
static int prim_shallow_copy(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;

    /* Immediates: return self (they are values, not heap objects). */
    if (STA_IS_IMMEDIATE(args[0])) {
        *result = args[0];
        return STA_PRIM_SUCCESS;
    }

    if (!g_prim_heap) return 7;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_ObjHeader *copy = sta_heap_alloc(g_prim_heap, h->class_index, h->size);
    if (!copy) return 7;

    /* Copy payload verbatim. */
    memcpy(sta_payload(copy), sta_payload(h), (size_t)h->size * sizeof(STA_OOP));

    /* Preserve reserved field (byte padding). */
    copy->reserved = h->reserved;

    /* The copy is always mutable — clear immutable flag. */
    copy->obj_flags &= (uint8_t)~STA_OBJ_IMMUTABLE;

    *result = (STA_OOP)(uintptr_t)copy;
    return STA_PRIM_SUCCESS;
}

/* ── Array primitives ──────────────────────────────────────────────────── */

/* Prim 51: Array >> #at: */
static int prim_array_at(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    intptr_t idx = STA_SMALLINT_VAL(args[1]);

    /* 1-based indexing per Smalltalk convention. */
    if (idx < 1 || (uint32_t)idx > h->size) return STA_PRIM_OUT_OF_RANGE;

    STA_OOP *payload = sta_payload(h);
    *result = payload[idx - 1];
    return STA_PRIM_SUCCESS;
}

/* Prim 52: Array >> #at:put: */
static int prim_array_at_put(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    intptr_t idx = STA_SMALLINT_VAL(args[1]);

    if (idx < 1 || (uint32_t)idx > h->size) return STA_PRIM_OUT_OF_RANGE;

    STA_OOP *payload = sta_payload(h);
    payload[idx - 1] = args[2];
    *result = args[2];
    return STA_PRIM_SUCCESS;
}

/* Prim 53: Array >> #size */
static int prim_array_size(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    *result = STA_SMALLINT_OOP((intptr_t)h->size);
    return STA_PRIM_SUCCESS;
}

/* ── Exception primitives (§7.8, §8.8) ─────────────────────────────────── */

/* Prim 88: BlockClosure >> #on:do:
 * args[0] = body block (receiver), args[1] = exception class,
 * args[2] = handler block.
 * Uses setjmp/longjmp for signal → on:do: transfer. */
static int prim_on_do(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!g_prim_slab || !g_prim_heap || !g_prim_class_table)
        return STA_PRIM_NOT_AVAILABLE;

    STA_OOP body_block    = args[0];
    STA_OOP exc_class     = args[1];
    STA_OOP handler_block = args[2];

    STA_HandlerEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.exception_class = exc_class;
    entry.handler_block   = handler_block;
    entry.saved_slab_top  = g_prim_slab->top;
    entry.saved_slab_sp   = g_prim_slab->sp;

    if (setjmp(entry.jmp) == 0) {
        /* Normal path: push handler, evaluate body. */
        sta_handler_push(&entry);
        STA_OOP body_result = sta_eval_block(g_prim_slab, g_prim_heap,
                                               g_prim_class_table,
                                               body_block, NULL, 0);
        sta_handler_pop();
        *result = body_result;
        return STA_PRIM_SUCCESS;
    } else {
        /* Signal transferred control here via longjmp.
         * The handler chain was already popped by the signal primitive.
         * Restore slab state to discard frames from the aborted body. */
        g_prim_slab->top = entry.saved_slab_top;
        g_prim_slab->sp  = entry.saved_slab_sp;

        STA_OOP exc = sta_handler_get_signaled_exception();
        STA_OOP handler_args[1] = { exc };
        STA_OOP handler_result = sta_eval_block(g_prim_slab, g_prim_heap,
                                                  g_prim_class_table,
                                                  handler_block,
                                                  handler_args, 1);
        *result = handler_result;
        return STA_PRIM_SUCCESS;
    }
}

/* Prim 89: Exception >> #signal
 * args[0] = exception instance (receiver). No arguments.
 * Walks the handler chain, longjmp to matching on:do:. */
static int prim_signal(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs; (void)result;
    if (!g_prim_class_table) return STA_PRIM_NOT_AVAILABLE;

    STA_OOP exception = args[0];

    STA_HandlerEntry *entry = sta_handler_find(exception, g_prim_class_table);
    if (entry) {
        /* Pop all handlers down to and including the matched entry. */
        sta_handler_set_top(entry->prev);

        /* Store exception in global (avoids volatile issues with setjmp). */
        sta_handler_set_signaled_exception(exception);

        /* Transfer control to the matching on:do: primitive. */
        longjmp(entry->jmp, 1);
        /* NOTREACHED */
    }

    /* No handler found — unhandled exception. */
    fprintf(stderr, "Unhandled exception (class index %u)\n",
            STA_IS_HEAP(exception)
                ? ((STA_ObjHeader *)(uintptr_t)exception)->class_index
                : 0u);
    abort();

    return STA_PRIM_NOT_AVAILABLE;  /* unreachable, silences warning */
}

/* Prim 90: BlockClosure >> #ensure:
 * args[0] = body block (receiver), args[1] = ensure block.
 * Phase 1: normal completion only — ensure block does NOT fire
 * on exception or NLR unwinding (that is Phase 2). */
static int prim_ensure(STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!g_prim_slab || !g_prim_heap || !g_prim_class_table)
        return STA_PRIM_NOT_AVAILABLE;

    STA_OOP body_block   = args[0];
    STA_OOP ensure_block = args[1];

    /* Evaluate body block. */
    STA_OOP body_result = sta_eval_block(g_prim_slab, g_prim_heap,
                                           g_prim_class_table,
                                           body_block, NULL, 0);

    /* Unconditionally evaluate ensure block (discard result). */
    (void)sta_eval_block(g_prim_slab, g_prim_heap,
                          g_prim_class_table,
                          ensure_block, NULL, 0);

    *result = body_result;
    return STA_PRIM_SUCCESS;
}

/* ── Table initialization ──────────────────────────────────────────────── */

void sta_primitive_table_init(void) {
    memset(sta_primitives, 0, sizeof(sta_primitives));

    /* SmallInteger arithmetic (§8.3) */
    sta_primitives[1]  = prim_smallint_add;    /* #+ */
    sta_primitives[2]  = prim_smallint_sub;    /* #- */
    sta_primitives[3]  = prim_smallint_lt;     /* #< */
    sta_primitives[4]  = prim_smallint_gt;     /* #> */
    sta_primitives[7]  = prim_smallint_eq;     /* #= */
    sta_primitives[9]  = prim_smallint_mul;    /* #* */

    /* Object identity and class (§8.4) */
    sta_primitives[29] = prim_identity;        /* #== */
    sta_primitives[30] = prim_class;           /* #class */

    /* Object creation (§8.5) */
    sta_primitives[31] = prim_basic_new;       /* #basicNew */
    sta_primitives[32] = prim_basic_new_size;  /* #basicNew: */

    /* Object and memory (§8.5) */
    sta_primitives[33] = prim_basic_at;        /* #basicAt: */
    sta_primitives[34] = prim_basic_at_put;    /* #basicAt:put: */
    sta_primitives[35] = prim_basic_size;      /* #basicSize */
    sta_primitives[36] = prim_hash;            /* #hash */
    sta_primitives[37] = prim_become;          /* #become: */
    sta_primitives[38] = prim_inst_var_at;     /* #instVarAt: */
    sta_primitives[39] = prim_inst_var_at_put; /* #instVarAt:put: */
    sta_primitives[40] = prim_identity_hash;   /* #identityHash */
    sta_primitives[41] = prim_shallow_copy;    /* #shallowCopy */

    /* Object (§8.4) */
    sta_primitives[42]  = prim_yourself;       /* #yourself */

    /* Array access (§8.6) */
    sta_primitives[51] = prim_array_at;        /* #at: */
    sta_primitives[52] = prim_array_at_put;    /* #at:put: */
    sta_primitives[53] = prim_array_size;      /* #size */

    /* Exception handling (§7.8, §8.8) */
    sta_primitives[88]  = prim_on_do;          /* BlockClosure >> #on:do: */
    sta_primitives[89]  = prim_signal;         /* Exception >> #signal */
    sta_primitives[90]  = prim_ensure;         /* BlockClosure >> #ensure: */

    /* Reflection / error handling */
    sta_primitives[120] = prim_responds_to;    /* #respondsTo: */
    sta_primitives[121] = prim_dnu;            /* #doesNotUnderstand: */
}
