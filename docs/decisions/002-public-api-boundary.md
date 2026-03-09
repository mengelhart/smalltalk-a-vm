# ADR 002 — Public API Boundary

**Status:** Accepted
**Date:** 2026-03-09

## Decision

`include/sta/vm.h` is the **only** public header. The Swift IDE uses exactly
this file and nothing else. No privileged back-channel exists or will be
created. All internal headers live in `src/` and are never installed.

## Rationale

- The only reliable way to ensure the API is sufficient and non-leaky
- Any IDE need that cannot be expressed through `sta/vm.h` must extend the
  public API — available to all embedders equally
- Enables future open sourcing and third-party embedders without API surgery
- Forces disciplined separation between contract and implementation

## Enforcement

- CMake installs only `include/sta/vm.h` — nothing from `src/`
- `find include/ -name "*.h"` must always return exactly one file
- CI should fail if this count changes unexpectedly

## Reference

Architecture document Section 4.2.
