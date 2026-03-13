# ADR 012 — Image Format and Snapshot Protocol

**Status:** Accepted
**Date:** 2026-03-11
**Spike:** `docs/spikes/spike-006-image.md`
**Depends on:** ADR 007 (object header layout), ADR 008 (mailbox), ADR 009 (scheduler), ADR 010 (frame layout), ADR 011 (async I/O)

---

## Decision

The Smalltalk/A image format is a **flat binary file with an explicit relocation
table**. Heap pointers are serialised as stable numeric object IDs (not
addresses); shared immutable objects (nil, true, false, symbols, class stubs)
are encoded by name string and resolved by the runtime on load. The root of the
serialised graph is always object ID 0.

The `STA_Actor` struct requires **one new field** for snapshot support:
`uint32_t snapshot_id` (4 bytes, plus 4 bytes alignment padding). The quiesce
signal reuses an existing `sched_flags` bit, not a new field.

---

## Measured results (arm64 M4 Max, Apple clang 17, C17, TSan, `-g`)

All measurements from `test_image_spike` via `ctest`.

### Format struct sizes

| Structure | Size | Notes |
|---|---|---|
| `STA_ImageHeader` | **48 bytes** | Written once per file |
| `STA_ObjRecord` | **16 bytes** | Fixed overhead per object, excl. payload |
| `STA_ImmutableEntry` | **10 bytes** | Fixed overhead per immutable, excl. name |
| `STA_RelocEntry` | **8 bytes** | Per heap-pointer payload slot |

All verified with `static_assert` at compile time.

### Throughput — 1,000 Array objects × 8 SmallInt slots (TSan build)

| Operation | Time | Objects/sec | Bytes | Throughput |
|---|---|---|---|---|
| Save | ~1.68 ms | ~595,000/sec | 80,048 | ~47.9 MB/s |
| Load | ~1.43 ms | ~700,000/sec | — | — |

Per-object on-disk size: 80 bytes (16-byte record + 64 bytes payload, no heap
pointer slots → no reloc entries). Matches theoretical formula:
`sizeof(STA_ObjRecord) + 8 * sizeof(STA_OOP) = 16 + 64 = 80 bytes`.

### Actor density

| Component | Bytes |
|---|---|
| `STA_ActorIo` (ADR 011 baseline) | 144 |
| `snapshot_id` + `_pad1` (this ADR) | +8 |
| `STA_ActorSnap` total | **152** |
| Initial nursery slab | 128 |
| Actor identity object (0-slot header) | 16 |
| **Total creation cost** | **296** |
| Target | ~300 |
| **Headroom** | **4 bytes** |

`_Static_assert(sizeof(STA_ActorSnap) == 152)` passes at compile time.

### Restore integrity

All of the following hold after `sta_image_load()` on the minimal test graph
(nil, true, false, sym\_hello, arr[SmallInt(42), sym\_hello, nil]):

- `arr->size == 3` ✓
- `arr->gc_flags` and `arr->obj_flags` round-trip exactly ✓
- `arr->payload[0]` is SmallInt(42) with `STA_SMALLINT_VAL == 42` ✓
- `arr->payload[1]` is the canonical runtime address of `sym_hello` ✓
- `sym_hello` payload word round-trips to the identical OOP value ✓
- `arr->payload[2]` is the canonical runtime address of `nil_obj` ✓

### TSan

Single-threaded spike (no scheduler). No shared state introduced. TSan clean.

---

## File format

### Layout

```
[offset 0]                  STA_ImageHeader (48 bytes)
[immutable_section_offset]  Immutable section (variable)
[data_section_offset]       Object data records (variable)
[reloc_section_offset]      Relocation table (variable)
```

### STA_ImageHeader (48 bytes, `__attribute__((packed))`)

| Field | Type | Value |
|---|---|---|
| `magic[4]` | uint8_t | `"STA\x01"` (0x53 0x54 0x41 0x01) |
| `version` | uint16_t | 1 |
| `endian` | uint8_t | 0 = little-endian, 1 = big-endian |
| `ptr_width` | uint8_t | 8 (arm64 — rejected if ≠ 8) |
| `object_count` | uint32_t | Total objects including immutables |
| `immutable_count` | uint32_t | Shared immutable objects |
| `immutable_section_offset` | uint64_t | File offset to immutable section |
| `data_section_offset` | uint64_t | File offset to object data section |
| `reloc_section_offset` | uint64_t | File offset to relocation table |
| `file_size` | uint64_t | Total file size in bytes |

### OOP encoding in snapshot files

In the snapshot file, OOPs are encoded as:

- **SmallInt** (bit 0 = 1): stored verbatim — no fixup needed.
- **Character immediate** (bits 1:0 = 10): stored verbatim — no fixup needed.
- **Heap pointer** (bits 1:0 = 00 in live OOP): encoded as
  `(object_id << 2) | 0x3`.

`STA_SNAP_HEAP_TAG = 0x3` (bits 1:0 = 11) is the reserved tag in the live OOP
scheme (ADR 007). It cannot appear in a live heap pointer (which requires bits
1:0 = 00), so it is unambiguous in context. **Disambiguation between an odd
SmallInt and a snapshot heap OOP is done via the relocation table**, not tag
bits alone. The relocation table is authoritative: only slots listed there are
decoded as heap OOPs on load. All other slots are treated as verbatim values.
Snapshot files are not live OOP streams; this is safe.

### Object record (STA_ObjRecord, 16 bytes fixed + payload)

| Field | Type | Meaning |
|---|---|---|
| `object_id` | uint32_t | Sequential, 0-based |
| `class_key` | uint32_t | Class index; high bit set if shared immutable class |
| `size` | uint32_t | Payload words (matches `STA_ObjHeader.size`) |
| `gc_flags` | uint8_t | Copied verbatim from header |
| `obj_flags` | uint8_t | Copied verbatim from header |
| `reserved` | uint16_t | Must be zero |
| payload | — | `size × 8` bytes of encoded OOPs |

### Immutable section entry (STA_ImmutableEntry, 10 bytes fixed + name)

| Field | Type | Meaning |
|---|---|---|
| `stable_key` | uint32_t | FNV-1a hash of the name string |
| `name_len` | uint16_t | Byte length of name, no null terminator |
| `immutable_id` | uint32_t | Object ID assigned to this immutable |
| name bytes | — | `name_len` bytes of UTF-8 |

On load, the resolver callback maps each name → runtime `STA_ObjHeader *`.
This ensures nil/true/false/Symbol use the canonical runtime address, not a
freshly allocated duplicate.

### Relocation table entry (STA_RelocEntry, 8 bytes)

| Field | Type | Meaning |
|---|---|---|
| `object_id` | uint32_t | Object containing the heap-pointer slot |
| `slot_index` | uint32_t | Payload word index (0-based) |

Flat array. On load (pass 5), each entry is decoded: extract the referenced
object ID from the encoded OOP, look up the runtime address in the ID→address
table, write the live pointer.

---

## Rationale

### Q1 — Why a flat binary with relocation table instead of dependency-ordered output?

Dependency-ordered output (topological sort, leaves first) eliminates the
relocation table but requires O(n·m) sort across n objects and m pointers.
The flat format with reloc table is simpler, linear, and equally fast. The
relocation table is compact: 8 bytes per heap-pointer slot. For a graph with
many SmallInt slots and few cross-object pointers (the common case), the reloc
table is small relative to the data section.

A secondary benefit: the flat format is amenable to memory-mapped load in Phase
1 (replace five `fseek+fread` passes with direct pointer arithmetic on a mapped
region), without changing the format itself.

### Q2 — Why object IDs instead of file offsets?

File offsets require objects to be written in a deterministic order that is
known at reference time, or a two-pass save that computes offsets first. Object
IDs are assigned during registration (a single sequential counter) and are
independent of the on-disk layout. The reloc table records (object\_id,
slot\_index) pairs; the load pass resolves these after all objects are
allocated. File-offset encoding would couple the pointer encoding to the
write order, making format evolution harder.

### Q3 — Why immutable objects encoded by name, not by a stable integer key?

A stable integer key (e.g., a class index) requires a globally consistent
mapping between the image file and the runtime. Name strings are
self-describing: any runtime that understands the name "nil" can resolve the
immutable without consulting an external registry. FNV-1a is used as a compact
stable hash for quick lookup; the full name string is stored alongside it for
collision resolution and debuggability.

### Q4 — Why add `snapshot_id` to `STA_Actor` rather than computing it on demand?

On-demand ID assignment during save requires a scan of all registered objects
to find the actor's position, or a hash-table lookup. A stored `snapshot_id`
(zero meaning "not yet assigned") allows the save path to assign IDs
incrementally without revisiting the actor struct. The cost is 4 bytes (+ 4
bytes pad for alignment); see the density table above.

### Q5 — Why reuse `sched_flags` bit for QUIESCED rather than a new field?

`STA_ACTOR_QUIESCED = 0x08u` occupies bit 3 of `sched_flags`, which is
currently unused (bits 0–2 are NONE/RUNNABLE/RUNNING/SUSPENDED from ADRs 009
and 011). Adding a new boolean field for a single bit would cost 4 bytes. The
`sched_flags` field is already atomic, already read by the scheduling loop, and
is the correct place for a scheduler-visible actor lifecycle state bit.

### Q6 — Why malloc per object on load rather than a slab allocator?

This is a spike. malloc is the correct choice for Phase 0 because it is
portable, debuggable, and imposes no alignment or lifetime constraints on the
spike infrastructure. Phase 1 will replace per-object malloc with arena
allocation from the actor's nursery slab; that change is O(lines changed) and
does not affect the file format.

### Q7 — Why is the load throughput (~700k objects/sec) acceptable for Phase 1?

Phase 1 development images contain << 10,000 objects (kernel bootstrap images
are small). At 700k objects/sec, a 10,000-object image loads in ~14 ms — well
within any interactive tolerance. Production image load times are a Phase 3
concern. The format and protocol are not load-throughput-limiting factors; the
malloc-per-object spike baseline is.

---

## Actor snapshot fields

### Fields added to `STA_Actor`

```c
uint32_t  snapshot_id;   /* 4 bytes — actor's object ID in snapshot; 0 = unassigned */
uint32_t  _pad1;         /* 4 bytes — alignment padding */
```

Total addition: **8 bytes**.

### Scheduler flag extension

```c
#define STA_ACTOR_QUIESCED  0x08u  /* sched_flags bit: actor stopped for snapshot */
```

Extends the flag constants from ADR 009 (NONE/RUNNABLE/RUNNING) and ADR 011
(SUSPENDED). No new field. No additional density cost.

### Quiescing protocol (Phase 1 definition scope)

The quiescing protocol — how a running actor is cleanly stopped so its heap
can be serialised — is not fully specified here. It is a Phase 1 concern.
Candidate: the reduction counter trips to zero at the next scheduling point,
the actor sets `STA_ACTOR_QUIESCED`, and the scheduler parks it without
re-enqueueing. See Open Questions.

---

## Consequences

- **File format is locked at version 1.** The magic bytes and version field
  allow forward detection. Incompatible format changes require a version bump
  and a new ADR. The packed structs (`__attribute__((packed))`) and
  `static_assert` on all sizes lock the binary layout.

- **`STA_Actor` must add `snapshot_id` + 4-byte pad before Phase 1 image
  save is implemented.** `STA_ActorSnap` in `src/image/image_spike.h`
  documents the exact layout. `_Static_assert(sizeof(STA_ActorSnap) == 152)`
  passes at compile time.

- **`STA_ACTOR_QUIESCED = 0x08u` must be added to the `sched_flags` constants**
  alongside the existing ADR 009/011 constants before Phase 1 snapshot support
  is wired into the scheduler.

- **4 bytes of headroom remain** against the 300-byte per-actor creation cost
  target. This is the tightest margin across all Phase 0 spikes. No field may
  be added to `STA_Actor` without a new ADR. Any addition that consumes the
  remaining 4 bytes — or requires a pad reduction to fit — breaches the
  300-byte target and must be explicitly justified per CLAUDE.md.

- **Root table: root-of-roots Array convention adopted.** The single root OOP
  points to an immutable Array of fixed-index roots. See §Resolved open
  questions. No format change required.

- **Memory-mapped load deferred to Phase 1.** The five-pass fseek+fread
  implementation is correct and sufficient for Phase 0 development images.
  Replace with `mmap` + direct pointer arithmetic for Phase 1 production
  images. The format is compatible with memory-mapped access without change.

- **ADR 012 open question 1 (quiescing) is a Phase 1 blocker.** Image save
  of a live actor graph requires a safe-point mechanism. Design it before the
  Phase 1 kernel bootstrap image save is implemented.

---

## Resolved open questions

### Open question 2 — Root table for multi-root images

**Status:** Resolved (2026-03-13)

**Decision:** Root-of-roots Array convention. The single `root` OOP in the
image header points to an immutable `Array` containing all top-level image
roots at fixed indices. No change to the binary format.

**Root Array layout:**

| Index | Constant | Content |
|---|---|---|
| 0 | `STA_IMAGE_ROOT_SPECIAL_OBJECTS` | Array of 32 OOPs — the special object table |
| 1 | `STA_IMAGE_ROOT_CLASS_TABLE` | Array of class OOPs — the class table |
| 2 | `STA_IMAGE_ROOT_GLOBALS` | SystemDictionary — the global dictionary |

**Constants (defined in `src/image/` headers):**

```c
#define STA_IMAGE_ROOT_SPECIAL_OBJECTS  0
#define STA_IMAGE_ROOT_CLASS_TABLE      1
#define STA_IMAGE_ROOT_GLOBALS          2
#define STA_IMAGE_ROOT_COUNT            3
```

**Rationale:**
- Zero change to the spike-006 binary format — the header stays 48 bytes and
  the root OOP field stays one field.
- The root Array is a normal immutable object, serialised and relocated like
  everything else — no special-case serialisation logic.
- Extensible by convention: Phase 2+ adds `root[3]`, `root[4]` etc. for actor
  heaps, capability graphs, etc., without any format change.
- The loader reads the root, verifies it is an Array of size ≥
  `STA_IMAGE_ROOT_COUNT`, and extracts the three objects by index.
- Bootstrap creates the root Array as its final step before calling the image
  writer.

### Open question 4 — Class identifier portability

**Status:** Resolved (2026-03-13)

**Decision:** Fixed indices 0–31, name-based resolution for indices 32+. Class
indices are an in-memory optimisation (fast array lookup), not a semantic
identity. The class name is the portable identity. Indices are rebuilt on load.

**Mechanism:**
- Indices 0–31 are hardcoded in the bytecode spec and assigned during bootstrap.
  They are the same on every build by construction. The loader writes them
  directly to their fixed slots — no lookup needed.
- For indices 32+, the image includes a class name table: an array of
  `(class_index, symbol_key)` pairs serialised alongside the object data. The
  `symbol_key` is an FNV-1a hash of the class name, consistent with how
  spike-006 already encodes shared immutables.
- On load: the loader reads the name table, looks up each name in the global
  dictionary (restored from `root[2]`), and assigns class table slots. If a
  name is not found, the loader signals an error.
- Indices 32+ may be assigned differently on load than on save. The loader
  rewrites `class_index` fields in all object headers using the name table
  mapping. This extends the existing relocation table rewriting — the
  relocation table already rewrites OOP fields; extending it to rewrite
  `class_index` fields is a natural addition.

**Rationale:**
- Kernel classes 0–31 are stable by construction — no portability concern.
- Class name is the stable identity; index is a runtime optimisation.
- Builds on the existing ADR 012 relocation infrastructure — no new mechanism
  required.
- User-defined classes created at runtime are captured in the name table on
  save and resolved by name on load.
- No complex hashing or namespace machinery needed for Phase 1.

---

## Open questions (deferred)

1. **Quiescing protocol for live actors.** Stopping a running actor cleanly
   for snapshot without data races requires a safe-point: the actor must reach
   a point where its heap is consistent, then park. The `STA_ACTOR_QUIESCED`
   flag and the reduction-interrupt mechanism (ADR 009) are the tools; the
   exact protocol (who sets the flag, in what order relative to the scheduler)
   must be defined before Phase 1 image save. This is a Phase 1 blocker.

2. **Root table.** ~~A single root (object ID 0) is sufficient for the spike.
   Production images need: active process list, class dictionary, symbol table,
   bootstrap globals. Define a root table section (likely a header extension or
   a dedicated pre-data section) before Phase 1 image save.~~ **Resolved — see
   §Resolved open questions above.**

3. **Incremental save vs. stop-the-world.** Full-image save with a live
   scheduler requires either a read barrier (incremental, no stop) or a
   quiesce-all-actors pass (stop-the-world). BEAM uses process-local snapshots
   (no global stop); Squeak uses stop-the-world with a write barrier. Neither
   approach changes this format. Decide the quiesce strategy as part of the
   Phase 1 save protocol definition.

4. **Class identifier portability.** ~~The spike uses `class_index` (an actor-
   local integer) for non-immutable objects and FNV-1a name hash for immutable
   classes. The production class registry (Phase 1) must assign stable,
   portable class identifiers.~~ **Resolved — see §Resolved open questions
   above.**

5. **4-byte density headroom.** With only 4 bytes remaining, the next `STA_Actor`
   field addition requires a new ADR and an explicit decision: either accept a
   breach of the 300-byte target (justified by measurement) or recover bytes
   elsewhere in the struct. **This is the most urgent open question.** Tracking
   issue: see the decision-pending GitHub issue filed with this ADR.
