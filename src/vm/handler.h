/* src/vm/handler.h
 * Exception handler stack — Phase 1 handler-stack approach per §7.8.
 * Phase 1 — permanent.
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

/* ── Handler entry ─────────────────────────────────────────────────────── */

typedef struct STA_HandlerEntry {
    STA_OOP  exception_class;       /* class to catch (isKindOf: match)   */
    STA_OOP  handler_block;         /* handler block closure              */
    jmp_buf  jmp;                   /* setjmp/longjmp buffer              */
    uint8_t *saved_slab_top;        /* slab->top before body evaluation   */
    uint8_t *saved_slab_sp;         /* slab->sp before body evaluation    */
    struct STA_HandlerEntry *prev;  /* previous handler in chain          */
} STA_HandlerEntry;

/* ── Handler chain API ─────────────────────────────────────────────────── */

/* Get the current handler chain top. */
STA_HandlerEntry *sta_handler_top(void);

/* Set the handler chain top directly. */
void sta_handler_set_top(STA_HandlerEntry *entry);

/* Push an entry onto the handler chain. Sets entry->prev and updates top. */
void sta_handler_push(STA_HandlerEntry *entry);

/* Pop the top entry. Sets top = top->prev. */
void sta_handler_pop(void);

/* Walk the chain from top, looking for an entry whose exception_class
 * matches the given exception's class (isKindOf: check — walks superclass
 * chain). Returns the matching entry, or NULL if none found. */
STA_HandlerEntry *sta_handler_find(STA_OOP exception, STA_ClassTable *ct);

/* ── Signal exception storage ──────────────────────────────────────────── */
/* Stored in a global (not in the handler entry) to avoid C volatile issues
 * with setjmp/longjmp. signal sets it before longjmp; on:do: reads it
 * after setjmp returns non-zero. */

void  sta_handler_set_signaled_exception(STA_OOP exc);
STA_OOP sta_handler_get_signaled_exception(void);
