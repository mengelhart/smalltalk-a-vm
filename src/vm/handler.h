/* src/vm/handler.h
 * Exception handler stack — Phase 1 handler-stack approach per §7.8.
 * Phase 2 Epic 2: handler chain lives on STA_Actor (per-actor isolation).
 * During bootstrap (ctx->actor == NULL), falls back to STA_VM fields.
 *
 * Actor-local linked list of handler entries. on:do: pushes an entry,
 * signal walks the chain to find a match, longjmp transfers control.
 *
 * Phase 3 may replace this with thisContext chain walking.
 */
#pragma once
#include "oop.h"
#include "class_table.h"
#include <setjmp.h>
#include <stdbool.h>

/* Forward-declare to avoid circular includes. */
struct STA_VM;
struct STA_ExecContext;
struct STA_Actor;

/* ── Handler entry ─────────────────────────────────────────────────────── */

typedef struct STA_HandlerEntry {
    STA_OOP  exception_class;       /* class to catch (isKindOf: match)   */
    STA_OOP  handler_block;         /* handler block closure              */
    jmp_buf  jmp;                   /* setjmp/longjmp buffer              */
    uint8_t *saved_slab_top;        /* slab->top before body evaluation   */
    uint8_t *saved_slab_sp;         /* slab->sp before body evaluation    */
    bool     is_ensure;             /* true = ensure: entry               */
    STA_OOP  ensure_block;          /* ensure: block to evaluate on unwind*/
    struct STA_HandlerEntry *prev;  /* previous handler in chain          */
} STA_HandlerEntry;

/* ── Handler chain API (STA_VM — bootstrap / test compatibility) ────── */

/* Push an entry onto the handler chain. Sets entry->prev and updates top. */
void sta_handler_push(struct STA_VM *vm, STA_HandlerEntry *entry);

/* Pop the top entry. Sets top = top->prev. */
void sta_handler_pop(struct STA_VM *vm);

/* Walk the chain from top, looking for an entry whose exception_class
 * matches the given exception's class (isKindOf: check — walks superclass
 * chain). Returns the matching entry, or NULL if none found. */
STA_HandlerEntry *sta_handler_find(struct STA_VM *vm, STA_OOP exception);

/* ── Signal exception storage ──────────────────────────────────────────── */

void    sta_handler_set_signaled_exception(struct STA_VM *vm, STA_OOP exc);
STA_OOP sta_handler_get_signaled_exception(struct STA_VM *vm);

/* ── Per-actor handler resolution (Phase 2 Epic 2) ────────────────────── */
/* These resolve to actor fields when ctx->actor is set, or fall back to
 * VM fields during bootstrap (ctx->actor == NULL). */

/* Get pointer to the handler_top field (actor or VM). */
STA_HandlerEntry **sta_handler_top_ptr(struct STA_ExecContext *ctx);

/* Get pointer to the signaled_exception field (actor or VM). */
STA_OOP *sta_handler_signaled_ptr(struct STA_ExecContext *ctx);

/* Push/pop/find using execution context (prefers actor, falls back to VM). */
void sta_handler_push_ctx(struct STA_ExecContext *ctx, STA_HandlerEntry *entry);
void sta_handler_pop_ctx(struct STA_ExecContext *ctx);
STA_HandlerEntry *sta_handler_find_ctx(struct STA_ExecContext *ctx, STA_OOP exception);
void    sta_handler_set_signaled_ctx(struct STA_ExecContext *ctx, STA_OOP exc);
STA_OOP sta_handler_get_signaled_ctx(struct STA_ExecContext *ctx);
