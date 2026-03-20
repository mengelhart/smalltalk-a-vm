/* src/actor/supervisor.c
 * Supervisor operations — Phase 2 Epic 6.
 * See supervisor.h for documentation.
 */
#include "supervisor.h"
#include "actor.h"
#include <stdlib.h>

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
