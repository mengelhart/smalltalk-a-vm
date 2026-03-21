/* src/actor/mailbox.c
 * Production MPSC mailbox — Phase 2 Epic 3.
 * Vyukov lock-free linked list with atomic capacity counter.
 * See mailbox.h and ADR 008.
 */
#include "mailbox.h"
#include "mailbox_msg.h"
#include <stdlib.h>
#include <string.h>

/* ── Internal node ───────────────────────────────────────────────────── */

/* The Vyukov protocol requires the dequeued node to become the new
 * sentinel. This means the node cannot be freed until the NEXT dequeue.
 * To keep STA_MailboxMsg independent and freely destroyable, we use a
 * separate internal node that wraps the message pointer. */

struct STA_MbNode {
    _Atomic(struct STA_MbNode *) next;   /* 8B */
    STA_MailboxMsg               *msg;   /* 8B — NULL for the stub sentinel */
};

/* ── Lifecycle ───────────────────────────────────────────────────────── */

int sta_mailbox_init(STA_Mailbox *mb, uint32_t capacity) {
    STA_MbNode *stub = calloc(1, sizeof(STA_MbNode));
    if (!stub) return -1;

    atomic_store_explicit(&stub->next, NULL, memory_order_relaxed);
    stub->msg = NULL;

    mb->stub = stub;
    atomic_store_explicit(&mb->tail, stub, memory_order_relaxed);
    mb->head = stub;
    atomic_store_explicit(&mb->count, 0u, memory_order_relaxed);
    mb->capacity = capacity;

    return 0;
}

void sta_mailbox_destroy(STA_Mailbox *mb) {
    if (!mb) return;

    /* Drain any remaining messages. */
    STA_MailboxMsg *msg;
    while ((msg = sta_mailbox_dequeue(mb)) != NULL) {
        sta_mailbox_msg_destroy(msg);
    }

    /* Free the stub sentinel (which may have been advanced). */
    /* head == stub at this point (or head is the last sentinel). */
    free(mb->head);
    mb->head = NULL;
    mb->stub = NULL;
}

/* ── Enqueue (multi-producer safe) ───────────────────────────────────── */

int sta_mailbox_enqueue(STA_Mailbox *mb, STA_MailboxMsg *msg) {
    /* Allocate a node to wrap the message. */
    STA_MbNode *node = malloc(sizeof(STA_MbNode));
    if (!node) return -1;

    /* Capacity check — drop-newest overflow policy.
     * The count increment uses release so that the scheduler's re-check
     * (acquire load on count) sees enqueued messages.  Together with the
     * seq_cst fences in sta_actor_send_msg and the scheduler dispatch
     * loop, this closes the store-buffer race in GitHub #319. */
    if (mb->capacity > 0) {
        uint32_t prev = atomic_fetch_add_explicit(&mb->count, 1u,
                                                   memory_order_release);
        if (prev >= mb->capacity) {
            atomic_fetch_sub_explicit(&mb->count, 1u, memory_order_relaxed);
            free(node);
            return STA_ERR_MAILBOX_FULL;
        }
    }

    node->msg = msg;
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);

    /* Vyukov protocol: atomically swing tail to node, then link prev→node. */
    STA_MbNode *prev = atomic_exchange_explicit(&mb->tail, node,
                                                 memory_order_acquire);
    atomic_store_explicit(&prev->next, node, memory_order_release);

    return 0;
}

/* ── Dequeue (single-consumer only) ──────────────────────────────────── */

STA_MailboxMsg *sta_mailbox_dequeue(STA_Mailbox *mb) {
    STA_MbNode *head = mb->head;
    STA_MbNode *next = atomic_load_explicit(&head->next, memory_order_acquire);

    if (next == NULL) return NULL;

    /* Extract the message from 'next'. */
    STA_MailboxMsg *msg = next->msg;
    next->msg = NULL;  /* Sentinel doesn't hold a message. */

    /* Advance: 'next' becomes the new sentinel. */
    mb->head = next;

    if (mb->capacity > 0) {
        atomic_fetch_sub_explicit(&mb->count, 1u, memory_order_relaxed);
    }

    /* Free the old sentinel node. */
    free(head);

    return msg;
}

/* ── Walk (Epic 7B Story 5) ──────────────────────────────────────────── */

void sta_mailbox_walk(STA_Mailbox *mb,
                      void (*visitor)(STA_MailboxMsg *msg, void *ctx),
                      void *ctx) {
    if (!mb || !visitor) return;

    /* Walk from head->next (first real message) through the linked list.
     * head is the sentinel — its msg is always NULL.
     * We follow the same path as dequeue but don't modify anything. */
    STA_MbNode *node = atomic_load_explicit(&mb->head->next,
                                              memory_order_acquire);
    while (node != NULL) {
        if (node->msg) {
            visitor(node->msg, ctx);
        }
        node = atomic_load_explicit(&node->next, memory_order_acquire);
    }
}

/* ── Queries ─────────────────────────────────────────────────────────── */

uint32_t sta_mailbox_count(const STA_Mailbox *mb) {
    return atomic_load_explicit(
        (_Atomic uint32_t *)&mb->count,
        memory_order_acquire);
}

bool sta_mailbox_is_empty(const STA_Mailbox *mb) {
    return sta_mailbox_count(mb) == 0;
}
