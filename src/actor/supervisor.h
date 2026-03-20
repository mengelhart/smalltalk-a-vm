/* src/actor/supervisor.h
 * Supervisor data structures and operations — Phase 2 Epic 6.
 * A supervisor IS an actor with additional STA_SupervisorData.
 * See Epic 6 design notes for rationale.
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include "vm/oop.h"
#include <sta/vm.h>    /* STA_RestartStrategy, STA_OOP, struct STA_VM */
#include <stdint.h>

/* Forward declarations. */
struct STA_Actor;

/* ── Child specification ──────────────────────────────────────────────── */

/* One per child slot in a supervisor. Linked list. */
typedef struct STA_ChildSpec {
    STA_OOP              behavior_class;   /* Class to instantiate on (re)start */
    STA_RestartStrategy  strategy;         /* What to do on failure */
    struct STA_Actor    *current_actor;    /* Currently running actor (or NULL) */
    struct STA_ChildSpec *next;            /* Linked list of children */
} STA_ChildSpec;

/* ── Supervisor-specific data ─────────────────────────────────────────── */

/* Allocated only for actors that ARE supervisors. */
typedef struct STA_SupervisorData {
    STA_ChildSpec *children;        /* Linked list of child specs */
    uint32_t       child_count;
    uint32_t       restart_count;   /* Restarts within current window (Part 2) */
    uint64_t       window_start_ns; /* Timestamp of first restart (Part 2) */
    uint32_t       max_restarts;    /* Default 3 (Part 2) */
    uint32_t       max_seconds;     /* Default 5 (Part 2) */
} STA_SupervisorData;

/* ── Lifecycle ────────────────────────────────────────────────────────── */

/* Initialize an actor as a supervisor.
 * Allocates and attaches STA_SupervisorData.
 * max_restarts/max_seconds are stored for Part 2 intensity limiting.
 * Returns 0 on success, -1 on allocation failure. */
int sta_supervisor_init(struct STA_Actor *actor,
                        uint32_t max_restarts,
                        uint32_t max_seconds);

/* Add a child to a supervisor.
 * Spawns a new actor via sta_actor_create, sets its supervisor pointer,
 * adds a child spec to the supervisor's list.
 * Returns the new child actor, or NULL on failure. */
struct STA_Actor *sta_supervisor_add_child(struct STA_Actor *supervisor,
                                            STA_OOP behavior_class,
                                            STA_RestartStrategy strategy);

/* Destroy supervisor data — frees child specs and the data struct.
 * Does NOT destroy child actors (caller must handle that). */
void sta_supervisor_data_destroy(STA_SupervisorData *data);

/* ── Failure handling ─────────────────────────────────────────────────── */

/* Handle a childFailed:reason: notification.
 * Looks up the child spec by actor_id, applies the restart strategy:
 *   RESTART:  destroy old actor, create new one, update child spec
 *   STOP:     destroy old actor, set current_actor = NULL
 *   ESCALATE: destroy old actor, forward failure to grandparent
 *
 * args[0] = failed actor_id (SmallInt OOP)
 * args[1] = error reason (Symbol OOP)
 *
 * Returns 0 on success, -1 on error. */
int sta_supervisor_handle_failure(struct STA_Actor *supervisor,
                                   STA_OOP *args, uint32_t arg_count);
