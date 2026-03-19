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
#include "mailbox.h"
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

    /* Lifecycle state machine */
    uint32_t          state;

    /* Actor identity */
    uint32_t          actor_id;

    /* The actor's Smalltalk-level class (for future use) */
    STA_OOP           behavior_class;

    /* MPSC mailbox — Vyukov linked list, bounded, per ADR 008 */
    STA_Mailbox       mailbox;

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
