# ADR 007 — Object Header Layout and OOP Tagging

**Status:** Accepted
**Date:** 2026-03-09
**Spike:** `docs/spikes/spike-001-objheader.md`
**Implements:** Architecture document §8.3

---

## Decision

The object representation for Smalltalk/A uses a 12-byte `STA_ObjHeader` struct
padded to 16 bytes at the allocation unit boundary, with a 63-bit tagged-integer
OOP scheme using bit 0 as the SmallInt discriminator.

### OOP type and tag scheme

```c
typedef uintptr_t STA_OOP;

/* bit 0 = 1 → SmallInt (63-bit signed, value in bits 63:1) */
#define STA_IS_SMALLINT(oop)   ((oop) & 1u)
#define STA_IS_HEAP(oop)       (!STA_IS_SMALLINT(oop))
#define STA_SMALLINT_VAL(oop)  ((intptr_t)(oop) >> 1)
#define STA_SMALLINT_OOP(n)    (((STA_OOP)(uintptr_t)(intptr_t)(n) << 1) | 1u)
```

Heap pointers have bit 0 clear. Since all heap objects are allocated at
16-byte boundaries (see below), the low 4 bits of every heap OOP are always
zero — leaving 3 bits available for future tag extensions without breaking
the SmallInt invariant.

`nil`, `true`, and `false` are heap objects in the shared immutable region
(§8.4). They carry no special OOP tag; `isNil` compares the pointer to a
well-known address stored in a VM-global table. This is the simplest correct
choice; a future ADR may revisit immediate encoding if `isNil` appears on a
hot dispatch path.

### Object header struct

```c
typedef struct {
    uint32_t class_index;  /* index into global class table              */
    uint32_t size;         /* payload size in OOP-sized words (8 bytes)  */
    uint8_t  gc_flags;     /* GC color, forwarding, remembered-set       */
    uint8_t  obj_flags;    /* immutable, pinned, actor-local, etc.       */
    uint16_t reserved;     /* must be zero; available for future use     */
} STA_ObjHeader;           /* sizeof = 12; allocation unit = 16          */
```

**Measured on arm64 M4 Max, Apple clang 17, C17 (`-std=c17`):**

| Field | Offset | Size |
|---|---|---|
| `class_index` | 0 | 4 bytes |
| `size` | 4 | 4 bytes |
| `gc_flags` | 8 | 1 byte |
| `obj_flags` | 9 | 1 byte |
| `reserved` | 10 | 2 bytes |
| *(struct total)* | — | **12 bytes** |
| *(allocation unit)* | — | **16 bytes** |

The 4-byte gap between the 12-byte struct and the 16-byte allocation unit
is explicit intentional padding (see Q1 rationale below). The first payload
word lands at offset 16 from the allocation base — 8-byte aligned and also
16-byte aligned (bonus). Both verified by the spike test.

### `gc_flags` bit field

```
bit 0:   GC mark low   (tri-color: 00=white, 01=grey, 10=black)
bit 1:   GC mark high
bits 1:0 mask: STA_GC_COLOR_MASK = 0x03
bit 2:   STA_GC_FORWARDED   — object copied; header stores new address
bit 3:   STA_GC_REMEMBERED  — old-space object references young-space object
bits 7:4: reserved — must be zero (asserted in debug builds)
```

### `obj_flags` bit field

```
bit 0:   STA_OBJ_IMMUTABLE   — writes to slots are a runtime error
bit 1:   STA_OBJ_PINNED      — must not be moved by a compacting GC pass
bit 2:   STA_OBJ_ACTOR_LOCAL — must not escape this actor's heap
bit 3:   STA_OBJ_SHARED_IMM  — lives in shared immutable region; GC must not collect
bit 4:   STA_OBJ_FINALIZE    — registered for finalization callback before collection
bits 7:5: reserved — must be zero (asserted in debug builds)
```

### `size` field semantics

`size` is the payload count in **OOP-sized words**. One word = one `STA_OOP`
= 8 bytes on arm64. A `ByteArray` of N bytes requires `ceil(N / 8)` words;
the actual byte count is stored in a leading payload word. This is the Squeak
convention. Maximum payload: 4 294 967 295 words × 8 bytes = ~32 GB —
sufficient for any foreseeable use; `uint32_t` does not need widening.

### `class_index` field semantics

`class_index` is a 32-bit index into a global class table, not a direct
pointer. At 4 bytes instead of 8, this saves 4 bytes per object. At one
million live objects that is 4 MB. The class table is shared coordinated
mutable metadata (§8.5); access requires a reader lock or equivalent
synchronization strategy (deferred to a future ADR on class table
concurrency — see Open Questions).

### Allocation helper

```c
/* Total bytes to allocate for an object with n payload words. */
static inline size_t sta_alloc_size(uint32_t n) {
    return 16u + (size_t)n * sizeof(STA_OOP);
}
```

### Actor density measurement

**Measured `sizeof(STA_Actor)` = 80 bytes** (stub struct with plausible
field types; see `src/vm/actor_spike.h`).

| Component | Bytes |
|---|---|
| `STA_Actor` runtime struct | 80 |
| Initial nursery slab (minimum) | 128 |
| Actor identity object (0-slot header) | 16 |
| **Total per-actor creation cost** | **224** |

**Target: ~300 bytes. Measured: 224 bytes. Delta: −76 bytes (under budget).**

The 300-byte target is met with 76 bytes of headroom. This headroom is not
slack to be spent freely — it provides room for fields that will be added to
`STA_Actor` as the scheduler, GC, and capability system mature. If the struct
grows beyond 128 bytes (current: 80), a new ADR must justify the increase.

---

## Rationale

### Q1: Why pad the 12-byte header to 16 bytes? (Option A)

`sizeof(STA_ObjHeader)` is 12 with no internal padding on any conforming
C17 compiler (`4+4+1+1+2 = 12`). The first payload word at `header + 12` is
misaligned for 8-byte reads on arm64, which requires 8-byte alignment for
pointer-sized loads. Three options were evaluated:

- **Option A (chosen):** Pad the allocation unit to 16 bytes. 4 bytes of
  intentional waste per object. Payload at offset 16, always 8-byte aligned.
  Simple, explicit, no allocator magic.
- **Option B:** Pack flags into high bits of `size` or `class_index`, keeping
  the header at 8 bytes. Saves 8 bytes per object but eliminates the named
  flag bytes and complicates any future expansion.
- **Option C:** Keep 12 bytes and require the allocator to over-align to 16
  anyway. Same cost as Option A but less explicit.

Option A was chosen. The 4-byte waste is real but acceptable: at one million
objects it is 4 MB, which is negligible compared to payload. The `reserved`
field occupies 2 of those 4 bytes and is available for future use without
changing the allocation unit size.

### Q2: OOP tagging — why 1 bit, why not immediates for nil/true/false?

63-bit SmallInt is correct and sufficient for all Smalltalk arithmetic. A
single tag bit is the lowest-overhead scheme with no false positives on any
aligned heap pointer. Nil/true/false as heap objects in the shared immutable
region (§8.4) keeps the tag scheme minimal and matches the Blue Book's object
model. The implementation cost of pointer comparison for `isNil` is one
load + compare, which is fast enough that no measured hot-path justifies the
complexity of reserving additional tag patterns now.

### Q3: Why define all flag bits before any GC or isolator is written?

The header size is fixed at allocation time and baked into every saved image.
Adding bits ad hoc after images exist requires a migration. Defining all bits
now — even those unused in Phase 0 — costs nothing and prevents the layout
from changing later.

### Q4: Why is `size` in words rather than bytes?

OOP-granularity is the natural unit for GC traversal and slot access — the
GC walks payload slots one OOP at a time regardless of object type. Storing
byte counts would require a division on every traversal. For variable-size
objects (ByteArray, String) that need an exact byte count, the count is stored
as a leading payload word per Squeak convention.

### Q5: Forwarding pointer mechanics

Deferred — see Open Questions item 3. Decided before GC is implemented.

### Q6: Why `class_index` rather than a direct pointer?

A 4-byte index saves 4 bytes per object relative to an 8-byte pointer. More
importantly it keeps the header at 12 bytes (padded to 16), whereas a direct
pointer would push the natural header size to 16 bytes (8+4+1+1+2 = 16,
with padding to align the pointer field). Both layouts have a 16-byte
allocation unit, so the savings is the 4 bytes of `reserved` that remains
available for future fields under the index scheme.

---

## Open questions (deferred)

These questions were identified by the spike and deliberately left for future
ADRs. They must be resolved before the corresponding runtime component is
built.

1. **Nil / True / False as immediates.** Should be decided before the first
   bytecode dispatch loop is written. If `isNil` appears on a critical hot
   path, a 2-bit immediate encoding (bits 1:0 = `10` for nil/true/false) is
   worth the tag complexity.

2. **Character representation.** ~~Immediate vs. interned heap object. Must be
   decided before string/character primitives are implemented.~~ **Resolved —
   see §Character representation decision below.**

3. **Forwarding pointer mechanics.** When `STA_GC_FORWARDED` is set, where
   exactly is the new address stored? The 12-byte header has only `class_index`
   and `size` to repurpose (8 bytes total = one 64-bit pointer). This works
   on arm64 but is fragile. A dedicated forwarding word in the header padding
   is the cleaner design. Decide before GC is implemented.

4. **Class table concurrency.** Reader-writer lock, epoch-based access, or
   seqlock? Decide before the method cache is built.

5. **Activation frame layout.** The interpreter's frame layout is not an
   `ObjHeader` but sits adjacent to the object model. A companion spike is
   needed before the bytecode interpreter is written. Relevant to TCO support
   (Appendix A of the architecture document).

---

## Character representation decision

**Status:** Accepted (addendum, 2026-03-11)
**Resolves:** Open question 2

### Decision: compiled Unicode tables (Option C)

Character property data — category, case mapping, bidirectional class,
digit value, decomposition — is compiled offline from the Unicode Consortium's
`UnicodeData.txt` into static C lookup tables and shipped as a generated
header (`src/vm/unicode_tables.h`). No platform Unicode API is used by the VM.

Three options were evaluated:

- **Option A: Bundle and parse `UnicodeData.txt` at bootstrap.** Fully
  portable. Cost: ~2 MB file, a parser, and runtime indexing at bootstrap.
  Used by Cuis Smalltalk.
- **Option B: Delegate to platform libc** (`<wctype.h>`, `towupper()` etc.).
  Zero bundle cost, but behavior is locale-dependent and coverage varies
  across platforms. Unacceptable for a system that must behave consistently
  on macOS, Linux, and any future arm64 target.
- **Option C (chosen): Compile `UnicodeData.txt` offline into static C
  lookup tables.** Same portability as Option A — the VM carries its own
  Unicode knowledge and has no platform dependency — but with no parse
  overhead at runtime. The generator runs once during development; the
  generated tables are committed to the repo and updated when the Unicode
  version is bumped. This is the approach used by CPython, Ruby MRI, and
  most production language runtimes.

### Rationale

The VM must be platform-agnostic. arm64 runs on macOS, Linux, and other
targets; behavior of `Character isLetter`, `Character asUppercase`, and
string sorting must be identical everywhere. Option B is eliminated on
this basis alone.

Option C over Option A: the bootstrap sequence is already the most fragile
part of Phase 1 (see §11 of the architecture document). Adding a Unicode
parser to the bootstrap path increases complexity and bootstrap time for
no runtime benefit. Static tables are faster, simpler, and carry zero
bootstrap risk.

### What the tables cover

The generated tables provide, at minimum:

- General category (`Lu`, `Ll`, `Lt`, `Lm`, `Lo`, `Nd`, `Zs`, etc.)
- Simple uppercase / lowercase / titlecase mappings
- Bidirectional class (needed for correct string display)
- Numeric digit value (for `Character digitValue`)
- Derived properties: `isLetter`, `isDigit`, `isAlphaNumeric`,
  `isSeparator`, `isUppercase`, `isLowercase`

Full Unicode normalization (NFC/NFD/NFKC/NFKD) and complex case folding
are deferred — they are not required by ANSI Smalltalk and can be added
as a later layer without changing the table format.

### Character OOP representation

Characters are **immediate values** using a 2-bit tag in the low bits of
the OOP, distinct from SmallInt (bit 0 = 1) and heap pointers (bit 0 = 0).
The encoding uses bits 1:0 = `10` to identify a Character immediate, with
the Unicode code point in bits 63:2. This gives a 62-bit code point range —
far exceeding Unicode's current 21-bit requirement (U+0000 to U+10FFFF).

```c
/* Character immediate: bits 1:0 = 0b10, code point in bits 63:2 */
#define STA_IS_CHAR(oop)      (((oop) & 3u) == 2u)
#define STA_CHAR_VAL(oop)     ((uint32_t)((oop) >> 2))
#define STA_CHAR_OOP(cp)      (((STA_OOP)(cp) << 2) | 2u)
```

This supersedes the note in ADR 007 §Q2 that nil/true/false carry no special
tag and `isNil` uses pointer comparison. The tag space is now:

| bits 1:0 | Meaning |
|---|---|
| `01` | SmallInt (63-bit signed, value in bits 63:1) |
| `10` | Character immediate (code point in bits 63:2) |
| `00` | Heap pointer (16-byte aligned; bits 3:2 also zero) |
| `11` | Reserved |

`nil`, `true`, and `false` remain heap objects in the shared immutable
region. The `isNil` pointer comparison is unchanged.

### Note on the `reserved` field in STA_ObjHeader

The 2-byte `reserved` field at offset 10 in `STA_ObjHeader` is not consumed
by this decision. Character objects do not exist as heap objects under this
scheme — all Characters are immediates. The `reserved` field remains
available for future header extensions.

---

## Consequences

- `STA_ObjHeader` is locked at 12 bytes / 16-byte allocation unit. Any future
  change to the allocation unit invalidates existing saved images and requires
  a migration plan.
- All allocators must return 16-byte aligned memory.
- The `reserved` field (2 bytes at offset 10) and the 4-byte allocation gap
  are available for future header extensions without changing the allocation
  unit.
- `gc_flags` bits 7:4 and `obj_flags` bits 7:5 are reserved and must be zero.
  Debug builds should assert this on every header write.
- The `class_index` scheme requires a concurrent-safe class table. Its
  synchronization strategy is deferred but must be designed before Phase 1
  method dispatch is implemented.
- **The OOP tag scheme is extended by the Character immediate decision.**
  bits 1:0 = `10` identifies a Character immediate; bits 1:0 = `01` remains
  SmallInt; bits 1:0 = `00` remains heap pointer. All existing SmallInt and
  heap pointer macros are unaffected — `STA_IS_SMALLINT` checks bit 0 only
  and correctly excludes Character immediates (bit 0 = 0 for `10`).
  `STA_IS_HEAP` must be updated: a heap pointer requires bits 1:0 = `00`,
  not merely bit 0 = 0.
- Unicode character property data is carried in static compiled tables
  (`src/vm/unicode_tables.h`). No platform Unicode API is called by the VM.
  The table generator must be re-run and the tables re-committed when the
  Unicode version is bumped.
