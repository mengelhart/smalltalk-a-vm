/* src/actor/actor.c
 * Production actor struct — Phase 2 Epic 2.
 * See actor.h for documentation.
 */
#include "actor.h"
#include "deep_copy.h"
#include "mailbox_msg.h"
#include "vm/vm_state.h"
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

    /* Initialize mailbox with default capacity. */
    if (sta_mailbox_init(&actor->mailbox, STA_MAILBOX_DEFAULT_CAPACITY) != 0) {
        sta_stack_slab_deinit(&actor->slab);
        sta_heap_deinit(&actor->heap);
        free(actor);
        return NULL;
    }

    /* Placeholders. */
    actor->behavior_class = 0;
    actor->supervisor = NULL;

    return actor;
}

void sta_actor_destroy(struct STA_Actor *actor) {
    if (!actor) return;

    sta_mailbox_destroy(&actor->mailbox);
    sta_stack_slab_deinit(&actor->slab);
    sta_heap_deinit(&actor->heap);

    actor->state = STA_ACTOR_TERMINATED;
    free(actor);
}

/* ── Messaging ───────────────────────────────────────────────────────── */

int sta_actor_send_msg(struct STA_Actor *sender,
                       struct STA_Actor *target,
                       STA_OOP selector,
                       STA_OOP *args, uint8_t nargs)
{
    /* Deep copy each argument from sender's heap to target's heap.
     * The copied args array is allocated on the target's heap so it
     * lives alongside the target's other objects. */
    STA_OOP *copied_args = NULL;

    if (nargs > 0) {
        /* Allocate an array on the target's heap for the copied args. */
        STA_ObjHeader *args_h = sta_heap_alloc(&target->heap, STA_CLS_ARRAY,
                                                (uint32_t)nargs);
        if (!args_h) return -1;
        copied_args = sta_payload(args_h);

        /* Resolve class table from VM (sender and target share the same VM). */
        STA_ClassTable *ct = &sender->vm->class_table;

        for (uint8_t i = 0; i < nargs; i++) {
            copied_args[i] = sta_deep_copy(args[i],
                                            &sender->heap,
                                            &target->heap,
                                            ct);
            /* Check for allocation failure during deep copy. */
            if (copied_args[i] == 0 && args[i] != 0 &&
                !STA_IS_IMMEDIATE(args[i])) {
                /* Deep copy failed — but the args array is on the target
                 * heap and will be reclaimed by GC. No explicit cleanup. */
                return -1;
            }
        }
    }

    /* Create the message envelope. */
    STA_MailboxMsg *msg = sta_mailbox_msg_create(
        selector, copied_args, nargs, sender->actor_id);
    if (!msg) return -1;

    /* Enqueue in target's mailbox. */
    int rc = sta_mailbox_enqueue(&target->mailbox, msg);
    if (rc != 0) {
        /* Mailbox full — destroy the envelope. The args array on the
         * target heap will be reclaimed by GC eventually. */
        sta_mailbox_msg_destroy(msg);
        return rc;  /* STA_ERR_MAILBOX_FULL */
    }

    return 0;
}
