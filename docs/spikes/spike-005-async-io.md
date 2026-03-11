# Spike: Async I/O Integration via libuv

**Phase 0 spike — async I/O architecture and actor suspension/resumption**
**Status:** Ready to execute
**Related ADRs:** 007 (object header), 008 (mailbox), 009 (scheduler), 010 (frame layout)
**Produces:** ADR 011 (async I/O architecture)

---

## Purpose

This spike answers the foundational questions of async I/O integration: how an
actor that initiates an I/O operation is suspended from the scheduler run queue
without blocking any scheduler thread, and how the actor is returned to runnable
state when the I/O operation completes.

The architecture document (§9.3) specifies the shape: libuv as the async I/O
substrate, I/O-bound actors registered and suspended, a poller that re-enqueues
them on completion. ADR 009 established the scheduler interface (`sta_sched_push`)
as the re-enqueue path. This spike validates that path end-to-end, measures the
actor density impact, and proves — with a TSan-clean test — that no scheduler
thread ever blocks on an I/O operation.

**This spike does not build the Smalltalk message layer, file I/O, DNS, or
subprocesses.** It builds the minimal C needed to answer the architecture questions
above, integrated with libuv, with ctest-passing test coverage. Permanent
implementation follows ADR 011.

---

## Background

### §9.3 — Async I/O

The architecture document states:

> "I/O-bound actors register interest and are suspended; the I/O poller wakes them
> when their operation completes by placing a completion message in their mailbox.
> The actor resumes on the next scheduler quantum as if it received any other
> message."
>
> "Concrete recommendation for the initial implementation: **wrap `libuv`** (the
> async I/O library underlying Node.js). It is production-proven, handles all
> platform differences between macOS and Linux, and provides the event loop model
> we need."

The architecture identifies the correctness requirement explicitly:

> "A scheduler thread blocked on a socket read is a scheduler thread that cannot
> execute any other actor — with a fixed pool of scheduler threads equal to CPU
> count, even one blocking call degrades the entire system."

### §8.3 — Runtime model

> "Per-actor heap target: actor creation cost and baseline heap size should be in
> the same order of magnitude as BEAM (~300 bytes). This is an explicit design
> constraint, not a nice-to-have."

### §9.4 — Backpressure

> "Bounded mailboxes with observable overflow behaviour. Unbounded mailboxes are
> available but explicit."

Relevant to this spike: the I/O completion re-enqueue model must not bypass the
mailbox overflow policy if the synthetic-completion-message path is chosen (see Q6).

### Current actor density baseline (ADR 009 + ADR 010 projection)

| Component | Bytes |
|---|---|
| `STA_ActorRevised` (ADR 009, projected with stack fields from ADR 010) | 136 |
| Initial nursery slab | 128 |
| Actor identity object (0-slot ObjHeader) | 16 |
| **Projected total creation cost entering Spike 005** | **280** |
| Target | ~300 |
| Headroom | **20** |

ADR 010 explicitly states: "⚠ Density headroom is tight and actionable. 20 bytes
entering Spike 005." The ADR 010 scenario table is reproduced here as the frame of
reference for this spike's density measurement:

| Scenario | `STA_Actor` size | Creation cost | Remaining headroom |
|---|---|---|---|
| Low (+8 bytes) | 144 | 288 | 12 |
| Mid (+12 bytes) | 148 | 292 | 8 |
| High (+16 bytes) | 152 | 296 | 4 |
| At limit (+20 bytes) | 156 | 300 | 0 |
| Breach (+24 bytes) | 160 | **304 — over target** | — |

The ADR 009 re-enqueue interface, confirmed in that ADR's consequences, is:

> "The `sta_sched_push` + Option 1 notification path is already the correct
> interface; no protocol change is anticipated."

This spike exercises that interface from the I/O completion path.

---

## Questions to answer

Each question requires a concrete answer — not a deferral — before ADR 011 is
written. The spike test binary must demonstrate the answer.

---

### Q1: Actor suspension and resumption mechanics

When an actor initiates an I/O operation (timer, TCP connect, read, write) it must
be immediately removed from the scheduler run queue. The scheduler thread must be
freed to run other actors for the entire duration of the I/O wait. When the I/O
operation completes, the actor must be returned to runnable state via
`sta_sched_push()` — the path validated in ADR 009.

**Define the full suspension protocol:**

1. Actor calls `sta_io_timer_start()` or `sta_io_tcp_read()` (spike API — spike-only).
2. The call registers the I/O handle on the libuv loop (running on the I/O thread).
3. The actor's `io_state` field is atomically set to `STA_IO_PENDING`.
4. The actor's `sched_flags` is atomically set to `STA_SCHED_SUSPENDED`
   (removing it from the run queue — it must not be picked up again by the
   scheduler's `execute_actor` path while suspended).
5. `sta_io_timer_start()` / `sta_io_tcp_read()` returns to the scheduler loop
   immediately — the calling scheduler thread does not wait.
6. The scheduler thread picks the next actor from its deque and continues.

**Define the full resumption protocol:**

1. The libuv callback fires on the I/O thread when the operation completes.
2. The callback stores the completion result in the actor's `io_result` field.
3. The callback atomically transitions `io_state` from `STA_IO_PENDING` to
   `STA_IO_IDLE`.
4. The callback calls `sta_sched_push(actor)` — using the same function path
   as any other re-enqueue event (ADR 009: `pthread_cond_signal` notification
   path). The actor is returned to a scheduler thread's deque.
5. The scheduler thread picks the actor, sets `sched_flags` to `STA_SCHED_RUNNING`,
   and executes it. The actor reads `io_result` to obtain the completion value.

**Validate with a concrete test — the "one I/O, one compute" scenario:**

- Spawn one scheduler thread (not the full core count — single-thread to make the
  test deterministic).
- Create one compute actor: a tight reduction loop that logs timestamps of every
  preemption (i.e., it runs continuously).
- Create one I/O actor: starts a 50 ms timer, then waits for it.
- While the timer is pending, the compute actor must be scheduled and execute
  reductions. Record the number of compute-actor preemptions that occur between
  the timer start and the timer completion callback.
- After the timer fires, the I/O actor is re-enqueued, receives its quota, and
  runs to completion.
- **Assert:** the compute actor was scheduled at least once between the I/O
  actor's suspension and resumption. Zero compute-actor preemptions during the
  50 ms wait is a correctness failure (it means the scheduler thread was
  blocked, violating the no-blocking-scheduler invariant from Q4).

**Measure:**

1. **Suspension overhead:** time from the I/O actor calling `sta_io_timer_start()`
   to the scheduler thread beginning to execute the compute actor. Report in
   nanoseconds. This is the overhead of removing the I/O actor from the run queue
   and picking the next actor.
2. **Wake latency:** time from the libuv timer callback firing to the I/O actor
   executing its first instruction after resumption. Report in nanoseconds. Compare
   to the ADR 009 cond-signal wake latency baseline (median 5,958 ns, p99 7,875 ns).
   The I/O wake latency should be in the same order of magnitude — if it is
   significantly higher, investigate whether the I/O thread's `sta_sched_push` is
   contending with the scheduler thread's deque.

---

### Q2: libuv integration architecture

Three options must be evaluated. Option A must be implemented. Options B and C are
sketched in the rationale only.

#### Option A: Dedicated I/O thread (implement this)

One OS thread (`pthread_create`) runs `uv_run(loop, UV_RUN_DEFAULT)` — the libuv
default run mode that blocks until all handles are inactive or the loop is stopped.
The libuv loop is created at I/O subsystem initialisation (`sta_io_init`) and torn
down at `sta_io_destroy`. The I/O thread starts before any scheduler thread begins
accepting work and is joined cleanly at shutdown.

**I/O thread lifecycle:**

```
sta_io_init()   — uv_loop_init, create I/O thread (pthread_create),
                   start a sentinel uv_async_t handle to keep the loop alive
sta_io_destroy() — signal the sentinel async, join the I/O thread,
                    uv_loop_close, uv_loop_delete
```

**Scheduler threads → I/O thread communication:**

When an actor requests an I/O operation, the scheduler thread (running the actor)
must register the libuv handle on the I/O thread's loop. The libuv API is not
thread-safe for handle creation across threads. The correct approach is to use a
`uv_async_t` as a cross-thread notification channel: the scheduler thread enqueues
the I/O request (handle type + parameters + actor pointer) in a lock-free MPSC
queue, then calls `uv_async_send()` on the I/O thread's wakeup async. The I/O
thread's `uv_async_t` callback drains the queue and creates/starts the handles
within the loop context.

For the spike, implement a simplified version: a single `uv_async_t` for wakeup
plus a mutex-protected FIFO of pending I/O requests. The lock-free MPSC variant
is a Phase 2 implementation concern; the spike must demonstrate correctness, not
maximum throughput.

**I/O thread → scheduler threads communication:**

libuv callbacks fire on the I/O thread. The callback reads the actor pointer from
the handle's `data` field, writes the completion result into `actor->io_result`,
then calls `sta_sched_push(actor)`. The `sta_sched_push` implementation (from
spike-003 / ADR 009) pushes the actor onto a scheduler thread's Chase-Lev deque
and signals the scheduler thread via `pthread_cond_signal`. This path is already
validated; this spike exercises it from the I/O callback context.

**Thread model diagram:**

```
Scheduler Thread 0          I/O Thread (libuv)
──────────────────          ──────────────────
execute_actor(A)
  → sta_io_timer_start(A)   ← uv_async_send (wakeup request queue)
  → suspend A               ← callback drains queue
  → push A.io_state=PENDING ← uv_timer_start(handle, 50ms, cb)
  → pick next actor         uv_run() blocks, polling...
execute_actor(B)
  ...                       [50 ms elapses]
                            timer_cb(handle)
                              → actor = handle->data
                              → actor->io_result = ...
                              → actor->io_state = IDLE
                              → sta_sched_push(actor)    ← enqueues A
                                  → pthread_cond_signal
Scheduler wakes (if idle)
execute_actor(A)            ← resumes here
```

**Measure:**

1. **Scheduler-thread CPU utilisation** while one I/O actor is suspended waiting for
   a 100 ms timer: with one scheduler thread and one compute actor, the compute actor
   should consume approximately 100% of the scheduler thread's time during the wait.
   Measure total reductions executed by the compute actor in the 100 ms window and
   compare to the expected quantum count (`100 ms / quantum_duration`). If the
   compute actor is significantly below the expected reduction rate, the scheduler
   thread is being stolen for I/O work.

2. **Wake latency** (I/O thread → scheduler thread): time from `sta_sched_push()`
   returning inside the libuv callback to the actor's first instruction on the
   scheduler thread. This is the same measurement as ADR 009 Q3 wake latency but
   measured from the I/O thread as the sender. Use `clock_gettime(CLOCK_MONOTONIC_RAW)`.
   Report median and p99 over 10,000 timer completions.

#### Option B: Scheduler thread doubles as I/O poller (sketch only — do not implement)

One of the N scheduler threads calls `uv_run(loop, UV_RUN_NOWAIT)` between actor
executions. This avoids a dedicated I/O thread but has critical problems:

- **Imbalanced scheduling:** one scheduler thread spends time polling; N-1 threads
  do not. Under I/O-heavy load the polling thread is slower than its peers, creating
  a systematic scheduling imbalance.
- **Poll latency:** `uv_run(UV_RUN_NOWAIT)` only processes events already queued;
  if the scheduler thread is busy executing a long-running actor, I/O completions
  accumulate until the next quantum boundary. The minimum wake latency is bounded
  by the quantum duration (potentially milliseconds).
- **Complexity:** the polling thread must interleave actor execution with event loop
  maintenance. Error handling for the event loop becomes entangled with the scheduler.

Option B is not prototyped. It is rejected on architectural grounds; the rationale
must be stated in ADR 011.

#### Option C: Per-actor uv_loop_t (sketch only — do not implement)

Each actor that uses I/O owns a separate `uv_loop_t`. A pool of I/O threads
(smaller than the scheduler pool) drains loops from a queue.

Problems:
- `uv_loop_t` is not a small struct — `sizeof(uv_loop_t)` on macOS is approximately
  1,168 bytes. Embedding it in `STA_Actor` would immediately breach the 300-byte
  density target by ~1,000 bytes. Allocated separately, it requires a pointer in
  `STA_Actor` (8 bytes, within the density budget) but adds heap fragmentation.
- The I/O thread pool size and scheduling are a new design problem. Which thread
  picks which actor's loop? Under heavy I/O load, actors with high-rate I/O starve
  actors with low-rate I/O sharing a thread.
- Per-actor loops cannot share file descriptor limits efficiently; on macOS, the
  default `kqueue` limit is per-process, not per-loop.

Option C is not prototyped. It is rejected on density and complexity grounds.
State this explicitly in ADR 011. Note: the per-actor loop size measurement
(`sizeof(uv_loop_t)`) must be included in the ADR as a measured data point,
not an estimate.

---

### Q3: STA_Actor density impact

The baseline entering this spike is the ADR 010 projection: `sizeof(STA_ActorRevised)`
with `stack_base` and `stack_top` added = 136 bytes. Total creation cost = 280 bytes.
Headroom = 20 bytes.

**Fields required for I/O suspension at minimum:**

```c
/* To be added to the STA_Actor struct (spike version: STA_ActorIo) */
_Atomic uint32_t io_state;    /* 4 bytes — STA_IO_IDLE / STA_IO_PENDING */
int32_t          io_result;   /* 4 bytes — completion result code or value */
```

The `io_state` field must be atomic because it is written by the I/O thread callback
and read by the scheduler thread when deciding whether an actor is runnable. The
`io_result` field is written before `io_state` transitions (sequenced by the
transition), so it does not need to be independently atomic (see Q4 for the memory
ordering rationale).

Total addition at minimum: **8 bytes** → Low scenario from ADR 010.

Additional fields that may be needed depending on the re-enqueue model chosen (Q6):
- If the direct-deque-push model is used: no additional fields beyond the 8 bytes above.
- If the synthetic-completion-message model is used: a completion message buffer or
  pointer may be needed (+8 bytes → Mid or High scenario).

**Spike task:** implement `STA_ActorIo` in `src/io/io_spike.h` — a spike-only struct
that embeds the ADR 009 `STA_ActorRevised` layout and adds the I/O fields. Measure
`sizeof(STA_ActorIo)` in the test binary. Report which ADR 010 scenario was reached.

**Measure and report:**

```
sizeof(STA_ActorIo)   = measured
initial nursery slab  = 128 bytes
actor identity object = 16 bytes
─────────────────────────────────
total                 = ?
target                = ~300 bytes
scenario              = Low / Mid / High / At limit / Breach
```

If the total exceeds 300 bytes, ADR 011 must justify the overage per CLAUDE.md:
"Drift from ~300 bytes must be explained in a decision record. Never silently ignored."
This is a mandatory measurement even if the total is within target.

---

### Q4: Scheduler-thread never blocks — proof

**The correctness requirement** (§9.3): no scheduler thread may ever block waiting
for an I/O operation. A scheduler thread blocked on a socket read while other actors
are runnable is a correctness failure, not a performance issue.

**Proof method:** TSan + structural analysis.

**Structural argument (must be stated in ADR 011):**

1. Scheduler threads call only: `sta_io_timer_start()`, `sta_io_tcp_connect()`,
   `sta_io_tcp_read()`, `sta_io_tcp_write()`. Each of these functions:
   a. Enqueues an I/O request in the pending-request FIFO (mutex-protected write).
   b. Calls `uv_async_send()` — a non-blocking cross-thread wakeup.
   c. Returns immediately to the scheduler loop.
   No scheduler thread calls `uv_run()`, `read()`, `recv()`, `accept()`, or any
   other blocking system call. Only the dedicated I/O thread calls `uv_run()`.

2. The FIFO mutex is held for microseconds (enqueue one request, release). If the
   mutex is contested (I/O thread is simultaneously draining), the worst case is one
   scheduler thread blocked for the duration of a FIFO drain operation — which is
   bounded by the number of pending requests, each of which requires only a
   `uv_timer_start()` or `uv_tcp_connect()` call (non-blocking). This is not I/O
   blocking; it is a bounded critical section. It must still be minimised; the
   lock-free MPSC path (Phase 2) eliminates it entirely.

3. `uv_async_send()` is documented as non-blocking and thread-safe by libuv. It
   writes to a pipe or eventfd internally; on macOS it uses a `uv__async_send`
   that writes to a pipe. This is not I/O blocking from the scheduler thread's
   perspective — it is a bounded syscall (one write to a local kernel buffer).

**TSan gate:**

The test binary must include a dedicated "no-blocking-scheduler" test:

- One scheduler thread, one I/O actor (50 ms timer), one compute actor.
- The compute actor must be scheduled at least once between I/O actor suspension
  and resumption (see Q1 validation).
- The entire test must be TSan clean.

**Memory ordering for io_state:**

The I/O thread writes `actor->io_result` and then atomically transitions
`actor->io_state` from `STA_IO_PENDING` to `STA_IO_IDLE` using
`memory_order_release`. The scheduler thread reads `actor->io_state` with
`memory_order_acquire`. The release/acquire pair provides the happens-before edge:
any scheduler thread that observes `io_state == STA_IO_IDLE` is guaranteed to see
the fully written `io_result`.

The transition from `STA_IO_IDLE` to `STA_IO_PENDING` (done by the scheduler
thread when the actor initiates an I/O request) must use `memory_order_release`
so that the I/O thread's `acquire` read of `io_state` sees the request has been
set up before the handle is started. Document all orderings in ADR 011.

---

### Q5: I/O types in scope for the spike

**Timer (uv_timer_t) — required**

The timer case validates the full suspend/resume loop with no real I/O. It is
the simplest possible I/O operation and must pass before TCP is attempted.

- `sta_io_timer_start(actor, delay_ms)` — registers a timer on the I/O thread's
  loop, suspends the actor.
- Timer callback: sets `io_result = 0` (success), calls `sta_sched_push(actor)`.
- Test: single actor, 50 ms timer, verifies suspension and correct resumption.
- Measure: wake latency distribution (10,000 samples).

**TCP loopback socket — required**

Validates the real network I/O path. The server and client must both be actors.

Operations to implement:
- `sta_io_tcp_listen(actor, port)` — bind + listen; actor suspends until a
  connection arrives.
- `sta_io_tcp_connect(actor, host, port)` — connect; actor suspends until
  connected.
- `sta_io_tcp_read(actor, buf, len)` — read; actor suspends until data arrives.
- `sta_io_tcp_write(actor, buf, len)` — write; actor suspends until data is
  flushed.

Test: two actors on the same scheduler thread (deterministic), loopback address
(127.0.0.1), one small message sent from client actor to server actor, server
echoes it back. Verify the round-trip completes correctly with no scheduler thread
blocking.

**File I/O and DNS — out of scope**

File I/O (`uv_fs_t`) and DNS (`uv_getaddrinfo`) follow the same actor suspension
pattern — the I/O thread calls the libuv API, the callback re-enqueues the actor.
They are deferred to Phase 2 because they do not add architectural information beyond
what the timer and TCP paths already validate. State this explicitly in ADR 011.

**`uv_loop_t` size measurement — required for Option C rejection**

In the test binary, print `sizeof(uv_loop_t)` as a data point for the ADR 011
Option C rejection rationale.

---

### Q6: I/O completion re-enqueue model

Two models must be evaluated. One must be chosen and implemented.

#### Model A: Direct deque push (recommended for evaluation)

The libuv callback (running on the I/O thread) calls `sta_sched_push(actor)`
directly. `sta_sched_push` is the same function validated in ADR 009 — it pushes
the actor onto a scheduler thread's Chase-Lev deque and signals the thread via
`pthread_cond_signal`.

**Advantages:**
- Minimal latency: one `sta_sched_push` call is cheaper than allocating and
  enqueuing a completion message.
- No message allocation on the completion path: the I/O thread does not touch
  the actor's nursery heap (which may be on a different NUMA node or cache
  region from the I/O thread).
- ADR 009 explicitly anticipated this: "The `sta_sched_push` + Option 1
  notification path is already the correct interface; no protocol change is
  anticipated."
- Simple implementation: the I/O callback is three lines (write result, set
  state, call push).

**Disadvantages:**
- Actor execution resumes mid-message rather than at a message boundary. The
  actor's current execution context (reduction counter, frame stack) must be
  in a state that permits resume. For this spike, the actor is always at the
  top of its execution (suspended before any frame was pushed) — this is a
  valid resume point. The full interpreter must define what constitutes a valid
  resume point for suspended actors.
- The `sta_sched_push` function must be callable safely from the I/O thread.
  This requires that the I/O thread can access the scheduler's deque array and
  call `pthread_cond_signal` — both of which are safe given the ADR 009 design.
  Verify this explicitly and state it in ADR 011.

#### Model B: Synthetic completion message (do not implement — sketch and reject or accept)

The libuv callback enqueues a completion message into the actor's MPSC mailbox
(the `STA_MpscList` from ADR 008). The actor is then considered "runnable with
a pending message" and the scheduler's normal message-delivery path handles
re-enqueue. When the actor is scheduled, it processes the completion message
and reads the result from the message payload.

**Advantages:**
- Resume always happens at a clean message boundary — consistent with the normal
  actor scheduling model.
- Completion results are carried in the message, not in actor struct fields, which
  is compositionally cleaner.
- Backpressure from §9.4 applies uniformly: if the mailbox is full, the I/O
  completion is subject to the same overflow policy as any other message.

**Disadvantages:**
- One message allocation per I/O completion. Under high I/O throughput this is
  a significant allocator pressure source.
- The I/O thread becomes a message producer — it must acquire the allocator for
  the completion message. The per-actor nursery allocator is not thread-safe
  (ADR 008: per-actor heap isolation). A completion message must either:
  a. Be pre-allocated before the I/O is initiated (adds fields to `STA_Actor`).
  b. Be allocated from a global pool (introduces a global lock on the completion
     path — undesirable).
  c. Be stack-allocated and copied into the mailbox (complicated, brittle).
- The additional message allocation adds latency to the critical completion path.
  The whole point of the I/O design is low wake latency for I/O-bound actors;
  a message allocation in the callback works against that goal.

**Decision:** choose one model, implement it, and state the rationale in ADR 011.
The direct deque push (Model A) is the simpler path and aligns with the ADR 009
design intent. If Model B is chosen, explain why the message-allocation problems
above are not disqualifying. **Do not leave this undecided in ADR 011.**

---

## What to build

All files produced by this spike are **spike code** — clearly marked, to be replaced
during Phase 2 with permanent implementations informed by ADR 011. No file produced
here is the permanent implementation.

### File 1: `src/io/io_spike.h`

Types and interface for the spike I/O subsystem. Define:

```
Contents (to define in implementation):
- STA_IoState enum     — STA_IO_IDLE, STA_IO_PENDING (values for io_state field)
- STA_ActorIo struct   — spike actor struct: embeds STA_ActorRevised fields plus
                          _Atomic uint32_t io_state and int32_t io_result.
                          Do NOT embed uv_loop_t. Do NOT embed uv_handle_t of any kind.
- STA_IoSubsystem struct — spike I/O subsystem: uv_loop_t, I/O thread handle,
                           wakeup async, request FIFO, mutex, running flag.
- Function declarations:
    sta_io_init(STA_IoSubsystem *)
    sta_io_destroy(STA_IoSubsystem *)
    sta_io_timer_start(STA_IoSubsystem *, STA_ActorIo *, uint64_t delay_ms)
    sta_io_tcp_listen(STA_IoSubsystem *, STA_ActorIo *, uint16_t port)
    sta_io_tcp_connect(STA_IoSubsystem *, STA_ActorIo *, const char *host, uint16_t port)
    sta_io_tcp_read(STA_IoSubsystem *, STA_ActorIo *, uint8_t *buf, size_t len)
    sta_io_tcp_write(STA_IoSubsystem *, STA_ActorIo *, const uint8_t *buf, size_t len)
    sta_io_resume_actor(STA_ActorIo *, int32_t result)   — internal: sets io_result, pushes to scheduler
```

**Hard rules for this header:**
- `uv_loop_t`, `uv_timer_t`, `uv_tcp_t`, or any other libuv handle type must NOT
  appear in this header. They are forward-declared or kept entirely inside
  `io_spike.c`. This enforces the constraint: "libuv types must not appear in any
  header outside src/io/".
- Include `<stdint.h>`, `<stdatomic.h>` only. No `<uv.h>` in the header.
- Do not embed implementation in the header. Only `static inline` size helpers
  or constants belong here.

### File 2: `src/io/io_spike.c`

All libuv interaction, I/O thread lifecycle, actor suspension and resumption,
request FIFO, and handle callbacks. Key requirements:

- **I/O thread function:** runs `uv_run(loop, UV_RUN_DEFAULT)`. When `sta_io_destroy`
  is called, the wakeup async callback calls `uv_stop(loop)` and `uv_async_close`
  on all active handles, then lets `uv_run` return naturally.

- **Request FIFO and wakeup async:**
  - `sta_io_timer_start()` (called from scheduler thread): lock the FIFO mutex,
    push a request `{type=TIMER, actor, delay_ms}`, unlock, call
    `uv_async_send(&io->wakeup)`.
  - The wakeup async callback (called on I/O thread): lock FIFO, drain all requests,
    unlock. For each timer request: allocate a `uv_timer_t` (heap), set `handle->data
    = actor`, call `uv_timer_start(handle, timer_cb, delay_ms, 0)`.
  - For TCP requests: analogous with `uv_tcp_t` handles.

- **Timer callback:**
  ```c
  static void timer_cb(uv_timer_t *handle) {
      STA_ActorIo *actor = handle->data;
      sta_io_resume_actor(actor, 0);
      uv_timer_stop(handle);
      uv_close((uv_handle_t*)handle, handle_close_cb);
  }
  ```
  where `handle_close_cb` frees the heap-allocated handle after libuv finishes
  closing it (the libuv close protocol requires waiting for the close callback).

- **`sta_io_resume_actor()`:**
  ```c
  actor->io_result = result;
  atomic_store_explicit(&actor->io_state, STA_IO_IDLE, memory_order_release);
  sta_sched_push(/* scheduler */ , actor);  /* ADR 009 path */
  ```
  Memory ordering: `memory_order_release` on `io_state` so the scheduler thread's
  `acquire` read of `io_state` in the actor's check loop is properly ordered.

- **TCP implementation:**
  - `sta_io_tcp_listen`: `uv_tcp_init`, `uv_ip4_addr`, `uv_tcp_bind`,
    `uv_listen` with `connection_cb`. Suspend the actor until `connection_cb` fires.
  - `sta_io_tcp_connect`: `uv_tcp_init`, `uv_ip4_addr`, `uv_tcp_connect` with
    `connect_cb`. Suspend the actor until `connect_cb` fires.
  - `sta_io_tcp_read`: `uv_read_start` with `alloc_cb` + `read_cb`. Suspend
    the actor; the `read_cb` writes data into the actor's pre-provided buffer
    and resumes.
  - `sta_io_tcp_write`: `uv_write` with `write_cb`. Suspend the actor until the
    write is flushed.

- **Startup barrier:** the I/O thread must signal that `uv_run` has started before
  `sta_io_init` returns. Use a `pthread_cond_t` + flag (same pattern as ADR 009
  scheduler startup barrier). The scheduler must not enqueue I/O requests before
  the I/O loop is running.

- **Shutdown:** call `uv_stop(loop)` from the wakeup async callback when the
  running flag is false. After `uv_run` returns, the I/O thread exits. The main
  thread joins it in `sta_io_destroy`.

### File 3: `tests/test_io_spike.c`

Correctness tests, suspension/resume validation, no-blocking-scheduler proof,
timer test, TCP loopback test, and TSan gate. The test binary must print
measurements and exit 0 on all checks passing.

**Required test cases:**

1. **I/O subsystem lifecycle.** `sta_io_init`, sleep 10 ms, `sta_io_destroy`.
   No active I/O operations. Verify clean startup and shutdown, no TSan reports.

2. **Timer suspension/resumption — single actor.** Create one `STA_ActorIo` with
   `io_state = STA_IO_IDLE`. Call `sta_io_timer_start(50 ms)`. Assert that
   `io_state` transitions to `STA_IO_PENDING` before the timer fires (i.e., the
   suspension is synchronous). After 60 ms, assert `io_state == STA_IO_IDLE`
   and `io_result == 0`. Use a polling loop with `_Atomic` read on `io_state`
   — do not use `usleep` on the scheduler path.

3. **No-blocking-scheduler proof.** One scheduler thread (simulated via a single
   pthread running the scheduling loop from ADR 009's `scheduler_spike`), one I/O
   actor with a 50 ms timer, one compute actor (tight reduction loop). Assert the
   compute actor's `run_count` increases during the 50 ms wait. The I/O actor must
   resume correctly after the timer fires. TSan gate: the entire test must be clean
   under `-fsanitize=thread`. This is the key correctness test — it must pass before
   ADR 011 is written.

4. **Timer wake latency benchmark.** 10,000 sequential timer completions (1 ms
   timer each). Record time from `uv_timer_cb` start to actor `run_count`
   incrementing on the scheduler thread. Discard first 1,000 samples. Report
   median and p99 in nanoseconds. Print in a format suitable for direct inclusion
   in ADR 011. (Run without TSan for accurate numbers — see CMake integration.)

5. **TCP loopback echo.** Two `STA_ActorIo` instances (server and client) on one
   scheduler thread. Server: `sta_io_tcp_listen(port=14400)`. Client:
   `sta_io_tcp_connect("127.0.0.1", 14400)`. Server accepts the connection.
   Client writes 16-byte payload. Server reads 16 bytes and echoes them back.
   Client reads the echo. Assert payload integrity round-trip. Verify both actors
   suspend during I/O operations and resume correctly. TSan gate.

6. **TCP loopback under compute load.** Same as test 5 but add a compute actor
   (tight reduction loop) running concurrently. Assert the compute actor runs at
   least 5 times between the client's write and the client's read-echo completion.
   This verifies that TCP I/O does not block the scheduler thread.

7. **`sizeof(uv_loop_t)` measurement.** Print `sizeof(uv_loop_t)`. This is the
   data point for ADR 011 Option C rejection. Assert it is greater than 300 bytes
   (i.e., embedding it in `STA_Actor` would trivially breach the density target).

8. **Actor density checkpoint.** Print the full density table:
   - `sizeof(STA_ActorIo)` (the spike struct with io fields)
   - `sizeof(STA_ActorIo)` + 128 (nursery) + 16 (identity obj) = total creation cost
   - Which ADR 010 scenario was reached (Low / Mid / High / At limit / Breach)
   Assert total ≤ 300 bytes. If > 300 bytes, print an explicit WARNING that ADR 011
   must justify the overage — do not fail the test (density is a design target, not
   a hard test gate in spike binaries), but the WARNING must be visible in ctest
   output.

---

## CMake integration

The `STA_USE_LIBUV` option is already wired in the build but defaults to OFF.
Set it to ON for this spike. The CMake option should find libuv via
`find_package(libuv)` or `pkg_check_modules(libuv libuv)` — use whichever is
available on macOS with Homebrew (`brew install libuv`).

Add to the root `CMakeLists.txt` (or the appropriate sub-CMake):

```cmake
option(STA_USE_LIBUV "Enable libuv async I/O (required for Spike 005)" OFF)

if(STA_USE_LIBUV)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBUV REQUIRED libuv)
    # libuv include dirs and link flags are now in LIBUV_INCLUDE_DIRS,
    # LIBUV_LIBRARIES, LIBUV_LDFLAGS_OTHER
endif()
```

Add to `tests/CMakeLists.txt`:

```cmake
if(STA_USE_LIBUV)
    add_executable(test_io_spike
        test_io_spike.c
        ../src/io/io_spike.c
        ../src/scheduler/scheduler_spike.c)
    target_include_directories(test_io_spike PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${LIBUV_INCLUDE_DIRS})
    target_compile_options(test_io_spike PRIVATE
        -fsanitize=thread
        ${LIBUV_CFLAGS_OTHER})
    target_link_options(test_io_spike PRIVATE -fsanitize=thread)
    target_link_libraries(test_io_spike PRIVATE ${LIBUV_LIBRARIES})
    add_test(NAME io_spike COMMAND test_io_spike)

    # Benchmark target (no TSan, -O2, for accurate latency numbers)
    add_executable(bench_io_spike
        test_io_spike.c
        ../src/io/io_spike.c
        ../src/scheduler/scheduler_spike.c)
    target_include_directories(bench_io_spike PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${LIBUV_INCLUDE_DIRS})
    target_compile_definitions(bench_io_spike PRIVATE STA_BENCH=1)
    target_compile_options(bench_io_spike PRIVATE -O2 ${LIBUV_CFLAGS_OTHER})
    target_link_libraries(bench_io_spike PRIVATE ${LIBUV_LIBRARIES})
    # Not registered as a ctest target — run manually for latency numbers.
endif()
```

Invoke the spike build with:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DSTA_USE_LIBUV=ON
cmake --build build
cd build && ctest --output-on-failure
```

Run the benchmark separately (no TSan overhead):

```bash
./build/tests/bench_io_spike
```

TSan and the benchmark target are separate builds for the same reason as in the
scheduler spike (spike-003): TSan instrumentation adds latency that would make
the wake latency numbers meaningless.

---

## Constraints

- All spike files must carry a prominent comment at the top:
  `SPIKE CODE — NOT FOR PRODUCTION`
- Do not modify `include/sta/vm.h` during this spike
- Do not modify any existing ADR (007, 008, 009, 010)
- libuv types (`uv_loop_t`, `uv_timer_t`, `uv_tcp_t`, all `uv_*_t`) must not
  appear in any header outside `src/io/`. Specifically, they must not appear in
  `io_spike.h`. They belong only in `io_spike.c`.
- `src/io/io_spike.h` and `.c` may include `src/scheduler/scheduler_spike.h`
  for `sta_sched_push` — intentional inter-spike dependency; all are spike code.
- Tests may not include `src/` headers directly except through the spike
  header chain — no back-channel to `include/sta/vm.h`.
- Write ADR 011 only after the test binary exists, `ctest` passes, and all
  measurements (wake latency, density table, `sizeof(uv_loop_t)`) are in hand.
  Do not write ADR 011 before that.
- The TSan gate applies to the no-blocking-scheduler test (test 3) and the TCP
  loopback tests (tests 5 and 6). A data race on any of these paths is a
  correctness failure, not a performance issue.
- `memory_order_seq_cst` is permitted only if there is no weaker ordering that
  is provably correct. Any use of `seq_cst` beyond what ADR 009 already
  justified must be explicitly documented in the spike doc and in ADR 011.
  For the `io_state` field: `release`/`acquire` is sufficient (see Q4).

---

## Open questions this spike deliberately does not answer

These are real questions identified during spike design. They are deferred to
future ADRs so they do not block this spike.

1. **File I/O (`uv_fs_t`).**  File I/O follows the same suspend/resume pattern
   as timers and TCP. The primary difference is that `uv_fs_t` operations are
   dispatched to a thread pool internal to libuv. The actor suspension model
   is identical. Defer to Phase 2.

2. **DNS resolution (`uv_getaddrinfo`).** Also dispatched to libuv's thread pool.
   Same actor suspension model. Defer to Phase 2.

3. **Lock-free I/O request queue.** The spike uses a mutex-protected FIFO for
   scheduler-thread-to-I/O-thread communication. Under high I/O request rates
   this mutex becomes a bottleneck. Replace with a lock-free MPSC queue
   (the `STA_MpscList` from ADR 008 is a natural candidate) in Phase 2.

4. **Handle pooling.** The spike allocates each `uv_timer_t` and `uv_tcp_t`
   from the heap and frees them in the close callback. Under high I/O rates,
   this allocation pattern stresses `malloc`. A pre-allocated handle pool
   is a Phase 2 performance concern.

5. **Mailbox overflow and I/O backpressure (§9.4).** If Model B (synthetic
   completion message) is chosen, mailbox overflow can affect I/O completion
   delivery. Under Model A (direct deque push), backpressure must be managed
   at the application layer (actors must not initiate more I/O than the
   scheduler can handle). The integration of §9.4 bounded mailbox policy
   with the I/O layer is a Phase 2 design question.

6. **`uv_loop_t` sharing vs. isolation.** This spike uses one `uv_loop_t`
   shared across all I/O-using actors. An alternative is to use a pool of
   loops, one per I/O thread (if the I/O thread pool is expanded in Phase 2).
   The single-loop design is simpler and correct for Phase 0; revisit when
   file I/O and DNS are added.

7. **I/O actor wakeup on scheduler thread migration.** Under the work-stealing
   scheduler, an actor that was suspended on thread N may be re-enqueued on
   thread M (because `sta_sched_push` chooses the least-loaded thread). The
   actor's frame stack and nursery heap are not pinned to a thread. The spike
   must not assume any affinity between an actor and the thread that suspended
   it. Verify in the TCP tests that the resumed actor is correctly executable
   regardless of which scheduler thread picks it up.

8. **`ask:` futures and I/O timeout.** The architecture's `ask:` primitive
   involves a future that can time out. The libuv timer primitive is a natural
   implementation for `ask:` timeouts. The integration of the `ask:` future
   protocol with the I/O timer path is a Phase 2 design question (deferred
   in ADR 009 open question 4).

---

## What ADR 011 must record

After running the spike, write ADR 011 covering:

- **libuv integration architecture chosen** (Option A) with rationale and explicit
  rejection of Options B and C (with measured `sizeof(uv_loop_t)` for C).
- **Actor suspension/resumption protocol**: complete field-by-field description
  of `STA_ActorIo`, including `io_state` and `io_result`, memory orderings, and
  the invariants that prevent double-scheduling of a suspended actor.
- **Re-enqueue model chosen** (Model A or B) with explicit rationale, including
  the tradeoff statement: direct deque push resumes mid-message vs. synthetic
  completion message resumes at message boundary.
- **Measured wake latency**: median and p99 from the timer benchmark (10,000
  samples, 1 ms timer). Compare to ADR 009 cond-signal baseline (5,958 ns median,
  7,875 ns p99). State whether the I/O-thread-to-scheduler path introduces
  meaningful additional latency.
- **Scheduler-thread CPU utilisation** during I/O actor suspension: total reductions
  executed by the compute actor in the 100 ms window from the compute-under-I/O test.
- **Measured `sizeof(uv_loop_t)`**: data point for Option C rejection.
- **Actor density table**: `sizeof(STA_ActorIo)`, total creation cost, which ADR 010
  scenario was reached (Low/Mid/High/At limit/Breach). If the 300-byte target is
  breached, the justification must appear in this ADR per CLAUDE.md.
- **No-GIL memory ordering audit**: every shared field touched by the I/O path —
  `io_state`, `io_result`, `sched_flags` — with ordering used and justification.
  TSan clean confirmation.
- **Required changes to `STA_ActorRevised`** (the spike's `STA_ActorIo` documents
  the minimum fields needed): `io_state` and `io_result` must be added to the
  permanent `STA_Actor` struct before Phase 2.
- **TCP loopback test result**: round-trip correctness, whether both actors
  suspended and resumed correctly, any TSan findings.
- **Open questions explicitly closed**: libuv integration architecture (this ADR
  closes the Phase 2 I/O question from spike-003 open question 3 and ADR 009
  open question 5).
