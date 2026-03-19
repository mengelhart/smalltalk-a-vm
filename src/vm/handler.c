/* src/vm/handler.c
 * Exception handler stack — see handler.h for documentation.
 * Phase 2 Epic 2: handler chain on STA_Actor, with VM fallback for bootstrap.
 */
#include "handler.h"
#include "vm_state.h"
#include "actor/actor.h"
#include "interpreter.h"
#include "special_objects.h"

/* ── Per-actor handler resolution ────────────────────────────────────── */

STA_HandlerEntry **sta_handler_top_ptr(STA_ExecContext *ctx) {
    if (ctx->actor)
        return &ctx->actor->handler_top;
    return &ctx->vm->handler_top;
}

STA_OOP *sta_handler_signaled_ptr(STA_ExecContext *ctx) {
    if (ctx->actor)
        return &ctx->actor->signaled_exception;
    return &ctx->vm->signaled_exception;
}

/* ── Chain operations (VM — legacy/bootstrap/test) ───────────────────── */

void sta_handler_push(STA_VM *vm, STA_HandlerEntry *entry) {
    entry->prev = vm->handler_top;
    vm->handler_top = entry;
}

void sta_handler_pop(STA_VM *vm) {
    if (vm->handler_top) {
        vm->handler_top = vm->handler_top->prev;
    }
}

/* ── Chain operations (exec context — prefers actor) ─────────────────── */

void sta_handler_push_ctx(STA_ExecContext *ctx, STA_HandlerEntry *entry) {
    STA_HandlerEntry **top = sta_handler_top_ptr(ctx);
    entry->prev = *top;
    *top = entry;
}

void sta_handler_pop_ctx(STA_ExecContext *ctx) {
    STA_HandlerEntry **top = sta_handler_top_ptr(ctx);
    if (*top) {
        *top = (*top)->prev;
    }
}

/* ── Signal exception storage ─────────────────────────────────────────── */

void sta_handler_set_signaled_exception(STA_VM *vm, STA_OOP exc) {
    vm->signaled_exception = exc;
}

STA_OOP sta_handler_get_signaled_exception(STA_VM *vm) {
    return vm->signaled_exception;
}

void sta_handler_set_signaled_ctx(STA_ExecContext *ctx, STA_OOP exc) {
    *sta_handler_signaled_ptr(ctx) = exc;
}

STA_OOP sta_handler_get_signaled_ctx(STA_ExecContext *ctx) {
    return *sta_handler_signaled_ptr(ctx);
}

/* ── isKindOf: check ──────────────────────────────────────────────────── */

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

/* ── Handler find (VM — legacy/bootstrap/test) ───────────────────────── */

STA_HandlerEntry *sta_handler_find(STA_VM *vm, STA_OOP exception) {
    STA_HandlerEntry *entry = vm->handler_top;
    while (entry) {
        if (!entry->is_ensure &&
            is_kind_of(exception, entry->exception_class, &vm->class_table)) {
            return entry;
        }
        entry = entry->prev;
    }
    return NULL;
}

/* ── Handler find (exec context — prefers actor) ─────────────────────── */

STA_HandlerEntry *sta_handler_find_ctx(STA_ExecContext *ctx, STA_OOP exception) {
    STA_HandlerEntry **top = sta_handler_top_ptr(ctx);
    STA_HandlerEntry *entry = *top;
    while (entry) {
        if (!entry->is_ensure &&
            is_kind_of(exception, entry->exception_class, &ctx->vm->class_table)) {
            return entry;
        }
        entry = entry->prev;
    }
    return NULL;
}
