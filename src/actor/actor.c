/* src/actor/actor.c
 * Production actor struct — Phase 2 Epic 2.
 * See actor.h for documentation.
 */
#include "actor.h"
#include "deep_copy.h"
#include "mailbox_msg.h"
#include "scheduler/scheduler.h"
#include "gc/gc.h"
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
    actor->saved_frame = NULL;
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
        struct STA_VM *vm = sender->vm;
        STA_ClassTable *ct = &vm->class_table;

        /* ── Pre-flight size estimation (GitHub #295) ──────────────
         * Estimate the total bytes needed for deep copying all args,
         * plus the args array itself. If the target heap doesn't have
         * enough free space, GC and/or grow it once upfront. */
        size_t estimated = sta_deep_copy_estimate_roots(args, nargs, ct);

        /* Add the args array allocation cost. */
        size_t args_alloc = sta_alloc_size((uint32_t)nargs);
        args_alloc = (args_alloc + 15u) & ~(size_t)15u;
        estimated += args_alloc;

        size_t free_space = target->heap.capacity - target->heap.used;
        if (estimated > free_space) {
            /* Try GC first. */
            sta_gc_collect(vm, target);
            free_space = target->heap.capacity - target->heap.used;

            if (estimated > free_space) {
                /* Still insufficient — grow the heap. */
                size_t needed = target->heap.used + estimated;
                /* Add 50% breathing room. */
                size_t new_cap = needed + needed / 2;
                if (sta_heap_grow(&target->heap, new_cap) != 0)
                    return -1;
            }
        }

        /* Allocate the args array on the target's heap.
         * Use GC-aware allocation as a safety net (pre-flight should
         * have ensured enough space, but defense in depth). */
        STA_ObjHeader *args_h = sta_heap_alloc_gc(vm, target,
                                                    STA_CLS_ARRAY,
                                                    (uint32_t)nargs);
        if (!args_h) return -1;
        copied_args = sta_payload(args_h);

        for (uint8_t i = 0; i < nargs; i++) {
            copied_args[i] = sta_deep_copy_gc(args[i],
                                               &sender->heap,
                                               vm, target, ct);
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
    if (!msg) return STA_ACTOR_MSG_EMPTY;

    /* 2. Look up the selector in the actor's behavior class. */
    STA_ObjHeader *beh_h = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    uint32_t cls_idx = beh_h->class_index;
    STA_OOP method = actor_method_lookup(vm, cls_idx, msg->selector);

    if (method == 0) {
        sta_mailbox_msg_destroy(msg);
        return STA_ACTOR_MSG_ERROR;
    }

    /* 3. Execute — swap root_actor for non-scheduler dispatch path. */
    struct STA_Actor *saved_root = vm->root_actor;
    vm->root_actor = actor;

    (void)sta_interpret(vm, method, actor->behavior_obj,
                         msg->args, msg->arg_count);

    vm->root_actor = saved_root;

    /* 4. Free the message envelope. */
    sta_mailbox_msg_destroy(msg);

    return STA_ACTOR_MSG_PROCESSED;
}

int sta_actor_process_one_preemptible(struct STA_VM *vm, struct STA_Actor *actor)
{
    /* If the actor was preempted mid-execution, resume. */
    if (actor->saved_frame) {
        int rc = sta_interpret_resume(vm, actor);
        if (rc == STA_INTERPRET_PREEMPTED) {
            return STA_ACTOR_MSG_PREEMPTED;
        }
        return STA_ACTOR_MSG_PROCESSED;
    }

    /* Dequeue a new message. */
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&actor->mailbox);
    if (!msg) return STA_ACTOR_MSG_EMPTY;

    /* Look up the method. */
    STA_ObjHeader *beh_h = (STA_ObjHeader *)(uintptr_t)actor->behavior_obj;
    uint32_t cls_idx = beh_h->class_index;
    STA_OOP method = actor_method_lookup(vm, cls_idx, msg->selector);

    if (method == 0) {
        sta_mailbox_msg_destroy(msg);
        return STA_ACTOR_MSG_ERROR;
    }

    /* Execute with preemption support. */
    int rc = sta_interpret_actor(vm, actor, method, actor->behavior_obj,
                                  msg->args, msg->arg_count);

    /* Free the message envelope.
     * Note: if preempted, the method is still mid-execution, but the
     * message args have been copied to the frame. Safe to free. */
    sta_mailbox_msg_destroy(msg);

    if (rc == STA_INTERPRET_PREEMPTED) {
        return STA_ACTOR_MSG_PREEMPTED;
    }
    return STA_ACTOR_MSG_PROCESSED;
}
