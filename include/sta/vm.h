/*
 * sta/vm.h — Smalltalk/A VM public API
 *
 * This is the ONLY public header. All implementation is in src/ (private).
 * The Swift IDE uses only this file. There is no privileged back-channel.
 *
 * External callers use STA_Handle* for all object references.
 * Raw STA_OOP is for internal VM use only.
 *
 * Error convention: functions returning int use STA_OK (0) for success
 * and STA_ERR_* (negative) for failure. Use sta_vm_last_error() for
 * human-readable diagnostics.
 */

#ifndef STA_VM_H
#define STA_VM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Opaque types
 * ---------------------------------------------------------------------- */

typedef struct STA_VM     STA_VM;
typedef struct STA_Actor  STA_Actor;
typedef struct STA_Handle STA_Handle;
typedef uintptr_t         STA_OOP;    /* internal VM use only */

/* -------------------------------------------------------------------------
 * Error codes
 * ---------------------------------------------------------------------- */

#define STA_OK            0
#define STA_ERR_INVALID  (-1)
#define STA_ERR_OOM      (-2)
#define STA_ERR_IO       (-3)
#define STA_ERR_INTERNAL (-4)
#define STA_ERR_COMPILE  (-5)

/* -------------------------------------------------------------------------
 * VM configuration
 * ---------------------------------------------------------------------- */

typedef struct STA_VMConfig {
    int         scheduler_threads;
    size_t      initial_heap_bytes;
    const char* image_path;
} STA_VMConfig;

/* -------------------------------------------------------------------------
 * VM lifecycle
 * ---------------------------------------------------------------------- */

STA_VM*     sta_vm_create(const STA_VMConfig* config);
void        sta_vm_destroy(STA_VM* vm);
int         sta_vm_load_image(STA_VM* vm, const char* path);
int         sta_vm_save_image(STA_VM* vm, const char* path);
const char* sta_vm_last_error(STA_VM* vm);

/**
 * Load a Squeak/Pharo chunk-format .st file into the live image.
 * Creates classes and installs methods.
 * Files must be loaded in dependency order.
 *
 * @param vm   The VM instance
 * @param path Path to the .st file
 * @return STA_OK on success, STA_ERR_COMPILE or STA_ERR_IO on failure.
 *         Call sta_vm_last_error() for details.
 */
int sta_vm_load_source(STA_VM* vm, const char* path);

/* -------------------------------------------------------------------------
 * Actor enumeration (Spike 007 — see ADR 013)
 *
 * STA_ActorInfo is a snapshot of a single actor's observable state,
 * delivered to the visitor by sta_actor_enumerate. Both handle fields
 * are freshly acquired (refcount = 1); the visitor must release them.
 * supervisor_handle is NULL for top-level actors with no supervisor.
 * ---------------------------------------------------------------------- */

typedef struct {
    STA_Handle* actor_handle;       /* identity handle — caller must release */
    STA_Handle* supervisor_handle;  /* supervisor handle or NULL — caller must release */
    uint32_t    mailbox_depth;      /* snapshot of mailbox queue depth */
    uint32_t    sched_flags;        /* snapshot of scheduler flags */
} STA_ActorInfo;

typedef void (*STA_ActorVisitor)(const STA_ActorInfo* info, void* ctx);

/* -------------------------------------------------------------------------
 * Event callbacks (Spike 007 — see ADR 013)
 *
 * The VM delivers events to registered callbacks from whichever thread the
 * event occurs on. Callbacks must be short and non-blocking. Callbacks
 * must not call sta_event_register or sta_event_unregister from within
 * the callback body. event->actor is a freshly acquired handle (refcount
 * = 1); the callback must release it (or retain it for later use).
 * event->message is VM-owned and valid only for the duration of the call.
 * ---------------------------------------------------------------------- */

typedef enum {
    STA_EVT_ACTOR_CRASH         = 1, /* actor terminated with unhandled error */
    STA_EVT_METHOD_INSTALLED    = 2, /* method install confirmed */
    STA_EVT_IMAGE_SAVE_COMPLETE = 3, /* sta_vm_save_image completed */
    STA_EVT_UNHANDLED_EXCEPTION = 4  /* exception escaped a top-level actor */
} STA_EventType;

typedef struct {
    STA_EventType type;
    STA_Handle*   actor;        /* relevant actor handle or NULL; caller must release */
    const char*   message;      /* VM-owned; valid only during callback */
    uint64_t      timestamp_ns; /* CLOCK_MONOTONIC_RAW nanoseconds */
} STA_Event;

typedef void (*STA_EventCallback)(STA_VM* vm, const STA_Event* event, void* ctx);

/* -------------------------------------------------------------------------
 * Actor interface
 * ---------------------------------------------------------------------- */

STA_Actor* sta_actor_spawn(STA_VM* vm, STA_Handle* class_handle);
int        sta_actor_send(STA_VM* vm, STA_Actor* actor, STA_Handle* message);

/* Enumerate all live actors. Takes a consistent snapshot of the actor
 * registry (under the actor-registry lock), releases the lock, then calls
 * visitor once per actor from the calling thread. The visitor must release
 * both handles in STA_ActorInfo. Returns the actor count visited, or a
 * negative STA_ERR_* code on failure. The caller must not modify VM state
 * from inside the visitor (no sta_actor_spawn, no sta_method_install). */
int sta_actor_enumerate(STA_VM* vm, STA_ActorVisitor visitor, void* ctx);

/* -------------------------------------------------------------------------
 * Handle lifecycle
 * ---------------------------------------------------------------------- */

STA_Handle* sta_handle_retain(STA_VM* vm, STA_Handle* handle);
void        sta_handle_release(STA_VM* vm, STA_Handle* handle);

/* -------------------------------------------------------------------------
 * Well-known roots (Spike 007 — see ADR 013)
 *
 * These are the bootstrapping entry points: the IDE can obtain its first
 * handle without calling sta_eval (which requires a live interpreter).
 * Each function returns a freshly acquired handle (refcount = 1).
 * The caller must release it via sta_handle_release.
 * These functions never return NULL on a valid, non-destroyed VM.
 * ---------------------------------------------------------------------- */

STA_Handle* sta_vm_nil(STA_VM* vm);
STA_Handle* sta_vm_true(STA_VM* vm);
STA_Handle* sta_vm_false(STA_VM* vm);

/* Look up a class by name. Returns a freshly acquired handle (refcount = 1)
 * on success. Returns NULL and sets sta_vm_last_error on failure. */
STA_Handle* sta_vm_lookup_class(STA_VM* vm, const char* name);

/* -------------------------------------------------------------------------
 * Evaluation and inspection
 * ---------------------------------------------------------------------- */

STA_Handle* sta_eval(STA_VM* vm, const char* expression);
STA_Handle* sta_inspect(STA_VM* vm, STA_Handle* object);

/* Returns a null-terminated C string representation of the object.
 *
 * *** NOT THREAD-SAFE. Single-caller-at-a-time by CONTRACT. ***
 *
 * The returned pointer is VM-owned and valid until the next call to
 * sta_inspect_cstring on the same VM (from any thread), or until
 * sta_vm_destroy. The IDE must call this from one thread only (the main
 * thread) and must not hold the pointer across any suspension point.
 * The lock serialises concurrent writes to the buffer but does not extend
 * the validity of the returned pointer — the caller must copy the string
 * before any other thread could call sta_inspect_cstring again.
 * See ADR 013 and Open Question 4 (Phase 3: caller-provided buffer). */
const char* sta_inspect_cstring(STA_VM* vm, STA_Handle* handle);

/* -------------------------------------------------------------------------
 * Live update (Spike 007 — see ADR 013)
 *
 * sta_method_install records (class_name, selector, source) in a thread-safe
 * log. Full compiler integration is Phase 1. Safe to call from any thread.
 *
 * sta_class_define records the class source in a separate log. Full class
 * table integration is Phase 1. Safe to call from any thread.
 * ---------------------------------------------------------------------- */

int sta_method_install(STA_VM*     vm,
                       STA_Handle* class_handle,
                       const char* selector,
                       const char* source);

int sta_class_define(STA_VM* vm, const char* source);

/* -------------------------------------------------------------------------
 * Event callbacks (Spike 007 — see ADR 013)
 *
 * Multiple callbacks may be registered; all are called for each event in
 * registration order. Protected by the IDE-API lock. Returns STA_OK or
 * STA_ERR_OOM if the callback table is full.
 * sta_event_unregister is a no-op if the (callback, ctx) pair is not found.
 * ---------------------------------------------------------------------- */

int  sta_event_register  (STA_VM* vm, STA_EventCallback callback, void* ctx);
void sta_event_unregister(STA_VM* vm, STA_EventCallback callback, void* ctx);

#ifdef __cplusplus
}
#endif

#endif /* STA_VM_H */
