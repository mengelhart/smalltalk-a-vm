/* src/vm/vm_internal.h
 * Private definition of STA_VM. Not part of the public API.
 * The public API in include/sta/vm.h declares STA_VM as an opaque type.
 * Only src/ files include this header to access the fields.
 *
 * ── Story 1 Audit: current state management ──────────────────────────
 *
 * Global state (prevents multiple VM instances in Phase 1):
 *   - sta_special_objects[32]     (special_objects.c)    — well-known roots
 *   - sta_primitives[256]         (primitive_table.c)    — C function pointers
 *   - g_prim_class_table          (primitive_table.c)    — primitive context
 *   - g_prim_heap                 (primitive_table.c)    — primitive context
 *   - g_prim_slab                 (primitive_table.c)    — primitive context
 *   - g_prim_symbol_table         (primitive_table.c)    — primitive context
 *   - g_prim_immutable_space      (primitive_table.c)    — primitive context
 *   - g_handler_top               (handler.c)            — exception chain
 *   - g_signaled_exception        (handler.c)            — exception state
 *
 * Parameter-passed state (clean, per-instance ready):
 *   - STA_Heap*            — created via sta_heap_create(), passed everywhere
 *   - STA_ImmutableSpace*  — created via sta_immutable_space_create()
 *   - STA_SymbolTable*     — created via sta_symbol_table_create()
 *   - STA_ClassTable*      — created via sta_class_table_create()
 *   - STA_StackSlab*       — created via sta_stack_slab_create()
 *
 * Phase 1 limitation: only one STA_VM instance at a time due to globals.
 * Phase 2 will move globals into the struct when the actor runtime needs it.
 */
#pragma once

#include "heap.h"
#include "immutable_space.h"
#include "symbol_table.h"
#include "class_table.h"
#include "frame.h"
#include <sta/vm.h>
#include <stdbool.h>

struct STA_VM {
    /* Object memory */
    STA_Heap             *heap;
    STA_ImmutableSpace   *immutable_space;
    STA_SymbolTable      *symbol_table;
    STA_ClassTable       *class_table;

    /* Execution */
    STA_StackSlab        *stack_slab;

    /* Error reporting */
    char                  last_error[512];

    /* Inspection (sta_inspect_cstring output buffer — VM-owned, not thread-safe) */
    char                  inspect_buffer[1024];

    /* Configuration */
    STA_VMConfig          config;

    /* State flags */
    bool                  bootstrapped;
    bool                  destroyed;
};
