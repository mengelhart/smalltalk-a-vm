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

---

## Open decisions (from ADR 007 and ADR 008)
These must be resolved before the corresponding component is built:

1. **Nil/True/False as immediates** — decide before first bytecode dispatch loop
2. **Character representation** — decide before string/character primitives
3. **Forwarding pointer mechanics** — decide before GC is implemented
4. **Class table concurrency** — decide before method cache is built
5. **Activation frame layout / TCO** — companion spike needed before bytecode interpreter
6. **Deep-copy visited set** — hash map for cycles/sharing required before Phase 1 copy implementation
7. **`ask:` future on mailbox-full** — future resolution path, Phase 2
8. **Transfer buffer allocator** — replace malloc stub with runtime slab, Phase 1

---

## Remaining Phase 0 spikes (suggested order)

| # | Spike | Depends on | Key questions |
|---|---|---|---|
| 003 | Work-stealing scheduler + reduction preemption | ADR 008 | Thread-per-core, steal protocol, scheduler notification of non-empty mailbox, no-GIL correctness |
| 004 | Activation frame layout + TCO | ADR 007 | Frame header, GC stack walk, tail-call frame reuse |
| 005 | Async I/O (libuv integration) | Spike 003 | Scheduler-thread never blocks, actor suspension/resume on I/O |
| 006 | Image save/load (closed-world subset) | Spikes 002-003 | Serialization format, snapshot safe point, restore integrity |
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

---

## Repo layout reminder
```
include/sta/vm.h          ← only public header (never add anything else here)
src/vm/                   ← oop_spike.h, actor_spike.h (Phase 0 spike code)
src/actor/                ← mailbox, lifecycle stubs
src/gc/                   ← Phase 1+
src/scheduler/            ← Phase 0 spike target
docs/decisions/           ← ADRs 001-008
docs/spikes/              ← spike-001-objheader.md, spike-002-mailbox.md
```

---

## How to orient a new chat with Claude
Paste this file plus `CLAUDE.md` at the start of the session.
For spike design work, also paste the relevant ADR(s) from `docs/decisions/`.

