# Smalltalk/A VM — Project Status

## What this is
C17 runtime for Smalltalk/A — an actor-concurrent, capability-aware Smalltalk
with BEAM-class density and true multi-core execution. No GIL, ever.
Full architecture: `docs/architecture/smalltalk-a-vision-architecture-v3.md`
Hard rules and toolchain: `CLAUDE.md`

---

## Current phase
**Phase 1 — Minimal Live Kernel**
Object memory, interpreter, bootstrap, image save/load.

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
8. **Closure and non-local return compatibility** (ADR 010) — design before Phase 1 block/closure support
9. **Deep-copy visited set** (ADR 008) — hash map for cycles/sharing required before Phase 1 copy implementation
10. **`ask:` future on mailbox-full** (ADR 008) — future resolution path, Phase 2
11. **Transfer buffer allocator** (ADR 008) — replace malloc stub with runtime slab, Phase 1
12. **Resume point protocol for mid-message I/O suspension** (ADR 011) — define valid suspension points before Phase 2 I/O primitives
13. **Lock-free I/O request queue** (ADR 011) — replace mutex-protected FIFO with `STA_MpscList`, Phase 2
14. **I/O backpressure integration with §9.4 bounded mailboxes** (ADR 011) — Phase 2 design question
15. **⚠ 4-byte density headroom** (ADR 012) — next `STA_Actor` addition requires a new ADR; breach of 300-byte target must be explicitly justified per CLAUDE.md
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
src/actor/                ← mailbox, lifecycle stubs
src/gc/                   ← Phase 1+
src/scheduler/            ← scheduler_spike.h, scheduler_spike.c
src/io/                   ← io_spike.h, io_spike.c (Spike 005 complete)
src/image/                ← image_spike.h, image_spike.c (Spike 006 complete)
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

### Epic 9: Kernel Source Loading — IN PROGRESS (Sessions 1-3 complete)
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
    - `kernel/SmallInteger.st` — placeholder (factorial deferred)
    - `kernel/Association.st` — key, value, key:, value:, key:value:
    - `tests/test_kernel_magnitude.c` — 21 tests for Magnitude/Number/Association
  - Modified files:
    - `src/bootstrap/kernel_load.c` — shared FileInContext with pre-registered bootstrap class ivars; added Magnitude, Number, Association to load order
    - `tests/CMakeLists.txt` — test_kernel_magnitude target
  - Deferred: SmallInteger>>factorial — nested recursive send inside binary expression triggers DNU (e.g. `1 * (0 factorial)`); individual pieces work but combination crashes. Likely interpreter expression stack issue during return from recursive call within binary send argument. Needs investigation.
  - Skipped (by design): Integer.st (no Integer class in bootstrap), //, \\ (no prims), to:do:/timesRepeat: (mutable closures), reciprocal (no / prim), gcd: (needs \\)
- Tests: 38/38 passing
- Session 5 next: Collection family, String, Stream, Exception extensions, then integration

---

## How to orient a new chat with Claude
Paste this file plus `CLAUDE.md` at the start of the session.
Phase 1 is in progress. Epics 1–8 are complete. Epic 9 is in progress (Stories 1-7 done, session 5 remains for Collection/String/Stream kernel .st files).

Epic ordering (actual):
  1. Object memory  2. Symbols/MethodDict  3. Interpreter  4. Bootstrap
  5. Object creation (basicNew/basicNew:)  6. Object/memory prims (33–41)
  7. Compiler  8. Exceptions  9. Kernel source loading  10. Image save/load
  11. Eval loop
For Phase 1 work: paste `CLAUDE.md` + this file + the relevant ADRs for the
component being built (ADR 007 for object memory, ADR 008 for mailbox,
ADR 009 for scheduler, ADR 010 for frames, ADR 013 for public API).
