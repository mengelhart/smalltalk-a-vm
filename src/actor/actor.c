/* src/actor/actor.c
 * Production actor struct — Phase 2 Epic 2.
 * See actor.h for documentation.
 */
#include "actor.h"
#include "registry.h"
#include "supervisor.h"
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
#include <stdio.h>
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

    /* Reference count: 1 = the registry holds the owning reference. */
    atomic_store_explicit(&actor->refcount, 1, memory_order_relaxed);

    /* Assign a unique actor_id from the VM's atomic counter.
     * If no VM is provided (test-only path), leave at 0. */
    if (vm) {
        actor->actor_id = atomic_fetch_add_explicit(
            &vm->next_actor_id, 1, memory_order_relaxed);
    } else {
        actor->actor_id = 0;
    }

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
    actor->sup_data = NULL;

    /* NOTE: The caller must call sta_registry_register() after finishing
     * initialization (behavior_obj, supervisor, etc.).  Registering here
     * would expose a half-initialized actor to concurrent lookups.
     * See GitHub #320. */

    return actor;
}

void sta_actor_register(struct STA_Actor *actor) {
    if (!actor || !actor->vm) return;
    struct STA_VM *vm = actor->vm;
    if (vm->registry) {
        sta_registry_register(vm->registry, actor);
    }
}

/* Forward declaration — used by sta_actor_terminate. */
static void drain_mailbox(struct STA_Actor *actor);

/* ── Internal: free actor resources (called when refcount reaches 0) ── */

static void sta_actor_free(struct STA_Actor *actor) {
    if (actor->sup_data) {
        sta_supervisor_data_destroy(actor->sup_data);
        actor->sup_data = NULL;
    }

    sta_mailbox_destroy(&actor->mailbox);
    sta_stack_slab_deinit(&actor->slab);
    sta_heap_deinit(&actor->heap);
    free(actor);
}

void sta_actor_release(struct STA_Actor *actor) {
    if (!actor) return;

    uint32_t prev = atomic_fetch_sub_explicit(&actor->refcount, 1,
                                               memory_order_acq_rel);
    if (prev == 1) {
        /* refcount reached 0 — we are the last reference. Free. */
        sta_actor_free(actor);
    }
}

void sta_actor_terminate(struct STA_Actor *actor) {
    if (!actor) return;

    /* Set state to TERMINATED. */
    atomic_store_explicit(&actor->state, STA_ACTOR_TERMINATED,
                          memory_order_release);

    /* Unregister from the VM-wide actor registry — no new lookups. */
    if (actor->vm && actor->vm->registry) {
        sta_registry_unregister(actor->vm->registry, actor->actor_id);
    }

    /* Do NOT drain the mailbox here — the actor may still be RUNNING on
     * another worker thread, and the mailbox is single-consumer.
     * sta_mailbox_destroy (called from sta_actor_free when refcount
     * reaches 0) safely drains any remaining messages. */

    /* If this actor is a supervisor, terminate children depth-first. */
    if (actor->sup_data) {
        STA_ChildSpec *spec = actor->sup_data->children;
        while (spec) {
            if (spec->current_actor) {
                sta_actor_terminate(spec->current_actor);
                spec->current_actor = NULL;
            }
            spec = spec->next;
        }
    }

    /* Release the registry's owning reference.
     * If no other thread holds a reference, this frees the actor. */
    sta_actor_release(actor);
}

/* ── Messaging ───────────────────────────────────────────────────────── */

int sta_actor_send_msg(struct STA_VM *vm,
                       struct STA_Actor *sender,
                       uint32_t target_id,
                       STA_OOP selector,
                       STA_OOP *args, uint8_t nargs)
{
    /* Resolve target by ID through the actor registry.
     * Registry lookup atomically increments refcount under the mutex,
     * so the target cannot be freed while we hold the reference. */
    struct STA_Actor *target = sta_registry_lookup(vm->registry, target_id);
    if (!target) return STA_ERR_ACTOR_DEAD;

    /* Check if the actor was terminated after lookup but before we use it.
     * Refcount keeps it alive but it's logically dead. */
    uint32_t tstate = atomic_load_explicit(&target->state, memory_order_acquire);
    if (tstate == STA_ACTOR_TERMINATED) {
        sta_actor_release(target);
        return STA_ERR_ACTOR_DEAD;
    }

    /* Deep copy each argument from sender's heap to target's heap. */
    STA_OOP *copied_args = NULL;
    bool args_mallocd = false;

    if (nargs > 0) {
        STA_ClassTable *ct = &vm->class_table;

        /* ── Pre-flight size estimation (GitHub #295) ──────────────
         * Estimate the total bytes needed for deep copying all args.
         * If zero, all args are immediates or immutables — no target
         * heap access needed. Use a malloc'd array instead, avoiding
         * the heap allocation entirely. */
        size_t estimated = sta_deep_copy_estimate_roots(args, nargs, ct);

        if (estimated == 0) {
            /* Zero-copy fast path: all args are immediates/immutables.
             * Allocate the args array via malloc (not on any heap). */
            copied_args = malloc((size_t)nargs * sizeof(STA_OOP));
            if (!copied_args) { sta_actor_release(target); return -1; }
            args_mallocd = true;
            for (uint8_t i = 0; i < nargs; i++)
                copied_args[i] = args[i];
        } else {
            /* Deep copy path: target heap allocation required. */

            /* Add the args array allocation cost. */
            size_t args_alloc = sta_alloc_size((uint32_t)nargs);
            args_alloc = (args_alloc + 15u) & ~(size_t)15u;
            estimated += args_alloc;

            size_t free_space = target->heap.capacity - target->heap.used;
            if (estimated > free_space) {
                size_t needed = target->heap.used + estimated;
                size_t new_cap = needed + needed / 2;
                if (sta_heap_grow(&target->heap, new_cap) != 0) {
                    sta_actor_release(target);
                    return -1;
                }
            }

            STA_ObjHeader *args_h = sta_heap_alloc_gc(vm, target,
                                                        STA_CLS_ARRAY,
                                                        (uint32_t)nargs);
            if (!args_h) { sta_actor_release(target); return -1; }
            copied_args = sta_payload(args_h);

            for (uint8_t i = 0; i < nargs; i++) {
                copied_args[i] = sta_deep_copy_gc(args[i],
                                                   &sender->heap,
                                                   vm, target, ct);
                if (copied_args[i] == 0 && args[i] != 0 &&
                    !STA_IS_IMMEDIATE(args[i])) {
                    sta_actor_release(target);
                    return -1;
                }
            }
        }
    }

    /* Create the message envelope. */
    STA_MailboxMsg *msg = sta_mailbox_msg_create(
        selector, copied_args, nargs, sender->actor_id);
    if (!msg) {
        if (args_mallocd) free(copied_args);
        sta_actor_release(target);
        return -1;
    }
    msg->args_owned = args_mallocd;

    /* Enqueue in target's mailbox. */
    int rc = sta_mailbox_enqueue(&target->mailbox, msg);
    if (rc != 0) {
        /* Mailbox full — destroy the envelope (frees args if owned). */
        sta_mailbox_msg_destroy(msg);
        sta_actor_release(target);
        return rc;  /* STA_ERR_MAILBOX_FULL */
    }

    /* ── Store-buffer fence (GitHub #319) ───────────────────────────
     * The enqueue above wrote to the mailbox count (release).  The CAS
     * below reads the actor state.  Without a full fence, both this
     * thread and the scheduler thread can each read stale values of the
     * *other* variable (classic Dekker/store-buffer race).  A matching
     * seq_cst fence sits in the scheduler dispatch loop between the
     * SUSPENDED store and the mailbox re-check.  Together the two
     * fences guarantee: either our CAS sees SUSPENDED (and we wake the
     * actor) OR the scheduler's re-check sees count > 0 (and it
     * re-enqueues the actor). */
    atomic_thread_fence(memory_order_seq_cst);

    /* Auto-schedule: if the target is idle (CREATED or SUSPENDED) and the
     * scheduler is running, transition to READY and enqueue.
     * Uses CAS to ensure only one sender wins the race.
     * sta_scheduler_enqueue increments refcount for the queue. */
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

    /* Release the reference acquired by registry lookup. */
    sta_actor_release(target);
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

/* Drain all remaining messages from a terminated actor's mailbox. */
static void drain_mailbox(struct STA_Actor *actor) {
    STA_MailboxMsg *msg;
    while ((msg = sta_mailbox_dequeue(&actor->mailbox)) != NULL) {
        sta_mailbox_msg_destroy(msg);
    }
}

/* Extract the exception class name as an immutable Symbol.
 * Called while still on the failed actor's thread, before termination.
 * Returns a Symbol OOP in immutable space, or the fallback unknownError. */
static STA_OOP extract_reason_symbol(struct STA_VM *vm,
                                      struct STA_Actor *actor) {
    STA_OOP exc = actor->signaled_exception;
    if (exc != 0 && STA_IS_HEAP(exc)) {
        STA_ObjHeader *exc_h = (STA_ObjHeader *)(uintptr_t)exc;
        STA_OOP cls = sta_class_table_get(&vm->class_table, exc_h->class_index);
        if (cls != 0 && STA_IS_HEAP(cls)) {
            STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)cls;
            if (cls_h->size >= 4) {
                STA_OOP name_sym = sta_payload(cls_h)[STA_CLASS_SLOT_NAME];
                if (name_sym != 0 && STA_IS_HEAP(name_sym)) {
                    STA_ObjHeader *nh = (STA_ObjHeader *)(uintptr_t)name_sym;
                    if (nh->class_index == STA_CLS_SYMBOL)
                        return name_sym;  /* immutable space — safe */
                }
            }
        }
    }
    return sta_spc_get(SPC_UNKNOWN_ERROR);
}

/* Send a failure notification to the actor's supervisor.
 * Uses sta_actor_send_msg (registry-based) with immediates-only args:
 *   args[0] = failed actor_id as SmallInt (tagged immediate)
 *   args[1] = exception class name Symbol (immutable space)
 * Zero heap allocation on the supervisor — fixes #312. */
static void notify_supervisor(struct STA_VM *vm, struct STA_Actor *actor,
                               STA_OOP reason) {
    struct STA_Actor *sup = actor->supervisor;
    if (!sup) return;

    STA_OOP actor_id_oop = STA_SMALLINT_OOP((intptr_t)actor->actor_id);
    STA_OOP selector = sta_spc_get(SPC_CHILD_FAILED_REASON);

    /* Both args are immediates or immutable-space objects.
     * sta_actor_send_msg deep copy passes them through unchanged. */
    STA_OOP notif_args[2] = { actor_id_oop, reason };

    sta_actor_send_msg(vm, actor, sup->actor_id,
                       selector, notif_args, 2);
}

/* Handle an unhandled exception in a scheduled actor:
 * 1. Transition to TERMINATED (CAS for thread safety)
 * 2. Notify supervisor (if any)
 * 3. Drain remaining messages */
static int handle_actor_exception(struct STA_VM *vm, struct STA_Actor *actor) {
    /* Extract exception class name BEFORE termination — we are still
     * on the failed actor's thread and its heap is valid. */
    STA_OOP reason = extract_reason_symbol(vm, actor);

    /* CAS to TERMINATED — another thread should not race on this. */
    uint32_t expected = STA_ACTOR_RUNNING;
    atomic_compare_exchange_strong_explicit(
        &actor->state, &expected, STA_ACTOR_TERMINATED,
        memory_order_acq_rel, memory_order_relaxed);

    notify_supervisor(vm, actor, reason);
    drain_mailbox(actor);

    return STA_ACTOR_MSG_EXCEPTION;
}

int sta_actor_process_one_preemptible(struct STA_VM *vm, struct STA_Actor *actor)
{
    /* If the actor was preempted mid-execution, resume. */
    if (actor->saved_frame) {
        int rc = sta_interpret_resume(vm, actor);
        if (rc == STA_INTERPRET_PREEMPTED) {
            return STA_ACTOR_MSG_PREEMPTED;
        }
        if (rc == STA_INTERPRET_EXCEPTION) {
            return handle_actor_exception(vm, actor);
        }
        return STA_ACTOR_MSG_PROCESSED;
    }

    /* Dequeue a new message. */
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&actor->mailbox);
    if (!msg) return STA_ACTOR_MSG_EMPTY;

    /* Supervisor-aware dispatch (Epic 6 Story 3):
     * If this actor is a supervisor and the message is childFailed:reason:,
     * route to C-level failure handler instead of Smalltalk dispatch. */
    if (actor->sup_data &&
        msg->selector == sta_spc_get(SPC_CHILD_FAILED_REASON)) {
        sta_supervisor_handle_failure(actor, msg->args, msg->arg_count);
        sta_mailbox_msg_destroy(msg);
        return STA_ACTOR_MSG_PROCESSED;
    }

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
    if (rc == STA_INTERPRET_EXCEPTION) {
        return handle_actor_exception(vm, actor);
    }
    return STA_ACTOR_MSG_PROCESSED;
}
