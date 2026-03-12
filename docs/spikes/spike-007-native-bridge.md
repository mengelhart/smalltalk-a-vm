# Spike 007: Native Bridge (C Runtime ↔ SwiftUI IDE)

**Phase 0 spike — handle model, public API gaps, thread safety, and event model**
**Status:** Ready to execute
**Related ADRs:** 002 (public API boundary), 005 (error codes), 006 (handle lifecycle — DEFERRED to this spike)
**Produces:** ADR 013 (native bridge)

---

## Purpose

This spike answers the foundational questions of the bridge between the C runtime
and the SwiftUI IDE. The architecture document (§4.3) defers the FFI handle
lifetime decision to a Phase 0 spike, and ADR 006 explicitly marks the handle
lifecycle as "DEFERRED to Phase 0 spike." This spike resolves that deferral.

Beyond handles, six questions must be answered before any IDE code is written
against the public API. The current `vm.h` has an incomplete handle model (no
creation path), missing well-known root accessors, no live-update path, no actor
enumeration, and no event-callback mechanism. Each gap must be identified,
designed, stubbed, and tested before ADR 013 is written.

**This spike does not build a Smalltalk interpreter, a Swift wrapper, or a live
class compiler.** It builds the minimal C needed to answer the architecture
questions above, with ctest-passing test coverage. Permanent implementation
follows ADR 013. No Swift code appears in this spike — pure C only.

---

## Background

### §4.2 — The public C API

> "A single public header (`sta/vm.h`) is the only interface consumers need. All
> internal implementation headers are private and not part of any public contract."
>
> "The Swift IDE uses exactly this public API. No privileged back-channel access.
> If the IDE needs something the public API cannot express, the API is extended —
> for everyone."

The "own IDE must use the public API" rule means every gap found during this spike
is a real gap that real IDE users of the API would encounter. Closing gaps in this
spike closes them for all present and future embedders equally.

### §4.3 — FFI object handles and lifetime

> "Raw `STA_OOP` values returned across the FFI boundary are movable pointers — a
> compacting GC or actor migration can invalidate them. The embedding API must
> define explicit lifetime semantics before any host code stores returned OOP values."
>
> "Recommended direction: prefer opaque host-visible handles (`STA_Handle*`) over
> raw `STA_OOP` values for any API function whose return value may be stored beyond
> the current call. The runtime registers these handles as GC roots. The host
> explicitly releases them. This is the same model used by the JNI (`jobject`) and
> CPython (`PyObject*` with reference counting)."

The current `vm.h` already adopts `STA_Handle*` as the external reference type.
The spike must validate this choice under simulated GC move conditions and define
the full acquire/retain/release protocol (ADR 006 open question).

### §8.3 — Runtime model and actor density

> "Per-actor heap target: actor creation cost and baseline heap size should be in
> the same order of magnitude as BEAM (~300 bytes). This is an explicit design
> constraint, not a nice-to-have."

The bridge must not add any fields to `STA_Actor` without a new ADR. See the
density section below.

### §8.4–8.5 — Shared immutable regions and shared coordinated mutable metadata

The well-known roots (nil, true, false, symbols) are shared immutables. The class
table and method dictionaries are shared coordinated mutable metadata. The bridge's
handle model and live-update path must respect these categories:

- Handles to shared immutables are safe to return to any caller at any time.
- Method install and class define mutate shared coordinated metadata and must acquire
  the appropriate internal lock — but never the scheduler lock (that would be a GIL).

### §14 / §14.1 / §14.2 — Tooling model and live update

> "Where possible, IDE operations should also exist as message-driven services."

> "Installing a new method atomically updates the class's method dictionary entry.
> Existing activations (stack frames) continue executing using the old compiled
> method object — they hold a direct reference to it. Future sends (after the
> atomic update) use the new method object. This is safe and requires no
> coordination with running actors."

The live update path is the highest-stakes part of the bridge: it modifies shared
metadata while the scheduler is running. The spike validates that the stub path is
TSan-clean before any compiler integration is attempted.

---

## Actor density constraint entering this spike

The ADR 012 baseline is the tightest the budget has been:

| Component | Bytes |
|---|---|
| `STA_ActorSnap` (ADR 012 baseline) | 152 |
| Initial nursery slab | 128 |
| Actor identity object (0-slot header) | 16 |
| **Total creation cost entering Spike 007** | **296** |
| Target | ~300 |
| **Headroom** | **4 bytes** |

ADR 012 explicitly states: "No field may be added to `STA_Actor` without a new ADR.
Any addition that consumes the remaining 4 bytes — or requires a pad reduction to
fit — breaches the 300-byte target and must be explicitly justified per CLAUDE.md."

**The bridge spike must not require any new `STA_Actor` fields.** The handle
table, the method-install log, the event callback registry, and the actor snapshot
list for enumeration are all VM-level data structures external to `STA_Actor`.
This is the correct design: the bridge is a service the VM provides, not state
embedded in each actor. ADR 013 must confirm that the 4-byte headroom is preserved
and that no STA_Actor field was added by this spike.

---

## Current `vm.h` state — gap analysis

The current `include/sta/vm.h` provides:

| Group | Functions present |
|---|---|
| VM lifecycle | `sta_vm_create`, `sta_vm_destroy`, `sta_vm_load_image`, `sta_vm_save_image`, `sta_vm_last_error` |
| Actor interface | `sta_actor_spawn`, `sta_actor_send` |
| Handle lifecycle | `sta_handle_retain`, `sta_handle_release` |
| Evaluation and inspection | `sta_eval`, `sta_inspect` |

**Identified gaps, by IDE scenario:**

| IDE scenario | Missing function | Category |
|---|---|---|
| Any IDE startup: obtain handle to nil | `sta_vm_nil(vm)` | Well-known roots |
| Conditional display: obtain true/false | `sta_vm_true(vm)`, `sta_vm_false(vm)` | Well-known roots |
| Class browser: look up class by name | `sta_vm_lookup_class(vm, name)` | Well-known roots |
| Inspector: display object as C string | `sta_inspect_cstring(vm, handle)` | Inspection |
| System browser: install a method | `sta_method_install(vm, class_h, sel, src)` | Live update |
| System browser: define a new class | `sta_class_define(vm, source)` | Live update |
| Actor monitor: walk actor tree | `sta_actor_enumerate(vm, visitor_fn, ctx)` | Actor inspection |
| VM events: register event handler | `sta_event_register(vm, callback, ctx)` | Event model |
| VM events: unregister event handler | `sta_event_unregister(vm, callback, ctx)` | Event model |

Additionally, `sta_handle_retain` takes an existing `STA_Handle*` — it has no
creation path. The IDE cannot obtain its first handle without first calling
`sta_eval()`, which requires a functional interpreter (Phase 1). The well-known
root accessors above close this bootstrapping gap.

The current `sta_inspect` returns `STA_Handle*` to a String-like object. The IDE
almost always needs a displayable C string for layout, not a handle to a String
object that it must then re-inspect. Both functions must exist: `sta_inspect` for
object-to-object inspection chains, `sta_inspect_cstring` for terminal display.

Every gap above must have: a proposed function signature in this document, a
rationale, a stub implementation in `src/bridge/bridge_spike.c`, and a test in
`tests/test_bridge_spike.c`. No function is added to `vm.h` that is not required
by one of the above IDE scenarios — the public API surface is an obligation, not
a scratchpad.

---

## Questions to answer

Each question requires a concrete answer — not a deferral — before ADR 013 is
written. The spike test binary must demonstrate the answer.

---

### Q1: Handle model — lifetime, GC safety, and bootstrapping

#### The bootstrapping problem

`sta_handle_retain` takes an existing `STA_Handle*`. There is no way to obtain the
first handle without going through `sta_eval()`. The IDE cannot call `sta_eval()`
before a functional interpreter exists (Phase 1). The well-known root accessors
solve this: they return handles to objects whose addresses are known to the VM at
construction time, before any interpreter is required.

**Proposed signatures:**

```c
/* Well-known root accessors — return a freshly acquired handle (refcount = 1).
 * Caller must release via sta_handle_release. Never return NULL on a valid VM. */
STA_Handle* sta_vm_nil(STA_VM* vm);
STA_Handle* sta_vm_true(STA_VM* vm);
STA_Handle* sta_vm_false(STA_VM* vm);

/* Class lookup by name. Returns NULL (and sets sta_vm_last_error) if not found.
 * Returns a freshly acquired handle (refcount = 1) if found. */
STA_Handle* sta_vm_lookup_class(STA_VM* vm, const char* name);
```

The stub implementation allocates a sentinel `STA_ObjHeader` for each well-known
root (nil, true, false) at VM-create time and registers them in the handle table.
Each accessor acquires the IDE-API lock, increments the target entry's refcount,
and returns the handle pointer.

#### Reference counting model

**Decision rationale:** explicit retain/release with reference counting, matching
the recommendation in ADR 006 (JNI / CPython model). Alternatives considered:

- **Scoped handles (arena/frame-based):** the Swift caller would need to push/pop
  handle frames, which is unergonomic in Swift and requires careful scoping
  discipline. The JNI model is already well-understood in Swift via CFTypeRef.
- **Automatic (all handles are GC roots forever):** creates unbounded handle table
  growth; the IDE must still signal when it is done with a handle.
- **Reference counting:** explicit, predictable lifetime, well-matched to Swift's
  ARC — `sta_handle_retain` maps to `+1`, `sta_handle_release` maps to `-1`, and
  Swift can wrap these in a class with `deinit` for automatic management.

**Acquire/retain/release rules (to be locked in ADR 013):**

1. Any function that returns `STA_Handle*` returns an acquired handle (refcount 1
   for a new handle, or incremented for an existing one). The caller owns one
   reference and must eventually call `sta_handle_release`.
2. `sta_handle_retain` increments the refcount and returns the same handle pointer.
   It is idempotent with respect to the object being rooted.
3. `sta_handle_release` decrements the refcount. When refcount reaches zero, the
   handle table entry is freed and the OOP is no longer rooted.
4. After `sta_handle_release`, the `STA_Handle*` pointer is dangling — do not use it.
5. All handles are invalidated by `sta_vm_destroy`. Using a handle after
   `sta_vm_destroy` is undefined behaviour.
6. Handle leaks are real memory leaks and real GC root leaks (the GC cannot collect
   a handle-rooted object). The Swift IDE should wrap handles in a class with
   `deinit` calling `sta_handle_release`.

#### Thread affinity

Handles are not thread-affine. Any handle may be passed to any public API function
from any OS thread. The IDE-API lock (see Q3) protects handle table access. The
caller is responsible for not using the same handle from two threads simultaneously
in a way that would violate its own higher-level invariants — but the VM itself does
not enforce this beyond the lock.

#### GC move invariant

When the GC moves an object (compacting nursery or old-space), it must update every
handle table entry that roots the moved OOP. The handle table is a GC root set. In
the permanent implementation, the GC scans the handle table as part of its root
enumeration pass and rewrites any stale OOP with the new forwarded address. The
`STA_Handle*` pointer itself (the address of the handle table entry) does not change.
The `STA_OOP` stored inside the entry is updated in place. Any caller holding the
`STA_Handle*` across a GC cycle sees the updated OOP transparently.

**Spike handle table design:**

```c
/* In bridge_spike.h */
#define STA_HANDLE_TABLE_CAPACITY 1024u

typedef struct {
    STA_OOP  oop;       /* rooted OOP — updated in place by GC move */
    uint32_t refcount;  /* reference count; 0 = free slot */
    uint32_t _pad;      /* maintain 16-byte entry size */
} STA_HandleEntry;     /* 16 bytes */

typedef struct {
    STA_HandleEntry entries[STA_HANDLE_TABLE_CAPACITY];
    pthread_mutex_t lock;       /* IDE-API lock — NEVER held by scheduler threads */
    uint32_t        free_count; /* number of free entries */
} STA_HandleTable;
```

The `STA_Handle*` returned to callers is a pointer into `entries[]`. Because the
table is a fixed-size array (in the spike), table addresses are stable — the table
does not realloc. The spike capacity of 1,024 entries is adequate for Phase 0
testing. Phase 1 will need a growable table with pointer-stable entries (e.g.,
a free-list of fixed-size slabs).

**Validation test (from the required outputs):**

1. Create a VM. Obtain `h = sta_vm_nil(vm)`. Verify `h != NULL`.
2. `sta_handle_retain(vm, h)` → refcount is now 2.
3. Simulate a GC move: directly update `h->entry->oop` to a new sentinel address
   (simulating the GC fixup). This is spike code and touches `src/bridge/` internals.
   Tests that reach into `src/` must be flagged (see Constraints).
4. Read the OOP back through the handle. Verify it reflects the new address.
5. `sta_handle_release(vm, h)` → refcount falls to 1. Entry still occupied.
6. `sta_handle_release(vm, h)` → refcount falls to 0. Entry is free.
7. Verify the handle table has no occupied entries.

---

### Q2: Gaps in the current `vm.h`

For each gap: signature, rationale, stub, test.

#### Well-known root accessors (see Q1)

Already specified in Q1. Signatures:

```c
STA_Handle* sta_vm_nil(STA_VM* vm);
STA_Handle* sta_vm_true(STA_VM* vm);
STA_Handle* sta_vm_false(STA_VM* vm);
STA_Handle* sta_vm_lookup_class(STA_VM* vm, const char* name);
```

**Test:** obtain each well-known root, verify handle is non-null, verify releasing
it leaves the table clean. For `sta_vm_lookup_class`, test both a registered class
name (returns a handle) and an unknown name (returns NULL, sets last error).

#### `sta_inspect_cstring` — inspection as a C string

```c
/* Returns a human-readable representation of the object as a null-terminated
 * C string. The returned pointer is valid until the next call to
 * sta_inspect_cstring on the same VM, or until sta_vm_destroy.
 * Returns NULL on error (sets sta_vm_last_error).
 *
 * *** NOT THREAD-SAFE. ***
 * This function is single-caller-at-a-time by CONTRACT, not just by convention.
 * The VM owns the output buffer; a second call from any thread overwrites it.
 * The IDE must call this function from one thread only (the main thread) and
 * must not hold the returned pointer across any suspension point.
 * The Phase 3 fix is a caller-provided buffer (see Open Questions). ADR 013
 * must record this function as explicitly NOT thread-safe rather than allowing
 * it to appear thread-safe by virtue of holding the IDE-API lock during the
 * write. The lock protects the write, not the returned pointer's lifetime. */
const char* sta_inspect_cstring(STA_VM* vm, STA_Handle* handle);
```

**Rationale:** the IDE Inspector, Debugger, and Actor Monitor all need displayable
strings. Returning an `STA_Handle*` to a String object (as `sta_inspect` does)
forces the IDE to call `sta_inspect` again on that String to get a C string — a
recursive inconvenience. `sta_inspect_cstring` breaks the recursion and is the
natural terminal display function. Both functions must exist: `sta_inspect` for
object-to-object inspection chains (e.g., inspect the class of an object), and
`sta_inspect_cstring` for display.

The VM-owned buffer model follows the same convention as `sta_vm_last_error`. It
is **not thread-safe** and must not be treated as such. The IDE will call this
from the main thread only (all SwiftUI display is main-thread), so the single-
threaded contract is met in practice — but the contract must be explicit so that
no future caller (a background Inspector, a script runner) accidentally violates
it and produces a silent data race. The Phase 3 caller-provided buffer is the
correct fix; do not defer the documentation of the hazard along with the fix.

**Stub:** returns a static string describing the stub object type (e.g., `"nil"`,
`"true"`, `"false"`, `"a StubObject"`) based on the OOP stored in the handle.

**Test:** call `sta_inspect_cstring` on nil, true, false handles. Verify the
returned strings are non-null, non-empty, and match the expected representations.

#### `sta_method_install` — live method update

```c
/* Install (or replace) a compiled method in a class's method dictionary.
 * class_handle: a handle to the class object (obtained via sta_vm_lookup_class).
 * selector:     the method name as a null-terminated C string (e.g., "printOn:").
 * source:       the Smalltalk source text as a null-terminated C string.
 * Returns STA_OK on success. Returns STA_ERR_INVALID if class_handle or selector
 * is null. Returns STA_ERR_INTERNAL if the method log is full (spike limit).
 * Thread safety: acquires the method-install lock (not the IDE-API lock, not the
 * scheduler lock). Safe to call from any thread. */
int sta_method_install(STA_VM* vm,
                       STA_Handle* class_handle,
                       const char* selector,
                       const char* source);
```

**Rationale:** §14.2 specifies that method install is atomic and safe with respect
to running activations. The stub records (class_name, selector, source) in a
thread-safe log without touching any actor's frame stack. Full compiler integration
is Phase 1; the log validates the threading model now.

**Stub log design:**

```c
#define STA_METHOD_LOG_CAPACITY 256

typedef struct {
    char class_name[64];
    char selector[64];
    char source[512];
} STA_MethodLogEntry;

typedef struct {
    STA_MethodLogEntry entries[STA_METHOD_LOG_CAPACITY];
    pthread_mutex_t    lock;  /* method-install lock — distinct from IDE-API lock */
    uint32_t           count;
} STA_MethodLog;
```

**Thread-safety requirement:** the method-install lock must never be acquired while
holding the IDE-API lock or the scheduler lock. Acquiring both simultaneously would
create a lock-order dependency that could deadlock. The spike must document the lock
order explicitly and verify it structurally.

**Test:** call `sta_method_install` from a background thread while stub scheduler
threads are running. Verify the log entry is correct (class_name, selector, source
match the inputs). TSan-clean.

#### `sta_class_define` — new class definition

```c
/* Define a new class from Smalltalk source.
 * source: the full class definition as a null-terminated C string.
 *         e.g., "Object subclass: #MyClass instanceVariableNames: 'x y' ..."
 * Returns STA_OK on success. Returns STA_ERR_INVALID if source is null.
 * Thread safety: acquires the method-install lock. Safe from any thread. */
int sta_class_define(STA_VM* vm, const char* source);
```

**Rationale:** §14.2 states "New classes may be installed at any time with no
coordination required." Full compiler integration is Phase 1.

**Stub design — separate log, not reuse of `STA_MethodLog`:**

The stub records the source string in a dedicated `STA_ClassDefineLog` (a simple
fixed-array of `char source[512]` entries, protected by the method-install lock).
It does **not** multiplex class definitions into `STA_MethodLog` using a sentinel
selector such as `"<class_define>"`. That approach would bake an implementation
accident — a method-shaped record for a non-method operation — into the test
assertions, and the tests would need rewriting when Phase 1 introduces a real
class table. The `sta_class_define` stub is the most throwaway part of this spike
(its entire job is to validate that the threading model is TSan-clean); the log
struct should reflect what the function actually is, not borrow a container that
doesn't fit.

```c
#define STA_CLASS_DEF_LOG_CAPACITY 64

typedef struct {
    char source[512];
} STA_ClassDefEntry;

typedef struct {
    STA_ClassDefEntry entries[STA_CLASS_DEF_LOG_CAPACITY];
    pthread_mutex_t   lock;  /* reuses the method-install lock in practice */
    uint32_t          count;
} STA_ClassDefLog;
```

The method-install lock may be shared between `STA_MethodLog` and `STA_ClassDefLog`
(one mutex, two log arrays) because they are independent append operations with no
ordering dependency between them. Sharing the lock is simpler than introducing a
third lock; document this in `bridge_spike.h`.

**Test:** call `sta_class_define` from a background thread. Assert `STA_OK`. Assert
`sta_class_def_log_count(vm) == 1`. Assert the log entry source matches the input.
Do not assert anything about `STA_MethodLog` — class defines are not method installs
and must not appear there. TSan-clean.

#### `sta_actor_enumerate` — actor monitor support

```c
/* Per-actor information snapshot delivered to the visitor.
 * actor_handle and supervisor_handle are freshly acquired handles (refcount = 1).
 * The visitor must release them or they will leak. supervisor_handle is NULL if
 * the actor has no supervisor (top-level actor). */
typedef struct {
    STA_Handle* actor_handle;      /* opaque handle to the actor object */
    STA_Handle* supervisor_handle; /* opaque handle to the supervisor, or NULL */
    uint32_t    mailbox_depth;     /* current mailbox queue depth */
    uint32_t    sched_flags;       /* current scheduler flags (STA_SCHED_* constants) */
} STA_ActorInfo;

typedef void (*STA_ActorVisitor)(const STA_ActorInfo* info, void* ctx);

/* Enumerate all live actors. Takes a consistent snapshot of the actor registry
 * under the actor-registry lock, then releases the lock before invoking the
 * visitor for each actor. The visitor is called from the calling thread (never
 * from a scheduler thread). The caller must not modify the VM state from inside
 * the visitor (no sta_actor_spawn, no sta_method_install). Returns the number of
 * actors visited, or a negative STA_ERR_* code on failure. */
int sta_actor_enumerate(STA_VM* vm, STA_ActorVisitor visitor, void* ctx);
```

**Rationale:** the Actor Monitor (§14) needs to walk the supervision tree. The
snapshot model is preferred over a live walk: the actor registry lock is held only
long enough to copy actor pointers into a local array, then released before the
visitor is invoked. This prevents the visitor from holding the registry lock for an
unbounded duration, and decouples enumeration latency from visitor complexity.

`STA_ActorInfo` exposes only information needed by the Actor Monitor UI: identity
(handle), supervisor linkage (handle), mailbox depth, and scheduler state. Raw
internal pointers are not exposed — handles are used for actor identity, consistent
with the public API boundary rule.

**Spike actor registry design:**

```c
/* In bridge_spike.c — spike-internal */
#define STA_ACTOR_REGISTRY_CAPACITY 4096

typedef struct {
    STA_Actor*      actors[STA_ACTOR_REGISTRY_CAPACITY];
    pthread_mutex_t lock;
    uint32_t        count;
} STA_ActorRegistry;
```

Actors are registered at spawn and unregistered at exit. The registry is separate
from the scheduler's run queues. The registry lock is neither the IDE-API lock nor
the scheduler lock.

**Test:** spawn 10 stub actors, call `sta_actor_enumerate` from a background thread
while stub scheduler threads are running. Assert visitor is called exactly 10 times.
Assert each `STA_ActorInfo` has non-null `actor_handle`. Assert all handles are
released by the test. TSan-clean.

#### `sta_event_register` and `sta_event_unregister` — event model (see Q6)

Specified in Q6 below.

---

### Q3: Threading model — IDE thread safety

#### Design decision

**All public API functions are safe to call from any OS thread.** This is the
preferred model (§4.2) because the IDE should not need to coordinate with VM thread
boundaries — Swift/SwiftUI already manages its own thread model, and requiring the
IDE to call certain functions only from certain threads creates a subtle, untestable
contract.

#### Coordination mechanism

The bridge uses two locks, distinct from the scheduler locks:

1. **IDE-API lock** (`pthread_mutex_t` in `STA_HandleTable.lock`): protects the
   handle table. Held during handle acquire, retain, release, and GC move fixup.
   Never held by scheduler threads. Never held simultaneously with the method-install
   lock (acquire IDE-API lock first if both are needed — but this should not occur;
   handle operations and method install are independent paths).

2. **Method-install lock** (`pthread_mutex_t` in `STA_MethodLog.lock`): protects
   the method-install log and class-define log. Held only during log append.
   Never held by scheduler threads. Never held simultaneously with the IDE-API lock.

3. **Actor-registry lock** (`pthread_mutex_t` in `STA_ActorRegistry.lock`): protects
   the actor registry. Held during actor registration (spawn), deregistration (exit),
   and the snapshot phase of `sta_actor_enumerate`. Released before the visitor is
   called. Never held by scheduler threads beyond actor spawn/exit — scheduler threads
   do not enumerate.

**Invariant: the scheduler must never hold any of these three locks.**

This is not a GIL. A GIL is a single lock that serialises all VM execution. These
three locks each protect a narrow, bounded critical section (table entry, log append,
registry snapshot). The scheduler runs without holding any of them. An IDE call may
block briefly on the lock; a scheduler thread never does.

**Lock-order rule:** if multiple locks must be held simultaneously (which should be
avoided), the order is: actor-registry lock → IDE-API lock → method-install lock.
No code path acquires these in reverse order.

#### TSan validation

The test binary includes a dedicated threading test:

- Two threads: IDE thread (calls all public API functions in a tight loop) and
  scheduler thread (runs the ADR 009 scheduler spike loop, pushing and popping actors).
- The IDE thread calls: `sta_vm_nil`, `sta_vm_true`, `sta_vm_false`,
  `sta_vm_lookup_class`, `sta_inspect_cstring`, `sta_handle_retain`,
  `sta_handle_release`, `sta_method_install`, `sta_class_define`,
  `sta_actor_enumerate`, `sta_event_register`.
- The scheduler thread calls `sta_sched_push` and `sta_sched_steal` as in the ADR 009
  spike.
- TSan must report zero data races across the entire test.
- Run with `-fsanitize=thread`. This is the gating correctness test — it must pass
  before ADR 013 is written.

---

### Q4: Live update path — method install and class define

#### Stub protocol

For this spike, "method install" is defined as: atomically append a
`STA_MethodLogEntry` to the `STA_MethodLog` under the method-install lock. No
bytecode compilation occurs. No class method dictionary is modified (there is no
class method dictionary in the spike). The log records (class_name, selector, source)
as a proof-of-concept for the threading model.

#### Atomicity and isolation

The log append is the entire critical section: lock → copy strings → unlock. The
strings are copied into the log entry (bounded-size fields); the lock is held for
the duration of the copy. This is the model for the permanent implementation:
method install will be a small critical section on the class table's method
dictionary entry (a pointer-width atomic compare-and-swap), not a long transaction.

**Existing activations are unaffected** (§14.2): in the permanent implementation,
existing stack frames hold a direct reference to the old compiled method object.
The method install atomically updates the class method dictionary pointer; the old
pointer is not freed until no frame references it (GC handles this). In the spike,
there are no stack frames, so this is not validated directly. The spike validates
the threading model only; the frame-safety invariant is validated in the interpreter
spike (Phase 1).

**Future sends use the new method** (§14.2): in the permanent implementation, a
send is a method dictionary lookup followed by dispatch to the found method object.
After atomic update, any future send goes through the new pointer. In the spike,
log entries are a stand-in for the atomic pointer update; the log count can be
verified to be exactly 1 after one install call, confirming atomicity.

#### Validation test

1. Start 4 stub scheduler threads (to simulate scheduler activity).
2. From the main thread (simulating IDE), call `sta_method_install(vm, class_h, "foo", "foo ^42")`.
3. Join scheduler threads.
4. Assert `sta_method_log_count(vm) == 1`.
5. Assert the log entry has class_name, selector "foo", and source "foo ^42".
6. TSan-clean.

---

### Q5: Actor enumeration for the Actor Monitor

#### Snapshot vs. live-walk decision

**Snapshot model chosen.** The actor registry lock is held only for the snapshot
pass (copy actor pointers into a local stack array). The lock is released before
any visitor call. The visitor sees a consistent view of the actor population at a
single point in time but is not constrained by the registry lock. New actors spawned
after the snapshot begins are not included in the current enumeration; this is
acceptable for a monitoring tool (the next refresh will include them).

**Live-walk rejected.** Holding the registry lock for the duration of visitor
invocation would block actor spawn and exit for the full duration of the walk.
Under a slow or I/O-blocked visitor (e.g., one that calls `sta_inspect_cstring`
on each actor), this would starve the scheduler of spawn/exit operations for an
unbounded time. The snapshot model bounds the lock hold time to one array copy.

#### Actor information per visitor call

`STA_ActorInfo` (specified in Q2) exposes:
- `actor_handle`: the actor's identity as a handle. The visitor can pass this to
  `sta_inspect_cstring` to get a display string. Must be released by visitor.
- `supervisor_handle`: the actor's supervisor, or NULL. Must be released if non-null.
- `mailbox_depth`: snapshot of the current mailbox queue depth (the MPSC counter
  from ADR 008, read with `memory_order_acquire`).
- `sched_flags`: snapshot of the current scheduler flags (the atomic `sched_flags`
  field from ADR 009/011, read with `memory_order_acquire`).

Internal pointers (actor struct address, nursery base, stack frame pointer) are
not exposed. All external references use handles.

#### Snapshot protocol (detailed)

```
sta_actor_enumerate(vm, visitor, ctx):
  1. Acquire actor-registry lock.
  2. Copy registry->actors[0..count-1] into a local array (stack-allocated).
     Copy count into local_count.
  3. Release actor-registry lock.
  4. For each actor in the local array:
     a. Acquire IDE-API lock.
     b. Create actor_handle (new handle table entry pointing to actor's identity OOP).
     c. Create supervisor_handle (same, for supervisor, or NULL).
     d. Release IDE-API lock.
     e. Read mailbox_depth (atomic acquire read of mailbox counter).
     f. Read sched_flags (atomic acquire read).
     g. Populate STA_ActorInfo.
     h. Call visitor(info, ctx).
     i. [Visitor is responsible for releasing handles.]
  5. Return local_count.
```

#### Validation test

1. Start 2 stub scheduler threads.
2. Register 10 stub actors in the actor registry.
3. From a third background thread: call `sta_actor_enumerate`. Count visitor calls.
4. Assert visitor called exactly 10 times.
5. Assert each `actor_handle` is non-null.
6. Within visitor: call `sta_handle_release` on both handles.
7. After enumeration: verify handle table has no outstanding entries from enumeration.
8. TSan-clean.

---

### Q6: Notification and event model

#### Design decision: push model

**The push model is chosen.** The VM calls a registered C callback from whichever
thread the event occurs on; the Swift caller's handler dispatches to the main thread
(using `DispatchQueue.main.async` or equivalent). The pull model (IDE polls a
lock-free queue on a timer) is rejected:

- **Pull latency:** a timer-based poll adds the poll interval to every event's
  delivery latency. The IDE's Actor Monitor needs to display crash events promptly.
- **Queue management:** the IDE must manage queue drain, overflow policy, and
  acknowledgement — complexity for no benefit over push.
- **Push is the norm:** libuv callbacks, NSNotification, Darwin dispatch sources,
  and the Combine framework all use push semantics. The IDE is already set up for
  push; a pull queue is an alien pattern.

The callback is called from the VM's internal thread (whichever thread the event
occurred on). It must be short and non-blocking. The Swift handler must not call
back into the VM from within the callback (no re-entrancy) — document this
explicitly.

#### Event types and struct

```c
/* Event types delivered to the registered callback. */
typedef enum {
    STA_EVT_ACTOR_CRASH          = 1, /* an actor terminated with an unhandled error */
    STA_EVT_METHOD_INSTALLED     = 2, /* a method was installed (confirmation) */
    STA_EVT_IMAGE_SAVE_COMPLETE  = 3, /* sta_vm_save_image completed */
    STA_EVT_UNHANDLED_EXCEPTION  = 4, /* an exception escaped a top-level actor */
} STA_EventType;

typedef struct {
    STA_EventType type;
    STA_Handle*   actor;        /* the relevant actor, or NULL; caller must release */
    const char*   message;      /* human-readable; owned by VM; valid only during callback */
    uint64_t      timestamp_ns; /* CLOCK_MONOTONIC_RAW nanoseconds */
} STA_Event;

typedef void (*STA_EventCallback)(STA_VM* vm, const STA_Event* event, void* ctx);
```

**Notes:**
- `event->actor` is a freshly acquired handle (refcount 1). The callback is
  responsible for releasing it (or retaining it for later use). If the callback
  ignores the handle, it must still release it.
- `event->message` is VM-owned and valid only for the duration of the callback.
  Callers who need the string beyond the callback must copy it.
- The `STA_Event*` itself is stack-allocated by the VM and is valid only during
  the callback invocation. Do not store the pointer.

#### Callback registration API

```c
/* Register an event callback. Multiple callbacks may be registered; all are called
 * for each event (in registration order). Returns STA_OK, or STA_ERR_OOM if the
 * callback table is full. Thread safety: protected by the IDE-API lock. */
int sta_event_register(STA_VM* vm, STA_EventCallback callback, void* ctx);

/* Unregister a previously registered (callback, ctx) pair. No-op if not found.
 * Thread safety: protected by the IDE-API lock. */
void sta_event_unregister(STA_VM* vm, STA_EventCallback callback, void* ctx);
```

#### Event dispatch (stub internal function)

```c
/* Called by VM internals (spike: called directly by test code).
 * Acquires the IDE-API lock to read the callback list, copies the list,
 * releases the lock, then calls each callback outside the lock.
 * This avoids calling user code while holding the IDE-API lock (which could
 * deadlock if the user callback calls back into the VM). */
void sta_event_dispatch(STA_VM* vm, const STA_Event* event);
```

**Re-entrancy rule:** callbacks must not call `sta_event_register` or
`sta_event_unregister`. Doing so would attempt to acquire the IDE-API lock from
inside a callback that was invoked after the lock was released — this is safe (the
lock is not held during callback dispatch) but is nonetheless a documented
restriction because it creates ordering hazards in the callback list. Document in
ADR 013.

#### Validation test

1. Register a crash callback that sets a `_Atomic int` flag to 1 and stores the
   actor handle in a global.
2. Dispatch a stub `STA_EVT_ACTOR_CRASH` event from a background thread (simulating
   a scheduler thread detecting an actor crash).
3. Assert the flag is 1.
4. Assert the stored handle is non-null.
5. Release the stored handle.
6. Assert handle table is clean.
7. Repeat from a second background thread concurrently (two crash events dispatched
   simultaneously). Assert the callback is called exactly twice (once per event).
8. TSan-clean.

---

## What to build

All files produced by this spike are **spike code** — clearly marked, to be replaced
during Phase 3 with permanent implementations informed by ADR 013. No file produced
here is the permanent implementation.

### File 1: `src/bridge/bridge_spike.h`

Types and interface for the spike bridge. Define:

```
Contents:
- STA_HandleEntry struct     — OOP + refcount + padding (16 bytes)
- STA_HandleTable struct     — entry array + IDE-API lock + free_count
- STA_MethodLogEntry struct  — class_name + selector + source (fixed-size fields)
- STA_MethodLog struct       — entry array + method-install lock + count
- STA_ActorInfo typedef      — as specified in Q2/Q5
- STA_ActorVisitor typedef   — function pointer type
- STA_ActorRegistry struct   — actor pointer array + registry lock + count
- STA_EventType enum         — STA_EVT_* values
- STA_Event struct           — type, actor, message, timestamp_ns
- STA_EventCallback typedef  — function pointer type
- STA_EventCallbackEntry     — callback + ctx (for registration table)
- STA_BridgeState struct     — handle table + method log + actor registry +
                               event callback table + well-known root OOPs
                               (nil_oop, true_oop, false_oop, inspect_buf)
- Function declarations:
    sta_bridge_init(STA_BridgeState *)
    sta_bridge_destroy(STA_BridgeState *)
    sta_handle_alloc(STA_BridgeState *, STA_OOP oop) → STA_Handle*
    sta_handle_retain_internal(STA_BridgeState *, STA_Handle *) → STA_Handle*
    sta_handle_release_internal(STA_BridgeState *, STA_Handle *)
    sta_event_dispatch(STA_VM *, const STA_Event *)
    sta_actor_registry_add(STA_BridgeState *, STA_Actor *)
    sta_actor_registry_remove(STA_BridgeState *, STA_Actor *)
```

**Hard rules for this header:**
- Pure C types only. No `pthread_t` in the public interface structs (keep pthreads
  internal to the `.c` file where possible, but `pthread_mutex_t` in the handle table
  and method log structs is unavoidable and acceptable — it is not a platform-specific
  opaque type in the same sense as `uv_loop_t`).
- No libuv types. No Swift types. No Objective-C types.
- Do not include `<uv.h>`. Include only `<stdint.h>`, `<stdatomic.h>`, `<pthread.h>`.
- Prominent `SPIKE CODE — NOT FOR PRODUCTION` comment at the top.

### File 2: `src/bridge/bridge_spike.c`

Stub implementations of all new `vm.h` functions and internal bridge helpers.
Key requirements:

- Implement `STA_BridgeState` lifecycle: `sta_bridge_init` initialises the handle
  table (all entries zeroed, free_count = CAPACITY), method log, actor registry,
  and event callback table. Creates well-known root OOPs as fixed-address sentinel
  `STA_ObjHeader` instances (static or heap-allocated at init time).

- Implement handle operations: `sta_handle_alloc` acquires the IDE-API lock, finds
  a free entry (refcount == 0), initialises it (oop, refcount = 1), releases the
  lock. `sta_handle_retain_internal` acquires the lock, increments refcount, releases.
  `sta_handle_release_internal` acquires the lock, decrements refcount (assert > 0
  before decrement), if now 0 clears the entry, releases the lock.

- Implement `sta_vm_nil`, `sta_vm_true`, `sta_vm_false` as thin wrappers:
  acquires bridge state from VM, calls `sta_handle_alloc(bridge, vm->bridge.nil_oop)`.

- Implement `sta_vm_lookup_class`: for the spike, maintain a tiny stub class registry
  (a fixed array of `{name, oop}` pairs). If name found, alloc handle. If not found,
  set last error and return NULL.

- Implement `sta_inspect_cstring`: switch on OOP type (nil_oop → "nil",
  true_oop → "true", false_oop → "false", otherwise → "a StubObject (#<id>)").
  Write into a VM-owned `char inspect_buf[256]` field. Return pointer to it.

- Implement `sta_method_install` and `sta_class_define`: acquire method-install lock,
  append to log (assert count < CAPACITY), release lock. Return STA_OK.

- Implement `sta_actor_enumerate`: per the snapshot protocol in Q5.

- Implement `sta_event_register`, `sta_event_unregister`, `sta_event_dispatch`: per Q6.

- All functions must validate their arguments: `vm != NULL`, `handle != NULL`, etc.
  Invalid arguments return `STA_ERR_INVALID` (for int-returning functions) or NULL
  (for handle-returning functions) and set `sta_vm_last_error`.

### File 3: `tests/test_bridge_spike.c`

Correctness tests, handle lifecycle, root accessors, live update, actor enumeration,
event callbacks, and TSan gate. The test binary must print results and exit 0 on
all checks passing.

**Required test cases:**

1. **Bridge lifecycle.** `sta_vm_create`, `sta_bridge_init` (via vm_create), `sta_vm_destroy`.
   No operations. Verify clean startup and shutdown, no TSan reports.

2. **Handle lifecycle — nil root.** Obtain `h = sta_vm_nil(vm)`. Assert non-null.
   `sta_handle_retain` → refcount 2. Simulate GC move (update `h->oop` directly —
   flagged as spike-internal access, see Constraints). Assert OOP reflects new address.
   `sta_handle_release` × 2. Assert handle table is empty. TSan-clean.

3. **Well-known roots.** Obtain nil, true, false handles. Assert all non-null and
   distinct. `sta_inspect_cstring` on each: assert "nil", "true", "false" respectively.
   Release all. Assert handle table clean.

4. **Class lookup — found and not-found.** Register stub class "Array" at bridge_init.
   `sta_vm_lookup_class(vm, "Array")` → non-null handle. Release.
   `sta_vm_lookup_class(vm, "NonExistent")` → NULL. `sta_vm_last_error(vm)` non-null.

5. **`sta_inspect_cstring` single-caller contract.** Call `sta_inspect_cstring` 100
   times from the main thread on nil, true, false handles in rotation. Copy the
   result into a local buffer before the next call. Assert each result is non-null
   and matches the expected representation. This test deliberately uses one thread
   only, enforcing the documented single-caller-at-a-time contract. Do NOT add a
   concurrent version of this test — it would assert that a race condition is safe,
   which it is not. The concurrent fix (caller-provided buffer) belongs in Phase 3.

6. **Method install — single install.** `sta_method_install(vm, class_h, "foo", "foo ^42")`.
   Assert STA_OK. Assert log count == 1. Assert log entry matches inputs.

7. **Method install — concurrent.** 8 background threads each call `sta_method_install`
   100 times with distinct selectors. Join threads. Assert log count == 800. TSan-clean.

8. **Class define.** `sta_class_define(vm, "Object subclass: #Foo ...")`. Assert STA_OK.
   Assert logged. TSan-clean.

9. **Actor enumeration — 10 actors.** Register 10 stub actors. `sta_actor_enumerate`
   from a background thread. Assert visitor called exactly 10 times. Assert all handles
   non-null. Assert all handles released in visitor. Assert handle table clean after.
   TSan-clean.

10. **Actor enumeration — concurrent with spawn.** One thread spawns actors in a loop
    (up to 20 total). Another thread enumerates concurrently. Assert no crash, no TSan
    report. Visitor count ≥ 0 and ≤ 20.

11. **Event callback — crash event.** Register callback. Dispatch `STA_EVT_ACTOR_CRASH`
    from a background thread. Assert callback called once with correct type. Assert
    actor handle non-null. Release handle in callback. TSan-clean.

12. **Event callback — concurrent dispatch.** Register callback. Two threads each
    dispatch a crash event simultaneously. Assert callback called exactly twice.
    TSan-clean.

13. **Event callback — unregister.** Register callback. Unregister it. Dispatch event.
    Assert callback NOT called.

14. **Full threading gate.** Start 4 stub scheduler threads (ADR 009 spike scheduler).
    From the main thread, call every public API function: nil, true, false, lookup_class,
    inspect_cstring, handle_retain, handle_release, method_install, class_define,
    actor_enumerate, event_register, event_unregister. All calls interleaved with
    scheduler threads running. TSan-clean. This is the mandatory gate test.

15. **Actor density checkpoint.** Print:
    - `sizeof(STA_HandleEntry)` — assert == 16
    - `sizeof(STA_HandleTable)` — informational
    - `sizeof(STA_BridgeState)` — informational
    - Confirm `sizeof(STA_ActorSnap)` unchanged from ADR 012 baseline (152 bytes)
    - Confirm total creation cost: 152 + 128 + 16 = 296 (4 bytes headroom preserved)
    - Print: "Bridge spike adds 0 bytes to STA_Actor. Headroom: 4 bytes."

---

## CMake integration

The bridge spike has no external dependencies (no libuv). It uses pthreads, which
are available on macOS without additional CMake configuration.

Add to `tests/CMakeLists.txt`:

```cmake
# Spike 007 — native bridge
add_executable(test_bridge_spike
    test_bridge_spike.c
    ../src/bridge/bridge_spike.c)
target_include_directories(test_bridge_spike PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_bridge_spike PRIVATE
    -fsanitize=thread
    -g)
target_link_options(test_bridge_spike PRIVATE -fsanitize=thread)
target_link_libraries(test_bridge_spike PRIVATE pthread)
add_test(NAME bridge_spike COMMAND test_bridge_spike)
```

Note: the test includes `${CMAKE_SOURCE_DIR}/src` in its include path to access
`src/bridge/bridge_spike.h`. This is intentional for spike code — the test is
validating the internal bridge, not the public API in isolation. Any test that
reaches into `src/` must carry a comment: `/* SPIKE-INTERNAL: direct src/ access */`.
Production tests must not include `src/` headers.

Build and run:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure -R bridge_spike
```

---

## Constraints

- All spike files must carry a prominent comment at the top:
  `/* SPIKE CODE — NOT FOR PRODUCTION */`
- No Swift code in this spike — pure C only.
- No libuv types in any header.
- `include/sta/vm.h` additions must follow the existing style: grouped by concern,
  one blank line between groups, a dashed-line comment header for each group,
  pure C types only, no platform-specific types. Additions are:
  - New group: "Well-known roots" (`sta_vm_nil`, `sta_vm_true`, `sta_vm_false`,
    `sta_vm_lookup_class`)
  - New group: "Event callbacks" (type definitions + `sta_event_register`,
    `sta_event_unregister`)
  - New group: "Live update" (`sta_method_install`, `sta_class_define`)
  - Existing "Actor interface" group: add `STA_ActorInfo` typedef, `STA_ActorVisitor`
    typedef, `sta_actor_enumerate`
  - Existing "Evaluation and inspection" group: add `sta_inspect_cstring`
- Do not modify any existing ADR (001–012).
- The IDE uses only `sta/vm.h`. If a test needs to reach into `src/bridge/` it is
  simulating a violation and must be flagged with `/* SPIKE-INTERNAL */`.
- `STA_Actor` must not grow. Any new bridge state lives in the VM struct, not in
  the actor struct. ADR 013 must confirm the 4-byte headroom is preserved.
- The scheduler must never hold the IDE-API lock, the method-install lock, or the
  actor-registry lock except during actor spawn/exit registration.
- No `memory_order_seq_cst` beyond what is already established by ADRs 009–012.
  The handle table uses `pthread_mutex_t` for its critical sections; no additional
  atomic ordering requirements arise.

---

## Open questions this spike deliberately does not answer

These are real questions identified during spike design. They are deferred so they
do not block this spike.

1. **Growable handle table.** The spike uses a fixed-capacity array of 1,024 entries.
   A production IDE session may hold handles to many more objects (inspector windows,
   debugger frames, actor monitor entries). Phase 3 must replace the fixed array with
   a growable structure whose entries have stable addresses (so `STA_Handle*` pointers
   remain valid after growth). A slab allocator (fixed-size slabs of handle entries)
   is the natural choice — growth allocates a new slab, old slabs are not freed, all
   pointers remain valid.

2. **Handle validity after `sta_vm_destroy`.** The spike invalidates all handles at
   destroy time (refcount forced to 0, OOP set to 0). The permanent implementation
   must specify what happens if the Swift caller retains a handle past `sta_vm_destroy`
   (undefined behaviour? a sentinel OOP that traps on dereference?). This is a Swift
   ARC discipline question; the C API should define the contract clearly.

3. **Bulk handle acquire and release.** The IDE Inspector may need to acquire handles
   to all slots of a large Array object in a single API call. A bulk interface
   (`sta_handle_acquire_slots(vm, array_handle, buf, count)`) would be more efficient
   than N individual calls. Deferred to Phase 3 when the Inspector is built.

4. **`sta_inspect_cstring` caller-provided buffer (Phase 3 breaking change).** The
   current design is **explicitly not thread-safe** — this is documented in the
   function signature, the rationale, and test case 5. The function is
   single-caller-at-a-time by contract; the IDE meets that contract today by calling
   only from the main thread. The Phase 3 fix changes the signature to a caller-
   provided buffer: `int sta_inspect_cstring(STA_VM*, STA_Handle*, char* buf, size_t len)`,
   returning bytes written (or required if buf is too small). This is a source-breaking
   API change and requires a version note in ADR 013. Phase 3 is the correct time
   because the Swift FFI wrapper is built then and can adopt the new signature from
   the start. Do not add thread-safety to the current single-buffer implementation as
   an interim step — the current design is correct for its documented contract; making
   the buffer swap atomic would give a false sense of thread-safety without fixing
   the underlying returned-pointer lifetime problem.

5. **Event callback re-entrancy.** The current design prohibits calling
   `sta_event_register` from within a callback. A production system may need to
   support dynamic callback registration (e.g., an actor monitoring another actor
   registers a crash callback). The exact re-entrancy rules need to be specified
   before the Swift FFI wrapper is built.

6. **Actor supervision tree traversal.** `STA_ActorInfo` exposes one level of
   supervisor linkage. The Actor Monitor needs the full tree. A recursive traversal
   using `sta_actor_enumerate` is possible but calls the visitor in undefined tree
   order. A separate `sta_actor_children(vm, actor_handle, visitor, ctx)` function
   that returns only the direct children of a given actor may be needed. Deferred
   to Phase 3 when the Actor Monitor is built.

7. **Event filtering and subscriptions.** The current push model delivers all events
   to all registered callbacks. The IDE may want to subscribe only to specific event
   types (e.g., only crash events for a specific actor). A type-filtered registration
   API (`sta_event_register_for_type(vm, type, callback, ctx)`) reduces callback
   overhead under high event rates. Deferred to Phase 3.

8. **`STA_Actor*` in `STA_ActorInfo` vs handle-only.** The current enumeration API
   creates handles for actor and supervisor identity. This requires two handle table
   allocations per enumerated actor. An alternative: expose the actor's stable numeric
   ID (a `uint64_t` assigned at spawn, monotonically increasing, never reused) as the
   identity instead of a handle. The ID is displayable without a handle. A separate
   `sta_actor_from_id(vm, id)` acquires a handle on demand. This would reduce handle
   pressure during large-scale enumeration. Evaluate in Phase 3.

---

## What ADR 013 must record

After running the spike, write ADR 013 covering:

- **Handle model chosen:** `STA_Handle*` with explicit reference counting, matching
  ADR 006's recommended direction. Full acquire/retain/release semantics as validated
  by the spike tests. This closes ADR 006 (DEFERRED status resolved).

- **Bootstrapping path:** how the IDE obtains its first handle without `sta_eval()`
  — via `sta_vm_nil()`, `sta_vm_true()`, `sta_vm_false()`, `sta_vm_lookup_class()`.
  No interpreter required for root access.

- **GC move invariant:** the handle table is a GC root set. GC moves update the
  `STA_OOP` inside the handle entry in place; the `STA_Handle*` pointer is stable.
  Validated by the handle lifecycle test.

- **`vm.h` additions accepted:** list each new function, its signature, and its
  rationale. Confirm that no function was added that is not required by a real IDE
  scenario.

- **Threading model:** all public API functions are safe from any OS thread. Three
  locks (IDE-API, method-install, actor-registry) are used; none is the scheduler
  lock; none constitutes a GIL. Lock-order rule documented. TSan result from test 14
  (full threading gate) must be quoted directly.

- **Live update stub validation:** method install and class define are TSan-clean
  under concurrent scheduler activity. Log count correct after concurrent install
  test. Frame-safety invariant deferred to Phase 1 interpreter spike.

- **Actor enumeration model:** snapshot model chosen (rationale: bounded lock hold,
  visitor decoupled from lock). Visitor called exactly N times for N registered actors.
  TSan result from tests 9 and 10.

- **Event model:** push model chosen (rationale: lower latency, matches SwiftUI/Combine
  idiom, pull model complexity). Callback registration API. Re-entrancy rules. TSan
  result from tests 11 and 12.

- **Actor density:** `STA_Actor` unchanged. Creation cost remains 296 bytes. 4 bytes
  of headroom preserved. Bridge state lives in the VM struct, not in the actor struct.
  `sizeof(STA_BridgeState)` reported as informational (not a per-actor cost).

- **Handle table sizing:** spike capacity of 1,024 entries. Production capacity deferred
  (see Open Questions). `sizeof(STA_HandleEntry)` = 16 bytes (measured).

- **`vm.h` surface obligation:** ADR 013 must enumerate every public function and type
  added by this spike and confirm each corresponds to a real IDE scenario from the
  spike document. No additions were made beyond those requirements.

- **Open questions closed:** ADR 006 (handle lifecycle, DEFERRED) is closed by this ADR.

- **Open questions explicitly deferred:** list the 8 open questions from this spike
  document with their deferral rationale.
