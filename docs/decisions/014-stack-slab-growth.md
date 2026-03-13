# ADR 014 — Stack Slab Growth Policy and TCO Frame Sizing

**Status:** Accepted
**Date:** 2026-03-13
**Resolves:** Open decisions #6 (stack slab growth policy) and #7 (TCO with callee having more locals)
**Depends on:** ADR 010 (activation frame layout and TCO)

---

## Decision

### Decision #6 — Stack slab growth policy: linked segments with deferred allocation

Each actor's call stack is stored in **linked segments** rather than a single
contiguous allocation. The initial segment is allocated on first message
dispatch (deferred), not at actor spawn.

### Decision #7 — TCO with callee having more locals: decline TCO, push normally

If the callee's frame does not fit within the space already allocated for the
current frame, TCO is not applied. The interpreter pushes a fresh frame
normally. This may land in the current segment or trigger a new segment
allocation.

---

## Stack slab model

### Segment sizes

| Segment | Total size | Usable (after 16-byte header) | Typical capacity |
|---|---|---|---|
| Initial | 512 bytes | 496 bytes | ~3 activation frames |
| Growth | 2,048 bytes | 2,032 bytes | ~12 activation frames |

### Segment header layout (first 16 bytes of each segment)

```c
typedef struct STA_StackSeg {
    struct STA_StackSeg *prev_seg;  /* previous segment, NULL for first (8 bytes) */
    uint8_t             *seg_end;   /* end of usable space in this segment (8 bytes) */
} STA_StackSeg;                     /* sizeof = 16 */
```

### Actor struct additions

```c
STA_StackSeg *current_seg;   /* newest (active) segment         — 8 bytes */
uint16_t      frame_depth;   /* current frame count             — 2 bytes */
uint16_t      seg_count;     /* current segment count           — 2 bytes */
```

Total addition to `STA_Actor`: **12 bytes** (152 → 164 bytes).

`frame_depth` and `seg_count` are maintained on every push/pop so actor
introspection tools can read them in O(1) without walking the segment chain.

### Deferred allocation

The initial 512-byte stack segment is allocated on **first message dispatch**,
not at actor spawn. An actor that has been spawned but has not yet processed a
message has no stack allocation. This keeps spawn cost low and avoids wasting
memory on actors that are created but never scheduled.

---

## Actor density (updated)

The 300-byte forcing function from CLAUDE.md served its purpose through Phase 0:
it kept the actor struct lean and forced every addition to be justified. Phase 1
targets replace it with measured numbers.

### Phase 1 density targets

| State | Bytes | Components |
|---|---|---|
| At spawn (struct + nursery) | **308 bytes** | `STA_Actor` (164) + nursery slab (128) + identity object (16) |
| Executing (+ initial stack) | **820 bytes** | 308 + initial stack segment (512) |

### BEAM comparison

| Runtime | Size at spawn |
|---|---|
| BEAM process | 2,704 bytes (338 words × 8; per Erlang/OTP efficiency guide; includes 1,864-byte heap) |
| Smalltalk/A actor (executing) | 820 bytes |

**Smalltalk/A is 3.3× more compact than BEAM at execution time.**

### Policy going forward

Any addition to `STA_Actor` still requires a new ADR. The justification bar is
"is this worth the bytes" — not "does this breach 300." The 308/820 numbers are
the new reference points.

---

## Frame push operation

```
1. Check frame_depth < max_depth (default 10,000).
   If exceeded → signal StackOverflow exception.
2. Check stack_top + frame_size <= current_seg->seg_end.
   If it fits → push frame, increment frame_depth.
3. If it does not fit:
   a. Allocate a new 2,048-byte growth segment.
   b. new_seg->prev_seg = current_seg.
   c. current_seg = new_seg.
   d. Increment seg_count.
   e. Push frame into new segment.
```

## Frame pop operation

```
1. Pop frame, decrement frame_depth.
2. If stack_top has retreated past current_seg base (segment is now empty):
   a. prev = current_seg->prev_seg.
   b. Free current_seg.
   c. current_seg = prev.
   d. Decrement seg_count.
```

---

## TCO with callee having more locals (decision #7)

TCO frame reuse requires that the callee's temp+stack area fits within the space
already allocated for the current frame. If the callee has more temporaries or
deeper stack usage than the current frame, the interpreter **skips TCO** for
that call and pushes a fresh frame normally.

TCO is an optimisation, not a semantic guarantee. The compiler already disables
TCO for methods containing NLR-bearing blocks (bytecode spec §5.11). Declining
TCO when the callee frame is larger is one more case where correctness and
simplicity take precedence over maximal optimisation.

The result is that infinite tail-recursive loops are still bounded in stack
growth (each new frame replaces the previous one if it fits), and in the rare
case where TCO cannot fire, the interpreter falls back to normal frame push,
which may allocate a new segment.

---

## GC interaction

The GC walks frames via sender pointers, which cross segment boundaries
transparently. The GC does **not** walk segments; segment headers are not heap
objects and are not traced. A future compacting GC does not move frames (frames
live on stack segments, not on the heap), so segment structure does not affect
compaction.

---

## `largeFrame` bit (bytecode spec §4.2)

The `largeFrame` bit in the method header is retained as VM-private metadata.
It is **not** used for segment pre-allocation. The interpreter checks segment
space on every frame push regardless of this bit. The bit may be used in the
future as a hint for eager space checks or JIT frame allocation strategy. Its
exact semantics are deferred to implementation — it is VM-private per the
bytecode spec and its meaning can evolve without breaking the image format.

---

## Tooling benefits

`frame_depth` and `seg_count` are O(1) reads on `STA_Actor`. They enable:

- **Actor monitor display:** `"Actor #N — depth: 47 frames, 3 segments (6.5 KB)"`
- **Stack overflow detection:** configurable `max_depth` per actor (default 10,000)
- **Memory profiling:** `seg_count × segment_size` for fast stack memory approximation

---

## Rationale

### Why linked segments over a single contiguous slab?

A contiguous slab requires an upfront size decision. Too small → crash or
realloc; too large → wasted memory per actor. The Phase 0 spike used a fixed
8 KB slab — unnecessarily large for most actors. Linked segments allocate
exactly what is needed: 512 bytes for actors with shallow call stacks (the
common case), growing on demand.

Realloc of a contiguous slab is not viable: frames contain raw pointers into
the slab. Moving the slab invalidates every in-flight sender pointer. Segments
never move; segment headers are stable addresses.

### Why 512 bytes initial / 2,048 bytes growth?

512 bytes (496 usable) holds ~3 typical frames (40 bytes each + locals). The
initial segment is sized to cover the vast majority of actors without waste.
Growth segments are 4× larger to amortise allocation cost when a call chain
is deeper than expected.

### Why defer initial allocation?

Many actors are spawned to handle a single short-lived task. Deferring to first
dispatch means spawn cost is exactly struct + nursery + identity object = 308
bytes. Actors that never run pay nothing for the stack.

### Why `uint16_t` for `frame_depth` and `seg_count`?

`frame_depth` max = 10,000 — fits in `uint16_t` (max 65,535). `seg_count` at
max_depth with 40-byte frames: 10,000 × 40 bytes / 2,032 bytes per growth
segment ≈ 200 segments — also fits comfortably. Two `uint16_t` fields cost 4
bytes; two `uint32_t` would cost 8. The saving is 4 bytes — exactly the
headroom that was consumed by this ADR's pointer addition.

---

## Consequences

- **`STA_Actor` grows from 152 to 164 bytes.** Creation cost (struct + nursery
  + identity): 308 bytes. Execution cost (+ initial stack segment): 820 bytes.
  BEAM baseline: 2,704 bytes. Smalltalk/A is 3.3× more compact at execution.
- **The 300-byte forcing function is retired.** The Phase 1 reference points
  are 308 bytes (spawn) and 820 bytes (executing). Any further addition to
  `STA_Actor` still requires a new ADR.
- **TCO fires when callee frame fits; is declined when it does not.** No
  semantic change — TCO is an optimisation. Infinite tail-recursive loops
  remain bounded if the recursive callee frame fits the current frame.
- **`largeFrame` bit semantics deferred.** Not used for segment pre-allocation.
  Meaning may evolve without breaking images.
- **Stack segments are not heap objects.** GC traversal crosses segment
  boundaries via sender pointers. No GC change required.
