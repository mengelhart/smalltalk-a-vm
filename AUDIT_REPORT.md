# GitHub Issue Audit Report

**Date:** 2026-03-21
**Audited by:** Claude Code
**Codebase state:** main @ 2386e9bea1f056a6f9238c659c2ee714e69746ea

---

## 1. Issues to Close (already fixed or obsolete)

| Issue | Title | Disposition | Evidence |
|-------|-------|-------------|----------|
| #38 | Decision pending: Character representation | Resolved — immediate encoding chosen and documented | ADR 007 §"Character representation decision"; `src/vm/oop.h:9` (bits 1:0 = `10`); prims 94–95 implemented in Batch 2 |
| #39 | Decision pending: Forwarding pointer mechanics | Resolved — Cheney semi-space with forwarding implemented | `src/gc/gc.c:425` (`sta_heap_grow`), `src/gc/gc.c:4` (comment: "forwarding"), Epic 5 (commit 7d7f550) |
| #42 | Decision pending: Deep-copy visited set for cycles | Resolved — VisitedSet hash map implemented | `src/actor/deep_copy.h:6` ("Uses a visited set hash map for cycle detection"); `src/actor/deep_copy.c:visited_init` |
| #48 | Decision pending: ask: reduction counter reset on suspension | Resolved — STA_PRIM_SUSPEND saves frame; sta_interpret_resume starts fresh | `src/vm/interpreter.c:407-411` (STA_PRIM_SUSPEND path); `src/vm/interpreter.c:998-1026` (sta_interpret_resume — fresh reduction counter at 0) |
| #79 | Decision pending: 4-byte density headroom | Obsolete — original 300-byte creation-cost target superseded by ADR 014 and Phase 2 measurements | PROJECT_STATUS.md shows creation cost is now 864B with explicit decisions; ADR 014 is the current density ADR |

### Notes on these closures

**#38:** The condition was "record before String/Character primitives implemented." The decision was recorded in ADR 007 (§Character representation decision, including the encoding table). Primitives 94–95 exist and work.

**#39:** The issue's condition was "decide before GC is implemented." GC is implemented. The forwarding approach (in-object forwarding word + zero-payload side table) is documented in `src/gc/gc.h` and implemented in `src/gc/gc.c`.

**#42:** The issue's condition was "decide before deep-copy implemented." `src/actor/deep_copy.c` has a full visited-set implementation. Cycles and DAG sharing are handled.

**#48:** The suspension/resume protocol is: primitive returns `STA_PRIM_SUSPEND` → interpreter saves frame to `actor->saved_frame` → actor CAS to `STA_ACTOR_SUSPENDED` → future resolver calls `wake_waiter` → scheduler re-enqueues → `sta_interpret_resume` runs, starting a new interpreter call with a fresh reduction counter. The protocol exists. Whether it needs a formal ADR 009 amendment is a documentation question, not a bug.

**#79:** The issue said "4 bytes headroom before 300-byte target." `STA_Actor` is now 208B (creation cost 864B) with explicit ADRs (014, and notes in PROJECT_STATUS.md §Open decisions item 15). The original per-number concern is entirely superseded.

---

## 2. Issues to Create (known bugs without GH issues)

| Proposed Title | Description | Labels | Milestone |
|----------------|-------------|--------|-----------|
| Bug: SmallInteger >> #/ does not raise ZeroDivide — returns DNU on zero divisor | `prim_smallint_div_exact` (`src/vm/primitive_table.c:144`) returns `STA_PRIM_OUT_OF_RANGE` for division by zero. The interpreter then falls through to send `#/` as a normal message, causing DNU (no Smalltalk fallback method). Blue Book §8.1 specifies ZeroDivide should be signalled. | `phase-2`, `bug` | Phase 2 — Actor Runtime and Headless |
| Bug: STA_MailboxMsg.args raw pointer into target heap — stale after heap growth between sends | When `sta_actor_send_msg` deep-copies mutable args onto the target actor's heap (`src/actor/actor.c:236-251`), `msg->args` is a raw `STA_OOP *` into that heap. If a subsequent send to the same actor triggers `sta_heap_grow` (`src/gc/gc.c:430`), the heap is `aligned_alloc`'d to a new address, OOP slots inside objects are fixed up, but `STA_MailboxMsg.args` in already-enqueued messages is **not** a heap OOP — it's a plain C pointer stored in a `malloc`'d struct. The fixup walk at `src/gc/gc.c:447-471` does not reach it. Result: the first message's args pointer is stale. The zero-copy fast path (all immediates/immutables, `args_mallocd=true`) is safe because args live in a `malloc`'d array, not on the heap. | `phase-2`, `bug` | Phase 2 — Actor Runtime and Headless |

---

## 3. Open Decisions to Resolve

| Item # | Issue | Decision | Current Status | Recommendation |
|--------|-------|----------|----------------|----------------|
| 2 | #39 | Forwarding pointer mechanics | **Resolved** — Cheney forwarding implemented in Epic 5 | Close #39 (see §1) |
| 3 | #40 | Class table concurrency strategy | **Still open** — `sta_class_table_set` writes without a lock; reads are relaxed-atomic loads. Safe only because bootstrap is single-threaded before scheduler starts. Not safe for Phase 3 live method installation while actors run. | Keep open |
| 4 | #45 | Variant B deque root cause | **Still uninvestigated** — spike code still shows Variant A chosen; no root-cause analysis recorded | Keep open |
| 5 | #46 | Per-actor scheduling fairness | **Partially measured** — Epic 7B soak test (`test_future_soak.c`) exercises the scheduler with real bytecode but measures throughput (275K rts/s), not per-actor fairness (min/max/median runs per actor). The spike tests do track `min_runs_per_actor` but with stub workloads. The condition ("confirm no starvation with real bytecode") has not been met. | Keep open; note throughput data gives partial confidence |
| 11 | #44 | Transfer buffer allocator | **Still open** — `sta_actor_send_msg` still uses `malloc(sizeof(STA_Heap))` for per-send transfer heaps (`src/actor/actor.c` comment references `sta_deep_copy_to_transfer` which uses `malloc`). No slab allocator exists. | Keep open |
| 12 | — | Resume point protocol for mid-message I/O suspension | **Not yet designed** — libuv I/O not implemented; Phase 2 I/O is future work (#49) | Keep open |
| 13 | — | Lock-free I/O request queue | **Not yet designed** — same as above | Keep open |
| 14 | — | I/O backpressure and bounded mailboxes | **Not yet designed** | Keep open |
| 15 | — | Density headroom consumed | **Tracked** — PROJECT_STATUS.md §"Current struct sizes" is up to date. Not a decision issue per se. | Remove from open decisions (it's a tracking note, not a pending decision) |
| 19 | ~~#88~~ | Growable handle table | **Already resolved** — #88 was closed; Epic 0 Story 5 implemented slab-based growable handle table | Remove from open decisions; correct PROJECT_STATUS.md |
| 20 | #89 | Handle validity after sta_vm_destroy | **Still open** — Phase 3 blocker | Keep open |
| 21 | #90 | sta_inspect_cstring caller-provided buffer | **Still open** — Phase 3 blocker | Keep open |
| 22 | #91 | Event callback re-entrancy rules | **Still open** — Phase 3 blocker | Keep open |

**Additional open decision (newly found):**

| Decision | Issue | Status |
|----------|-------|--------|
| ask: MAILBOX_FULL future resolution protocol | #43 | Still open and confirmed unresolved — `sta_actor_ask_msg` at `src/actor/actor.c:434-440` removes the future from the table and releases it when MAILBOX_FULL, but does NOT call `sta_future_fail`. A Smalltalk actor holding the future OOP and calling `Future >> wait` will hang indefinitely (the future is gone from the table; `prim_future_wait` returns `STA_PRIM_BAD_RECEIVER` on lookup failure — which causes DNU, not a clean error). |

---

## 4. Deferred Items to Clean Up

The following items in PROJECT_STATUS.md §"Deferred items" are marked "done" and should be **removed** from that section to reduce noise:

| Item | Reason |
|------|--------|
| Byte-aware at:/at:put: for String/ByteArray | Done — Batch 2, prims 60–64. Confirmed in PROJECT_STATUS.md Phase 1.5 table. |
| OrderedCollection | Done — Batch 3. |
| Full closures | Done — Phase 2 Epic 1. |
| ensure: during exception unwinding | Done — Phase 2 Epic 1. |
| Real handle table | Done — Phase 2 Epic 0. |
| Per-instance state | Done — Phase 2 Epic 0. |

All six items are already acknowledged as done in the Phase completion table above them. Keeping them in "Deferred items" creates confusing duplication. Only **Stream classes (ReadStream, WriteStream)** is genuinely deferred (Phase 3) and should remain.

---

## 5. Issue / PROJECT_STATUS.md Inconsistencies

| Inconsistency | Details |
|---------------|---------|
| #320 listed as known issue but closed | PROJECT_STATUS.md §"Known issues" lists `#320: Actor registered before fully initialized`. GitHub shows #320 was closed 2026-03-21T01:55:16Z via PR #323 ("Fix late-registration race and stranded-actor cleanup"). Should be removed from Known issues. |
| #321 listed as known issue but closed | PROJECT_STATUS.md §"Known issues" lists `#321: restart_child skips scheduling when scheduler is stopping`. GitHub shows #321 was closed with comment "Fixed in PR #323." Should be removed from Known issues. |
| Open decisions item 19 references #88 as open | PROJECT_STATUS.md §"Open decisions" item 19 says "Growable handle table (ADR 013, #88) — Phase 3 blocker". But #88 was closed with comment "Resolved in Epic 0 Story 5. Slab-based growable handle table implemented." The handle table is not a pending decision. |
| Open decisions item 2 (forwarding pointer mechanics) | References the forwarding decision as open. This was resolved in Epic 5. Remove from open decisions after #39 is closed. |
| Open decisions item 15 (density headroom) | Phrased as a decision item but is really a tracking note. The current struct sizes table already serves this purpose. Could be removed from open decisions. |

---

## 6. Issues Confirmed Still Open (no action needed)

| Issue | Title | Confirmed Still Broken | Notes |
|-------|-------|------------------------|-------|
| #40 | Class table concurrency strategy | Yes — no concurrent grow/write strategy exists | Safe now (bootstrap-only writes); critical in Phase 3 when IDE installs methods while actors run |
| #43 | ask: future resolution on STA_ERR_MAILBOX_FULL | Yes — future removed but not failed on MAILBOX_FULL | `src/actor/actor.c:434-440`; waiter gets DNU or hangs |
| #44 | Transfer buffer allocator | Yes — still using `malloc` | `src/actor/actor.c` still calls `malloc(sizeof(STA_Heap))` for deep-copy |
| #45 | Variant B deque root cause | Yes — not investigated | `src/scheduler/scheduler_spike.h` still has Variant B stub |
| #46 | Per-actor scheduling fairness | Yes — condition not met | No per-actor min/max/median measurement with real bytecode exists |
| #47 | Quantum wall-clock duration with real bytecode | Yes — `STA_REDUCTION_QUOTA` still 1000; no ADR 009 amendment | `src/vm/interpreter.h:79`; soak test measures aggregate throughput, not per-quantum latency |
| #49 | libuv integration notification path | Yes — I/O not implemented | Entire `src/io/` subsystem is Phase 2 future work |
| #89 | Handle validity after sta_vm_destroy | Yes — Phase 3 blocker | Not yet needed |
| #90 | sta_inspect_cstring caller-provided buffer | Yes — Phase 3 blocker | Not yet needed |
| #91 | Event callback re-entrancy rules | Yes — Phase 3 blocker | Not yet needed |
| #243 | SmallInteger = with non-SmallInt returns receiver instead of false | Yes | `src/vm/primitive_table.c` prim 7 (`prim_smallint_eq`); condition check returns receiver OOP on bad arg |
| #244 | Catching DNU via on:do: triggers BlockCannotReturn | Yes | No code change observed in interpreter or exception handling |
| #296 | Immutable space bounds need atomic read for concurrent GC | Yes | `src/vm/immutable_space.c` — `used` field is non-atomic; GC reads it without synchronisation |
| #307 | Smalltalk-level supervisor behavior | Yes | No Smalltalk-level supervisor class exists |
| #308 | one_for_all and rest_for_one restart strategies | Yes | `src/actor/supervisor.c` only has `one_for_one` |
| #309 | restart-with-backoff strategy | Yes | No backoff logic in `src/actor/supervisor.c` |
| #325 | Refactor: shared helper for deep-copy + enqueue + wakeup | Yes | `sta_actor_send_msg` and `sta_actor_ask_msg` in `src/actor/actor.c` still duplicate the full deep-copy/enqueue/wakeup block |
| #337 | TSan race: sta_scheduler_enqueue writes next_runnable outside wake_mutex | Yes | Acknowledged pre-existing race in PROJECT_STATUS.md §"Test count" |

---

## Summary

| Category | Count |
|----------|-------|
| Issues to close (fixed or obsolete) | 5 (#38, #39, #42, #48, #79) |
| Issues to create (unfiled bugs) | 2 (ZeroDivide fallback; stale args pointer) |
| Open decisions already resolved | 3 (forwarding #39, deep-copy visited set #42, ask suspension #48) |
| Open decisions with PROJECT_STATUS.md errors | 2 (#88 says open, is closed; #320/#321 say open, are closed) |
| Deferred items to remove | 6 (all the "done" entries except Stream classes) |
| Issues confirmed still open | 17 |
