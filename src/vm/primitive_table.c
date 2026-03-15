/* src/vm/primitive_table.c
 * Primitive function table — see primitive_table.h for documentation.
 */
#include "primitive_table.h"
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
