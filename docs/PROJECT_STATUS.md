# Smalltalk/A VM — Project Status

## What this is
C17 runtime for Smalltalk/A — an actor-concurrent, capability-aware Smalltalk
with BEAM-class density and true multi-core execution. No GIL, ever.
Full architecture: `docs/architecture/smalltalk-a-vision-architecture-v3.md`
Hard rules and toolchain: `CLAUDE.md`

---

## Current phase
**Phase 2 — Actor Runtime and Headless**
Phase 1 (Minimal Live Kernel) complete. Now: scheduler, supervision, async I/O, headless lifecycle.

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
6. ~~**Stack slab growth policy**~~ — **Resolved in ADR 014 — linked segments, 512-byte initial, 2 KB growth, deferred allocation.**
7. ~~**TCO with callee having more locals**~~ — **Resolved in ADR 014 — decline TCO when callee frame does not fit current frame space.**
8. ~~**Closure and non-local return compatibility**~~ — **Resolved in Epic 1.** Heap contexts, OP_NON_LOCAL_RETURN, BlockCannotReturn safety.
9. ~~**Deep-copy visited set**~~ — **Resolved in Epic 3.** Open-addressing hash map (initial cap 64, 70% load grow) for cycle detection and shared structure preservation.
10. ~~**`ask:` future on mailbox-full** (ADR 008)~~ — **Resolved in Epic 7A. Mailbox-full returns STA_ERR_MAILBOX_FULL, no future created.**
11. **Transfer buffer allocator** (ADR 008) — replace malloc stub with runtime slab, Phase 1
12. **Resume point protocol for mid-message I/O suspension** (ADR 011) — define valid suspension points before Phase 2 I/O primitives
13. **Lock-free I/O request queue** (ADR 011) — replace mutex-protected FIFO with `STA_MpscList`, Phase 2
14. **I/O backpressure integration with §9.4 bounded mailboxes** (ADR 011) — Phase 2 design question
15. **⚠ Density headroom consumed** (ADR 012) — `STA_Actor` grew from 200→208 bytes in Epic 7A (`pending_future_id` for ask: reply routing). Phase 1 300-byte target was a forcing function; Phase 2 targets are per ADR 014. Creation cost now 864 bytes.
16. ~~**Quiescing protocol for live actors**~~ — **Resolved in ADR 012 amendment — quiesce at safe point (end of quantum), bounded wait, Phase 1 trivially correct.**
17. ~~**Root table for multi-root images**~~ — **Resolved in ADR 012 amendment — root-of-roots Array convention, no format change.**
18. ~~**Class identifier portability**~~ — **Resolved in ADR 012 amendment — fixed indices 0-31, name-based resolution for 32+.**
19. **Growable handle table** (ADR 013, #88) — fixed 1,024-entry spike table; Phase 3 blocker before Swift FFI wrapper
20. **Handle validity after `sta_vm_destroy`** (ADR 013, #89) — undefined behaviour contract; Phase 3 blocker before Swift FFI wrapper
21. **`sta_inspect_cstring` caller-provided buffer** (ADR 013, #90) — source-breaking Phase 3 change; do not add interim "fix"
22. **Event callback re-entrancy rules** (ADR 013, #91) — specify before Phase 3 Swift FFI wrapper

---

## Phase 0 complete

All seven architectural spikes are complete. ADRs 007–014 are accepted.

---

## Phase 1 progress

### Epic 1: Object Memory and Allocator — COMPLETE
- GitHub: Epic #108, stories #109–#113 (all closed)
- Branch: `phase1/epic-1-object-memory` (merged to main)
- Production files:
  - `src/vm/oop.h`, `src/vm/oop.c` — OOP typedef, STA_ObjHeader (12 bytes / 16-byte alloc unit), SmallInt/Character/heap tagging, gc_flags/obj_flags bit definitions
  - `src/vm/immutable_space.h`, `src/vm/immutable_space.c` — mmap-backed bump allocator for shared immutable region, mprotect seal after bootstrap
  - `src/vm/heap.h`, `src/vm/heap.c` — actor-local bump allocator (single-heap Phase 1 variant, API accepts context parameter for Phase 2 per-actor heaps)
  - `src/vm/special_objects.h`, `src/vm/special_objects.c` — 32-entry special object table per bytecode spec §11.1
  - `src/vm/class_table.h`, `src/vm/class_table.c` — class table with 32 reserved indices per bytecode spec §11.5, atomic backing pointer for Phase 2 growth
- Tests: 12/12 passing (5 new production + 7 existing spike tests)

### Epic 2: Symbol Table and Method Dictionary — COMPLETE
- GitHub: Epic #115, stories #116–#120 (all closed)
- Branch: `phase1/epic-2-symbols-method-dict` (merged to main)
- Production files:
  - `src/vm/symbol_table.h`, `src/vm/symbol_table.c` — Symbol object layout (class_index 7, FNV-1a hash in slot 0, packed UTF-8 in slots 1+), symbol table with open-addressing linear probe, idempotent interning in immutable space, power-of-two capacity with 70% load-factor growth
  - `src/vm/method_dict.h`, `src/vm/method_dict.c` — MethodDictionary (class_index 15) with backing Array, selector→method lookup by identity, insert with 70% load-factor growth and rehash
  - `src/vm/special_selectors.h`, `src/vm/special_selectors.c` — Bootstrap interning of all 32 special selectors (bytecode spec §10.7), populates SPC_SPECIAL_SELECTORS Array and individual SPC entries (doesNotUnderstand:, cannotReturn:, mustBeBoolean, startUp, shutDown, run)
- Tests: 14/14 passing (7 production + 7 existing spike tests)

### Epic 3: Bytecode Interpreter Core — COMPLETE
- GitHub: Epic #123, stories #124–#131 (all closed)
- Branch: `phase1/epic-3-interpreter` (merged to main)
- Production files:
  - `src/vm/compiled_method.h`, `src/vm/compiled_method.c` — CompiledMethod layout (header word with bitfield encoding: numArgs, numTemps, numLiterals, primitiveIndex, hasPrimitive, largeFrame), header decode/encode macros, literal/bytecode accessors, builder allocating in immutable space
  - `src/vm/frame.h`, `src/vm/frame.c` — STA_Frame (48 bytes: method, receiver, sender, saved_sp, pc, arg_count, local_count, flags), STA_StackSlab bump allocator, frame push/pop with expression stack pointer save/restore, inline expression stack ops (push/pop/peek/depth), GC root enumeration
  - `src/vm/selector.h`, `src/vm/selector.c` — Selector arity helper: binary (first char is special), keyword (count colons), unary (neither)
  - `src/vm/primitive_table.h`, `src/vm/primitive_table.c` — 256-entry primitive function table, kernel primitives: SmallInt #+→1 #-→2 #<→3 #>→4 #=→7 #*→9, Object #==→29 #class→30, Array #at:→51 #at:put:→52 #size→53, overflow-safe arithmetic via `__builtin_*_overflow`
  - `src/vm/interpreter.h`, `src/vm/interpreter.c` — Full dispatch loop with all Phase 1 opcodes (push/pop/store/dup, jumps, sends, returns, primitives, OP_WIDE), message dispatch with class hierarchy walk, two-stage primitive dispatch (header check → prim call), TCO via send-return lookahead (ADR 010/014), reduction counting (quota=1000)
- Tests: 19/19 passing (5 new production + 14 existing)
- Milestone: **"3 + 4 = 7" executes through full send/dispatch/primitive path**

### Epic 4: Bootstrap — Metaclass Circularity and Kernel Wiring — COMPLETE
- GitHub: Epic #98 (closed)
- Branch: `phase1/epic-4-bootstrap` (merged to main)
- Production files:
  - `src/bootstrap/bootstrap.h`, `src/bootstrap/bootstrap.c` — Full bootstrap: nil/true/false allocation, symbol interning, Tier 0 metaclass circularity (10 objects), Tier 1 class creation (26 classes + metaclasses, all 32 reserved indices), character table (256 tagged Character OOPs), SystemDictionary with class bindings, kernel primitive method installation, hand-assembled boolean conditional methods
- Modified files:
  - `src/vm/primitive_table.h/c` — Added prims 42 (yourself), 120 (respondsTo:), 121 (doesNotUnderstand:); fixed prim 30 (class) for real class OOPs; added `sta_primitive_set_class_table()`
  - `src/vm/interpreter.c` — Nil-aware superclass chain walk (terminates on nil_oop), block activation frames for prims 81–84, real DNU protocol (sends #doesNotUnderstand: instead of aborting)
  - `CMakeLists.txt` — bootstrap.c added to sta_vm library
- Tests: 20/20 passing (19 existing + 11 new bootstrap tests)
- Milestone: **Bootstrapped Smalltalk object system — interpreter uses real class objects with method dictionaries through the full dispatch path**

### Epic 5: basicNew / basicNew: — Smalltalk-Level Object Creation — COMPLETE
- GitHub: Epic #136, stories #137–#141 (all closed)
- Branch: `phase1/epic-5-object-creation` (merged to main)
- New files:
  - `src/vm/format.h` — Class format field encoding/decoding, query helpers (is_indexable, is_bytes, is_instantiable); format type constants extracted from bootstrap.h
- Modified files:
  - `src/vm/oop.h` — Added STA_BYTE_PADDING_MASK / STA_BYTE_PADDING() for byte-indexable object size tracking in ObjHeader.reserved bits 2:0
  - `src/vm/class_table.h/c` — Added sta_class_table_index_of() reverse lookup (OOP → class index)
  - `src/vm/primitive_table.h/c` — Prims 31 (basicNew) and 32 (basicNew:), sta_primitive_set_heap() for allocation context
  - `src/bootstrap/bootstrap.h` — Format macros replaced with `#include "format.h"`
  - `src/bootstrap/bootstrap.c` — Installed Behavior>>basicNew, basicNew:, new, new: and Object>>initialize in step 7
- Tests: 19/19 Phase 1 tests passing (5 new test files, 47 new tests)
  - `test_format.c` — format field decode for all 31 kernel classes, encode/decode round-trip (7 types × 256 instVars)
  - `test_basic_new.c` — prim 31: fixed-size allocation, failure cases, heap exhaustion, class_table_index_of
  - `test_basic_new_size.c` — prim 32: pointer-indexable, byte-indexable with padding boundary cases, failure cases
  - `test_new_methods.c` — interpreter-level: Association new, Array new: 10, Object new, class identity, respondsTo:
  - `test_object_creation.c` — end-to-end: store/retrieve, size check, multi-object arrays, chained creates
- Milestone: **Smalltalk code can create objects via new/new:/basicNew/basicNew: through normal message dispatch — first time the interpreter reads the class format field to determine allocation shape**

### Epic 6: Object and Memory Primitives (prims 33–41) — COMPLETE
- GitHub: Epic #144, stories #145–#150 (all closed)
- Branch: `phase1/epic-6-object-memory-prims` (merged to main)
- Modified files:
  - `src/vm/primitive_table.h/c` — Added prims 33 (basicAt:), 34 (basicAt:put:), 35 (basicSize), 36 (hash), 37 (become:), 38 (instVarAt:), 39 (instVarAt:put:), 40 (identityHash), 41 (shallowCopy); get_receiver_format() helper
  - `src/bootstrap/bootstrap.c` — Installed 9 primitive methods on Object in step 7; dict capacity 16→32
- Tests: 26/26 passing
- Milestone: **Complete Object protocol (prims 29–42) — all Blue Book §8.5 object/memory primitives implemented**

### Epic 7: Compiler (Scanner, Parser, Codegen) — COMPLETE
- GitHub: Epic #153 (closed), stories #154–#155 + codegen/integration stories (all closed)
- Branch: `phase1/epic-7-compiler` (merged to main)
- New files:
  - `src/compiler/scanner.h`, `src/compiler/scanner.c` — Pull-model lexer for Smalltalk method source: 22 token types, comment skipping, negative number handling, symbol literals (#foo, #+, #at:put:, #'string'), character literals, literal array start (#(), keyword tokens scanned one piece at a time, peek support
  - `src/compiler/ast.h`, `src/compiler/ast.c` — 17 AST node types (NODE_METHOD, NODE_RETURN, NODE_ASSIGN, NODE_SEND, NODE_SUPER_SEND, NODE_CASCADE, NODE_VARIABLE, NODE_LITERAL_*, NODE_BLOCK, NODE_SELF), cascade message struct, recursive sta_ast_free()
  - `src/compiler/parser.h`, `src/compiler/parser.c` — Recursive descent parser: standard Smalltalk precedence (unary > binary > keyword), method header parsing (unary/binary/keyword), temporaries, assignment, cascades, clean blocks with args and temps, non-local return rejection with clear error message
  - `src/compiler/codegen.h`, `src/compiler/codegen.c` — Bytecode generator: all §5 reference compilations, 10 inlined control structures (§5.6 ifTrue:, ifFalse:, ifTrue:ifFalse:, ifFalse:ifTrue:, whileTrue:, whileFalse:, whileTrue, whileFalse, and:, or:), literal table management, temp/arg slot allocation, jump backpatching
  - `src/compiler/compiler.h`, `src/compiler/compiler.c` — Top-level compile API: `sta_compile_method` (source → installed CompiledMethod), `sta_compile_expression` (source → executable method for eval)
- Modified files:
  - `src/vm/interpreter.c` — OP_BLOCK_COPY implementation for block activation
  - `CMakeLists.txt` — compiler sources and test targets added
- Tests: 30/30 passing (61 scanner/parser + 40 codegen + 10 integration)
- Milestone: **Smalltalk source compiles to bytecode and executes through the interpreter — `^3 + 4` compiles and returns 7**

### Epic 8: Exception Handling — COMPLETE
- GitHub: Epic #160, stories #161–#167 (all closed)
- Branch: `phase1/epic-8-exceptions`
- New files:
  - `src/vm/handler.h`, `src/vm/handler.c` — Actor-local handler stack (linked list of STA_HandlerEntry), push/pop/walk/find operations, isKindOf: superclass chain match, signaled exception global storage for setjmp/longjmp safety
- Modified files:
  - `src/vm/primitive_table.h/c` — Added prims 88 (BlockClosure>>on:do: — setjmp/longjmp handler install, body eval, signal transfer), 89 (Exception>>signal — handler chain walk, longjmp to matching on:do:), 90 (BlockClosure>>ensure: — body eval, ensure block eval, body result return; Phase 1: normal completion only)
  - `src/vm/interpreter.c` — Block arg tempOffset fix (BlockClosure slot 4, BlockDescriptor slot 3), sta_eval_block uses tempOffset for correct arg placement
  - `src/compiler/codegen.c` — BlockDescriptor 4th slot emits tempOffset for block arg indexing
  - `src/bootstrap/bootstrap.c` — Exception hierarchy classes (Exception, Error, MessageNotUnderstood, BlockCannotReturn) created in step 4 with instance variables; step 8: Exception accessors (messageText, messageText:, signal), MNU accessors (message, message:, receiver, receiver:), Object>>doesNotUnderstand: compiled from Smalltalk source (replaces prim 121 stub), BlockClosure>>on:do: and ensure: installed
- Tests: 32/32 passing (12 new exception tests + 2 handler unit tests)
  - `test_handler.c` — handler stack push/pop/walk, isKindOf: matching
  - `test_exceptions.c` — on:do: normal completion, signal caught, superclass match, messageText access, exception accessors, DNU creates MNU (receiver + message accessible), ensure: normal + body result preserved, nested handlers (inner catches), ensure+on:do: Phase 1 limitation documented
- Phase 1 limitation: ensure: block does NOT fire during exception unwinding (longjmp bypasses it). Phase 2 will fix this.
- Milestone: **Full exception handling — on:do:, signal, ensure:, doesNotUnderstand: with real Smalltalk MessageNotUnderstood objects**

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

## Repo layout reminder
```
include/sta/vm.h          ← only public header (never add anything else here)
src/vm/                   ← Phase 1 production code + Phase 0 spike code
  oop.h, oop.c                ← OOP typedef, ObjHeader, tagging macros (production)
  immutable_space.h/c         ← shared immutable region allocator (production)
  heap.h/c                    ← actor-local heap allocator (production)
  special_objects.h/c         ← 32-entry special object table (production)
  class_table.h/c             ← class table with reserved indices (production)
  format.h                    ← class format field encoding/decoding (production)
  symbol_table.h/c            ← symbol interning and FNV-1a hash (production)
  method_dict.h/c             ← method dictionary with backing Array (production)
  special_selectors.h/c       ← bootstrap interning of 32 special selectors (production)
  compiled_method.h/c         ← CompiledMethod layout, header macros, builder (production)
  frame.h/c                   ← activation frame, stack slab, expression stack (production)
  selector.h/c                ← selector arity helper (production)
  primitive_table.h/c         ← 256-entry primitive table, kernel primitives (production)
  interpreter.h/c             ← bytecode dispatch loop, message send, TCO (production)
  handler.h/c                 ← exception handler stack, setjmp/longjmp support (production)
  oop_spike.h, actor_spike.h  ← Phase 0 spike code (exploratory, not promoted)
  frame_spike.h/c             ← Phase 0 spike code
src/actor/                ← actor struct, mailbox, message send, deep copy
  actor.h/c                   ← STA_Actor with per-actor heap/slab/mailbox, lifecycle, dispatch (production)
  mailbox.h/c                 ← MPSC Vyukov mailbox (production)
  mailbox_msg.h/c             ← message envelope (production)
  deep_copy.h/c               ← cross-actor deep copy engine (production)
src/gc/                   ← Phase 1+
src/scheduler/            ← work-stealing scheduler (Epic 4)
  scheduler.h/c               ← N-thread scheduler, overflow queue, dispatch loop (production)
  deque.h/c                   ← Chase-Lev work-stealing deque (production)
  scheduler_spike.h/c         ← Phase 0 spike (reference only)
src/io/                   ← io_spike.h, io_spike.c (Spike 005 complete)
src/image/                ← production image save/load + spike reference
  image.h/c                   ← production writer + loader (Epic 10)
  image_spike.h/c             ← Phase 0 spike (reference only, not modified)
src/bridge/               ← bridge_spike.h, bridge_spike.c (Spike 007 complete)
src/bootstrap/            ← one-time kernel bootstrap + file-in reader
  bootstrap.h/c               ← kernel bootstrap (Epics 4/5/8) (production)
  filein.h/c                  ← chunk-format file-in reader (Epic 9) (scaffolding)
  kernel_load.h/c             ← dependency-ordered .st file loader (Epic 9) (scaffolding)
kernel/                   ← Smalltalk kernel source files (.st chunk format)
  Object.st, True.st, False.st, UndefinedObject.st
src/compiler/             ← scanner, parser, AST, codegen (Epic 7)
  scanner.h/c                 ← pull-model lexer for method source (production)
  ast.h, ast.c                ← AST node types and recursive free (production)
  parser.h/c                  ← recursive descent parser (production)
  codegen.h/c                 ← bytecode generator, control structure inlining (production)
  compiler.h/c                ← top-level compile API (production)
docs/decisions/           ← ADRs 001-014
docs/spikes/              ← spike-001 through spike-007
```

### Epic 9: Kernel Source Loading — COMPLETE
- GitHub: Epic #169
- Stories 1-2 (session 1): Class creation primitive (prim 122), chunk format reader
  - Branch: phase1/epic-9a-filein-infra (merged)
  - New: `src/bootstrap/filein.h/c`, `tests/fixtures/simple.st`, `tests/fixtures/escaped.st`, `tests/test_class_creation.c`, `tests/test_filein.c`
  - Modified: `src/vm/primitive_table.h/c` (prim 122), `src/vm/class_table.h/c` (alloc_index), `src/bootstrap/bootstrap.c` (Class>>subclass:...)
- Stories 3-4 (session 2): Public API (sta_vm_load_source), smoke test
  - Branch: phase1/epic-9b-public-api (merged)
  - New: `src/vm/vm.c` (sta_vm_load_source), `tests/test_load_source.c`, `tests/test_smoke_filein.c`, `tests/fixtures/smoke.st`
- Stories 5-6 (session 3): Eval-path fix, kernel load infrastructure + first .st files
  - Branch: phase1/epic-9c-kernel-source-1
  - New files:
    - `src/bootstrap/kernel_load.h/c` — sta_kernel_load_all() loads .st files in dependency order
    - `kernel/Object.st` — isNil, notNil, =, ~=, subclassResponsibility, error:
    - `kernel/True.st` — not, &, |, printString
    - `kernel/False.st` — not, &, |, printString
    - `kernel/UndefinedObject.st` — isNil, notNil, printString, ifNil:, ifNotNil:, ifNil:ifNotNil:
    - `tests/test_kernel_load.c` — 18 tests for kernel methods
  - Modified files:
    - `src/compiler/parser.c` — TOKEN_VBAR (|) accepted as binary selector in method headers and expressions
    - `tests/test_smoke_filein.c` — 2 new eval-path tests (eval-path DNU bug resolved)
  - Story 5: Eval-path DNU bug from session 2 resolved (was already fixed in prior work)
  - Story 6: Boolean.st skipped (Boolean class not in bootstrap; not/&/| added directly to True/False)
- Story 7 (session 4): Magnitude, Number, SmallInteger, Association kernel .st files
  - Branch: phase1/epic-9d-kernel-source-2
  - New files:
    - `kernel/Magnitude.st` — >=, <=, max:, min:, between:and:
    - `kernel/Number.st` — isZero, positive, negative, negated, abs, sign
    - `kernel/SmallInteger.st` — factorial
    - `kernel/Association.st` — key, value, key:, value:, key:value:
    - `tests/test_kernel_magnitude.c` — 24 tests for Magnitude/Number/SmallInteger/Association
  - Modified files:
    - `src/bootstrap/kernel_load.c` — shared FileInContext with pre-registered bootstrap class ivars; added Magnitude, Number, SmallInteger, Association to load order
    - `tests/CMakeLists.txt` — test_kernel_magnitude target
  - Bug fix (task/fix-nested-send-dnu): sta_frame_push was allocating new frames at slab->top, but the caller's expression stack extends above slab->top to slab->sp; non-primitive sends with pending expression stack values (e.g. receiver for a binary send) corrupted those values. Fix: advance slab->top to slab->sp before allocating the new frame.
  - Skipped (by design): Integer.st (no Integer class in bootstrap), //, \\ (no prims), to:do:/timesRepeat: (mutable closures), reciprocal (no / prim), gcd: (needs \\)
- Stories 8-12 (session 5): whileTrue: verification, Collection family, String, integration
  - Branch: phase1/epic-9e-kernel-source-3
  - Story 8: Confirmed whileTrue: inlining allows outer temp access — both condition/body blocks compile inline via emit_block_body_inline(), so temp references resolve as normal OP_PUSH_TEMP/OP_POP_STORE_TEMP
  - Story 9: Collection family kernel .st files
    - New files:
      - `kernel/Collection.st` — do:, collect:, select:, reject:, detect:ifNone: (all subclassResponsibility), isEmpty, notEmpty
      - `kernel/SequenceableCollection.st` — do:, collect:, select:, reject:, detect:ifNone: (whileTrue:-based iteration), includes:, first, last
      - `kernel/ArrayedCollection.st` — (empty, placeholder for hierarchy)
    - Modified files:
      - `src/bootstrap/bootstrap.c` — size (prim 53) moved from Array to ArrayedCollection; Symbol format instvar_count 0→1 (hash slot)
      - `src/vm/primitive_table.c` — prim 53 now byte-aware: checks sta_format_is_bytes() and computes (h->size - instVars) * 8 - padding
      - `src/vm/symbol_table.c` — alloc_symbol sets byte padding in h->reserved for correct size computation
      - `src/bootstrap/kernel_load.c` — added Collection, SequenceableCollection, ArrayedCollection, String to load order
    - Clean-block limitation: blocks passed to do:/collect:/select: etc. can only use their own args + literals. Mutable outer temp capture requires Phase 2 closures. collect:, select:, reject:, detect:ifNone: all work (blocks are pure functions of their arg).
  - Story 10: String kernel source
    - New: `kernel/String.st` — printString (minimal; byte-aware at:/at:put: primitives deferred)
    - String size works via prim 53 byte-aware path + ArrayedCollection inheritance
  - Story 11: Streams — deferred (ReadStream/WriteStream not in bootstrap class hierarchy)
  - Story 12: Integration tests — 28 new tests, all passing
    - New: `tests/test_kernel_collections.c` — whileTrue: inlining (2), Collection/Array (14), String (5), integration smoke (7)
  - Modified: `tests/test_format.c` — Symbol instvar_count updated 0→1
- Tests: 39/39 passing (38 existing + 1 new test_kernel_collections target)
- **Epic 9 complete.** 12 kernel .st files, 39 CTest targets.

### Epic 10: Image Save/Load (Production) — COMPLETE
- GitHub: Epic #190, stories #191–#196 (all closed)
- Branch: `phase1/epic-10-image-save-load`
- New files:
  - `src/image/image.h` — Production image format structs (STA_ImageHeader 48B, STA_ObjRecord 16B, STA_ImmutableEntry 10B, STA_RelocEntry 8B), OOP encoding macros, root table constants, FNV-1a hash, writer and loader declarations
  - `src/image/image.c` — Production writer (sta_image_save_to_file) and loader (sta_image_load_from_file)
  - `tests/test_image_format.c` — 13 format struct and encoding unit tests
  - `tests/test_image_save.c` — 7 save smoke tests (bootstrap → save → verify file structure)
  - `tests/test_image_roundtrip.c` — 16 round-trip acid tests (bootstrap → save → fresh state → load → interpreter works)
- Modified files:
  - `src/vm/symbol_table.h/c` — Added sta_symbol_table_register() for image loader to rebuild symbol table index
  - `CMakeLists.txt` — image.c added to sta_vm library
  - `tests/CMakeLists.txt` — 3 new test targets
- Writer algorithm:
  - Constructs root Array (special objects, class table, globals) in immutable space
  - Walks reachable object graph with O(1) hash table (replaces spike's O(n²) linear scan)
  - Handles byte-indexable objects (Symbol/String/ByteArray) — raw bytes not scanned as OOPs
  - Handles CompiledMethod — only scans header + literal slots, not bytecodes
  - Emits immutable name entries for nil, true, false, and all interned symbols
  - Writes ADR 012 format: header → immutable section → data section → relocation table
- Loader algorithm (6 passes):
  - Validate header (magic, version, endian, ptr_width)
  - Read immutable section entries
  - Allocate all objects via production allocators (sta_heap_alloc / sta_immutable_alloc, not malloc)
  - Fill payload words from encoded values
  - Apply relocations (patch heap pointers)
  - Rebuild runtime tables: special objects, class table, symbol table index, globals
- Key numbers: 461 objects, 135 immutables, 41,803 bytes image after bootstrap + kernel load
- Acid test: `3 + 4 = 7`, `true ifTrue: [42] ifFalse: [0] = 42`, `(4 * 5) + 2 = 22` all pass after save → fresh state → load
- Tests: 42/42 passing (39 existing + 3 new targets)
- Public API wiring (sta_vm_save_image/sta_vm_load_image) deferred to Epic 11 when STA_VM struct is fleshed out
- **Epic 10 complete.** Image round-trip works — interpreter executes Smalltalk after loading from image.

### Epic 11: Eval Loop, Public API, and Phase 1 Capstone — COMPLETE
- GitHub: Epic #197, stories #198–#206 (all closed)
- Branch: `phase1/epic-11-eval-loop`
- New files:
  - `src/vm/vm_internal.h` — Private STA_VM struct definition (heap, immutable_space, symbol_table, class_table, stack_slab, last_error, inspect_buffer, config, state flags). Audit of global state documented in header comment.
  - `tests/test_vm_lifecycle.c` — 6 tests: bootstrap create, image round-trip, NULL config, double destroy, last error, kernel methods
  - `tests/test_capstone.c` — 20 capstone tests through public API only: arithmetic (5), boolean (5), blocks (2), collections (2), identity (3), exceptions (1), error cases (1), image round-trip (1)
- Modified files:
  - `src/vm/vm.c` — Full implementations: sta_vm_create() with load-or-bootstrap pipeline, sta_vm_destroy() with reverse-order teardown, sta_vm_last_error() with per-VM and static fallback, sta_vm_save_image/load_image through STA_VM struct, sta_vm_load_source with legacy NULL-vm path
  - `src/vm/eval.c` — sta_eval() (compile expression + interpret + return handle), sta_inspect_cstring() (C-level formatter for SmallInt, nil, true, false, Symbol, String, Array, generic objects)
  - `examples/embed_basic/main.c` — Real working example: create VM, eval "3+4", "true ifTrue:[42]", "10 factorial", inspect results, destroy
  - `CMakeLists.txt` — KERNEL_DIR compile definition on sta_vm library target
  - `tests/CMakeLists.txt` — test_vm_lifecycle and test_capstone targets
- Handle model (Phase 1): STA_Handle* is raw STA_OOP cast to pointer. sta_handle_retain/release are no-ops. Phase 2 will introduce real handle table per ADR 013.
- Global state audit: 9 globals identified (special_objects, primitives, handler). Single VM instance at a time for Phase 1. Phase 2 will move to per-instance state.
- Tests: 44/44 passing (42 existing + 2 new targets)
- embed_basic output: `3 + 4 = 7`, `true ifTrue: [42] ifFalse: [0] = 42`, `10 factorial = 3628800`
- **Epic 11 complete. Phase 1 — Minimal Live Kernel is DONE.**

---

## Phase 1 summary

**Phase 1 — Minimal Live Kernel: COMPLETE** (2026-03-16)

11 epics, 44 CTest targets, 7 architectural spikes, 14 ADRs.

Key milestones:
- Epic 3: "3 + 4 = 7" through full send/dispatch/primitive path
- Epic 4: Bootstrapped Smalltalk object system with metaclass circularity
- Epic 7: Smalltalk source compiles to bytecode and executes
- Epic 8: Full exception handling (on:do:, signal, ensure:, doesNotUnderstand:)
- Epic 9: 12 kernel .st files loaded, kernel methods work
- Epic 10: Image save/load round-trip — interpreter works after loading from image
- Epic 11: Public API complete — sta_vm_create/destroy/eval/inspect_cstring/save_image/load_image all wired

The VM can: bootstrap from scratch, load kernel source, compile and execute Smalltalk expressions, save and load images, and expose all functionality through a clean C public API (`include/sta/vm.h`).

---

## Phase 1.5 progress

### Phase 1.5 Batch 1: Arithmetic Completion — COMPLETE
- Branch: phase1.5/batch-1-arithmetic
- New primitives: 5 (<=), 6 (>=), 8 (~=), 10 (/), 11 (\\\\), 12 (//),
  13 (quo:), 14 (bitAnd:), 15 (bitOr:), 16 (bitXor:), 17 (bitShift:),
  200 (SmallInteger>>printString)
- Scanner fix: backslash added to binary character set for \\\\ selector
- New/expanded kernel .st: SmallInteger (even, odd, gcd:, lcm:),
  Number (to:do:, to:by:do:, timesRepeat:)
- Tests: test_arithmetic_prims (30 tests), test_batch1_integration (18 tests)
- Key validations:
  - Blue Book floor division semantics (// and \\\\) correct
  - Bit operations with sign extension correct
  - SmallInteger printString works end-to-end via C primitive (prim 200)
  - gcd: Euclidean algorithm works (46368 gcd: 28657 = 1, ~24 recursive calls; not tail-recursive due to temp pattern)
  - to:do: and timesRepeat: run with real block value: dispatch under iteration
  - to:do: mutable capture limitation documented (requires Phase 2 closures)

### Phase 1.5 Batch 2: Byte-Indexable Primitives, Character, String — COMPLETE
- Branch: phase1.5/batch-2-byte-char-string
- New primitives: 60 (basicAt:), 61 (basicAt:put:), 62 (basicSize),
  63 (stringAt:), 64 (stringAt:put:), 94 (Character>>value),
  95 (Character class>>value:)
- Closes: GitHub issue #188 (byte-aware at:/at:put:)
- New kernel .st: Character.st, ByteArray.st
- Expanded: String.st (concatenation, reversed, asUppercase,
  asLowercase, copyFrom:to:, includes:, =, <, printString)
- Tests: test_byte_prims (15 tests), test_batch2_integration (25 tests)
- Key validations:
  - Byte-indexable at:/at:put: works end-to-end
  - Character immediates extract and create correctly
  - String concatenation allocates new byte-indexable objects
  - Polymorphic at: (Array→OOP, String→Character, ByteArray→SmallInt)
  - Polymorphic printString across SmallInteger/String/Character/ByteArray

### Phase 1.5 Batch 3: Collection Completion — COMPLETE
- Branch: phase1.5/batch-3-collections (merged to main)
- New primitives: 54 (replaceFrom:to:with:startingAt: — OOP + byte paths, memmove overlap safety), 83 (value:value: for 2-arg blocks)
- New kernel .st: OrderedCollection.st, Array.st
- Expanded: Collection.st (inject:into:, anySatisfy:, allSatisfy:, asArray stubs),
  SequenceableCollection.st (inject:into:, anySatisfy:, allSatisfy:, reverseDo:,
  with:collect:, indexOf:, indexOf:ifAbsent:, copyFrom:to:, copyWith:,
  copyWithout:, asArray)
- Tests: test_replace_prim (6 tests), test_batch3_collections (24 tests)
- Key validations:
  - 4-argument primitive dispatch (prim 54 replaceFrom:to:with:startingAt:)
  - OrderedCollection grow cycle (10 elements into capacity-4 backing array)
  - Array printString deep call chains (polymorphic element printing, nested arrays)
  - inject:into: with 2-arg block (value:value: prim 83 dispatch)
  - replaceFrom:to:with:startingAt: self-overlap safety (memmove)
  - Cuis Smalltalk sources consulted as reference for all implementations

### Phase 1.5 Batch 4: Number Protocol, Symbol, and Hashing — COMPLETE
- Branch: phase1.5/batch-4-number-symbol (merged to main)
- New primitives: 91 (String>>asSymbol), 92 (Symbol>>asString),
  93 (Symbol class>>intern:)
- New kernel .st: Symbol.st (printString, asSymbol, concatenation)
- Expanded: Object.st (isNumber, isInteger), Number.st (isNumber),
  SmallInteger.st (isInteger, asInteger, printString:, printStringHex,
  printStringOctal, printStringBinary),
  String.st (hash — content-based, 31-multiply with 29-bit mask),
  Association.st (hash — delegates to key hash)
- Codegen fix: string literals now allocate as proper String objects
  (class_index = STA_CLS_STRING) in immutable space, fixing Symbol/String
  type confusion that caused 'hi' printString to return '#hi'
- Tests: test_batch4_symbol (12 tests), test_batch4_hash (10 tests),
  test_batch4_number (30 tests), test_batch4_integration (11 tests)
- Key validations:
  - Symbol interning round-trip (String→Symbol→String)
  - Hash consistency across String/Symbol boundary
  - printString:base: with hex/octal/binary radixes
  - gcd: TCO on deep recursion (46368 gcd: 28657 = 1)
  - Hash protocol ready for Dictionary/Set (Batch 5 or Phase 2)

### Phase 1.5 Batch 5: Integration Stress Tests — COMPLETE
- Branch: phase1.5/batch-5-stress-tests (merged to main)
- No new primitives or kernel source
- 4 new test files: test_stress_dispatch, test_stress_depth,
  test_stress_exceptions, test_stress_strings
- 59 new tests exercising feature combinations:
  - Polymorphic dispatch (7+ classes, same selector: printString, =, hash, size)
  - Deep call chains (15+ frames via nested Array printString)
  - 20 factorial (20 recursive frames + printString of 19-digit result)
  - Sustained allocation pressure (50 string concatenations, 100-element OC grow)
  - inject:into: over 100-element array (100 iterations of value:value:)
  - Exception propagation through collection iteration
  - Nested exception handlers (inner catch → re-signal → outer catch)
  - DNU caught on SmallInteger, String, nil
  - String comparison boundary cases (empty, length tie-break)
  - Character classification sweep (128 characters)
  - Hash distribution validation (10 strings, ≥8 distinct hashes)
  - printString:base: sweep (hex, binary, octal, negative, zero)
- Bugs discovered:
  - #243: SmallInteger = with non-SmallInt arg returns receiver instead of false
  - #244: Catching DNU via on:do: triggers unhandled BlockCannotReturn
- Total CTest targets: 58/58 passing (2 tests KNOWN_FAIL/skipped with filed issues)

## Phase 1.5 — Class Library Foundation: COMPLETE

5 batches, 20 new primitives (1–17, 54, 60–64, 81–83, 88–95, 100, 120–122,
200, 91–93), 17 kernel .st files (Object, True, False, UndefinedObject,
Magnitude, Number, SmallInteger, Association, Collection, SequenceableCollection,
ArrayedCollection, Character, String, Symbol, ByteArray, Array, OrderedCollection),
58 CTest targets all passing. Ready for Phase 2 — Actor Runtime.

---

## Phase 2 progress

### Epic 0: Per-Instance VM State — COMPLETE
- Branch: phase2/epic-0-per-instance-vm-state (Stories 1-3)
- Branch: phase2/epic-0-stories-4-6 (Stories 4-6)
- Key changes:
  - STA_VM flat struct owns all runtime state
  - STA_ExecContext passed to all primitives (vm + actor pointers)
  - 9 mutable globals eliminated; 5 g_prim_* setter functions deleted
  - Real handle table per ADR 013 (reference-counted, growable slab allocator)
  - Handler state (handler_top, signaled_exception) on STA_VM
    (moves to STA_Actor in Epic 3)
- Tests: 59 CTest targets passing, ASan clean
- Remaining mutable statics (approved):
  - g_last_error (vm.c) — pre-VM-creation error reporting
  - g_fallback_specials (special_objects.c) — fallback before VM exists

### Epic 1: Full Closures (Captured Variables, NLR, ensure: Unwinding) — COMPLETE
- GitHub: Epic #255, stories #256–#261 (all closed)
- Branch: phase2/epic-1-closures
- Key changes:
  - Capture analysis pre-pass in codegen detects which methods need heap contexts
  - Heap-allocated context objects (STA_CLS_ARRAY) for captured variable sharing
  - Frame-level redirection: effective_slots() returns context payload or inline frame slots
  - OP_CLOSURE_COPY creates 6-slot BlockClosure (adds context reference)
  - OP_BLOCK_COPY unchanged for clean blocks — zero overhead for non-capturing code
  - OP_NON_LOCAL_RETURN walks sender chain to home method frame (MARKER flag)
  - BlockCannotReturn signaled when home method already returned
  - Parser now accepts ^ inside blocks (was rejected in Phase 1)
  - Codegen emits OP_NON_LOCAL_RETURN for ^ in blocks
  - ensure: registers on handler chain; prim_signal fires ensure: blocks during unwinding
  - STA_Frame gains context field (56 bytes, was 48)
  - STA_HandlerEntry gains is_ensure and ensure_block fields
  - TCO skipped for context methods (context lifecycle too complex for frame reuse)
  - sta_compiled_method_create_with_header for custom header flags (needsContext = largeFrame bit)
- New opcodes implemented: OP_CLOSURE_COPY (0x61), OP_NON_LOCAL_RETURN (0x35),
  OP_PUSH_OUTER_TEMP (0x62), OP_STORE_OUTER_TEMP (0x16), OP_POP_STORE_OUTER_TEMP (0x17)
- Tests: 17 new closure tests, 61 CTest targets total (60 passing + 1 disabled), ASan clean
- Closes deferred items: "do: with mutable-capture blocks", "ensure: during exception unwinding"
- Resolves open decision #8: Closure and NLR compatibility (ADR 010)

### Epic 2: Actor Struct, Lifecycle & Per-Actor Heaps — COMPLETE
- GitHub: Epic #263, stories #264–#269 (all closed)
- Branch: phase2/epic-2-actor-struct
- New files:
  - `src/actor/actor.h` — Production STA_Actor struct: per-actor STA_Heap, STA_StackSlab, handler chain (handler_top, signaled_exception), lifecycle state machine (CREATED/READY/RUNNING/SUSPENDED/TERMINATED), actor_id, behavior_class, NULL placeholders for mailbox and supervisor, vm back-pointer
  - `src/actor/actor.c` — sta_actor_create/sta_actor_terminate with heap+slab init/teardown; public API stubs retained
  - `tests/test_actor_lifecycle.c` — 9 lifecycle tests
  - `tests/test_actor_heap.c` — 6 heap isolation tests
  - `tests/test_actor_epic2.c` — 12 comprehensive tests (lifecycle, execution in actors, density measurement)
- Modified files:
  - `src/vm/vm_state.h` — STA_VM gains root_actor field; ExecContext comment updated
  - `src/vm/vm.c` — Root actor created after bootstrap, takes ownership of VM's heap/slab/handler chain via struct copy; sta_vm_destroy cleans up root actor; save/load image use root actor's heap
  - `src/vm/interpreter.c` — interpret_loop, sta_interpret, sta_eval_block resolve slab/heap from root actor
  - `src/vm/handler.h/c` — New _ctx API: sta_handler_push/pop/find/set_signaled/get_signaled _ctx variants that resolve through STA_ExecContext (actor when set, VM fallback for bootstrap)
  - `src/vm/primitive_table.c` — resolve_heap(ctx)/resolve_slab(ctx) helpers; all allocating primitives use them; exception prims (88/89/90) use _ctx handler API
  - `src/vm/eval.c` — sta_compile_expression uses root actor's heap
  - `src/bootstrap/filein.c` — filein_heap() helper for post-bootstrap source loading
- Density measurement:
  - sizeof(STA_Actor) = 112 bytes (under 164-byte ADR 014 target — scheduler/IO/snapshot fields not yet added)
  - Creation cost = 768 bytes (112 struct + 128 nursery + 512 stack + 16 identity)
  - 3.5x more compact than BEAM (2704 bytes)
- Sanitizers: ASan clean; LSan (detect_leaks) not supported on macOS ARM — deferred to Linux CI with Valgrind
- Tests: 63 CTest targets passing (27 new actor-specific tests across 3 test files)

### Epic 3: Mailbox & Message Send — COMPLETE
- GitHub: Epic #271, stories #272–#277 (all closed)
- Branch: phase2/epic-3-mailbox
- New files:
  - `src/actor/mailbox.h`, `src/actor/mailbox.c` — Production MPSC mailbox: Vyukov lock-free linked list with atomic capacity counter, bounded (default 256), drop-newest overflow per ADR 008. STA_Mailbox (32 bytes), internal STA_MbNode separation (node vs message). Enqueue multi-producer safe, dequeue single-consumer.
  - `src/actor/mailbox_msg.h`, `src/actor/mailbox_msg.c` — Message envelope: STA_MailboxMsg (selector + args + arg_count + sender_id), malloc/free lifecycle.
  - `src/actor/deep_copy.h`, `src/actor/deep_copy.c` — Cross-actor deep copy engine: recursive graph copier with open-addressing visited set hash map (initial cap 64, 70% load grow). Immediates pass through, immutables shared by pointer, mutable objects deep copied. Byte-indexable objects memcpy'd (not scanned as OOPs). Cycle-safe, sharing-preserving. Resolves open decision #9.
  - `tests/test_mailbox.c` — 15 mailbox + envelope tests
  - `tests/test_deep_copy.c` — 15 deep copy tests (cycles, sharing, nesting, byte objects)
  - `tests/test_actor_send.c` — 9 send API tests (deep copy, isolation, overflow)
  - `tests/test_actor_dispatch.c` — 6 dispatch tests (method lookup, args, state mutation)
  - `tests/test_epic3_integration.c` — 9 end-to-end tests (multi-actor chains, edge cases)
- Modified files:
  - `src/actor/actor.h` — STA_Actor gains STA_Mailbox (replaces void* placeholder), behavior_obj field, sta_actor_send_msg and sta_actor_process_one declarations
  - `src/actor/actor.c` — Mailbox init/destroy wired into actor lifecycle; sta_actor_send_msg (deep copy + enqueue); sta_actor_process_one (dequeue + method lookup + dispatch via interpreter)
  - `src/vm/vm.c` — Root actor mailbox initialized in sta_vm_create
  - `CMakeLists.txt` — mailbox.c, mailbox_msg.c, deep_copy.c added to sta_vm library
- Design decisions resolved:
  - Open decision #9: Deep-copy visited set — open-addressing hash map implemented
- Tests: 68 CTest targets passing (63 existing + 5 new test files), ASan clean
- Density: STA_Mailbox adds 32 bytes to STA_Actor; stub sentinel node (16 bytes) allocated separately

### Epic 4: Work-Stealing Scheduler — COMPLETE
- GitHub: Epic #279, stories #280–#286 (all closed)
- Branch: phase2/epic-4-scheduler
- New files:
  - `src/scheduler/scheduler.h`, `src/scheduler/scheduler.c` — Production scheduler: N OS threads (default = CPU cores via sysconf), per-thread dispatch loops, overflow queue for external enqueue, idle-thread wakeup via pthread_cond_signal with 5ms timeout
  - `src/scheduler/deque.h`, `src/scheduler/deque.c` — Chase-Lev fixed-capacity work-stealing deque (1024 slots), adapted from TSan-clean Spike 003 Variant A. Owner push/pop bottom (LIFO), stealers CAS top (FIFO)
  - `tests/test_scheduler.c` — 7 lifecycle tests
  - `tests/test_scheduler_auto.c` — 7 auto-scheduling tests
  - `tests/test_preemption.c` — 5 preemption tests
  - `tests/test_scheduler_mt.c` — 6 multi-threaded tests
  - `tests/test_work_stealing.c` — 5 work-stealing tests
  - `tests/test_eval_integration.c` — 6 eval+scheduler integration tests
  - `tests/test_scheduler_stress.c` — 11 stress tests (up to 1000 actors × 100 msgs on 16 cores)
- Modified files:
  - `src/actor/actor.h` — STA_Actor gains `_Atomic uint32_t state` (was plain uint32_t), `next_runnable` (intrusive run-queue link), `saved_frame` (preemption save point)
  - `src/actor/actor.c` — Auto-schedule on message arrival (CAS CREATED/SUSPENDED → READY), `sta_actor_process_one_preemptible` for scheduler dispatch
  - `src/vm/interpreter.h` — `STA_INTERPRET_COMPLETED`/`PREEMPTED` status codes, `sta_interpret_actor`/`sta_interpret_resume` entry points
  - `src/vm/interpreter.c` — `interpret_loop_ex` with `sched_actor` parameter: preemption at OP_SEND and OP_JUMP_BACK safe points. sta_eval path unchanged (NULL sched_actor = no preemption)
  - `src/vm/vm_state.h` — STA_VM gains `scheduler` field
  - `src/vm/vm.c` — Auto-stop/destroy scheduler on vm_destroy
- Key design decisions:
  - Reduction-based preemption: STA_REDUCTION_QUOTA=1000, preempt at OP_SEND (before arg pop) and OP_JUMP_BACK (loop boundary) — frame state always consistent for resume
  - Actor exclusivity: CAS READY→RUNNING ensures only one thread executes an actor
  - sta_eval synchronous on calling thread, root actor not scheduled
  - Overflow queue for external enqueues (Chase-Lev push is owner-only)
  - Preempted actors re-enqueue to local deque (LIFO, cache-warm)
- Known constraint: exception primitives (on:do:, signal, ensure:) call sta_eval_block which resolves from root_actor — not yet safe for scheduled actors
- Stress test results (M4 Max, 16 cores):
  - 200 actors × 500 msgs = 100,000 messages, ~20k steals, all 16 workers active
  - 1000 actors × 100 msgs = 100,000 messages, ~20k steals, all 16 workers active
- Sanitizers: TSan clean (all 7 test files), ASan clean (all 7 test files)
- Tests: 75 CTest targets passing (68 existing + 7 new test files)

### Epic 5: Per-Actor Garbage Collection — COMPLETE
- GitHub: Epic #288, stories #289–#294 (all closed)
- Branch: phase2/epic-5-gc
- New files:
  - `src/gc/gc.h` — GC public API: sta_gc_collect, sta_heap_alloc_gc, sta_heap_grow, STA_GCStats struct (24 bytes)
  - `src/gc/gc.c` — Cheney semi-space copying collector: gc_copy (copy one object from→to space), gc_scan_object (update OOP slots), gc_scan_roots (stack frames, handler chain, actor OOP fields), forwarding pointer mechanics (STA_GC_FORWARDED + first payload word, side table for zero-payload objects), heap growth policy (>75% survivors → 2x, >50% → 1.5x), GC statistics
  - `tests/test_gc.c` — 8 core GC unit tests
  - `tests/test_gc_heap.c` — 7 heap integration tests
  - `tests/test_gc_roots.c` — 6 root enumeration correctness tests
  - `tests/test_gc_safety.c` — 5 GC safety audit tests
  - `tests/test_gc_scheduled.c` — 6 multi-threaded stress tests
  - `tests/test_gc_stats.c` — 6 diagnostics tests
- Modified files:
  - `src/actor/actor.h` — STA_Actor gains STA_GCStats gc_stats (24 bytes inline)
  - `src/actor/actor.c` — sta_actor_send_msg uses GC-aware allocation (sta_heap_alloc_gc, sta_deep_copy_gc)
  - `src/actor/deep_copy.h/c` — Added sta_deep_copy_gc (GC-aware variant with vm/actor pointers)
  - `src/vm/interpreter.c` — GC_SAVE_FRAME macro before allocation sites; 6 allocation sites audited and fixed for GC safety (DNU Message/Array, NLR BlockCannotReturn, context alloc in sta_interpret/sta_interpret_actor)
  - `src/vm/primitive_table.c` — prim_alloc helper (GC-aware allocation for primitives); all allocating primitives (31, 32, 41, 122, 200, 92) wired through prim_alloc
- Algorithm: Cheney semi-space copying collector, per-actor, stop-the-world within actor only
  - Trigger: allocation failure → GC → retry → grow → retry → OOM
  - Roots: stack frames (method, receiver, context, args, temps, expression stack), handler chain (exception_class, handler_block, ensure_block), actor struct (behavior_class, behavior_obj, signaled_exception)
  - Pointer classification: SmallInt/Character → skip, immutable space → skip (address range check), from-space → copy/forward, to-space → leave, NULL → skip
  - Scannable slots: format-aware (byte-indexable → instVars only, CompiledMethod → header + literals only)
  - Forwarding: gc_flags |= STA_GC_FORWARDED, new address in first payload word; side table hash map for zero-payload objects
  - Growth: survivors > 75% → double, > 50% → 1.5x; post-GC sta_heap_grow with full pointer fixup
- GC statistics (inline in STA_Actor, 24 bytes):
  - gc_count: number of collections (cumulative)
  - gc_bytes_reclaimed: cumulative bytes freed
  - gc_bytes_survived: bytes surviving most recent GC
  - current_heap_size: read from actor->heap.capacity (not stored)
- Density: sizeof(STA_Actor) = 184 bytes (was 160, +24 for GC stats)
- Stress test results (M4 Max, 16 cores):
  - 100 actors × 500 messages (8 allocs per message, retain 2): max_heap=512B, all retained objects valid
  - GC + preemption: 8 actors × 2000-iteration allocating loops, preempted and resumed correctly
  - Concurrent GC all cores: 32 actors on 16 threads, 100 messages each, no corruption
- Sanitizers: ASan clean (all test files), TSan clean (all test files)
- Deferred: immutable space atomic bounds for concurrent GC reads (GitHub #296)
- Tests: 81 CTest targets passing (75 existing + 6 new test files, 38 new tests)

### Fix #295: Deep Copy Heap Growth — COMPLETE
- Branch: fix/295-deep-copy-heap-growth
- Problem: Deep copy during cross-actor message send could fail if the target heap was too small for the payload, even after GC (no garbage to collect on a fresh actor).
- Solution: Pre-flight size estimation in `sta_actor_send_msg`. Before deep copying, walks all arg graphs in a single pass to estimate total allocation cost. If the target heap lacks sufficient free space, grows it once upfront (used + estimate * 1.5 breathing room).
- New API: `sta_deep_copy_estimate()` and `sta_deep_copy_estimate_roots()` in deep_copy.h/c — same traversal rules as deep copy (skip immediates, immutables, visited-set for DAG/cycles), measurement only.
- Design choice: pre-flight only grows (no GC) because mailbox-referenced objects are not GC roots and would be incorrectly collected. Per-object `sta_heap_alloc_gc` remains as defense-in-depth during actual copy.
- Known limitation: `STA_MailboxMsg.args` holds raw C pointers into the target heap; if heap grows between sends (without draining), prior message pointers become stale. In production, the scheduler drains promptly; a future fix would make mailbox references GC-aware.
- Tests: 11 new (6 estimation unit tests + 5 send tests including 128-byte→816-byte growth). ASan clean.
- Total: 82 CTest targets passing

### Epic 6: Supervision — COMPLETE
- GitHub: Issues #311, #312, #313 (all closed)
- Branch: phase2/epic-6-supervision
- New files:
  - `src/actor/supervisor.h` — STA_SupervisorData, STA_ChildSpec, restart strategies (enum moved to sta/vm.h)
  - `src/actor/supervisor.c` — sta_supervisor_init, add_child, handle_failure, data_destroy, restart intensity limiting, escalation helpers
  - `tests/test_supervisor_linkage.c` — 5 linkage tests
  - `tests/test_failure_detection.c` — 6 failure detection tests
  - `tests/test_restart.c` — 5 restart strategy tests
  - `tests/test_stop_escalate.c` — 7 stop/escalate tests
  - `tests/test_restart_intensity.c` — 8 intensity limiting tests
  - `tests/test_supervision_tree.c` — 8 root supervisor and tree tests
  - `tests/test_supervision_stress.c` — 5 scheduler-based stress tests (multi-threaded, full core count)
- Modified files:
  - `include/sta/vm.h` — STA_RestartStrategy enum, sta_vm_spawn_supervised declaration
  - `src/vm/vm_state.h` — STA_VM gains root_supervisor field, event callback table, sta_vm_fire_event declaration, actor registry, next_actor_id counter
  - `src/vm/vm.c` — Root supervisor creation in sta_vm_create, teardown in sta_vm_destroy, sta_vm_spawn_supervised, sta_event_register/unregister/fire_event, registry lifecycle
  - `src/actor/actor.h` — STA_Actor gains supervisor and sup_data fields (16 bytes), STA_ACTOR_MSG_EXCEPTION return code, STA_ERR_ACTOR_DEAD
  - `src/actor/actor.c` — handle_actor_exception (TERMINATED + notify supervisor + drain mailbox), supervisor-aware dispatch, registry-based send, zero-copy fast path, exception class name extraction
  - `src/actor/registry.h/c` — VM-wide actor_id→STA_Actor* hash map (mutex-protected)
  - `src/actor/mailbox_msg.h/c` — args_owned flag for zero-copy sends
- Design decisions:
  - OTP-style supervision: restart, stop, escalate strategies per child spec
  - Restart intensity limiting: configurable max_restarts within max_seconds window
  - Root supervisor never terminates — fires event and resets counters on intensity exceeded
  - Teardown order: scheduler stop → supervision tree (depth-first) → root actor → cleanup
  - Event system: sta_event_register/unregister with 16-slot callback table, sta_vm_fire_event internal API
  - Actor registry: actor_id→STA_Actor* indirection layer; sta_actor_send_msg resolves by ID
  - Zero-copy fast path: immediates/immutables skip target heap entirely (malloc'd args array)
  - Lightweight notifications: SmallInt actor_id + immutable Symbol reason, zero supervisor heap allocation
- sizeof(STA_Actor) = 200 bytes (184 + 16 for supervisor/sup_data pointers)
- Known issues (filed separately):
  - ~~#316: spec->current_actor pointer race during supervisor restart~~ — **Resolved** by actor reference counting (see cleanup sprint below)
  - ~~#317: mailbox destroy races with in-flight enqueue during restart~~ — **Resolved** by actor reference counting (see cleanup sprint below)
- Tests: 90 CTest targets, all passing
- Removed: scheduler_spike disabled test target (superseded by production scheduler tests)

### Actor Registry and Safe Send — COMPLETE
- GitHub: #313 (closed)
- Branch: phase2/epic-6-supervision (continuation)
- Merged via PR #315
- Changes:
  - `src/actor/registry.h/c` — VM-wide actor_id→STA_Actor* hash map with mutex protection
  - `src/actor/actor.c` — sta_actor_send_msg resolves targets by registry lookup; zero-copy fast path for immediates/immutables
  - `src/actor/mailbox_msg.h/c` — args_owned flag for zero-copy sends
- All sends now go through registry indirection — no raw STA_Actor* pointer chasing

### Lightweight Supervisor Notifications — COMPLETE
- GitHub: #311, #312 (closed)
- Merged via PR #318
- Changes:
  - Supervisor failure notifications use SmallInt actor_id + immutable Symbol reason
  - Zero heap allocation on supervisor during childFailed processing
  - Event system: sta_event_register/unregister with 16-slot callback table

### Actor Reference Counting and Race Fixes — COMPLETE
- GitHub: #316, #317, #319 (all resolved)
- Branch: task/actor-refcount
- New/modified files:
  - `src/actor/actor.h` — STA_Actor gains `_Atomic uint32_t refcount` (fits in existing padding, sizeof unchanged at 200 bytes)
  - `src/actor/actor.c` — sta_actor_release (decrement + free on zero), sta_actor_create initializes refcount=1 (registry ref), registry lookup increments refcount, scheduler dispatch holds reference during processing. Removed drain_mailbox from sta_actor_terminate (was racing with active dispatch on other threads — sta_mailbox_destroy handles drain on final free).
  - `src/actor/registry.c` — sta_registry_lookup returns retained reference (caller must release)
  - `src/actor/mailbox.c` — Enqueue count upgraded to memory_order_release; sta_mailbox_count upgraded to memory_order_acquire (closes store-buffer visibility gap)
  - `src/scheduler/scheduler.c` — Holds refcount during dispatch; checks for TERMINATED state after processing (prevents overwriting TERMINATED with READY/SUSPENDED); seq_cst fence after SUSPENDED store closes Dekker-style store-buffer race (#319)
  - `src/actor/actor.c` — seq_cst fence in sta_actor_send_msg between enqueue and state CAS (matching fence pair with scheduler, closes #319)
  - `tests/test_actor_refcount.c` — 4 refcount lifecycle tests
  - `tests/test_supervision_stress.c` — Capture spec IDs before scheduler start; removed KNOWN_FAIL on test_mass_restart (now reliable)
- Design:
  - Registry owns the base reference (refcount=1 at creation)
  - Every lookup (registry, scheduler dequeue) increments refcount
  - Every release decrements; actor freed when refcount reaches 0
  - sta_actor_terminate: unregisters + releases registry ref, but struct lives until last holder releases
  - Terminate-while-running safety: scheduler checks for externally-set TERMINATED after dispatch, skips re-enqueue
- sizeof(STA_Actor) = 200 bytes (unchanged — refcount fits in padding after existing atomic state field)
- Density: creation cost = 856 bytes (200 struct + 128 nursery + 512 stack + 16 identity)
- Sanitizers: TSan clean (91/91), ASan clean (86/86, 5 spike tests excluded due to hardcoded TSan flags)
- Stability: 20/20 consecutive runs of test_supervision_stress
- Tests: 91 CTest targets, all passing
- Known issues (filed during cleanup):
  - #320: Actor registered before fully initialized — behavior_obj is zero during registration window
  - #321: restart_child skips scheduling when scheduler is stopping — new child stranded in CREATED

### Epic 7A: Futures Infrastructure — COMPLETE
- Branch: phase2/epic-7a-futures
- New files:
  - `src/actor/future.h`, `src/actor/future.c` — STA_Future struct (40 bytes): CAS state machine with intermediate-state pattern (PENDING→RESOLVING→RESOLVED, PENDING→FAILING→FAILED). Refcounted, malloc'd, freed on last release.
  - `src/actor/future_table.h`, `src/actor/future_table.c` — STA_FutureTable (88 bytes): VM-wide future_id→STA_Future* hash map. Mutex-protected open-addressing with linear probe, shift-back deletion, grow at 70% load. Initial capacity 256.
  - `tests/test_future.c` — 10 tests (create, resolve, fail, double-resolve, race, refcount, lookup, remove, grow, destroy cleanup)
  - `tests/test_ask_send.c` — 6 tests (ask creates future, dead actor, mailbox full, envelope future_id, deep copy args, fire-and-forget zero)
  - `tests/test_reply_routing.c` — 7 tests (immediate reply, immutable reply, mutable reply with transfer heap, fire-and-forget no routing, future-already-failed, manual round trip, scheduled round trip)
- Modified files:
  - `src/vm/vm_state.h` — STA_VM gains `STA_FutureTable *future_table`
  - `src/vm/vm.c` — Create/destroy future table in VM lifecycle
  - `src/actor/mailbox_msg.h` — STA_MailboxMsg gains `uint32_t future_id` (+4 bytes, 0=fire-and-forget)
  - `src/actor/actor.h` — STA_Actor gains `uint32_t pending_future_id` (+8 bytes with padding, 200→208); declares `sta_actor_ask_msg`
  - `src/actor/actor.c` — `sta_actor_ask_msg` (ask send path), `route_reply` (reply routing after COMPLETED)
  - `src/actor/deep_copy.h`, `src/actor/deep_copy.c` — `sta_deep_copy_to_transfer` for mutable return values
  - `src/vm/interpreter.c` — `sta_interpret_actor`/`sta_interpret_resume` push return value onto slab on COMPLETED
  - `CMakeLists.txt` — future.c, future_table.c added to sta_vm sources
- Design decisions:
  - D1: VM-wide future table (mutex-protected hash map, same pattern as actor registry)
  - D2: future_id field on STA_MailboxMsg (4 bytes, 0 = fire-and-forget)
  - D3: Deferred copy via malloc'd transfer buffer + optional transfer_heap
  - D4: Mailbox-full on ask: returns STA_ERR_MAILBOX_FULL, no future created (resolves open decision #10)
  - D5: Intermediate-state CAS pattern (PENDING→RESOLVING→RESOLVED) — TSan-clean, no data race on non-atomic fields
- sizeof(STA_Future) = 40 bytes, sizeof(STA_FutureTable) = 88 bytes
- sizeof(STA_Actor) = 208 bytes (was 200, +4 for pending_future_id + 4 padding)
- Density: creation cost = 864 bytes (208 struct + 128 nursery + 512 stack + 16 identity)
- Tests: 23 new, 94 total CTest targets (all passing). ASan clean. TSan clean on multi-threaded tests.
- Note: Epic 7B (wait primitive, crash-triggered future failure, stress tests) follows

---

## Current phase
**Phase 2 — Actor Runtime and Headless**
Scheduler, supervision, async I/O, headless lifecycle.

---

## How to orient a new chat with Claude
Paste this file plus `CLAUDE.md` at the start of the session.
Phase 1 is complete. Phase 2 (Actor Runtime and Headless) is next.

Epic ordering (actual, all 11 complete):
  1. Object memory  2. Symbols/MethodDict  3. Interpreter  4. Bootstrap
  5. Object creation (basicNew/basicNew:)  6. Object/memory prims (33–41)
  7. Compiler  8. Exceptions  9. Kernel source loading  10. Image save/load
  11. Eval loop + public API + capstone ✅

### Deferred items from Phase 1
- **Byte-aware at:/at:put: for String/ByteArray** — prim 51/52 read OOP slots, not bytes. Need dedicated byte-indexable primitives.
- **Stream classes (ReadStream, WriteStream)** — not in bootstrap; require file-in class creation or bootstrap addition.
- **OrderedCollection** — needs instance variable tracking and grow-on-add semantics.
- ~~**do: with mutable-capture blocks**~~ — **Done in Epic 1.** Full closure support with heap-allocated context objects.
- ~~**ensure: during exception unwinding**~~ — **Done in Epic 1.** ensure: registers on handler chain; prim_signal fires ensure: blocks during unwinding.
- ~~**Real handle table**~~ — **Done in Epic 0.** Slab-based, reference-counted, growable per ADR 013.
- ~~**Per-instance state**~~ — **Done in Epic 0.** All mutable globals migrated to STA_VM struct.

For Phase 2 work: paste `CLAUDE.md` + this file + the relevant ADRs for the
component being built (ADR 008 for mailbox, ADR 009 for scheduler,
ADR 010 for frames, ADR 011 for async I/O, ADR 013 for public API).
