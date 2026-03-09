# ADR 005 — API Error Reporting

**Status:** Accepted
**Date:** 2026-03-09

## Decision

All API operations that can fail return `int` using `STA_OK` (0) for success
and `STA_ERR_*` (negative) for failure. Human-readable diagnostics are
available via `sta_vm_last_error(vm)`, which never returns NULL.

```c
#define STA_OK            0
#define STA_ERR_INVALID  (-1)
#define STA_ERR_OOM      (-2)
#define STA_ERR_IO       (-3)
#define STA_ERR_INTERNAL (-4)
```

## Rationale

SQLite pattern — compact, no allocations on the error path, clean C FFI,
embeddable without exception handling infrastructure. The string from
`sta_vm_last_error()` is VM-owned and valid until the next API call.

## Future

Richer structured errors (error codes with payload, stack traces) are a
Phase 4+ concern if embedders need them. Do not add prematurely.
