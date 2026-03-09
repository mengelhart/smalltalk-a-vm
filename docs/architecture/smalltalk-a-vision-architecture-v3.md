# Smalltalk/A

## Synthesized Vision, Architecture, and Project Plan

**Project name: Smalltalk/A**

*Draft foundation document*
*March 2026*

> **Purpose**
>
> This document replaces the earlier architecture draft with a more cohesive plan that integrates polished design and upstream brainstorming. It preserves the project's core ambition while separating foundational work from later experimental layers. It incorporates explicit decisions on implementation language, actor density targets, concurrency architecture, and async I/O — the load-bearing choices that must be made correctly from the start.

---

| | |
|---|---|
| **Project type** | Passion project focused on a live, coherent computing medium |
| **Primary platform** | macOS first, with a portable runtime core |
| **Project name** | Smalltalk/A |
| **Runtime direction** | Actor-concurrent Smalltalk runtime with capability-aware composition |
| **Guiding principle** | Correctness and explicit authority first; performance and speculative features later |

---

## 1. Executive Summary

**Smalltalk/A** is a modern Smalltalk system for macOS that combines a live image, actor-based concurrency, explicit capabilities, native developer tooling, and a runtime that operates both interactively and headlessly for services, daemons, workers, and web backends.

The key architectural move is to separate the system into layers. Layer 1 is already substantial and worthwhile on its own: a live Smalltalk-like environment with a portable runtime core, BEAM-class actor density, true preemptive concurrency, async I/O, image persistence, and native tooling. More speculative ideas — semantic capability discovery, LLM assistance, and distributed composition — remain additive layers rather than foundational dependencies.

> **Bottom-line stance**
>
> Build the practical core as if the higher layers may someday exist, but never require those higher layers for the system to be coherent or useful.

---

## 2. Vision

### 2.1 One-sentence vision

Smalltalk/A is a modern Smalltalk system: live, image-based, actor-concurrent at BEAM density, capability-aware, native on macOS, and deployable headlessly for production services.

### 2.2 What the system should feel like

- **Smalltalk in language and spirit:** the programming language *is* Smalltalk. Standard syntax, standard message sending, standard class hierarchy, standard collection protocols. A Smalltalk developer from 1995 should sit down and feel immediately at home. This is not "Smalltalk-like" or "Smalltalk-inspired" — it is Smalltalk, with a modern runtime underneath. The word "inspired" does not give implementors latitude to invent new syntax, drop standard protocols, or reinterpret semantics. When in doubt, the Blue Book is the reference.
- **BEAM in concurrency:** mutable state is owned, faults are isolated, supervision is explicit, and actor density is not a constraint — millions of lightweight actors is a normal operating condition, not a stress test.
- **No GIL, ever:** the system is designed from day one for true preemptive concurrency across multiple OS threads. Classical Smalltalk's cooperative green threads and global interpreter lock are explicit non-starters. Squeak's inability to use more than one CPU core is a known failure mode this project exists to correct.
- **Native macOS in user experience:** the environment feels like a serious modern application, not a nostalgic port.
- **Capability-oriented in composition:** authority and service surfaces are explicit rather than ambient.
- **Service-capable in deployment:** the same runtime supports headless execution for daemons, workers, web services, and unattended processes — no IDE required.

### 2.3 Constitutional principles

- Everything meaningful is an object.
- Every interaction is a message.
- Mutable state is owned, not shared.
- Authority is explicit, not ambient.
- The system is live and inspectable.
- Complexity must be additive, never foundational.
- Nothing is privileged unless explicitly designated as privileged runtime or tooling infrastructure.

---

## 3. The Case Against Classical Smalltalk Concurrency

This section exists to be explicit about what we are replacing and why, so architectural decisions later in this document are grounded in a clear problem statement.

Classical Smalltalk environments (Squeak, Pharo) have a **global interpreter lock (GIL)** and use **cooperative green threads**. This means:

- Only one thread of Smalltalk code executes at a time, regardless of how many CPU cores are available.
- Concurrency is simulated by cooperative yielding, not preemptive scheduling.
- A single long-running computation stalls the entire system.
- No true parallelism is possible — you cannot use more than one core.

This is why Smalltalk never achieved adoption for serious server-side or concurrent workloads. It is a fundamental architectural limitation, not a quality-of-implementation issue.

**Erlang/BEAM solved this in 1986.** The BEAM VM runs a scheduler per CPU core, each independently executing actor mailbox processing. A BEAM process (actor) costs approximately 300 bytes at creation. Millions of live actors is normal. Preemption is based on reduction counts — no actor can starve the scheduler. There is no GIL.

This project adopts the BEAM's concurrency model as a hard requirement, not an aspiration. The object model and live image come from Smalltalk. The concurrency architecture comes from BEAM. This combination has never existed in a production system. That is the opportunity.

---

## 4. Implementation Language Decisions

These decisions are load-bearing. They are made explicitly here and should not be revisited without a strong technical reason.

| Layer | Language | Rationale |
|---|---|---|
| **VM, object memory, actor runtime, GC, scheduler, async I/O** | **C** | Full memory control; no ARC interference when managing actor heaps; no GC pauses from the host language; portable; the only practical choice for a no-GIL, BEAM-density runtime |
| **IDE, tooling, native macOS UI** | **Swift / SwiftUI** | Native macOS UX; clean C FFI; modern ergonomics for the layer where they matter |

**Why C is non-negotiable for the core:**

BEAM-level actor density requires per-actor heap allocation in the range of hundreds of bytes at creation. Achieving this in a language with a host GC or ARC is not feasible — the host runtime's memory management overhead swamps the per-actor cost. C gives a blank slate: we control every allocation, every header layout, every cache line.

The no-GIL requirement similarly demands that the GC, method lookup cache, symbol table, and class metadata are designed for concurrent access from the start. This means explicit locking strategies, lock-free data structures where appropriate, and the ability to reason about memory visibility at the level of individual cache operations. This is C-level work.

Swift is used above the FFI boundary where it adds genuine value: SwiftUI for native macOS IDE, Swift actors for the IDE's own concurrency, and Swift's type system for the tooling layer. The IDE is a client of the runtime, not the runtime itself.

### 4.2 The public C API — designed for potential open sourcing

The VM is designed from day one as an **embeddable library with a clean, stable public C API** — even if it is not open-sourced immediately. The intent is that the public API could be opened at any point without requiring architectural surgery. This is a deliberate contrast with Squeak's VM, which was built as a monolithic application with no embeddable API boundary (see Appendix D).

**What this means in practice:**

- A single public header (`sta/vm.h`) is the only interface consumers need. All internal implementation headers are private and not part of any public contract.
- The public API surface is deliberately minimal and conservative. Things added to the public API become obligations. When in doubt, keep it internal.
- The API is expressed in pure C with no Swift types, no Objective-C, and no platform-specific types leaking through. Any language with C FFI — Python, Rust, Go, Java via JNI, another C program — can embed the VM.
- The Swift IDE uses exactly this public API. No privileged back-channel access. If the IDE needs something the public API cannot express, the API is extended — for everyone.
- A minimal embedding example (`examples/embed_basic/`) is maintained as both documentation and a permanent integration test for the public API surface.

**Why the "own IDE must use the public API" rule matters:** it is the only reliable way to ensure the API is sufficient and non-leaky. If the IDE author (you) ever finds yourself reaching around the public API for convenience, that is a signal the API has a gap — fix the gap rather than create a privileged channel. A future third-party IDE author would not have that privileged channel available; neither should the first one.

**Rough shape of the public API:**

```c
/* sta/vm.h — the only header embedders need */

typedef struct STA_VM    STA_VM;
typedef struct STA_Actor STA_Actor;
typedef uintptr_t        STA_OOP;

/* Lifecycle */
STA_VM*    sta_vm_create(const STA_VMConfig* config);
void       sta_vm_destroy(STA_VM* vm);
int        sta_vm_load_image(STA_VM* vm, const char* path);
int        sta_vm_save_image(STA_VM* vm, const char* path);

/* Actor interface */
STA_Actor* sta_actor_spawn(STA_VM* vm, STA_OOP class_oop);
int        sta_actor_send(STA_VM* vm, STA_Actor* actor, STA_OOP message);

/* Inspection and evaluation — used by IDE, available to all */
STA_OOP    sta_eval(STA_VM* vm, const char* expression);
STA_OOP    sta_inspect(STA_VM* vm, STA_OOP object);
```

This shape is illustrative, not final — it will be refined during Phase 0 spikes. The principle it embodies is fixed: small, stable, pure C, no privileged paths.

> **Note on `STA_OOP` in the sketch above:** the use of raw `STA_OOP` values is illustrative only and should not be read as the final embedding model. Long-lived host interaction will likely require opaque handles or rooted references rather than raw object pointers. See Section 4.3.

### 4.3 FFI object handles and lifetime

This is one of the most important missing-detail areas for the public API. Raw `STA_OOP` values returned across the FFI boundary are movable pointers — a compacting GC or actor migration can invalidate them. The embedding API must define explicit lifetime semantics before any host code stores returned OOP values.

The API must define:
- Whether host code receives raw `OOP`s or opaque handles (e.g. `STA_Handle*`)
- How returned objects are rooted or retained across GC
- Whether a host-held value may refer to actor-local memory
- Whether values are copied, proxied, pinned, or invalidated on GC
- Thread-affinity rules — which thread may use a given handle
- How inspector and debugger sessions safely refer to live runtime objects
- How handles are explicitly released to avoid leaks

**Recommended direction:** prefer opaque host-visible handles (`STA_Handle*`) over raw `STA_OOP` values for any API function whose return value may be stored beyond the current call. The runtime registers these handles as GC roots. The host explicitly releases them. This is the same model used by the JNI (`jobject`) and CPython (`PyObject*` with reference counting).

This decision will be made during Phase 0 spikes and locked before any IDE code is written against the public API.

---

## 5. Project Identity

The clearest identity for this project: **Smalltalk/A — a capability-oriented actor runtime running standard Smalltalk, with BEAM-class concurrency and a live image, deployable both as an interactive development environment and as a headless production runtime.**

### 5.1 The language is Smalltalk — not "Smalltalk-inspired"

This distinction matters and must be unambiguous, especially for anyone implementing the system.

The language surface is standard Smalltalk:
- Smalltalk syntax — message sends, cascades, blocks, assignments, returns
- Standard class hierarchy — `Object`, `Behavior`, `Class`, `Metaclass`, `Collection` family, `Number` hierarchy, `Stream`, `Exception`
- Standard protocols — `ifTrue:ifFalse:`, `do:`, `collect:`, `printOn:`, `doesNotUnderstand:`, `become:`, `yourself`, and the rest of the vocabulary a Smalltalk programmer expects
- ANSI Smalltalk compliance is the target for the core class library surface where practical
- The Blue Book (Goldberg & Robson, 1983) is the authoritative reference for core language, VM structure, and any ambiguous behaviour

The Blue Book and ANSI Smalltalk are treated as separate authorities. The Blue Book governs the core language and VM architecture. ANSI governs the class library surface. Where they differ, this project documents the chosen behaviour explicitly rather than assuming they are interchangeable.

**What "inspired" does and does not mean:** the word "inspired" in earlier drafts referred only to the fact that we are not promising binary image compatibility with Squeak or Pharo, and that we are not recreating every historical quirk of Smalltalk-80 exactly as shipped in 1980. It does not mean: inventing new syntax, dropping standard protocols, reinterpreting semantics, or taking liberties with the object model. When implementing any language feature, the default answer is "do what Smalltalk does."

The actor model is the **runtime and concurrency architecture**, not a language change. See Section 5.2.

**Runtime-level extensions beyond historical Smalltalk:**

The following additions are made explicitly to the standard Smalltalk model. They are not hidden behind "standard Smalltalk" phrasing:

- Actor-oriented concurrency — isolated actors with owned heaps, mailboxes, and scheduled execution
- Futures and asynchronous request-response — `ask:` / `future` semantics for cross-actor messaging
- Capability-oriented authority and service discovery — explicit capability tokens; no ambient authority
- Headless runtime and deployment surfaces — lifecycle, supervision, and logging for unattended execution
- Privileged tooling capabilities — debugger and inspector access that would otherwise violate actor isolation

Code *inside* an actor uses none of this explicitly. These extensions only surface at actor boundaries and in system-level design.

### 5.2 The actor model is OOP — not a replacement for it

This is the other point of potential confusion. The actor model does not replace the object model. It *is* the object model, taken more seriously than classical Smalltalk did.

Alan Kay's original vision for OOP — cells communicating by message passing, no cell reaching into another cell's internals — is exactly the actor model. Classical Smalltalk got the syntax and the message metaphor right but compromised on the runtime: shared heap, synchronous sends, no isolation. The actor model restores what the metaphor always implied.

**From a programmer's perspective:**

Code *inside* an actor is pure classical Smalltalk. Same syntax, same semantics, same feel. A class, its methods, its instance variables, the objects it creates — all of that is unchanged. The programmer writes Smalltalk.

The actor boundary only surfaces when designing the concurrent structure of a system:

```smalltalk
"Inside an actor — pure Smalltalk, unchanged"
result := someCollection select: [:each | each value > 10].

"Communicating between actors — new, but natural"
future := someActor ask: #processCollection: with: someCollection.
future onValue: [:result | self handleResult: result].
```

**The key principle:** most application code never touches the actor boundary. Logic, algorithms, domain models, data transformation — all pure Smalltalk. Actors are the architectural wiring, not the code you write every line.

A Smalltalk developer from 1990 would find:
- Syntax: identical ✓
- Class library: familiar, ANSI-compatible ✓
- IDE (browser, inspector, workspace, debugger): familiar ✓
- Single-actor code: identical to what they know ✓
- Multi-actor system design: new, but recognisable as message passing taken seriously ✓

### 5.3 What this project is not

- Not a Morphic revival or a compatibility clone of existing Smalltalk systems.
- Not a new syntax experiment — the syntax is standard Smalltalk, full stop.
- Not a distributed internet-scale system in version 1.
- Not an LLM-native runtime in version 1.
- Not a cross-platform GUI project from day one.
- Not a claim of full Phoenix/OTP feature parity in version 1 — but actor density and true preemptive concurrency are v1 requirements, not future aspirations.

---

## 6. Repository Structure and Toolchain

### 6.1 Two repositories

The project lives in two separate Git repositories with a clean dependency direction: the IDE depends on the VM; the VM has no knowledge of the IDE.

```
yourname/smalltalk-a-vm       ← C runtime, CMake, standalone library
yourname/smalltalk-a-ide      ← Swift / SwiftUI, Xcode project
```

**Why separate repos rather than a monorepo:**

- **Portability is real.** The VM must build on Linux for headless deployment. A Linux CI pipeline should never encounter an Xcode project file. CMake builds cleanly everywhere with no Apple toolchain dependency.
- **Different rhythms.** The C VM will see intense low-level work during Phases 0–2 before the Swift IDE starts in earnest. Separate repos mean separate commit histories, issues, and release tags. The VM can be at v0.4 while the IDE is just beginning.
- **Dependency direction is enforced.** A separate repo makes it structurally impossible for the VM to accidentally import IDE code. The one-way relationship is architectural, not a convention.
- **Future open sourcing is clean.** If the VM is open-sourced independently, there is nothing to extract or disentangle. The repo is already self-contained.
- **Claude Code works better with focused repos.** A repo that is purely C with a clear CMakeLists.txt and no Xcode noise is easier to reason about and navigate.

### 6.2 VM repository layout

```
smalltalk-a-vm/
    include/
        sta/
            vm.h            ← public API only — the stable contract
    src/                    ← private implementation (not installed)
        vm/
        actor/
        gc/
        scheduler/
        io/
        bootstrap/
    tests/
    examples/
        embed_basic/        ← minimal embedding example and API smoke test
    docs/
    CMakeLists.txt          ← primary build system
    LICENSE
    CONTRIBUTING.md
    README.md
```

The `include/sta/` directory contains only public headers. Everything in `src/` is private. The CMake install target installs only `include/sta/vm.h` and the built library — nothing else.

`compile_commands.json` is emitted by CMake (`set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`) and either symlinked or copied to the project root for clangd.

### 6.3 IDE repository layout

```
smalltalk-a-ide/
    SmalltalkA.xcodeproj
    Sources/
        Bridge/             ← Swift FFI wrapper around sta/vm.h
        IDE/                ← SwiftUI application
    Tests/
    Frameworks/
        smalltalk-a-vm/     ← git submodule, pinned to a commit
    README.md
```

The VM is included as a **git submodule** pinned to a specific commit. This keeps builds reproducible without requiring a package distribution step — the right tradeoff for a solo passion project. The Xcode project links against the static library built by CMake from the submodule.

During development, the submodule path can be pointed at a local checkout of the VM repo for rapid iteration across both codebases simultaneously.

### 6.4 Toolchain

| Task | Tool |
|---|---|
| C VM editing | Emacs + LSP + clangd (via `compile_commands.json`) |
| C VM building and testing | CMake + make / ninja, terminal |
| C VM debugging | `lldb` in terminal |
| Swift IDE editing | Emacs or Xcode (Xcode required for SwiftUI previews and Instruments) |
| Swift IDE building | Xcode |
| Agentic coding assistance | Claude Code in terminal |
| Version control | Git, two repos |

Xcode is touched only for the Swift IDE layer. All C work — editing, building, testing, debugging — stays in the terminal with Emacs and clangd. This is not a compromise; clangd with `compile_commands.json` provides better C navigation than Xcode for a pure C codebase.

### 6.5 Licensing intent

No licensing decision is required before the first commit. The decision is worth making deliberately before any public release. Two realistic options:

- **MIT or Apache 2.0** — maximum openness; anyone can embed the VM commercially without restriction; maximizes ecosystem potential; the BEAM itself is Apache 2.0
- **LGPL** — the VM stays open; modifications to the VM must be shared back; embedders do not have to open-source their own applications

GPL is too restrictive for a runtime intended to be embedded. MIT/Apache or LGPL are the only practical choices. Record the decision in LICENSE before publishing.

---

## 7. Layered Roadmap

The project stays coherent by treating ambition as a ladder rather than a bundle. Each layer should be independently valuable.

| Layer | Purpose | Primary contents |
|---|---|---|
| **Layer 1** | Practical foundation | Live Smalltalk-like kernel, BEAM-density actor runtime, true preemptive concurrency, async I/O, image persistence, native IDE, basic capability substrate, headless runtime |
| **Layer 2** | Capability-oriented composition | Capability manifests, registry actor, exact lookup, structural matching |
| **Layer 3** | Semantic assistance | Embeddings, semantic search, cold-path reasoning, suggestion workflows |
| **Layer 4** | Agentic system participants | IDE message surface, agents using the same APIs as humans, assisted workflows |
| **Layer 5** | Federated experiments | Cross-image negotiation, signed manifests, explicit distribution boundaries |

---

## 8. High-Level Architecture

### 8.1 Architectural split

A clean boundary separates the native UI/tooling layer from the portable runtime layer. The runtime is portable and disciplined; the user experience is unapologetically native on macOS. The same runtime binary supports both IDE-hosted development and UI-free deployment.

```
┌─────────────────────────────────────────────────────────┐
│                    SWIFT / SWIFTUI LAYER                │
│  Workspace · Browser · Inspector · Debugger             │
│  Actor Monitor · Semantic tooling (Layer 3+)            │
│  Privileged operational shell                           │
└─────────────────────┬───────────────────────────────────┘
                      │  Clean FFI / host interface
                      │  IDE is a client; runtime is
                      │  the semantic authority
┌─────────────────────┴───────────────────────────────────┐
│                      C RUNTIME LAYER                    │
│  Object memory · Bytecode interpreter · Primitive set   │
│  Actor runtime · Per-actor heaps · Work-stealing sched  │
│  Generational GC (per-actor nursery; progressive concurrency)  │
│  Async I/O substrate · Image persistence                │
│  Capability substrate · Symbol table (lock-free)        │
└─────────────────────────────────────────────────────────┘
```

### 8.2 Runtime operating modes

- **Interactive mode:** a live image hosted by the native IDE for exploratory programming, browsing, debugging, and system evolution.
- **Headless mode:** a UI-free runtime for daemons, services, workers, automation, and web backends. Explicit lifecycle, supervision, configuration, and logging surfaces. The image is the same semantic unit whether or not an IDE is attached. Headless services must acquire operational authority through capabilities exactly as IDE-hosted code does — deployment mode does not justify ambient power.

### 8.3 Runtime model

Objects use a classic reflective Smalltalk-style model with compact representation and tagged immediates.

```c
typedef uintptr_t OOP;        // Object-Oriented Pointer

// Low bit set = small integer (63-bit signed on 64-bit platforms)
#define IS_SMALLINT(oop)      ((oop) & 1)
#define SMALLINT_VAL(oop)     ((intptr_t)(oop) >> 1)
#define SMALLINT_OOP(n)       (((uintptr_t)(n) << 1) | 1)

// Heap objects: word-aligned pointer, low bit clear
typedef struct ObjHeader {
    uint32_t class_index;     // index into class table
    uint32_t size;            // payload size in words
    uint8_t  gc_flags;        // GC color, forwarding flag
    uint8_t  obj_flags;       // immutable, pinned, actor-local, etc.
    uint16_t reserved;
} ObjHeader;
```

**Actors are runtime entities presented as objects.** They are not merely ordinary objects; the distinction is load-bearing. Each actor has:

- A mailbox (lock-free MPSC queue)
- A private mutable heap (isolated; no external references in)
- A behavior object or class
- A supervisor linkage
- An opaque address / capability

**Per-actor heap target:** actor creation cost and baseline heap size should be in the same order of magnitude as BEAM (~300 bytes). This is an explicit design constraint, not a nice-to-have. It determines header layout, allocator granularity, and GC strategy.

### 8.4 Shared immutable regions

These regions are shared across all actors and may be referenced without violating isolation because they are immutable after bootstrap. The allocator marks these pages read-only once bootstrap completes — immutability is enforced at the C level, not by convention.

- Interned symbols
- Compiled method bytecode (write-once at compile time)
- Immutable literals
- Explicitly frozen binary buffers

### 8.5 Shared coordinated mutable metadata

Some system-wide structures are shared but not immutable. They require explicit synchronization and have privileged mutation paths:

- Class table and method dictionaries (mutated through method compilation and live editing)
- Dispatch caches and selector lookup structures
- Capability manifest associations
- Registry indexes
- Source metadata used by tooling

These are not a general shared mutable heap available to arbitrary code. They are coordinated runtime metadata with explicit concurrency rules. Ordinary actors cannot write to them. Mutation goes through privileged runtime operations — method installation, class definition, capability registration — which acquire appropriate locks and handle cache invalidation.

This distinction resolves an apparent contradiction: the system has "no shared mutable state" from an application code perspective, but the runtime itself must maintain coordinated shared metadata. Both statements are true in their respective scopes.

---

## 9. Concurrency Architecture

This section is foundational. The decisions made here cannot be changed later without rebuilding the runtime.

### 9.1 Scheduler

The scheduler runs **one OS thread per CPU core** (configurable). Each scheduler thread independently processes actor mailboxes. The design is directly inspired by BEAM's SMP scheduler.

- **Work-stealing:** idle scheduler threads steal runnable actors from busy threads' run queues. This provides automatic load balancing without central coordination.
- **Reduction-based preemption:** each actor is granted a budget of reductions (roughly equivalent to bytecode operations) per scheduling quantum. When the budget is exhausted, the actor is suspended and returned to the run queue. No actor can starve the scheduler. This is the BEAM model.
- **No GIL:** there is no global lock protecting the interpreter. The symbol table, class metadata, and method cache are designed for concurrent access. Actors on different scheduler threads execute truly in parallel.

### 9.2 Garbage collection

The GC is **generational**, with per-actor nursery collection as the common case. The target is progressive concurrency — early implementations should prioritize correctness and short safepoints over ambitious latency claims, with more concurrent behaviour as the runtime matures.

- **Per-actor nursery:** minor GC runs within a single actor's heap and requires no coordination with other actors. This is the common case and should be extremely fast.
- **Immutable shared regions:** not collected; allocated at bootstrap and held for the lifetime of the image.
- **Old space / major GC:** may initially require brief global coordination (a short stop-the-world pause). Concurrent mark-and-sweep is the target as the implementation matures. Actor isolation dramatically simplifies this — cross-actor references exist only through capabilities and actor addresses, which are well-defined roots.

### 9.3 Async I/O

**This is a first-class architectural requirement, not a Phase 2 afterthought.** Actors must never block a scheduler thread on I/O. A scheduler thread blocked on a socket read is a scheduler thread that cannot execute any other actor — with a fixed pool of scheduler threads equal to CPU count, even one blocking call degrades the entire system.

The async I/O substrate sits in the C runtime layer and provides non-blocking I/O for:

- TCP/UDP sockets
- File I/O
- DNS resolution
- Subprocess execution
- Timers and timeouts

**Implementation approach:** on macOS/Linux, use `kqueue`/`epoll` via a dedicated I/O polling thread (or `io_uring` on Linux for maximum throughput). I/O-bound actors register interest and are suspended; the I/O poller wakes them when their operation completes by placing a completion message in their mailbox. The actor resumes on the next scheduler quantum as if it received any other message.

This model means:
- Scheduler threads never block on I/O
- I/O-heavy workloads scale naturally with actor count, not thread count
- Web server actors, database client actors, and file processing actors are all first-class citizens with the same scheduling guarantees as compute-bound actors

Concrete recommendation for the initial implementation: **wrap `libuv`** (the async I/O library underlying Node.js). It is production-proven, handles all the platform differences between macOS and Linux, and provides the event loop model we need. It can be replaced with a custom implementation later if needed. This is not a permanent dependency — it is a pragmatic starting point.

### 9.4 Backpressure, mailboxes, and flow control

Async I/O alone is insufficient for production workloads without defined mailbox and buffering policy. Silent queue growth under load is a failure mode that must be designed out, not discovered in production.

The runtime must define:
- Whether mailboxes are bounded or unbounded by default
- Overflow policy — options include: drop oldest, drop newest, reject sender (send failure reply), or escalate to supervisor
- Timeout and cancellation semantics for pending `ask:` futures
- Backpressure behaviour for service-facing actors under sustained load

**Default stance:** bounded mailboxes with observable overflow behaviour. Unbounded mailboxes are available but explicit — a programmer opting into them is acknowledging the risk. Silent queue growth is never the default. This follows the principle that overload should be visible, not hidden.

---

## 10. The Actor Model — Design Decisions

This section captures explicit decisions about how actors relate to plain Smalltalk objects, where the actor boundary lives, what the global memory model looks like, and how the supervision tree is structured. These decisions are foundational — they affect both the VM implementation and how programmers experience the system.

### 10.1 Actors are explicit runtime-backed objects

Actors are explicitly declared through an `Actor` base class. This is a deliberate design decision: actor boundaries carry isolation, lifecycle, mailbox, and supervision semantics, and those things should be visible in the code. When you see `Actor subclass: #Foo` you know immediately this is a concurrent boundary. When you see `Object subclass: #Bar` you know it is plain logic living inside some actor's heap.

`Actor` is a runtime-backed class — each instance has a mailbox, a private heap, scheduler identity, and supervisor linkage. It is not part of the normal domain class hierarchy. `Object` remains the root for all plain domain objects. `Actor` is a separate root for concurrent system entities. The Smalltalk class hierarchy for domain code is unentangled with the concurrency model.

```smalltalk
"A plain domain class — pure Smalltalk, no actor machinery"
Object subclass: #ShoppingCart
    instanceVariableNames: 'items customerId'

ShoppingCart >> addItem: anItem
    items add: anItem.    "pure Smalltalk, nothing actor-specific"

"An actor class — explicitly a concurrent, isolated, supervised entity"
Actor subclass: #CartActor
    instanceVariableNames: 'cart'

CartActor >> initialize
    cart := ShoppingCart new.    "plain object lives on my private heap"

CartActor >> addItem: anItem
    cart addItem: anItem.        "synchronous call to plain object — fine"
    self notifySubscribers.      "async message to another actor"
```

The `CartActor` is the concurrent unit — scheduled, isolated, supervised. The `ShoppingCart` is a plain Smalltalk object living inside `CartActor`'s private heap. You test `ShoppingCart` in complete isolation with no actor machinery involved. You use `CartActor` when you need concurrency, fault isolation, or external addressability.

**Why Model A over making every object a potential actor:** explicit actor declaration keeps the architecture visible in the code. When you see `Actor subclass: #Foo` you know immediately this is a concurrent boundary with its own heap and lifecycle. When you see `Object subclass: #Bar` you know it's plain logic living inside some actor. Blurring that boundary makes cost, lifecycle, and supervision invisible — exactly the accidental complexity this system is designed to avoid.

### 10.2 When to use an actor

Use an actor when one or more of these is true:

- **Concurrency** — this thing should execute in parallel with other things
- **Fault isolation** — a crash here must not affect the rest of the system
- **Supervision** — something needs to monitor and restart this if it fails
- **Owned state** — this is the single long-term owner of a resource (connection, file handle, cache, session)
- **External addressability** — other actors need to hold a stable reference to this and message it over time

Use a plain object for everything else. Most application code is plain objects. Actors are the architectural skeleton — a few dozen to a few hundred in a typical service. Plain objects are everything else, living inside those actor heaps.

### 10.3 What is and is not an actor — concrete examples

**In the IDE:**

| Thing | Actor? | Reason |
|---|---|---|
| Workspace | Yes | Owns mutable session state; fault-isolated; supervised |
| System Browser | Yes | Independent UI concern; own lifecycle |
| Inspector | Yes | Watches a live object; own lifecycle |
| Debugger | Yes | Privileged system participant; supervised separately |
| Actor Monitor | Yes | Observes the supervision tree; own lifecycle |
| HashMap created in a workspace | No | Plain data owned by WorkspaceActor |
| String keys in that map | No | Plain values on the workspace heap |
| Objects stored as map values | No | Plain objects on the workspace heap |

**In a server application:**

| Thing | Actor? | Reason |
|---|---|---|
| HTTP listener | Yes | Owns a socket; runs concurrently |
| Per-request handler | Yes | Isolated fault boundary per request |
| Session state manager | Yes | Long-lived owned state; addressable over time |
| Database connection pool | Yes | Owns external resources |
| ShoppingCart | No | Plain logic object inside a session actor |
| SQL query builder | No | Plain logic; no state of its own |
| JSON parser | No | Stateless utility; just a plain object |

### 10.4 No global mutable heap

There is no global mutable heap. This is a stronger property than BEAM provides (BEAM has ETS — shared mutable tables between processes). Every mutable object in this system is owned by exactly one actor.

Image memory has exactly two kinds of regions:

```
Image memory
├── Immutable shared regions  (read-only after bootstrap)
│     ├── Symbol table
│     ├── Compiled method bytecode
│     ├── Class metadata
│     └── Immutable literals
│
└── Actor heaps  (mutable, isolated, one per actor)
      ├── ImageSupervisorActor heap
      ├── IDESupervisorActor heap
      ├── WorkspaceActor #1 heap    ← HashMap created here lives here
      ├── SystemBrowserActor heap
      ├── DebuggerActor heap
      └── ... every other actor
```

The immutable shared regions are the compiled system — symbols, bytecode, class definitions. They are written once during bootstrap or method compilation and never modified during normal execution. Everything mutable belongs to one actor, full stop.

**Workspace example:** when you create a `HashMap` in a workspace and store objects in it, those objects live in the `WorkspaceActor`'s private heap. The workspace is transparently an actor — the programmer does not think about this, they just use the workspace. The actor machinery is invisible until you open the Actor Monitor and see workspace actors listed alongside everything else.

### 10.5 The supervision tree

Every actor has a supervisor. The tree is rooted at the `ImageSupervisorActor` — the only actor that starts with ambient authority, because something must bootstrap the system.

```
ImageSupervisorActor                    ← root; grants all privileged capabilities
├── IDESupervisorActor
│     ├── WorkspaceActor #1
│     ├── WorkspaceActor #2
│     ├── SystemBrowserActor
│     ├── InspectorActor
│     ├── DebuggerActor
│     └── ActorMonitorActor
└── SystemServicesSupervisorActor
      ├── CapabilityRegistryActor
      ├── ImagePersistenceActor
      └── AsyncIOBrokerActor
```

In headless mode the IDE subtree is absent. The system services subtree remains, plus whatever application supervisors the deployed service defines.

**The image supervisor is deliberately thin.** It spawns its direct children, holds supervision references, grants startup capabilities, and handles top-level restart strategy. The less logic at the root, the less that can go wrong there.

### 10.6 Supervision references vs. capability references

This distinction is critical and commonly conflated:

- **Supervision reference** — held by a supervisor to manage an actor's lifecycle: restart it, stop it, or escalate when it crashes. This is a structural relationship, not a work messaging channel.
- **Capability reference** — held by any actor that needs to *send messages* to another actor for actual work. May be held by a supervisor, a sibling, or an external system.

These are separate objects with separate purposes. `IDESupervisorActor` holds supervision references to all IDE actors so it can restart them. `SystemBrowserActor` holds a capability reference to `DebuggerActor` so it can ask it to open a debugging session. The supervisor can restart the debugger without messaging it for work. The browser can message the debugger without supervising it.

Designing supervision and messaging as the same relationship leads to supervisors doing too much work — which undermines the fault isolation supervision is meant to provide.

### 10.7 Privileged tooling capabilities

The debugger, inspector, and other IDE tooling need capabilities ordinary actors must not have — specifically the ability to suspend another actor and read its private heap. These are not back-doors around the capability model. They are explicit privileged capabilities granted at startup by the image supervisor to trusted system actors.

```
ImageSupervisorActor grants at startup:
  → DebuggerActor  receives DebugPrivilegeCapability
      (may request any actor suspend; may read its heap for inspection)
  → InspectorActor receives InspectPrivilegeCapability
      (may read any actor's heap non-destructively)
  → IDESupervisorActor receives ToolingPrivilegeCapability
      (may spawn and manage IDE actors with elevated access)
  → No other actor receives these
```

The difference between the debugger legitimately reading another actor's heap and a malicious actor attempting the same is that the debugger holds `DebugPrivilegeCapability` issued by the root supervisor at startup. Nobody else holds it. The capability model is not bypassed — it is what makes that distinction enforceable.

### 10.8 Privileged cross-actor inspection and mutation semantics

Privileged tooling (debugger, inspector) may inspect or mutate another actor's state only through explicit runtime-mediated operations. The sequence is:

1. Tooling actor presents its `DebugPrivilegeCapability` or `InspectPrivilegeCapability`
2. Runtime suspends the target actor at a safe point (not mid-message, not mid-GC)
3. Tooling performs inspection or edit through privileged runtime APIs
4. Runtime resumes the target actor in a coherent state

Cross-actor heap mutation is not a general capability. It is exceptional, explicit, auditable, and runtime-coordinated. An ordinary actor that somehow obtained a reference to another actor's heap objects could not read or write them — the runtime enforces actor-local access. Only privileged capabilities unlock this path, and only the root supervisor issues them.

### 10.9 Failure, restart, and request semantics

The supervision model requires operational semantics, not just structural description. The runtime must define exactly what happens when things go wrong.

**Actor failure:**
- An uncaught exception during message processing terminates the actor
- The supervisor receives a failure notification containing the actor reference and error
- Pending `ask:` futures to the failed actor receive a failure result, not a timeout
- The actor's heap is discarded; its supervisor reference remains valid for restart decisions

**Restart behaviour:**
- Restart creates a fresh actor instance with a clean heap (same class, new state)
- The previous instance's heap is not available to the new instance
- Restart strategy (immediate restart, back-off, stop, escalate) is configured per supervisor

**Future semantics on failure:**
- An `ask:` future has three terminal states: value, failure, timeout
- All three must be handleable by the caller
- A future that can only handle value is an incomplete design

**Default restart strategy:**
- Unexpected termination → supervisor notified → supervisor applies configured strategy
- Strategy choices: restart-immediately, restart-with-backoff, stop-and-notify-parent, escalate

This section will be fleshed out further during Phase 2, but these defaults must be established before the first actor runtime is built.

### 10.10 Network addressability (Layer 5 direction)

Because actor addresses are opaque capability tokens rather than raw memory pointers, extending them across the network is architecturally natural. This is a Layer 5 concern, but it shapes how local addressing is designed from the start.

```
Local:   <opaque-capability-token>          (unforgeable, local scope)
Remote:  sta://host:port/actor-id           (URL-shaped capability, verified at ingress)
```

Local and remote actor references share a common conceptual message model, but remote messaging is not operationally identical to local messaging. Network latency, failure modes, timeout, authentication, and serialization are first-class concerns that must remain visible in the model. Code that sends a message to a remote actor must acknowledge those differences — transparent distribution that hides them entirely is a known distributed-systems trap (see CORBA, Java RMI).

The design goal is: local actor addressing uses opaque tokens from the start so that remote addressing is a natural extension, not a retrofit. It does not mean remote failure and latency are hidden from callers.

---

## 11. The Bootstrapping Problem

**This is the hardest single implementation task in the project.** It is documented here explicitly so it is not underestimated.

The fundamental challenge: the Smalltalk object system is self-describing. Classes are objects. `Class` is an instance of `Metaclass`. `Metaclass` is a subclass of `ClassDescription`. `ClassDescription` is a subclass of `Behavior`. `Behavior` uses `MethodDictionary`. `MethodDictionary` depends on `Array`. `Array` is an instance of `Array class`... and so on.

You cannot use the object system to build the object system, because the object system does not yet exist.

**The bootstrap strategy:**

1. **Manually allocate kernel objects in C** — bypass the object system entirely. Lay out `SmallInt`, `Object`, `Behavior`, `ClassDescription`, `Class`, `Metaclass`, `Array`, `Symbol`, `MethodDictionary`, and `ByteArray` by hand using the header format. This is ugly and direct.
2. **Wire the metaclass web manually** — establish the `Class`/`Metaclass` circularity by directly writing OOPs into the header fields.
3. **Install primitive methods** — add C-backed primitives to the kernel classes. At this point the object system is minimally functional.
4. **Load the kernel source** — a minimal Smalltalk source file defines the rest of the class hierarchy using the now-functional object system.
5. **Snapshot** — serialize the bootstrapped image to disk. All subsequent launches load from this snapshot. The bootstrap code runs exactly once.

The Blue Book (Goldberg & Robson, 1983) is the reference for the class hierarchy and metaclass structure. Reading it before writing any bootstrap code is not optional.

---

## 12. Capability Model

A capability is an unforgeable reference to an object that implicitly grants the right to send it messages. Capabilities serve two related but distinct purposes:

| Aspect | Meaning | Examples |
|---|---|---|
| **Authority** | What an actor is allowed to do | Filesystem access, network access, privileged IDE actions |
| **Service surface** | What an actor can provide | Parse JSON, transform text, persist an object graph |
| **Negotiation surface** | How unknown peers discover compatibility | Manifest matching, version/protocol agreement, semantic suggestions (later layers) |

### 12.1 The class/instance distinction — capabilities are zero overhead per actor

This is one of the most important design clarifications in the entire capability model, and it resolves what might otherwise appear to be a tension between BEAM-level actor density and rich capability descriptions.

**Capability advertisements live at the class level, not the instance level.**

When an actor class declares "I can lowercase text" or "I provide a JSON parsing service," that declaration is metadata on the *class* — defined once, shared across every instance of that class. An actor instance carries a pointer to its class as it already must for method dispatch. The capability advertisement adds zero bytes to the per-actor instance cost.

```
LowercaseTransformerActor  ← class (defined once)
  manifest: "I can lowercase text"
  protocol: TextTransform/1.0
  required capabilities: none
        │
        ├── instance #1  (~300 bytes — private heap + mailbox)
        ├── instance #2  (~300 bytes)
        ├── instance #3  (~300 bytes)
        └── instance #N  (~300 bytes)
```

This maps directly onto how Smalltalk already works: classes carry shared behavioral descriptions, instances carry only private mutable state. Capability metadata is a natural extension of that existing distinction, not a new concept bolted on. The 300-byte instance target is entirely compatible with arbitrarily rich capability manifests.

**Registry queries operate on the class table, not on actor instances.** When the registry searches for "something that can lowercase text," it queries class metadata. It finds `LowercaseTransformerActor`, then returns a reference to an existing instance or creates a new one. Discovery never touches instance heaps. As the system scales to millions of actor instances, the registry index grows with *class* count — orders of magnitude smaller.

### 12.2 Advertisement vs. authorization — two different things

A critical distinction that must not be conflated:

- **Advertisement** ("this class of actor can lowercase text") — static, class-level, declared in the manifest, consulted during discovery. Lives on the class. Free per instance.
- **Authorization** ("this specific caller is authorized to send lowercase requests to this specific actor") — dynamic, per-relationship, encoded in a capability token held by the *caller*. Lives in the caller's heap, not in the actor's heap.

The class advertises what it *can* do. The runtime *issues* a capability token to a specific caller with specific permissions when that authorization is established. These are separate objects with separate lifecycles. An actor that serves a million callers holds no per-caller state in its own heap — each caller holds their own token.

A cryptographically secure capability token is compact — roughly 32–64 bytes:

```
[ actor_address | capability_id | permissions_mask | HMAC ]
```

**Inside a single runtime image, capabilities are unforgeable by construction** — they are opaque runtime references that cannot be fabricated by ordinary code. Cryptographic protection is relevant at trust boundaries: cross-image federation, remote ingress, or serialization to untrusted storage. Local capability use does not depend on hot-path cryptographic verification. The HMAC in the layout above applies to externally-visible tokens (Layer 5); local tokens are opaque pointers whose unforgeability is structural, not cryptographic.

For cross-image or federated capabilities (Layer 5): a public-key signature is verified *once at the trust boundary* and converted to a local unforgeable reference inside the runtime. Crypto happens at ingress, not on every message send.

### 12.3 What lives where — summary

| Thing | Lives at | Cost per actor instance |
|---|---|---|
| "I can lowercase text" manifest | Class | 0 — class pointer already exists |
| Bytecode for message handlers | Class (compiled method) | 0 |
| Version / protocol metadata | Class manifest | 0 |
| Semantic description (Layer 3+) | Class manifest | 0 |
| Authorization token for a held capability | Caller's heap | 0 in the serving actor |
| Private mutable state | Actor instance heap | The ~300 bytes |
| Mailbox | Actor runtime struct | Included in ~300 bytes |

### 12.4 No ambient authority

There is no global namespace of dangerous objects any code can reach. To open a file, you must hold a capability to a filesystem object, which must have been explicitly granted. This applies equally to IDE-hosted and headless code. Deployment mode does not grant ambient power.

### 12.5 Capability manifests

Each actor class declares:
- Provided capabilities
- Required capabilities
- Version or protocol metadata
- Optional semantic description (Layer 3+)
- Attenuation and security properties

### 12.6 Registry actor

Capability discovery begins with an explicit registry actor rather than hidden framework magic. The registry primarily indexes the class table — capability manifests and schemas are class-level metadata, so discovery scales with class count, not instance count.

The registry also maintains instance-level registrations for discoverable live services where identity, placement, availability, or external bindings matter at runtime (e.g. a named database connection pool, the active HTTP listener, a singleton session manager). These are explicit, named registrations — not automatic per-instance metadata. Evolution path: exact-match index first → structural lookup → semantic suggestion (Layer 3).

### 12.7 Composition ladder

1. Exact capability lookup — deterministic and fast
2. Conjunctive or declared composites — deterministic compositions of known pieces
3. Suggested dynamic pipelines — proposed assemblies from known components, initially subject to human approval
4. Promotion — repeated validated success may justify promoting a composition to a first-class capability

### 12.8 Security constraints

- External resources (files, sockets, OS integrations) available only through explicit capabilities
- Tooling privileges are real privileges — debugger, inspector, IDE bridge are privileged infrastructure, not untrusted participants
- Serialization and restore must not recreate authority illegitimately; capability tokens for external resources are revivable handles, not persisted authority
- Reflection must not bypass capability boundaries
- The GC and runtime must not provide a side channel for bypassing isolation
- For federated/cross-image capabilities (Layer 5): cryptographic verification happens once at the trust boundary; local references inside the runtime are unforgeable by construction, not by ongoing crypto cost

### 12.9 Revocation, attenuation, and expiry

The capability model must define revocation and attenuation semantics, even if early versions implement a minimal version explicitly. Leaving these undefined means they are accidentally defined by whatever the implementation happens to do.

**Attenuation:** a capability may be delegated in a restricted form — a filesystem capability attenuated to read-only, or a service capability scoped to a specific operation. The attenuated token carries fewer permissions than the original.

**Revocation:** a granted capability may be revoked. The design must choose: immediate (the token stops working the moment it is revoked) vs. lazy (checked at use time). Revocable capability references (a small proxy object that can be zeroed) are the standard pattern.

**Expiry:** capabilities may have a time-bound. After expiry they become invalid without explicit revocation.

**Persistence:** capability tokens serialized into an image snapshot must be revalidated on restore, not assumed still valid.

**Early version stance:** implement capability grant without revocation first. Define the revocation API surface (so callers can be written against it) but leave the implementation as a stub that always returns "still valid." Fill in real revocation semantics in Phase 4. This keeps the design honest without blocking early progress.

---

## 13. Image and Persistence Model

The image is the entire persistent state of the system: actor heaps, immutable shared runtime regions, class metadata, symbol tables, and capability graphs. This definition holds in both interactive and headless operation.

### 13.1 Snapshot mechanics

Taking a consistent snapshot of a multi-actor, multi-scheduler-thread system:

1. Send `#prepareForSnapshot` to the root supervisor
2. Supervisor propagates to all actors
3. Each actor finishes its current message and quiesces
4. Scheduler threads reach a global safe point
5. The C layer walks all actor heaps and serializes
6. Resume — actors receive `#snapshotComplete`

### 13.2 External resource handling

In-flight futures, timers, sockets, file handles, and network connections cannot be naively serialized.

> **Practical rule for early versions:** treat external resources as revivable handles. Do not promise complete persistence of open files, sockets, timers, or model-backed services in the first implementation. Define explicit restore semantics — an actor whose external handle cannot be reattached receives a notification and can respond accordingly.

### 13.3 Headless deployment model

Headless execution is not merely "run the VM without an IDE." It is a defined operational mode with its own requirements.

A headless deployment consists of:
- The runtime binary (the C VM, built without any IDE dependency)
- A boot image — either a full application image or a minimal bootstrap image plus application code loaded at startup
- External configuration (environment variables, config files, command-line arguments)
- Secrets and operational authority injected as capability tokens at startup — not baked into the image
- Logging and metrics surfaces suitable for unattended execution (structured log output, health endpoint, signal handling)

The image supervisor in headless mode spawns the application supervision tree directly, with no IDE subtree. Operational authority (filesystem access, network sockets, database connections) is injected via capabilities at startup by an operator-controlled configuration layer. Nothing in the image has ambient access to external resources — the deployment environment grants exactly the authority the application is configured to need.

---

## 14. Tooling Model

The native IDE is both a polished application and a message-addressable operational surface.

- **Workspace** — evaluate expressions in the live image
- **System Browser** — browse and edit classes, methods, protocols, and hierarchy
- **Inspector** — drill into live objects and actor-local state; edit instance variables directly
- **Debugger** — suspend any actor, walk the stack, resume or restart
- **Actor Monitor** — visualize the actor tree, mailbox depths, restart behavior, message rates, scheduler utilization per core
- **Source export/import** — serialize and restore source outside the image
- **Image save/load** — from within the IDE

### 14.1 IDE as a message surface

Where possible, IDE operations should also exist as message-driven services. A human using native UI, a script, another actor, or a future agent should be able to invoke the same operations: `browseClass:`, `inspect:`, `evaluate:`, `addMethod:to:`. This preserves conceptual integrity without forcing the UI into abstraction.

### 14.2 Live update and class evolution semantics

"Live editing works" is a promise that needs explicit implementation rules, especially around in-flight message activations. These rules must be decided before the system browser is built.

**Method replacement:**
- Compiled method objects are immutable once created
- Installing a new method atomically updates the class's method dictionary entry
- Existing activations (stack frames) continue executing using the old compiled method object — they hold a direct reference to it
- Future sends (after the atomic update) use the new method object
- This is safe and requires no coordination with running actors

**Structural changes (instance variable layout changes):**
- Adding or removing instance variables changes the object layout
- Existing instances have the old layout; new instances use the new layout
- A migration step is required: either lazy (migrate on first access) or eager (walk the actor's heap and migrate all instances)
- Structural changes require the affected actor to be at a safe point — send it a migration message rather than interrupting mid-execution
- This is more complex than method replacement and should be designed carefully in Phase 3

**Class addition:**
- New classes may be installed at any time with no coordination required
- They become visible to future method lookups immediately

**Class removal:**
- Removing a class while live instances exist is undefined without explicit migration
- Early versions should treat class removal as an error if live instances exist

**Default stance:** method replacement is always safe and immediate. Structural changes require explicit migration. The system browser UI should make this distinction visible — method edits are "hot" (immediate), structural edits prompt for migration strategy.

### 14.3 Source export, file sync, and external editor support

The live image is the truth. Files are a projection. This section defines how those two representations coexist without either being second-class.

**The keybinding problem is already solved.** The IDE uses native macOS text widgets throughout. A native `NSTextView`-backed editor gets macOS system keybindings for free — including the emacs bindings built into every native macOS text field (`ctrl+a`, `ctrl+e`, `ctrl+k`, `ctrl+n`, `ctrl+p`, and the rest). Vim modal editing works via system-level tools. This is a direct consequence of using SwiftUI rather than drawing a custom canvas. The complaint that Squeak and Pharo feel alien to developers with years of editor muscle memory does not apply here.

**What files can and cannot represent.** Source export covers the *code* portion of the image — class definitions, method source, protocol organisation. It does not and cannot represent runtime state: live actor populations, objects created at the REPL, in-flight sessions, the supervision tree as it currently exists. This is a feature, not a limitation. The file representation is always clean — just code, no runtime noise. Git history is legible. Code review in any standard tool works normally.

**Source export format.** Classes and methods are serialized to `.st` files in a structured, git-friendly format — one file per class, human-readable, diffable. This is generated from the image on demand and can be re-ingested. The format is defined by this project; it does not attempt binary compatibility with Squeak or Pharo image formats. Design goals for the format:

- One class per file, named `ClassName.st`
- Methods grouped by protocol within the file
- Human-readable without tooling — a developer should be able to read and understand a `.st` file with no knowledge of the runtime
- Round-trippable — export followed by import produces an identical class definition
- Diff-friendly — method additions and changes produce minimal, readable diffs

**The file sync actor.** An optional system actor — `FileSyncActor` — provides bidirectional sync between a designated source directory and the live image. It is not special infrastructure. It is an ordinary actor in the system that happens to watch the filesystem and send IDE messages.

```
FileSyncActor
  watches:  ~/projects/myapp/src/
  on change to CartService.st:
    → sends addMethod:to: or defineClass: to IDE actor
    → change appears in live image within milliseconds

  on method change in live image:
    → serializes updated class to CartService.st
    → file watcher ignores its own writes (no loop)
```

The sync actor uses exactly the same IDE message API (`addMethod:to:`, `defineClass:`, `removeMethod:from:`) as the system browser, as scripts, and as future agents. There is no privileged path. A developer who wants to work in Emacs, Zed, or any other editor opens the source directory in their editor of choice, edits `.st` files, and sees changes reflected in the running image in real time. A developer who prefers the live browser works entirely in the IDE and optionally exports to files for version control. Both workflows are first-class.

**What this means for version control.** The source directory is a normal git repository. `git diff`, pull requests, and code review in GitHub or any other tool work without special plugins or extensions. Committing a change is: edit in browser or external editor → change appears in source directory → `git commit`. The image itself is not version-controlled (it is large, binary, and contains runtime state); only the source export is. The image is reconstructible from source at any time via bootstrap + file-in.

**Workflow summary:**

| Developer preference | Primary editing surface | How changes reach the image |
|---|---|---|
| Live browser (Smalltalk-native) | System browser in the IDE | Immediate — browser sends `addMethod:to:` directly |
| External editor (Emacs, Zed, etc.) | `.st` files in source directory | FileSyncActor detects file change, sends IDE messages |
| Hybrid | Either, freely mixed | Both paths converge on the same IDE message API |

**The image vs. source distinction in one sentence:** the image is what is *running*; the source directory is what is *defined*. Runtime state lives only in the image. Code definitions live in both and are kept in sync by the FileSyncActor.

---

## 15. Non-Goals for Version 1

- No full cross-platform GUI target
- No transparent network distribution
- No autonomous hot-path use of LLMs
- No attempt to recreate the full surface area of Squeak, Pharo, or historical Morphic environments
- No promise that every speculative capability concept exists in the first usable release
- No claim of full Phoenix/OTP feature parity — but BEAM-class actor density and true preemptive concurrency **are** v1 requirements
- No requirement to ship a full web framework before the headless runtime is solid

---

## 16. Implementation Plan

### Phase 0 — Architectural spikes

- Prototype object representation, method lookup, and bytecode dispatch
- Prototype actor mailbox semantics (lock-free MPSC queue) and cross-actor copy behavior
- Spike per-actor heap allocation — validate 300-byte creation target is achievable
- Spike work-stealing scheduler with reduction-based preemption (no object system yet — pure C prototype)
- Spike async I/O integration (libuv wrapper)
- Spike image save/load for a closed-world subset
- Spike the native bridge between C runtime and SwiftUI tooling
- **Validate that activation frame layout supports tail call reuse** — TCO requires reusing the current frame rather than pushing a new one; this must be compatible with the frame header design, the GC's stack-walking strategy, and the reduction counter. Confirm the layout accommodates this before it is locked in.
- **Write down decisions before building permanent implementations**

### Phase 1 — Minimal live kernel

- Object memory, tagged immediates, classes and metaclasses
- Bytecode interpreter and primitive set
- Bootstrap kernel (the hard part — see Section 9)
- Basic expression evaluation in a live image
- Image save and load

### Phase 2 — Actor runtime + headless

- Actor creation, mailbox, asynchronous send, ask/future
- Per-actor mutable heaps with BEAM-density target
- Work-stealing multi-core scheduler with reduction-based preemption
- Supervision basics (restart, escalate, stop)
- Cross-actor copy/transfer semantics
- Isolation guarantees and diagnostics
- Async I/O substrate (libuv integration)
- Headless runtime lifecycle and service bootstrapping
- Concurrent GC (per-actor nursery + concurrent old-space collection)

### Phase 3 — Native IDE

- Workspace and transcript
- System browser (live editing — method changes take effect in running image)
- Inspector
- Debugger
- Actor monitor (per-core scheduler utilization, mailbox depths, supervision tree)
- Image save/load from IDE

### Phase 4 — Capability substrate

- Capability manifest model
- Registry actor
- Exact lookup
- Capability-gated resource access for selected subsystems
- Attenuation experiments

### Phase 5 — Semantic tools

- Embeddings over methods, classes, and capabilities
- Semantic search in the IDE
- Suggestion-oriented discovery for "find something that does X" queries
- Cold-path reasoning assistance outside the runtime hot path

### Phase 6 — Experimental lab

- Composition proposals
- Validation and promotion experiments
- Cross-image registry or federation experiments
- Simulation and sandbox workspaces for speculative composition

---

## 17. Key Risks and Design Traps

| Risk area | Why it matters | Mitigation |
|---|---|---|
| **GIL creep** | It is easy to accidentally introduce a global lock that becomes the new GIL — e.g. a non-concurrent symbol table or a single method cache. | Design concurrent data structures for the symbol table, class table, and method cache from the start. Audit every global data structure before Phase 2. |
| **Bootstrapping underestimated** | The metaclass circularity has blocked every Smalltalk implementor who encountered it unprepared. | Read the Blue Book before writing bootstrap code. Budget significant time. The difficulty is not conceptual but it is tedious and unforgiving. |
| **Actor density target drifting** | Without a concrete per-actor byte target, implementation decisions will silently inflate actor cost. | Measure creation cost from Phase 0 spike. Treat 300-byte baseline as a failing test if exceeded without justification. |
| **Async I/O deferred too long** | Adding async I/O after the scheduler is built requires retrofitting. | Include it in Phase 2 alongside the scheduler, not after. |
| **Overclaiming the synthesis** | Elegant rhetoric can hide unresolved interaction design questions. | State ambition clearly but distinguish foundational work from research-like exploration. |
| **Capability purity blocking progress** | A perfect security story can stall implementation. | Adopt capability-friendly boundaries early, then deepen enforcement over time. |
| **Speculative features swallowing the core** | Semantic and LLM layers can consume effort before the kernel is compelling. | Delay Layers 3–5 until the live system, actor model, and tooling are excellent. |
| **Public API boundary erosion** | Convenience shortcuts from the IDE into VM internals accumulate silently, making future open-sourcing or third-party embedding difficult. | Enforce the rule: the IDE uses only `sta/vm.h`. Any need to reach into internals is a signal to extend the public API properly. |

---

## 18. Success Criteria

- A user can develop inside a live image and meaningfully prefer it to a static edit-compile-run loop for certain classes of work.
- The runtime demonstrates that large actor populations are practical, measurable, and operationally useful — actor cost is low enough for service-style concurrency rather than thread-per-unit designs.
- The system uses all available CPU cores with no GIL bottleneck.
- The same runtime launches headlessly, supervises unattended workloads, and hosts service processes without requiring the IDE.
- Actor isolation is real, understandable, and visible in tooling.
- The IDE feels modern and native rather than historically preserved.
- Capabilities improve security and composition without turning ordinary development into ceremony.
- The system remains coherent and worthwhile even if the most speculative future layers are never built.

---

## 19. Source Compatibility and Migration

This section defines what developers can bring with them from existing Smalltalk systems and what they leave behind. Being honest about this is important for setting expectations and for making the project useful to the existing Smalltalk community.

### What is compatible

**Pure logic code — very high compatibility.** Any Smalltalk code that deals with objects, messages, collections, arithmetic, strings, blocks, and control flow is standard Smalltalk and will work as-is or with trivial changes. Sorting algorithms, parsers, domain models, business logic — this class of code ports directly. It is the majority of most applications.

**Class library code — moderate compatibility.** The algorithmic core of most Smalltalk libraries is portable. The parts that touch platform-specific infrastructure — Squeak's file streams, Pharo's networking libraries, Morphic UI — need to be rewritten against this system's equivalents. The logic inside those classes is fine; the integration points are not.

**`.st` source file import — planned tool.** Standard Smalltalk chunk-format source files (`.st`) are plain text. A file-in importer that reads `.st` source and loads classes into a live image is a realistic Phase 3 or 4 tool. It will not be 100% — anything referencing Squeak/Pharo-specific classes will need manual adjustment — but for pure logic classes it can be largely automatic. This is an explicit roadmap item.

### What is not compatible

**Binary image files (`.image`)** — not compatible and not worth attempting. The image format is completely different by design.

**Morphic or eToys code** — not compatible. Morphic is explicitly excluded (see non-goals). Code that depends on Morphic's widget hierarchy needs to be rewritten against the native SwiftUI bridge.

**Squeak/Pharo-specific libraries** — not compatible without porting. Code that depends on Pharo's Zinc HTTP library, Squeak's Socket class, or other platform-specific packages needs to be reimplemented. The logic inside those packages is portable; the infrastructure they build on is not.

### The migration story in one sentence

Bring your Smalltalk knowledge and your pure logic code. Leave behind the platform-specific infrastructure and expect to rewrite the wiring — but not the thinking.

---

## 20. Closing Position

This is a serious but survivable passion project if success is defined correctly. The strongest version of Smalltalk/A is not "everything noble, all at once." It is a disciplined C-core runtime with BEAM-class concurrency running standard Smalltalk, wrapped in a native macOS environment, deployable headlessly as a production runtime. If that core succeeds, the more ambitious capability, semantic, and federated ideas can be explored as real experiments rather than architectural obligations.

The honest competitive position: true multi-core execution and lightweight actor economics are central to the project's credibility. If the runtime cannot demonstrate that actors are cheap enough for large-scale service-style concurrency, and cannot avoid global bottlenecks, much of the motivation for building this instead of writing Elixir with a nicer IDE weakens substantially. Those two properties — actor density and true parallelism — are what distinguish this from "standard Smalltalk with a better UI."

---

## Appendix A: Key Design Decisions

| Decision | Choice | Rationale |
|---|---|---|
| VM implementation language | C | Full memory control, no ARC/GC interference, portable, required for BEAM-density actor heaps |
| Language | Standard Smalltalk (ANSI target) | The language is Smalltalk — not "inspired by" it. Blue Book is the reference. No new syntax, no dropped protocols. |
| IDE language | Swift / SwiftUI | Native macOS, clean C FFI, modern ergonomics |
| Concurrency model | Actor model, BEAM-inspired | Proven at scale; eliminates shared mutable state by construction |
| Scheduler | Work-stealing, one thread per core | True parallelism; no GIL; auto load balancing |
| Preemption | Reduction-based | No actor can starve the scheduler; consistent latency |
| GC strategy | Per-actor nursery + progressive old-space concurrency | Actor isolation makes per-actor minor GC trivial; major GC starts simple (brief stop-the-world), becomes more concurrent over time |
| Async I/O | libuv (initial), custom later | Production-proven; scheduler threads never block on I/O |
| Cross-actor message semantics | Deep copy (default) | Simplest correct rule; enforces good actor design |
| Security model | Capability-based | Structural security; no ambient authority |
| Bootstrap strategy | Manual C wiring → kernel image | Unavoidable for any Smalltalk; one-time cost |
| Repository structure | Two repos: `smalltalk-a-vm` (C/CMake) + `smalltalk-a-ide` (Swift/Xcode) | Enforces dependency direction; enables independent open-sourcing; Linux CI never sees Xcode |
| Public API design | Minimal stable `sta/vm.h`; IDE uses only this | Designed for potential open-sourcing; enables third-party embedders and alternative IDEs |
| Toolchain | Emacs + clangd + CMake + terminal for C; Xcode for Swift | Right tool for each layer; no Xcode required for VM work |
| Tail call elimination (TCO) | Detect `send` + `returnTop` in bytecode dispatch; reuse current frame | ~6% of static sends are tail calls; up to 21% dynamically in compiler-heavy workloads (Ralston & Mason 2019). Dramatic speedups for recursive workloads; modest but real gains for real-world code. Cheap to implement from day one — one-instruction lookahead in the dispatch loop. Disable per-actor when a debug capability is attached (flag already checked each quantum for reduction counting). Frame layout must support reuse; validated in Phase 0. |

## Appendix B: What Classical Smalltalk Got Right (Keep)

- Everything meaningful is an object — the programming model is uniformly object-oriented; runtime implementation techniques (tagged immediates, C primitives, host FFI) exist but do not fracture the language experience
- Live image — the system is always running; you develop inside it
- `doesNotUnderstand:` — proxy patterns and dynamic dispatch without special syntax
- `become:` — pointer swapping within an actor (scoped, safe)
- Metaclasses — classes are objects, introspectable and modifiable at runtime
- Blocks — closures as first-class objects

## Appendix C: What Classical Smalltalk Got Wrong (Fix)

| Classical Smalltalk | This System |
|---|---|
| Global interpreter lock (GIL) | No GIL; lock-free concurrent data structures |
| Cooperative green threads | Preemptive, reduction-based scheduling |
| Single-threaded; cannot use multiple cores | Work-stealing scheduler; one thread per core |
| Shared mutable heap | Per-actor isolated heaps |
| No concurrency model | First-class actor model at BEAM density |
| No security model | Capability-based security |
| Global namespace accessible to all | No ambient authority |
| IDE-only execution | Headless runtime for production deployment |

## Appendix D: Prior Art and Influences

| System | Contribution |
|---|---|
| Smalltalk-80 (Goldberg & Robson, 1983) | Object model, image, bytecode, metaclass architecture |
| Squeak / Pharo | Proof that image-based Smalltalk works on modern hardware. Three explicit cautionary examples: (1) GIL and cooperative green threads make true parallelism impossible; (2) the VM is written in Slang (a Smalltalk subset) that transpiles to C — the C is generated output, not authored code, making it hard to read, hard to contribute to, and architecturally entangled with the image; (3) there is no embeddable public C API — the VM and IDE are a monolithic application. This project explicitly inverts all three. |
| Erlang/OTP / BEAM | Actor model, supervision trees, reduction scheduling, actor density target |
| E language | Capability-based security model |
| KeyKOS | Earliest production capability OS |
| Self | Prototype-based Smalltalk; VM optimization techniques |
| Node.js / libuv | Async I/O event loop model |
| Alan Kay (ARPA/PARC) | The biological metaphor; objects as autonomous communicating agents |
