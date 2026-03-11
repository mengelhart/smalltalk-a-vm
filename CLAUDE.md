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

## Actor density target
~300 bytes per actor at creation — inherited from BEAM, validated in Phase 0 spikes.
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
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build
cd build && ctest --output-on-failure
```

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
docs/decisions/         ← ADRs 001-009
```

---

## Branching

Never commit directly to `main`. Every spike and every task gets its own branch.

```bash
# Spikes
git checkout -b spike/004-frame-layout

# Non-spike tasks (ADR revisions, tooling, doc fixes)
git checkout -b task/<short-name>
```

Naming conventions:
- `spike/00N-<short-name>` — one branch per Phase 0 spike (e.g. `spike/004-frame-layout`)
- `task/<short-name>` — for any non-spike work (e.g. `task/update-adr-007`)

Phase 0 spike branches are not merged to `main` — spike code is exploratory and
clearly marked as such. The ADR is the deliverable, not the branch. Merging
decisions are made explicitly at the end of each phase.

---

## Issue tracking — GitHub

All task tracking uses GitHub Issues. No markdown TODOs. No local tracking files.
Use the `gh` CLI for all issue operations.

### Orientation — start here in a new session
```bash
gh issue list --milestone "Phase 0 — Architectural Spikes" --state open
gh issue list --label "decision-pending"
gh issue list --label "spike" --state open
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
| `Phase 3 — Native IDE` | Workspace, browser, inspector, debugger, actor monitor |

### Creating a spike epic and child issues
```bash
# Create the epic first — note the issue number it returns
gh issue create \
  --title "Phase 0 Spike 00N: <title>" \
  --body "<summary of goals, key questions, links to spike doc and ADR>" \
  --label "spike,phase-0" \
  --milestone "Phase 0 — Architectural Spikes"

# Create child issues referencing the epic number
gh issue create \
  --title "<child story title>" \
  --body "Part of #<epic-number>. <detail>" \
  --label "spike,phase-0" \
  --milestone "Phase 0 — Architectural Spikes"
```

### Creating a decision-pending issue
```bash
gh issue create \
  --title "<open question — exact wording from ADR open questions section>" \
  --body "From ADR 00N. Must be resolved before <component> is built. See docs/decisions/00N-*.md" \
  --label "decision-pending,phase-0" \
  --milestone "Phase 0 — Architectural Spikes"
```

### Closing issues on spike completion
```bash
gh issue close <number> --comment "Spike complete. ADR accepted: docs/decisions/00N-*.md"
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

# Update in-progress work if session ends mid-spike
gh issue edit <number> --body "<updated status and where things stand>"
```

Never leave work stranded locally. Push before ending the session.


