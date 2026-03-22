# Smalltalk/A VM — Claude Code Context

## What this repo is
C17 runtime for Smalltalk/A. The Swift IDE lives in `smalltalk-a-ide` (separate repo).
Full architecture: `docs/architecture/smalltalk-a-vision-architecture-v3.md`
Decision records: `docs/decisions/` — read all ADRs before making structural changes.

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
- Production `src/` files must not be modified for test diagnosis only.

## Context budget
This codebase is architecturally complex (bytecode interpreter, actor scheduler,
GC, lock-free data structures). Session quality degrades noticeably after
~250K tokens of context usage. Plan work to complete within that budget.
If approaching 250K, finish the current story, commit, and suggest starting
a fresh session rather than pushing further.

## Actor density targets (Phase 1)
- `STA_Actor` struct: **164 bytes**. Any addition requires a new ADR.
- Creation cost (struct + nursery + identity object): **308 bytes**.
- Execution cost (+ initial 512-byte stack segment): **820 bytes**.
- BEAM comparison: 2,704 bytes at spawn. Smalltalk/A is 3.3× more compact.
- The 300-byte forcing function served its purpose through Phase 0. Phase 1
  targets are the numbers above. See ADR 014.

## Current phase
Phase 2 — Actor Runtime and Headless.
Scheduler, supervision, async I/O, headless lifecycle.
Phase 0 spikes and Phase 1 (Minimal Live Kernel) are complete.

## Key architectural decisions (summary — see ADRs and arch doc for full rationale)
- VM: C17, no GIL, concurrent data structures from day one
- Concurrency: work-stealing scheduler, one thread per CPU core
- Preemption: reduction-based (no actor can starve the scheduler)
- GC: per-actor nursery + progressive old-space
- Async I/O: libuv (Phase 2); CMake option STA_USE_LIBUV wired but OFF now
- Cross-actor messages: deep copy semantics by default
- Language: standard Smalltalk — Blue Book is the authoritative reference

## Build and test
**Never cd into `build/`.** All commands run from the repo root. Always.

Build:

    cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

After moving or reordering functions within a file, always do a full clean rebuild
before testing. Incremental builds with partially-failed compilations can produce
stale binaries that mask the real issue:

    cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build

Run a single test by name (use `-R` regex filter):

    ctest --test-dir build --output-on-failure -R test_name_pattern

Run the full test suite:

    ctest --test-dir build --output-on-failure

**Testing new code — mandatory order:**
1. Build.
2. Run the NEW test in isolation first: `ctest --test-dir build --output-on-failure -R new_test_name`
3. Only after the new test passes, run the full suite: `ctest --test-dir build --output-on-failure`
4. Never run the full suite as the first validation of new code.

## Sanitizer builds
Some spike tests have `-fsanitize=thread` hardcoded in CMakeLists.txt.
TSan and ASan cannot be combined — clang will reject it.

For ASan runs, exclude the spike tests:

    cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" && \
      cmake --build build-asan
    ctest --test-dir build-asan --output-on-failure -E "_spike"

For TSan runs, use the normal build (spike tests already have TSan flags):

    ctest --test-dir build --output-on-failure

Do not attempt to combine ASan and TSan in the same build. Do not try to
"fix" the spike tests — they are Phase 0 exploratory code and will not
be modified.

## Debugging rule
Strict 3-attempt debug limit. If a test failure or bug is not resolved within
3 focused attempts, stop and report the situation to the human rather than
spiraling. Include what was tried and what was observed.

## File layout
```
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
docs/decisions/         ← ADRs 001-014
```

---

## Branching

Never commit directly to `main`. Every epic and every task gets its own branch.

```bash
# Phase 2 epics
git checkout -b phase2/epic-N-<short-name>

# Non-epic tasks (ADR revisions, tooling, doc fixes)
git checkout -b task/<short-name>
```

Naming conventions:
- `phase2/epic-N-<short-name>` — one branch per Phase 2 epic (e.g. `phase2/epic-7b-futures-wait`)
- `task/<short-name>` — for any non-epic work (e.g. `task/update-adr-007`)

Epic branches are merged to `main` via squash-merge PR after review.

---
## Pacing — pause between stories

After completing each story (tests passing, committed), STOP and report:
- What was completed
- Key design choices made
- Anything surprising or concerning
- Current test count

Then WAIT for human confirmation before starting the next story.
Do not ask "shall I continue?" and proceed on a yes — actually stop
and wait. This gives the human time to review, check context usage,
ask questions, or adjust direction.

Human can say "please continue" or "continue" which is your signal to move on with the plan.

---

## Issue tracking — GitHub

All task tracking uses GitHub Issues. No markdown TODOs. No local tracking files.
Use the `gh` CLI for all issue operations.

### Orientation — start here in a new session
```bash
gh issue list --milestone "Phase 2 — Actor Runtime and Headless" --state open
gh issue list --label "decision-pending"
```

### Labels in use
| Label | Meaning |
|---|---|
| `spike` | Architectural spike — Phase 0 exploratory work |
| `adr` | Architecture Decision Record |
| `decision-pending` | Open question that must be resolved before build |
| `architecture` | Architecture and design |
| `phase-0` through `phase-3` | Which phase this belongs to |
| `reconstructed` | Issue reconstructed from historical docs |

### Milestones in use
| Milestone | Scope |
|---|---|
| `Phase 0 — Architectural Spikes` | Spike → measure → ADR → implement |
| `Phase 1 — Minimal Live Kernel` | Object memory, interpreter, bootstrap, image save/load |
| `Phase 2 — Actor Runtime and Headless` | Scheduler, supervision, async I/O, headless lifecycle |
| `Phase 2.5 — Runtime Completion` | GC fix, mutable closures, class library scaling, async I/O, headless, multi-actor image |
| `Phase 3 — Native IDE` | Workspace, browser, inspector, debugger, actor monitor |

### Creating issues
```bash
gh issue create \
  --title "<title>" \
  --body "<detail>" \
  --label "<labels>" \
  --milestone "Phase 2 — Actor Runtime and Headless"
```

### Closing issues
```bash
gh issue close <number> --comment "<what was completed>"
```

---

## Session discipline — mandatory before ending any session

Work is NOT complete until pushed. Always finish with:

```bash
git add -A
git commit -m "<meaningful message>"
git pull --rebase
git push
git status    # must show clean and up to date
```

Then update GitHub issues to reflect current state:
```bash
# Close completed work
gh issue close <number> --comment "<what was completed>"

# Update in-progress work if session ends mid-epic
gh issue edit <number> --body "<updated status and where things stand>"
```

Never leave work stranded locally. Push before ending the session.

