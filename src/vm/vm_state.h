/* src/vm/vm_state.h
 * Private definition of STA_VM and STA_ExecContext.
 * Not part of the public API — the public API declares STA_VM as opaque.
 * Only src/ files include this header to access the fields.
 *
 * Phase 2, Epic 0: all mutable runtime state is inline in STA_VM.
 * One calloc() for the whole VM struct.
 *
 * Globals that stay global (by design):
 *   - g_last_error (vm.c)         — pre-VM-creation error reporting
 *   - kernel_files (kernel_load.c) — const file list
 *   - special_selector_names (special_selectors.c) — const names
 */
#pragma once

#include "heap.h"
#include "immutable_space.h"
#include "symbol_table.h"
#include "class_table.h"
#include "frame.h"
#include "special_objects.h"
#include "primitive_table.h"
#include "handler.h"
#include "handle.h"
#include <sta/vm.h>
#include <stdbool.h>

/* ── STA_ExecContext ───────────────────────────────────────────────────── */

/* Passed to every primitive function. vm is always valid;
 * actor is NULL until Epic 3 (per-actor scheduling). */
typedef struct STA_ExecContext {
    struct STA_VM    *vm;
    struct STA_Actor *actor;
} STA_ExecContext;

/* ── STA_VM ────────────────────────────────────────────────────────────── */

struct STA_VM {
    /* Object memory — inline, not heap-allocated pointers */
    STA_Heap             heap;
    STA_ImmutableSpace   immutable_space;
    STA_SymbolTable      symbol_table;
    STA_ClassTable       class_table;

    /* Special object table (well-known roots) */
    STA_OOP              specials[STA_SPECIAL_OBJECTS_COUNT];

    /* Primitive function table */
    STA_PrimFn           primitives[STA_PRIM_TABLE_SIZE];

    /* Execution — stack slab */
    STA_StackSlab        slab;

    /* Exception handler state (moves to STA_Actor in Epic 3) */
    STA_HandlerEntry    *handler_top;
    STA_OOP              signaled_exception;

    /* Handle table (per ADR 013: reference-counted, growable) */
    STA_HandleTable      handles;

    /* Error reporting */
    char                 last_error[512];

    /* Inspection (sta_inspect_cstring output buffer — VM-owned, not thread-safe) */
    char                 inspect_buffer[1024];

    /* Configuration */
    STA_VMConfig         config;

    /* State flags */
    bool                 bootstrapped;
    bool                 destroyed;
};
