# ADR 001 — Implementation Language

**Status:** Accepted
**Date:** 2026-03-09

## Decision

The VM and runtime are implemented in C17. The Swift IDE lives in a separate
repository (`smalltalk-a-ide`) and interacts with the VM exclusively through
the public C API (`include/sta/vm.h`).

## Rationale

- Full manual memory control — no ARC or GC interference from the host language
- No global interpreter lock (GIL) by design; concurrent data structures from day one
- Required for BEAM-density actor heaps (~300 bytes per actor at creation)
- Portable across macOS and future targets without toolchain coupling
- C17 is stable, well-specified, and supported by Apple clang 17 with `-std=c17`

## Reference

Architecture document Section 4.
