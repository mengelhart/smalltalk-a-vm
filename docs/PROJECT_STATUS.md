# Smalltalk/A VM — Project Status

## What this is
C17 runtime for Smalltalk/A — an actor-concurrent, capability-aware Smalltalk
with BEAM-class density and true multi-core execution. No GIL, ever.
Full architecture: `docs/architecture/smalltalk-a-vision-architecture-v3.md`
Hard rules and toolchain: `CLAUDE.md`

---

## Current phase
**Phase 0 — Architectural spikes**
Spike → measure → write decision record → implement.
No permanent implementation yet. All spike code is clearly marked.

---

## Completed spikes

### Spike 001 — ObjHeader layout and OOP tagging
- Spike doc: `docs/spikes/spike-001-objheader.md`
- ADR: `docs/decisions/007-object-header-layout.md`
- Key results:
  - `STA_ObjHeader` = 12 bytes, allocation unit = 16 bytes (payload 8-byte aligned)
  - OOP: 63-bit SmallInt (bit 0 tag), heap pointer (bit 0 clear)
  - `nil`/`true`/`false` are heap objects in shared immutable region — no special tag
  - `gc_flags` and `obj_flags` bit fields fully defined
  - `sizeof(STA_Actor)` stub = 80 bytes
  - Total per-actor creation cost = 224 bytes (target ~300 — 76 bytes headroom)

### Spike 002 — Lock-free MPSC mailbox and cross-actor copy
- Spike doc: `docs/spikes/spike-002-mailbox.md`
- ADR: `docs/decisions/008-mailbox-and-message-copy.md`
- Key results:
  - Variant A chosen: Vyukov linked list + `_Atomic uint32_t count` capacity counter (~40 bytes)
  - Ring buffer (Variant B) measured and rejected — 4 112 bytes pre-allocated, 14× density blowup
  - SPSC median 84 ns / p99 250 ns; 4P1C median 458 ns / p99 1250 ns
  - Overflow policy: drop-newest (`STA_ERR_MAILBOX_FULL` returned to sender), default limit 256
  - Deep copy default; `STA_OBJ_IMMUTABLE` objects shared by pointer (no copy)
  - Copy cost baseline: 41 ns median for 5-element Array with one mutable sub-object
  - TSan clean: 1M messages (4 producers × 250k) in 0.60s — no data races
  - Per-actor cost ~264 bytes (within ~300-byte target): struct (~120) + nursery (128) + identity (16)

### Spike 003 — Work-stealing scheduler and reduction-based preemption
- Spike doc: `docs/spikes/spike-003-scheduler.md`
- ADR: `docs/decisions/009-scheduler.md`
- Key results:
  - Chase-Lev fixed-capacity deque (Variant A) chosen; ring buffer (Variant B) rejected (hang under large -O2 workload, root cause deferred to Phase 1)
  - `pthread_cond_signal` chosen for idle-thread wakeup; median wake latency 5,958 ns / p99 7,875 ns
  - `STA_REDUCTION_QUOTA = 1000` reductions per quantum; accounting cost ~2–4 µs per quantum
  - `STA_ActorRevised` = 120 bytes; total creation cost = 264 bytes (within ~300-byte target)
  - TSan clean: Variant A, 4 threads, 100 actors, 3 runs each — no data races
  - `src/vm/actor_spike.h` must be revised before Phase 1: add `sched_flags` (atomic), `next_runnable`, `home_thread`

### Spike 004 — Activation frame layout and tail-call optimisation (TCO)
- Spike doc: `docs/spikes/spike-004-frame-layout.md`
- ADR: `docs/decisions/010-frame-layout.md`
- Key results:
  - Option A chosen: plain C struct in contiguous per-actor stack slab (not heap-allocated `STA_ObjHeader`)
  - `sizeof(STA_Frame)` = 40 bytes; first payload slot at offset 40 (8-byte aligned)
  - Option B (ObjHeader-based frame) rejected: 72-byte fixed overhead vs. 40-byte, no meaningful benefit
  - TCO: one-instruction lookahead in dispatch loop; `bytecode[pc + STA_SEND_WIDTH] == OP_RETURN_TOP` → frame reuse
  - TCO validated: 1,000,000-deep tail recursion with `max_depth == 1` throughout
  - Preemption under tail recursion: 10 preemptions in 10,000-deep countdown (quota=1,000) — correct
  - GC stack-walk: `sta_frame_gc_roots()` visits exactly 10 OOP slots in 3-frame chain — correct
  - Debugger policy TC-A: TCO-elided frames permanently lost; TCO disabled per-actor when debug capability attached
  - `STA_ActorRevised` projection: 136 bytes with `stack_base` + `stack_top` fields; creation cost = 280 bytes
  - **Headroom: 20 bytes entering Spike 005 — tight, see ADR 010 density table**

---

## Open decisions (from ADR 007, 008, 009, 010)
These must be resolved before the corresponding component is built:

1. **Nil/True/False as immediates** — decide before first bytecode dispatch loop
2. **Forwarding pointer mechanics** — decide before GC is implemented
3. **Class table concurrency** — decide before method cache is built
4. **Variant B deque root cause** (ADR 009) — investigate in Phase 1
5. **Per-actor scheduling fairness** (ADR 009) — measure with real bytecode in Phase 1
6. **Stack slab growth policy** (ADR 010) — decide before bytecode interpreter is written
7. **TCO with callee having more locals** (ADR 010) — decide alongside slab growth policy
8. **Closure and non-local return compatibility** (ADR 010) — design before Phase 1 block/closure support
9. **Deep-copy visited set** (ADR 008) — hash map for cycles/sharing required before Phase 1 copy implementation
10. **`ask:` future on mailbox-full** (ADR 008) — future resolution path, Phase 2
11. **Transfer buffer allocator** (ADR 008) — replace malloc stub with runtime slab, Phase 1

---

## Remaining Phase 0 spikes (suggested order)

| # | Spike | Depends on | Key questions |
|---|---|---|---|
| 005 | Async I/O (libuv integration) | ADR 009, ADR 010 | Scheduler-thread never blocks; actor suspend/resume on I/O; `STA_Actor` density impact (20-byte headroom — see ADR 010 §Consequences) |
| 006 | Image save/load (closed-world subset) | Spikes 002–004 | Serialization format, snapshot safe point, restore integrity |
| 007 | Native bridge (C runtime ↔ SwiftUI IDE) | Spike 006 | `sta/vm.h` public API surface, FFI contract, live update path |

---

## ADR index

| ADR | Topic | Status |
|---|---|---|
| 001 | Implementation language (C17) | Accepted |
| 002 | Public API boundary (`sta/vm.h`) | Accepted |
| 003 | Internal header convention | Accepted |
| 004 | Live update semantics | Accepted |
| 005 | API error reporting | Accepted |
| 006 | Handle lifecycle | Accepted |
| 007 | Object header layout and OOP tagging | Accepted |
| 008 | Mailbox and message copy | Accepted |
| 009 | Work-stealing scheduler and reduction-based preemption | Accepted |
| 010 | Activation frame layout and tail-call optimisation | Accepted |

---

## Repo layout reminder
```
include/sta/vm.h          ← only public header (never add anything else here)
src/vm/                   ← oop_spike.h, actor_spike.h (Phase 0 spike code)
src/actor/                ← mailbox, lifecycle stubs
src/gc/                   ← Phase 1+
src/scheduler/            ← scheduler_spike.h, scheduler_spike.c
src/io/                   ← Phase 0 spike target (Spike 005)
docs/decisions/           ← ADRs 001-010
docs/spikes/              ← spike-001 through spike-004
```

---

## How to orient a new chat with Claude
Paste this file plus `CLAUDE.md` at the start of the session.
For spike design work, also paste the relevant ADR(s) from `docs/decisions/`.
For Spike 005: paste `CLAUDE.md` + this file + `ADR 009` + `ADR 010`.
