# ADR 006 — Handle Lifecycle

**Status:** DEFERRED to Phase 0 spike
**Date:** 2026-03-09

## Question

Should the public API use opaque `STA_Handle*` rooted by the runtime, or raw
`STA_OOP` with explicit pinning?

## Recommended direction

`STA_Handle*` model (JNI / CPython pattern):

- Handles are acquired per-operation (e.g. from `sta_eval`, `sta_actor_spawn`)
- `sta_handle_retain` roots a handle across GC cycles
- `sta_handle_release` unroots it — every acquired handle must be released
- Handle leaks are real memory leaks

Raw `STA_OOP` is reserved for internal VM use only and never crosses the FFI
boundary.

## What must be spiked before locking this decision

- Measure overhead of handle table vs pinning under concurrent GC
- Validate that the Swift IDE's usage patterns don't require bulk acquire/release
- Confirm handle invalidation behavior on `sta_vm_destroy`

## Lock this before

Any IDE code is written against the public API. Record full
acquire/retain/release semantics here once the spike is complete.

## Reference

Architecture document Section 4.3.
