# ADR 010 — Activation Frame Layout and Tail-Call Optimisation (TCO)

**Status:** Accepted
**Date:** 2026-03-11
**Spike:** `docs/spikes/spike-004-frame-layout.md`
**Depends on:** ADR 007 (object header layout), ADR 008 (mailbox), ADR 009 (scheduler)

---

## Decision

The Smalltalk/A activation frame uses **Option A: a plain C struct embedded in a
contiguous per-actor stack slab**, not a heap-allocated `STA_ObjHeader` object.

TCO is implemented as a **one-instruction lookahead in the dispatch loop**: when
`bytecode[pc + STA_SEND_WIDTH] == OP_RETURN_TOP`, the current frame is reused
in-place via `sta_frame_tail_call()` rather than pushing a new frame.

The debugger policy is **TC-A: TCO-elided frames are permanently lost**. When a
debug capability is attached (Phase 3, §10.7–10.8), TCO is disabled per-actor and
every call pushes a full frame — no layout change required.

---

## Measured results (arm64 M4 Max, Apple clang 17, C17, `-g`)

All numbers from `test_frame_spike` via `ctest`.

### Frame layout (Option A)

| Field | Offset | Size |
|---|---|---|
| `method_bytecode` | 0 | 8 bytes |
| `receiver` | 8 | 8 bytes |
| `sender_fp` | 16 | 8 bytes |
| `pc` | 24 | 4 bytes |
| `arg_count` | 28 | 2 bytes |
| `local_count` | 30 | 2 bytes |
| `reduction_hook` | 32 | 4 bytes |
| `_pad` | 36 | 4 bytes |
| *(struct total)* | — | **40 bytes** |

First payload OOP slot at `frame + 40`: 8-byte aligned. ✓

### Option B comparison (STA_FrameAlt — rejected)

`sizeof(STA_FrameAlt) = 72 bytes` (16-byte header allocation unit + 7 fixed OOP
fields). Each additional arg or local slot adds 8 bytes, same as Option A. The
fixed overhead is 32 bytes higher (72 vs 40) for the same 0-slot frame. Option B
is rejected as the default; see Rationale below.

### TCO validation

| Test | Result |
|---|---|
| Tail-recursive countdown from 1,000,000 | `max_depth == 1` throughout ✓ |
| Non-TCO countdown from 1,000 | `max_depth == 1001` ✓ |
| TCO sends decrement reduction counter | 10,000 sends → 10,000 decrements ✓ |
| Preemption under tail recursion (10,000 countdown, quota=1,000) | 10 preemptions ✓ |
| Frame state after each preemption | `args[0]` matches `10000 - k*1000` for preemption k ✓ |
| Final result correctness | `self_oop` returned after 10 preemptions + resume ✓ |

### GC stack-walk correctness

Three-frame chain (innermost: 1 arg + 2 locals; middle: 2 args; outermost: 1 arg +
1 local). `sta_frame_gc_roots()` visits exactly 10 OOP slots, each address visited
exactly once, in innermost-first order. ✓

### Actor density checkpoint

| Component | Bytes |
|---|---|
| `STA_ActorRevised` (ADR 009, current) | 120 |
| `stack_base` + `stack_top` fields (consequence) | +16 |
| Projected `STA_Actor` with stack pointers | **136** |
| Initial nursery slab | 128 |
| Actor identity object (0-slot header) | 16 |
| **Projected total creation cost** | **280** |
| Target | ~300 |
| Headroom | **20 — see density warning in Consequences** |

Stack slab is allocated lazily at first call — it is not a creation cost. The
300-byte density target applies to the zero-frame creation cost of 280 bytes. ✓
**Warning:** 20 bytes is the thinnest headroom across all Phase 0 spikes. Spike 005
(libuv async I/O) is expected to add 8–16 bytes to `STA_Actor`, consuming most or
all of this margin. If the 300-byte target is breached, a new ADR must justify it
per CLAUDE.md. See the Consequences section for the full scenario table.

---

## Frame layout: Option A (C struct in stack slab)

### Struct

```c
/* SPIKE CODE — NOT FOR PRODUCTION. See ADR 010. */
typedef struct STA_Frame {
    const uint8_t    *method_bytecode; /* 8 — pointer to bytecode array        */
    STA_OOP           receiver;        /* 8 — receiver OOP (self)              */
    struct STA_Frame *sender_fp;       /* 8 — sender frame (null = outermost)  */
    uint32_t          pc;              /* 4 — current bytecode offset          */
    uint16_t          arg_count;       /* 2 — number of argument slots         */
    uint16_t          local_count;     /* 2 — number of local variable slots   */
    uint32_t          reduction_hook;  /* 4 — reserved for scheduler (ADR 009) */
    uint32_t          _pad;            /* 4 — align payload to 8-byte boundary */
} STA_Frame;                           /* sizeof = 40 bytes                    */
```

Payload slots (args then locals) follow immediately after the struct at `frame + 1`.

### Allocation per frame

```
Total bytes = sizeof(STA_Frame) + (arg_count + local_count) * sizeof(STA_OOP)
            = 40 + n * 8   (always a multiple of 8)
```

### Stack slab

The slab is a contiguous `malloc`'d buffer. Frames are bump-allocated from the
bottom upward. Deallocation is a bump-pointer retract (LIFO, matching the call
stack's natural structure). Spike uses a fixed-size slab; production implementation
must define a growth or chaining policy (see Open Questions).

---

## GC stack-walk strategy

The GC locates the root set for an actor's activation stack as follows:

1. Read `STA_Actor.stack_top` — a pointer to the current executing frame (see
   Consequences: new field to add to `STA_ActorRevised`).
2. Call `sta_frame_gc_roots(stack_top, visit_fn, ctx)`.
3. `sta_frame_gc_roots` iterates the `sender_fp` chain until `NULL`:
   - For each frame: call `visit(&frame->receiver)`, then call `visit(&slots[i])`
     for `i` in `[0, arg_count + local_count)`.

**ADR 007 compatibility:** all visited values are `STA_OOP` — either SmallInt
immediates or heap pointers. SmallInts pass through the visitor without effect (the
GC ignores them by checking `STA_IS_SMALLINT`). Heap pointers are the actual roots.

**ADR 008 compatibility:** per-actor heap isolation means no OOP slot in an actor's
frame can reference a foreign actor's heap (cross-actor sends use deep copy). The GC
does not need to check for cross-actor references during stack walks.

**TCO-reused frames are walked identically to freshly-pushed frames.** After
`sta_frame_tail_call()`, the frame's `receiver`, `arg_count`, and arg slots are all
updated to reflect the callee. The `sender_fp` and `local_count` fields are
unchanged. There is no distinguishable difference from the GC's perspective.

**Cost:** `O(total_slots_in_all_frames)` — proportional to the live working set,
not the stack depth alone. This is the same asymptotic cost as any generational GC's
root-set scan.

---

## TCO detection and frame reuse

### Detection

In the dispatch loop, every `OP_SEND` decodes the next instruction before
dispatching:

```c
bool is_tail = (bytecode[pc + STA_SEND_WIDTH] == OP_RETURN_TOP);
```

`STA_SEND_WIDTH = 2` (one opcode byte + one nargs byte). This is a single array
access — the lookahead adds zero branch cost beyond the normal dispatch.

### Tail-call path

```c
/* args[] already popped from eval stack */
(*reductions)--;           /* reduction cost is identical to a normal send */
sta_frame_tail_call(frame, new_receiver, new_method, nargs, args);
eval_top = 0;              /* reset eval stack for new method body */
slab->top_frame = frame;   /* frame stays at depth 1 */
if (*reductions == 0) return STA_EXEC_PREEMPTED;
/* continue dispatch loop at frame->pc == 0 */
```

### `sta_frame_tail_call` semantics

- Updates `frame->receiver`, `frame->method_bytecode`, `frame->pc` (→ 0),
  `frame->arg_count`, and argument slots.
- `frame->sender_fp` is **not** changed — the tail call inherits the caller's
  return address.
- `frame->local_count` is preserved in the spike. Production: must zero additional
  locals if the callee method has more than the current frame (see Open Questions).

### Validated invariant

A tail-recursive countdown from 1,000,000: `max_depth == 1` throughout the entire
execution. The slab bump pointer never advances beyond the first frame's allocation.

---

## Reduction counter interaction

**A tail call costs exactly one reduction — identical to a normal send.** The
counter is decremented before preemption is checked, after the frame update is
committed. This means:

- A tight tail-recursive loop cannot run forever — it yields after
  `STA_REDUCTION_QUOTA` reductions.
- Preemption happens at a clean boundary: the current frame is at PC=0 of the new
  method body. Resume is `sta_exec_actor(slab, slab->top_frame, &reductions)` with
  a fresh quota.

**Validated:** 10,000-step tail-recursive countdown at quota=1,000 produces exactly
10 preemptions. Frame state (`args[0]` = current n) is correct after every
preemption. Final result is correct after all resumes.

---

## Debugger and inspector access

### Frame chain traversal

A suspended actor's frame chain is accessed via `STA_Actor.stack_top` (the new
field added by this ADR's consequences). The chain is followed via `frame->sender_fp`
until `NULL` (the outermost frame). The frame at `stack_top` is the innermost
(most recently entered).

### Slot presentation

For each frame in the chain, the inspector presents:
- `frame->receiver` — the receiver OOP (`self` in this method)
- `sta_frame_slots(frame)[0..arg_count-1]` — argument OOPs
- `sta_frame_slots(frame)[arg_count..arg_count+local_count-1]` — local OOPs

All slots are `STA_OOP` values, directly readable by the inspector.

### TCO-elided frames — TC-A decision

**TCO-elided frames are permanently lost.** When a tail call executes, the current
frame's receiver, PC, and arguments are overwritten with the callee's values. The
caller's execution state at the moment of the tail call is unrecoverable.

This is the same behaviour as Elixir/BEAM, which is the system this VM is inspired
by. It is an explicit product decision:

- **In production mode** (no debug capability attached): TCO is active, elided
  frames are lost, call stacks shown in the debugger reflect only live frames. This
  is correct and expected by Smalltalk programmers familiar with BEAM-style systems.
- **In debug mode** (debug capability attached to an actor): TCO is disabled for
  that actor. Every call pushes a full frame. The debugger sees the complete call
  chain at the cost of stack growth. The per-actor flag check for debug capability
  is already present in the scheduler reduction-counting path (ADR 009, Appendix A).

No frame layout change is required to support TC-B (debug mode with full frames) —
disabling TCO in the dispatch loop is a compile-time or runtime flag check.

---

## Rationale

### Why Option A over Option B?

| | Option A (C struct) | Option B (ObjHeader) |
|---|---|---|
| Fixed overhead per frame | **40 bytes** | 72 bytes |
| TCO frame reuse | **Trivial — in-place C struct mutation** | Awkward — GC must bypass allocator or flag frame as pinned/reused |
| Allocation cost per push | **Zero** (bump pointer) | One nursery allocation per frame |
| Stack depth limit | Slab capacity (tunable, lazy allocation) | Nursery capacity (fills GC heap on deep recursion) |
| GC walkability | Requires explicit `sta_frame_gc_roots()` | Automatic via normal ObjHeader traversal |
| Debugger access | `sender_fp` pointer chain; slots by count | OOP chain; slots by standard payload traversal |

Option B's primary advantage — automatic GC walkability — is not actually free: the
GC still needs to enter the frame chain from `stack_top` and follow it, which is
exactly what `sta_frame_gc_roots()` does for Option A. The difference is whether
each node in the chain carries an `STA_ObjHeader` (Option B: yes, +32 bytes per
frame) or not (Option A: no, simple C struct).

Option A is chosen. The 32-byte overhead of Option B per frame is not justified by
the walkability benefit, especially given the BEAM-density requirement.

### Why not flexible array members in STA_Frame?

Flexible array members (`STA_OOP slots[]`) would make the struct variable-length
at the language level, which complicates `sizeof` computation and may interact
poorly with strict-aliasing analysis. The chosen design places payload slots at
a fixed offset (`frame + 1`) via `sta_frame_slots()`, which is explicit, portable,
and well-defined in C17.

### Why `reduction_hook` in the frame header?

ADR 009 establishes that the reduction counter is in `STA_Actor`, not per-frame.
The `reduction_hook` slot in the frame header is reserved for a future optimisation
where the scheduler can store a per-frame preemption point (e.g., a callback pointer
or interrupt flag that the dispatch loop checks without loading from the actor
struct). It costs 4 bytes (with the 4-byte `_pad` it occupies the natural alignment
gap anyway). It is unused in the spike; set to zero.

---

## Open questions (deferred)

1. **Stack slab growth policy.** The spike uses a fixed-size `malloc`'d slab. The
   production implementation must decide: re-alloc (invalidates all frame pointers —
   dangerous without careful pointer fixup) or slab chaining (two slabs linked by
   a chain pointer). Chaining is simpler and safer; it adds one pointer check to
   `sta_frame_pop`. Decide before the bytecode interpreter is written.

2. **TCO with callee having more locals.** `sta_frame_tail_call` (spike) asserts
   that the new `arg_count` fits within the current frame's slot allocation. In
   production, a tail call to a method with more locals than the current frame
   requires either: (a) ensuring sufficient slack in the slab allocation at push
   time (reserve N extra slots), or (b) growing the slab. Decide alongside the
   growth policy above.

3. **Closure and non-local return compatibility.** Smalltalk `BlockContext` with
   non-local returns (`^`) breaks the LIFO stack invariant. Blocks that escape their
   enclosing method must have their frame (or a copy of relevant state) promoted to
   the heap. This interaction is not modelled in the spike. Design before Phase 1
   block/closure support.

4. **`become:` and frame receiver update.** If `recv become: other` is called while
   `recv` appears as a frame receiver, all frame receiver slots holding that OOP must
   be updated. The GC walk helper provides the mechanism (visit each receiver slot);
   `become:` can reuse it. Protocol TBD before `become:` is implemented.

5. **Stack pointer field name.** The consequence below adds `stack_top` to
   `STA_ActorRevised`. The permanent implementation may prefer `frame_ptr` or
   `current_frame`. The name is deferred to Phase 1 when the field is actually added.

---

## Consequences

- **`STA_Frame`** is locked at 40 bytes, 8-byte aligned, with payload at offset 40.
  Payload slots are `STA_OOP` words at `frame + 1` via `sta_frame_slots()`.
  Any future addition to the frame header changes the allocation unit and invalidates
  any serialised stack format. (Stacks are not serialised to images in Phase 0–1;
  actors are not suspended mid-message at image save time. This is a deferred risk.)

- **`src/vm/actor_spike.h` must be revised before Phase 1.** The following fields
  must be added to `STA_ActorRevised` (projected struct: 136 bytes, well under the
  180-byte single-struct threshold):
  - `void *stack_base`  (8 bytes) — bottom of the stack slab (for GC bounds check)
  - `void *stack_top`   (8 bytes) — current executing frame pointer

- **`src/scheduler/scheduler_spike.h`: no revision required.** The scheduler's
  public interface (`sta_sched_push`, the deque, the reduction counter path) is
  unaffected by the frame layout. The reduction counter check (`*reductions == 0`
  → yield) already operates correctly with the TCO path, as validated by the spike.

- **Projected density after adding stack fields:**

| Component | Bytes |
|---|---|
| `STA_Actor` struct (with stack_base + stack_top) | 136 |
| Initial nursery slab | 128 |
| Actor identity object (0-slot header) | 16 |
| **Total creation cost** | **280** |
| Target | ~300 |
| Headroom | **20** |

- **⚠ Density headroom is tight and actionable. 20 bytes entering Spike 005.**
  libuv async I/O integration (Spike 005, Phase 2) is the next known consumer.
  At minimum it requires a `uv_loop_t *` pointer (8 bytes) embedded in or
  associated with `STA_Actor`; it will likely also require an I/O-state field
  (4–8 bytes for pending handle count, completion callback, or wakeup channel).
  **Estimated addition: 8–16 bytes, consuming most or all of the remaining
  20-byte margin.**

  Projected scenarios entering Spike 005:

  | Scenario | `STA_Actor` size | Creation cost | Remaining headroom |
  |---|---|---|---|
  | Low (+8 bytes, pointer only) | 144 | 288 | 12 |
  | Mid (+12 bytes) | 148 | 292 | 8 |
  | High (+16 bytes, pointer + state) | 152 | 296 | 4 |
  | Over (+20 bytes) | 156 | **300 — at limit** | 0 |
  | Breach (+24 bytes) | 160 | **304 — over target** | — |

  **Rule: Spike 005 must include an explicit `sizeof(STA_Actor)` measurement and
  a density table in its ADR.** If the 300-byte target is breached, a new ADR
  must justify the increase per CLAUDE.md ("Drift from ~300 bytes must be
  explained in a decision record. Never silently ignored."). Do not absorb any
  struct growth without measuring it.

- **Stack slab allocation is lazy.** The slab is not allocated at actor creation
  time. It is allocated on first message dispatch (first `sta_frame_push`). Creation
  cost stays at 280 bytes regardless of whether the actor ever receives a message.
  The slab allocation cost at first dispatch is a one-time per-actor amortised cost.

- **TCO is enabled by default.** Disabled per-actor when a debug capability is
  attached (Phase 3). The disable path is a flag check in the dispatch loop; no
  frame layout change is required.

- **`sta_frame_gc_roots()` is the canonical GC stack-walk function.** All GC
  implementations (Phase 1+) must call it for each actor's frame chain. Its contract
  (visit every live OOP slot exactly once) is tested and locked by the spike.
