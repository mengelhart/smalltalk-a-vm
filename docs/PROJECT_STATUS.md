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

### Spike 005 — Async I/O integration via libuv
- Spike doc: `docs/spikes/spike-005-async-io.md`
- ADR: `docs/decisions/011-async-io.md`
- Key results:
  - Option A chosen: dedicated I/O thread running `uv_run(UV_RUN_DEFAULT)`; scheduler threads never block
  - `sizeof(uv_loop_t)` = 1,072 bytes — Option C (per-actor loop) trivially rejected on measured data
  - Model A chosen: direct deque push (`sta_io_sched_push`) on I/O completion — no message allocation
  - `STA_SCHED_SUSPENDED = 0x04u` gates re-enqueue; three-way flag check prevents double-scheduling
  - `sizeof(STA_ActorIo)` = 144 bytes; permanent I/O fields: `io_state` (4) + `io_result` (4) = 8 bytes
  - Total creation cost = 288 bytes (target ~300); **Low scenario** from ADR 010 table; headroom = 12 bytes
  - 13,150 compute-actor executions during 50 ms I/O wait — scheduler thread never blocked
  - TCP loopback echo: 16-byte round trip; correct under compute load (82 compute runs during TCP)
  - TSan clean: all tests pass with `-fsanitize=thread`
  - Chase-Lev LIFO replaced with FIFO in spike scheduler — single-thread starvation artefact only, not a production change
  - **`STA_Actor` must add `io_state` + `io_result` before Phase 2; headroom: 12 bytes**

### Spike 006 — Image save/load (closed-world subset)
- Spike doc: `docs/spikes/spike-006-image.md`
- ADR: `docs/decisions/012-image-format.md`
- Key results:
  - Flat binary format chosen: 48-byte header + immutable section + object data records + relocation table
  - OOP encoding: SmallInts and Character immediates verbatim; heap ptrs as `(object_id << 2) | 0x3`
  - Disambiguation via relocation table (authoritative), not tag bits alone
  - Shared immutables (nil, true, false, symbols) encoded by FNV-1a name key; resolved via callback on load
  - Restore integrity: all OOPs, flags, and payload words round-trip identically (asserted in test)
  - Save ~595k objects/sec · 47.9 MB/s; Load ~700k objects/sec (TSan build, M4 Max)
  - Per-object on-disk overhead: 16 bytes fixed (`STA_ObjRecord`) + payload; reloc: 8 bytes/heap-ptr slot
  - `STA_Actor` gains `snapshot_id` (4 bytes) + pad (4 bytes); `STA_ACTOR_QUIESCED = 0x08u` reuses `sched_flags`
  - `sizeof(STA_ActorSnap)` = 152 bytes; total creation cost = **296 bytes; headroom = 4 bytes ⚠**
  - TSan clean: single-threaded spike, no shared state introduced
  - **⚠ Only 4 bytes of density headroom remain — any future `STA_Actor` addition requires a new ADR**

### Spike 007 — Native bridge (C runtime ↔ SwiftUI IDE)
- Spike doc: `docs/spikes/spike-007-native-bridge.md`
- ADR: `docs/decisions/013-native-bridge.md` (also closes ADR 006 — Handle lifecycle)
- Key results:
  - Handle model: explicit reference counting (JNI/CPython); `STA_Handle*` stable, OOP updated in-place on GC move
  - Bootstrapping: `sta_vm_nil/true/false/lookup_class` provide first handles without `sta_eval`
  - 10 new `vm.h` functions + 5 types; each justified by a real IDE scenario; no speculative additions
  - Threading: 3 narrow locks (IDE-API, method-install, actor-registry); scheduler never holds any; not a GIL
  - `sta_inspect_cstring`: **NOT thread-safe** — single-caller-at-a-time by contract; Phase 3 changes to caller-provided buffer
  - Live update: method_install and class_define stub-logged under `install_lock`; 8×100 concurrent installs TSan-clean
  - Actor enumeration: snapshot model (registry lock released before visitor); 10 actors, TSan-clean
  - Event model: push; `STA_EVT_ACTOR_CRASH`, `METHOD_INSTALLED`, `IMAGE_SAVE_COMPLETE`, `UNHANDLED_EXCEPTION`
  - `STA_Actor` unchanged: 0 bytes added; **4-byte density headroom preserved**
  - 15 tests passing; ctest: 0.12 s, TSan-clean; `sizeof(STA_Handle)` = 16, `sizeof(STA_ActorEntry)` = 48

---

## Open decisions (from ADRs 007–013)
These must be resolved before the corresponding component is built:

1. ~~**Nil/True/False as immediates**~~ — **Resolved in ADR 007 amendment — heap objects in shared immutable region.**
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
12. **Resume point protocol for mid-message I/O suspension** (ADR 011) — define valid suspension points before Phase 2 I/O primitives
13. **Lock-free I/O request queue** (ADR 011) — replace mutex-protected FIFO with `STA_MpscList`, Phase 2
14. **I/O backpressure integration with §9.4 bounded mailboxes** (ADR 011) — Phase 2 design question
15. **⚠ 4-byte density headroom** (ADR 012) — next `STA_Actor` addition requires a new ADR; breach of 300-byte target must be explicitly justified per CLAUDE.md
16. **Quiescing protocol for live actors** (ADR 012) — Phase 1 blocker; define before Phase 1 image save
17. ~~**Root table for multi-root images**~~ — **Resolved in ADR 012 amendment — root-of-roots Array convention, no format change.**
18. **Class identifier portability** (ADR 012) — stable class keys required before Phase 1 image save
19. **Growable handle table** (ADR 013, #88) — fixed 1,024-entry spike table; Phase 3 blocker before Swift FFI wrapper
20. **Handle validity after `sta_vm_destroy`** (ADR 013, #89) — undefined behaviour contract; Phase 3 blocker before Swift FFI wrapper
21. **`sta_inspect_cstring` caller-provided buffer** (ADR 013, #90) — source-breaking Phase 3 change; do not add interim "fix"
22. **Event callback re-entrancy rules** (ADR 013, #91) — specify before Phase 3 Swift FFI wrapper

---

## Phase 0 complete

All seven architectural spikes are complete. ADRs 007–013 are accepted.
The next phase is **Phase 1 — Minimal Live Kernel**.

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

---

## Repo layout reminder
```
include/sta/vm.h          ← only public header (never add anything else here)
src/vm/                   ← oop_spike.h, actor_spike.h (Phase 0 spike code)
src/actor/                ← mailbox, lifecycle stubs
src/gc/                   ← Phase 1+
src/scheduler/            ← scheduler_spike.h, scheduler_spike.c
src/io/                   ← io_spike.h, io_spike.c (Spike 005 complete)
src/image/                ← image_spike.h, image_spike.c (Spike 006 complete)
src/bridge/               ← bridge_spike.h, bridge_spike.c (Spike 007 complete)
docs/decisions/           ← ADRs 001-013
docs/spikes/              ← spike-001 through spike-007
```

---

## How to orient a new chat with Claude
Paste this file plus `CLAUDE.md` at the start of the session.
Phase 0 is complete. The next session begins Phase 1 — Minimal Live Kernel.
For Phase 1 work: paste `CLAUDE.md` + this file + the relevant ADRs for the
component being built (ADR 007 for object memory, ADR 008 for mailbox,
ADR 009 for scheduler, ADR 013 for public API).
