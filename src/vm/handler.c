/* src/vm/handler.c
 * Exception handler stack — see handler.h for documentation.
 * All state lives in STA_VM (handler_top, signaled_exception).
 */
#include "handler.h"
#include "vm_state.h"
#include "interpreter.h"
#include "special_objects.h"

/* ── Chain operations ─────────────────────────────────────────────────── */

void sta_handler_push(STA_VM *vm, STA_HandlerEntry *entry) {
    entry->prev = vm->handler_top;
    vm->handler_top = entry;
}

void sta_handler_pop(STA_VM *vm) {
    if (vm->handler_top) {
        vm->handler_top = vm->handler_top->prev;
    }
}

/* ── Signal exception storage ─────────────────────────────────────────── */

void sta_handler_set_signaled_exception(STA_VM *vm, STA_OOP exc) {
    vm->signaled_exception = exc;
}

STA_OOP sta_handler_get_signaled_exception(STA_VM *vm) {
    return vm->signaled_exception;
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

STA_HandlerEntry *sta_handler_find(STA_VM *vm, STA_OOP exception) {
    STA_HandlerEntry *entry = vm->handler_top;
    while (entry) {
        if (is_kind_of(exception, entry->exception_class, &vm->class_table)) {
            return entry;
        }
        entry = entry->prev;
    }
    return NULL;
}
