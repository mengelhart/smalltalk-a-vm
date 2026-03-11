# ADR 011 — Async I/O Architecture via libuv

**Status:** Accepted
**Date:** 2026-03-11
**Spike:** `docs/spikes/spike-005-async-io.md`
**Depends on:** ADR 008 (mailbox), ADR 009 (scheduler), ADR 010 (frame layout)

---

## Decision

Async I/O in Smalltalk/A uses **Option A: a dedicated I/O thread** running
`uv_run(uv_loop, UV_RUN_DEFAULT)`. Scheduler threads never call `uv_run()` or
any blocking syscall. I/O requests are dispatched to the I/O thread via
`uv_async_send()` after being placed in a mutex-protected FIFO. Completion
callbacks fire on the I/O thread and re-enqueue the suspended actor via the
`sta_sched_push` path established in ADR 009.

The I/O completion re-enqueue model is **Model A: direct deque push**. The
libuv callback calls `sta_sched_push(actor)` directly — no message allocation
on the completion path.

The `STA_Actor` struct requires **two new fields** for I/O suspension:
`_Atomic uint32_t io_state` (4 bytes) and `int32_t io_result` (4 bytes), for a
total addition of **8 bytes** — the Low scenario from ADR 010's projection table.

---

## Measured results (arm64 M4 Max, Apple clang 17, C17, TSan, `-g`)

All measurements from `test_io_spike` via `ctest`.

### Actor density

| Component | Bytes |
|---|---|
| `STA_ActorIo` (production projection: ADR 010 baseline + I/O fields) | **144** |
| Initial nursery slab | 128 |
| Actor identity object (0-slot header) | 16 |
| **Total creation cost** | **288** |
| Target | ~300 |
| Headroom | **12** |
| ADR 010 scenario | **Low (+8 bytes)** |

`sizeof(STA_IoSpikeActor)` (working spike struct) = 96 bytes.
`_Static_assert(sizeof(STA_ActorIo) == 144)` passes at compile time.

### Option C rejection: `sizeof(uv_loop_t)`

`sizeof(uv_loop_t)` = **1,072 bytes** on arm64 macOS with Homebrew libuv.
Embedding a `uv_loop_t` per actor would add 1,072 bytes, immediately breaching
the 300-byte density target by 772 bytes. Option C is rejected on measured
data, not estimation.

### No-blocking-scheduler proof

Test 3 (one scheduler thread, one 50 ms I/O actor, one compute actor):

- Compute actor executed **13,150 times** during the 50 ms I/O wait.
- Compute actor ran 0 times means the scheduler thread was blocked — not observed.

Test 6 (TCP loopback under compute load):

- Compute actor executed **82 times** during the TCP round trip.
- TCP round-trip completed correctly with no scheduler thread blocking.

### Wake latency

The timer wake latency benchmark (`STA_BENCH` build) requires explicit
instrumentation of `sta_io_resume_actor` to capture the callback timestamp.
Full latency distribution (10,000 samples, 1 ms timer) was not captured in
this spike cycle. The structural path — I/O thread calls `sta_io_sched_push`,
which calls `sta_notif_wake` (a `pthread_cond_signal`) — is identical to the
ADR 009 scheduler wake path. ADR 009 baseline for that path:
**median 5,958 ns / p99 7,875 ns**. The I/O completion path adds one
FIFO-drain iteration and one `atomic_store_explicit` before the
`pthread_cond_signal`, introducing negligible additional latency
(< 100 ns estimated). Exact measurement is deferred to Phase 2 benchmarking
when the production path is implemented.

### TSan

All tests pass TSan clean. No data races detected on `io_state`, `io_result`,
`sched_flags`, the FIFO mutex path, or the scheduler run-queue mutex.

---

## libuv integration architecture

### Option A chosen: dedicated I/O thread

```
Scheduler Thread                 I/O Thread (libuv)
────────────────                 ──────────────────
actor->run_fn()
  → sta_io_timer_start()
      lock fifo_mutex
      enqueue {TIMER, actor, delay_ms}
      unlock fifo_mutex
      uv_async_send(&io->wakeup)   ──→  wakeup_cb() fires
  → sets sched_flags=SUSPENDED         drain FIFO
  → sets io_state=PENDING              uv_timer_start(handle, delay_ms, cb)
  → returns immediately           uv_run() blocks/polls...
pick next actor                   [delay_ms elapses]
execute_actor(B)                  timer_cb(handle)
  ...                               actor->io_result = 0
                                    io_state ← STA_IO_IDLE (release)
                                    sta_io_sched_push(actor)
                                      → sets sched_flags=RUNNABLE (release)
                                      → lock run_mutex, enqueue, unlock
                                      → sta_notif_wake (pthread_cond_signal)
Scheduler wakes
execute_actor(A)  ←── resumes here
```

**Lifecycle:**

- `sta_io_init()` — `uv_loop_init`, allocates a sentinel `uv_async_t` to keep
  the loop alive, spawns the I/O thread, waits on a startup barrier
  (`pthread_cond_t`) until `uv_run` is executing before returning.
- `sta_io_destroy()` — sets `io->running = 0` (release), calls
  `uv_async_send(&io->wakeup)`, joins the I/O thread, calls `uv_walk` to
  close any remaining handles, runs `uv_run(UV_RUN_DEFAULT)` until all close
  callbacks fire, then calls `uv_loop_close`.

### Option B rejected: scheduler thread doubles as I/O poller

- Creates systematic scheduling imbalance: the polling thread is slower than
  its peers.
- Poll latency bounded by quantum duration (potentially milliseconds) rather
  than I/O completion time.
- Not prototyped. Rejected on architectural grounds.

### Option C rejected: per-actor `uv_loop_t`

- `sizeof(uv_loop_t)` = **1,072 bytes** (measured). Embedding in `STA_Actor`
  would add 1,072 bytes — 772 bytes over the 300-byte target.
- Even as a separate allocation (pointer in actor), the I/O thread pool
  scheduling and per-loop fd-limit implications are unnecessary complexity for
  Phase 2.
- Not prototyped. Rejected on density grounds (measured data above).

---

## Actor suspension and resumption protocol

### Fields added to `STA_Actor`

```c
/* Minimum permanent additions (io_spike.h STA_ActorIo documents these): */
_Atomic uint32_t io_state;   /* 4 bytes — STA_IO_IDLE (0) or STA_IO_PENDING (1) */
int32_t          io_result;  /* 4 bytes — completion result (0 = ok, < 0 = errno) */
```

`io_result` does not need to be atomic because it is always written before the
release store on `io_state` (sequenced-before). Any scheduler thread that
observes `io_state == STA_IO_IDLE` with acquire semantics is guaranteed to see
the completed `io_result` write.

### Scheduler flag extension

```c
#define STA_SCHED_SUSPENDED  0x04u   /* actor waiting for I/O — do NOT re-enqueue */
```

Extends the `sched_flags` constants from ADR 009
(`STA_SCHED_NONE`, `STA_SCHED_RUNNABLE`, `STA_SCHED_RUNNING`).

### Full suspension sequence (scheduler thread)

1. Actor's `run_fn` calls `sta_io_timer_start()` / `sta_io_tcp_*()`.
2. The helper calls `suspend_actor(actor)`:
   - `atomic_store_explicit(&actor->sched.sched_flags, STA_SCHED_SUSPENDED, memory_order_release)`
   - `atomic_store_explicit(&actor->io_state, STA_IO_PENDING, memory_order_release)`
   Both stores use release so the I/O thread's subsequent acquire reads are
   properly ordered.
3. The helper enqueues an `STA_IoRequest` in `io->fifo` (under `fifo_mutex`)
   and calls `uv_async_send(&io->wakeup)` — non-blocking.
4. Returns immediately to the scheduling loop.
5. The scheduling loop reads `sched_flags == STA_SCHED_SUSPENDED` after
   `run_fn` returns and does **not** re-enqueue the actor.

### Full resumption sequence (I/O thread)

1. libuv callback fires when the operation completes.
2. Callback writes `actor->io_result = result` (plain store — ordered by step 3).
3. `atomic_store_explicit(&actor->io_state, STA_IO_IDLE, memory_order_release)` —
   this release store is the happens-before fence for `io_result` and any other
   fields written before it (e.g., `tcp_handle`).
4. `sta_io_sched_push(actor)`:
   - `atomic_store_explicit(&actor->sched.sched_flags, STA_SCHED_RUNNABLE, memory_order_release)`
   - Enqueues in the FIFO run queue (under `run_mutex`).
   - Calls `sta_notif_wake` (`pthread_cond_signal`) to wake the scheduler thread.
5. Scheduler thread wakes, dequeues the actor, sets `sched_flags = RUNNING`,
   and calls `actor->run_fn(actor)`. The actor reads `io_result` and
   `tcp_handle` (for TCP operations) — both are fully visible by the
   release/acquire pair on `io_state`.

### Race between I/O callback and scheduling loop

The scheduling loop checks `sched_flags` after `run_fn` returns:

- `SUSPENDED` — I/O callback has not yet fired; callback will re-enqueue. Do nothing.
- `RUNNABLE` — I/O callback fired and re-enqueued before the loop read the flag. Do nothing (double-enqueue guard).
- `RUNNING` — normal compute return; retire or re-enqueue based on `max_runs`.

This three-way check prevents double-scheduling regardless of whether the I/O
completes before or after the scheduling loop reads `sched_flags`.

---

## Re-enqueue model: Model A (direct deque push)

**Chosen.** The libuv callback calls `sta_sched_push(actor)` directly. Three
lines: write result, release-store `io_state`, push to run queue.

```c
void sta_io_resume_actor(STA_IoSpikeActor *actor, int32_t result) {
    actor->io_result = result;
    atomic_store_explicit(&actor->io_state, STA_IO_IDLE, memory_order_release);
    sta_io_sched_push(actor->io->sched, actor);
}
```

**Model B rejected** (synthetic completion message via MPSC mailbox):

- One message allocation per I/O completion; the per-actor nursery allocator
  is not thread-safe (ADR 008 per-actor heap isolation). The I/O thread would
  need either pre-allocated buffers (adds actor struct fields) or a global pool
  (global lock on the critical completion path).
- Additional latency on the completion path — works against the design goal of
  low I/O wake latency.
- No architectural benefit in Phase 2 over Model A given that the actor is
  always suspended at the top of its execution context (not mid-message) when
  it initiates I/O.

**Resume point note:** under Model A, an actor resumes mid-message rather than
at a clean message boundary. This is valid in the spike because actors are
always at the top of their execution context when they call a `sta_io_*`
function. The production interpreter must ensure that `sta_io_*` calls are
only permitted at well-defined suspension points (top-of-message or explicit
yield sites). This is a Phase 2 protocol definition concern, not an ADR 011
correctness gap.

---

## Spike scheduler: Chase-Lev LIFO replaced with FIFO

The io spike introduces `STA_IoSched` — a single-thread FIFO scheduler that
replaces the Chase-Lev deque for spike test purposes. The reason is
**single-thread starvation avoidance**, not a production scheduler change:

- The Chase-Lev deque is a LIFO owner-pop structure. With a single scheduler
  thread, a compute actor that repeatedly re-enqueues itself at the bottom
  (hot end) monopolises the thread; a re-enqueued I/O actor arriving at the
  bottom is popped before any pre-existing actor higher in the deque.
- A mutex-protected FIFO (using `actor->sched.next_runnable` as the intrusive
  link) gives every actor fair round-robin access.

**This is a spike-only artefact.** The production work-stealing scheduler
(ADR 009) uses Chase-Lev across N threads. With multiple threads, a compute
actor that monopolises its owner thread's deque can be stolen by other threads;
starvation does not arise. The FIFO in the spike is not a production scheduler
change.

---

## Memory ordering audit (no-GIL)

| Field | Writer | Ordering | Reader | Ordering | Justification |
|---|---|---|---|---|---|
| `io_state` IDLE→PENDING | scheduler thread | `release` | I/O thread (if checked) | `acquire` | Orders all prior writes visible on scheduler thread to I/O thread |
| `io_state` PENDING→IDLE | I/O thread | `release` | scheduler thread | `acquire` | Orders `io_result` and `tcp_handle` writes to scheduler thread |
| `io_result` | I/O thread | plain store | scheduler thread | — (ordered by `io_state` acquire) | Sequenced-before the release store on `io_state` |
| `tcp_handle` | I/O thread | plain store | scheduler thread | — (ordered by `io_state` acquire) | Same as `io_result` |
| `sched_flags` SUSPENDED | scheduler thread | `release` | scheduling loop | `acquire` | Prevents re-enqueue of suspended actor |
| `sched_flags` RUNNABLE | I/O thread (via `sta_io_sched_push`) | `release` | scheduling loop | `acquire` | Double-enqueue guard in scheduling loop |
| `sched_flags` RUNNING | scheduling loop | `release` | — | — | Records execution start |
| `run_count` | scheduling loop | `acq_rel` fetch_add | test helpers (wait loop) | `acquire` | Test infrastructure only |

No `memory_order_seq_cst` is used. All orderings are `release`/`acquire` or
`relaxed` (for initialisation). TSan confirms no data races.

---

## Scheduler-thread non-blocking proof

**Structural argument:**

1. Scheduler threads call only: `sta_io_timer_start`, `sta_io_tcp_listen`,
   `sta_io_tcp_connect`, `sta_io_tcp_read`, `sta_io_tcp_write`. Each function:
   a. Enqueues an `STA_IoRequest` (lock `fifo_mutex`, insert, unlock).
   b. Calls `uv_async_send()` — documented non-blocking and thread-safe by libuv;
      internally a single write to a local kernel buffer (pipe/eventfd on macOS).
   c. Returns immediately.
   No scheduler thread calls `uv_run()`, `read()`, `recv()`, `accept()`, or any
   other blocking syscall. Only the dedicated I/O thread calls `uv_run()`.

2. The `fifo_mutex` critical section is bounded: one pointer insertion (< 100 ns).
   This is not I/O blocking. The lock-free MPSC path (Phase 2) eliminates it.

**Empirical confirmation (Test 3):**
13,150 compute-actor executions during a 50 ms I/O wait. Zero executions would
mean the scheduler thread was blocked. The measured value is consistent with
a full 50 ms of unimpeded compute scheduling (scheduler quantum ~3.8 µs → ~13,000
quanta in 50 ms).

---

## Consequences

- **`STA_Actor` must add two fields** before Phase 2:
  - `_Atomic uint32_t io_state` (4 bytes) — `STA_IO_IDLE` / `STA_IO_PENDING`
  - `int32_t io_result` (4 bytes) — completion result
  The `STA_ActorIo` struct in `src/io/io_spike.h` documents the exact layout.
  A `_Static_assert(sizeof(STA_ActorIo) == 144)` in `io_spike.c` locks this.

- **`STA_SCHED_SUSPENDED = 0x04u`** must be added to the `sched_flags` constants
  alongside the existing `STA_SCHED_NONE`, `STA_SCHED_RUNNABLE`, `STA_SCHED_RUNNING`
  from ADR 009.

- **ADR 010 density headroom consumed.** 20 bytes of headroom entering Spike 005;
  8 bytes consumed. 12 bytes remain. The next consumer of `STA_Actor` fields
  (Phase 1 bootstrap, GC, or Phase 2 Phase 2 supervision state) must track against
  this margin. No further additions to `STA_Actor` without a new ADR.

- **`STA_SpikeActor` is not the production scheduler struct.** The io spike
  introduces `STA_IoSched` with a FIFO run queue for single-thread fairness. The
  production scheduler (ADR 009 Chase-Lev + work-stealing) is unchanged. The FIFO
  is a spike-only artefact.

- **File I/O (`uv_fs_t`) and DNS (`uv_getaddrinfo`) deferred to Phase 2.** They
  follow the same actor suspension model as timers and TCP. No architectural
  information is added beyond what this spike already validates.

- **Lock-free I/O request queue deferred to Phase 2.** The spike uses a
  `pthread_mutex_t`-protected FIFO for scheduler-thread → I/O-thread requests.
  Under high I/O request rates this mutex is a bottleneck. Replace with
  `STA_MpscList` (ADR 008) in Phase 2.

- **Handle pooling deferred to Phase 2.** The spike allocates each `uv_timer_t`,
  `uv_tcp_t`, and `uv_write_t` from `malloc` and frees them in libuv close
  callbacks. A pre-allocated handle pool is a Phase 2 performance concern.

- **ADR 009 open question 5 is closed.** The I/O completion re-enqueue interface
  is `sta_sched_push` (Model A, direct deque push). No protocol change to the
  ADR 009 scheduler interface is required.

- **spike-003 open question 3 is closed.** libuv is confirmed as the async I/O
  substrate with a dedicated I/O thread (Option A). No scheduler-thread polling.

---

## Open questions (deferred)

1. **Resume point protocol for mid-message suspension.** Under Model A, the actor
   resumes mid-message. The production interpreter must define which bytecode
   positions are valid I/O suspension points. Decide before Phase 2 I/O primitives.

2. **I/O backpressure (§9.4).** Bounded mailboxes govern actor-to-actor messages.
   Under Model A, I/O completions bypass the mailbox. The integration of §9.4
   overflow policy with the I/O completion path is a Phase 2 design question.

3. **`uv_loop_t` sharing vs. per-I/O-thread loops.** The spike uses one shared
   `uv_loop_t`. If the I/O thread pool is expanded in Phase 2 (for file I/O),
   revisit whether a single loop or one-loop-per-thread is appropriate.

4. **I/O actor thread affinity.** Under work-stealing, an actor suspended on
   thread N may resume on thread M. The spike validates this is structurally
   safe (no actor-to-thread pinning). Production: verify no performance
   regression from cache-line migration on resume under realistic workloads.

5. **`ask:` futures and I/O timeouts.** ADR 009 open question 4 deferred this.
   The libuv timer primitive is the natural `ask:` timeout implementation.
   Design the integration before Phase 2 `ask:` semantics are finalised.
