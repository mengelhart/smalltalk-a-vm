# Spike: Activation Frame Layout and Tail-Call Optimisation (TCO)

**Phase 0 spike — interpreter frame representation and TCO prototype**
**Status:** Ready to execute
**Related ADRs:** 007 (object header layout), 008 (mailbox and message copy), 009 (scheduler)
**Produces:** ADR 010 (activation frame layout and TCO)

---

## Purpose

This spike answers the foundational questions of interpreter frame representation:
what an activation frame looks like in memory, how the GC locates live OOP slots
reachable from an actor's stack, and whether tail-call optimisation (TCO) can be
implemented as a one-instruction lookahead in the dispatch loop without touching
the garbage collector.

The architecture document (Appendix A) records TCO as a day-one requirement: a
`send` bytecode immediately followed by `returnTop` is a tail call; the current
frame is reused rather than a new frame pushed. The frame layout is the load-bearing
constraint: if TCO is to reuse the frame, the frame must be defined before the
interpreter is written. This spike locks that definition.

The architecture document (§9.1) also establishes reduction-based preemption with
`STA_REDUCTION_QUOTA = 1000` reductions per quantum (ADR 009). A tight tail-recursive
loop must still respect preemption — the TCO path must decrement the reduction
counter identically to a normal send. This spike validates that.

**This spike does not build the bytecode compiler, the method lookup machinery, or
a complete class system.** It builds the minimal C needed to define the frame layout
concretely, prototype TCO in a stub dispatch loop, walk the frame chain from the
GC perspective, and measure the effects on actor density. Permanent implementation
follows ADR 010.

---

## Background: relevant sketches from the architecture document

### §8.3 — Runtime model

The architecture document describes the object representation used by all heap
objects (the `STA_ObjHeader` scheme, locked in ADR 007). Activation frames are
explicitly **not** ordinary heap objects — they are a separate runtime artefact
adjacent to the object model. ADR 007 open question 5 explicitly deferred their
layout to this spike.

### §9.1 — Scheduler

Each actor receives `STA_REDUCTION_QUOTA = 1000` reductions per scheduling quantum.
The reduction counter is an `_Atomic uint32_t` field in `STA_ActorRevised` (ADR 009,
confirmed 120 bytes). When the counter reaches zero the actor yields; the scheduler
returns it to the run queue and picks the next runnable actor.

### Appendix A — TCO decision

> "Detect `send` + `returnTop` in bytecode dispatch; reuse current frame. ~6% of
> static sends are tail calls; up to 21% dynamically in compiler-heavy workloads
> (Ralston & Mason 2019). Cheap to implement from day one — one-instruction lookahead
> in the dispatch loop. Disable per-actor when a debug capability is attached.
> **Frame layout must support reuse; validated in Phase 0.**"

The one-instruction lookahead means: after decoding a `send` bytecode, peek at
`bytecode[pc + send_instruction_width]`. If the next opcode is `returnTop` (or
an equivalent tail-return), treat the send as a tail call: update receiver, PC,
and arguments in the current frame rather than allocating a new frame.

### Current actor density baseline (ADR 008 + ADR 009)

| Component | Bytes |
|---|---|
| `STA_ActorRevised` struct | 120 |
| Initial nursery slab | 128 |
| Actor identity object (0-slot header) | 16 |
| **Total per-actor creation cost** | **264** |

**Target: ~300 bytes. Headroom: 36 bytes.** The 300-byte target applies to creation
cost (zero call depth). Stack frames are an additional runtime cost; the spike must
account for them explicitly and state whether they are created lazily.

---

## Questions to answer

Each question below requires a concrete answer — not a deferral — before ADR 010
is written. The spike test binary must demonstrate the answer, not merely assert it
from reasoning.

### Q1: Activation frame header layout — Option A vs Option B

Define the in-memory layout of a Smalltalk activation frame. The frame must carry
at minimum:

- The compiled method pointer or bytecode array pointer (8 bytes on arm64)
- The receiver OOP (`self` at call time)
- The sender frame pointer (for stack walks and returns)
- The program counter (current bytecode offset)
- The argument count and local count (needed for GC slot enumeration and TCO setup)
- A reduction-counter hook slot reserved for the scheduler (see Q4)

Two layout options must be evaluated:

**Option A — plain C struct in a contiguous per-actor stack slab**

The frame header is a C struct embedded at the base of a stack slab. Arguments
and locals follow immediately after the header in the same contiguous allocation.
A per-actor `stack_base` pointer (added to `STA_ActorRevised` as a consequence —
not done in this spike, recorded in ADR 010) points to the current top of the
stack slab. The slab may be allocated lazily at first call to preserve creation
density.

```
[STA_Frame header — fixed size]
[arg_0][arg_1]...[arg_N]          <- argument OOPs, contiguous after header
[local_0][local_1]...[local_M]    <- local OOPs, contiguous after args
[STA_Frame header — caller frame]
...
```

The GC must walk this slab explicitly: it cannot use the normal `STA_ObjHeader`
traversal. A dedicated `sta_frame_gc_roots()` function iterates the frame chain
and marks every reachable OOP slot.

**Option B — heap-allocated `STA_ObjHeader` object (frame as a Smalltalk object)**

The frame header is an object with a standard `STA_ObjHeader` prefix. Its class
index points to a well-known `ActivationRecord` class entry. All frame fields —
method, receiver, sender frame pointer, PC, arg count, local count — are stored
as OOP-typed payload slots. The GC traverses frames identically to any other heap
object.

```
[STA_ObjHeader — 16-byte allocation unit]
[method_oop][receiver_oop][sender_frame_oop][pc_smallint][arg_count_smallint]
[local_count_smallint][reduction_hook_smallint]
[arg_0][arg_1]...[arg_N][local_0]...[local_M]
```

The sender frame pointer is itself an OOP referencing the caller's frame object.
The outermost frame has `nil` as its sender.

**Spike task:** implement both option structs in `src/vm/frame_spike.h`. Measure
`sizeof` and alignment for each. Evaluate the tradeoffs below and recommend one
for ADR 010.

**Tradeoffs to state explicitly:**

| Property | Option A (C struct / stack slab) | Option B (heap ObjHeader) |
|---|---|---|
| GC walkability | Requires dedicated stack-walk helper; not automatic | Free — GC traverses frames like any other object |
| Stack depth limit | Bounded by slab capacity; slab can be grown or chained | Bounded by nursery capacity; deep recursion fills nursery |
| Allocation cost | One slab allocation per actor (lazy); zero cost per frame push | One nursery allocation per frame push (header + payload) |
| TCO frame reuse | In-place C struct mutation — trivial; no allocator involved | Must "reuse" heap object; allocator must be bypassed or frame must be flagged pinned/reused |
| Debugger access | Frame chain via `sender_fp` pointer chain; OOP slots enumerated by count | Frame chain via OOP pointer; all slots naturally visible as OOPs |
| Image serialisation | Frames are transient C state; not serialised to image (actors are not suspended mid-message in image saves) | Frames are Smalltalk objects; theoretically serialisable, but mid-execution serialisation is not a Phase 0 requirement |
| Footprint per frame | `sizeof(STA_Frame)` (header only); locals via direct array arithmetic | 16-byte ObjHeader + 7 fixed OOP fields + locals; minimum 72 bytes |

---

### Q2: GC stack-walk compatibility

The GC must locate every live OOP reachable from an actor's activation stack.
Under the chosen frame layout, describe exactly how the GC locates these roots.

**Requirements:**
- Must be compatible with the `STA_ObjHeader` scheme from ADR 007
- Must respect per-actor heap isolation from ADR 008: no OOP reachable from a frame
  that belongs to a foreign actor's heap should be missed or incorrectly treated
- Must enumerate arguments and locals correctly given `arg_count` and `local_count`
  in the frame header
- A tail-call-reused frame (Q3) must be walked identically to a freshly-pushed frame

**Spike task:** implement `sta_frame_gc_roots()` in `src/vm/frame_spike.c`. This
function takes a frame pointer (or frame OOP, depending on the chosen option) and
a callback, and calls the callback with the address of every OOP slot in the frame
chain. The test must verify that no slot is visited twice and no slot is missed.

For Option A, the GC root set for an actor's stack is:
1. The actor's current frame pointer (`stack_top` field to be added to `STA_ActorRevised`).
2. Walk the frame chain via `sender_fp` until null.
3. For each frame: mark `receiver`, then mark `arg_count + local_count` OOP slots
   at `(STA_OOP*)(frame + 1)`.

For Option B, the GC root set is:
1. The actor's current frame OOP.
2. Walk the frame chain via the sender field (a payload OOP slot).
3. The normal ObjHeader traversal covers all payload slots — no special walk needed
   beyond the chain pointer.

---

### Q3: Tail-call frame reuse (TCO)

The stub interpreter loop must implement one-instruction lookahead TCO. The prototype
does not need real bytecodes — a minimal enum of `OP_SEND`, `OP_RETURN_TOP`,
`OP_PUSH_SELF`, `OP_PUSH_ARG`, and `OP_HALT` is sufficient to demonstrate the
mechanism.

**TCO detection in the dispatch loop:**

```c
case OP_SEND:
    /* peek one instruction ahead */
    if (bytecode[pc + SEND_WIDTH] == OP_RETURN_TOP) {
        /* tail call: reuse current frame */
        sta_frame_tail_call(frame, new_receiver, new_method, new_args, nargs);
        reductions--;   /* Q4: tail call still costs a reduction */
        /* do NOT push a new frame; do NOT advance pc past RETURN_TOP */
    } else {
        /* normal call: push a new frame */
        frame = sta_frame_push(frame, receiver, method, args, nargs);
        reductions--;
    }
    break;
```

**`sta_frame_tail_call()` semantics:**
- Update `frame->receiver` to the new receiver OOP
- Update `frame->method_bytecode` to the new method's bytecode
- Reset `frame->pc` to 0
- Copy new arguments into the frame's argument slots
- Update `frame->arg_count` and `frame->local_count` for the new method
  (locals may need to be zeroed if the new method has more locals)
- Do NOT alter `frame->sender_fp` — the tail call inherits the caller's return address
- Do NOT alter the reduction counter beyond the single decrement already done

**Validation requirements:**

1. **Arbitrary-depth tail recursion without stack growth.** A tail-recursive
   countdown method (pseudo-bytecode: `push_arg 0`, `push_smallint 0`, `eq?`,
   conditional-branch-to-exit, `push_arg 0`, `push_smallint 1`, `subtract`,
   `send countdown:`, `return_top`) must run to 1,000,000 iterations without
   allocating more than one frame. Verify by asserting that `stack_high_water`
   (the maximum frame depth reached) equals 1 throughout the entire countdown.

2. **Correct receiver and argument update.** After a tail call in the test loop,
   the frame's receiver and the first argument must reflect the updated values.
   Spot-check at depth 500,000.

3. **Sender frame pointer and reduction counter preserved.** The `sender_fp`
   field must remain unchanged through any number of tail calls. The reduction
   counter must decrement once per tail call, identical to a normal send.

**Measure:** run the 1,000,000-step tail-recursive countdown with and without TCO.
Without TCO, measure the maximum stack depth reached (expect: 1,000,000 frames
allocated, likely a stack overflow or OOM). With TCO, assert that stack depth
never exceeds 1.

---

### Q4: Interaction with the reduction counter

`STA_REDUCTION_QUOTA = 1000` (ADR 009). The reduction counter is `_Atomic uint32_t
reductions` in `STA_ActorRevised`. In the spike, represent it as a local variable
passed by pointer into the stub executor, mirroring the layout consequence recorded
in ADR 009.

**Invariants to validate:**

1. A tail call decrements the reduction counter exactly once — the same as a normal
   send. A tail-recursive countdown from 1,000,000 must trigger exactly 1,000,000
   reduction decrements.

2. An actor running a tight tail-recursive loop must yield after `STA_REDUCTION_QUOTA`
   reductions. The stub executor must check `reductions == 0` on the dispatch loop
   boundary and return a "preempted" status when the quota is exhausted. The test
   must verify that a 1,000,000-step countdown with a quota of 1,000 causes exactly
   1,000 preemptions (each resuming with a fresh quota), and that the countdown
   completes correctly across all preemptions.

3. When the actor is resumed after preemption, the frame state (receiver, pc, sender_fp,
   args, locals) is fully preserved. The test must verify the final result of the
   countdown is correct (0) regardless of how many preemptions occurred.

**Measurement:** count total reduction decrements during the 1,000,000-step countdown
with quota = 1,000. Assert the count equals exactly 1,000,000.

---

### Q5: Debugger and inspector access

The debugger (IDE Phase 3, capability-gated per §10.7–10.8) needs to walk a suspended
actor's frame chain and present it to the user.

**Under the chosen frame layout, state explicitly:**

1. **Frame chain traversal.** How does the debugger locate the outermost frame of a
   suspended actor? What is the termination condition for the walk? (Null `sender_fp`
   for Option A; `nil` OOP sender for Option B.)

2. **Slot presentation.** How are the receiver and each local variable at a given frame
   presented as OOPs to the inspector? Under Option A: `frame->receiver` is the
   receiver OOP; `(STA_OOP*)(frame + 1)[i]` for i in `[0, arg_count + local_count)`.
   Under Option B: all slots are payload OOPs readable by the normal header traversal.

3. **TCO-elided frames.** A tail call permanently replaces the current frame's contents
   with the callee's state. The caller's receiver, arguments, and PC at the moment of
   the tail call are gone. **This is a product decision, not an implementation detail.**
   State one of the following explicitly in ADR 010:

   - **Option TC-A (chosen for simplicity):** TCO-elided frames are permanently lost.
     The debugger sees the deepest live frame but not the sequence of tail-calling
     callers. This is consistent with Elixir/BEAM behaviour. The user can disable
     TCO per-actor when a debug capability is attached (Appendix A: "Disable per-actor
     when a debug capability is attached").

   - **Option TC-B (shadow frames):** when a debug capability is attached, TCO is
     disabled and a full frame is pushed for every call, including tail calls. The
     debugger sees the complete call chain at the cost of stack growth in debug mode.
     This is the standard approach in runtimes that support both TCO and full-fidelity
     debugging (e.g. V8, SpiderMonkey).

   ADR 010 must pick one and justify it. The spike should implement the TC-A path
   (TCO enabled, elided frames lost) since it is simpler and the TC-B path can be
   added later without layout changes.

---

### Q6: Actor density checkpoint

**At creation (0 call depth):**

| Component | Bytes |
|---|---|
| `STA_ActorRevised` struct (ADR 009) | 120 |
| Initial nursery slab | 128 |
| Actor identity object (0-slot ObjHeader) | 16 |
| **Total creation cost** | **264** |
| Target | ~300 |
| Headroom | 36 |

The `STA_ActorRevised` struct will require a `stack_base` pointer and possibly a
`stack_top` pointer when frames are introduced. These are consequences of this spike
and are recorded in ADR 010 — they are not added to the struct during this spike.
At 8–16 bytes of addition, the total creation cost remains within the 300-byte target
(the stack slab itself is lazily allocated on first call, not at creation).

**At typical call depth (10 frames, baseline measurement):**

Compute the per-actor stack cost for a 10-frame call chain under the chosen layout.
Use the following baseline assumptions:
- Average method has 3 arguments + 2 locals = 5 OOP slots per frame
- Frame header size = `sizeof(STA_Frame)` bytes (measured, not assumed)
- Stack slab is allocated lazily at first call

| Component | Bytes |
|---|---|
| Frame header × 10 | `10 × sizeof(STA_Frame)` |
| Arg/local slots × 10 (5 OOPs each) | `10 × 5 × 8 = 400` |
| **Total stack footprint at depth 10** | `10 × (sizeof(STA_Frame) + 40)` |

This is additional runtime cost beyond the 264-byte creation baseline — it is not
a creation cost. The stack slab is allocated lazily and grows on demand. Record
whether the initial slab (if any) fits within the nursery budget or requires a
separate allocation.

**ADR 010 must state the allocation strategy explicitly:**
- Is the stack slab a separate allocation from the nursery (recommended)?
- What is the initial slab size (e.g. 512 bytes = ~6 frames at the baseline)?
- Is the slab grown by re-allocation or by chaining to a new slab?

---

### Q7: Stub revision check

**`src/vm/actor_spike.h`:** The current stub (`STA_Actor`, 80 bytes) was superseded
by `STA_ActorRevised` (120 bytes) in ADR 009. The frame spike adds at least one new
field consequence: a stack pointer (or frame pointer) indicating the current top of
the frame chain. This field must be added to `STA_ActorRevised` before Phase 1.
Record as an ADR 010 consequence; do not modify the file during this spike.

Specifically:
- `void *stack_base` — bottom of the stack slab (for GC bounds check)
- `void *stack_top` — current top of the stack slab (current frame pointer)

Both are 8-byte pointers. Adding 16 bytes to `STA_ActorRevised` brings it from
120 to 136 bytes, and total creation cost from 264 to 280 bytes — still within the
300-byte target. Verify this arithmetic explicitly in the spike test and report it.

**`src/scheduler/scheduler_spike.h`:** No revision required based on the frame layout
spike. The scheduler's interface (`sta_sched_push`, the run-queue, the reduction
counter decrement in the execute loop) is unaffected by the frame layout choice.
The reduction counter check (`reductions == 0` → yield) is already present in the
stub execute loop and is exercised by Q4. Record "no revision required" explicitly
in ADR 010.

---

## What to build

All files produced by this spike are **spike code** — clearly marked, to be replaced
during Phase 1 with permanent implementations informed by ADR 010. No file produced
here is the permanent implementation.

### File 1: `src/vm/frame_spike.h`

Define the frame struct and layout constants for the chosen option (A or B), plus
the alternative struct for comparison. All relevant sizeof/offsetof values should be
derivable from this header. Include function declarations for the stub interpreter
and GC walk helper.

```
Contents (to define in implementation, not pre-baked here):
- STA_Frame          — chosen frame header struct (Option A or B)
- STA_FrameAlt       — alternative struct for sizeof comparison
- Bytecode opcode enum (OP_SEND, OP_RETURN_TOP, OP_PUSH_SELF, OP_PUSH_ARG, OP_HALT, etc.)
- STA_ExecStatus enum (STA_EXEC_HALT, STA_EXEC_PREEMPTED, STA_EXEC_ERROR)
- sta_frame_push()   — allocate and link a new frame on the stack slab
- sta_frame_pop()    — unlink and free the current frame
- sta_frame_tail_call() — update current frame for TCO send
- sta_frame_gc_roots() — walk frame chain, call callback for each live OOP slot
- sta_exec_actor()   — stub dispatch loop with TCO and reduction counter
- STA_FRAME_HEADER_SIZE, STA_FRAME_OOP_SIZE constants
```

Do not embed dispatch logic in the header. Only `static inline` layout helpers
belong here. All substantive logic lives in `frame_spike.c`.

### File 2: `src/vm/frame_spike.c`

The frame allocator, TCO send, GC walk helper, and stub dispatch loop.

Key implementation requirements:

- **`sta_frame_push()`:** carve a new frame from the stack slab at `stack_top`.
  The slab is a `malloc`'d buffer for this spike (production uses a per-actor
  slab allocator). The frame header is written first, then arg OOPs are stored
  at `(STA_OOP*)(frame + 1)[0..arg_count)`. Local slots are zero-initialised.
  The new frame's `sender_fp` points to the previous `stack_top` frame.

- **`sta_frame_pop()`:** advance `stack_top` back to `frame->sender_fp`. The slab
  memory is not freed — it is reclaimed by resetting the bump pointer (LIFO property
  of the call stack).

- **`sta_frame_tail_call()`:** update the current frame's fields in-place. The
  sender_fp is not changed. If the callee has more locals than the current frame,
  the extra slots must be zero-initialised within the existing slab allocation
  (spike simplification: assume the slab always has enough headroom; ADR 010 must
  define the policy for callee-has-more-locals-than-caller).

- **`sta_frame_gc_roots()`:** iterate the frame chain from `stack_top` to the
  sentinel (null sender_fp). For each frame, call the provided callback with the
  address of `frame->receiver` and then with the address of each arg/local slot.
  The callback signature is `void (*)(STA_OOP *slot, void *ctx)`.

- **`sta_exec_actor()`:** stub dispatch loop. Takes a frame pointer, a reduction
  counter pointer, and a quota. Dispatches opcodes in a `switch` statement.
  On `OP_SEND`: peek at the next opcode; if `OP_RETURN_TOP`, call `sta_frame_tail_call()`
  (TCO path); otherwise call `sta_frame_push()` (normal path). Decrement the
  reduction counter on every send regardless of TCO. Return `STA_EXEC_PREEMPTED`
  when the counter reaches zero. Return `STA_EXEC_HALT` on `OP_HALT`.

### File 3: `tests/test_frame_spike.c`

Correctness tests, GC walk verification, TCO depth validation, and preemption
behaviour. The test binary must print measurements and exit 0 on all checks
passing.

**Required test cases:**

1. **Layout assertions.** Print and assert `sizeof(STA_Frame)`, offsets of all
   fields, `sizeof(STA_FrameAlt)`, and the comparison table between Options A and B.
   Assert that `sizeof(STA_Frame)` is a multiple of 8 (OOP-aligned).

2. **Stack push/pop correctness.** Push 5 frames with distinct receivers and arg
   values. Assert the chain is correctly linked via `sender_fp`. Pop all 5 and
   assert the stack is empty. Assert no memory corruption (canary values).

3. **GC walk correctness.** Push 3 frames with known receiver OOPs and known
   arg/local OOP values. Call `sta_frame_gc_roots()` and collect the visited
   addresses. Assert every expected slot address appears exactly once. Assert no
   extra slots appear.

4. **TCO: constant stack depth.** Run the stub tail-recursive countdown from
   1,000,000 with TCO enabled. Assert that the maximum frame depth reached equals
   exactly 1. Assert the final result is correct.

5. **No-TCO: stack growth.** Run a non-tail-recursive stub countdown from a small
   depth (suggest: 1,000 — deep enough to confirm growth, shallow enough not to OOM).
   Assert that the frame depth reached equals 1,000. This confirms the TCO path
   is distinct from the normal path.

6. **Reduction counter: tail-call decrements.** Run the tail-recursive countdown
   from 10,000 with an effectively infinite quota (set quota to `UINT32_MAX`).
   Assert the total reduction decrements equal exactly 10,000.

7. **Preemption under tail recursion.** Run the tail-recursive countdown from 10,000
   with `STA_REDUCTION_QUOTA = 1000`. Assert that exactly 10 preemptions occur and
   that the countdown result is correct (0) after all resumes.

8. **Frame state preservation across preemption.** After each preemption in test 7,
   inspect the frame's receiver and first argument. Assert they hold the values
   expected at that point in the countdown.

9. **Actor density checkpoint.** Print the updated density table:
   - `sizeof(STA_Frame)` (frame header)
   - Per-frame cost at 5 OOP slots (header + 5 × 8 bytes)
   - Stack footprint at 10 frames
   - `sizeof(STA_ActorRevised)` + 16 bytes (projected stack pointer fields)
   - Projected total creation cost
   Assert the projected total is ≤ 300 bytes.

---

## CMake integration

Add to `tests/CMakeLists.txt`:

```cmake
add_executable(test_frame_spike
               test_frame_spike.c
               ../src/vm/frame_spike.c)
target_include_directories(test_frame_spike PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME frame_spike COMMAND test_frame_spike)
```

The spike test must pass on `ctest --output-on-failure` before ADR 010 is written.

No TSan target is required for this spike: the frame allocator and dispatch loop
are single-threaded (one actor per scheduler thread per quantum). The scheduler
integration (Q4) is tested via the stub reduction counter, not via actual thread
concurrency. If a multi-actor concurrency test is deemed necessary, it belongs in
Phase 1.

---

## Constraints

- All spike files must carry a prominent comment at the top:
  `SPIKE CODE — NOT FOR PRODUCTION`
- Do not modify `include/sta/vm.h` during this spike
- Do not modify any existing ADR (007, 008, 009)
- Do not modify `src/vm/actor_spike.h` or `src/scheduler/scheduler_spike.h` —
  record required changes as consequences in ADR 010
- `src/vm/frame_spike.h` and `.c` may include `src/vm/oop_spike.h` for the `STA_OOP`
  type and flag constants — intentional inter-spike dependency; both are spike code
- Tests may not include `src/` headers directly except through the spike header
  chain — no back-channel to `include/sta/vm.h`
- Write ADR 010 only after the spike test binary exists, ctest passes, and the
  measurements are in hand — not before

---

## Open questions this spike deliberately does not answer

These are real questions identified during spike design. They are deferred to future
ADRs so they do not block this spike.

1. **Stack slab growth policy.** The spike uses a fixed-size `malloc`'d slab. The
   production implementation must define what happens when the slab is exhausted
   (currently: grow via realloc, which invalidates all frame pointers — dangerous;
   or chain a second slab, which complicates the frame walk). The growth policy is
   a Phase 1 concern and belongs in ADR 010's consequences.

2. **Closure captures (non-local returns).** Smalltalk blocks that perform non-local
   returns (`^`) break the strict LIFO property of the call stack. A `BlockContext`
   may outlive its enclosing method. The frame layout for such blocks is a separate
   design question — the spike assumes LIFO frames only.

3. **`become:` and frame mutation.** If `receiver become: anotherObject` is called
   while a frame holds a reference to `receiver` as its receiver OOP, the OOP must
   be updated. Under Option A, `become:` must update every `frame->receiver` in the
   chain that matches. The mechanism for this is not defined here.

4. **Method inlining.** The architecture envisions eventual JIT or inlining
   optimisations. Inlined methods do not push frames. The frame layout must be
   compatible with an eventual inline frame map. Deferred to Phase 1.

5. **Exact byte count of revised `STA_ActorRevised`.** ADR 009 records
   `sizeof(STA_ActorRevised) = 120`. Adding `stack_base` + `stack_top` (16 bytes)
   brings it to 136. Verify the measured value after adding those fields; confirm
   the 300-byte target still holds. This is a Q7 consequence to record in ADR 010.

6. **Nil/True/False representation.** ADR 007 open question 1 deferred this. The
   stub dispatch loop uses SmallInt OOPs for constants. If nil/true/false are later
   assigned immediate tag patterns, no frame layout change is required (they are
   OOP-typed slots regardless).

---

## What ADR 010 must record

After running the spike, write ADR 010 covering:

- Chosen frame layout option (A or B) with measured `sizeof` values for both
- Full field layout with offsets and sizes, measured on arm64 M4 Max
- GC stack-walk strategy: how `sta_frame_gc_roots()` works, what the root-set
  scan costs relative to a normal object traversal
- TCO detection mechanism: the one-instruction lookahead, opcode width assumptions
- Validated invariant: tail-recursive loop at 1,000,000 depth, stack depth = 1
- Measured interaction with reduction counter: exact decrement count at depth 1,000,000
- Preemption validation: 10 preemptions for 10,000-step countdown at quota 1,000
- Debugger access: which of TC-A or TC-B is chosen, and the explicit product rationale
- Stub revision consequences: fields to add to `STA_ActorRevised`, projected size
- Updated actor density table: creation cost after adding stack pointer fields
- Stack slab allocation strategy: lazy vs. eager, initial size, growth policy
- Whether `src/vm/actor_spike.h` and `src/scheduler/scheduler_spike.h` need revision
  (based on Q7 — expected: actor_spike.h needs new fields; scheduler_spike.h does not)
