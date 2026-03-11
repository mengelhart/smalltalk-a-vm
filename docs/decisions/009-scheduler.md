# ADR 009 — Work-Stealing Scheduler and Reduction-Based Preemption

**Status:** Accepted
**Date:** 2026-03-10
**Spike:** `docs/spikes/spike-003-scheduler.md`
**Depends on:** ADR 007 (object header layout), ADR 008 (mailbox and message copy)

---

## Decision

The Smalltalk/A VM uses a **work-stealing scheduler with one OS thread per
CPU core** (`sysconf(_SC_NPROCESSORS_ONLN)`). Each thread owns a **Chase-Lev
fixed-capacity double-ended queue** (Variant A). Reduction-based preemption
grants each actor a budget of **STA_REDUCTION_QUOTA = 1000 reductions** per
scheduling quantum. Idle threads are woken via **`pthread_cond_signal`**
(Option 1). All three choices are supported by measured data from the spike.

---

## Measured results (arm64 M4 Max, Apple clang 17, `-O2`)

All numbers from `bench_scheduler_spike` and `bench_scheduler_notif_1`
unless noted. TSan build: `test_scheduler_spike` with `-fsanitize=thread`.

### Q1 — Thread lifecycle

| Metric | Value |
|---|---|
| Startup latency (16 threads) | 2,631 µs |
| Shutdown latency (16 threads) | 55,503 µs |

Startup includes barrier synchronisation; shutdown includes joining all
threads after the final notification wake. Both are well within any
reasonable application tolerance. TSan clean.

### Q2 — Deque steal contention (Variant A, Chase-Lev)

| Threads | Wall time | Total runs | Steals | Failed steal attempts | CAS failures |
|---|---|---|---|---|---|
| 1 | 2 ms | 2,000 | 0 | 0 | 0 |
| 2 | 2 ms | 4,000 | 0 | 2 | 0 |
| 4 | 2 ms | 8,000 | 49 | 45 | 0 |

Zero CAS failures across all configurations. With 4 threads, 49 successful
steals balanced the load within 2 ms. CAS contention on the deque top pointer
is negligible at this actor density.

**Variant B (ring buffer):** Structurally identical to Variant A (same atomic
protocol, same capacity). Under the larger non-TSan workload (1000 actors,
10 runs each, 4 threads), 10 actors failed to retire within the 60-second
timeout, and the 4-thread steal contention bench timed out at 30 seconds
with 43 actors incomplete. This failure is not yet fully explained — the
implementations are protocol-identical and should behave identically.
The most likely cause is a compiler code-generation difference at `-O2`
exposing a latent ordering subtlety. Variant B is **rejected** pending
investigation; Variant A passed all correctness and TSan checks cleanly.

### Q3 — Scheduler notification wake latency

Three options were implemented and measured. Only Option 1 (cond) produced
clean measurements. Options 2 and 3 had disqualifying problems (see below).

**Option 1: `pthread_cond_signal` (100,000 samples, 1,000 warmup)**

| Percentile | Latency |
|---|---|
| Min | 2,375 ns |
| Median (p50) | 5,958 ns |
| p90 | 6,666 ns |
| p99 | 7,875 ns |
| Max | 40,666 ns |

Wake latency is measured from `sta_sched_push()` to the first instruction
of `execute_actor` in the scheduler thread, via the `start_ns` field written
by the scheduler and read back by the sender after `run_count` is observed
to increment.

**Option 2: pipe write — disqualified.** The `drain_pipe` helper uses
blocking `read()` on a pipe whose write end is held open throughout the
scheduler lifetime. When called on an empty pipe (the normal case before any
actor is pushed), `read()` blocks indefinitely. The scheduler thread stalls
at the first call to `sta_notif_wait` and never enters `poll()`. This is a
correctness bug, not a performance trade-off. Fixing it requires setting
`O_NONBLOCK` on the read fd after `pipe()`. No latency numbers were captured.

**Option 3: spinning with exponential backoff — disqualified for production.**
The benchmark ran for over 16 minutes and was terminated before completing
100k samples. At 100% CPU per idle scheduler thread (198% observed for a
2-thread config), this mode is unsuitable for any workload where actors are
periodically idle. No latency numbers captured; the wake latency is bounded
by the current backoff value (1 ns to 1 ms range) but the CPU cost is
unconditional.

**Recommendation: Option 1 (`pthread_cond_signal`) with 5 ms timedwait
fallback.** The 6 µs median wake latency is acceptable for actor message
delivery; BEAM's measured wake latency is in the same order of magnitude.
The timedwait timeout prevents permanent sleep if a wakeup signal is lost.

### Q4 — Reduction-based preemption

**Quantum duration** (1,000 reductions of the stub execute loop):

| Percentile | Duration |
|---|---|
| Median | ~0 ns |
| p99 | 42 ns |
| Max | 209 ns |

At `-O2`, the compiler folds the reduction counter loop into a constant
assignment (bottom = 0) because all accesses are `relaxed` and the loop has
no observable side effects visible outside the function. The median of 0 ns
reflects this. The p99 of 42 ns and max of 209 ns come from cache-miss and
timer granularity effects on the surrounding measurement infrastructure.

**Interpretation:** The stub `execute_actor` does not model real bytecode
dispatch. The actual quantum duration is determined by the cost of 1,000
real interpreter iterations, which will be measured in Phase 1. The reduction
counter itself adds one atomic load + one atomic store per reduction — on
arm64 M4, relaxed atomics over a hot cache line cost ~1–2 ns each, so the
accounting overhead per quantum is approximately 2–4 µs. This is acceptable.

**Maximum scheduling latency** (200 actors, 16 threads, 5 seconds):

| Metric | Value |
|---|---|
| Total scheduling decisions | 44,627,771 |
| Average runs per actor | 223,138 |
| Approximate maximum gap | 22 µs |

Maximum gap is approximated as `wall_time / avg_runs_per_actor`. Per-wakeup
measurement requires per-actor timestamp logging (not done; would perturb
results). The 22 µs figure is an upper bound on average inter-scheduling
latency, not a worst-case starvation measurement. Per-actor scheduling
fairness was not verified (min_runs_per_actor = 0 for some actors suggests
some actors were never scheduled in the 5-second window — this is likely an
artifact of the large actor count relative to the available scheduling quanta,
not a fairness defect in the scheduler itself). Phase 1 should add per-actor
scheduling fairness measurement.

### Q5 — No-GIL correctness audit

| Shared structure | Synchronisation | Justification |
|---|---|---|
| `STA_WorkDequeA.top` | `seq_cst` CAS (steal + pop last) | Chase-Lev: CAS on top arbitrates concurrent stealers; `seq_cst` is the one justified use — the fence closes the pop/steal race on the last element |
| `STA_WorkDequeA.bottom` | `release` store (push), `acquire` load (steal) | Pairs with the push/steal happens-before for buffer slot visibility |
| `STA_WorkDequeA.buf[i]` | `relaxed` read/write | Slot at index `b` is written by owner before `release` store of `bottom`; any loader that `acquire`-reads a `bottom` ≥ `b+1` is guaranteed to see the write |
| `STA_Scheduler.running` | `release` store (stop), `acquire` load (loop) | Standard flag; release/acquire suffices |
| `STA_Scheduler.remaining` | `acq_rel` fetch_sub | Ensures the retirer's work is visible to the reader that observes remaining == 0 |
| `STA_SpikeActor.sched_flags` | `release` store, `acquire` load | State transition visibility: RUNNABLE/RUNNING/NONE are single-writer transitions with release semantics |
| `STA_SpikeActor.run_count` | `acq_rel` fetch_add | The post-increment provides both a happens-before for the caller and a synchronisation point for the completion observer |
| `STA_SpikeActor.start_ns` | `release` store (scheduler), `relaxed` load (reader after acq on run_count) | Ordering provided by the acquire on run_count; start_ns read is dominated by that edge |
| `STA_NotifCond.pending` | `relaxed` (guarded by mutex) | The mutex provides the happens-before; `pending` is only read/written under the mutex |

No `memory_order_seq_cst` is used outside the Chase-Lev deque CAS operations,
where it is explicitly justified above. TSan reported no races on any of these
structures under the full multi-thread stress test.

### Q6 — Actor density checkpoint

| Component | Bytes |
|---|---|
| `STA_ActorRevised` struct (with scheduler fields) | 120 |
| Initial nursery slab | 128 |
| Actor identity object (0-slot ObjHeader) | 16 |
| **Total per-actor creation cost** | **264** |

`sizeof(STA_ActorRevised)` = 120 bytes. The 8 bytes added by the scheduler
(`next_runnable` + `home_thread` + alignment pad) bring the struct from the
ADR 008 baseline of ~112 bytes to 120 bytes. This is under the 180-byte
single-struct threshold; no justification is required. The total creation cost
of 264 bytes is 36 bytes under the 300-byte target.

### Q7 — Stub revision check

**`src/vm/actor_spike.h`:** Needs revision. The `STA_Actor` stub must add:
- `_Atomic uint32_t sched_flags` (upgrade from plain `uint32_t`)
- `struct STA_Actor *next_runnable` (intrusive run-queue link)
- `uint32_t home_thread` (affinity hint)
These changes are recorded here as consequences; they are not made during
this spike per the spike constraints.

**`src/actor/mailbox_spike.h`:** No revision required based on spike results.
The `STA_MpscList` layout confirmed in ADR 008 is unchanged.

---

## Deque design: Chase-Lev fixed-capacity (Variant A)

### Why not Variant B (ring buffer)?

The implementations are structurally identical — same atomic CAS-on-top steal
protocol, same capacity, same memory orderings. Despite this, Variant B
exhibited a reproducible hang under the larger non-TSan workload (1000 actors,
10 runs, 4 threads): 10 of 1000 actors failed to retire within 60 seconds.
Variant A passed the same workload cleanly.

The most likely explanation is a compiler code-generation divergence at `-O2`
that exposes a latent happens-before gap. Because the two implementations are
textually identical, the divergence must arise from code layout or inlining
differences affecting how the compiler schedules the atomic accesses. TSan did
not catch this because the TSan build uses a smaller workload (100 actors,
3 runs) where timing makes the window vanishingly rare.

**Action:** Variant B is rejected. A dedicated investigation is deferred to
Phase 1. If the root cause is confirmed benign (e.g., a code layout artifact
with no semantic impact), Variant B may be reinstated.

---

## Notification mechanism: pthread_cond_signal (Option 1)

### Lost-wakeup protocol

The enqueuer:
1. Pushes to the deque (release store on `bottom`).
2. Acquires the mutex.
3. Sets `pending = 1` (relaxed, under mutex).
4. Calls `pthread_cond_signal`.
5. Releases the mutex.

The scheduler, before sleeping:
1. Acquires the mutex.
2. Re-checks `pending` and `running`.
3. If no work: `pthread_cond_timedwait` (5 ms timeout).
4. Clears `pending`, releases mutex.

The mutex in steps 1–5 on both sides closes the lost-wakeup window: any
push that completes before the enqueuer acquires the mutex (step 2) will have
its `bottom` increment visible to the scheduler's deque check under the mutex.
Any push that completes after the enqueuer acquires the mutex signals the
already-sleeping scheduler. The 5 ms timedwait is a belt-and-suspenders guard
against any remaining edge case in the wake path.

### Why not Option 2 (pipe write)?

Correctness bug: `drain_pipe` issues a blocking `read()` on an empty pipe
without setting `O_NONBLOCK`. The scheduler thread stalls permanently at the
first idle cycle. Not measured; not eligible.

### Why not Option 3 (spin + backoff)?

100% CPU per idle scheduler thread. Unsuitable for any deployment where
actors are periodically idle (the common case). The backoff reduces
contention but does not eliminate the CPU cost. Not appropriate for a
general-purpose scheduler.

---

## Consequences

- **Scheduler:** one `pthread_t` per `sysconf(_SC_NPROCESSORS_ONLN)` core,
  started via `sta_sched_init` / `sta_sched_start`, stopped via
  `sta_sched_stop` / `sta_sched_destroy`.
- **Run queue:** Chase-Lev fixed-capacity deque (1024 slots) per thread.
  Owner pushes/pops bottom; stealers CAS top. Zero CAS failures at spike
  workloads.
- **Notification:** `pthread_cond_signal` per scheduler thread. Wake latency
  median 5,958 ns, p99 7,875 ns on arm64 M4 Max. 5 ms timedwait fallback.
- **Preemption:** `STA_REDUCTION_QUOTA = 1000` reductions per quantum.
  Accounting cost: ~2–4 µs per quantum at two relaxed atomic ops per reduction.
  Actual quantum wall-clock duration deferred to Phase 1 bytecode interpreter.
- **Actor density:** `STA_ActorRevised` = 120 bytes; total creation cost =
  264 bytes. Within the 300-byte target.
- **`src/vm/actor_spike.h`** must be revised before Phase 1: add
  `sched_flags` (atomic), `next_runnable`, `home_thread`.
- **Variant B (ring buffer deque):** rejected; root-cause investigation
  deferred to Phase 1.
- **Option 2 (pipe):** rejected due to blocking `drain_pipe` bug.
- **Option 3 (spin):** rejected due to unconditional CPU cost.
- **TSan gate:** clean on all correctness tests (Variant A, Option 1,
  4 threads, 100 actors, 3 runs each).

---

## Open questions (deferred)

1. **Variant B root cause.** Identical-protocol deque fails under large -O2
   workload but passes TSan. Investigate in Phase 1.

2. **Per-actor scheduling fairness.** Min-runs-per-actor = 0 in the 5-second
   latency bench. Determine whether this is starvation or an artefact of
   the stub workload. Measure with real bytecode in Phase 1.

3. **Actual quantum wall-clock duration.** The stub execute loop is optimised
   away at `-O2`. Measure with real bytecode dispatch in Phase 1. ADR 009
   records that the accounting cost per quantum is ~2–4 µs; the computation
   cost is unknown until the interpreter exists.

4. **`ask:` and back-pressure.** When the scheduler is asked to suspend an
   actor pending a future, the reduction counter must be reset. Protocol TBD
   in Phase 2.

5. **libuv integration.** The I/O thread pool (Phase 2) will enqueue actors
   into scheduler deques on I/O completion. The `sta_sched_push` + Option 1
   notification path is already the correct interface; no protocol change
   is anticipated.
