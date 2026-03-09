# Spike: ObjHeader Layout and OOP Tagging

**Phase 0 spike — object representation**  
**Status:** Ready to execute  
**Related ADRs:** 001 (implementation language), 003 (internal header convention)  
**Produces:** ADR 007 (object header layout)

---

## Purpose

This spike answers the foundational question of object representation: what does an OOP look like, what does the header of a heap object look like, and how do these choices interact with the GC, the actor heap allocator, and the 300-byte actor density target?

The architecture document (§8.3) provides a starting sketch. This spike either validates that sketch or replaces it with something better — and records the decision before any interpreter or allocator code is written. The header layout is load-bearing: it affects GC traversal, method dispatch, actor isolation enforcement, image serialization, and every hot path in the VM.

**This spike does not build the interpreter, the GC, or the allocator.** It builds the minimal C that is needed to reason concretely about memory layout, measure sizes, and test the tagging invariants. Permanent implementation follows the decision record.

---

## Background: the sketch from §8.3

The architecture document proposes this layout:

```c
typedef uintptr_t OOP;

#define IS_SMALLINT(oop)   ((oop) & 1)
#define SMALLINT_VAL(oop)  ((intptr_t)(oop) >> 1)
#define SMALLINT_OOP(n)    (((uintptr_t)(n) << 1) | 1)

typedef struct ObjHeader {
    uint32_t class_index;   // index into class table
    uint32_t size;          // payload size in words
    uint8_t  gc_flags;      // GC color, forwarding flag
    uint8_t  obj_flags;     // immutable, pinned, actor-local, etc.
    uint16_t reserved;
} ObjHeader;
```

This is 12 bytes. On a 64-bit platform, the first payload word lands at offset 12 — which is **not 8-byte aligned** unless the allocator pads it or the struct is reordered. That is one of several things this spike must resolve explicitly.

---

## Questions to answer

These are the decisions that must be locked before any allocator or interpreter code is written. The spike produces a concrete answer — not a deferral — for each one.

### Q1: Does the 12-byte header actually align payload correctly?

`sizeof(ObjHeader)` is 12 on any standard C17 compiler with natural alignment (`uint32_t` + `uint32_t` + `uint8_t` + `uint8_t` + `uint16_t` = 4+4+1+1+2 = 12 with no internal padding). The first payload word at `(ObjHeader*)p + 1` lands at offset 12, which is not a multiple of 8. An `OOP` stored at offset 12 is misaligned on any 64-bit platform that requires 8-byte alignment for pointer-sized reads.

**Options to evaluate:**

Option A — Pad the header to 16 bytes (add 4 bytes of reserved or future fields). Payload at offset 16, always 8-byte aligned. Cost: 4 extra bytes per object.

Option B — Move `gc_flags` and `obj_flags` into the high bits of `size` or `class_index`, keeping the header at exactly 8 bytes. Payload at offset 8, always 8-byte aligned. Saves 4 bytes per object vs. the sketch; tight but achievable.

Option C — Keep 12 bytes and require the allocator to over-align every allocation to 16 bytes. The header starts at an aligned address; payload at offset 12 is misaligned; allocator rounds up. Effectively the same cost as Option A but less explicit.

**Measure:** `sizeof(ObjHeader)`, `offsetof(ObjHeader, ...)` for each field, alignment of a simulated payload after the header. Print these from a test binary — do not reason from memory alone.

---

### Q2: How many OOP tag bits do we need, and what do they mean?

The architecture uses bit 0 as the SmallInt tag. A 63-bit SmallInt is correct and sufficient for Smalltalk arithmetic. But the full tagging scheme needs to be defined now, not discovered later, because it affects every OOP test in the interpreter hot path.

**Minimum tagging requirements:**

- SmallInt — bit 0 set, value in bits 63:1
- Heap pointer — bit 0 clear, word-aligned pointer (low 3 bits always 0 on 64-bit)
- Nil / True / False — these are either heap objects with well-known addresses, or they consume additional tag patterns

**Questions to resolve:**

1. Are `nil`, `true`, and `false` heap objects with special addresses in a shared immutable region, or are they special immediate values (using e.g. bits 1:0 = 10 or 11)?

   - As heap objects: simplest implementation, no special casing in the tag scheme; costs 3 small objects in the immutable region; slightly slower to test `isNil` (must compare pointer, not tag)
   - As immediates: faster `isNil` test (single mask); slightly complicates the tag scheme; requires reserving 2 more tag patterns

2. Are Characters immediates (using e.g. bits 2:0 = 110) or heap objects?

   - The Blue Book treats Character as a heap object but implementations almost universally cache/intern them. As an immediate they cost nothing on the heap; as heap objects they require an intern table.

3. Is there a tag pattern reserved for forwarding pointers (used by the GC during collection)?

   - A forwarding pointer replaces an ObjHeader in-place when the GC copies an object. The tag must be detectable by the GC traversal code without reading the header. Common approach: use a bit in `gc_flags` — no tag needed in the OOP itself. Validate this works with the chosen layout.

**Measure:** define all tag macros, write a table of test values and their expected `IS_SMALLINT` / `IS_HEAP_OBJ` results, assert all of them in a test binary.

---

### Q3: What goes in `gc_flags` and `obj_flags` — specifically?

The sketch names the bytes but not their bit fields. These must be defined now because:
- The GC traversal code reads `gc_flags` on every live object during collection
- The actor isolation check reads `obj_flags` on cross-actor sends and when the debugger requests heap access
- If bits are added ad hoc later, the header size may need to change, invalidating every existing object in saved images

**Proposed bit layout to validate:**

`gc_flags` (8 bits):
```
bit 0:   GC mark (for tri-color marking: 0=white, set during mark phase)
bit 1:   GC color high (0=white/grey, 1=black — with bit 0: 00=white, 01=grey, 10=black)
bit 2:   Forwarding pointer flag (object has been copied; header word IS the new address)
bit 3:   Remembered set flag (object in old-space references young-space objects)
bits 7:4: Reserved — must be zero, checked in debug assertions
```

`obj_flags` (8 bits):
```
bit 0:   Immutable (writes to instance variables are a runtime error)
bit 1:   Pinned (must not be moved by a compacting GC pass — used for objects shared with C)
bit 2:   Actor-local (this object must not escape this actor's heap — checked on cross-actor copy)
bit 3:   Shared-immutable-region (lives in a shared immutable page; GC must not attempt to collect)
bit 4:   Finalization registered (notified before collection)
bits 7:5: Reserved — must be zero, checked in debug assertions
```

**Spike task:** define named constants and masks for all bits. Write a test that sets each flag on a synthetic header and reads it back correctly. Confirm there is no accidental overlap and no reserved bit set by default.

---

### Q4: Is `size` in words or bytes, and what is the maximum object size?

The sketch says "payload size in words." A `uint32_t` size field in words gives a maximum payload of 4 billion words = 32 GB on a 64-bit platform. That is more than enough. But "words" needs a precise definition: is a "word" the size of an OOP (8 bytes on arm64), or always 4 bytes?

**Recommendation to validate:** one word = one OOP = 8 bytes on arm64. `size` is the number of OOP-sized payload slots. A `ByteArray` of N bytes requires `ceil(N / 8)` words; the actual byte count is stored in a leading payload word or derived from the class. This is the Squeak convention and is correct — validate it compiles and the arithmetic works.

**Maximum string / array size:** with 32-bit word count and 8 bytes per word, max payload = 32 GB. That is fine for any foreseeable use. Confirm the `uint32_t` size field does not need to be widened.

---

### Q5: Does the 300-byte actor target survive contact with a real header layout?

The actor struct (from §8.3) includes: mailbox, private heap pointer, behavior/class pointer, supervisor linkage, opaque address/capability. These are not heap objects — they live in a C struct. But actors are *presented as* objects, which means there is also an `ObjHeader` + payload for the actor's Smalltalk-visible identity.

**Two distinct things to measure:**

1. `sizeof(STA_Actor)` — the C-level actor runtime struct. Budget: aim for ≤ 128 bytes, leaving room for the heap header and initial nursery allocation within the 300-byte creation cost.

2. The per-actor heap creation cost. At creation, a fresh actor needs at minimum: its runtime struct + an initial nursery slab (even a tiny one, e.g. 128 bytes) + the ObjHeader for its Smalltalk-visible identity. Sum these.

**Spike task:** write a stub `STA_Actor` struct with plausible field types (use `void*` for pointers, `uint32_t` for counters, `_Atomic uint32_t` for reduction counter), measure `sizeof(STA_Actor)`, and confirm the total creation budget lands near 300 bytes. If it does not, record the delta and the reason in the ADR.

---

### Q6: Is the `class_index` a 32-bit table index or a direct pointer, and what are the tradeoffs?

The sketch uses `uint32_t class_index` — an index into a global class table rather than a direct pointer to the class object. This is a deliberate choice with real tradeoffs.

**Index (current sketch):**
- 4 bytes in the header (vs. 8 for a pointer on arm64) — saves 4 bytes per object
- Class table lookup adds one indirection on every method dispatch
- Class table is a global shared structure (concurrency implications — see §8.5)
- Maximum 4 billion classes — absurdly sufficient

**Direct pointer:**
- 8 bytes in the header — costs 4 bytes per object
- No extra indirection on dispatch — class pointer is immediately available
- Moving GC must update class pointers if classes are moveable objects (complicates GC)
- Pointer-sized fields require 8-byte alignment everywhere, which interacts with Q1

**Decision to record:** the index approach is correct for BEAM-density targets. A 4-byte index saves 4 bytes per object — at one million live objects, that is 4 MB. More importantly, keeping the header at 12 bytes (padded to 16 for alignment per Q1) is still smaller than a 16-byte header with a direct 8-byte class pointer. Validate that a class table lookup on arm64 (one array index multiply + load) is not a dispatch bottleneck on a microbenchmark.

---

## What to build

This spike consists of three C files plus one test. All files go in `src/vm/` following ADR 003. They are **spike code** — clearly marked, to be replaced during Phase 1 with permanent implementations informed by the decisions here.

### File 1: `src/vm/oop_spike.h`

Define the full OOP type, all tag macros, the complete `ObjHeader` struct with named flag constants, and a small set of inline helpers. This is the thing being evaluated.

```c
/* src/vm/oop_spike.h
 * Phase 0 spike: OOP tagging and object header layout.
 * NOT the permanent implementation — see ADR 007 for decisions.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ── OOP ──────────────────────────────────────────────────── */
typedef uintptr_t STA_OOP;

/* Tag scheme — bit 0 discriminates SmallInt from heap pointer.
 * Heap pointers are always 8-byte aligned (low 3 bits zero).
 * SmallInt: bit 0 = 1, value = bits 63:1 (63-bit signed).       */
#define STA_IS_SMALLINT(oop)    ((oop) & 1u)
#define STA_IS_HEAP(oop)        (!STA_IS_SMALLINT(oop))
#define STA_SMALLINT_VAL(oop)   ((intptr_t)(oop) >> 1)
#define STA_SMALLINT_OOP(n)     (((STA_OOP)(uintptr_t)(intptr_t)(n) << 1) | 1u)

/* Well-known immediate constants.
 * nil/true/false are heap objects in the shared immutable region.
 * Their addresses are stored in a VM-global table; no special tags. */
#define STA_OOP_ZERO     STA_SMALLINT_OOP(0)

/* ── ObjHeader ───────────────────────────────────────────── */

/* gc_flags bits */
#define STA_GC_WHITE        0x00u   /* not yet visited */
#define STA_GC_GREY         0x01u   /* discovered, not yet scanned */
#define STA_GC_BLACK        0x02u   /* fully scanned */
#define STA_GC_COLOR_MASK   0x03u
#define STA_GC_FORWARDED    0x04u   /* object copied; header IS new address */
#define STA_GC_REMEMBERED   0x08u   /* old-space obj refs young-space obj */
#define STA_GC_RESERVED     0xF0u   /* must be zero */

/* obj_flags bits */
#define STA_OBJ_IMMUTABLE   0x01u   /* writes to slots are a runtime error */
#define STA_OBJ_PINNED      0x02u   /* must not be moved by compacting GC */
#define STA_OBJ_ACTOR_LOCAL 0x04u   /* must not escape this actor's heap */
#define STA_OBJ_SHARED_IMM  0x08u   /* lives in shared immutable region */
#define STA_OBJ_FINALIZE    0x10u   /* registered for finalization */
#define STA_OBJ_RESERVED    0xE0u   /* must be zero */

typedef struct {
    uint32_t class_index;  /* index into global class table */
    uint32_t size;         /* payload size in OOP-sized words (8 bytes each) */
    uint8_t  gc_flags;
    uint8_t  obj_flags;
    uint16_t reserved;     /* must be zero; available for future use */
} STA_ObjHeader;

/* Derived sizes and offsets — validated in spike tests */
#define STA_HEADER_SIZE     sizeof(STA_ObjHeader)   /* expect 12 */
#define STA_HEADER_ALIGNED  16u                     /* allocation unit */

/* Payload pointer: first OOP-sized word after the header,
 * at offset STA_HEADER_ALIGNED (16) from the start of the allocation. */
static inline STA_OOP *sta_payload(STA_ObjHeader *h) {
    return (STA_OOP *)((char *)h + STA_HEADER_ALIGNED);
}

/* Total allocation size for an object with `n` payload words.
 * Always a multiple of 8 (one OOP). */
static inline size_t sta_alloc_size(uint32_t n) {
    return STA_HEADER_ALIGNED + (size_t)n * sizeof(STA_OOP);
}
```

**Note on the padding decision embedded above:** `STA_HEADER_ALIGNED = 16` pads the 12-byte header by 4 bytes to ensure the payload is 8-byte aligned. This is Option A from Q1. The 4 bytes of padding are intentional waste — record that explicitly in ADR 007.

### File 2: `src/vm/actor_spike.h`

Stub actor struct with plausible fields to measure `sizeof(STA_Actor)`.

```c
/* src/vm/actor_spike.h
 * Phase 0 spike: actor struct layout for density measurement.
 * NOT the permanent implementation.
 */
#pragma once
#include <stdint.h>
#include <stdatomic.h>
#include "oop_spike.h"

/* Mailbox node — one per queued message */
typedef struct STA_MboxNode {
    struct STA_MboxNode *next;   /* 8 bytes — MPSC queue link */
    STA_OOP              msg;    /* 8 bytes — the message OOP */
} STA_MboxNode;                  /* 16 bytes */

/* Lock-free MPSC mailbox — stub head/tail pointers */
typedef struct {
    _Atomic(STA_MboxNode *) head;    /* 8 bytes */
    STA_MboxNode           *tail;    /* 8 bytes — written only by owner */
} STA_Mailbox;                       /* 16 bytes */

/* Actor runtime struct */
typedef struct STA_Actor {
    /* Identity and dispatch */
    uint32_t   class_index;     /* 4 bytes — class of this actor */
    uint32_t   actor_id;        /* 4 bytes — unique within the image */

    /* Concurrency */
    STA_Mailbox mailbox;        /* 16 bytes */
    _Atomic uint32_t reductions;/* 4 bytes — budget counter */
    uint32_t   sched_flags;     /* 4 bytes — runnable, suspended, etc. */

    /* Heap */
    void      *heap_base;       /* 8 bytes — start of nursery slab */
    void      *heap_bump;       /* 8 bytes — next allocation point */
    void      *heap_limit;      /* 8 bytes — end of nursery slab */

    /* Supervision */
    struct STA_Actor *supervisor;  /* 8 bytes — pointer to supervisor actor */
    uint32_t   restart_strategy;   /* 4 bytes */
    uint32_t   restart_count;      /* 4 bytes */

    /* Capability address (opaque, unforgeable within runtime) */
    uint64_t   capability_token;   /* 8 bytes */
} STA_Actor;
/* Target: sizeof(STA_Actor) <= 96 bytes.
 * Per-actor creation budget:
 *   sizeof(STA_Actor)  = measured
 *   initial nursery    = 128 bytes (minimum viable slab)
 *   actor identity obj = STA_HEADER_ALIGNED + 0 payload = 16 bytes
 *   ─────────────────────────────────────────────────
 *   total              = should be <= 300 bytes        */
```

### File 3: `tests/test_objheader_spike.c`

The executable spike. Prints measurements, asserts invariants, exits 0 on success.

```c
/* tests/test_objheader_spike.c
 * Phase 0: ObjHeader layout and OOP tagging spike.
 * Run with: cmake --build build && ./build/tests/test_objheader_spike
 */
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../src/vm/oop_spike.h"
#include "../src/vm/actor_spike.h"

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); return 1; } \
         else { printf("  OK: %s\n", msg); } } while(0)

int main(void) {
    int failures = 0;

    /* ── Header layout ─────────────────────────────────── */
    printf("\n=== ObjHeader layout ===\n");
    printf("  sizeof(STA_ObjHeader)  = %zu (expect 12)\n", sizeof(STA_ObjHeader));
    printf("  STA_HEADER_ALIGNED     = %u  (expect 16)\n", STA_HEADER_ALIGNED);
    printf("  offsetof class_index   = %zu (expect 0)\n",  offsetof(STA_ObjHeader, class_index));
    printf("  offsetof size          = %zu (expect 4)\n",  offsetof(STA_ObjHeader, size));
    printf("  offsetof gc_flags      = %zu (expect 8)\n",  offsetof(STA_ObjHeader, gc_flags));
    printf("  offsetof obj_flags     = %zu (expect 9)\n",  offsetof(STA_ObjHeader, obj_flags));
    printf("  offsetof reserved      = %zu (expect 10)\n", offsetof(STA_ObjHeader, reserved));

    CHECK(sizeof(STA_ObjHeader) == 12, "ObjHeader is 12 bytes");
    CHECK(offsetof(STA_ObjHeader, class_index) == 0,  "class_index at offset 0");
    CHECK(offsetof(STA_ObjHeader, size)        == 4,  "size at offset 4");
    CHECK(offsetof(STA_ObjHeader, gc_flags)    == 8,  "gc_flags at offset 8");
    CHECK(offsetof(STA_ObjHeader, obj_flags)   == 9,  "obj_flags at offset 9");
    CHECK(offsetof(STA_ObjHeader, reserved)    == 10, "reserved at offset 10");

    /* ── Payload alignment ──────────────────────────────── */
    printf("\n=== Payload alignment ===\n");
    // Simulate an allocation: 16-byte aligned base, header at offset 0,
    // first payload word at offset STA_HEADER_ALIGNED.
    _Alignas(16) char buf[64];
    STA_ObjHeader *h = (STA_ObjHeader *)buf;
    STA_OOP *payload = sta_payload(h);
    uintptr_t payload_addr = (uintptr_t)payload;

    printf("  buf address            = %p\n", (void*)buf);
    printf("  payload address        = %p (offset %td from header)\n",
           (void*)payload, (char*)payload - (char*)h);
    printf("  payload 8-byte aligned = %s\n", (payload_addr % 8 == 0) ? "yes" : "NO");

    CHECK((payload_addr % 8) == 0, "payload is 8-byte aligned");
    CHECK((payload_addr % 16) == 0, "payload is also 16-byte aligned (bonus)");

    /* Allocation size helper */
    CHECK(sta_alloc_size(0) == 16, "alloc_size(0 slots) = 16 (header only)");
    CHECK(sta_alloc_size(1) == 24, "alloc_size(1 slot)  = 24");
    CHECK(sta_alloc_size(4) == 48, "alloc_size(4 slots) = 48");

    /* ── OOP tagging ────────────────────────────────────── */
    printf("\n=== OOP tagging ===\n");

    /* SmallInt round-trips */
    intptr_t test_vals[] = { 0, 1, -1, 42, -42, 1000000,
                              INT32_MAX, INT32_MIN,
                              (intptr_t)4611686018427387903LL,   /* 2^62 - 1 */
                              (intptr_t)(-4611686018427387904LL) /* -(2^62) */ };
    for (size_t i = 0; i < sizeof(test_vals)/sizeof(test_vals[0]); i++) {
        STA_OOP oop = STA_SMALLINT_OOP(test_vals[i]);
        intptr_t back = STA_SMALLINT_VAL(oop);
        CHECK(STA_IS_SMALLINT(oop),        "smallint tag set");
        CHECK(!STA_IS_HEAP(oop),           "smallint is not heap");
        CHECK(back == test_vals[i],        "smallint round-trips");
    }

    /* Heap OOP (simulated aligned pointer) */
    _Alignas(8) char obj_buf[32];
    STA_OOP heap_oop = (STA_OOP)(uintptr_t)obj_buf;
    CHECK(!STA_IS_SMALLINT(heap_oop),      "heap pointer: not smallint");
    CHECK(STA_IS_HEAP(heap_oop),           "heap pointer: is heap");
    CHECK((heap_oop & 1u) == 0,            "heap pointer: low bit clear");

    /* ── GC and obj flag bits ───────────────────────────── */
    printf("\n=== Flag bits ===\n");

    STA_ObjHeader hdr;
    memset(&hdr, 0, sizeof(hdr));

    /* GC colors */
    hdr.gc_flags = STA_GC_WHITE;
    CHECK((hdr.gc_flags & STA_GC_COLOR_MASK) == STA_GC_WHITE, "GC white");
    hdr.gc_flags = STA_GC_GREY;
    CHECK((hdr.gc_flags & STA_GC_COLOR_MASK) == STA_GC_GREY,  "GC grey");
    hdr.gc_flags = STA_GC_BLACK;
    CHECK((hdr.gc_flags & STA_GC_COLOR_MASK) == STA_GC_BLACK, "GC black");

    /* Forwarding and remembered-set don't alias the color bits */
    hdr.gc_flags = STA_GC_FORWARDED | STA_GC_BLACK;
    CHECK(hdr.gc_flags & STA_GC_FORWARDED, "forwarded bit set alongside black");
    CHECK((hdr.gc_flags & STA_GC_RESERVED) == 0, "no reserved gc bits set");

    /* obj_flags */
    hdr.obj_flags = STA_OBJ_IMMUTABLE | STA_OBJ_ACTOR_LOCAL;
    CHECK(hdr.obj_flags & STA_OBJ_IMMUTABLE,   "immutable bit");
    CHECK(hdr.obj_flags & STA_OBJ_ACTOR_LOCAL, "actor-local bit");
    CHECK(!(hdr.obj_flags & STA_OBJ_PINNED),   "pinned not set");
    CHECK((hdr.obj_flags & STA_OBJ_RESERVED) == 0, "no reserved obj bits set");

    /* ── Actor density ──────────────────────────────────── */
    printf("\n=== Actor density ===\n");
    size_t actor_struct  = sizeof(STA_Actor);
    size_t initial_slab  = 128;   /* minimum nursery slab at creation */
    size_t identity_obj  = STA_HEADER_ALIGNED + 0; /* zero-slot identity object */
    size_t total_creation = actor_struct + initial_slab + identity_obj;

    printf("  sizeof(STA_Actor)      = %zu bytes\n", actor_struct);
    printf("  initial nursery slab   = %zu bytes\n", initial_slab);
    printf("  actor identity object  = %zu bytes\n", identity_obj);
    printf("  ──────────────────────────────────\n");
    printf("  total creation cost    = %zu bytes (target: ~300)\n", total_creation);

    if (actor_struct > 128) {
        printf("  WARNING: STA_Actor struct exceeds 128-byte sub-budget.\n");
        printf("           Review fields — see ADR 007 rationale.\n");
    }
    if (total_creation > 320) {
        printf("  WARNING: total creation cost %zu > 320 bytes.\n", total_creation);
        printf("           Either justify the overage or reduce field count.\n");
    } else {
        printf("  OK: creation cost within ~300-byte target.\n");
    }

    /* ── Summary ──────────────────────────────────────── */
    printf("\n=== Spike complete ===\n");
    if (failures == 0) {
        printf("All checks passed. Record layout decisions in ADR 007.\n\n");
        printf("Key numbers for ADR 007:\n");
        printf("  ObjHeader:        %2zu bytes (padded to %u for alignment)\n",
               sizeof(STA_ObjHeader), STA_HEADER_ALIGNED);
        printf("  STA_Actor struct: %2zu bytes\n", actor_struct);
        printf("  Total per-actor:  %2zu bytes (struct + 128-byte slab + identity)\n",
               total_creation);
        return 0;
    } else {
        printf("%d check(s) FAILED. Fix before writing ADR 007.\n", failures);
        return 1;
    }
}
```

---

## CMake integration

Add the spike test to `tests/CMakeLists.txt`:

```cmake
add_executable(test_objheader_spike test_objheader_spike.c)
target_include_directories(test_objheader_spike PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME objheader_spike COMMAND test_objheader_spike)
```

The spike test must pass on `ctest --output-on-failure` before ADR 007 is written.

---

## Open questions the spike deliberately does not answer

These are real questions that come next — after the layout is locked. They are listed here so they don't get lost.

**Deferred to Phase 1 / ADR 008+:**

1. **Nil / True / False representation.** The spike treats these as heap objects. The alternative (special immediates using bit patterns in bits 2:0) is worth evaluating before the first interpreter dispatch loop is written, since `isNil` is in every hot path. Should be decided during Phase 1 before the bytecode decoder is built.

2. **Character representation.** Same trade-off: immediate vs. interned heap object. The Blue Book specifies heap objects; every serious Smalltalk implementation interns them. Immediate Characters are faster. Decide before the string/character primitives are implemented.

3. **Forwarding pointer mechanism.** The `STA_GC_FORWARDED` flag is defined, but the actual mechanics — where the new address is stored when a 12-byte header only has room for a flag — needs explicit design. One approach: when `FORWARDED` is set, the `class_index` and `size` words are overwritten with the forwarding address (two 32-bit halves of a 64-bit pointer). This works on arm64 but is fragile. The cleaner design reserves a word in the header specifically for forwarding. Decide before GC is implemented.

4. **Class table concurrency.** The spike measures `class_index` lookup as a simple array offset. The class table itself is shared across all scheduler threads. The locking and invalidation strategy (reader-writer lock? epoch-based? seqlock?) needs its own ADR before the method cache is built.

5. **Tail-call optimization compatibility.** The architecture doc (Appendix A) records a decision to support TCO by detecting `send` + `returnTop` in bytecode. TCO requires reusing the current activation frame. The frame layout (which lives adjacent to the object model but is not itself an `ObjHeader`) needs a companion spike. Should be done before the bytecode interpreter is written.

---

## What ADR 007 should record

After running the spike, write ADR 007 covering:

- `ObjHeader` struct layout with actual measured sizes and offsets
- Why the header is padded to 16 bytes (or the alternative chosen)
- Full OOP tag scheme: SmallInt, heap, and treatment of nil/true/false
- `gc_flags` and `obj_flags` bit field definitions
- `size` field semantics (words vs. bytes, word size definition)
- Measured `sizeof(STA_Actor)` and total per-actor creation cost
- Whether the ~300-byte density target was met, and if not, what the justified delta is

---

## Estimated time

| Task | Time |
|---|---|
| Write `oop_spike.h` and `actor_spike.h` | 1–2 hours |
| Write and pass `test_objheader_spike.c` | 1–2 hours |
| Wire into CMake and run | 30 minutes |
| Write ADR 007 from measurements | 1 hour |
| **Total** | **3.5–5.5 hours** |

This is a half-day spike. Do not let it grow. If a question can't be answered by measuring layout or running the test binary, it belongs in a future ADR — write it down and move on.
