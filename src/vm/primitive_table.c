/* src/vm/primitive_table.c
 * Primitive function table — Phase 2 STA_ExecContext calling convention.
 * All primitives receive ctx->vm for access to VM state.
 */
#include "primitive_table.h"
#include "vm_state.h"
#include "format.h"
#include "handler.h"
#include "interpreter.h"
#include "method_dict.h"
#include "special_objects.h"
#include "symbol_table.h"
#include "immutable_space.h"
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <stdio.h>

STA_PrimFn sta_primitives[STA_PRIM_TABLE_SIZE];

/* ── SmallInteger arithmetic primitives ────────────────────────────────── */

static int prim_smallint_add(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    intptr_t r;
    if (__builtin_add_overflow(a, b, &r)) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_SMALLINT_OOP(r);
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_sub(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    intptr_t r;
    if (__builtin_sub_overflow(a, b, &r)) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_SMALLINT_OOP(r);
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_lt(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = (STA_SMALLINT_VAL(args[0]) < STA_SMALLINT_VAL(args[1]))
              ? ctx->vm->specials[SPC_TRUE] : ctx->vm->specials[SPC_FALSE];
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_gt(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = (STA_SMALLINT_VAL(args[0]) > STA_SMALLINT_VAL(args[1]))
              ? ctx->vm->specials[SPC_TRUE] : ctx->vm->specials[SPC_FALSE];
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_eq(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = (args[0] == args[1])
              ? ctx->vm->specials[SPC_TRUE] : ctx->vm->specials[SPC_FALSE];
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_mul(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    intptr_t r;
    if (__builtin_mul_overflow(a, b, &r)) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_SMALLINT_OOP(r);
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_le(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = (STA_SMALLINT_VAL(args[0]) <= STA_SMALLINT_VAL(args[1]))
              ? ctx->vm->specials[SPC_TRUE] : ctx->vm->specials[SPC_FALSE];
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_ge(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = (STA_SMALLINT_VAL(args[0]) >= STA_SMALLINT_VAL(args[1]))
              ? ctx->vm->specials[SPC_TRUE] : ctx->vm->specials[SPC_FALSE];
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_ne(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = (args[0] != args[1])
              ? ctx->vm->specials[SPC_TRUE] : ctx->vm->specials[SPC_FALSE];
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_div_exact(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    if (b == 0) return STA_PRIM_OUT_OF_RANGE;
    if (a % b != 0) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_SMALLINT_OOP(a / b);
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_mod(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    if (b == 0) return STA_PRIM_OUT_OF_RANGE;
    intptr_t r = a % b;
    if (r != 0 && ((r ^ b) < 0)) r += b;
    *result = STA_SMALLINT_OOP(r);
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_div_floor(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    if (b == 0) return STA_PRIM_OUT_OF_RANGE;
    intptr_t q = a / b;
    intptr_t r = a % b;
    if (r != 0 && ((a ^ b) < 0)) q -= 1;
    *result = STA_SMALLINT_OOP(q);
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_quo(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t b = STA_SMALLINT_VAL(args[1]);
    if (b == 0) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_SMALLINT_OOP(a / b);
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_bitand(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = STA_SMALLINT_OOP(STA_SMALLINT_VAL(args[0]) & STA_SMALLINT_VAL(args[1]));
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_bitor(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = STA_SMALLINT_OOP(STA_SMALLINT_VAL(args[0]) | STA_SMALLINT_VAL(args[1]));
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_bitxor(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    *result = STA_SMALLINT_OOP(STA_SMALLINT_VAL(args[0]) ^ STA_SMALLINT_VAL(args[1]));
    return STA_PRIM_SUCCESS;
}

static int prim_smallint_bitshift(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t a = STA_SMALLINT_VAL(args[0]);
    intptr_t shift = STA_SMALLINT_VAL(args[1]);
    if (shift >= 63 || shift <= -63) return STA_PRIM_OUT_OF_RANGE;
    intptr_t r;
    if (shift >= 0) {
        uintptr_t ua = (uintptr_t)a;
        uintptr_t shifted = ua << shift;
        r = (intptr_t)shifted;
        if ((r >> shift) != a) return STA_PRIM_OUT_OF_RANGE;
    } else {
        r = a >> (-shift);
    }
    *result = STA_SMALLINT_OOP(r);
    return STA_PRIM_SUCCESS;
}

/* Prim 200: SmallInteger >> #printString */
static int prim_smallint_printstring(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[0])) return STA_PRIM_BAD_RECEIVER;

    intptr_t val = STA_SMALLINT_VAL(args[0]);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%ld", (long)val);
    if (len <= 0) return STA_PRIM_OUT_OF_RANGE;

    uint32_t var_words = ((uint32_t)len + (uint32_t)(sizeof(STA_OOP) - 1))
                         / (uint32_t)sizeof(STA_OOP);
    STA_ObjHeader *str_h = sta_heap_alloc(&ctx->vm->heap, STA_CLS_STRING, var_words);
    if (!str_h) return STA_PRIM_NO_MEMORY;

    str_h->reserved = (uint8_t)(var_words * sizeof(STA_OOP) - (uint32_t)len);
    memset(sta_payload(str_h), 0, var_words * sizeof(STA_OOP));
    memcpy(sta_payload(str_h), buf, (size_t)len);

    *result = (STA_OOP)(uintptr_t)str_h;
    return STA_PRIM_SUCCESS;
}

/* Forward declaration */
static STA_OOP get_receiver_format(STA_ClassTable *ct, STA_ObjHeader *h);

/* ── Byte-indexable primitives ──────────────────────────────────────────── */

static int byte_access_setup(STA_ClassTable *ct, STA_OOP receiver,
                              uint8_t **out_bytes, uint32_t *out_byte_count) {
    if (STA_IS_IMMEDIATE(receiver)) return STA_PRIM_BAD_RECEIVER;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)receiver;
    STA_OOP fmt = get_receiver_format(ct, h);
    if (fmt == 0) return STA_PRIM_BAD_RECEIVER;
    if (!sta_format_is_bytes(fmt)) return STA_PRIM_BAD_RECEIVER;

    uint8_t inst_vars = STA_FORMAT_INST_VARS(fmt);
    uint32_t var_words = h->size - inst_vars;
    uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP)
                          - STA_BYTE_PADDING(h);
    *out_bytes = (uint8_t *)&sta_payload(h)[inst_vars];
    *out_byte_count = byte_count;
    return STA_PRIM_SUCCESS;
}

static int prim_byte_at(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    uint8_t *bytes; uint32_t byte_count;
    int rc = byte_access_setup(&ctx->vm->class_table, args[0], &bytes, &byte_count);
    if (rc != STA_PRIM_SUCCESS) return rc;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t idx = STA_SMALLINT_VAL(args[1]);
    if (idx < 1 || (uint32_t)idx > byte_count) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_SMALLINT_OOP(bytes[idx - 1]);
    return STA_PRIM_SUCCESS;
}

static int prim_byte_at_put(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (h->obj_flags & STA_OBJ_IMMUTABLE) return STA_PRIM_BAD_RECEIVER;
    uint8_t *bytes; uint32_t byte_count;
    int rc = byte_access_setup(&ctx->vm->class_table, args[0], &bytes, &byte_count);
    if (rc != STA_PRIM_SUCCESS) return rc;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    if (!STA_IS_SMALLINT(args[2])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t idx = STA_SMALLINT_VAL(args[1]);
    intptr_t val = STA_SMALLINT_VAL(args[2]);
    if (idx < 1 || (uint32_t)idx > byte_count) return STA_PRIM_OUT_OF_RANGE;
    if (val < 0 || val > 255) return STA_PRIM_OUT_OF_RANGE;
    bytes[idx - 1] = (uint8_t)val;
    *result = args[2];
    return STA_PRIM_SUCCESS;
}

static int prim_byte_size(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    uint8_t *bytes; uint32_t byte_count;
    int rc = byte_access_setup(&ctx->vm->class_table, args[0], &bytes, &byte_count);
    if (rc != STA_PRIM_SUCCESS) return rc;
    *result = STA_SMALLINT_OOP((intptr_t)byte_count);
    return STA_PRIM_SUCCESS;
}

static int prim_string_at(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    uint8_t *bytes; uint32_t byte_count;
    int rc = byte_access_setup(&ctx->vm->class_table, args[0], &bytes, &byte_count);
    if (rc != STA_PRIM_SUCCESS) return rc;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t idx = STA_SMALLINT_VAL(args[1]);
    if (idx < 1 || (uint32_t)idx > byte_count) return STA_PRIM_OUT_OF_RANGE;
    *result = STA_CHAR_OOP(bytes[idx - 1]);
    return STA_PRIM_SUCCESS;
}

static int prim_string_at_put(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (h->obj_flags & STA_OBJ_IMMUTABLE) return STA_PRIM_BAD_RECEIVER;
    uint8_t *bytes; uint32_t byte_count;
    int rc = byte_access_setup(&ctx->vm->class_table, args[0], &bytes, &byte_count);
    if (rc != STA_PRIM_SUCCESS) return rc;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    if (!STA_IS_CHAR(args[2])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t idx = STA_SMALLINT_VAL(args[1]);
    uint32_t cp = STA_CHAR_VAL(args[2]);
    if (idx < 1 || (uint32_t)idx > byte_count) return STA_PRIM_OUT_OF_RANGE;
    if (cp > 255) return STA_PRIM_OUT_OF_RANGE;
    bytes[idx - 1] = (uint8_t)cp;
    *result = args[2];
    return STA_PRIM_SUCCESS;
}

/* ── Character primitives ──────────────────────────────────────────────── */

static int prim_char_value(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (!STA_IS_CHAR(args[0])) return STA_PRIM_BAD_RECEIVER;
    *result = STA_SMALLINT_OOP((intptr_t)STA_CHAR_VAL(args[0]));
    return STA_PRIM_SUCCESS;
}

static int prim_char_for(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    intptr_t cp = STA_SMALLINT_VAL(args[1]);
    if (cp < 0) return STA_PRIM_OUT_OF_RANGE;
    if (cp <= 255) {
        STA_OOP table = ctx->vm->specials[SPC_CHARACTER_TABLE];
        if (table != 0) {
            STA_ObjHeader *arr_h = (STA_ObjHeader *)(uintptr_t)table;
            *result = sta_payload(arr_h)[cp];
            return STA_PRIM_SUCCESS;
        }
    }
    *result = STA_CHAR_OOP((uint32_t)cp);
    return STA_PRIM_SUCCESS;
}

/* ── Object primitives ─────────────────────────────────────────────────── */

static int prim_identity(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    *result = (args[0] == args[1])
              ? ctx->vm->specials[SPC_TRUE] : ctx->vm->specials[SPC_FALSE];
    return STA_PRIM_SUCCESS;
}

static int prim_class(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    uint32_t cls_idx;
    if (STA_IS_SMALLINT(args[0]))  cls_idx = STA_CLS_SMALLINTEGER;
    else if (STA_IS_CHAR(args[0])) cls_idx = STA_CLS_CHARACTER;
    else                           cls_idx = ((STA_ObjHeader *)(uintptr_t)args[0])->class_index;
    *result = sta_class_table_get(&ctx->vm->class_table, cls_idx);
    return (*result != 0) ? STA_PRIM_SUCCESS : STA_PRIM_NOT_AVAILABLE;
}

static int prim_yourself(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    *result = args[0];
    return STA_PRIM_SUCCESS;
}

static int prim_responds_to(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    STA_OOP receiver = args[0];
    STA_OOP selector = args[1];

    uint32_t cls_idx;
    if (STA_IS_SMALLINT(receiver))  cls_idx = STA_CLS_SMALLINTEGER;
    else if (STA_IS_CHAR(receiver)) cls_idx = STA_CLS_CHARACTER;
    else                            cls_idx = ((STA_ObjHeader *)(uintptr_t)receiver)->class_index;

    STA_OOP nil_oop = ctx->vm->specials[SPC_NIL];
    STA_OOP cls = sta_class_table_get(&ctx->vm->class_table, cls_idx);
    while (cls != 0 && cls != nil_oop) {
        STA_OOP md = sta_class_method_dict(cls);
        if (md != 0) {
            if (sta_method_dict_lookup(md, selector) != 0) {
                *result = ctx->vm->specials[SPC_TRUE];
                return STA_PRIM_SUCCESS;
            }
        }
        cls = sta_class_superclass(cls);
    }
    *result = ctx->vm->specials[SPC_FALSE];
    return STA_PRIM_SUCCESS;
}

static int prim_dnu(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs; (void)args;
    *result = ctx->vm->specials[SPC_NIL];
    return STA_PRIM_SUCCESS;
}

/* ── Object creation primitives ────────────────────────────────────────── */

static int prim_basic_new(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;

    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];
    if (!sta_format_is_instantiable(fmt)) return STA_PRIM_BAD_RECEIVER;
    if (sta_format_is_indexable(fmt)) return STA_PRIM_BAD_RECEIVER;

    uint32_t cls_idx = sta_class_table_index_of(&ctx->vm->class_table, args[0]);
    if (cls_idx == 0) return STA_PRIM_BAD_RECEIVER;

    uint8_t inst_size = STA_FORMAT_INST_VARS(fmt);
    STA_ObjHeader *obj = sta_heap_alloc(&ctx->vm->heap, cls_idx, inst_size);
    if (!obj) return STA_PRIM_NO_MEMORY;

    STA_OOP nil_oop = ctx->vm->specials[SPC_NIL];
    STA_OOP *slots = sta_payload(obj);
    for (uint8_t i = 0; i < inst_size; i++)
        slots[i] = nil_oop;

    *result = (STA_OOP)(uintptr_t)obj;
    return STA_PRIM_SUCCESS;
}

static int prim_basic_new_size(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;

    intptr_t requested = STA_SMALLINT_VAL(args[1]);
    if (requested < 0) return STA_PRIM_OUT_OF_RANGE;

    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];
    if (!sta_format_is_instantiable(fmt)) return STA_PRIM_BAD_RECEIVER;
    if (!sta_format_is_indexable(fmt)) return STA_PRIM_BAD_RECEIVER;

    uint32_t cls_idx = sta_class_table_index_of(&ctx->vm->class_table, args[0]);
    if (cls_idx == 0) return STA_PRIM_BAD_RECEIVER;

    uint8_t inst_size = STA_FORMAT_INST_VARS(fmt);
    bool is_bytes = sta_format_is_bytes(fmt);
    uint32_t var_words;
    uint8_t byte_padding = 0;

    if (is_bytes) {
        var_words = ((uint32_t)requested + (uint32_t)(sizeof(STA_OOP) - 1))
                    / (uint32_t)sizeof(STA_OOP);
        byte_padding = (uint8_t)(var_words * sizeof(STA_OOP) - (uint32_t)requested);
    } else {
        var_words = (uint32_t)requested;
    }

    uint32_t total_words = (uint32_t)inst_size + var_words;
    STA_ObjHeader *obj = sta_heap_alloc(&ctx->vm->heap, cls_idx, total_words);
    if (!obj) return STA_PRIM_NO_MEMORY;

    if (is_bytes) obj->reserved = byte_padding;

    STA_OOP nil_oop = ctx->vm->specials[SPC_NIL];
    STA_OOP *slots = sta_payload(obj);
    for (uint32_t i = 0; i < total_words; i++)
        slots[i] = nil_oop;

    if (is_bytes && var_words > 0)
        memset(&slots[inst_size], 0, var_words * sizeof(STA_OOP));

    *result = (STA_OOP)(uintptr_t)obj;
    return STA_PRIM_SUCCESS;
}

/* ── Helper: get class format OOP for a heap object ────────────────────── */

static STA_OOP get_receiver_format(STA_ClassTable *ct, STA_ObjHeader *h) {
    STA_OOP cls = sta_class_table_get(ct, h->class_index);
    if (cls == 0) return 0;
    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)cls;
    return sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];
}

/* ── Object and memory primitives (§8.5, prims 33–41) ─────────────────── */

static int prim_basic_at(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 1;
    if (!STA_IS_SMALLINT(args[1])) return 3;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = get_receiver_format(&ctx->vm->class_table, h);
    if (fmt == 0) return 1;
    if (!sta_format_is_indexable(fmt)) return 1;

    uint8_t inst_vars = STA_FORMAT_INST_VARS(fmt);
    intptr_t idx = STA_SMALLINT_VAL(args[1]);

    if (sta_format_is_bytes(fmt)) {
        uint32_t var_words = h->size - inst_vars;
        uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
        if (idx < 1 || (uint32_t)idx > byte_count) return 2;
        uint8_t *bytes = (uint8_t *)&sta_payload(h)[inst_vars];
        *result = STA_SMALLINT_OOP(bytes[idx - 1]);
    } else {
        uint32_t var_count = h->size - inst_vars;
        if (idx < 1 || (uint32_t)idx > var_count) return 2;
        *result = sta_payload(h)[inst_vars + idx - 1];
    }
    return STA_PRIM_SUCCESS;
}

static int prim_basic_at_put(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 1;
    if (!STA_IS_SMALLINT(args[1])) return 3;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (h->obj_flags & STA_OBJ_IMMUTABLE) return 4;

    STA_OOP fmt = get_receiver_format(&ctx->vm->class_table, h);
    if (fmt == 0) return 1;
    if (!sta_format_is_indexable(fmt)) return 1;

    uint8_t inst_vars = STA_FORMAT_INST_VARS(fmt);
    intptr_t idx = STA_SMALLINT_VAL(args[1]);

    if (sta_format_is_bytes(fmt)) {
        uint32_t var_words = h->size - inst_vars;
        uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
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

static int prim_basic_size(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) {
        *result = STA_SMALLINT_OOP(0);
        return STA_PRIM_SUCCESS;
    }

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = get_receiver_format(&ctx->vm->class_table, h);
    if (fmt == 0 || !sta_format_is_indexable(fmt)) {
        *result = STA_SMALLINT_OOP(0);
        return STA_PRIM_SUCCESS;
    }

    uint8_t inst_vars = STA_FORMAT_INST_VARS(fmt);
    if (sta_format_is_bytes(fmt)) {
        uint32_t var_words = h->size - inst_vars;
        uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
        *result = STA_SMALLINT_OOP((intptr_t)byte_count);
    } else {
        *result = STA_SMALLINT_OOP((intptr_t)(h->size - inst_vars));
    }
    return STA_PRIM_SUCCESS;
}

static int prim_hash(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (STA_IS_SMALLINT(args[0])) {
        *result = args[0];
        return STA_PRIM_SUCCESS;
    }
    if (STA_IS_CHAR(args[0])) {
        *result = STA_SMALLINT_OOP((intptr_t)STA_CHAR_VAL(args[0]));
        return STA_PRIM_SUCCESS;
    }
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (h->class_index == STA_CLS_SYMBOL) {
        uint32_t fnv = sta_symbol_get_hash(args[0]);
        *result = STA_SMALLINT_OOP((intptr_t)(fnv & 0x3FFFFFFFu));
        return STA_PRIM_SUCCESS;
    }
    uintptr_t addr = (uintptr_t)args[0];
    *result = STA_SMALLINT_OOP((intptr_t)((addr >> 4) & 0x3FFFFFFFu));
    return STA_PRIM_SUCCESS;
}

static int prim_become(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 1;
    if (STA_IS_IMMEDIATE(args[1])) return 1;
    if (args[0] == args[1]) { *result = args[0]; return STA_PRIM_SUCCESS; }

    STA_ObjHeader *a = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_ObjHeader *b = (STA_ObjHeader *)(uintptr_t)args[1];
    if (a->obj_flags & STA_OBJ_IMMUTABLE) return 1;
    if (b->obj_flags & STA_OBJ_IMMUTABLE) return 1;

    size_t size_a = sta_alloc_size(a->size);
    size_t size_b = sta_alloc_size(b->size);
    if (size_a != size_b) return 6;

    size_t total = size_a;
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

static int prim_inst_var_at(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 2;
    if (!STA_IS_SMALLINT(args[1])) return 3;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_OOP fmt = get_receiver_format(&ctx->vm->class_table, h);
    if (fmt == 0) return 2;

    uint8_t inst_var_count = STA_FORMAT_INST_VARS(fmt);
    intptr_t idx = STA_SMALLINT_VAL(args[1]);
    if (idx < 1 || (uint32_t)idx > inst_var_count) return 2;

    *result = sta_payload(h)[idx - 1];
    return STA_PRIM_SUCCESS;
}

static int prim_inst_var_at_put(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return 2;
    if (!STA_IS_SMALLINT(args[1])) return 3;

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (h->obj_flags & STA_OBJ_IMMUTABLE) return 4;

    STA_OOP fmt = get_receiver_format(&ctx->vm->class_table, h);
    if (fmt == 0) return 2;

    uint8_t inst_var_count = STA_FORMAT_INST_VARS(fmt);
    intptr_t idx = STA_SMALLINT_VAL(args[1]);
    if (idx < 1 || (uint32_t)idx > inst_var_count) return 2;

    sta_payload(h)[idx - 1] = args[2];
    *result = args[2];
    return STA_PRIM_SUCCESS;
}

static int prim_identity_hash(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (STA_IS_SMALLINT(args[0])) {
        *result = STA_SMALLINT_OOP((intptr_t)(STA_SMALLINT_VAL(args[0]) & 0x3FFFFFFFu));
        return STA_PRIM_SUCCESS;
    }
    if (STA_IS_CHAR(args[0])) {
        *result = STA_SMALLINT_OOP((intptr_t)(STA_CHAR_VAL(args[0]) & 0x3FFFFFFFu));
        return STA_PRIM_SUCCESS;
    }
    uintptr_t addr = (uintptr_t)args[0];
    *result = STA_SMALLINT_OOP((intptr_t)((addr >> 4) & 0x3FFFFFFFu));
    return STA_PRIM_SUCCESS;
}

static int prim_shallow_copy(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) {
        *result = args[0];
        return STA_PRIM_SUCCESS;
    }

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    STA_ObjHeader *copy = sta_heap_alloc(&ctx->vm->heap, h->class_index, h->size);
    if (!copy) return 7;

    memcpy(sta_payload(copy), sta_payload(h), (size_t)h->size * sizeof(STA_OOP));
    copy->reserved = h->reserved;
    copy->obj_flags &= (uint8_t)~STA_OBJ_IMMUTABLE;

    *result = (STA_OOP)(uintptr_t)copy;
    return STA_PRIM_SUCCESS;
}

/* ── Array primitives ──────────────────────────────────────────────────── */

static int prim_array_at(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    intptr_t idx = STA_SMALLINT_VAL(args[1]);
    if (idx < 1 || (uint32_t)idx > h->size) return STA_PRIM_OUT_OF_RANGE;
    *result = sta_payload(h)[idx - 1];
    return STA_PRIM_SUCCESS;
}

static int prim_array_at_put(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)ctx; (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    if (!STA_IS_SMALLINT(args[1])) return STA_PRIM_BAD_ARGUMENT;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    intptr_t idx = STA_SMALLINT_VAL(args[1]);
    if (idx < 1 || (uint32_t)idx > h->size) return STA_PRIM_OUT_OF_RANGE;
    sta_payload(h)[idx - 1] = args[2];
    *result = args[2];
    return STA_PRIM_SUCCESS;
}

static int prim_array_size(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];

    STA_OOP fmt = get_receiver_format(&ctx->vm->class_table, h);
    if (fmt != 0 && sta_format_is_bytes(fmt)) {
        uint8_t inst_vars = STA_FORMAT_INST_VARS(fmt);
        uint32_t byte_slots = h->size - inst_vars;
        uint32_t padding = STA_BYTE_PADDING(h);
        uint32_t byte_count = byte_slots * 8 - padding;
        *result = STA_SMALLINT_OOP((intptr_t)byte_count);
    } else {
        *result = STA_SMALLINT_OOP((intptr_t)h->size);
    }
    return STA_PRIM_SUCCESS;
}

static int prim_replace(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    STA_ObjHeader *dst_h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (dst_h->obj_flags & STA_OBJ_IMMUTABLE) return STA_PRIM_BAD_RECEIVER;

    if (!STA_IS_SMALLINT(args[1]) || !STA_IS_SMALLINT(args[2]) ||
        !STA_IS_SMALLINT(args[4]))
        return STA_PRIM_BAD_ARGUMENT;
    if (STA_IS_IMMEDIATE(args[3])) return STA_PRIM_BAD_ARGUMENT;

    intptr_t start    = STA_SMALLINT_VAL(args[1]);
    intptr_t stop     = STA_SMALLINT_VAL(args[2]);
    STA_ObjHeader *src_h = (STA_ObjHeader *)(uintptr_t)args[3];
    intptr_t repStart = STA_SMALLINT_VAL(args[4]);

    if (start < 1 || stop < start - 1 || repStart < 1)
        return STA_PRIM_OUT_OF_RANGE;

    intptr_t count = stop - start + 1;
    if (count == 0) { *result = args[0]; return STA_PRIM_SUCCESS; }

    STA_OOP dst_fmt = get_receiver_format(&ctx->vm->class_table, dst_h);
    STA_OOP src_fmt = get_receiver_format(&ctx->vm->class_table, src_h);
    if (dst_fmt == 0 || src_fmt == 0) return STA_PRIM_BAD_RECEIVER;

    bool dst_bytes = sta_format_is_bytes(dst_fmt);
    bool src_bytes = sta_format_is_bytes(src_fmt);
    if (dst_bytes != src_bytes) return STA_PRIM_BAD_ARGUMENT;

    if (dst_bytes) {
        uint8_t dst_iv = STA_FORMAT_INST_VARS(dst_fmt);
        uint8_t src_iv = STA_FORMAT_INST_VARS(src_fmt);
        uint32_t dst_var = dst_h->size - dst_iv;
        uint32_t src_var = src_h->size - src_iv;
        uint32_t dst_bc = dst_var * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(dst_h);
        uint32_t src_bc = src_var * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(src_h);

        if ((uint32_t)stop > dst_bc) return STA_PRIM_OUT_OF_RANGE;
        if ((uint32_t)(repStart + count - 1) > src_bc) return STA_PRIM_OUT_OF_RANGE;

        uint8_t *dst_p = (uint8_t *)&sta_payload(dst_h)[dst_iv];
        uint8_t *src_p = (uint8_t *)&sta_payload(src_h)[src_iv];
        memmove(&dst_p[start - 1], &src_p[repStart - 1], (size_t)count);
    } else {
        if ((uint32_t)stop > dst_h->size) return STA_PRIM_OUT_OF_RANGE;
        if ((uint32_t)(repStart + count - 1) > src_h->size) return STA_PRIM_OUT_OF_RANGE;

        STA_OOP *dst_p = sta_payload(dst_h);
        STA_OOP *src_p = sta_payload(src_h);
        memmove(&dst_p[start - 1], &src_p[repStart - 1],
                (size_t)count * sizeof(STA_OOP));
    }

    *result = args[0];
    return STA_PRIM_SUCCESS;
}

/* ── Exception primitives ──────────────────────────────────────────────── */

static int prim_on_do(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    STA_OOP body_block    = args[0];
    STA_OOP exc_class     = args[1];
    STA_OOP handler_block = args[2];

    STA_HandlerEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.exception_class = exc_class;
    entry.handler_block   = handler_block;
    entry.saved_slab_top  = ctx->vm->slab.top;
    entry.saved_slab_sp   = ctx->vm->slab.sp;

    if (setjmp(entry.jmp) == 0) {
        sta_handler_push_ctx(ctx, &entry);
        STA_OOP body_result = sta_eval_block(ctx->vm, body_block, NULL, 0);
        sta_handler_pop_ctx(ctx);
        *result = body_result;
        return STA_PRIM_SUCCESS;
    } else {
        ctx->vm->slab.top = entry.saved_slab_top;
        ctx->vm->slab.sp  = entry.saved_slab_sp;

        STA_OOP exc = sta_handler_get_signaled_ctx(ctx);
        STA_OOP handler_args[1] = { exc };
        STA_OOP handler_result = sta_eval_block(ctx->vm, handler_block, handler_args, 1);
        *result = handler_result;
        return STA_PRIM_SUCCESS;
    }
}

static int prim_signal(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs; (void)result;

    STA_OOP exception = args[0];
    STA_HandlerEntry **top_ptr = sta_handler_top_ptr(ctx);

    /* First, find the target on:do: handler (skip ensure: entries). */
    STA_HandlerEntry *target = NULL;
    {
        STA_HandlerEntry *e = *top_ptr;
        while (e) {
            if (!e->is_ensure) {
                /* Check if this on:do: handler matches the exception. */
                STA_HandlerEntry *saved_top = *top_ptr;
                *top_ptr = e;
                STA_HandlerEntry *match = sta_handler_find_ctx(ctx, exception);
                *top_ptr = saved_top;
                if (match == e) {
                    target = e;
                    break;
                }
            }
            e = e->prev;
        }
    }

    if (target) {
        /* Fire all ensure: blocks between handler_top and target. */
        while (*top_ptr != target) {
            STA_HandlerEntry *top = *top_ptr;
            *top_ptr = top->prev;
            if (top->is_ensure) {
                (void)sta_eval_block(ctx->vm, top->ensure_block, NULL, 0);
            }
        }
        /* Now handler_top == target. Pop it and longjmp. */
        *top_ptr = target->prev;
        sta_handler_set_signaled_ctx(ctx, exception);
        longjmp(target->jmp, 1);
    }

    fprintf(stderr, "Unhandled exception (class index %u)\n",
            STA_IS_HEAP(exception)
                ? ((STA_ObjHeader *)(uintptr_t)exception)->class_index
                : 0u);
    abort();
    return STA_PRIM_NOT_AVAILABLE;
}

static int prim_ensure(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    STA_OOP body_block   = args[0];
    STA_OOP ensure_block = args[1];

    /* Register on the handler chain so exception unwinding fires this
     * ensure: block. prim_signal walks the chain and fires ensure: entries
     * it encounters before the target on:do: handler. */
    STA_HandlerEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.is_ensure     = true;
    entry.ensure_block  = ensure_block;

    sta_handler_push_ctx(ctx, &entry);
    STA_OOP body_result = sta_eval_block(ctx->vm, body_block, NULL, 0);
    sta_handler_pop_ctx(ctx);

    /* Normal completion: fire ensure block. */
    (void)sta_eval_block(ctx->vm, ensure_block, NULL, 0);

    *result = body_result;
    return STA_PRIM_SUCCESS;
}

/* ── Symbol/String interning primitives ──────────────────────────────── */

static int prim_as_symbol(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (h->class_index != STA_CLS_STRING) return STA_PRIM_BAD_RECEIVER;

    uint32_t var_words = h->size;
    uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
    const char *bytes = (const char *)sta_payload(h);

    STA_OOP sym = sta_symbol_intern(&ctx->vm->immutable_space,
                                     &ctx->vm->symbol_table,
                                     bytes, byte_count);
    if (sym == 0) return STA_PRIM_NO_MEMORY;
    *result = sym;
    return STA_PRIM_SUCCESS;
}

static int prim_sym_as_string(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[0])) return STA_PRIM_BAD_RECEIVER;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[0];
    if (h->class_index != STA_CLS_SYMBOL) return STA_PRIM_BAD_RECEIVER;

    size_t len = 0;
    const char *bytes = sta_symbol_get_bytes(args[0], &len);
    if (!bytes) return STA_PRIM_BAD_RECEIVER;

    uint32_t var_words = ((uint32_t)len + (uint32_t)(sizeof(STA_OOP) - 1))
                         / (uint32_t)sizeof(STA_OOP);
    STA_ObjHeader *str_h = sta_heap_alloc(&ctx->vm->heap, STA_CLS_STRING, var_words);
    if (!str_h) return STA_PRIM_NO_MEMORY;

    str_h->reserved = (uint8_t)(var_words * sizeof(STA_OOP) - (uint32_t)len);
    memset(sta_payload(str_h), 0, var_words * sizeof(STA_OOP));
    memcpy(sta_payload(str_h), bytes, len);

    *result = (STA_OOP)(uintptr_t)str_h;
    return STA_PRIM_SUCCESS;
}

static int prim_sym_intern(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    if (STA_IS_IMMEDIATE(args[1])) return STA_PRIM_BAD_ARGUMENT;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)args[1];
    if (h->class_index != STA_CLS_STRING && h->class_index != STA_CLS_SYMBOL)
        return STA_PRIM_BAD_ARGUMENT;

    if (h->class_index == STA_CLS_SYMBOL) {
        *result = args[1];
        return STA_PRIM_SUCCESS;
    }

    uint32_t var_words = h->size;
    uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
    const char *bytes = (const char *)sta_payload(h);

    STA_OOP sym = sta_symbol_intern(&ctx->vm->immutable_space,
                                     &ctx->vm->symbol_table,
                                     bytes, byte_count);
    if (sym == 0) return STA_PRIM_NO_MEMORY;
    *result = sym;
    return STA_PRIM_SUCCESS;
}

/* ── Class creation primitive ──────────────────────────────────────────── */

static const char *string_bytes(STA_OOP str, size_t *out_len) {
    if (STA_IS_IMMEDIATE(str)) return NULL;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)str;
    if (h->class_index != STA_CLS_STRING && h->class_index != STA_CLS_SYMBOL)
        return NULL;
    uint32_t var_words = h->size;
    uint32_t byte_count = var_words * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(h);
    *out_len = byte_count;
    return (const char *)sta_payload(h);
}

static int prim_sysdict_put(STA_Heap *heap, STA_OOP dict, STA_OOP key_sym, STA_OOP value) {
    STA_ObjHeader *ah = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    if (!ah) return -1;
    sta_payload(ah)[0] = key_sym;
    sta_payload(ah)[1] = value;
    STA_OOP assoc = (STA_OOP)(uintptr_t)ah;

    STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)dict;
    STA_OOP *dp = sta_payload(dh);
    STA_OOP arr = dp[1];
    uint32_t tally = (uint32_t)STA_SMALLINT_VAL(dp[0]);

    STA_ObjHeader *arr_h = (STA_ObjHeader *)(uintptr_t)arr;
    uint32_t cap = arr_h->size / 2;
    STA_OOP *slots = sta_payload(arr_h);

    uint32_t hash = sta_symbol_get_hash(key_sym);
    uint32_t idx = hash % cap;

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t pos = ((idx + i) % cap) * 2;
        if (slots[pos] == 0) {
            slots[pos]     = key_sym;
            slots[pos + 1] = assoc;
            dp[0] = STA_SMALLINT_OOP((intptr_t)(tally + 1));
            return 0;
        }
        if (slots[pos] == key_sym) {
            slots[pos + 1] = assoc;
            return 0;
        }
    }
    return -1;
}

static int prim_subclass(STA_ExecContext *ctx, STA_OOP *args, uint8_t nargs, STA_OOP *result) {
    (void)nargs;
    STA_VM *vm = ctx->vm;

    STA_OOP superclass = args[0];
    STA_OOP name_sym   = args[1];
    STA_OOP ivars_str  = args[2];

    if (STA_IS_IMMEDIATE(superclass)) return STA_PRIM_BAD_RECEIVER;
    if (STA_IS_IMMEDIATE(name_sym)) return STA_PRIM_BAD_ARGUMENT;
    STA_ObjHeader *name_h = (STA_ObjHeader *)(uintptr_t)name_sym;
    if (name_h->class_index != STA_CLS_SYMBOL) return STA_PRIM_BAD_ARGUMENT;

    size_t ivars_len = 0;
    const char *ivars_bytes = NULL;
    uint16_t inst_var_count = 0;

    if (!STA_IS_IMMEDIATE(ivars_str))
        ivars_bytes = string_bytes(ivars_str, &ivars_len);

    if (ivars_bytes && ivars_len > 0) {
        bool in_word = false;
        for (size_t i = 0; i < ivars_len; i++) {
            if (ivars_bytes[i] == ' ' || ivars_bytes[i] == '\t' ||
                ivars_bytes[i] == '\n' || ivars_bytes[i] == '\r') {
                in_word = false;
            } else {
                if (!in_word) inst_var_count++;
                in_word = true;
            }
        }
    }

    STA_ObjHeader *super_h = (STA_ObjHeader *)(uintptr_t)superclass;
    STA_OOP super_fmt = sta_payload(super_h)[STA_CLASS_SLOT_FORMAT];
    uint8_t super_inst_vars = STA_FORMAT_INST_VARS(super_fmt);
    uint16_t total_inst_vars = (uint16_t)super_inst_vars + inst_var_count;
    if (total_inst_vars > 255) return STA_PRIM_OUT_OF_RANGE;

    uint32_t cls_index = sta_class_table_alloc_index(&vm->class_table);
    if (cls_index == 0) return STA_PRIM_NO_MEMORY;
    sta_class_table_set(&vm->class_table, cls_index, STA_SMALLINT_OOP(1));

    uint32_t meta_index = sta_class_table_alloc_index(&vm->class_table);
    if (meta_index == 0) {
        sta_class_table_set(&vm->class_table, cls_index, 0);
        return STA_PRIM_NO_MEMORY;
    }

    STA_ObjHeader *cls_h = sta_heap_alloc(&vm->heap, meta_index, 4);
    if (!cls_h) {
        sta_class_table_set(&vm->class_table, cls_index, 0);
        return STA_PRIM_NO_MEMORY;
    }
    STA_OOP cls = (STA_OOP)(uintptr_t)cls_h;

    STA_ObjHeader *meta_h = sta_heap_alloc(&vm->heap, STA_CLS_METACLASS, 4);
    if (!meta_h) {
        sta_class_table_set(&vm->class_table, cls_index, 0);
        return STA_PRIM_NO_MEMORY;
    }
    STA_OOP meta = (STA_OOP)(uintptr_t)meta_h;

    STA_OOP cls_md  = sta_method_dict_create(&vm->heap, 8);
    STA_OOP meta_md = sta_method_dict_create(&vm->heap, 4);
    if (!cls_md || !meta_md) {
        sta_class_table_set(&vm->class_table, cls_index, 0);
        return STA_PRIM_NO_MEMORY;
    }

    STA_OOP nil_oop = vm->specials[SPC_NIL];
    STA_OOP meta_super = nil_oop;
    if (superclass != 0 && superclass != nil_oop) {
        uint32_t super_meta_idx = super_h->class_index;
        meta_super = sta_class_table_get(&vm->class_table, super_meta_idx);
    }

    STA_OOP *cls_slots = sta_payload(cls_h);
    cls_slots[STA_CLASS_SLOT_SUPERCLASS] = superclass;
    cls_slots[STA_CLASS_SLOT_METHODDICT] = cls_md;
    cls_slots[STA_CLASS_SLOT_FORMAT]     = STA_FORMAT_ENCODE(total_inst_vars, STA_FMT_NORMAL);
    cls_slots[STA_CLASS_SLOT_NAME]       = name_sym;

    STA_OOP *meta_slots = sta_payload(meta_h);
    meta_slots[STA_CLASS_SLOT_SUPERCLASS] = meta_super;
    meta_slots[STA_CLASS_SLOT_METHODDICT] = meta_md;
    meta_slots[STA_CLASS_SLOT_FORMAT]     = STA_FORMAT_ENCODE(4, STA_FMT_NORMAL);
    meta_slots[STA_CLASS_SLOT_NAME]       = nil_oop;

    sta_class_table_set(&vm->class_table, cls_index, cls);
    sta_class_table_set(&vm->class_table, meta_index, meta);

    STA_OOP sysdict = vm->specials[SPC_SMALLTALK];
    if (sysdict != 0 && sysdict != nil_oop)
        prim_sysdict_put(&vm->heap, sysdict, name_sym, cls);

    *result = cls;
    return STA_PRIM_SUCCESS;
}

/* ── Table initialization ──────────────────────────────────────────────── */

void sta_primitive_table_init(void) {
    memset(sta_primitives, 0, sizeof(sta_primitives));

    sta_primitives[1]  = prim_smallint_add;
    sta_primitives[2]  = prim_smallint_sub;
    sta_primitives[3]  = prim_smallint_lt;
    sta_primitives[4]  = prim_smallint_gt;
    sta_primitives[5]  = prim_smallint_le;
    sta_primitives[6]  = prim_smallint_ge;
    sta_primitives[7]  = prim_smallint_eq;
    sta_primitives[8]  = prim_smallint_ne;
    sta_primitives[9]  = prim_smallint_mul;
    sta_primitives[10] = prim_smallint_div_exact;
    sta_primitives[11] = prim_smallint_mod;
    sta_primitives[12] = prim_smallint_div_floor;
    sta_primitives[13] = prim_smallint_quo;
    sta_primitives[14] = prim_smallint_bitand;
    sta_primitives[15] = prim_smallint_bitor;
    sta_primitives[16] = prim_smallint_bitxor;
    sta_primitives[17] = prim_smallint_bitshift;

    sta_primitives[200] = prim_smallint_printstring;

    sta_primitives[29] = prim_identity;
    sta_primitives[30] = prim_class;
    sta_primitives[31] = prim_basic_new;
    sta_primitives[32] = prim_basic_new_size;
    sta_primitives[33] = prim_basic_at;
    sta_primitives[34] = prim_basic_at_put;
    sta_primitives[35] = prim_basic_size;
    sta_primitives[36] = prim_hash;
    sta_primitives[37] = prim_become;
    sta_primitives[38] = prim_inst_var_at;
    sta_primitives[39] = prim_inst_var_at_put;
    sta_primitives[40] = prim_identity_hash;
    sta_primitives[41] = prim_shallow_copy;
    sta_primitives[42] = prim_yourself;

    sta_primitives[51] = prim_array_at;
    sta_primitives[52] = prim_array_at_put;
    sta_primitives[53] = prim_array_size;
    sta_primitives[54] = prim_replace;

    sta_primitives[60] = prim_byte_at;
    sta_primitives[61] = prim_byte_at_put;
    sta_primitives[62] = prim_byte_size;
    sta_primitives[63] = prim_string_at;
    sta_primitives[64] = prim_string_at_put;

    sta_primitives[88]  = prim_on_do;
    sta_primitives[89]  = prim_signal;
    sta_primitives[90]  = prim_ensure;

    sta_primitives[91] = prim_as_symbol;
    sta_primitives[92] = prim_sym_as_string;
    sta_primitives[93] = prim_sym_intern;

    sta_primitives[94] = prim_char_value;
    sta_primitives[95] = prim_char_for;

    sta_primitives[120] = prim_responds_to;
    sta_primitives[121] = prim_dnu;
    sta_primitives[122] = prim_subclass;
}
