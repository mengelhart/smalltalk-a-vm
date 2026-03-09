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

/* -------------------------------------------------------------------------
 * Actor interface
 * ---------------------------------------------------------------------- */

STA_Actor* sta_actor_spawn(STA_VM* vm, STA_Handle* class_handle);
int        sta_actor_send(STA_VM* vm, STA_Actor* actor, STA_Handle* message);

/* -------------------------------------------------------------------------
 * Handle lifecycle
 * ---------------------------------------------------------------------- */

STA_Handle* sta_handle_retain(STA_VM* vm, STA_Handle* handle);
void        sta_handle_release(STA_VM* vm, STA_Handle* handle);

/* -------------------------------------------------------------------------
 * Evaluation and inspection
 * ---------------------------------------------------------------------- */

STA_Handle* sta_eval(STA_VM* vm, const char* expression);
STA_Handle* sta_inspect(STA_VM* vm, STA_Handle* object);

#ifdef __cplusplus
}
#endif

#endif /* STA_VM_H */
