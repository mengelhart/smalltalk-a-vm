/* src/actor/mailbox_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: lock-free MPSC mailbox variants and cross-actor copy.
 * See docs/spikes/spike-002-mailbox.md and ADR 008.
 */
#include "mailbox_spike.h"
#include <stdlib.h>
#include <string.h>
#include <stddef.h>

/* ── Variant A: Vyukov intrusive linked-list MPSC ────────────────────────── */

void sta_list_init(STA_MpscList *q, uint32_t limit) {
    atomic_store_explicit(&q->stub.next, NULL, memory_order_relaxed);
    q->stub.msg = 0;
    atomic_store_explicit(&q->tail, &q->stub, memory_order_relaxed);
    q->head  = &q->stub;
    atomic_store_explicit(&q->count, 0u, memory_order_relaxed);
    q->limit = limit;
}

int sta_list_enqueue(STA_MpscList *q, STA_ListNode *node, STA_OOP msg) {
    /* Capacity check — drop-newest: claim a count slot or return FULL.
     * memory_order_relaxed is sufficient; the list's release/acquire on
     * the next pointer provides the necessary happens-before for msg.      */
    if (q->limit > 0) {
        uint32_t prev = atomic_fetch_add_explicit(&q->count, 1u,
                                                  memory_order_relaxed);
        if (prev >= q->limit) {
            atomic_fetch_sub_explicit(&q->count, 1u, memory_order_relaxed);
            return STA_ERR_MAILBOX_FULL;   /* node untouched — caller may retry */
        }
    }

    node->msg = msg;
    atomic_store_explicit(&node->next, NULL, memory_order_relaxed);

    /* Atomically swing tail to node, then link previous tail → node.
     * The transient "broken link" window is handled by the consumer
     * treating a NULL next as EMPTY and retrying next call.               */
    STA_ListNode *prev = atomic_exchange_explicit(&q->tail, node,
                                                  memory_order_acquire);
    atomic_store_explicit(&prev->next, node, memory_order_release);
    return STA_OK;
}

int sta_list_dequeue(STA_MpscList *q, STA_OOP *msg_out) {
    STA_ListNode *head = q->head;
    STA_ListNode *next = atomic_load_explicit(&head->next, memory_order_acquire);

    if (next == NULL) {
        return STA_ERR_MAILBOX_EMPTY;
    }

    *msg_out = next->msg;
    q->head  = next;   /* next becomes new sentinel; old head freed by caller */

    if (q->limit > 0) {
        atomic_fetch_sub_explicit(&q->count, 1u, memory_order_relaxed);
    }
    return STA_OK;
}

/* ── Variant B: bounded ring buffer MPSC ─────────────────────────────────── */

void sta_ring_init(STA_MpscRing *q) {
    atomic_store_explicit(&q->write_cursor, 0, memory_order_relaxed);
    q->read_cursor = 0;
    for (uint32_t i = 0; i < STA_RING_CAPACITY; i++) {
        /* sequence == i means "slot i is empty and ready to accept round 0". */
        atomic_store_explicit(&q->slots[i].sequence, i, memory_order_relaxed);
        q->slots[i].msg  = 0;
        q->slots[i]._pad = 0;
    }
}

int sta_ring_enqueue(STA_MpscRing *q, STA_OOP msg) {
    uint32_t pos;
    STA_RingSlot *slot;

    for (;;) {
        pos  = atomic_load_explicit(&q->write_cursor, memory_order_acquire);
        slot = &q->slots[pos & STA_RING_MASK];
        uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
        int32_t  diff = (int32_t)seq - (int32_t)pos;

        if (diff == 0) {
            /* Slot is ready for this position. Try to claim it.             */
            if (atomic_compare_exchange_weak_explicit(
                    &q->write_cursor, &pos, pos + 1,
                    memory_order_acquire, memory_order_acquire)) {
                break;   /* we own slot at pos */
            }
            /* Another producer beat us — retry. */
        } else if (diff < 0) {
            /* Slot is still occupied by a previous round — buffer is full.  */
            return STA_ERR_MAILBOX_FULL;
        }
        /* diff > 0: write_cursor lagged behind; reload and retry.          */
    }

    slot->msg = msg;
    /* Publish: sequence = pos + 1 tells the consumer this slot is ready.   */
    atomic_store_explicit(&slot->sequence, pos + 1, memory_order_release);
    return STA_OK;
}

int sta_ring_dequeue(STA_MpscRing *q, STA_OOP *msg_out) {
    uint32_t     pos  = q->read_cursor;   /* private — no atomic needed      */
    STA_RingSlot *slot = &q->slots[pos & STA_RING_MASK];
    uint32_t seq = atomic_load_explicit(&slot->sequence, memory_order_acquire);
    int32_t  diff = (int32_t)seq - (int32_t)(pos + 1);

    if (diff < 0) {
        return STA_ERR_MAILBOX_EMPTY;
    }

    *msg_out = slot->msg;
    /* Mark slot empty for next round: sequence = pos + CAPACITY.           */
    atomic_store_explicit(&slot->sequence,
                          pos + STA_RING_CAPACITY,
                          memory_order_release);
    q->read_cursor = pos + 1;
    return STA_OK;
}

/* ── Cross-actor deep copy ────────────────────────────────────────────────── */
/*
 * Spike-level implementation: malloc-backed transfer buffer.
 * Layout of buf: one or more (STA_ObjHeader + payload) blocks, packed
 * contiguously, each 16-byte aligned per ADR 007 sta_alloc_size().
 * The root object is always at buf offset 0.
 *
 * Immutable objects (STA_OBJ_IMMUTABLE set) are not copied — their original
 * OOP is stored directly in the copy's slot. SmallInts are passed through.
 *
 * Limitations of this spike implementation:
 * - No cycle detection. Object graphs with cycles will recurse infinitely.
 *   Production implementation needs a visited-set. This is spike code only.
 * - No forwarding map. Sub-objects referenced from multiple slots are copied
 *   multiple times. Production needs a sender-address → copy-address map.
 * - The transfer buffer is a single malloc slab pre-sized for the root object
 *   only; sub-object copies are separate malloc calls appended to a linked
 *   list and freed together. This is intentionally simple for the spike.
 */

/* Internal copy context — links together all sub-allocations. */
typedef struct CopyBlock {
    struct CopyBlock *next;
    /* Data follows immediately after this header at 16-byte alignment.     */
} CopyBlock;

/* Allocate size bytes from a fresh CopyBlock, record it in a list. */
static void *alloc_block(CopyBlock **list, size_t size) {
    /* Ensure at least 16-byte alignment for the embedded ObjHeader.        */
    size_t total = sizeof(CopyBlock) + size;
    /* Round up to 16-byte boundary.                                        */
    total = (total + 15u) & ~(size_t)15u;
    CopyBlock *blk = malloc(total);
    if (!blk) return NULL;
    blk->next = *list;
    *list = blk;
    return (char *)blk + sizeof(CopyBlock);
}

/* Forward declaration for recursion. */
static STA_OOP copy_oop(STA_OOP src, CopyBlock **list);

static STA_OOP copy_heap_object(STA_ObjHeader *src_hdr, CopyBlock **list) {
    uint32_t nslots = src_hdr->size;
    size_t   obj_size = 16u + (size_t)nslots * sizeof(STA_OOP);

    STA_ObjHeader *dst_hdr = alloc_block(list, obj_size);
    if (!dst_hdr) return 0;   /* allocation failure — propagate as 0 (nil) */

    /* Copy header fields. */
    dst_hdr->class_index = src_hdr->class_index;
    dst_hdr->size        = src_hdr->size;
    dst_hdr->gc_flags    = STA_GC_WHITE;  /* fresh object in transfer buffer */
    dst_hdr->obj_flags   = src_hdr->obj_flags;
    dst_hdr->reserved    = 0;

    /* Copy and recursively deep-copy each payload slot. */
    STA_OOP *src_slots = (STA_OOP *)((char *)src_hdr + 16u);
    STA_OOP *dst_slots = (STA_OOP *)((char *)dst_hdr + 16u);
    for (uint32_t i = 0; i < nslots; i++) {
        dst_slots[i] = copy_oop(src_slots[i], list);
    }

    return (STA_OOP)(uintptr_t)dst_hdr;
}

static STA_OOP copy_oop(STA_OOP src, CopyBlock **list) {
    /* Immediates (SmallInt) pass through unchanged. */
    if (STA_IS_SMALLINT(src)) return src;
    /* NULL OOP (uninitialized slot) passes through. */
    if (src == 0) return 0;

    STA_ObjHeader *hdr = (STA_ObjHeader *)(uintptr_t)src;

    /* Immutable objects are shared by pointer — no copy. */
    if (hdr->obj_flags & STA_OBJ_IMMUTABLE) return src;

    return copy_heap_object(hdr, list);
}

int sta_msg_copy_deep(STA_OOP src_oop, STA_MsgCopy *out) {
    out->buf  = NULL;
    out->size = 0;
    out->root = 0;

    /* Immediates need no allocation. */
    if (STA_IS_SMALLINT(src_oop) || src_oop == 0) {
        out->root = src_oop;
        return STA_OK;
    }

    STA_ObjHeader *src_hdr = (STA_ObjHeader *)(uintptr_t)src_oop;

    /* Immutable: share by pointer, no copy. */
    if (src_hdr->obj_flags & STA_OBJ_IMMUTABLE) {
        out->root = src_oop;
        return STA_OK;
    }

    CopyBlock *list = NULL;
    STA_OOP root_oop = copy_heap_object(src_hdr, &list);
    if (root_oop == 0 && list == NULL) {
        return STA_ERR_MAILBOX_FULL;   /* repurposed as alloc failure */
    }

    /* Store the head of the block list as buf so sta_msg_copy_free can
     * walk and free it. root is the OOP of the first block's data.        */
    out->buf  = list;
    out->size = 0;   /* spike doesn't track total — not needed for ADR 008 */
    out->root = root_oop;
    return STA_OK;
}

void sta_msg_copy_free(STA_MsgCopy *mc) {
    CopyBlock *blk = (CopyBlock *)mc->buf;
    while (blk) {
        CopyBlock *next = blk->next;
        free(blk);
        blk = next;
    }
    mc->buf  = NULL;
    mc->root = 0;
    mc->size = 0;
}
