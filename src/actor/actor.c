/* src/actor/actor.c
 * Production actor struct — Phase 2 Epic 2.
 * See actor.h for documentation.
 */
#include "actor.h"
#include "deep_copy.h"
#include "mailbox_msg.h"
#include "scheduler/scheduler.h"
#include "vm/vm_state.h"
#include "vm/interpreter.h"
#include "vm/method_dict.h"
#include "vm/class_table.h"
#include "vm/special_objects.h"
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
    atomic_store_explicit(&actor->state, STA_ACTOR_CREATED, memory_order_relaxed);
    actor->actor_id = 0;  /* assigned by VM or scheduler */
    actor->next_runnable = NULL;

    /* Initialize mailbox with default capacity. */
    if (sta_mailbox_init(&actor->mailbox, STA_MAILBOX_DEFAULT_CAPACITY) != 0) {
        sta_stack_slab_deinit(&actor->slab);
        sta_heap_deinit(&actor->heap);
        free(actor);
        return NULL;
    }

    /* Placeholders. */
    actor->behavior_class = 0;
    actor->behavior_obj = 0;
    actor->supervisor = NULL;

    return actor;
}

void sta_actor_destroy(struct STA_Actor *actor) {
    if (!actor) return;

    sta_mailbox_destroy(&actor->mailbox);
    sta_stack_slab_deinit(&actor->slab);
    sta_heap_deinit(&actor->heap);

    atomic_store_explicit(&actor->state, STA_ACTOR_TERMINATED, memory_order_relaxed);
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

    /* Auto-schedule: if the target is idle (CREATED or SUSPENDED) and the
     * scheduler is running, transition to READY and enqueue.
     * Uses CAS to ensure only one sender wins the race. */
    STA_Scheduler *sched = target->vm ? target->vm->scheduler : NULL;
    if (sched && atomic_load_explicit(&sched->running, memory_order_acquire)) {
        uint32_t expected = STA_ACTOR_SUSPENDED;
        if (atomic_compare_exchange_strong_explicit(
                &target->state, &expected, STA_ACTOR_READY,
                memory_order_acq_rel, memory_order_relaxed)) {
            sta_scheduler_enqueue(sched, target);
        } else {
            expected = STA_ACTOR_CREATED;
            if (atomic_compare_exchange_strong_explicit(
                    &target->state, &expected, STA_ACTOR_READY,
                    memory_order_acq_rel, memory_order_relaxed)) {
                sta_scheduler_enqueue(sched, target);
            }
        }
    }

    return 0;
}

/* ── Message dispatch ────────────────────────────────────────────────── */

/* Look up selector in class hierarchy starting from class_index. */
static STA_OOP actor_method_lookup(STA_VM *vm, uint32_t class_index,
                                    STA_OOP selector) {
    STA_OOP nil_oop = vm->specials[SPC_NIL];
    STA_OOP cls = sta_class_table_get(&vm->class_table, class_index);
    while (cls != 0 && cls != nil_oop) {
        STA_OOP md = sta_class_method_dict(cls);
        if (md != 0) {
            STA_OOP method = sta_method_dict_lookup(md, selector);
            if (method != 0) return method;
        }
        cls = sta_class_superclass(cls);
    }
    return 0;
}

int sta_actor_process_one(struct STA_VM *vm, struct STA_Actor *actor)
{
    /* 1. Dequeue one message from the mailbox. */
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&actor->mailbox);
    if (!msg) return 0;  /* empty — nothing to do */

    /* 2. Look up the selector in the actor's behavior class. */
    STA_ObjHeader *beh_h = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    uint32_t cls_idx = beh_h->class_index;
    STA_OOP method = actor_method_lookup(vm, cls_idx, msg->selector);

    if (method == 0) {
        /* Method not found — free envelope and return error.
         * In production, this would trigger doesNotUnderstand:. */
        sta_mailbox_msg_destroy(msg);
        return -1;
    }

    /* 3. Execute the method on the target actor's resources.
     * Temporarily swap vm->root_actor so sta_interpret uses this
     * actor's heap and stack slab. This is correct for single-threaded
     * manual dispatch (no scheduler yet). */
    struct STA_Actor *saved_root = vm->root_actor;
    vm->root_actor = actor;

    (void)sta_interpret(vm, method, actor->behavior_obj,
                         msg->args, msg->arg_count);

    vm->root_actor = saved_root;

    /* 4. Free the message envelope. */
    sta_mailbox_msg_destroy(msg);

    return 1;  /* message processed */
}
