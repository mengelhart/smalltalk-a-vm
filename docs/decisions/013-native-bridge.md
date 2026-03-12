# ADR 013 — Native Bridge (C Runtime ↔ SwiftUI IDE)

**Status:** Accepted
**Date:** 2026-03-11
**Spike:** `docs/spikes/spike-007-native-bridge.md`
**Closes:** ADR 006 (Handle lifecycle — DEFERRED status resolved)
**Depends on:** ADR 002 (public API boundary), ADR 005 (error reporting), ADR 012 (image format)

---

## Decision

The native bridge between the C runtime and the SwiftUI IDE is defined entirely
through `include/sta/vm.h`. The bridge adds nine new public functions and five
new public types. No privileged back-channel exists. All functions are safe to
call from any OS thread.

The **handle model** is explicit reference counting (JNI/CPython pattern): any
function returning `STA_Handle*` returns an acquired handle (refcount 1); the
caller must release via `sta_handle_release`. The runtime registers handles as
GC roots. The `STA_Handle*` pointer is stable; the OOP inside the entry is
updated in place on GC move.

The **threading model** uses three narrow locks (IDE-API, method-install, and
actor-registry) that the scheduler never holds. These are not a GIL.

The **live update path** is validated as TSan-clean under concurrent scheduler
activity. Full compiler integration is Phase 1.

The **event model** is push: the VM calls registered C callbacks from whichever
thread the event occurs on; the Swift caller dispatches to the main thread.

**`STA_Actor` is unchanged.** The bridge adds 0 bytes to the actor struct.
The 4-byte density headroom from ADR 012 is preserved.

---

## Measured results (arm64 M4 Max, Apple clang 17, C17, TSan, `-g`)

All measurements from `test_bridge_spike` via `ctest`.

### Struct sizes

| Structure | Size | Notes |
|---|---|---|
| `struct STA_Handle` | **16 bytes** | oop(8) + refcount(4) + pad(4); verified with `_Static_assert` |
| `STA_ActorEntry` | **48 bytes** | identity(16) + sup(16) + flags(4+4) + active+pad(8); verified with `_Static_assert` |
| `STA_MethodLog` | 393,224 bytes | 1024 entries × 384 bytes + overhead; VM-level, not per-actor |
| `STA_ClassDefLog` | 16,392 bytes | 64 entries × 256 bytes + overhead; VM-level |
| `struct STA_VM` | 441,880 bytes | Heap-allocated spike struct; irrelevant to per-actor density |

### Actor density checkpoint

| Component | Bytes |
|---|---|
| `STA_ActorSnap` (ADR 012 baseline) | 152 |
| Bridge spike addition to `STA_Actor` | **0** |
| Initial nursery slab | 128 |
| Actor identity object (0-slot header) | 16 |
| **Total creation cost** | **296** |
| Target | ~300 |
| **Headroom** | **4 bytes** |

Bridge state lives in `struct STA_VM`, not in `STA_Actor`. The 4-byte headroom
from ADR 012 is fully preserved.

### TSan result

All 15 tests passing. ctest result: **Passed, 0.12 s**. Zero TSan reports across:
- Handle lifecycle with concurrent scheduler thread (test 14)
- Concurrent method install: 8 threads × 100 installs = 800 log entries (test 7)
- Concurrent class define: 4 threads (test 8)
- Actor enumeration from background thread while spawn runs concurrently (tests 9, 10)
- Event dispatch from two background threads simultaneously (test 12)
- Full threading gate: IDE thread calling all API functions while scheduler thread runs (test 14)

---

## Handle model

### Bootstrapping

The current `vm.h` had no handle creation path — `sta_handle_retain` required an
existing handle, forcing the IDE to call `sta_eval` (which requires a live
interpreter) to obtain its first handle. The following well-known root accessors
close this gap:

```c
STA_Handle* sta_vm_nil(STA_VM* vm);
STA_Handle* sta_vm_true(STA_VM* vm);
STA_Handle* sta_vm_false(STA_VM* vm);
STA_Handle* sta_vm_lookup_class(STA_VM* vm, const char* name);
```

Each returns a freshly acquired handle (refcount 1). No interpreter is required.
The underlying root objects are allocated at `sta_vm_create` time and live for the
VM's lifetime.

### Acquire / retain / release protocol

1. Any function returning `STA_Handle*` returns an acquired handle (refcount 1).
   The caller owns one reference and must call `sta_handle_release` exactly once.
2. `sta_handle_retain` increments the refcount and returns the same pointer.
3. `sta_handle_release` decrements. When refcount reaches zero, the entry is freed
   and the OOP is no longer rooted.
4. After `sta_handle_release`, the `STA_Handle*` pointer is dangling — do not use.
5. All handles are invalidated by `sta_vm_destroy`. Using a handle after
   `sta_vm_destroy` is undefined behaviour.
6. Handle leaks are GC root leaks — the GC cannot collect a handle-rooted object.
   The Swift IDE should wrap handles in a class with `deinit` calling release.

### GC move invariant

The handle table is a GC root set. On GC move, the GC updates `handle->oop` in
place to the forwarded address. The `STA_Handle*` pointer itself (the table entry
address) is stable for the entry's lifetime. Any code holding the pointer across a
GC cycle sees the updated OOP transparently.

Validated in test 2: direct in-place OOP update simulates GC forwarding; the
handle pointer remains valid and reflects the new address.

### Spike handle table

Fixed array of 1,024 × 16-byte entries embedded in `struct STA_VM`. Not realloc'd —
entry addresses are stable. Phase 3 must replace with a growable slab structure
(see Open Questions).

---

## `vm.h` additions accepted (ADR 002 obligations)

Every addition is justified by a real IDE scenario. None is speculative.

| Function | IDE scenario | Group |
|---|---|---|
| `sta_vm_nil` | Any startup: obtain handle to nil | Well-known roots |
| `sta_vm_true` | Conditional display | Well-known roots |
| `sta_vm_false` | Conditional display | Well-known roots |
| `sta_vm_lookup_class` | Class browser: look up class by name | Well-known roots |
| `sta_inspect_cstring` | Inspector, Debugger, Actor Monitor: display string | Evaluation and inspection |
| `sta_method_install` | System browser: install edited method | Live update |
| `sta_class_define` | System browser: define new class | Live update |
| `sta_actor_enumerate` | Actor Monitor: walk actor tree | Actor interface |
| `sta_event_register` | VM events: register crash/install handler | Event callbacks |
| `sta_event_unregister` | VM events: unregister handler | Event callbacks |

New types added: `STA_ActorInfo`, `STA_ActorVisitor`, `STA_EventType`, `STA_Event`,
`STA_EventCallback`.

---

## Threading model

**All public API functions are safe to call from any OS thread.**

Three locks protect bridge state. The scheduler never holds any of them:

| Lock | Protects | Max hold time |
|---|---|---|
| `htbl_lock` (IDE-API lock) | Handle table + event callback table | Handle table scan (bounded) |
| `install_lock` (method-install lock) | Method log + class define log | One string copy |
| `actor_lock` (actor-registry lock) | Actor registry | One registry scan (snapshot phase only) |

None of these is a GIL. A GIL would serialise all VM execution through a single
lock. These three locks each protect a narrow, bounded critical section; the
scheduler runs without holding any of them.

**Lock-order rule:** if multiple locks must ever be held simultaneously (avoid
this), the order is: `actor_lock` → `htbl_lock` → `install_lock`. No code path
currently holds more than one at a time.

**`sta_inspect_cstring` is explicitly NOT thread-safe** (see below). The
`htbl_lock` serialises concurrent writes to the inspect buffer but does not extend
the validity of the returned pointer. The function is single-caller-at-a-time by
contract. ADR 013 records this explicitly to prevent the lock from creating a
false appearance of thread-safety.

---

## `sta_inspect_cstring` — not thread-safe by contract

```c
const char* sta_inspect_cstring(STA_VM* vm, STA_Handle* handle);
```

The returned pointer is VM-owned and valid until the next call to
`sta_inspect_cstring` on the same VM (from any thread) or until `sta_vm_destroy`.
The `htbl_lock` serialises the write to the inspect buffer; it does not prevent a
second thread from overwriting the buffer before the first thread reads the result.

**Contract:** single-caller-at-a-time. The IDE calls this from the main thread
only (all SwiftUI display is main-thread); this contract is met in practice.
Phase 3 changes the signature to a caller-provided buffer (source-breaking change):

```c
int sta_inspect_cstring(STA_VM*, STA_Handle*, char* buf, size_t len);
```

This is the standard POSIX pattern, trivially wrappable from Swift, and
eliminates the lifetime hazard entirely.

---

## Live update path

```c
int sta_method_install(STA_VM*, STA_Handle* class_handle,
                       const char* selector, const char* source);
int sta_class_define(STA_VM*, const char* source);
```

For this spike, these functions record their arguments in thread-safe logs
(method log and class define log respectively, each under `install_lock`).
Full compiler integration is Phase 1.

**Separation of logs:** `sta_class_define` records to `STA_ClassDefLog` (a
dedicated log, not the method log with a sentinel selector). This ensures test
assertions are structurally correct and that Phase 1 replacement of the class
define path does not require rewriting method log assertions.

**Existing activations are unaffected** (§14.2): in the permanent implementation,
stack frames hold a direct reference to the old compiled method object; the
method install atomically updates the class method dictionary pointer. The spike
validates the threading model only; frame-safety is validated in the Phase 1
interpreter spike.

TSan gate: test 7 (8 threads × 100 concurrent method installs = 800 log entries)
and test 8 (4 concurrent class defines) are TSan-clean.

---

## Actor enumeration

```c
int sta_actor_enumerate(STA_VM* vm, STA_ActorVisitor visitor, void* ctx);
```

**Snapshot model chosen** (vs. live walk): the `actor_lock` is held only for the
snapshot pass (copy active entry indices to a local stack array), then released
before any visitor call. The visitor is never called under `actor_lock`.

**Rationale for snapshot:** holding `actor_lock` for the duration of the visitor
would block `sta_actor_registry_add` and `sta_actor_registry_remove` (called at
actor spawn/exit) for the full duration of the walk. Under a slow visitor, this
stalls the scheduler's spawn path for an unbounded time.

**Fields per actor** (from `STA_ActorInfo`):
- `actor_handle`: freshly acquired handle (refcount 1); visitor must release
- `supervisor_handle`: same, or NULL for top-level actors; visitor must release
- `mailbox_depth`: `memory_order_acquire` read of the mailbox counter
- `sched_flags`: `memory_order_acquire` read of the scheduler flags field

No raw internal pointers are exposed. All external identity is via handles.

TSan gate: tests 9 and 10 (10 actors + concurrent spawn) are TSan-clean. Visitor
called exactly 10 times in test 9.

---

## Event / notification model

**Push model chosen** (vs. pull/polling):

| | Push | Pull |
|---|---|---|
| Latency | Event delivered immediately | Bounded by poll interval |
| IDE complexity | Dispatch in `deinit`/`DispatchQueue.main.async` | Queue drain + overflow policy |
| Fit with Swift | Natural (Combine, NotificationCenter) | Alien pattern |

```c
int  sta_event_register  (STA_VM*, STA_EventCallback, void* ctx);
void sta_event_unregister(STA_VM*, STA_EventCallback, void* ctx);
```

Event types: `STA_EVT_ACTOR_CRASH`, `STA_EVT_METHOD_INSTALLED`,
`STA_EVT_IMAGE_SAVE_COMPLETE`, `STA_EVT_UNHANDLED_EXCEPTION`.

`event->actor` is a freshly acquired handle (refcount 1); the callback must
release it. `event->message` is VM-owned, valid only during the callback.

**Dispatch protocol:** `htbl_lock` is acquired to copy the callback list to a
local stack array, then released before any callback is invoked. This prevents
deadlock if a callback re-enters the API (re-entrancy is documented as prohibited
but the dispatch implementation does not assume it).

**Re-entrancy rule:** callbacks must not call `sta_event_register` or
`sta_event_unregister`. The order of callbacks in the table is unspecified after
an unregister call (the implementation compacts by swapping the last entry).

TSan gate: tests 11, 12, 13 (crash event delivery, concurrent dispatch, unregister)
are TSan-clean.

---

## Rationale

### Q1 — Why explicit reference counting rather than scoped handles or automatic rooting?

Scoped/frame-based handles require the Swift caller to push/pop handle frames —
unergonomic in Swift and incompatible with Swift ARC's ownership model. Automatic
rooting (all handles are roots forever) creates unbounded table growth.

Explicit retain/release maps directly to Swift's `deinit`: the Swift IDE wraps
each handle in a class whose `deinit` calls `sta_handle_release`. This is the
established pattern for C-FFI lifetime management in Swift (see `CFTypeRef`).
ADR 006's recommended direction ("JNI / CPython model") is adopted unchanged.

### Q2 — Why not expose raw `STA_OOP` across the FFI boundary?

Raw OOPs are movable. A compacting GC or actor migration invalidates them.
`STA_Handle*` is a stable pointer into the handle table; the OOP inside the entry
is updated in place on GC move. Any caller holding the `STA_Handle*` across a GC
cycle sees the correct OOP without requiring any caller-side action. §4.3 of the
architecture document identifies this as the correct model.

### Q3 — Why three separate locks rather than one bridge lock?

A single bridge lock would serialise all bridge operations — handle alloc, method
install, actor enumeration, and event dispatch — behind one mutex. Under IDE load
(system browser installing methods while the actor monitor enumerates) this creates
unnecessary contention. Three narrow locks allow these operations to proceed
concurrently. None of the three locks is the scheduler lock; the scheduler is
unaffected by any IDE operation.

### Q4 — Why separate `STA_ClassDefLog` instead of multiplexing into `STA_MethodLog`?

Using a sentinel selector `"<class_define>"` in the method log bakes an
implementation accident into test assertions. The spike's class define log is
replaced by a real class table mutation in Phase 1; that replacement must not
require rewriting method log tests. Structural separation is correct even for
throwaway spike code.

### Q5 — Why is the push event model preferred over pull?

See the comparison table above. The immediate practical reason: the IDE's
Actor Monitor must display crash events promptly. A timer-based poll adds
the poll interval (typically 100–500 ms for UI) to every crash event's
delivery latency. The push model delivers in the same scheduling quantum as
the crash itself.

---

## Consequences

- **ADR 006 (Handle lifecycle — DEFERRED) is closed.** The full
  acquire/retain/release protocol is now specified and validated.

- **`vm.h` now has 10 new functions and 5 new types.** Each is an obligation
  for all present and future embedders. No function may be removed without a
  deprecation plan.

- **`sta_inspect_cstring` is a known Phase 3 breaking change.** The current
  single-buffer signature will change to a caller-provided buffer signature
  in Phase 3 when the Swift FFI wrapper is built. This is acceptable; the
  Swift wrapper will be written once, against the Phase 3 signature.

- **The handle table capacity (1,024 entries) must grow before Phase 3.** The
  IDE Inspector will hold handles to many objects simultaneously. Phase 3 must
  replace the fixed array with a growable slab structure (stable entry addresses
  required). See Open Questions.

- **`STA_Actor` is unchanged.** The 4-byte density headroom from ADR 012 is
  preserved. Any future `STA_Actor` field addition still requires a new ADR.

- **Method install and class define are stub implementations.** The threading
  model is validated; the compiler integration is Phase 1. The stub logs are
  discarded when Phase 1 replaces them with real class table mutations.

---

## Open questions (deferred)

1. **Growable handle table (Phase 3 blocker).** The spike uses a fixed-capacity
   array of 1,024 entries. The IDE Inspector may hold handles to many more
   objects (open inspector windows, debugger frames, actor monitor entries).
   Phase 3 must replace with a growable structure whose entries have stable
   addresses. A slab allocator (fixed-size slabs, never freed) is the natural
   choice — growth allocates a new slab; old slabs are never freed; all
   `STA_Handle*` pointers remain valid.

2. **Handle validity after `sta_vm_destroy`.** The spike sets all handle entries
   to refcount 0 and OOP 0 at destroy time. The contract for what happens if the
   Swift caller holds a `STA_Handle*` past `sta_vm_destroy` needs to be specified
   before the Swift FFI wrapper is written (undefined behaviour? a sentinel OOP
   that traps on dereference?). Must be decided before Phase 3 Swift wrapper.

3. **Bulk handle acquire and release (Phase 3).** The IDE Inspector may need to
   acquire handles to all slots of a large Array in one call. A bulk interface
   `sta_handle_acquire_slots(vm, array_handle, buf, count)` would be more
   efficient than N individual calls. Deferred to Phase 3 when Inspector is built.

4. **`sta_inspect_cstring` caller-provided buffer (Phase 3 breaking change).**
   New signature: `int sta_inspect_cstring(STA_VM*, STA_Handle*, char* buf,
   size_t len)`. This is a source-breaking change to `vm.h`. The Swift FFI
   wrapper is built in Phase 3 and will be written against the new signature
   from the start. Must be done before Phase 3 Swift wrapper is written.

5. **Event callback re-entrancy rules.** Callbacks must not call
   `sta_event_register` or `sta_event_unregister`. If future use cases require
   dynamic callback registration from within a callback, the dispatch
   implementation must be revised. Must be specified before Phase 3 Swift
   FFI wrapper.

6. **Actor supervision tree traversal (Phase 3).** `STA_ActorInfo` exposes one
   level of supervisor linkage. The Actor Monitor needs the full tree. Evaluate
   `sta_actor_children(vm, actor_handle, visitor, ctx)` vs. recursive use of
   `sta_actor_enumerate`. Deferred to Phase 3 when Actor Monitor is built.

7. **Event filtering and subscriptions (Phase 3).** The current push model
   delivers all events to all callbacks. A type-filtered registration API
   (`sta_event_register_for_type(vm, type, callback, ctx)`) reduces overhead
   under high event rates. Deferred to Phase 3.

8. **`STA_Actor*` in `STA_ActorInfo` vs. stable numeric ID.** The current
   enumeration allocates two handles per actor. An alternative: expose a stable
   `uint64_t actor_id` as the identity and acquire a handle on demand via
   `sta_actor_from_id(vm, id)`. Evaluate in Phase 3 when Actor Monitor is built.
