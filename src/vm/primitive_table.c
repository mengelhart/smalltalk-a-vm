/* src/vm/primitive_table.c
 * Primitive function table — see primitive_table.h for documentation.
 */
#include "primitive_table.h"
#include "format.h"
#include "interpreter.h"
#include "method_dict.h"
#include "special_objects.h"
#include <string.h>

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

    /* Object (§8.4) */
    sta_primitives[42]  = prim_yourself;       /* #yourself */

    /* Array access (§8.6) */
    sta_primitives[51] = prim_array_at;        /* #at: */
    sta_primitives[52] = prim_array_at_put;    /* #at:put: */
    sta_primitives[53] = prim_array_size;      /* #size */

    /* Reflection / error handling */
    sta_primitives[120] = prim_responds_to;    /* #respondsTo: */
    sta_primitives[121] = prim_dnu;            /* #doesNotUnderstand: */
}
