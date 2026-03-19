/* src/actor/mailbox.h
 * Production MPSC mailbox — Phase 2 Epic 3.
 * Lock-free Vyukov linked list with atomic capacity counter.
 * Bounded, default limit 256, drop-newest overflow.
 * See ADR 008.
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/* ── Error codes (match public STA_ERR_* pattern) ───────────────────── */

#define STA_ERR_MAILBOX_FULL  (-10)
#define STA_ERR_MAILBOX_EMPTY (-11)

/* ── Default mailbox capacity ────────────────────────────────────────── */

#define STA_MAILBOX_DEFAULT_CAPACITY 256u

/* ── Forward declarations ────────────────────────────────────────────── */

/* Message envelope — defined in mailbox_msg.h. */
typedef struct STA_MailboxMsg STA_MailboxMsg;

/* Internal linked-list node — defined in mailbox.c.
 * Not visible to callers; mailbox API accepts/returns STA_MailboxMsg*. */
typedef struct STA_MbNode STA_MbNode;

/* ── STA_Mailbox ─────────────────────────────────────────────────────── */

/*
 * Vyukov MPSC linked list + _Atomic uint32_t capacity counter.
 * See ADR 008 for the full design rationale.
 *
 * Producer (enqueue): any thread, lock-free.
 * Consumer (dequeue): owner thread only (single-consumer).
 */
typedef struct STA_Mailbox {
    _Atomic(STA_MbNode *)  tail;      /* 8B — producer CAS target     */
    STA_MbNode            *head;      /* 8B — private to consumer     */
    STA_MbNode            *stub;      /* 8B — permanent sentinel node */
    _Atomic uint32_t       count;     /* 4B — current message depth   */
    uint32_t               capacity;  /* 4B — ceiling (0=unbounded)   */
} STA_Mailbox;                         /* 32 bytes                     */

/* ── Lifecycle ───────────────────────────────────────────────────────── */

/* Initialize the mailbox with the given capacity (0 = unbounded).
 * Allocates the stub sentinel node. Returns 0 on success, -1 on failure. */
int sta_mailbox_init(STA_Mailbox *mb, uint32_t capacity);

/* Destroy the mailbox. Frees the stub sentinel and any remaining
 * enqueued messages. Does NOT free the STA_Mailbox struct itself. */
void sta_mailbox_destroy(STA_Mailbox *mb);

/* ── Operations ──────────────────────────────────────────────────────── */

/* Enqueue a message. Safe to call from any thread concurrently.
 * msg must be heap-allocated (malloc); ownership transfers to the mailbox.
 * Returns 0 on success, STA_ERR_MAILBOX_FULL if at capacity.
 * On FULL the msg is NOT freed — caller retains ownership. */
int sta_mailbox_enqueue(STA_Mailbox *mb, STA_MailboxMsg *msg);

/* Dequeue one message. Must be called from the owner thread only.
 * Returns the message on success, or NULL if the mailbox is empty.
 * Caller takes ownership of the returned STA_MailboxMsg and must
 * call sta_mailbox_msg_destroy when done. */
STA_MailboxMsg *sta_mailbox_dequeue(STA_Mailbox *mb);

/* ── Queries ─────────────────────────────────────────────────────────── */

/* Current number of messages in the mailbox (approximate — may lag
 * slightly under concurrent enqueue). */
uint32_t sta_mailbox_count(const STA_Mailbox *mb);

/* True if the mailbox is empty (count == 0). Same approximation caveat. */
bool sta_mailbox_is_empty(const STA_Mailbox *mb);
