# Spike 006: Image Save/Load (Closed-World Subset)

**Phase 0 spike — image serialization format, pointer fixup, and restore integrity**
**Status:** Complete
**Produces:** ADR 012 (image format and snapshot protocol)

---

## Overview

This spike validates the image save/load format for a closed-world subset of the
Smalltalk/A object graph. No running scheduler is involved: objects are
constructed manually, serialized to a binary file, and restored. The goals are:

1. Validate a flat-binary format with a relocation table.
2. Confirm OOP encoding by stable numeric IDs (not addresses) survives round-trip.
3. Validate shared-immutable handling (nil, true, false, symbols, class stubs)
   encoded by stable string key, not address.
4. Measure the per-object overhead and throughput of save and load.
5. Determine what fields (if any) `STA_Actor` must grow to support snapshot.

---

## Format Specification

### File layout

```
[  0] STA_ImageHeader         48 bytes
[48] Immutable section        variable
[48 + imm_size] Data section  variable
[...] Relocation table        variable
```

### STA_ImageHeader (48 bytes, packed)

| Field                     | Type      | Value / meaning                        |
|---------------------------|-----------|----------------------------------------|
| `magic[4]`                | uint8_t   | `"STA\x01"` (0x53 0x54 0x41 0x01)     |
| `version`                 | uint16_t  | 1                                      |
| `endian`                  | uint8_t   | 0 = little, 1 = big; write native      |
| `ptr_width`               | uint8_t   | 8 (arm64 requirement)                  |
| `object_count`            | uint32_t  | total objects including immutables      |
| `immutable_count`         | uint32_t  | shared immutable objects                |
| `immutable_section_offset`| uint64_t  | file offset to immutable section        |
| `data_section_offset`     | uint64_t  | file offset to object data section      |
| `reloc_section_offset`    | uint64_t  | file offset to relocation table         |
| `file_size`               | uint64_t  | total file size in bytes               |

Header is written `__attribute__((packed))` to avoid any padding. Size is
validated with `static_assert(sizeof(STA_ImageHeader) == 48)`.

### OOP encoding in snapshot files

In the snapshot file, OOPs are encoded as follows:

| Tag bits (1:0) | Live meaning | Snapshot meaning |
|---|---|---|
| `01` or `11` (bit 0 = 1) | SmallInt | SmallInt — stored verbatim, no fixup |
| `10` | Character immediate | Character immediate — stored verbatim, no fixup |
| `00` | Heap pointer (8-byte aligned) | **Never written** — see below |
| `11` | Reserved (live OOP) | `(object_id << 2) \| 0x3` — snapshot heap OOP |

**Key insight:** `STA_SNAP_HEAP_TAG = 0x3` (bits 1:0 = 11) is the currently
reserved tag in the live OOP encoding (ADR 007), so it cannot appear as a
live heap pointer (which requires bits 1:0 = 00). In the snapshot file, tag
`0x3` identifies a pointer that must be fixed up.

**Collision with odd SmallInts:** An odd SmallInt `STA_SMALLINT_OOP(n)` where
n is odd also has bits 1:0 = 11. This is acceptable because disambiguation
between SmallInts and snapshot heap OOPs is done via the **relocation table**,
not tag bits alone. During save, only slots that hold live heap pointers (not
SmallInts, not Characters) get relocation entries. During load, only slots
listed in the relocation table are decoded as snapshot heap OOPs. All other
slots are treated as verbatim values.

This is only valid in snapshot files — live OOP streams never use tag `0x3`.

### Object record (STA_ObjRecord, 16 bytes fixed + payload)

| Field       | Type     | Meaning |
|-------------|----------|---------|
| `object_id` | uint32_t | Sequential, 0-based |
| `class_key` | uint32_t | High bit: 0 = actor-local class_index, 1 = immutable class |
| `size`      | uint32_t | Payload words (matches `STA_ObjHeader.size`) |
| `gc_flags`  | uint8_t  | Copied verbatim from header |
| `obj_flags` | uint8_t  | Copied verbatim from header |
| `reserved`  | uint16_t | Must be zero |
| payload     | —        | `size × 8` bytes, OOPs encoded per scheme above |

### Immutable section entry (STA_ImmutableEntry, 10 bytes fixed + name)

| Field          | Type     | Meaning |
|----------------|----------|---------|
| `stable_key`   | uint32_t | FNV-1a hash of the name string |
| `name_len`     | uint16_t | Byte length of name, no null terminator |
| `immutable_id` | uint32_t | Object ID assigned to this immutable |
| name bytes     | —        | `name_len` bytes of UTF-8 |

On restore, the loader calls the `STA_ImmutableResolver` callback for each
entry. The resolver maps name → runtime `STA_ObjHeader *`. This ensures that
a shared canonical nil/true/false/Symbol address is used rather than
allocating a duplicate.

### Relocation table entry (STA_RelocEntry, 8 bytes)

| Field        | Type     | Meaning |
|--------------|----------|---------|
| `object_id`  | uint32_t | Which object contains the heap-pointer slot |
| `slot_index` | uint32_t | Which payload word, 0-based |

The relocation table is a flat array. During load (pass 5), the loader walks
this table and patches each identified slot: reads the `(object_id << 2) | 0x3`
value, extracts the referenced object_id, looks up the runtime address in the
id→address table, and writes the live pointer.

---

## Variants Considered

### V1: Flat binary with relocation table (CHOSEN)

This spike implements a single flat binary file: header, immutable section,
object data section (sequential records), relocation table. Load requires 5
passes over the file (read header, resolve immutables, allocate objects,
fill payloads, apply relocs). All passes are O(n) where n = number of objects.

**Advantages:**
- Simple, no tree structure, no recursive pointer chasing during save.
- Relocation table is compact (8 bytes/entry) and decouples pointer fixup from
  object allocation order.
- A single `fseek` + linear read per section; cache-friendly for large images.
- The file can be memory-mapped in Phase 1 for zero-copy load.

**Disadvantages:**
- Two-pass save (first build reloc table, then write). In practice both passes
  are pure CPU with no I/O, so this is not a bottleneck.
- No incremental/append mode. Image save is always a full snapshot.

### V2: Address-sorted object graph with embedded forward pointers

Objects written in dependency order (leaves first), with inline object IDs.
Eliminates the separate reloc table but requires topological sorting during
save and imposes ordering constraints on load.

**Rejected:** Topological sort is O(n·m) for n objects and m pointers in the
worst case. The flat format with reloc table is simpler and equally fast.

### V3: Text-based format (Squeak-style .st image segments)

Human-readable format using printString representations.

**Rejected immediately:** No binary round-trip guarantees for arbitrary objects;
slow for any non-trivial image size; not relevant to the performance targets.

---

## Measured Results

All measurements taken on Apple M4 Max (arm64), macOS Tahoe, Debug build
(TSan enabled for the ctest gate), `-O2` for the bench target.

### Minimal graph (5 objects: nil, true, false, sym_hello, arr)

| Metric | Value |
|---|---|
| File size | < 1 KB |
| Save time | < 0.1 ms |
| Load time | < 0.1 ms |

### Large graph: 1000 Array objects × 8 SmallInt slots (Debug+TSan)

| Metric | Debug+TSan | Notes |
|---|---|---|
| File size | 80,048 bytes | ≈ 80 bytes/object |
| Save time | ~1.68 ms | ≈ 595,000 objects/sec |
| Load time | ~1.43 ms | ≈ 700,000 objects/sec |
| Save throughput | ~47.9 MB/s | |

### Large graph (Optimized `-O2`, no TSan)

| Metric | -O2 | Notes |
|---|---|---|
| Save | ~1.67 ms | ≈ 598,000 objects/sec |
| Load | ~1.42 ms | ≈ 706,000 objects/sec |
| Save throughput | ~47.9 MB/s | |

**Observation:** The TSan overhead for this single-threaded spike is negligible —
the Debug+TSan and -O2 numbers are nearly identical. The bottleneck is malloc
(one per object) in the load path; a slab allocator in Phase 1 will eliminate
this.

### Per-object overhead

| Structure | Fixed bytes | Notes |
|---|---|---|
| `STA_ImageHeader` | 48 | Written once per file |
| `STA_ObjRecord` | 16 | Per object, excl. payload |
| `STA_ImmutableEntry` | 10 | Per immutable, excl. name |
| `STA_RelocEntry` | 8 | Per heap-pointer slot |

For a typical object with 8 SmallInt payload slots (no heap pointers): 16 + 64
= 80 bytes on disk. This matches the measured 80,048 bytes / 1000 = 80 bytes/obj.

---

## Restore Integrity Test Results

Test 4 (restore integrity) asserts the following after `sta_image_load()`:

| Assertion | Result |
|---|---|
| `arr->size == 3` | PASS |
| `arr->gc_flags == STA_GC_WHITE` | PASS |
| `arr->obj_flags == STA_OBJ_ACTOR_LOCAL` | PASS |
| `arr->payload[0]` is SmallInt(42) | PASS |
| `STA_SMALLINT_VAL(arr->payload[0]) == 42` | PASS |
| `arr->payload[1]` is live heap ptr to sym_hello | PASS |
| `arr->payload[1]` == canonical runtime sym_hello address | PASS |
| sym_hello payload word round-trips exactly | PASS |
| `arr->payload[2]` is live heap ptr to nil_obj | PASS |
| `arr->payload[2]` == canonical runtime nil_obj address | PASS |

All OOP values, all flags, and all payload words round-trip identically.

---

## Actor Density Impact

### Added fields to STA_Actor

| Field | Bytes | Justification |
|---|---|---|
| `snapshot_id` (uint32_t) | 4 | Actor's object ID in snapshot; 0 = not assigned |
| `_pad1` (uint32_t) | 4 | Alignment padding |
| `STA_ACTOR_QUIESCED` flag | 0 | Bit 3 of existing `sched_flags` — no new field |

Total addition: **8 bytes**.

### Density table (cumulative)

| Spike | Component | Size | Creation cost | Headroom |
|---|---|---|---|---|
| 002 | STA_ActorRevised (mailbox + sched) | 120 B | — | — |
| 003 | + next_runnable + home_thread + pad | 128 B | — | — |
| 004 | + stack_base + stack_top | 136 B | — | — |
| 005 | + io_state + io_result | 144 B | 288 B | 12 B |
| **006** | **+ snapshot_id + pad** | **152 B** | **296 B** | **4 B** |

**4 bytes of headroom remain** before the 300-byte per-actor creation cost
target is breached. This is the tightest the budget has been.

`static_assert(sizeof(STA_ActorSnap) == 152)` passes in `image_spike.c`.

---

## Open Questions for ADR 012

1. **Quiescing protocol:** How do we stop a running actor cleanly for snapshot?
   The `STA_ACTOR_QUIESCED` flag in `sched_flags` is defined, but the protocol
   for transitioning an actor from RUNNING → QUIESCED without data races is not
   specified here. This is a Phase 1 concern (full image save requires a safe
   point mechanism). Candidate: reduction-count interrupt + cooperative yield.

2. **Root object convention:** This spike uses object_id 0 as the root. A
   production image may have multiple roots (the active process list, the class
   dictionary, the symbol table). ADR 012 should define a root table section.

3. **Incremental image saves:** Full snapshot of a live image requires either
   a stop-the-world or a read barrier. ADR 012 should pick one. BEAM uses
   process-local snapshots (no global stop); Squeak uses stop-the-world with a
   write barrier for pre-copy GC.

4. **Class encoding:** This spike uses `class_index` (actor-local integer) or
   `FNV-1a hash | STA_CLASS_KEY_IMMUT_FLAG` (shared class). The production
   class registry (Phase 1) needs a stable, image-portable class identifier.
   Recommendation: class name string, same mechanism as immutable symbols.

5. **4-byte headroom:** With only 4 bytes remaining, any future `STA_Actor`
   field addition requires a new ADR. The Phase 1 implementation must either
   hold this line or explicitly file an ADR to breach the 300-byte target.

6. **Memory-mapped load:** The 5-pass file-seek approach used in the spike
   works but is suboptimal for large images. Phase 1 should memory-map the
   file and replace the five `fseek+fread` passes with direct pointer arithmetic.

---

## Recommendation

Accept the flat-binary format with relocation table as the basis for ADR 012.

- **Format:** Magic + version + endian + ptr_width header, immutable section,
  object data section, relocation table.
- **OOP encoding:** SmallInts and Character immediates verbatim; heap pointers
  as `(object_id << 2) | 0x3`; relocation table is the authoritative
  discriminator (not tag bits alone).
- **Shared immutables:** Encoded by FNV-1a name key; resolved at load time via
  an `STA_ImmutableResolver` callback. This decouples the image format from any
  specific runtime address.
- **Actor snapshot field:** Add `uint32_t snapshot_id` (+ 4-byte pad for
  alignment) to `STA_Actor`. Use bit 3 of `sched_flags` for `STA_ACTOR_QUIESCED`.
  Total addition: 8 bytes. New creation cost: **296 bytes (4 bytes headroom)**.
- **Throughput:** ~600,000 objects/sec save, ~700,000 objects/sec load on M4 Max.
  Adequate for Phase 1 development images (< 100,000 objects). Revisit with
  mmap for production images.
- **TSan:** Single-threaded spike is clean. No shared state introduced.
