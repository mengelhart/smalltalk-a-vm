/* src/bootstrap/filein.h
 * Squeak/Pharo chunk-format file-in reader.
 * Phase 1 — bootstrap scaffolding. Will be replaced by a Smalltalk-level
 * implementation in Phase 3+ once FileStream and Compiler are available
 * as Smalltalk objects.
 */
#pragma once
#include "../vm/oop.h"

/* Forward declaration — full definition in vm_state.h */
struct STA_VM;

/* ── Instvar registry (tracks instvar names for file-in classes) ─────── */

#define STA_FILEIN_MAX_CLASSES  64
#define STA_FILEIN_MAX_IVARS    32

typedef struct {
    char class_name[128];
    char ivar_names[STA_FILEIN_MAX_IVARS][64];
    uint32_t ivar_count;
    char super_name[128];
} STA_FileInClassInfo;

/* ── File-in context ─────────────────────────────────────────────────── */

typedef struct {
    struct STA_VM      *vm;
    char                error_msg[512];
    STA_FileInClassInfo class_info[STA_FILEIN_MAX_CLASSES];
    uint32_t            class_info_count;
} STA_FileInContext;

/* ── File-in entry point ─────────────────────────────────────────────── */

/* Load a Squeak/Pharo chunk-format .st file.
 * Returns 0 on success, negative error code on failure.
 * On failure, ctx->error_msg contains a human-readable diagnostic. */
int sta_filein_load(STA_FileInContext *ctx, const char *path);
