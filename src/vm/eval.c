/* src/vm/eval.c
 * Public API: sta_eval, sta_inspect, sta_inspect_cstring.
 * Phase 1, Epic 11.
 *
 * Handle model (Phase 1): STA_Handle* is the raw STA_OOP cast to a pointer.
 * sta_handle_retain/release are no-ops. Phase 2 will introduce a real handle
 * table with reference counting per ADR 013.
 */
#include "vm/vm_state.h"
#include "vm/oop.h"
#include "vm/special_objects.h"
#include "vm/class_table.h"
#include "vm/symbol_table.h"
#include "vm/interpreter.h"
#include "compiler/compiler.h"
#include <sta/vm.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* ── OOP ↔ Handle conversion (Phase 1: raw cast) ──────────────────── */

static STA_Handle *oop_to_handle(STA_OOP oop) {
    return (STA_Handle *)(uintptr_t)oop;
}

static STA_OOP handle_to_oop(STA_Handle *h) {
    return (STA_OOP)(uintptr_t)h;
}

/* ── sta_eval ──────────────────────────────────────────────────────── */

STA_Handle* sta_eval(STA_VM* vm, const char* expression) {
    if (!vm || !expression) return NULL;

    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);

    STA_CompileResult cr = sta_compile_expression(
        expression, &vm->symbol_table, &vm->immutable_space,
        &vm->heap, sysdict);

    if (cr.had_error) {
        snprintf(vm->last_error, sizeof(vm->last_error),
                 "compile error: %s", cr.error_msg);
        return NULL;
    }

    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP result = sta_interpret(vm, cr.method, nil_oop, NULL, 0);

    return oop_to_handle(result);
}

/* ── sta_inspect (returns handle — stub for Phase 1) ───────────────── */

STA_Handle* sta_inspect(STA_VM* vm, STA_Handle* object) {
    (void)vm; (void)object;
    return NULL;
}

/* ── sta_inspect_cstring ───────────────────────────────────────────── */

/* Class name lookup: class_index → class OOP → name symbol → C string. */
static const char *class_name_for_index(STA_ClassTable *ct, uint32_t idx) {
    STA_OOP cls = sta_class_table_get(ct, idx);
    if (!cls || !STA_IS_HEAP(cls)) return "???";
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)cls;
    if (h->size < 4) return "???";
    STA_OOP name_sym = sta_payload(h)[STA_CLASS_SLOT_NAME];
    if (!name_sym || !STA_IS_HEAP(name_sym)) return "???";
    STA_ObjHeader *nh = (STA_ObjHeader *)(uintptr_t)name_sym;
    if (nh->class_index != STA_CLS_SYMBOL) return "???";
    return sta_symbol_get_bytes(name_sym, NULL);
}

/* Extract raw bytes from a String heap object (byte-indexable). */
static const char *string_raw_bytes(STA_OOP str, size_t *out_len) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)str;
    uint32_t inst_vars = 0; /* String has 0 named instance variables */
    uint32_t data_words = h->size - inst_vars;
    size_t total_bytes = (size_t)data_words * sizeof(STA_OOP);
    size_t padding = STA_BYTE_PADDING(h);
    size_t exact = total_bytes - padding;
    const char *bytes = (const char *)&sta_payload(h)[inst_vars];
    if (out_len) *out_len = exact;
    return bytes;
}

const char* sta_inspect_cstring(STA_VM* vm, STA_Handle* handle) {
    if (!vm || !handle) return "";

    STA_OOP oop = handle_to_oop(handle);
    char *buf = vm->inspect_buffer;
    size_t cap = sizeof(vm->inspect_buffer);

    /* SmallInteger */
    if (STA_IS_SMALLINT(oop)) {
        snprintf(buf, cap, "%" PRIdPTR, STA_SMALLINT_VAL(oop));
        return buf;
    }

    /* Character immediate */
    if (STA_IS_CHAR(oop)) {
        uint32_t cp = STA_CHAR_VAL(oop);
        if (cp >= 32 && cp < 127) {
            snprintf(buf, cap, "$%c", (char)cp);
        } else {
            snprintf(buf, cap, "$<U+%04X>", cp);
        }
        return buf;
    }

    /* Heap object — check for well-known singletons first. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP true_oop = sta_spc_get(SPC_TRUE);
    STA_OOP false_oop = sta_spc_get(SPC_FALSE);

    if (oop == nil_oop) {
        snprintf(buf, cap, "nil");
        return buf;
    }
    if (oop == true_oop) {
        snprintf(buf, cap, "true");
        return buf;
    }
    if (oop == false_oop) {
        snprintf(buf, cap, "false");
        return buf;
    }

    /* Heap object — dispatch on class_index. */
    if (!STA_IS_HEAP(oop)) {
        snprintf(buf, cap, "<unknown immediate>");
        return buf;
    }

    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)oop;

    switch (h->class_index) {
    case STA_CLS_SYMBOL: {
        size_t len;
        const char *bytes = sta_symbol_get_bytes(oop, &len);
        snprintf(buf, cap, "#%.*s", (int)len, bytes);
        return buf;
    }
    case STA_CLS_STRING: {
        size_t len;
        const char *bytes = string_raw_bytes(oop, &len);
        snprintf(buf, cap, "'%.*s'", (int)len, bytes);
        return buf;
    }
    case STA_CLS_ARRAY: {
        uint32_t sz = h->size;
        STA_OOP *slots = sta_payload(h);
        size_t pos = 0;
        pos += (size_t)snprintf(buf + pos, cap - pos, "#(");
        for (uint32_t i = 0; i < sz && pos < cap - 2; i++) {
            if (i > 0) pos += (size_t)snprintf(buf + pos, cap - pos, " ");
            /* Recursion-safe: only inline SmallInts in arrays. */
            if (STA_IS_SMALLINT(slots[i])) {
                pos += (size_t)snprintf(buf + pos, cap - pos,
                    "%" PRIdPTR, STA_SMALLINT_VAL(slots[i]));
            } else {
                pos += (size_t)snprintf(buf + pos, cap - pos, "...");
                break;
            }
        }
        snprintf(buf + pos, cap - pos, ")");
        return buf;
    }
    default: {
        const char *name = class_name_for_index(&vm->class_table, h->class_index);
        /* "a Foo" or "an Array" — use "an" for vowel start. */
        char article = 'a';
        if (name[0] == 'A' || name[0] == 'E' || name[0] == 'I' ||
            name[0] == 'O' || name[0] == 'U')
            article = 'n';
        if (article == 'n')
            snprintf(buf, cap, "an %s", name);
        else
            snprintf(buf, cap, "a %s", name);
        return buf;
    }
    }
}
