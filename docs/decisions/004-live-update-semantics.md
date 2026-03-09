# ADR 004 — Live Update Semantics

**Status:** DEFERRED to Phase 3
**Date:** 2026-03-09

## Decisions to record when Phase 3 begins

- **Method replacement policy:** atomic dict update; old activations complete
  with the old method body before the new one takes effect
- **Structural ivar change policy:** safe-point + explicit migration; no
  silent field reinterpretation
- **Class addition policy:** safe at any time — no lock required
- **Class removal policy:** error if live instances exist (early versions);
  migration path TBD

## Open questions (also Phase 3)

- FileSyncActor conflict policy: what happens when the browser and a file
  editor modify the same method simultaneously? Record decision here when addressed.

## Reference

Architecture document Section 14.2.
