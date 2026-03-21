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
#include <stdatomic.h>

/* ── STA_ExecContext ───────────────────────────────────────────────────── */

/* Passed to every primitive function. vm is always valid;
 * actor is set to root_actor after bootstrap (NULL during bootstrap). */
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

    /* Exception handler state — bootstrap-only fallback.
     * During normal execution, handlers live on the root actor. */
    STA_HandlerEntry    *handler_top;
    STA_OOP              signaled_exception;

    /* Root actor — created after bootstrap; all execution runs inside it. */
    struct STA_Actor    *root_actor;

    /* Root supervisor — top of the supervision tree. Created at VM init.
     * All actors created via sta_vm_spawn_supervised become children of
     * the root supervisor. */
    struct STA_Actor    *root_supervisor;

    /* Actor registry — VM-wide actor_id → STA_Actor* lookup table. */
    struct STA_ActorRegistry *registry;

    /* Future table — VM-wide future_id → STA_Future* lookup table (Epic 7A). */
    struct STA_FutureTable *future_table;

    /* Next actor ID counter — atomic for thread-safe assignment. */
    _Atomic uint32_t next_actor_id;

    /* Scheduler — optionally started for multi-core execution. */
    struct STA_Scheduler *scheduler;

    /* Handle table (per ADR 013: reference-counted, growable) */
    STA_HandleTable      handles;

    /* Event callback table (max 16 callbacks). */
    struct {
        STA_EventCallback callback;
        void             *ctx;
    }                    event_cbs[16];
    uint8_t              event_cb_count;

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

/* Fire an event to all registered callbacks. Internal API. */
void sta_vm_fire_event(STA_VM *vm, STA_EventType type, const char *message);
