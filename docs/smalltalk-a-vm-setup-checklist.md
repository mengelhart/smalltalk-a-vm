# Smalltalk/A VM — Repository Setup Checklist
## For: `smalltalk-a-vm` (C runtime repo)
## Target: Claude Code in terminal

This checklist covers Phase 0 repository scaffolding only.
No implementation code is written here — only the build system,
directory structure, public API skeleton, and tooling configuration.
Stop after each section and verify before continuing.

---

## Section 1 — Create the repository

- [ ] Create a new directory: `smalltalk-a-vm`
- [ ] `cd smalltalk-a-vm && git init`
- [ ] Create a `.gitignore` with entries for:
  - `build/`
  - `compile_commands.json` (at root — symlink target, not source-controlled)
  - `.cache/`
  - `*.o`, `*.a`, `*.dylib`, `*.so`
  - `.DS_Store`
- [ ] Create an empty initial commit: `git commit --allow-empty -m "Initial commit"`

---

## Section 2 — Directory structure

Create the following directories. Each should contain a `.gitkeep`
file so they are tracked by git before any real files exist.

```
smalltalk-a-vm/
    include/
        sta/               ← public headers only; this is the stable contract
    src/
        vm/                ← object memory, interpreter, method dispatch
        actor/             ← actor struct, mailbox, lifecycle
        gc/                ← garbage collector
        scheduler/         ← work-stealing scheduler, reduction counter
        io/                ← async I/O substrate (libuv integration)
        bootstrap/         ← one-time kernel bootstrap
    tests/                 ← CTest-based test suite
    examples/
        embed_basic/       ← minimal embedding example; permanent API smoke test
    docs/                  ← design notes, decision records
    CLAUDE.md              ← context file for Claude Code (see below)
```

- [ ] Create all directories listed above
- [ ] Add a `.gitkeep` to each empty directory
- [ ] Verify the full tree with `find . -not -path './.git/*' | sort`

---

## Section 3 — Root CMakeLists.txt

Create `CMakeLists.txt` at the repo root with the following requirements.
Do not implement any library code yet — this file only needs to build
cleanly with an empty source tree.

Requirements:
- [ ] Minimum CMake version: 3.20
- [ ] Project name: `smalltalk-a-vm`, language: C, C standard: C17
- [ ] Set `CMAKE_C_STANDARD_REQUIRED ON` and `CMAKE_C_EXTENSIONS OFF`
- [ ] Enable `CMAKE_EXPORT_COMPILE_COMMANDS ON` — required for clangd
- [ ] Define a static library target `sta_vm` (initially with no source files — use an interface target or placeholder)
- [ ] Set include path: `include/` is public; `src/` is private
- [ ] Add a compiler warning set for both Clang and GCC:
  - `-Wall -Wextra -Wpedantic -Werror`
  - `-Wno-unused-parameter` (acceptable during early development)
- [ ] Add an install target that installs:
  - The static library to `lib/`
  - `include/sta/vm.h` to `include/sta/`
  - Nothing else — `src/` headers are never installed
- [ ] Add subdirectory includes for `tests/` and `examples/embed_basic/`
- [ ] Verify: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build` completes without errors

---

## Section 4 — clangd configuration

- [ ] After running cmake, create a symlink at the repo root:
  `ln -s build/compile_commands.json compile_commands.json`
- [ ] Create `.clangd` at the repo root with content:
  ```yaml
  CompileFlags:
    CompilationDatabase: .
  Diagnostics:
    UnusedIncludes: Strict
  ```
- [ ] Verify clangd can find the compilation database:
  `clangd --check=include/sta/vm.h` should run without "no compile commands" errors

---

## Section 5 — Public API header skeleton

Create `include/sta/vm.h`. This is the only header external consumers
(including the future Swift IDE) will ever include. Keep it minimal.
This is a skeleton — no implementation exists yet.

Requirements:
- [ ] Standard include guard: `#ifndef STA_VM_H` / `#define STA_VM_H`
- [ ] `#ifdef __cplusplus extern "C" {` guard for future Swift/C++ FFI use
- [ ] Forward-declare opaque types (no struct bodies exposed):
  - `typedef struct STA_VM    STA_VM;`
  - `typedef struct STA_Actor STA_Actor;`
  - `typedef struct STA_Handle STA_Handle;`  ← opaque handle (preferred over raw OOP)
- [ ] Define `STA_OOP` as `typedef uintptr_t STA_OOP;` with a comment
  noting this is for internal/illustrative use; external callers use `STA_Handle*`
- [ ] Define a minimal `STA_VMConfig` struct with:
  - `int scheduler_threads;`  ← 0 = use CPU count
  - `size_t initial_heap_bytes;`
  - `const char* image_path;`  ← NULL = start fresh
- [ ] Declare (but do not implement) these lifecycle functions:
  - `STA_VM*  sta_vm_create(const STA_VMConfig* config);`
  - `void     sta_vm_destroy(STA_VM* vm);`
  - `int      sta_vm_load_image(STA_VM* vm, const char* path);`
  - `int      sta_vm_save_image(STA_VM* vm, const char* path);`
- [ ] Declare actor interface stubs:
  - `STA_Actor* sta_actor_spawn(STA_VM* vm, STA_Handle* class_handle);`
  - `int        sta_actor_send(STA_VM* vm, STA_Actor* actor, STA_Handle* message);`
- [ ] Declare handle lifecycle stubs:
  - `void       sta_handle_release(STA_VM* vm, STA_Handle* handle);`
- [ ] Add a comment block at the top of the file explaining:
  - This is the only public header
  - All implementation is in `src/` (private)
  - The IDE uses only this file — no privileged back-channel
- [ ] Verify the header compiles cleanly:
  `clang -std=c17 -Wall -Wextra -Wpedantic -Iinclude -c /dev/null -include include/sta/vm.h`

---

## Section 6 — Stub implementation files

Create minimal stub `.c` files so the build system has something to
compile. These contain no real logic — only the function signatures
returning safe no-op values (NULL, 0, -1 as appropriate).

- [ ] `src/vm/vm.c` — stub implementations of `sta_vm_create`, `sta_vm_destroy`,
  `sta_vm_load_image`, `sta_vm_save_image`
- [ ] `src/actor/actor.c` — stub implementations of `sta_actor_spawn`, `sta_actor_send`
- [ ] `src/vm/handle.c` — stub implementation of `sta_handle_release`
- [ ] Update `CMakeLists.txt` to add these source files to the `sta_vm` target
- [ ] Verify: `cmake --build build` compiles cleanly with `-Werror`

---

## Section 7 — Minimal embedding example

Create `examples/embed_basic/main.c`. This file serves two purposes:
(1) documents the intended public API usage, and (2) is a permanent
smoke test that the public API compiles and links.

Requirements:
- [ ] Include only `<sta/vm.h>` — no internal headers
- [ ] Create a VM with default config
- [ ] Attempt to destroy it (even though create returns NULL for now)
- [ ] Print a "smoke test passed" message to stdout
- [ ] Create `examples/embed_basic/CMakeLists.txt` that:
  - Defines an executable target `embed_basic`
  - Links against `sta_vm`
  - Uses only the public include path
- [ ] Verify: `cmake --build build && ./build/examples/embed_basic/embed_basic`
  compiles and runs (even if it just prints the message and exits)

---

## Section 8 — Test scaffolding

- [ ] Create `tests/CMakeLists.txt` that enables CTest:
  `enable_testing()`
- [ ] Create `tests/test_public_api.c` with a single test:
  - Include `<sta/vm.h>` only
  - Call `sta_vm_create(NULL)` — expect NULL return (stub)
  - Call `sta_vm_destroy(NULL)` — expect no crash
  - Exit 0 on success
- [ ] Add the test to CTest in `tests/CMakeLists.txt`
- [ ] Verify: `cd build && ctest --output-on-failure` passes

---

## Section 9 — README and docs

- [ ] Create `README.md` at root with:
  - Project name: Smalltalk/A VM (`smalltalk-a-vm`)
  - One-sentence description from the architecture doc
  - Build instructions (cmake + make)
  - Note that this is the C runtime only; IDE lives in `smalltalk-a-ide`
  - Note on licensing: TBD before public release
- [ ] Create `docs/decisions/` directory
- [ ] Create `docs/decisions/001-implementation-language.md` with a brief
  record of the C language decision and rationale (from architecture doc section 4)
- [ ] Create `docs/decisions/002-public-api-boundary.md` recording the
  "IDE uses only sta/vm.h" rule and why it matters

---

## Section 10 — CLAUDE.md

Create `CLAUDE.md` at the repo root. This file is read by Claude Code
at session start and provides essential project context.

- [ ] Create `CLAUDE.md` with the following sections (content described below)

### What to put in CLAUDE.md

**Keep it focused and operational.** CLAUDE.md is not a copy of the
architecture document — it is a quick-reference for an AI coding
assistant that needs to make correct local decisions without
re-reading 1000 lines of prose. Link to the architecture doc for
deep reference; summarise only what affects day-to-day coding decisions.

Recommended sections:

```markdown
# Smalltalk/A VM — Claude Code Context

## What this repo is
C runtime for Smalltalk/A. See docs/architecture/ for full design.
This repo contains only the VM. The Swift IDE lives in smalltalk-a-ide (separate repo).

## Hard rules — never violate these
- The public API is `include/sta/vm.h` ONLY. Never add internal headers to include/.
- `src/` headers are private. Never include them from examples/ or tests/.
- The IDE (when it exists) uses only sta/vm.h. No privileged back-channel.
- No GIL. Every global data structure must be designed for concurrent access.
- Per-actor heap target: ~300 bytes at creation. Measure it. Treat drift as a bug.
- C17 only. No GNU extensions. `-std=c17 -Wall -Wextra -Wpedantic -Werror`.
- CMake is the build system. Do not create Makefiles or Xcode projects in this repo.

## Current phase
Phase 0 — architectural spikes. No permanent implementation yet.
Spike, validate, write down the decision, then implement.

## Key architectural decisions
- VM language: C (full memory control, no ARC/GC interference, portable)
- Concurrency: BEAM-style, one scheduler thread per CPU core, work-stealing
- Preemption: reduction-based (no actor can starve the scheduler)
- GC: per-actor nursery + progressive old-space; NO stop-the-world GIL
- Async I/O: libuv (initial); scheduler threads must NEVER block on I/O
- Cross-actor messages: deep copy semantics by default
- Language: standard Smalltalk (Blue Book is the reference); not "Smalltalk-inspired"

## Architecture reference
Full design: docs/architecture/smalltalk-a-vision-architecture-v3.md

## Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build && ctest --output-on-failure

## File layout
include/sta/vm.h     ← public API (the only contract)
src/vm/              ← object memory, interpreter
src/actor/           ← actor struct, mailbox, lifecycle
src/gc/              ← garbage collector
src/scheduler/       ← work-stealing scheduler
src/io/              ← async I/O (libuv)
src/bootstrap/       ← one-time kernel bootstrap
tests/               ← CTest suite
examples/embed_basic ← public API smoke test
docs/decisions/      ← architectural decision records
```

---

## Section 11 — Architecture document

Copy the master architecture document into the repo so Claude Code can
reference it locally without needing external access.

- [ ] Create `docs/architecture/` directory
- [ ] Copy `smalltalk-a-vision-architecture-v3.md` into
  `docs/architecture/smalltalk-a-vision-architecture-v3.md`
- [ ] Add a note in CLAUDE.md pointing to this path (already included in
  the CLAUDE.md template above)

---

## Final verification checklist

Run these checks before declaring the scaffold complete:

- [ ] `cmake -B build -DCMAKE_BUILD_TYPE=Debug` — exits 0
- [ ] `cmake --build build` — exits 0, zero warnings
- [ ] `cd build && ctest --output-on-failure` — all tests pass
- [ ] `./build/examples/embed_basic/embed_basic` — prints smoke test message
- [ ] `clangd --check=include/sta/vm.h` — no errors
- [ ] `git status` — only intentional untracked files
- [ ] `git log --oneline` — at least 2 commits (initial + scaffold)
- [ ] `find include/ -name "*.h" | wc -l` — exactly 1 (only vm.h)
- [ ] `find src/ -name "*.h"` — internal headers only; none in include/

---

## What comes next (Phase 0 spikes — not in this checklist)

Once the scaffold is verified, the first spikes begin:
1. OOP tagged integer representation and ObjHeader layout in `src/vm/`
2. Lock-free MPSC mailbox prototype in `src/actor/`
3. Per-actor heap allocator spike — validate ~300 byte creation target
4. Work-stealing scheduler skeleton in `src/scheduler/`
5. libuv async I/O wrapper stub in `src/io/`

Each spike gets a decision record in `docs/decisions/` before any code
is committed to a non-spike branch.
