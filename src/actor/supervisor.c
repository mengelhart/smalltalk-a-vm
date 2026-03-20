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
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include "scheduler/scheduler.h"
#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

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

    /* actor_id is now assigned by sta_actor_create via the VM's
     * atomic counter, and registered in the VM-wide registry. */

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

    /* actor_id assigned by sta_actor_create via VM's atomic counter. */

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

/* Get current monotonic time in nanoseconds. */
static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* Terminate all living children of a supervisor.
 * Used when restart intensity is exceeded. */
static void terminate_all_children(struct STA_Actor *supervisor) {
    STA_ChildSpec *spec = supervisor->sup_data->children;
    while (spec) {
        if (spec->current_actor) {
            spec->current_actor->supervisor = NULL;
            sta_actor_destroy(spec->current_actor);
            spec->current_actor = NULL;
        }
        spec = spec->next;
    }
}

/* Send escalation notification to this supervisor's parent.
 * reason_oop is the reason symbol to include. */
static void escalate_to_parent(struct STA_Actor *supervisor, STA_OOP reason_oop) {
    struct STA_Actor *parent = supervisor->supervisor;
    if (!parent) {
        fprintf(stderr, "supervisor %u: intensity exceeded but no parent — "
                "dropping escalation\n", supervisor->actor_id);
        return;
    }

    uint32_t p_state = atomic_load_explicit(&parent->state, memory_order_acquire);
    if (p_state == STA_ACTOR_TERMINATED) return;

    STA_OOP sup_id_oop = STA_SMALLINT_OOP((intptr_t)supervisor->actor_id);
    STA_OOP selector = sta_spc_get(SPC_CHILD_FAILED_REASON);

    STA_ObjHeader *p_args_h = sta_heap_alloc(&parent->heap, STA_CLS_ARRAY, 2);
    if (!p_args_h) return;
    STA_OOP *p_copied = sta_payload(p_args_h);
    p_copied[0] = sup_id_oop;
    p_copied[1] = reason_oop;

    STA_MailboxMsg *notif = sta_mailbox_msg_create(selector, p_copied, 2,
                                                     supervisor->actor_id);
    if (!notif) return;

    int rc = sta_mailbox_enqueue(&parent->mailbox, notif);
    if (rc != 0) {
        sta_mailbox_msg_destroy(notif);
        return;
    }

    /* Auto-schedule parent if idle. */
    STA_Scheduler *sched = parent->vm ? parent->vm->scheduler : NULL;
    if (sched && atomic_load_explicit(&sched->running, memory_order_acquire)) {
        uint32_t expected = STA_ACTOR_SUSPENDED;
        if (atomic_compare_exchange_strong_explicit(
                &parent->state, &expected, STA_ACTOR_READY,
                memory_order_acq_rel, memory_order_relaxed)) {
            sta_scheduler_enqueue(sched, parent);
        } else {
            expected = STA_ACTOR_CREATED;
            if (atomic_compare_exchange_strong_explicit(
                    &parent->state, &expected, STA_ACTOR_READY,
                    memory_order_acq_rel, memory_order_relaxed)) {
                sta_scheduler_enqueue(sched, parent);
            }
        }
    }
}

/* Check restart intensity. Returns 0 if restart is allowed, -1 if exceeded.
 * When exceeded, terminates all children, escalates to parent, and
 * transitions this supervisor to TERMINATED. */
static int check_intensity(struct STA_Actor *supervisor, STA_OOP reason) {
    STA_SupervisorData *data = supervisor->sup_data;
    uint64_t current = now_ns();

    /* First restart in window — set window start. */
    if (data->restart_count == 0) {
        data->window_start_ns = current;
    }

    /* Check if window has elapsed — if so, reset. */
    uint64_t elapsed = current - data->window_start_ns;
    uint64_t window_ns = (uint64_t)data->max_seconds * 1000000000ULL;
    if (elapsed > window_ns) {
        data->restart_count = 0;
        data->window_start_ns = current;
    }

    /* Increment and check. */
    data->restart_count++;
    if (data->restart_count > data->max_restarts) {
        struct STA_VM *vm = supervisor->vm;

        if (vm && supervisor == vm->root_supervisor) {
            /* Root supervisor — do NOT terminate. Fire event, reset, continue.
             * The failing subtree is already terminated (child supervisor
             * tore itself down before escalating). */
            sta_vm_fire_event(vm, STA_EVT_ACTOR_CRASH,
                              "root supervisor intensity exceeded");
            /* Reset BOTH counters so next crash doesn't re-trigger. */
            data->restart_count = 0;
            data->window_start_ns = now_ns();
            return -1;  /* Skip the individual restart */
        }

        /* Non-root supervisor: terminate all children and escalate. */
        terminate_all_children(supervisor);

        STA_OOP escalation_reason = 0;
        if (vm) {
            escalation_reason = sta_symbol_intern(
                &vm->immutable_space, &vm->symbol_table,
                "restartIntensityExceeded", 24);
        }
        if (escalation_reason == 0) {
            escalation_reason = sta_spc_get(SPC_UNKNOWN_ERROR);
        }

        if (supervisor->supervisor) {
            escalate_to_parent(supervisor, escalation_reason);
        }

        /* Transition this supervisor to TERMINATED. */
        atomic_store_explicit(&supervisor->state, STA_ACTOR_TERMINATED,
                              memory_order_release);

        return -1;  /* Intensity exceeded */
    }

    return 0;  /* Restart allowed */
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

    /* Root supervisor: fire STA_EVT_ACTOR_CRASH for visibility. */
    if (supervisor->vm && supervisor == supervisor->vm->root_supervisor) {
        char msg[128];
        snprintf(msg, sizeof(msg), "child %u failed under root supervisor",
                 failed_id);
        sta_vm_fire_event(supervisor->vm, STA_EVT_ACTOR_CRASH, msg);
    }

    struct STA_Actor *old_actor = spec->current_actor;

    switch (spec->strategy) {
    case STA_RESTART_RESTART: {
        /* Check restart intensity before applying RESTART. */
        if (check_intensity(supervisor, args[1]) != 0) {
            /* Intensity exceeded — all children terminated, supervisor
             * transitioned to TERMINATED, escalation sent. */
            return 0;
        }

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
        escalate_to_parent(supervisor, args[1]);
        break;
    }
    }

    return 0;
}
