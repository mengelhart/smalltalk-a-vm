# ADR 003 — Internal Header Convention

**Status:** Accepted
**Date:** 2026-03-09

## Decision

Internal `.h` files live beside their `.c` files in `src/`. There is no
`src/include/` or `src/internal/` subdirectory. `include/` contains only
public API headers — nothing else ever goes there.

## Rationale

- Simple and navigable: `src/actor/actor.h` pairs with `src/actor/actor.c`
- Prevents ad hoc layout drift over time
- clangd resolves correctly with the private `src/` include path in CMake
- No ambiguity about what is public vs internal

## Enforcement

- CMake sets `target_include_directories(sta_vm PRIVATE src/)` — internal
  headers are never propagated to consumers
- `examples/` and `tests/` must only include `<sta/vm.h>` — never `"src/..."` paths
- Verify: `find include/ -name "*.h"` returns exactly one file at all times
