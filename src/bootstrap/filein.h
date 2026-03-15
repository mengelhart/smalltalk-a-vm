/* src/bootstrap/filein.h
 * Squeak/Pharo chunk-format file-in reader.
 * Phase 1 — bootstrap scaffolding. Will be replaced by a Smalltalk-level
 * implementation in Phase 3+ once FileStream and Compiler are available
 * as Smalltalk objects.
 *
 * Reads .st files containing class definitions and method source in
 * standard Squeak/Pharo chunk format (! delimited). Creates classes
 * via the class-creation primitive and compiles/installs methods via
 * sta_compile_method().
 */
#pragma once
#include "../vm/oop.h"
#include "../vm/heap.h"
#include "../vm/immutable_space.h"
#include "../vm/symbol_table.h"
#include "../vm/class_table.h"

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
    STA_Heap           *heap;
    STA_ImmutableSpace *immutable_space;
    STA_SymbolTable    *symbol_table;
    STA_ClassTable     *class_table;
    char                error_msg[512];
    STA_FileInClassInfo class_info[STA_FILEIN_MAX_CLASSES];
    uint32_t            class_info_count;
} STA_FileInContext;

/* ── File-in entry point ─────────────────────────────────────────────── */

/* Load a Squeak/Pharo chunk-format .st file.
 *
 * Reads the file, parses chunk boundaries, identifies class definitions
 * and method source, creates classes and installs methods.
 *
 * Returns 0 on success, negative error code on failure.
 * On failure, ctx->error_msg contains a human-readable diagnostic. */
int sta_filein_load(STA_FileInContext *ctx, const char *path);
