# Smalltalk/A VM — Claude Code Context

## Issue tracking
Use `bd` (beads) for all task tracking. Run `bd ready --json` to see available work.
Do not use markdown TODOs — create a beads issue instead.

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
