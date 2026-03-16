/* src/bootstrap/kernel_load.c
 * Load kernel .st files in dependency order — see kernel_load.h.
 *
 * Uses a shared STA_FileInContext across all files so that class info
 * (especially instvar names) is available to later files.
 * Pre-registers bootstrap classes that have named instance variables.
 */
#include "kernel_load.h"
#include "filein.h"
#include "../vm/primitive_table.h"
#include "../vm/special_objects.h"
#include <sta/vm.h>
#include <stdio.h>
#include <string.h>

/* Kernel files in dependency order.
 * Object must come first (base methods for all classes).
 * True/False next (Boolean operations used by Object>>~=).
 * UndefinedObject next (overrides Object>>isNil etc.).
 * Magnitude before Number (Number inherits Magnitude comparisons).
 * Number before SmallInteger (SmallInteger inherits Number arithmetic).
 * Association standalone (no deps beyond Object).
 * Collection before SequenceableCollection (abstract iteration protocol).
 * SequenceableCollection before ArrayedCollection (index-based iteration).
 * ArrayedCollection before Array/String (concrete indexed collections).
 * String last in collection family (inherits size from ArrayedCollection). */
static const char *kernel_files[] = {
    "Object.st",
    "True.st",
    "False.st",
    "UndefinedObject.st",
    "Magnitude.st",
    "Number.st",
    "SmallInteger.st",
    "Association.st",
    "Collection.st",
    "SequenceableCollection.st",
    "ArrayedCollection.st",
    "Character.st",
    "String.st",
    "ByteArray.st",
};

#define KERNEL_FILE_COUNT (sizeof(kernel_files) / sizeof(kernel_files[0]))

/* Pre-register bootstrap classes that have named instance variables.
 * The filein compiler needs these to resolve instvar references. */
static void register_bootstrap_ivars(STA_FileInContext *ctx) {
    /* Association: key, value */
    if (ctx->class_info_count < STA_FILEIN_MAX_CLASSES) {
        STA_FileInClassInfo *info = &ctx->class_info[ctx->class_info_count];
        memset(info, 0, sizeof(*info));
        strncpy(info->class_name, "Association", sizeof(info->class_name) - 1);
        strncpy(info->super_name, "Object", sizeof(info->super_name) - 1);
        strncpy(info->ivar_names[0], "key", 63);
        strncpy(info->ivar_names[1], "value", 63);
        info->ivar_count = 2;
        ctx->class_info_count++;
    }

    /* Exception: messageText, signalContext */
    if (ctx->class_info_count < STA_FILEIN_MAX_CLASSES) {
        STA_FileInClassInfo *info = &ctx->class_info[ctx->class_info_count];
        memset(info, 0, sizeof(*info));
        strncpy(info->class_name, "Exception", sizeof(info->class_name) - 1);
        strncpy(info->super_name, "Object", sizeof(info->super_name) - 1);
        strncpy(info->ivar_names[0], "messageText", 63);
        strncpy(info->ivar_names[1], "signalContext", 63);
        info->ivar_count = 2;
        ctx->class_info_count++;
    }

    /* MessageNotUnderstood: message, receiver (inherits Exception ivars) */
    if (ctx->class_info_count < STA_FILEIN_MAX_CLASSES) {
        STA_FileInClassInfo *info = &ctx->class_info[ctx->class_info_count];
        memset(info, 0, sizeof(*info));
        strncpy(info->class_name, "MessageNotUnderstood", sizeof(info->class_name) - 1);
        strncpy(info->super_name, "Exception", sizeof(info->super_name) - 1);
        strncpy(info->ivar_names[0], "message", 63);
        strncpy(info->ivar_names[1], "receiver", 63);
        info->ivar_count = 2;
        ctx->class_info_count++;
    }
}

int sta_kernel_load_all(const char *kernel_dir) {
    STA_Heap           *heap = sta_primitive_get_heap();
    STA_ImmutableSpace *imm  = sta_primitive_get_immutable_space();
    STA_SymbolTable    *syms = sta_primitive_get_symbol_table();
    STA_ClassTable     *ct   = sta_primitive_get_class_table();

    if (!heap || !imm || !syms || !ct) {
        return STA_ERR_INTERNAL;
    }

    STA_FileInContext ctx = {
        .heap = heap,
        .immutable_space = imm,
        .symbol_table = syms,
        .class_table = ct,
    };

    register_bootstrap_ivars(&ctx);

    char path[1024];

    for (size_t i = 0; i < KERNEL_FILE_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/%s", kernel_dir, kernel_files[i]);

        int rc = sta_filein_load(&ctx, path);
        if (rc != 0) {
            fprintf(stderr, "kernel_load: failed to load %s: %s\n",
                    kernel_files[i], ctx.error_msg);
            return rc < 0 ? rc : STA_ERR_COMPILE;
        }
    }

    return STA_OK;
}
