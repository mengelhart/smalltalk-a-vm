/* src/actor/actor.h
 * Production actor struct — Phase 2 Epic 2.
 * Per-actor heap, stack slab, handler chain, lifecycle state machine.
 *
 * Internal header — not part of the public API.
 * The public API declares STA_Actor as opaque (sta/vm.h).
 */
#pragma once

#include "vm/heap.h"
#include "vm/frame.h"
#include "vm/handler.h"
#include "vm/oop.h"
#include "gc/gc.h"
#include "mailbox.h"
#include <stdatomic.h>
#include <stdint.h>

/* Forward-declare STA_VM to avoid circular include. */
struct STA_VM;

/* ── Actor lifecycle states ──────────────────────────────────────────── */

#define STA_ACTOR_CREATED     0u
#define STA_ACTOR_READY       1u
#define STA_ACTOR_RUNNING     2u
#define STA_ACTOR_SUSPENDED   3u
#define STA_ACTOR_TERMINATED  4u

/* ── STA_Actor ───────────────────────────────────────────────────────── */

struct STA_Actor {
    /* Back-pointer to owning VM (for shared immutable space, class table, etc.) */
    struct STA_VM   *vm;

    /* Per-actor mutable heap — independent bump allocator */
    STA_Heap         heap;

    /* Per-actor activation frame stack */
    STA_StackSlab    slab;

    /* Per-actor exception handler chain (migrated from STA_VM in Story 3) */
    STA_HandlerEntry *handler_top;
    STA_OOP           signaled_exception;

    /* Lifecycle state machine (atomic for scheduler thread safety) */
    _Atomic uint32_t  state;

    /* Actor identity */
    uint32_t          actor_id;

    /* Intrusive run-queue link (scheduler only) */
    struct STA_Actor *next_runnable;

    /* The actor's Smalltalk-level behavior class and object.
     * Messages dispatched to this actor are looked up in behavior_class
     * and invoked with behavior_obj as the receiver. */
    STA_OOP           behavior_class;
    STA_OOP           behavior_obj;

    /* MPSC mailbox — Vyukov linked list, bounded, per ADR 008 */
    STA_Mailbox       mailbox;

    /* Preemption: saved frame pointer when actor is suspended mid-execution.
     * NULL means the actor is not mid-execution (start fresh from mailbox).
     * Non-NULL means the actor was preempted and should resume here. */
    struct STA_Frame *saved_frame;

    /* Per-actor GC statistics (Story 6). 24 bytes inline. */
    STA_GCStats       gc_stats;

    /* Supervisor linkage — NULL placeholder, wired in Epic 6 */
    void             *supervisor;
};

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/* Create a new actor with the given heap and stack sizes.
 * The actor is allocated via malloc. heap and slab are initialized in-place.
 * Returns NULL on allocation failure.
 * Initial state: STA_ACTOR_CREATED. */
struct STA_Actor *sta_actor_create(struct STA_VM *vm,
                                   size_t heap_size,
                                   size_t stack_size);

/* Destroy an actor — deinitialize heap and slab, free the struct. */
void sta_actor_destroy(struct STA_Actor *actor);

/* ── Messaging (Epic 3) ─────────────────────────────────────────────── */

/* Send an asynchronous message from sender to target.
 * - selector: a Symbol OOP (immutable, shared by pointer — never copied)
 * - args: array of argument OOPs on the sender's heap (may be NULL if nargs==0)
 * - nargs: number of arguments (0–255)
 *
 * Each argument is deep-copied from sender's heap to target's heap.
 * A message envelope is created and enqueued in target's mailbox.
 * Fire-and-forget: returns immediately, no return value from target.
 *
 * Returns 0 on success.
 * Returns STA_ERR_MAILBOX_FULL if the target's mailbox is at capacity.
 * Returns negative error on allocation failure. */
int sta_actor_send_msg(struct STA_Actor *sender,
                       struct STA_Actor *target,
                       STA_OOP selector,
                       STA_OOP *args, uint8_t nargs);

/* ── Message dispatch (Epic 3 Story 5) ───────────────────────────────── */

/* Return codes for sta_actor_process_one. */
#define STA_ACTOR_MSG_PROCESSED  1
#define STA_ACTOR_MSG_EMPTY      0
#define STA_ACTOR_MSG_ERROR     (-1)
#define STA_ACTOR_MSG_PREEMPTED  2

/* Process one message from the actor's mailbox.
 *
 * 1. If actor->saved_frame is set, resume preempted execution.
 * 2. Otherwise, dequeue one message from the mailbox.
 * 3. Look up the selector in the actor's behavior_class hierarchy.
 * 4. Execute the method with the actor's behavior_obj as receiver
 *    and the message's copied arguments.
 * 5. Free the message envelope.
 *
 * The actor must have behavior_class and behavior_obj set.
 * Execution uses the actor's own heap and stack slab.
 *
 * Returns:
 *   STA_ACTOR_MSG_PROCESSED (1) — message completed
 *   STA_ACTOR_MSG_EMPTY     (0) — mailbox was empty
 *   STA_ACTOR_MSG_ERROR    (-1) — method not found, etc.
 *   STA_ACTOR_MSG_PREEMPTED (2) — actor preempted mid-execution
 *
 * When use_preemption is true, the interpreter may preempt the actor
 * after STA_REDUCTION_QUOTA reductions, saving state on actor->saved_frame.
 * When false, the method runs to completion (backward compat). */
int sta_actor_process_one(struct STA_VM *vm, struct STA_Actor *actor);

/* Preemption-aware variant for the scheduler. */
int sta_actor_process_one_preemptible(struct STA_VM *vm, struct STA_Actor *actor);
