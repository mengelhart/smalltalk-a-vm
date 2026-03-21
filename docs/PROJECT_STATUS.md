# Smalltalk/A VM — Project Status

## What this is
C17 runtime for Smalltalk/A — an actor-concurrent, capability-aware Smalltalk
with BEAM-class density and true multi-core execution. No GIL, ever.
Full architecture: `docs/architecture/smalltalk-a-vision-architecture-v3.md`
Hard rules and toolchain: `CLAUDE.md`
Detailed history of all completed work: `docs/PROJECT_HISTORY.md`

---

## Current phase
**Phase 2 — Actor Runtime and Headless**
Phase 1 (Minimal Live Kernel) complete. Phase 1.5 (Class Library Foundation) complete.
Now: scheduler, supervision, async I/O, headless lifecycle.

**Current epic:** Epic 7B (Futures Wait, Crash Safety, Stress) complete.

---

## Phase completion summary

### Phase 0 — Architectural Spikes: COMPLETE
7 spikes, ADRs 007–014 accepted. All spike code in `src/` is exploratory reference.

| Spike | Topic | Key outcome | ADR |
|---|---|---|---|
| 001 | ObjHeader layout | ObjHeader=12B, alloc=16B, SmallInt tag bit 0 | 007 |
| 002 | MPSC mailbox | Vyukov linked list, ~264B/actor, TSan clean | 008 |
| 003 | Work-stealing scheduler | Chase-Lev deque, 1000 reductions/quantum | 009 |
| 004 | Frame layout & TCO | STA_Frame=40B, send-return lookahead TCO | 010 |
| 005 | Async I/O (libuv) | Dedicated I/O thread, 288B creation cost | 011 |
| 006 | Image save/load | Flat binary format, 48B header + reloc table | 012 |
| 007 | Native bridge | Handle refcounting, 3 narrow locks, event model | 013 |

### Phase 1 — Minimal Live Kernel: COMPLETE (2026-03-16)
11 epics, 44 CTest targets, 14 ADRs.

| Epic | Topic | Key milestone |
|---|---|---|
| 1 | Object memory & allocator | OOP tagging, heap, immutable space, class table |
| 2 | Symbol table & method dict | FNV-1a symbols, open-addressing method lookup |
| 3 | Bytecode interpreter | "3 + 4 = 7" through full dispatch path |
| 4 | Bootstrap | Metaclass circularity, 26 classes, kernel primitives |
| 5 | basicNew / basicNew: | Smalltalk-level object creation via format field |
| 6 | Object/memory prims 33–41 | Complete Blue Book §8.5 Object protocol |
| 7 | Compiler | Scanner/parser/codegen, 10 inlined control structures |
| 8 | Exceptions | on:do:, signal, ensure:, doesNotUnderstand: |
| 9 | Kernel source loading | 12 .st files, chunk format reader, prim 122 |
| 10 | Image save/load | Round-trip acid test passes (save→load→execute) |
| 11 | Eval loop & public API | sta_vm_create/destroy/eval/inspect/save/load |

### Phase 1.5 — Class Library Foundation: COMPLETE
5 batches, 20 new primitives, 17 kernel .st files, 58 CTest targets.

| Batch | Topic |
|---|---|
| 1 | Arithmetic completion (prims 5–17, 200), to:do:, timesRepeat: |
| 2 | Byte-indexable prims (60–64), Character (94–95), String expansion |
| 3 | Collection completion, OrderedCollection, replaceFrom:to:with:startingAt: |
| 4 | Number protocol, Symbol (prims 91–93), hashing |
| 5 | Integration stress tests (59 tests, polymorphic dispatch, deep chains) |

### Phase 2 — Actor Runtime: IN PROGRESS

| Epic | Topic | Status |
|---|---|---|
| 0 | Per-instance VM state | ✅ Complete — STA_VM owns all state, real handle table |
| 1 | Full closures | ✅ Complete — heap contexts, NLR, ensure: unwinding |
| 2 | Actor struct & per-actor heaps | ✅ Complete — STA_Actor=112B, per-actor heap/slab/handler |
| 3 | Mailbox & message send | ✅ Complete — MPSC mailbox, deep copy, dispatch |
| 4 | Work-stealing scheduler | ✅ Complete — N-thread, preemption, work stealing, TSan clean |
| 5 | Per-actor GC | ✅ Complete — Cheney semi-space, per-actor, forwarding, growth |
| 5.1 | Fix #295: deep copy heap growth | ✅ Complete — pre-flight size estimation |
| 6 | Supervision | ✅ Complete — OTP-style restart/stop/escalate, intensity limiting |
| 6.1 | Actor registry & safe send | ✅ Complete — actor_id indirection, zero-copy fast path |
| 6.2 | Lightweight supervisor notifications | ✅ Complete — zero heap allocation on supervisor |
| 6.3 | Actor refcount & race fixes | ✅ Complete — refcount lifecycle, TSan clean 91/91 |
| 7A | Futures infrastructure | ✅ Complete — STA_Future, future table, ask/reply routing |
| 7B | Wait primitive, crash failure, stress | ✅ Complete — Future class, prim 201, crash→fail, soak 275K rts/s |

---

## Current struct sizes and density

| Metric | Value |
|---|---|
| sizeof(STA_Actor) | 208 bytes (grew from 200 in Epic 7A: +pending_future_id) |
| Creation cost | 864 bytes (208 struct + 128 nursery + 512 stack + 16 identity) |
| sizeof(STA_Future) | 48 bytes (grew from 40 in Epic 7B: +vm back-pointer) |
| sizeof(STA_FutureTable) | 96 bytes (grew from 88 in Epic 7B: +vm pointer) |
| sizeof(STA_Frame) | 56 bytes (was 48, +context field in Epic 1) |
| BEAM comparison | 2,704 bytes at spawn — Smalltalk/A is ~3.1× more compact |

---

## Open decisions (from ADRs 007–013)

Resolved items moved to `docs/PROJECT_HISTORY.md`.

2. **Forwarding pointer mechanics** — decide before GC is implemented
3. **Class table concurrency** — decide before method cache is built
4. **Variant B deque root cause** (ADR 009) — investigate in Phase 1
5. **Per-actor scheduling fairness** (ADR 009) — measure with real bytecode in Phase 1
11. **Transfer buffer allocator** (ADR 008) — replace malloc stub with runtime slab, Phase 1
12. **Resume point protocol for mid-message I/O suspension** (ADR 011) — define valid suspension points before Phase 2 I/O primitives
13. **Lock-free I/O request queue** (ADR 011) — replace mutex-protected FIFO with `STA_MpscList`, Phase 2
14. **I/O backpressure integration with §9.4 bounded mailboxes** (ADR 011) — Phase 2 design question
15. **⚠ Density headroom consumed** — STA_Actor grew to 208B in Epic 7A. Creation cost 864B.
19. **Growable handle table** (ADR 013, #88) — Phase 3 blocker
20. **Handle validity after `sta_vm_destroy`** (ADR 013, #89) — Phase 3 blocker
21. **`sta_inspect_cstring` caller-provided buffer** (ADR 013, #90) — Phase 3 blocker
22. **Event callback re-entrancy rules** (ADR 013, #91) — Phase 3 blocker

---

## Known issues (open)

- #320: Actor registered before fully initialized — behavior_obj is zero during registration window
- #321: restart_child skips scheduling when scheduler is stopping — new child stranded in CREATED
- #243: SmallInteger = with non-SmallInt arg returns receiver instead of false
- #244: Catching DNU via on:do: triggers unhandled BlockCannotReturn
- SmallInteger >> #/ needs Smalltalk fallback for ZeroDivide (prim 10 returns out-of-range, DNU results)
- `STA_MailboxMsg.args` holds raw C pointers into target heap — stale if heap grows between sends
- #337: TSan race in sta_scheduler_enqueue — next_runnable written outside wake_mutex

---

## Deferred items

- **Stream classes (ReadStream, WriteStream)** — Phase 3
- **Byte-aware at:/at:put: for String/ByteArray** — done (Batch 2, prims 60–64)
- **OrderedCollection** — done (Batch 3)
- **Full closures** — done (Phase 2 Epic 1)
- **ensure: during exception unwinding** — done (Phase 2 Epic 1)
- **Real handle table** — done (Phase 2 Epic 0)
- **Per-instance state** — done (Phase 2 Epic 0)

---

## ADR index

| ADR | Topic | Status |
|---|---|---|
| 001 | Implementation language (C17) | Accepted |
| 002 | Public API boundary (`sta/vm.h`) | Accepted |
| 003 | Internal header convention | Accepted |
| 004 | Live update semantics | Accepted |
| 005 | API error reporting | Accepted |
| 006 | Handle lifecycle | Accepted (closed by ADR 013) |
| 007 | Object header layout and OOP tagging | Accepted |
| 008 | Mailbox and message copy | Accepted |
| 009 | Work-stealing scheduler and reduction-based preemption | Accepted |
| 010 | Activation frame layout and tail-call optimisation | Accepted |
| 011 | Async I/O architecture via libuv | Accepted |
| 012 | Image format and snapshot protocol | Accepted |
| 013 | Native bridge (C runtime ↔ SwiftUI IDE) | Accepted |
| 014 | Stack slab growth policy and TCO frame sizing | Accepted |

---

## Repo layout

```
include/sta/vm.h          ← only public header
src/vm/                   ← object memory, interpreter, eval, handler, primitives
src/actor/                ← actor struct, mailbox, deep copy, registry, supervisor, future
src/gc/                   ← per-actor GC (Cheney semi-space)
src/scheduler/            ← work-stealing scheduler + Chase-Lev deque
src/io/                   ← async I/O spike (Phase 2 production TBD)
src/image/                ← production image save/load
src/bootstrap/            ← kernel bootstrap + file-in reader
src/compiler/             ← scanner, parser, AST, codegen
kernel/                   ← Smalltalk kernel .st files (17 files)
tests/                    ← 94 CTest targets
examples/embed_basic/     ← public API smoke test
docs/architecture/        ← master architecture document
docs/decisions/           ← ADRs 001–014
docs/spikes/              ← spike-001 through spike-007
```

---

## Test count
98 active CTest targets (100 total, 2 disabled), all passing. ASan clean. TSan: 1 pre-existing race in sta_scheduler_enqueue (#337).

---

## How to orient a new session

1. Read `CLAUDE.md` (hard rules, build commands, workflow)
2. Read this file (current state, open decisions, what exists)
3. For epic-specific work, also read the relevant ADRs for the component being built
4. Full historical detail: `docs/PROJECT_HISTORY.md`
