/* src/scheduler/deque.c
 * Chase-Lev fixed-capacity work-stealing deque — Phase 2 Epic 4 Story 5.
 * Adapted from Spike 003 (Variant A, TSan clean).
 * See ADR 009 for memory ordering audit.
 */
#include "deque.h"

void sta_deque_init(STA_WorkDeque *dq) {
    atomic_store_explicit(&dq->top,    0u, memory_order_relaxed);
    atomic_store_explicit(&dq->bottom, 0u, memory_order_relaxed);
    for (uint32_t i = 0; i < STA_DEQUE_CAPACITY; i++)
        atomic_store_explicit(&dq->buf[i], NULL, memory_order_relaxed);
}

int sta_deque_push(STA_WorkDeque *dq, struct STA_Actor *a) {
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&dq->top,    memory_order_acquire);
    if (b - t >= STA_DEQUE_CAPACITY) return -1;  /* full */
    atomic_store_explicit(&dq->buf[b & STA_DEQUE_MASK], a,
                          memory_order_relaxed);
    atomic_store_explicit(&dq->bottom, b + 1u, memory_order_release);
    return 0;
}

struct STA_Actor *sta_deque_pop(STA_WorkDeque *dq) {
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    if (b == 0) return NULL;
    b -= 1u;
    atomic_store_explicit(&dq->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);
    uint32_t t = atomic_load_explicit(&dq->top, memory_order_relaxed);

    if ((int32_t)(t - b) > 0) {
        atomic_store_explicit(&dq->bottom, b + 1u, memory_order_relaxed);
        return NULL;
    }

    struct STA_Actor *a =
        atomic_load_explicit(&dq->buf[b & STA_DEQUE_MASK],
                             memory_order_relaxed);

    if (t == b) {
        uint32_t expected = t;
        bool won = atomic_compare_exchange_strong_explicit(
            &dq->top, &expected, t + 1u,
            memory_order_seq_cst, memory_order_relaxed);
        atomic_store_explicit(&dq->bottom, t + 1u, memory_order_relaxed);
        return won ? a : NULL;
    }
    return a;
}

struct STA_Actor *sta_deque_steal(STA_WorkDeque *dq) {
    uint32_t t = atomic_load_explicit(&dq->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_acquire);
    if ((int32_t)(b - t) <= 0) return NULL;

    struct STA_Actor *a =
        atomic_load_explicit(&dq->buf[t & STA_DEQUE_MASK],
                             memory_order_relaxed);

    uint32_t expected = t;
    if (!atomic_compare_exchange_strong_explicit(
            &dq->top, &expected, t + 1u,
            memory_order_seq_cst, memory_order_relaxed)) {
        return NULL;
    }
    return a;
}

int sta_deque_size(const STA_WorkDeque *dq) {
    uint32_t b = atomic_load_explicit(&dq->bottom, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&dq->top,    memory_order_relaxed);
    return (int32_t)(b - t);
}
