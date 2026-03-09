# Smalltalk/A VM — Repository Setup Checklist
## For: `smalltalk-a-vm` (C runtime repo)
## Target: Claude Code in terminal
## Environment: macOS Tahoe, Xcode 26.3, Apple clang 17, CMake 4.2.3, arm64 M4 Max

This checklist covers Phase 0 repository scaffolding only.
No implementation code is written here — only the build system,
directory structure, public API skeleton, tooling configuration,
and decision records.

Stop after each section and verify before continuing.

---

## Section 1 — Create the repository

- [ ] Create a new directory: `smalltalk-a-vm`
- [ ] `cd smalltalk-a-vm && git init`
- [ ] Create `.gitignore` with the following content:

```gitignore
# =============================================================================
# Smalltalk/A VM — .gitignore
# =============================================================================

# CMake build artifacts
build/
cmake-build-*/
CMakeCache.txt
CMakeFiles/
cmake_install.cmake
install_manifest.txt
CTestTestfile.cmake
_deps/

# compile_commands.json — lives in build/, symlinked to root, not tracked
compile_commands.json

# Compiled output
*.o
*.a
*.dylib
*.so
*.so.*
*.out

# clangd cache
.cache/
.clangd/

# macOS
.DS_Store
.DS_Store?
._*
.Spotlight-V100
.Trashes

# Xcode (belt-and-suspenders — should not appear in VM repo)
*.xcodeproj/
*.xcworkspace/
xcuserdata/
DerivedData/

# Emacs
*~
\#*\#
.\#*

# Vim / Neovim
*.swp
*.swo

# VS Code / Cursor
.vscode/

# lldb / Instruments
*.dSYM/
*.trace/

# CTest output
Testing/
LastTest.log
CTestCostData.txt

# Dependency managers
vcpkg_installed/
conan/

# Crash reports
*.crash

# Scratch — safe for throwaway spike work inside the repo
scratch/
```

- [ ] `git add .gitignore && git commit -m "Add .gitignore"`

---

## Section 2 — Directory structure

Create the following directories. Each should contain a `.gitkeep`
so they are tracked before any real files exist.

```
smalltalk-a-vm/
    include/
        sta/                ← public headers ONLY — the stable external contract
    src/
        vm/                 ← object memory, interpreter, method dispatch
        actor/              ← actor struct, mailbox, lifecycle
        gc/                 ← garbage collector
        scheduler/          ← work-stealing scheduler, reduction counter
        io/                 ← async I/O substrate (libuv, Phase 2)
        bootstrap/          ← one-time kernel bootstrap
    tests/                  ← CTest-based test suite
    examples/
        embed_basic/        ← minimal embedding example; permanent API smoke test
    docs/
        architecture/       ← master architecture document
        decisions/          ← architectural decision records (ADRs)
    CLAUDE.md
```

**Internal header convention — enforced from day one:**
Internal `.h` files live beside their `.c` files in `src/`.
`src/actor/actor.h` pairs with `src/actor/actor.c`.
There is no `src/include/` or `src/internal/` directory.
`include/` contains only public API headers — nothing else.
This is ADR 003.

- [ ] Create all directories above with `.gitkeep` files
- [ ] Verify: `find . -not -path './.git/*' | sort`

---

## Section 3 — Stub source files

Create these stub `.c` files now — before CMakeLists.txt — so the
library target is real and concrete from the start. Each file contains
only the function signatures returning safe no-op values.

**`src/vm/vm.c`:**
```c
#include "sta/vm.h"

STA_VM* sta_vm_create(const STA_VMConfig* config) {
    (void)config;
    return NULL;
}

void sta_vm_destroy(STA_VM* vm) {
    (void)vm;
}

int sta_vm_load_image(STA_VM* vm, const char* path) {
    (void)vm; (void)path;
    return STA_ERR_INTERNAL;
}

int sta_vm_save_image(STA_VM* vm, const char* path) {
    (void)vm; (void)path;
    return STA_ERR_INTERNAL;
}

const char* sta_vm_last_error(STA_VM* vm) {
    (void)vm;
    return "";
}
```

**`src/actor/actor.c`:**
```c
#include "sta/vm.h"

STA_Actor* sta_actor_spawn(STA_VM* vm, STA_Handle* class_handle) {
    (void)vm; (void)class_handle;
    return NULL;
}

int sta_actor_send(STA_VM* vm, STA_Actor* actor, STA_Handle* message) {
    (void)vm; (void)actor; (void)message;
    return STA_ERR_INTERNAL;
}
```

**`src/vm/handle.c`:**
```c
#include "sta/vm.h"

STA_Handle* sta_handle_retain(STA_VM* vm, STA_Handle* handle) {
    (void)vm;
    return handle;
}

void sta_handle_release(STA_VM* vm, STA_Handle* handle) {
    (void)vm; (void)handle;
}
```

**`src/vm/eval.c`:**
```c
#include "sta/vm.h"

STA_Handle* sta_eval(STA_VM* vm, const char* expression) {
    (void)vm; (void)expression;
    return NULL;
}

STA_Handle* sta_inspect(STA_VM* vm, STA_Handle* object) {
    (void)vm; (void)object;
    return NULL;
}
```

- [ ] Create all four stub files as above

---

## Section 4 — Root CMakeLists.txt

Create `CMakeLists.txt` at the repo root.

Requirements:
- [ ] `cmake_minimum_required(VERSION 3.20...4.2)` — handles CMake 4.x policy changes
- [ ] Project: `smalltalk-a-vm`, language C, standard C17
- [ ] `set(CMAKE_C_STANDARD_REQUIRED ON)`
- [ ] `set(CMAKE_C_EXTENSIONS OFF)`
- [ ] `set(CMAKE_EXPORT_COMPILE_COMMANDS ON)`
- [ ] Static library target `sta_vm` with all four stub source files:
  - `src/vm/vm.c`
  - `src/vm/handle.c`
  - `src/vm/eval.c`
  - `src/actor/actor.c`
- [ ] Public include: `include/` (propagated to consumers via INTERFACE)
- [ ] Private include: `src/` (internal only, not propagated)
- [ ] Compiler warnings:
  ```cmake
  target_compile_options(sta_vm PRIVATE
      -Wall -Wextra -Wpedantic -Werror
      -Wno-unused-parameter)
  ```
- [ ] libuv hook — wired but OFF by default:
  ```cmake
  option(STA_USE_LIBUV "Enable libuv async I/O substrate (Phase 2)" OFF)
  if(STA_USE_LIBUV)
      find_package(libuv REQUIRED)
      target_link_libraries(sta_vm PRIVATE libuv::libuv)
      target_compile_definitions(sta_vm PRIVATE STA_USE_LIBUV=1)
  endif()
  ```
- [ ] Install target: library to `lib/`, `include/sta/vm.h` to `include/sta/`; nothing from `src/`
- [ ] `add_subdirectory(tests)`
- [ ] `add_subdirectory(examples/embed_basic)`
- [ ] Verify: `cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build` — zero warnings

---

## Section 5 — Public API header

Create `include/sta/vm.h`. This is the only header external consumers
ever include. The IDE will use exactly this file and nothing else.

```c
/*
 * sta/vm.h — Smalltalk/A VM public API
 *
 * This is the ONLY public header. All implementation is in src/ (private).
 * The Swift IDE uses only this file. There is no privileged back-channel.
 *
 * External callers use STA_Handle* for all object references.
 * Raw STA_OOP is for internal VM use only.
 *
 * Error convention: functions returning int use STA_OK (0) for success
 * and STA_ERR_* (negative) for failure. Use sta_vm_last_error() for
 * human-readable diagnostics.
 */

#ifndef STA_VM_H
#define STA_VM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque types
 * ---------------------------------------------------------------------- */

typedef struct STA_VM     STA_VM;
typedef struct STA_Actor  STA_Actor;
typedef struct STA_Handle STA_Handle;  /* preferred over raw OOP across FFI */
typedef uintptr_t         STA_OOP;    /* internal VM use only */

/* -------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------- */

#define STA_OK            0    /* success */
#define STA_ERR_INVALID  (-1)  /* bad argument or precondition */
#define STA_ERR_OOM      (-2)  /* allocation failure */
#define STA_ERR_IO       (-3)  /* I/O or image load/save failure */
#define STA_ERR_INTERNAL (-4)  /* unexpected internal error */

/* -------------------------------------------------------------------------
 * VM configuration
 * ---------------------------------------------------------------------- */

typedef struct STA_VMConfig {
    int         scheduler_threads;   /* 0 = use CPU core count */
    size_t      initial_heap_bytes;  /* 0 = use default */
    const char* image_path;          /* NULL = start fresh */
} STA_VMConfig;

/* -------------------------------------------------------------------------
 * VM lifecycle
 * ---------------------------------------------------------------------- */

STA_VM*     sta_vm_create(const STA_VMConfig* config);
void        sta_vm_destroy(STA_VM* vm);
int         sta_vm_load_image(STA_VM* vm, const char* path);
int         sta_vm_save_image(STA_VM* vm, const char* path);
const char* sta_vm_last_error(STA_VM* vm);  /* never NULL; "" if no error */

/* -------------------------------------------------------------------------
 * Actor interface
 * ---------------------------------------------------------------------- */

STA_Actor* sta_actor_spawn(STA_VM* vm, STA_Handle* class_handle);
int        sta_actor_send(STA_VM* vm, STA_Actor* actor, STA_Handle* message);

/* -------------------------------------------------------------------------
 * Handle lifecycle
 *
 * Handles are acquired per-operation (e.g. from sta_eval, sta_actor_spawn).
 * sta_handle_retain roots a handle across GC cycles.
 * sta_handle_release unroots it — every acquired handle must be released.
 * Handle leaks are real memory leaks.
 *
 * Full handle acquisition API is defined in Phase 0 spikes — see ADR 006.
 * ---------------------------------------------------------------------- */

STA_Handle* sta_handle_retain(STA_VM* vm, STA_Handle* handle);
void        sta_handle_release(STA_VM* vm, STA_Handle* handle);

/* -------------------------------------------------------------------------
 * Evaluation and inspection (used by IDE; available to all embedders)
 * ---------------------------------------------------------------------- */

STA_Handle* sta_eval(STA_VM* vm, const char* expression);
STA_Handle* sta_inspect(STA_VM* vm, STA_Handle* object);

#ifdef __cplusplus
}
#endif

#endif /* STA_VM_H */
```

- [ ] Create `include/sta/vm.h` with the content above
- [ ] Verify: `clang -std=c17 -Wall -Wextra -Wpedantic -Iinclude -c /dev/null -include include/sta/vm.h`
  compiles with zero warnings

---

## Section 6 — clangd configuration

- [ ] `ln -s build/compile_commands.json compile_commands.json`
- [ ] Create `.clangd`:
  ```yaml
  CompileFlags:
    CompilationDatabase: .
  Diagnostics:
    UnusedIncludes: Strict
  ```
- [ ] Verify: `clangd --check=include/sta/vm.h` — no "no compile commands" errors

---

## Section 7 — Embedding example

Create `examples/embed_basic/main.c`:

```c
#include <sta/vm.h>
#include <stdio.h>

int main(void) {
    STA_VMConfig config = {0};  /* all defaults */

    STA_VM* vm = sta_vm_create(&config);
    /* vm is NULL from stub — expected */

    const char* err = sta_vm_last_error(vm);
    printf("last_error (stub): \"%s\"\n", err);

    sta_vm_destroy(vm);  /* NULL-safe */

    printf("embed_basic: smoke test passed\n");
    return 0;
}
```

Create `examples/embed_basic/CMakeLists.txt`:
```cmake
add_executable(embed_basic main.c)
target_link_libraries(embed_basic PRIVATE sta_vm)
```

- [ ] Create both files
- [ ] Verify: `cmake --build build && ./build/examples/embed_basic/embed_basic`
  prints smoke test message

---

## Section 8 — Test scaffolding

Create `tests/CMakeLists.txt`:
```cmake
enable_testing()

add_executable(test_public_api test_public_api.c)
target_link_libraries(test_public_api PRIVATE sta_vm)

add_test(NAME test_public_api COMMAND test_public_api)
```

Create `tests/test_public_api.c`:
```c
#include <sta/vm.h>
#include <assert.h>
#include <stddef.h>

int main(void) {
    /* sta_vm_create with NULL config — stub returns NULL */
    STA_VM* vm = sta_vm_create(NULL);
    assert(vm == NULL);

    /* sta_vm_last_error — must return non-NULL string even with NULL vm */
    const char* err = sta_vm_last_error(NULL);
    assert(err != NULL);

    /* sta_vm_destroy — must not crash on NULL */
    sta_vm_destroy(NULL);

    /* sta_handle_release — must not crash on NULL */
    sta_handle_release(NULL, NULL);

    return 0;
}
```

- [ ] Create both files
- [ ] Verify: `cd build && ctest --output-on-failure` — all tests pass

---

## Section 9 — Decision records

Create these ADR files in `docs/decisions/`. Keep them short — the
purpose is to record *what* was decided and *why*, not to re-explain
the entire architecture.

- [ ] `docs/decisions/001-implementation-language.md`
  - Decision: C17 for VM/runtime; Swift/SwiftUI for IDE only
  - Rationale: full memory control; no ARC/GC interference; portable;
    required for BEAM-density actor heaps and no-GIL concurrent design
  - Reference: architecture doc Section 4

- [ ] `docs/decisions/002-public-api-boundary.md`
  - Decision: `include/sta/vm.h` is the only public header; IDE uses
    only this; no privileged back-channel exists or will be created
  - Rationale: the only reliable way to ensure the API is sufficient
    and non-leaky; enables future open sourcing and third-party embedders
  - Reference: architecture doc Section 4.2

- [ ] `docs/decisions/003-internal-header-convention.md`
  - Decision: internal `.h` files live beside their `.c` files in `src/`;
    no `src/include/` or `src/internal/` subdirectory
  - Rationale: simple, navigable, prevents ad hoc layout drift,
    clangd resolves correctly with the private `src/` include path
  - Enforcement: CMake private include path is `src/`; verify with
    `find include/ -name "*.h"` — must always return exactly one file

- [ ] `docs/decisions/004-live-update-semantics.md`
  - Status: DEFERRED to Phase 3
  - Decisions to record when Phase 3 begins:
    - Method replacement policy (atomic dict update; old activations complete)
    - Structural ivar change policy (safe-point + explicit migration)
    - Class addition policy (safe at any time)
    - Class removal policy (error if live instances exist — early versions)
  - Reference: architecture doc Section 14.2
  - Note: FileSyncActor conflict policy (simultaneous browser + file edit)
    also needs a decision record — add here when addressed

- [ ] `docs/decisions/005-api-error-reporting.md`
  - Decision: integer `STA_OK` / `STA_ERR_*` codes for all operations;
    `sta_vm_last_error(vm)` for human-readable diagnostics
  - Rationale: SQLite pattern — compact, no allocations on error path,
    clean C FFI, embeddable without exception handling
  - Richer structured errors are a Phase 4+ concern if needed

- [ ] `docs/decisions/006-handle-lifecycle.md`
  - Status: DEFERRED to Phase 0 spike
  - Question: opaque `STA_Handle*` rooted by runtime vs raw `STA_OOP`
    with explicit pinning
  - Recommended direction: `STA_Handle*` model (JNI/CPython pattern)
    — see architecture doc Section 4.3
  - Lock this decision before any IDE code is written against the API
  - Record full acquire/retain/release semantics here once spiked

---

## Section 10 — README

Create `README.md`:

```markdown
# smalltalk-a-vm

C17 runtime for [Smalltalk/A](docs/architecture/smalltalk-a-vision-architecture-v3.md) —
a modern Smalltalk system with BEAM-class actor concurrency, a live image,
and a native macOS IDE.

This repository contains the C runtime only.
The Swift IDE lives in `smalltalk-a-ide` (separate repository).

## Build

    cmake -B build -DCMAKE_BUILD_TYPE=Debug
    cmake --build build

## Test

    cd build && ctest --output-on-failure

## Environment

- macOS Tahoe · Xcode 26.3 · Apple clang 17 · CMake 4.2.3 · arm64
- C17, `-Wall -Wextra -Wpedantic -Werror`

## License

TBD before public release. Candidates: MIT/Apache 2.0 or LGPL.
```

---

## Section 11 — CLAUDE.md

Create `CLAUDE.md` at the repo root:

```markdown
# Smalltalk/A VM — Claude Code Context

## What this repo is
C17 runtime for Smalltalk/A. The Swift IDE lives in `smalltalk-a-ide` (separate repo).
Full architecture: `docs/architecture/smalltalk-a-vision-architecture-v3.md`
Decision records: `docs/decisions/` — read ADRs 001-006 before making structural changes.

## Environment
macOS Tahoe · Xcode 26.3 · Apple clang 17 · CMake 4.2.3 · arm64 M4 Max

## Hard rules — never violate these
- `include/sta/vm.h` is the ONLY public header. Never add anything else to `include/`.
- `src/` headers are private. Never include them from `examples/` or `tests/`.
- The IDE uses only `sta/vm.h`. No privileged back-channel, ever.
  If the IDE needs something the public API cannot express, extend the API — for everyone.
- No GIL. Every global data structure must be designed for concurrent access from day one.
- C17 only. `-std=c17 -Wall -Wextra -Wpedantic -Werror`. No GNU extensions.
- CMake is the build system. No Makefiles, no Xcode projects in this repo.
- Internal headers live beside their .c files in `src/`. See ADR 003.
- Error shape: `STA_OK` / `STA_ERR_*` integer codes + `sta_vm_last_error()`. See ADR 005.

## Actor density target
~300 bytes per actor at creation — inherited from BEAM, validated in Phase 0 spike.
Measure actor creation size from every relevant spike.
Drift from ~300 bytes must be explained in a decision record. Never silently ignored.
This is a design target and forcing function, not an automatic pass/fail during early spikes.

## Current phase
Phase 0 — architectural spikes. No permanent implementation yet.
Workflow: spike → measure → write decision record → implement.

## Key architectural decisions (summary — see ADRs and arch doc for full rationale)
- VM: C17, no GIL, concurrent data structures from day one
- Concurrency: work-stealing scheduler, one thread per CPU core
- Preemption: reduction-based (no actor can starve the scheduler)
- GC: per-actor nursery + progressive old-space
- Async I/O: libuv (Phase 2); CMake option STA_USE_LIBUV wired but OFF now
- Cross-actor messages: deep copy semantics by default
- Language: standard Smalltalk — Blue Book is the authoritative reference

## Build
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build && ctest --output-on-failure

## File layout
include/sta/vm.h        ← public API (only external contract)
src/vm/                 ← object memory, interpreter, handle, eval stubs
src/actor/              ← actor struct, mailbox, lifecycle stub
src/gc/                 ← GC (Phase 1+)
src/scheduler/          ← work-stealing scheduler (Phase 0 spike)
src/io/                 ← async I/O via libuv (Phase 2)
src/bootstrap/          ← one-time kernel bootstrap (Phase 1)
tests/                  ← CTest suite
examples/embed_basic/   ← public API smoke test
docs/architecture/      ← master architecture document
docs/decisions/         ← ADRs 001-006
```

---

## Section 12 — Architecture document

- [ ] Verify `docs/architecture/smalltalk-a-vision-architecture-v3.md` exists
- [ ] If not present: copy it in from your local docs location now
- [ ] `ls docs/architecture/` — should show the file

---

## Final verification

- [ ] `cmake -B build -DCMAKE_BUILD_TYPE=Debug` — exits 0
- [ ] `cmake --build build` — exits 0, zero warnings
- [ ] `cd build && ctest --output-on-failure` — all tests pass
- [ ] `./build/examples/embed_basic/embed_basic` — prints smoke test message
- [ ] `clangd --check=include/sta/vm.h` — no errors
- [ ] `find include/ -name "*.h" | wc -l` — exactly 1
- [ ] `find src/ -name "*.h"` — internal headers only; nothing leaked to `include/`
- [ ] `ls docs/decisions/` — six ADR files present (001 through 006)
- [ ] `git status` — only intentional untracked files
- [ ] `git log --oneline` — at least 2 commits

---

## What comes next (Phase 0 spikes)

1. OOP tagged integer representation and `ObjHeader` layout — measure struct size on arm64
2. Lock-free MPSC mailbox prototype — validate in isolation
3. Per-actor heap allocator spike — validate ~300 byte creation target; record in ADR
4. Work-stealing scheduler skeleton — pure C, no object system yet
5. libuv async I/O wrapper — enable `STA_USE_LIBUV`, wire a minimal event loop
6. Handle lifetime model — resolve ADR 006 before any IDE code touches the public API

Each spike: write decision record first, then implement.
