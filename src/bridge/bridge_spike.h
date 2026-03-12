/* src/bridge/bridge_spike.h
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: native bridge between the C runtime and the SwiftUI IDE.
 * See docs/spikes/spike-007-native-bridge.md and ADR 013.
 *
 * This header provides:
 *   - The complete definition of struct STA_Handle (satisfying the forward
 *     declaration in sta/vm.h).
 *   - The complete definition of struct STA_VM (satisfying the forward
 *     declaration in sta/vm.h).
 *   - All spike-internal types (handle table, method log, actor registry,
 *     event table).
 *   - Declarations of spike-internal helper functions (sta_handle_alloc,
 *     sta_event_dispatch, sta_actor_registry_add, etc.) for test access.
 *
 * Hard rules:
 *   - Only <stdint.h>, <stdatomic.h>, <pthread.h>, plus sta/vm.h and
 *     src/vm/oop_spike.h are included here.
 *   - No libuv types. No Swift types. No platform-specific opaque types.
 *   - pthread_mutex_t is acceptable (C POSIX, no opaque platform binding).
 */

#pragma once

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

#include <sta/vm.h>           /* Forward declarations of STA_VM, STA_Handle */
#include "src/vm/oop_spike.h" /* STA_ObjHeader, STA_OOP tag macros         */

/* -------------------------------------------------------------------------
 * Capacity constants
 * ---------------------------------------------------------------------- */

#define STA_HANDLE_TABLE_CAPACITY   1024u
#define STA_METHOD_LOG_CAPACITY     1024u
#define STA_CLASS_DEF_LOG_CAPACITY    64u
#define STA_STUB_CLASS_CAPACITY       32u
#define STA_ACTOR_REGISTRY_CAPACITY  256u
#define STA_EVENT_CALLBACK_CAPACITY   16u

/* -------------------------------------------------------------------------
 * Stub class_index tags — used to identify object types in sta_inspect_cstring
 * ---------------------------------------------------------------------- */

#define STA_STUB_NIL_CLASS    0xFFFFFF00u
#define STA_STUB_TRUE_CLASS   0xFFFFFF01u
#define STA_STUB_FALSE_CLASS  0xFFFFFF02u
#define STA_STUB_CLASS_CLASS  0xFFFFFF03u
#define STA_STUB_ACTOR_CLASS  0xFFFFFF04u

/* -------------------------------------------------------------------------
 * struct STA_Handle — complete definition (vm.h forward-declares this)
 *
 * Each handle is a 16-byte entry in the VM's handle table. The oop field
 * holds the rooted OOP and is updated in place on GC move. The handle
 * pointer itself is stable for the lifetime of the table entry.
 * ---------------------------------------------------------------------- */

struct STA_Handle {
    STA_OOP  oop;      /* rooted OOP — updated in place by GC scan       */
    uint32_t refcount; /* reference count; 0 = free slot                 */
    uint32_t _pad;     /* explicit padding to 16 bytes                   */
};
/* SPIKE-INTERNAL: _Static_assert lives in bridge_spike.c */

/* -------------------------------------------------------------------------
 * Method install log (sta_method_install)
 * ---------------------------------------------------------------------- */

typedef struct {
    char class_name[64];
    char selector[64];
    char source[256];
} STA_MethodLogEntry; /* 384 bytes */

typedef struct {
    STA_MethodLogEntry entries[STA_METHOD_LOG_CAPACITY]; /* 98,304 bytes */
    uint32_t           count;
    uint32_t           _pad;
} STA_MethodLog;

/* -------------------------------------------------------------------------
 * Class define log (sta_class_define) — shares the install_lock in STA_VM
 * ---------------------------------------------------------------------- */

typedef struct {
    char source[256];
} STA_ClassDefEntry; /* 256 bytes */

typedef struct {
    STA_ClassDefEntry entries[STA_CLASS_DEF_LOG_CAPACITY]; /* 16,384 bytes */
    uint32_t          count;
    uint32_t          _pad;
} STA_ClassDefLog;

/* -------------------------------------------------------------------------
 * Stub class registry (sta_vm_lookup_class)
 *
 * Read-only after sta_bridge_init; no lock required for lookups.
 * ---------------------------------------------------------------------- */

typedef struct {
    char          name[64];   /* null-terminated class name */
    STA_ObjHeader identity;   /* embedded identity object */
    uint32_t      _pad;       /* align identity to 16 bytes total per entry */
} STA_StubClassEntry; /* 64 + 12 + 4 = 80 bytes */

typedef struct {
    STA_StubClassEntry entries[STA_STUB_CLASS_CAPACITY]; /* 2,560 bytes */
    uint32_t           count;
    uint32_t           _pad;
} STA_ClassRegistry;

/* -------------------------------------------------------------------------
 * Actor registry (sta_actor_enumerate, sta_actor_registry_add/remove)
 *
 * Each entry owns embedded ObjHeader instances for actor and supervisor
 * identity. These remain at stable addresses for the VM's lifetime.
 * Protected by STA_VM.actor_lock.
 * ---------------------------------------------------------------------- */

typedef struct {
    STA_ObjHeader     identity;       /* actor identity object — 12 bytes  */
    uint32_t          actor_id;       /* unique actor ID for display       */
    STA_ObjHeader     sup_identity;   /* supervisor identity object        */
    uint32_t          has_supervisor; /* 1 = supervisor present            */
    _Atomic uint32_t  sched_flags;    /* snapshot-readable scheduler flags */
    _Atomic uint32_t  mailbox_depth;  /* snapshot-readable mailbox depth   */
    uint32_t          active;         /* 1 = registered; 0 = tombstoned    */
    uint32_t          _pad;
} STA_ActorEntry; /* 12+4 + 12+4+4+4+4+4 = 48 bytes */

typedef struct {
    STA_ActorEntry entries[STA_ACTOR_REGISTRY_CAPACITY]; /* 12,288 bytes */
    uint32_t       count;     /* number of active entries                 */
    uint32_t       next_id;   /* monotonically increasing actor ID        */
} STA_ActorRegistry;

/* -------------------------------------------------------------------------
 * Event callback table — protected by STA_VM.htbl_lock (the IDE-API lock)
 * ---------------------------------------------------------------------- */

typedef struct {
    STA_EventCallback callback;
    void             *ctx;
} STA_EventCallbackEntry;

typedef struct {
    STA_EventCallbackEntry entries[STA_EVENT_CALLBACK_CAPACITY];
    uint32_t               count;
    uint32_t               _pad;
} STA_EventTable;

/* -------------------------------------------------------------------------
 * struct STA_VM — complete definition (vm.h forward-declares this)
 *
 * All bridge state is embedded here. The VM is heap-allocated via malloc
 * in sta_vm_create; the large embedded arrays (~130 KB total) are not a
 * concern for a spike.
 *
 * Lock ordering (if multiple locks ever needed simultaneously — avoid this):
 *   actor_lock → htbl_lock → install_lock
 * No code path should hold more than one lock at a time.
 * ---------------------------------------------------------------------- */

struct STA_VM {
    /* Well-known root objects — embedded so their addresses are stable    */
    STA_ObjHeader nil_obj;
    uint32_t      _pad_nil;
    STA_ObjHeader true_obj;
    uint32_t      _pad_true;
    STA_ObjHeader false_obj;
    uint32_t      _pad_false;

    /* Handle table — protected by htbl_lock (the IDE-API lock).
     * htbl_lock is also used for the event callback table.               */
    pthread_mutex_t htbl_lock;
    struct STA_Handle htbl[STA_HANDLE_TABLE_CAPACITY];

    /* Method install and class define logs — protected by install_lock.  */
    pthread_mutex_t install_lock;
    STA_MethodLog   method_log;
    STA_ClassDefLog class_def_log;

    /* Class registry — read-only after init; no lock required.           */
    STA_ClassRegistry class_registry;

    /* Actor registry — protected by actor_lock.                          */
    pthread_mutex_t  actor_lock;
    STA_ActorRegistry actor_registry;

    /* Event callback table — protected by htbl_lock.                     */
    STA_EventTable events;

    /* Inspection buffer — NOT THREAD-SAFE, single-caller-at-a-time.
     * See spike doc Q2 and Open Question 4.                              */
    char inspect_buf[256];

    /* Last error string — written under htbl_lock.                       */
    char last_error[256];
};

/* -------------------------------------------------------------------------
 * Spike-internal function declarations
 * (tests include this header and may call these — flagged SPIKE-INTERNAL)
 * ---------------------------------------------------------------------- */

/* Allocate a handle entry pointing to oop; returns NULL if table is full.
 * Caller must hold htbl_lock. */
STA_Handle *sta_handle_alloc_locked(STA_VM *vm, STA_OOP oop);

/* Public-API wrappers that acquire htbl_lock internally. */
STA_Handle *sta_handle_alloc(STA_VM *vm, STA_OOP oop);

/* Dispatch an event to all registered callbacks. Creates a fresh handle
 * for actor_oop for each callback call (or passes NULL if actor_oop == 0).
 * The callback is responsible for releasing the handle. */
void sta_event_dispatch(STA_VM *vm, STA_EventType type,
                        STA_OOP actor_oop, const char *message);

/* Register a stub actor entry. Returns its index in the registry, or -1
 * if the registry is full.
 * SPIKE-INTERNAL: tests call this to populate the registry. */
int sta_actor_registry_add(STA_VM *vm,
                           uint32_t sched_flags,
                           uint32_t mailbox_depth,
                           int      has_supervisor);

/* Remove (tombstone) an actor entry by index.
 * SPIKE-INTERNAL */
void sta_actor_registry_remove(STA_VM *vm, int index);

/* Accessor for test assertions.
 * SPIKE-INTERNAL */
uint32_t sta_method_log_count(STA_VM *vm);
uint32_t sta_class_def_log_count(STA_VM *vm);

/* Timing helper: nanoseconds from CLOCK_MONOTONIC_RAW. */
uint64_t sta_bridge_now_ns(void);
