/* src/actor/future.c
 * Future lifecycle — Phase 2 Epic 7A + 7B waiter wake.
 * See future.h for documentation.
 *
 * Memory ordering protocol (intermediate-state pattern):
 *   Writer (resolve/fail):
 *     1. CAS state PENDING → RESOLVING/FAILING (acq_rel). Only winner proceeds.
 *     2. Winner stores result_buf, result_count, transfer_heap (plain writes).
 *     3. atomic_store state → RESOLVED/FAILED (release). Publishes all prior stores.
 *     4. (Epic 7B) Check waiter_actor_id. If non-zero, CAS waiter
 *        SUSPENDED → READY and push to scheduler.
 *   Reader (Epic 7B wait primitive):
 *     1. acquire-load state. Only acts on RESOLVED or FAILED.
 *     2. The acquire synchronizes-with the release store in step 3,
 *        guaranteeing all field stores in step 2 are visible.
 *
 * TSan-clean: only the CAS winner writes to non-atomic fields (steps 2–3).
 *   The loser never touches them. Readers only access fields after seeing
 *   the final RESOLVED/FAILED state via acquire-load.
 */
#include "future.h"
#include "actor.h"
#include "registry.h"
#include "scheduler/scheduler.h"
#include "vm/vm_state.h"
#include "vm/heap.h"
#include <stdlib.h>

/* ── Waiter wake (Epic 7B) ─────────────────────────────────────────────── */

/* After a future transitions to a terminal state, check if an actor is
 * suspended waiting on it. If so, CAS SUSPENDED → READY and push to
 * the scheduler so the waiter resumes execution. */
static void wake_waiter(STA_Future *f) {
    uint32_t waiter = atomic_load_explicit(&f->waiter_actor_id,
                                            memory_order_acquire);
    if (waiter == 0) return;
    if (!f->vm) return;

    struct STA_Actor *a = sta_registry_lookup(f->vm->registry, waiter);
    if (!a) return;

    uint32_t expected = STA_ACTOR_SUSPENDED;
    if (atomic_compare_exchange_strong_explicit(
            &a->state, &expected, STA_ACTOR_READY,
            memory_order_acq_rel, memory_order_relaxed)) {
        /* Push to scheduler. The seq_cst fence in sta_actor_send_msg
         * isn't needed here because we're the only thread transitioning
         * this particular actor from SUSPENDED → READY. */
        STA_Scheduler *sched = f->vm->scheduler;
        if (sched && atomic_load_explicit(&sched->running,
                                            memory_order_acquire)) {
            sta_scheduler_enqueue(sched, a);
        }
    }
    sta_actor_release(a);
}

STA_Future *sta_future_retain(STA_Future *f) {
    if (!f) return NULL;
    atomic_fetch_add_explicit(&f->refcount, 1, memory_order_relaxed);
    return f;
}

void sta_future_release(STA_Future *f) {
    if (!f) return;
    uint32_t prev = atomic_fetch_sub_explicit(&f->refcount, 1,
                                               memory_order_acq_rel);
    if (prev == 1) {
        /* refcount reached 0 — free resources. */
        if (f->result_buf)
            free(f->result_buf);
        if (f->transfer_heap) {
            sta_heap_deinit(f->transfer_heap);
            free(f->transfer_heap);
        }
        free(f);
    }
}

bool sta_future_resolve(STA_Future *f, STA_OOP *buf, uint32_t count,
                        STA_Heap *transfer_heap) {
    if (!f) {
        free(buf);
        if (transfer_heap) {
            sta_heap_deinit(transfer_heap);
            free(transfer_heap);
        }
        return false;
    }

    /* Step 1: CAS PENDING → RESOLVING. Only the winner proceeds. */
    uint32_t expected = STA_FUTURE_PENDING;
    bool won = atomic_compare_exchange_strong_explicit(
        &f->state, &expected, STA_FUTURE_RESOLVING,
        memory_order_acq_rel, memory_order_relaxed);

    if (won) {
        /* Step 2: Store result fields (plain writes, exclusive access). */
        f->result_buf = buf;
        f->result_count = count;
        f->transfer_heap = transfer_heap;

        /* Step 3: Publish — release-store makes all field stores visible
         * to any thread that acquire-loads state and sees RESOLVED. */
        atomic_store_explicit(&f->state, STA_FUTURE_RESOLVED,
                              memory_order_release);

        /* Step 4 (Epic 7B): Wake any actor suspended on this future. */
        wake_waiter(f);
    } else {
        /* CAS lost — someone else already claimed the future. */
        free(buf);
        if (transfer_heap) {
            sta_heap_deinit(transfer_heap);
            free(transfer_heap);
        }
    }
    return won;
}

bool sta_future_fail(STA_Future *f, STA_OOP *buf, uint32_t count) {
    if (!f) {
        free(buf);
        return false;
    }

    /* Step 1: CAS PENDING → FAILING. */
    uint32_t expected = STA_FUTURE_PENDING;
    bool won = atomic_compare_exchange_strong_explicit(
        &f->state, &expected, STA_FUTURE_FAILING,
        memory_order_acq_rel, memory_order_relaxed);

    if (won) {
        /* Step 2: Store result fields. */
        f->result_buf = buf;
        f->result_count = count;
        /* transfer_heap is always NULL for failures. */

        /* Step 3: Publish. */
        atomic_store_explicit(&f->state, STA_FUTURE_FAILED,
                              memory_order_release);

        /* Step 4 (Epic 7B): Wake any actor suspended on this future. */
        wake_waiter(f);
    } else {
        free(buf);
    }
    return won;
}
