# Spike: Work-Stealing Scheduler and Reduction-Based Preemption

**Phase 0 spike — scheduler architecture**
**Status:** Ready to execute
**Related ADRs:** 007 (object header layout), 008 (mailbox and message copy)
**Produces:** ADR 009 (scheduler design)

---

## Purpose

This spike answers the foundational questions of the concurrent scheduler: how
scheduler threads are created and terminated, how runnable actors are
distributed across threads, how an idle thread wakes when work arrives, and
how the reduction counter enforces preemption without a GIL.

The architecture document (§9.1) specifies the shape: one thread per core,
work-stealing run queues, reduction-based preemption. ADR 008 deferred the
scheduler notification question explicitly (open question #1). This spike
either validates the architecture's sketch or replaces it with something
better — and records concrete numbers before any interpreter or scheduler
code is written.

**This spike does not build the interpreter, the GC, or the full actor
lifecycle.** It builds the minimal C needed to measure scheduler throughput,
validate lock-freedom under TSan, and determine the correct notification
mechanism. Permanent implementation follows ADR 009.

---

## Background: the sketch from §9.1 and current stubs

The architecture document (§9.1) specifies:

- **One OS thread per CPU core** (sysconf(_SC_NPROCESSORS_ONLN)), each
  running an independent scheduling loop
- **Work-stealing:** idle threads steal runnable actors from busy threads'
  run queues
- **Reduction-based preemption:** each actor gets a budget of reductions
  per quantum; exhaustion returns it to the run queue tail

The current actor stub (`src/vm/actor_spike.h`) contains:

```c
_Atomic uint32_t reductions;  /* 4 bytes — budget counter */
uint32_t         sched_flags; /* 4 bytes — runnable, suspended, etc. */
```

These are placeholders. The spike must add scheduler-owned fields (run-queue
linkage, thread affinity hint), measure the resulting `sizeof(STA_Actor)`,
and confirm the ~300-byte density target still holds.

ADR 008 established:
- `STA_Actor` with `STA_MpscList` embedded: ~120 bytes
- Per-actor creation cost: ~264 bytes (128-byte nursery + 16-byte identity object)
- Headroom to target: 300 − 264 = **36 bytes**

The scheduler fields added in this spike must fit within that headroom, or
the overage must be justified in ADR 009.

---

## Questions to answer

Each question requires a concrete answer — not a deferral — before ADR 009
is written. The spike test binary must demonstrate the answer.

---

### Q1: Thread-per-core lifecycle

Spawn one scheduler thread per `sysconf(_SC_NPROCESSORS_ONLN)` core using
`pthread_create`. Each thread runs an independent scheduling loop. The spike
must validate clean startup and shutdown:

- All threads must start before any scheduling work begins (a barrier is
  appropriate — `pthread_barrier_t` or a counting latch with `_Atomic int`)
- Shutdown must be a cooperative signal: a `_Atomic bool running` flag that
  each thread checks at the top of its loop
- All threads must join cleanly — no detached threads, no cancellation,
  no `pthread_kill`
- The main thread must block until all scheduler threads have joined

**TSan gate:** run the full lifecycle (spawn → work loop → shutdown → join)
under `clang -fsanitize=thread`. TSan clean is a hard pass/fail requirement.
A lifecycle that fails TSan does not proceed to ADR 009.

**Measure:** time from `pthread_create` of the first thread to all threads
reporting ready (via the barrier). This is the scheduler startup cost — record
it in ADR 009.

---

### Q2: Per-thread run queue (work-stealing deque)

Each scheduler thread owns a local double-ended queue of runnable
`STA_Actor *` pointers. The owning thread pushes and pops from the bottom.
Stealing threads take from the top.

Two variants must be implemented and validated:

**Variant A — Chase-Lev dynamic array deque**

The classic work-stealing deque due to Chase and Lev (2005), "Dynamic
Circular Work-Stealing Deque." The owning thread maintains a fixed-size
(for this spike) circular array plus `top` and `bottom` indices.

- `push(x)`: write `x` to `buf[bottom % size]`, increment `bottom` with
  release store. Single write, no CAS.
- `pop()` (owning thread only): decrement `bottom`, load `top`. If
  `bottom > top`, the slot is ours — load and return it. If
  `bottom == top`, race with a stealer: CAS on `top` to resolve. Restore
  `bottom` on failure.
- `steal()` (any other thread): load `top`, load `buf[top % size]`, CAS
  `top` to `top + 1`. Single CAS. Retry on failure.

Memory ordering: `bottom` stores use `memory_order_release`; stealer loads
on `top` use `memory_order_acquire`. The CAS on `top` uses
`memory_order_seq_cst` — this is the one location where sequentially
consistent ordering is justified: the CAS arbitrates between concurrent
stealers. Document this explicitly.

For the spike, use a fixed-capacity array of 1024 slots. The dynamic
resize path is a Phase 1 concern. Assert in the test that the queue never
exceeds 512 entries (half capacity) under the synthetic load to confirm
no resize is needed.

**Variant B — Fixed-capacity ring buffer deque**

A simpler bounded deque using a power-of-two ring buffer with atomic
`top` and non-atomic `bottom` (owner-only).

- `push(x)`: write `x` to `buf[bottom & mask]`, increment `bottom` with
  release store. Check `bottom - top < capacity` before writing; return
  failure if full (the owning thread can handle overflow by deferring the
  actor to the global queue or dropping — record the policy chosen).
- `pop()` (owning thread): same protocol as Variant A but simpler since
  capacity is always fixed and no resize is possible.
- `steal()`: same as Variant A — CAS on `top`.

Memory ordering: same as Variant A. `memory_order_seq_cst` on the steal CAS
is required for the same reason.

**Correctness gate:** both variants must pass `clang -fsanitize=thread` under
the steal stress test: N scheduler threads, M actors per thread initially,
random cross-thread sends causing steal events. TSan clean is hard pass/fail.

**Measure for both variants:**

1. **Steal contention:** number of CAS failures per 1000 steal attempts
   under the synthetic load (N threads, N × 100 actors, 10 000 send events).
   Lower is better.

2. **Throughput:** total actors scheduled per second across all threads.
   Report for N = 1, 2, 4, and as many cores as the test machine has.
   Use `clock_gettime(CLOCK_MONOTONIC_RAW)`.

3. **Steal latency:** time from an actor being pushed to another thread's
   queue to that actor being dequeued (via steal) on the idle thread. Measure
   under the condition where one thread is the sole producer and one adjacent
   thread is idle. Report median and p99.

**Recommend one variant** with rationale for ADR 009. The recommendation must
be based on the measured numbers, not prior preference.

---

### Q3: Scheduler notification — ADR 008 open question #1

When a sender enqueues a message into a previously-empty mailbox, the target
actor becomes runnable. If the target actor's home scheduler thread is sleeping
(its run queue was empty), it must wake up. How?

Three options must be implemented and measured:

**Option 1: `pthread_cond_signal` per scheduler thread**

Each scheduler thread owns a `pthread_mutex_t` + `pthread_cond_t` pair.
When the run queue transitions from empty to non-empty, the enqueuer calls
`pthread_mutex_lock` + `pthread_cond_signal` + `pthread_mutex_unlock`. The
sleeping scheduler thread is in `pthread_cond_wait`.

Advantage: standard, portable, integrates with C11/C17 threading.
Concern: the lock acquisition on the notification path adds latency on every
send to a sleeping thread. Measure this.

**Option 2: Pipe write (one-byte wakeup)**

Each scheduler thread owns a read end of a `pipe(2)` file descriptor. The
scheduling loop blocks on `select(2)` or `poll(2)` over the pipe read end
with a timeout. When the run queue transitions from empty to non-empty, the
enqueuer writes one byte to the write end.

Advantage: integrates naturally with the libuv event loop (Phase 2) — the
I/O poller and the scheduler notification share the same `poll` call.
Note: on macOS, `kqueue` with `EVFILT_USER` is an eventfd equivalent; the
spike may use either a real pipe or `kqueue` with `EVFILT_USER`. Use a pipe
for portability; note the alternative in the spike doc.
Concern: pipe write involves at least one syscall per notification. Measure
this cost under light load (single send to idle scheduler).

**Option 3: Spinning with exponential backoff + periodic yield**

The scheduler thread never sleeps. Instead, it spins on its run queue with
exponential backoff: check the queue, if empty spin for 2^k nanoseconds
(starting at 1 ns, capped at ~1 ms), then yield with `sched_yield()`. No OS
sleep, no condvar, no pipe.

Advantage: zero wakeup latency — the scheduler sees the new actor at the
next spin iteration. No syscall overhead.
Concern: wastes CPU on idle cores. With one scheduler thread per core, an
idle actor system burns 100% CPU on all cores. This is likely unacceptable
for a development-mode VM. Measure the wakeup latency and CPU burn at idle.

**Measure for all three options:**

**Wake latency:** time from the enqueuer pushing an actor into a run queue
to the scheduler thread executing the first reduction of that actor. Measured
under light load: one idle scheduler thread, one sender thread, 100 000 single
sends, scheduler thread starts from a fully-sleeping state. Report median and
p99. Use `clock_gettime(CLOCK_MONOTONIC_RAW)` with nanosecond resolution.

**Warmup:** discard the first 1000 samples. Measure the remaining 99 000.
Sort the array to compute exact percentiles.

**Recommend one option** for ADR 009. If Option 3 is fastest but burns CPU at
idle, note the trade-off explicitly and state whether a hybrid (spin briefly,
then fall back to Option 1 or 2) is worth evaluating in a follow-up spike.
The recommendation must not be left implicit — ADR 009 closes this open
question from ADR 008.

---

### Q4: Reduction-based preemption

Each actor is granted `STA_REDUCTION_QUOTA` reductions per scheduling quantum.
The scheduler decrements the counter on each simulated "instruction." When
the counter reaches zero, the actor is suspended and returned to the run
queue tail. The scheduler then picks the next actor from its queue.

```c
/* Tunable constant — defined in scheduler_spike.h */
#define STA_REDUCTION_QUOTA 1000u
```

**No object system yet.** Model execution as a tight loop:

```c
static void execute_actor(STA_Actor *actor) {
    while (actor->reductions > 0) {
        /* Simulate one reduction: decrement the counter. */
        actor->reductions--;
        /* Real interpreter will do bytecode dispatch here. */
    }
}
```

The scheduler loop picks an actor from its local queue, restores
`reductions = STA_REDUCTION_QUOTA`, calls `execute_actor`, then either
re-enqueues the actor (it is still runnable) or parks it (mailbox empty).

**Measure:**

1. **Scheduling quantum duration:** time to execute `STA_REDUCTION_QUOTA`
   reductions of the stub `execute_actor` function on arm64 M4 Max, compiled
   with `-O2`. Report in wall-clock nanoseconds. Use
   `clock_gettime(CLOCK_MONOTONIC_RAW)` around the `execute_actor` call.
   Discard the first 1000 quanta; measure 100 000. Report median and p99.

2. **Maximum scheduling latency:** with N scheduler threads each running a
   steady-state actor workload, what is the longest observed gap between
   one actor's preemption and the next actor beginning on the same thread?
   Run for 10 seconds of wall-clock time. Report the maximum observed
   latency in microseconds.

These two numbers go directly into ADR 009. They set the baseline for
reasoning about tail latency and the `ask:` future timeout floor.

---

### Q5: No-GIL correctness audit

List every shared data structure touched by the scheduler and state the
memory ordering used for each. The audit must cover:

**a. The run queue deques (per-thread)**

- `bottom` index: written only by owning thread. Is a plain `uint32_t` with
  a release store on push/pop sufficient? Or is `_Atomic uint32_t` required
  for the stealer's visibility? State the C17 standard's requirements
  explicitly.
- `top` index: read and CAS'd by stealers; read by owner. Must be
  `_Atomic uint32_t`. Steal CAS: `memory_order_seq_cst`. Owner read:
  `memory_order_acquire`. State why seq_cst is justified here and not
  elsewhere.
- Array slots: written by owner (push), read by stealer (steal). The
  release store on `bottom` provides the write barrier; the acquire load
  on `top` provides the read barrier. State why no per-slot atomic is
  needed.

**b. The notification mechanism (whichever is chosen)**

State which objects are shared between enqueuer and scheduler thread,
and what ordering prevents the "scheduler sleeps after enqueuer checks
queue empty but before enqueuer signals" race. This is the classic
condvar lost-wakeup problem — it must be closed explicitly.

**c. Actor state flags (`sched_flags`)**

The `sched_flags` field in `STA_Actor` tracks whether the actor is
currently in a run queue (to prevent double-scheduling). It is written
by the scheduler (when dequeuing) and by any enqueuer (when transitioning
from empty mailbox to non-empty). Must be `_Atomic uint32_t`. State the
ordering used for the check-and-set transition and why it prevents
double-enqueueing.

**d. The global actor table (if any)**

If the spike introduces any structure mapping actor IDs to `STA_Actor *`
pointers (e.g. for routing a message to a specific actor by ID), state its
concurrency model. If no such table exists in the spike, state that
explicitly.

**TSan gate:** the full multi-thread stress test (Q2's steal stress test
plus Q3's wake latency benchmark) must complete TSan clean. No race may be
suppressed with annotations unless explicitly justified in the spike doc.

---

### Q6: Actor density checkpoint

After embedding the scheduler fields into `STA_Actor`, re-measure
`sizeof(STA_Actor)`. The fields to add are:

```
struct STA_Actor *next_runnable;   /* 8 bytes — intrusive run-queue link     */
uint32_t          home_thread;     /* 4 bytes — affinity hint (thread index) */
/* reductions and sched_flags already exist in actor_spike.h                 */
```

The `reductions` and `sched_flags` fields are already in the stub. The
spike replaces the stub's `_Atomic uint32_t reductions` with the scheduler's
authoritative version (same type, same size — no change). `sched_flags`
becomes `_Atomic uint32_t` (was `uint32_t` — 4-byte size preserved, 4-byte
alignment preserved, no struct size change).

Expected impact: +12 bytes from `next_runnable` + `home_thread`, assuming no
padding added.

**Measure:** print `sizeof(STA_Actor)` from the spike test binary. Compute
the updated per-actor creation budget:

```
sizeof(STA_Actor)  = measured
initial nursery    = 128 bytes
actor identity obj = 16 bytes
─────────────────────────────
total              = ?
target             = ~300 bytes
```

If `sizeof(STA_Actor)` exceeds 180 bytes, a justification is required in
ADR 009. If the total creation cost exceeds 300 bytes, the excess must be
explained and the architecture doc must be updated to reflect the revised
target with rationale.

Current baseline from ADR 008: `sizeof(STA_Actor)` ≈ 120 bytes (with
`STA_MpscList` embedded). Expected post-spike: ~132 bytes. Total creation
cost: ~276 bytes. Headroom: ~24 bytes.

---

### Q7: Stub revision check

Review `src/vm/actor_spike.h` and `src/actor/mailbox_spike.h`.

**`src/vm/actor_spike.h`:**

The current stub still embeds `STA_Mailbox` (the pre-ADR-008 placeholder,
16 bytes) rather than `STA_MpscList` (~40 bytes). ADR 008 states that the
permanent struct should embed `STA_MpscList`. The scheduler spike adds
`next_runnable` (8 bytes) and upgrades `sched_flags` to `_Atomic uint32_t`.
State explicitly:

1. Does `actor_spike.h` need to be revised to embed `STA_MpscList` so that
   the density measurement in Q6 is accurate? (Yes — the measurement is
   invalid against the old 16-byte stub; use the ADR 008 ~120-byte baseline
   and add scheduler fields on top of it.)
2. Should `sched_flags` be changed from `uint32_t` to `_Atomic uint32_t`
   in the spike struct? (Yes — Q5 requires it; record the consequence.)
3. Do any other fields change? State explicitly, and if no changes are
   needed beyond the above, say so.

Do not make changes to `actor_spike.h` or `mailbox_spike.h` during this
spike. Record the required changes as consequences in ADR 009.

**`src/actor/mailbox_spike.h`:**

No changes expected. State whether the `STA_MpscList.count` field and
the `sched_flags` atomic in `STA_Actor` together provide the necessary
signal for scheduler notification (i.e. can the scheduler determine that an
actor has gone from empty mailbox to non-empty without additional
fields?). If not, state what is missing.

---

## What to build

All files are **spike code** — clearly marked, to be replaced during Phase 1
with permanent implementations informed by ADR 009. No file produced by this
spike is the permanent implementation.

### File 1: `src/scheduler/scheduler_spike.h`

Types and interface for the spike scheduler. Define:

```
Contents (to define in implementation):
- STA_WorkDequeA   — Chase-Lev deque variant (fixed-capacity for spike)
- STA_WorkDequeB   — Ring buffer deque variant
- STA_SchedThread  — Per-thread scheduler context (deque, notification fd/condvar,
                     thread handle, thread index)
- STA_Scheduler    — Top-level scheduler struct (array of STA_SchedThread,
                     thread count, running flag)
- STA_REDUCTION_QUOTA — tunable constant (1000)
- Function declarations: init, start, stop, push_actor, steal_actor
- Notification helper declarations (whichever option is chosen plus stubs for
  the other two so all three can be compiled and benchmarked)
```

Do not embed implementation in the header. Only `static inline` size/index
helpers (e.g. deque slot computation) belong in the header. All substantive
logic lives in the `.c` file.

### File 2: `src/scheduler/scheduler_spike.c`

Both deque variants, all three notification options (selectable at compile
time via a `#define STA_NOTIF_MODE` flag: 1=condvar, 2=pipe, 3=spin), the
scheduling loop, and the reduction preemption stub. Key implementation
requirements:

- Deque `push`/`pop` memory ordering: release store on `bottom`, acquire
  load on `top`, seq_cst CAS on steal
- All three notification options must be compilable and benchmarkable via
  `STA_NOTIF_MODE`
- `execute_actor`: stub tight loop decrementing `reductions` to zero
- Scheduling loop: pick actor → execute → re-enqueue or park → steal if
  queue empty → sleep/spin if nothing to steal
- Startup barrier: `pthread_barrier_t` so no thread starts work until all
  are ready
- Shutdown: `_Atomic bool running = false` checked at top of each iteration

### File 3: `tests/test_scheduler_spike.c`

Correctness tests, timing benchmarks, and TSan stress test. The test binary
must:

1. **Lifecycle test:** spawn N threads (sysconf count), run empty loop for
   100 ms, shut down, join all. Verify no TSan reports.

2. **Deque correctness (single-threaded):** push 500 actors onto a Variant A
   deque, pop all 500, verify LIFO order (pop = bottom). Push 500 actors,
   steal all 500 from a second thread, verify FIFO-from-top order.
   Repeat for Variant B.

3. **Steal stress test (multi-threaded):** N scheduler threads, N × 100
   initial actors randomly distributed. Run 10 000 enqueue events (random
   thread targets). After all events, count total actors scheduled across
   all threads. Assert equal to initial count + enqueue events (no loss, no
   duplication). This is the TSan gate.

4. **Notification latency benchmark:** for each of the three options,
   measure wake latency (enqueue → first reduction of actor on scheduler
   thread). 100 000 samples, discard first 1000, report median and p99 in
   nanoseconds. Print in a format suitable for direct inclusion in ADR 009.

5. **Preemption timing:** 10 000 consecutive actor quanta (1 000 reductions
   each). Measure total wall time. Compute per-quantum duration (ns). Report
   median, p99, and max.

6. **Maximum scheduling latency:** N threads running steady-state actors for
   10 seconds. Record the maximum gap between one actor's preemption and the
   next actor starting on the same thread. Report in microseconds.

7. **Actor density check:** print `sizeof(STA_Actor)` and compute total
   per-actor creation cost. Assert total ≤ 300 bytes (print warning if
   exceeded, do not fail — density is a design target, not a hard test gate
   in the spike binary, but any overage must be explained in ADR 009).

### File 4: `docs/decisions/009-scheduler.md`

Written **after** the tests pass and the measurements are in hand. The ADR
must record:

- Chosen deque variant and why (with measured steal contention and throughput)
- Chosen notification option and why (with measured wake latency numbers)
- Scheduling quantum duration in nanoseconds at `STA_REDUCTION_QUOTA = 1000`
- Maximum observed scheduling latency
- No-GIL memory ordering audit for every shared data structure
- Updated `sizeof(STA_Actor)` and per-actor creation cost
- Whether the ~300-byte density target was met, and the justified delta if not
- Required changes to `actor_spike.h` and `mailbox_spike.h` as consequences

Do not write ADR 009 before the test binary exists and `ctest` passes.

---

## CMake integration

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(test_scheduler_spike
    test_scheduler_spike.c
    ../src/scheduler/scheduler_spike.c)
target_include_directories(test_scheduler_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(test_scheduler_spike PRIVATE -fsanitize=thread)
target_link_options(test_scheduler_spike PRIVATE -fsanitize=thread)
add_test(NAME scheduler_spike COMMAND test_scheduler_spike)
```

The TSan-instrumented binary is the ctest target. Passing ctest with TSan
clean is the gate before ADR 009 is written.

Latency benchmarks should be run separately from the TSan build, since TSan
instrumentation adds significant overhead. Provide a second CMake target for
benchmark output:

```cmake
add_executable(bench_scheduler_spike
    test_scheduler_spike.c
    ../src/scheduler/scheduler_spike.c)
target_include_directories(bench_scheduler_spike PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(bench_scheduler_spike PRIVATE STA_BENCH=1)
target_compile_options(bench_scheduler_spike PRIVATE -O2)
# Not registered as a ctest target — run manually for benchmark output.
```

All three notification modes must be buildable from the same source:

```cmake
foreach(mode 1 2 3)
    add_executable(bench_scheduler_notif_${mode}
        test_scheduler_spike.c
        ../src/scheduler/scheduler_spike.c)
    target_include_directories(bench_scheduler_notif_${mode}
        PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(bench_scheduler_notif_${mode}
        PRIVATE STA_BENCH=1 STA_NOTIF_MODE=${mode})
    target_compile_options(bench_scheduler_notif_${mode} PRIVATE -O2)
endforeach()
```

Run each with `./build/tests/bench_scheduler_notif_1` etc. to generate the
three-way comparison table for ADR 009.

---

## Constraints

- All spike files must carry a prominent comment at the top:
  `SPIKE CODE — NOT FOR PRODUCTION`
- Do not modify `include/sta/vm.h` during this spike
- Do not modify `docs/decisions/007-object-header-layout.md` or
  `docs/decisions/008-mailbox-and-message-copy.md`
- `src/scheduler/scheduler_spike.h` and `.c` may include
  `src/vm/actor_spike.h` and `src/actor/mailbox_spike.h` — intentional
  inter-spike dependency; all are spike code
- Tests may not include `src/` headers directly except through the spike
  header chain — no back-channel to `include/sta/vm.h`
- The TSan gate applies to the full stress test (Q2 steal + Q3 wake), not
  only to individual unit tests. A race that only manifests under combined
  load is still a race.
- `memory_order_seq_cst` is permitted only on the steal CAS on `top`. All
  other orderings must be `acquire`, `release`, or `relaxed`. Any additional
  use of `seq_cst` requires an explicit justification in the spike doc.

---

## Open questions this spike deliberately does not answer

These are real questions identified during spike design. They are deferred
to future ADRs so they do not block this spike.

1. **Dynamic deque resize (Chase-Lev).** The spike uses a fixed-capacity
   array of 1024 slots. The real Chase-Lev deque resizes by replacing the
   array with a larger one via a hazard-pointer-protected pointer swap. This
   is a non-trivial concurrent data structure problem; it belongs in a
   Phase 1 spike if Variant A is chosen.

2. **Global run queue / work injection queue.** When a new actor is spawned
   or a previously-parked actor becomes runnable due to an external event
   (I/O completion, timer), it may need to enter a scheduler thread's queue
   without the enqueuer knowing which thread has capacity. A global injection
   queue (MPSC, bounded) is the typical solution. The spike uses direct
   push to a target thread's deque; the injection queue design belongs in
   ADR 009 or a follow-up.

3. **libuv integration (Phase 2).** The pipe-based notification (Option 2)
   integrates naturally with libuv's event loop, which will be introduced in
   Phase 2. The spike does not build a libuv event loop. The integration
   design — specifically, whether each scheduler thread runs its own
   `uv_loop_t` or whether a dedicated I/O thread feeds a shared queue —
   is a Phase 2 concern. The notification mechanism chosen here must not
   preclude the preferred libuv integration pattern.

4. **Preemption via signal (SIGALRM / timer_create).** An alternative to
   reduction counting is a hardware timer signal that interrupts the current
   actor. This is not viable without a GIL (a signal delivered to the wrong
   thread, or to a thread inside a malloc call, causes undefined behavior).
   Reduction counting is the correct approach for a no-GIL VM. No further
   evaluation is needed; document this closure in ADR 009.

5. **Scheduler thread pinning (CPU affinity).** Setting thread CPU affinity
   with `pthread_setaffinity_np` can improve cache locality and reduce steal
   frequency. macOS does not expose `pthread_setaffinity_np`; the equivalent
   is `thread_policy_set` with `THREAD_AFFINITY_POLICY`. This is a Phase 1
   performance tuning concern, not an architecture question. The `home_thread`
   field added in Q6 provides the hook for this without committing to an
   implementation.

6. **Supervisor tree and fault propagation.** When an actor faults during
   `execute_actor`, the scheduler must notify the supervisor. The notification
   channel (a message sent to the supervisor's mailbox) is the correct model
   (§10.9), but the full fault propagation path — including `ask:` future
   resolution on failure — is deferred to Phase 2.

7. **Reduction accounting for blocking primitives.** A primitive that performs
   a system call (even a non-blocking one) consumes wall-clock time that
   reduction counting does not capture. The eventual model is to charge a
   fixed reduction cost to primitives (e.g. 100 reductions for any syscall)
   and to suspend the actor entirely for blocking I/O. This is a Phase 2
   concern; the spike models pure compute quanta only.

---

## What ADR 009 must record

After running the spike, write ADR 009 covering:

- **Deque variant chosen** (A or B), with measured steal contention, steal
  latency, and throughput numbers (median and max, 1/2/4/N-thread configs)
- **Notification mechanism chosen** (Option 1/2/3), with measured wake
  latency (median and p99) for all three options under light load
- **Scheduling quantum duration** in nanoseconds at `STA_REDUCTION_QUOTA = 1000`
  on arm64 M4 Max (median and p99)
- **Maximum observed scheduling latency** in microseconds across N threads
  over 10 seconds of steady-state load
- **No-GIL memory ordering audit** — every shared data structure, memory
  ordering used, and why it is sufficient; TSan clean confirmation
- **Updated `sizeof(STA_Actor)`** after adding scheduler fields, and the
  resulting total per-actor creation cost
- Whether the **~300-byte density target** was met, and the justified delta
  if it was not; updated headroom table
- **Required changes to `actor_spike.h` and `mailbox_spike.h`** as
  consequences — including the `sched_flags` → `_Atomic uint32_t` change
  and the `STA_Mailbox` → `STA_MpscList` substitution
- **The `STA_REDUCTION_QUOTA` value** recorded as the baseline constant;
  rationale for 1000 (inherited from BEAM) and the measurement that either
  validates or adjusts it for arm64 M4 Max
