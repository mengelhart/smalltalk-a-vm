/* src/vm/handler.c
 * Exception handler stack — see handler.h for documentation.
 */
#include "handler.h"
#include "interpreter.h"
#include "special_objects.h"

/* ── Global state (Phase 1: single-threaded) ──────────────────────────── */

static STA_HandlerEntry *g_handler_top = NULL;
static STA_OOP g_signaled_exception = 0;

/* ── Chain operations ─────────────────────────────────────────────────── */

STA_HandlerEntry *sta_handler_top(void) {
    return g_handler_top;
}

void sta_handler_set_top(STA_HandlerEntry *entry) {
    g_handler_top = entry;
}

void sta_handler_push(STA_HandlerEntry *entry) {
    entry->prev = g_handler_top;
    g_handler_top = entry;
}

void sta_handler_pop(void) {
    if (g_handler_top) {
        g_handler_top = g_handler_top->prev;
    }
}

/* ── Signal exception storage ─────────────────────────────────────────── */

void sta_handler_set_signaled_exception(STA_OOP exc) {
    g_signaled_exception = exc;
}

STA_OOP sta_handler_get_signaled_exception(void) {
    return g_signaled_exception;
}

/* ── isKindOf: check ──────────────────────────────────────────────────── */

/* Check if obj's class is a subclass of (or equal to) target_class.
 * Walks the superclass chain from obj's class. */
static bool is_kind_of(STA_OOP obj, STA_OOP target_class, STA_ClassTable *ct) {
    uint32_t cls_idx;
    if (STA_IS_SMALLINT(obj))       cls_idx = STA_CLS_SMALLINTEGER;
    else if (STA_IS_CHAR(obj))      cls_idx = STA_CLS_CHARACTER;
    else                            cls_idx = ((STA_ObjHeader *)(uintptr_t)obj)->class_index;

    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP cls = sta_class_table_get(ct, cls_idx);

    while (cls != 0 && cls != nil_oop) {
        if (cls == target_class) return true;
        cls = sta_class_superclass(cls);
    }
    return false;
}

/* ── Handler find ─────────────────────────────────────────────────────── */

STA_HandlerEntry *sta_handler_find(STA_OOP exception, STA_ClassTable *ct) {
    STA_HandlerEntry *entry = g_handler_top;
    while (entry) {
        if (is_kind_of(exception, entry->exception_class, ct)) {
            return entry;
        }
        entry = entry->prev;
    }
    return NULL;
}
