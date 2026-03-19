/* src/vm/handler.h
 * Exception handler stack — Phase 1 handler-stack approach per §7.8.
 * Phase 1 — permanent.
 *
 * Actor-local linked list of handler entries. on:do: pushes an entry,
 * signal walks the chain to find a match, longjmp transfers control.
 *
 * All state lives in STA_VM (handler_top, signaled_exception).
 * Moves to STA_Actor in Epic 3.
 *
 * Phase 3 may replace this with thisContext chain walking.
 */
#pragma once
#include "oop.h"
#include "class_table.h"
#include <setjmp.h>
#include <stdbool.h>

/* Forward-declare STA_VM to avoid circular include. */
struct STA_VM;

/* ── Handler entry ─────────────────────────────────────────────────────── */

typedef struct STA_HandlerEntry {
    STA_OOP  exception_class;       /* class to catch (isKindOf: match)   */
    STA_OOP  handler_block;         /* handler block closure              */
    jmp_buf  jmp;                   /* setjmp/longjmp buffer              */
    uint8_t *saved_slab_top;        /* slab->top before body evaluation   */
    uint8_t *saved_slab_sp;         /* slab->sp before body evaluation    */
    bool     is_ensure;             /* true = ensure: entry (Phase 2)     */
    STA_OOP  ensure_block;          /* ensure: block to evaluate on unwind*/
    struct STA_HandlerEntry *prev;  /* previous handler in chain          */
} STA_HandlerEntry;

/* ── Handler chain API ─────────────────────────────────────────────────── */

/* Push an entry onto the handler chain. Sets entry->prev and updates top. */
void sta_handler_push(struct STA_VM *vm, STA_HandlerEntry *entry);

/* Pop the top entry. Sets top = top->prev. */
void sta_handler_pop(struct STA_VM *vm);

/* Walk the chain from top, looking for an entry whose exception_class
 * matches the given exception's class (isKindOf: check — walks superclass
 * chain). Returns the matching entry, or NULL if none found. */
STA_HandlerEntry *sta_handler_find(struct STA_VM *vm, STA_OOP exception);

/* ── Signal exception storage ──────────────────────────────────────────── */
/* Stored in STA_VM (not in the handler entry) to avoid C volatile issues
 * with setjmp/longjmp. signal sets it before longjmp; on:do: reads it
 * after setjmp returns non-zero. */

void    sta_handler_set_signaled_exception(struct STA_VM *vm, STA_OOP exc);
STA_OOP sta_handler_get_signaled_exception(struct STA_VM *vm);
