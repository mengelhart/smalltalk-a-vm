/* src/actor/supervisor.c
 * Supervisor operations — Phase 2 Epic 6.
 * See supervisor.h for documentation.
 */
#include "supervisor.h"
#include "actor.h"
#include "mailbox.h"
#include "mailbox_msg.h"
#include "vm/vm_state.h"
#include "vm/oop.h"
#include "vm/heap.h"
#include "vm/special_objects.h"
#include "scheduler/scheduler.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>

/* Default heap and stack sizes for child actors. */
#define CHILD_HEAP_SIZE  128u
#define CHILD_STACK_SIZE 512u

/* ── Lifecycle ────────────────────────────────────────────────────────── */

int sta_supervisor_init(struct STA_Actor *actor,
                        uint32_t max_restarts,
                        uint32_t max_seconds)
{
    if (!actor) return -1;

    STA_SupervisorData *data = calloc(1, sizeof(STA_SupervisorData));
    if (!data) return -1;

    data->children      = NULL;
    data->child_count   = 0;
    data->restart_count = 0;
    data->window_start_ns = 0;
    data->max_restarts  = max_restarts;
    data->max_seconds   = max_seconds;

    actor->sup_data = data;
    return 0;
}

struct STA_Actor *sta_supervisor_add_child(struct STA_Actor *supervisor,
                                            STA_OOP behavior_class,
                                            STA_RestartStrategy strategy)
{
    if (!supervisor || !supervisor->sup_data) return NULL;

    /* Create the child actor. */
    struct STA_Actor *child = sta_actor_create(supervisor->vm,
                                                CHILD_HEAP_SIZE,
                                                CHILD_STACK_SIZE);
    if (!child) return NULL;

    /* Wire supervisor pointer. */
    child->supervisor = supervisor;
    child->behavior_class = behavior_class;

    /* Assign a unique actor_id. Use a simple atomic counter on the VM.
     * For now, use a non-atomic increment — Story 1 is single-threaded.
     * The scheduler assigns proper IDs in production. */
    static uint32_t next_id = 100;
    child->actor_id = next_id++;

    /* Create child spec. */
    STA_ChildSpec *spec = calloc(1, sizeof(STA_ChildSpec));
    if (!spec) {
        sta_actor_destroy(child);
        return NULL;
    }

    spec->behavior_class = behavior_class;
    spec->strategy       = strategy;
    spec->current_actor  = child;
    spec->next           = supervisor->sup_data->children;

    supervisor->sup_data->children = spec;
    supervisor->sup_data->child_count++;

    return child;
}

void sta_supervisor_data_destroy(STA_SupervisorData *data)
{
    if (!data) return;

    /* Free all child specs. */
    STA_ChildSpec *spec = data->children;
    while (spec) {
        STA_ChildSpec *next = spec->next;
        free(spec);
        spec = next;
    }

    free(data);
}

/* ── Failure handling ─────────────────────────────────────────────────── */

/* Find child spec by actor_id. */
static STA_ChildSpec *find_child_by_id(STA_SupervisorData *data,
                                         uint32_t actor_id) {
    STA_ChildSpec *spec = data->children;
    while (spec) {
        if (spec->current_actor &&
            spec->current_actor->actor_id == actor_id) {
            return spec;
        }
        spec = spec->next;
    }
    return NULL;
}

/* Create a new actor for a child spec (used by RESTART). */
static struct STA_Actor *restart_child(struct STA_Actor *supervisor,
                                         STA_ChildSpec *spec) {
    struct STA_Actor *child = sta_actor_create(supervisor->vm,
                                                CHILD_HEAP_SIZE,
                                                CHILD_STACK_SIZE);
    if (!child) return NULL;

    child->supervisor = supervisor;
    child->behavior_class = spec->behavior_class;

    /* Assign new actor_id. */
    static uint32_t restart_id = 10000;
    child->actor_id = restart_id++;

    /* Allocate a behavior_obj on the child's heap (instance of behavior_class).
     * Need to get the class index from the class table. */
    struct STA_VM *vm = supervisor->vm;
    uint32_t cls_idx = sta_class_table_index_of(&vm->class_table,
                                                  spec->behavior_class);
    if (cls_idx != 0) {
        /* Get inst var count from the class format. */
        STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)spec->behavior_class;
        STA_OOP *cls_slots = sta_payload(cls_h);
        STA_OOP format_oop = cls_slots[2];  /* STA_CLASS_SLOT_FORMAT */
        uint32_t inst_vars = 0;
        if (STA_IS_SMALLINT(format_oop)) {
            int64_t fmt = STA_SMALLINT_VAL(format_oop);
            inst_vars = (uint32_t)(fmt & 0xFF);  /* low 8 bits = instVarCount */
        }

        STA_ObjHeader *obj_h = sta_heap_alloc(&child->heap, cls_idx, inst_vars);
        if (obj_h) {
            /* Initialize inst vars to nil. */
            STA_OOP nil_oop = vm->specials[SPC_NIL];
            STA_OOP *slots = sta_payload(obj_h);
            for (uint32_t i = 0; i < inst_vars; i++)
                slots[i] = nil_oop;
            child->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
        }
    }

    /* Send #initialize to the new actor's mailbox. */
    STA_OOP init_sel = sta_spc_get(SPC_INITIALIZE);
    if (init_sel != 0) {
        STA_MailboxMsg *init_msg = sta_mailbox_msg_create(init_sel, NULL, 0,
                                                            supervisor->actor_id);
        if (init_msg) {
            sta_mailbox_enqueue(&child->mailbox, init_msg);
        }
    }

    /* Set to CREATED — auto-schedule will transition when appropriate. */
    atomic_store_explicit(&child->state, STA_ACTOR_CREATED, memory_order_relaxed);

    /* If scheduler is running, auto-schedule the new actor. */
    STA_Scheduler *sched = vm->scheduler;
    if (sched && atomic_load_explicit(&sched->running, memory_order_acquire)) {
        uint32_t expected = STA_ACTOR_CREATED;
        if (atomic_compare_exchange_strong_explicit(
                &child->state, &expected, STA_ACTOR_READY,
                memory_order_acq_rel, memory_order_relaxed)) {
            sta_scheduler_enqueue(sched, child);
        }
    }

    return child;
}

int sta_supervisor_handle_failure(struct STA_Actor *supervisor,
                                   STA_OOP *args, uint32_t arg_count)
{
    if (!supervisor || !supervisor->sup_data || arg_count < 2) return -1;

    /* Extract failed actor_id from args[0] (SmallInt). */
    STA_OOP id_oop = args[0];
    if (!STA_IS_SMALLINT(id_oop)) return -1;
    uint32_t failed_id = (uint32_t)STA_SMALLINT_VAL(id_oop);

    /* Find the child spec. */
    STA_ChildSpec *spec = find_child_by_id(supervisor->sup_data, failed_id);
    if (!spec) {
        /* Unknown child or already removed — log and return. */
        return 0;
    }

    struct STA_Actor *old_actor = spec->current_actor;

    switch (spec->strategy) {
    case STA_RESTART_RESTART: {
        /* Destroy old actor. */
        if (old_actor) {
            old_actor->supervisor = NULL;  /* Prevent recursive teardown */
            sta_actor_destroy(old_actor);
        }

        /* Create new actor of same class. */
        struct STA_Actor *new_actor = restart_child(supervisor, spec);
        spec->current_actor = new_actor;

        if (!new_actor) {
            fprintf(stderr, "supervisor: restart failed for child %u\n", failed_id);
            return -1;
        }
        break;
    }

    case STA_RESTART_STOP:
        /* Permanently stop — do not restart. Slot remains with NULL actor. */
        if (old_actor) {
            old_actor->supervisor = NULL;
            sta_actor_destroy(old_actor);
        }
        spec->current_actor = NULL;
        break;

    case STA_RESTART_ESCALATE: {
        /* Destroy the failed child. */
        if (old_actor) {
            old_actor->supervisor = NULL;
            sta_actor_destroy(old_actor);
        }
        spec->current_actor = NULL;

        /* Forward the failure to this supervisor's supervisor. */
        struct STA_Actor *grandparent = supervisor->supervisor;
        if (!grandparent) {
            fprintf(stderr, "supervisor %u: ESCALATE but no grandparent — "
                    "dropping failure for child %u\n",
                    supervisor->actor_id, failed_id);
            break;
        }

        /* Don't escalate to a terminated grandparent. */
        uint32_t gp_state = atomic_load_explicit(&grandparent->state,
                                                   memory_order_acquire);
        if (gp_state == STA_ACTOR_TERMINATED) break;

        /* Build childFailed:reason: notification for grandparent.
         * args[0] = THIS supervisor's actor_id (not original child's).
         * args[1] = original reason symbol (from args[1]). */
        STA_OOP sup_id_oop = STA_SMALLINT_OOP((intptr_t)supervisor->actor_id);
        STA_OOP reason = args[1];
        STA_OOP selector = sta_spc_get(SPC_CHILD_FAILED_REASON);

        /* Allocate args array on the grandparent's heap. */
        STA_ObjHeader *gp_args_h = sta_heap_alloc(&grandparent->heap,
                                                     STA_CLS_ARRAY, 2);
        if (!gp_args_h) break;  /* Grandparent heap full — drop */
        STA_OOP *gp_copied = sta_payload(gp_args_h);
        gp_copied[0] = sup_id_oop;
        gp_copied[1] = reason;

        STA_MailboxMsg *notif = sta_mailbox_msg_create(selector, gp_copied, 2,
                                                         supervisor->actor_id);
        if (!notif) break;

        int rc = sta_mailbox_enqueue(&grandparent->mailbox, notif);
        if (rc != 0) {
            sta_mailbox_msg_destroy(notif);
            break;
        }

        /* Auto-schedule grandparent if idle. */
        STA_Scheduler *sched = grandparent->vm ? grandparent->vm->scheduler : NULL;
        if (sched && atomic_load_explicit(&sched->running,
                                            memory_order_acquire)) {
            uint32_t expected = STA_ACTOR_SUSPENDED;
            if (atomic_compare_exchange_strong_explicit(
                    &grandparent->state, &expected, STA_ACTOR_READY,
                    memory_order_acq_rel, memory_order_relaxed)) {
                sta_scheduler_enqueue(sched, grandparent);
            } else {
                expected = STA_ACTOR_CREATED;
                if (atomic_compare_exchange_strong_explicit(
                        &grandparent->state, &expected, STA_ACTOR_READY,
                        memory_order_acq_rel, memory_order_relaxed)) {
                    sta_scheduler_enqueue(sched, grandparent);
                }
            }
        }
        break;
    }
    }

    return 0;
}
