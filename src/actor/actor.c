/* src/actor/actor.c
 * Production actor struct — Phase 2 Epic 2.
 * See actor.h for documentation.
 */
#include "actor.h"
#include <stdlib.h>
#include <string.h>

/* Public API stubs — still required by sta/vm.h declarations. */
#include <sta/vm.h>

STA_Actor *sta_actor_spawn(STA_VM *vm, STA_Handle *class_handle) {
    (void)vm; (void)class_handle;
    return NULL;
}

int sta_actor_send(STA_VM *vm, STA_Actor *actor, STA_Handle *message) {
    (void)vm; (void)actor; (void)message;
    return STA_ERR_INTERNAL;
}

/* ── Production lifecycle ────────────────────────────────────────────── */

struct STA_Actor *sta_actor_create(struct STA_VM *vm,
                                   size_t heap_size,
                                   size_t stack_size)
{
    struct STA_Actor *actor = calloc(1, sizeof(struct STA_Actor));
    if (!actor) return NULL;

    actor->vm = vm;

    /* Initialize per-actor heap. */
    if (sta_heap_init(&actor->heap, heap_size) != 0) {
        free(actor);
        return NULL;
    }

    /* Initialize per-actor stack slab. */
    if (sta_stack_slab_init(&actor->slab, stack_size) != 0) {
        sta_heap_deinit(&actor->heap);
        free(actor);
        return NULL;
    }

    /* Handler chain starts empty. */
    actor->handler_top = NULL;
    actor->signaled_exception = 0;

    /* Lifecycle state. */
    actor->state = STA_ACTOR_CREATED;
    actor->actor_id = 0;  /* assigned by VM or scheduler */

    /* Placeholders. */
    actor->behavior_class = 0;
    actor->mailbox = NULL;
    actor->supervisor = NULL;

    return actor;
}

void sta_actor_destroy(struct STA_Actor *actor) {
    if (!actor) return;

    sta_stack_slab_deinit(&actor->slab);
    sta_heap_deinit(&actor->heap);

    actor->state = STA_ACTOR_TERMINATED;
    free(actor);
}
