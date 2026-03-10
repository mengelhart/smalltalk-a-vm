# Spike: Lock-Free MPSC Mailbox and Cross-Actor Message Copy

**Phase 0 spike — actor mailbox and message semantics**
**Status:** Ready to execute
**Related ADRs:** 007 (object header layout)
**Produces:** ADR 008 (mailbox design and message copy semantics)

---

## Purpose

This spike answers the two foundational questions of actor communication: how
messages are queued between actors (the mailbox mechanism), and what it means
to send a message across an actor boundary (the copy semantics). Both decisions
are load-bearing — they affect the scheduler, the GC, memory allocation, and
every cross-actor interaction in the system.

The architecture document (§9.4) mandates bounded mailboxes with observable
overflow as the default. This spike validates that the chosen queue structure
achieves this, measures the actual latency cost under realistic load, and locks
down the message representation at the actor boundary before any scheduler or
copy-on-send implementation is written.

**This spike does not build the scheduler, the GC, or the full actor
lifecycle.** It builds the minimal C needed to measure queue performance,
validate lock-freedom under TSan, and determine the copy cost for a concrete
message type. Permanent implementation follows ADR 008.

---

## Background: the sketch from §9.4 and the current actor stub

The architecture document (§9.4) specifies:

- Bounded mailboxes by default, unbounded as an opt-in
- Observable overflow — silent queue growth is never the default
- Overflow policy options: drop-oldest, drop-newest, reject-sender, escalate

The current stub in `src/vm/actor_spike.h` defines:

```c
/* Mailbox node — one per queued message */
typedef struct STA_MboxNode {
    struct STA_MboxNode *next;   /* 8 bytes — MPSC queue link */
    STA_OOP              msg;    /* 8 bytes — the message OOP */
} STA_MboxNode;                  /* 16 bytes */

/* Lock-free MPSC mailbox — stub head/tail pointers */
typedef struct {
    _Atomic(STA_MboxNode *) head;    /* 8 bytes */
    STA_MboxNode           *tail;    /* 8 bytes — written only by owner */
} STA_Mailbox;                       /* 16 bytes */
```

This is an unbounded intrusive linked-list stub — a placeholder, not a
design decision. The spike must replace it with a pair of validated variants,
measure them, and produce the numbers that ADR 008 will record.

---

## Questions to answer

Each question below requires a concrete answer — not a deferral — before ADR
008 is written. The spike test binary must demonstrate the answer, not merely
assert it from reasoning.

### Q1: MPSC correctness on arm64 with C17 atomics

Two variants must be implemented and validated:

**Variant A — Vyukov intrusive linked-list MPSC (unbounded)**

The classic lock-free MPSC queue due to Dmitry Vyukov. Each enqueue is a
single CAS on the tail pointer; the consumer walks the list from head to tail.
The critical property: multiple producers can enqueue concurrently with no
locking. The consumer is single-threaded by definition (one actor, one owner
thread at a time under the scheduler).

Key correctness concern: the consumer may encounter a transient inconsistency
window between the producer's CAS succeeding and the `next` pointer being
visible. The standard fix is to spin on `next` in the dequeue path. This spin
must be bounded or the scheduler's reduction accounting is broken. The spike
must implement the spin and verify it terminates under the stress test.

Memory ordering: use `memory_order_acquire` on loads and `memory_order_release`
on stores throughout. Do not use `memory_order_seq_cst`. The acquire/release
pairing is sufficient on arm64 and avoids unnecessary full memory barriers.

**Variant B — bounded power-of-two ring buffer with atomic head/tail**

A fixed-capacity MPSC ring buffer. Producers atomically advance a shared write
cursor; the consumer advances a private read cursor. Capacity must be a power
of two to allow index wrapping with bitwise AND.

The ring buffer is bounded by definition — this is the design that implements
§9.4's requirement. The spike must wire in overflow detection and return a
failure code from enqueue when the buffer is full.

Memory ordering: head/tail advances use `memory_order_acquire`/
`memory_order_release`. Slot state (empty/filled) uses the same pairing. Do
not use `memory_order_seq_cst`.

**Correctness gate:** both variants must pass `clang -fsanitize=thread` under
the multi-producer stress test (4 producers, 1 consumer, 1 million messages
per producer). TSan clean is a hard pass/fail requirement, not advisory. A
variant that fails TSan does not proceed to ADR 008.

The stress test must:
- Verify all enqueued messages are dequeued (no loss, no duplication)
- Verify message ordering is FIFO per-producer (messages from the same producer
  arrive in enqueue order, regardless of interleaving with other producers)
- Run for enough iterations that TSan has reasonable coverage of interleavings

### Q2: Measured enqueue/dequeue latency

Measure both variants under two load configurations:

**Baseline: single-producer / single-consumer**
One thread enqueues, one thread dequeues. Measures the pure queue overhead
with no contention. Establishes the floor latency.

**Stress: 4-producer / single-consumer**
Four threads enqueue concurrently into one queue consumed by one thread.
Simulates the realistic case where four scheduler threads may each send to
the same actor. This is the latency number that feeds scheduling quantum
sizing.

Report for each variant and each configuration:
- Median nanoseconds per enqueue+dequeue round-trip
- 99th-percentile nanoseconds per enqueue+dequeue round-trip
- Measure on arm64 Apple Silicon using `clock_gettime(CLOCK_MONOTONIC_RAW)`

The measurement methodology must be:
- Warm up the queue (discard first 1000 iterations)
- Measure a minimum of 100,000 round-trips per configuration
- Sort measurements to compute percentiles; do not use a running approximation

These numbers go directly into ADR 008 and will be used to justify the chosen
variant.

### Q3: Bounded vs. unbounded decision and overflow policy

The architecture (§9.4) requires bounded mailboxes as the default. Variant B
implements this. The spike must validate that Variant B's ring buffer achieves
observable overflow — that is, the enqueue function returns a failure code
(not silently drops) when the buffer is full, and the caller can observe and
act on the failure.

Three overflow policies to evaluate:

1. **Drop-oldest:** the oldest unread message is evicted to make room for the
   new one. The producer never blocks or fails; the consumer silently loses
   old messages. Simple to implement in the ring buffer (advance the read
   cursor unconditionally). Problem: the consumer cannot distinguish "no
   messages" from "messages were lost"; backpressure signal is lost.

2. **Drop-newest:** the new message is discarded when the buffer is full. The
   producer receives a failure code (`STA_ERR_MAILBOX_FULL`). The consumer
   sees every message it ever dequeues, in order. The sender must decide what
   to do — retry, drop, notify, or escalate. This is the recommended policy
   (see rationale below).

3. **Return-failure-to-sender:** identical to drop-newest in the single-shot
   send case. For `ask:` futures, the failure is returned to the waiting
   future. Requires the scheduler to have a channel back to the sender. Not
   implementable in the queue itself — requires scheduler cooperation.

**Recommendation to validate:** drop-newest (policy 2). Rationale:
- Preserves all messages the consumer has not yet seen — no silent data loss
- Gives the sender an explicit signal it can act on
- Does not require scheduler cooperation in the queue layer
- Consistent with §9.4: "overload should be visible, not hidden"
- The BEAM analogy: `gen_server:call/3` returns `{error, ...}` on timeout,
  not silent drop

The spike must demonstrate drop-newest working in the test: fill the buffer,
attempt one more enqueue, assert the return code is `STA_ERR_MAILBOX_FULL`,
assert the consumer still dequeues the original N messages in order.

The capacity for the ring buffer must be a power of two. The default capacity
to validate in the spike: 256 slots. This is a placeholder — the final value
will be a tunable parameter in the permanent implementation. Record the
measured headroom at default capacity (how long until a fast producer fills a
256-slot buffer receiving no attention from the consumer). ADR 008 must record
the rationale for this value — specifically, what headroom 256 slots provides
at a realistic inter-actor message rate, so the choice is not arbitrary when
revisited.

### Q4: Message representation at the actor boundary

The unit of copy must be defined explicitly. The reference case for this
spike is a **5-element Array object**:

- Header: `STA_ObjHeader` = 12 bytes (padded allocation = 16 bytes)
- Payload: 5 OOP slots × 8 bytes = 40 bytes
- Total allocation: 56 bytes

The spike must answer each of the following sub-questions with a concrete
implementation and measurement, not a design opinion:

**Q4a: Who allocates the message copy — sender or runtime?**

Two options:
- **Sender-allocates:** the sending actor's code (or the `send:` primitive)
  allocates the copy on the receiver's heap before enqueuing. Requires the
  sender to have a pointer to the receiver's allocator. Simplifies the
  dequeue path (the message is already on the right heap). Risk: sender holds
  a raw pointer into a foreign heap — must be mediated by the runtime.
- **Runtime-allocates on enqueue:** the send primitive passes an OOP; the
  runtime performs the copy into a transfer buffer or receiver heap during
  the enqueue call. The copy happens synchronously in the sender's thread.
  Simpler ownership model; slightly more latency on the send path.

The spike must implement runtime-allocates-on-enqueue using a per-message
`malloc`-backed allocation (standing in for the receiver's nursery). The
permanent allocator is out of scope for this spike; `malloc` is the correct
stub.

**Q4b: Is allocation on the receiver's heap or a neutral transfer buffer?**

Two options:
- **Receiver heap:** the copy lives on the receiver's nursery from the moment
  of send. When the receiver dequeues, the object is already local — no second
  copy. Risk: the sender must safely write to a foreign heap without races.
- **Transfer buffer:** the copy lives in a neutral allocation (e.g. a
  `malloc`'d slab) owned by the message node. The receiver copies from the
  transfer buffer into its nursery on dequeue. Second copy is a disadvantage.
  Ownership is cleaner — the message node owns the allocation; the consumer
  frees it after delivering to the actor.

The spike must implement the transfer-buffer model (single `malloc` per
message, freed by the consumer after delivery). This avoids the foreign-heap
write problem for Phase 0. Record whether the second-copy cost is measurable
or negligible at the scale of a 56-byte Array.

**Q4c: Is the copy shallow or deep?**

- **Shallow copy (message object only):** copy the Array header + 5 OOP
  slots. OOPs that point to other heap objects are copied as raw pointer
  values. The receiver now holds pointers into the sender's heap — only safe
  if those sub-objects are immutable (see Q4d).
- **Deep copy (full reachable graph):** traverse the object graph from the
  message root and copy every reachable mutable object. The receiver gets a
  fully independent graph. Higher cost, but no aliasing between actor heaps.

The architecture specifies deep copy semantics as the default (§10.1: "actors
are isolated — sharing requires explicit opt-in"). The spike must implement a
recursive deep-copy traversal that respects the `STA_OBJ_IMMUTABLE` bit from
ADR 007.

**Q4d: How does copy interact with the `STA_OBJ_IMMUTABLE` bit?**

From ADR 007: `obj_flags` bit 0 (`STA_OBJ_IMMUTABLE`) marks an object whose
slots cannot be written. An immutable object is safe to share by pointer
across actor boundaries — it will never change, so no isolation invariant is
broken.

The deep-copy traversal must check `obj_flags & STA_OBJ_IMMUTABLE` on each
reachable object:
- If set: do not copy — share the pointer. This is the optimization that
  makes large immutable structures (string literals, method bytecode, compiled
  method arrays) cheap to pass between actors.
- If clear: copy the object into the transfer buffer and update the OOP in the
  copy to point to the new copy (pointer fixup).

The spike must demonstrate this with a test case: a 5-element Array where
elements 0, 2, and 4 are SmallInts (immediate — no copy needed), element 1 is
a mutable String object (must be deep-copied), and element 3 is an immutable
Symbol object (must be shared, not copied). Assert the correct behavior for
each slot.

### Q5: Cross-actor copy cost

Measure the wall-clock cost of copying the 5-element Array reference case
(56 bytes of header + payload, with one mutable sub-object requiring a
recursive copy) from a simulated sender allocation to a transfer buffer.

Report in nanoseconds. Use `clock_gettime(CLOCK_MONOTONIC_RAW)` on arm64.

Warm up (discard first 1000 iterations), then measure 100,000 copy operations.
Report median and 99th-percentile.

This number becomes the baseline cross-actor copy cost in ADR 008. It sets
the floor for the `ask:` future latency budget and informs whether the
deep-copy model is viable for the workloads described in §10.9.

### Q6: Stub revision check

Review the `STA_Mailbox` stub in `src/vm/actor_spike.h`:

```c
typedef struct {
    _Atomic(STA_MboxNode *) head;    /* 8 bytes */
    STA_MboxNode           *tail;    /* 8 bytes — written only by owner */
} STA_Mailbox;                       /* 16 bytes */
```

State explicitly whether this stub needs revision based on spike results:
- Does it model Variant A correctly?
- Does it model Variant B at all?
- Does it capture the capacity field required for bounded operation?
- Should `STA_Actor.mailbox` be revised to embed the chosen variant?

The spike doc must give a plain-language answer here, which the ADR confirms
with measured evidence.

---

## What to build

All files are **spike code** — clearly marked, to be replaced during Phase 1
with permanent implementations informed by ADR 008. No file produced by this
spike is the permanent implementation.

### File 1: `src/actor/mailbox_spike.h`

Define both queue variant types, the message copy API, and all error codes
needed for overflow. This is the interface being evaluated.

```
Contents (to define in implementation, not to pre-bake here):
- STA_MpscList    — Variant A: intrusive linked-list (unbounded)
- STA_MpscRing    — Variant B: ring buffer (bounded, power-of-two capacity)
- STA_MsgCopy     — result of a cross-actor copy (transfer buffer + size)
- enqueue/dequeue function declarations for both variants
- sta_msg_copy_deep() declaration
- STA_ERR_MAILBOX_FULL error code (must not conflict with existing STA_ERR_* codes)
```

Do not embed the implementation in the header. Only `static inline` size/index
helpers belong in the header. All substantive logic lives in the `.c` file.

### File 2: `src/actor/mailbox_spike.c`

Both MPSC variants plus the deep-copy logic. Key implementation requirements:

- Variant A enqueue: single CAS on tail, `memory_order_acquire` on the load,
  `memory_order_release` on the store
- Variant A dequeue: spin on `next` pointer until visible (with backoff);
  `memory_order_acquire` on loads
- Variant B enqueue: CAS loop on write cursor; `memory_order_acquire` to read
  current state, `memory_order_release` to publish slot
- Variant B dequeue: non-atomic read of read cursor (owner-only);
  `memory_order_acquire` to read slot state
- `sta_msg_copy_deep()`: recursive traversal respecting `STA_OBJ_IMMUTABLE`,
  using `malloc` for transfer buffer allocations in this spike

### File 3: `tests/test_mailbox_spike.c`

Correctness tests and latency benchmarks. The test binary must:

1. **Correctness — Variant A (single-threaded):** enqueue N messages,
   dequeue N messages, verify values and order.

2. **Correctness — Variant B (single-threaded):** same as above plus an
   overflow test (fill buffer, assert `STA_ERR_MAILBOX_FULL` on next enqueue,
   drain buffer, assert values).

3. **Stress — both variants (multi-threaded):** 4 producer threads + 1
   consumer thread, 1 million messages per producer. Verify no loss, no
   duplication, FIFO per-producer order. This test is the TSan gate.

4. **Latency benchmark — both variants:** SPSC and 4P1C configurations.
   Report median and p99 nanoseconds. Print in a format suitable for direct
   inclusion in ADR 008.

5. **Copy correctness:** the 5-element Array test case with mixed
   mutable/immutable slots. Assert SmallInts pass through, mutable sub-objects
   are copied, immutable sub-objects are shared.

6. **Copy cost benchmark:** 100,000 copy operations of the 5-element Array
   reference case. Report median and p99 nanoseconds.

### File 4: `docs/decisions/008-mailbox-and-message-copy.md`

Written **after** the tests pass and the measurements are in hand. The ADR
must record:

- The chosen queue variant and why (with latency numbers from Q2)
- The overflow policy and why (with the overflow test result from Q3)
- The message copy model: who allocates, where, shallow vs. deep (from Q4)
- The `STA_OBJ_IMMUTABLE` sharing optimization (from Q4d)
- The baseline copy cost in nanoseconds (from Q5)
- Whether `STA_Mailbox` in `actor_spike.h` needs revision (from Q6)
- Updated actor density measurement: if the ring buffer is embedded in
  `STA_Actor`, add `sizeof(STA_MpscRing)` to the per-actor creation budget
  and confirm the ~300-byte target still holds

Do not write ADR 008 before the test binary exists and ctest passes.

---

## CMake integration

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(test_mailbox_spike test_mailbox_spike.c
               ../src/actor/mailbox_spike.c)
target_include_directories(test_mailbox_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_mailbox_spike PRIVATE -fsanitize=thread)
target_link_options(test_mailbox_spike PRIVATE -fsanitize=thread)
add_test(NAME mailbox_spike COMMAND test_mailbox_spike)
```

The TSan-instrumented binary is the ctest target. Passing ctest with TSan
clean is the gate before ADR 008 is written.

Note: latency benchmarks should be run separately from the TSan build, since
TSan instrumentation adds significant overhead. The benchmark results must come
from a non-TSan build (`-O2`, no sanitizers). The CMake integration should
provide a second target for this:

```cmake
add_executable(bench_mailbox_spike test_mailbox_spike.c
               ../src/actor/mailbox_spike.c)
target_include_directories(bench_mailbox_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(bench_mailbox_spike PRIVATE STA_BENCH=1)
target_compile_options(bench_mailbox_spike PRIVATE -O2)
# Not registered as a ctest target — run manually for benchmark output
```

---

## Constraints

- All spike files must carry a prominent comment at the top:
  `SPIKE CODE — NOT FOR PRODUCTION`
- Do not modify `include/sta/vm.h` during this spike
- Do not modify `docs/decisions/007-object-header-layout.md`
- `src/actor/mailbox_spike.h` and `.c` may include `src/vm/oop_spike.h` for
  the `STA_OOP` type and `STA_OBJ_IMMUTABLE` flag — this is intentional
  inter-spike dependency; both are spike code
- Tests may not include `src/` headers directly except through the spike
  header chain — no back-channel to `include/sta/vm.h`

---

## Open questions this spike deliberately does not answer

These are real questions identified during spike design. They are deferred
to future ADRs so they do not block this spike.

1. **Scheduler integration.** The mailbox is a data structure; how the
   scheduler learns that a mailbox has become non-empty is a separate
   concern. Options include a per-actor `_Atomic bool runnable` flag, a
   scheduler-owned run queue, and an eventfd/futex wake channel. This belongs
   in the scheduler spike.

2. **Memory reclamation for Variant A nodes.** The linked-list variant
   allocates one node per message. In Phase 0, the spike uses `malloc`/`free`.
   The permanent implementation will use per-actor pool allocation. Hazard
   pointers or epoch-based reclamation may be needed if nodes are freed while
   other threads hold references. Deferred to Phase 1.

3. **Actor heap allocator.** The spike uses `malloc` to stand in for the
   receiver's nursery in the copy path. The real implementation uses the
   actor's bump-pointer nursery allocator. The copy function's allocator
   argument will be a function pointer or inline callback in the permanent
   implementation.

4. **`ask:` future semantics on mailbox-full.** If the sender is blocked on a
   future and `STA_ERR_MAILBOX_FULL` is returned, the future must be resolved
   with a failure. The future/promise machinery does not exist yet. The spike
   records the error code; the resolution path is a Phase 2 concern.

5. **Immutable region and shared-immutable objects.** The `STA_OBJ_SHARED_IMM`
   flag from ADR 007 marks objects in the shared immutable region (compiled
   methods, string literals). These are never copied — they are always shared.
   The spike handles `STA_OBJ_IMMUTABLE` (object-level immutability); the
   region-level flag and its interaction with GC is deferred to the GC spike.

6. **Message serialization for external transport.** The architecture envisions
   eventual cluster-mode actors (§10.x, deferred). Deep copy within a process
   is not the same problem as serialization across a network. This spike is
   intra-process only.

---

## What ADR 008 must record

After running the spike, write ADR 008 covering:

- Queue variant chosen (A or B or both, with rationale)
- Measured latency numbers (median and p99, SPSC and 4P1C, both variants)
- Overflow policy chosen and rationale
- Default ring buffer capacity and rationale
- Message copy model: allocator, location, depth
- `STA_OBJ_IMMUTABLE` sharing rule
- Measured cross-actor copy cost for the 5-element Array reference case
- Revised `STA_Mailbox` definition if the stub needs changing
- Updated actor density table with mailbox embedded in `STA_Actor`
- Confirmation that ~300-byte target still holds (or justified delta)
