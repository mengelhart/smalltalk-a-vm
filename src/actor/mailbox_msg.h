/* src/actor/mailbox_msg.h
 * Message envelope for cross-actor messaging — Phase 2 Epic 3.
 * Placed in the mailbox by sta_mailbox_enqueue, dequeued by
 * sta_mailbox_dequeue. malloc'd per message, freed after dispatch.
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include "vm/oop.h"
#include <stdint.h>

/* ── STA_MailboxMsg ──────────────────────────────────────────────────── */

typedef struct STA_MailboxMsg {
    /* Selector — a Symbol OOP. Immutable, shared by pointer (never copied). */
    STA_OOP     selector;

    /* Arguments — pointer to copied argument array on target actor's heap.
     * May be NULL if arg_count == 0. */
    STA_OOP    *args;

    /* Number of arguments (0–255). */
    uint8_t     arg_count;

    /* Sender actor ID — for reply routing (used by Epic 7 ask: semantics). */
    uint32_t    sender_id;
} STA_MailboxMsg;

/* ── Allocation helpers ──────────────────────────────────────────────── */

/* Allocate a message envelope (malloc). Returns NULL on allocation failure. */
STA_MailboxMsg *sta_mailbox_msg_create(STA_OOP selector,
                                        STA_OOP *args,
                                        uint8_t arg_count,
                                        uint32_t sender_id);

/* Free a message envelope. Does NOT free the args array (which lives
 * on the target actor's heap). */
void sta_mailbox_msg_destroy(STA_MailboxMsg *msg);
