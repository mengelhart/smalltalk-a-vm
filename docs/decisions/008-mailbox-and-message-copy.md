# ADR 008 — Mailbox Design and Cross-Actor Message Copy

**Status:** Accepted
**Date:** 2026-03-10
**Spike:** `docs/spikes/spike-002-mailbox.md`
**Depends on:** ADR 007 (object header layout)

---

## Decision

The actor mailbox uses a **Vyukov MPSC linked list with an atomic capacity
counter** as the default. This is Variant A from the spike, extended with
a single `_Atomic uint32_t count` field that enforces the §9.4 bounded-
mailbox requirement without pre-allocating any buffer storage. The overflow
policy is **drop-newest**: enqueue returns `STA_ERR_MAILBOX_FULL` when
`count >= limit`; the sender decides what to do.

Cross-actor message sending uses **deep copy with `STA_OBJ_IMMUTABLE`
sharing**, allocated into a malloc-backed transfer buffer by the runtime
at send time (spike approximation; permanent implementation uses a pool
allocator).

A bounded ring buffer (Variant B) was measured and **rejected** as the
default. See rationale below.

---

## Measured results (arm64 M4 Max, Apple clang 17, `-O2`)

### Queue latency — chosen design (Variant A, bounded, limit=256)

| Configuration | Median | p99 |
|---|---|---|
| SPSC enqueue | 84 ns | 250 ns |
| 4P1C enqueue (4 producers → 1 consumer) | 458 ns | 1250 ns |

### Queue latency — Variant B ring buffer (reference, not chosen)

| Configuration | Median | p99 |
|---|---|---|
| SPSC enqueue | 125 ns | 167 ns |
| 4P1C enqueue | 334 ns | 4750 ns |

The list is faster at median under SPSC (84 vs 125 ns). The ring buffer has
a tighter p99 under SPSC (167 vs 250 ns) and lower 4P1C median (334 vs
458 ns) due to pre-allocated slots avoiding malloc. Under a permanent pool
allocator the list's SPSC median will drop further and its p99 will tighten;
the ring buffer advantage is entirely a malloc-vs-pre-allocated comparison,
not an intrinsic property of the data structure.

### TSan stress test

Variant A (bounded, limit=256): 4 producers × 250k messages = 1M total.
TSan clean. Completed in **0.60s** (vs 1.19s for the ring buffer variant).
No data races reported.

### Cross-actor copy cost

| Object | Median | p99 |
|---|---|---|
| 5-element Array (56 bytes; one mutable sub-object) | 41 ns | 42 ns |

---

## Queue variant: Variant A + capacity counter

### Struct layout

```c
typedef struct {
    _Atomic(STA_ListNode *) tail;   /* 8 bytes  — producer CAS target    */
    STA_ListNode           *head;   /* 8 bytes  — private to consumer    */
    STA_ListNode            stub;   /* 16 bytes — permanent sentinel     */
    _Atomic uint32_t        count;  /* 4 bytes  — current depth          */
    uint32_t                limit;  /* 4 bytes  — ceiling (0=unbounded)  */
} STA_MpscList;                     /* ~40 bytes                          */
```

**Density:** 40 bytes per mailbox struct. Node memory (16 bytes each) is
only allocated when a message is in flight — nothing pre-allocated at actor
creation. This preserves the ~300-byte per-actor density target.

### Why not Variant B (ring buffer)?

The initial draft of this ADR chose the ring buffer. That was revised after
review identified three errors in the reasoning:

1. **`sizeof(STA_MpscRing)` = 4 112 bytes.** Embedding it in `STA_Actor`
   inflates per-actor creation cost from ~224 bytes to ~4 344 bytes — a
   14× increase. The ~300-byte target is a hard architectural constraint
   (CLAUDE.md, ADR 007). The draft ADR waved this through as "justified and
   expected"; that is not justified.

2. **The latency comparison was unfair.** The measured advantage of the ring
   buffer (167 ns p99 vs 250 ns for the list at SPSC) is entirely the
   difference between pre-allocated slots and malloc. With a pool allocator,
   list node allocation approaches a bump-pointer cost of ~5–10 ns; the
   measured 4P1C median of 458 ns is expected to drop below the ring
   buffer's 334 ns. This is a falsifiable prediction: the Phase 1 allocator
   spike or Spike 003 should verify it, and if the prediction is wrong the
   ring buffer should be reconsidered. The ring buffer's pre-allocation just
   moves that cost to actor creation time.

3. **The §9.4 bounded requirement does not require pre-allocated storage.**
   A capacity ceiling enforced by an atomic counter achieves identical
   overflow semantics at the cost of one extra `fetch_add` per enqueue and
   one `fetch_sub` per dequeue. This is the correct level of mechanism.

BEAM/ERTS uses an unbounded linked-list queue for the same reasons: node
lifetime is clean (enqueue allocates, dequeue frees), pool allocation makes
per-message cost negligible at scale, and backpressure is observable through
the count without pre-committing memory. The ring buffer is a better fit for
fixed-rate, fixed-message-size scenarios (network packet queues, audio
buffers) — not a general-purpose actor mailbox.

The ring buffer code is retained in `src/actor/mailbox_spike.h/c` for
reference; its latency numbers are recorded above for completeness.

### Correctness properties

**Enqueue protocol:**
1. If `limit > 0`: `fetch_add(count)`. If `prev >= limit`, `fetch_sub` and
   return `STA_ERR_MAILBOX_FULL` — node untouched, caller may retry.
2. Store `msg` into node, store `NULL` into `node->next` (relaxed).
3. `atomic_exchange(tail, node, acquire)` → `prev`.
4. `store(prev->next, node, release)` — makes node visible to consumer.

**Dequeue protocol:**
1. Load `head->next` with `acquire`.
2. If NULL: return `STA_ERR_MAILBOX_EMPTY`.
3. Read `next->msg`; advance `head = next` (consumer-private).
4. If `limit > 0`: `fetch_sub(count, relaxed)`.

`memory_order_relaxed` is sufficient for the count operations: the
acquire/release on the `next` pointer provides the necessary happens-before
for message visibility. No `memory_order_seq_cst` is used anywhere.

The transient broken-link window (between steps 3 and 4 of enqueue) means
the consumer may see NULL and return EMPTY even though a message is in
flight. This is correct behavior — the consumer retries on the next
scheduling opportunity. The count already reflects the reservation so no
message is lost.

---

## Overflow policy: drop-newest

When `sta_list_enqueue` returns `STA_ERR_MAILBOX_FULL`, the new message
is not enqueued. The caller handles the failure. Three policies were
evaluated:

| Policy | Consumer data loss | Sender signal | Complexity |
|---|---|---|---|
| Drop-oldest | Yes | None | Requires consumer cooperation |
| **Drop-newest (chosen)** | No | `STA_ERR_MAILBOX_FULL` | None — in enqueue only |
| Return-failure-to-sender | No | Future resolved with error | Requires scheduler |

Drop-newest is chosen: the consumer never sees a gap; the sender has an
explicit observable signal. Consistent with §9.4: "overload should be
visible, not hidden."

### Default limit: 256

At 458 ns median enqueue latency under 4P1C load, a sender can enqueue
approximately 2 million messages/second. A 256-slot limit provides ~128 µs
of headroom before overflow at that rate. A typical scheduling quantum is
~1 ms, giving ~8× headroom. For a REST-endpoint workload (~50k req/s,
~250k actor sends/s), the queue is almost never at risk of overflow at the
default limit.

ADR 008 records this value so it is not arbitrary when revisited: if the
scheduler quantum changes or if measured actor message rates consistently
exceed 2M/s sustained, the limit should be re-evaluated. The limit is a
runtime-tunable parameter; 256 is the default.

---

## Message copy model

### Who allocates: runtime at send time

The runtime performs the copy synchronously during `send:`, in the sender's
thread. The sender passes an OOP; the runtime allocates a transfer buffer,
copies the graph into it, and enqueues the transfer-buffer OOP. The consumer
frees the transfer buffer after delivery.

### Location: transfer buffer

The copy lives in a malloc-backed transfer buffer for this spike. The
permanent implementation uses a runtime slab allocator, but the ownership
model — transfer buffer owned by the message node, freed by the consumer —
is locked by this ADR.

### Depth: deep copy with `STA_OBJ_IMMUTABLE` sharing

- **`STA_OBJ_IMMUTABLE` set:** shared by pointer. No copy.
- **`STA_OBJ_IMMUTABLE` clear:** recursively copied into transfer buffer.
- **SmallInt or null OOP:** passed through unchanged.

Validated by test: 5-element Array with SmallInts (pass-through), mutable
String (copied, new address, payload preserved), immutable Symbol (same OOP).

**Spike limitation:** no cycle detection, no forwarding map for shared
sub-objects. Production implementation requires a sender-address →
copy-address hash map. Phase 1 concern.

### Baseline copy cost

**41 ns median, 42 ns p99** for the reference case. Dominated by two malloc
calls (root array + mutable sub-object). Pool allocation will reduce this.

---

## `STA_Mailbox` stub revision

The stub in `src/vm/actor_spike.h` (`STA_Mailbox` with `head`/`tail`
pointers) is superseded. The permanent struct embeds `STA_MpscList` directly:

```c
typedef struct STA_Actor {
    /* ... other fields ... */
    STA_MpscList mailbox;   /* ~40 bytes — replaces STA_Mailbox stub */
    /* ... */
} STA_Actor;
```

### Updated actor density

| Component | Bytes |
|---|---|
| `STA_Actor` runtime struct (with `STA_MpscList` embedded) | ~120 |
| Initial nursery slab (minimum) | 128 |
| Actor identity object (0-slot header) | 16 |
| **Total per-actor creation cost** | **~264 bytes** |

**Target: ~300 bytes. Revised estimate: ~264 bytes. Within budget.**

Node memory (16 bytes per in-flight message) is not a creation cost — it is
allocated on first send and freed on dequeue. The density target is preserved.

---

## Open questions (deferred)

1. **Scheduler notification.** How the scheduler learns a previously-empty
   mailbox is non-empty. Scheduler spike (003).

2. **Pool allocator for list nodes.** The spike uses malloc; production
   uses a per-actor slab. Node reclamation is trivial (freed by consumer
   after dequeue). Phase 1.

3. **`ask:` future on `STA_ERR_MAILBOX_FULL`.** Future resolution path.
   Phase 2.

4. **Deep-copy visited set.** Hash map for DAGs and cycles. Phase 1, before
   any mutable cyclic structures exist.

5. **Transfer buffer allocator.** Replace malloc with runtime slab. Phase 1.

---

## Consequences

- `STA_MpscList` (~40 bytes) is the default mailbox, embedded in `STA_Actor`.
- Per-actor creation cost is ~264 bytes — within the ~300-byte target.
- `STA_ERR_MAILBOX_FULL` is the canonical overflow signal. No silent drops.
- Deep copy with `STA_OBJ_IMMUTABLE` sharing is the default transfer
  semantics. The traversal must respect ADR 007 flag bits.
- Baseline copy cost: 41 ns median for a 5-element Array with one mutable
  sub-object.
- The ring buffer (Variant B) is measured, retained for reference, and
  rejected as the default due to the 4 KB pre-allocation density cost.
