/* src/actor/mailbox_spike.h
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: lock-free MPSC mailbox variants and cross-actor copy.
 * See docs/spikes/spike-002-mailbox.md and ADR 008.
 *
 * CHOSEN DESIGN: Variant A (Vyukov linked list) + atomic capacity counter.
 * Variant B (ring buffer) is retained below as reference — it was measured
 * but rejected because the 4 KB pre-allocated buffer blows the ~300-byte
 * per-actor density target. See ADR 008.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include "../vm/oop_spike.h"

/* ── Error codes ──────────────────────────────────────────────────────────── */
#define STA_OK                  0
#define STA_ERR_MAILBOX_FULL  (-1)
#define STA_ERR_MAILBOX_EMPTY (-2)

/* ── Variant A: Vyukov MPSC linked list + atomic capacity counter ─────────── */
/*
 * One node per message; allocated/freed by caller (spike uses malloc).
 * Bounded overflow is implemented via an _Atomic uint32_t count alongside
 * the list — no pre-allocated storage required. limit=0 means unbounded.
 *
 * Density: ~40 bytes for the mailbox struct itself (head, tail, stub, count,
 * limit). Node memory is only allocated when a message is in flight — nothing
 * is pre-allocated per actor. This preserves the ~300-byte density target.
 */

typedef struct STA_ListNode {
    _Atomic(struct STA_ListNode *) next;  /* 8 bytes — written by producers   */
    STA_OOP                        msg;   /* 8 bytes — the message OOP        */
} STA_ListNode;                           /* 16 bytes                          */

typedef struct {
    _Atomic(STA_ListNode *) tail;   /* 8 bytes  — producer CAS target         */
    STA_ListNode           *head;   /* 8 bytes  — private to consumer         */
    STA_ListNode            stub;   /* 16 bytes — permanent sentinel node     */
    _Atomic uint32_t        count;  /* 4 bytes  — current message depth       */
    uint32_t                limit;  /* 4 bytes  — capacity ceiling (0=unbnd)  */
} STA_MpscList;                     /* ~40 bytes                               */

/* Initialize. limit=0 means unbounded; limit>0 enables drop-newest overflow. */
void sta_list_init(STA_MpscList *q, uint32_t limit);

/* Enqueue msg. Safe to call from any thread concurrently.
 * node must remain valid until the consumer dequeues it.
 * Returns STA_OK on success, STA_ERR_MAILBOX_FULL if limit exceeded.
 * On FULL the node is untouched — caller may retry with the same node. */
int sta_list_enqueue(STA_MpscList *q, STA_ListNode *node, STA_OOP msg);

/* Dequeue into *msg_out. Must be called from the owner thread only.
 * Returns STA_OK on success, STA_ERR_MAILBOX_EMPTY if nothing available.
 * On success the dequeued node becomes the new sentinel — the caller may
 * free the previous head (returned implicitly via the node pointer chain). */
int sta_list_dequeue(STA_MpscList *q, STA_OOP *msg_out);

/* ── Variant B: bounded ring buffer (MEASURED, NOT CHOSEN) ───────────────── */
/*
 * Retained for reference. Rejected as default because sizeof(STA_MpscRing)
 * = 4112 bytes — pre-allocated per actor, inflating creation cost to ~4344
 * bytes vs. the ~300-byte target. See ADR 008 rationale.
 */

#define STA_RING_CAPACITY 256u
#define STA_RING_MASK     (STA_RING_CAPACITY - 1u)

typedef struct {
    _Atomic uint32_t sequence;
    STA_OOP          msg;
    uint32_t         _pad;
} STA_RingSlot;                  /* 16 bytes */

typedef struct {
    _Atomic uint32_t write_cursor;
    uint32_t         read_cursor;
    uint32_t         _pad[6];
    STA_RingSlot     slots[STA_RING_CAPACITY];
} STA_MpscRing;                  /* ~4112 bytes — too large for per-actor embed */

void sta_ring_init(STA_MpscRing *q);
int  sta_ring_enqueue(STA_MpscRing *q, STA_OOP msg);
int  sta_ring_dequeue(STA_MpscRing *q, STA_OOP *msg_out);

/* ── Cross-actor message copy ─────────────────────────────────────────────── */

typedef struct {
    void   *buf;
    size_t  size;
    STA_OOP root;
} STA_MsgCopy;

int  sta_msg_copy_deep(STA_OOP src_oop, STA_MsgCopy *out);
void sta_msg_copy_free(STA_MsgCopy *mc);
