/* src/bootstrap/kernel_load.c
 * Load kernel .st files in dependency order — see kernel_load.h.
 */
#include "kernel_load.h"
#include "filein.h"
#include "../vm/vm_state.h"
#include "../vm/special_objects.h"
#include <sta/vm.h>
#include <stdio.h>
#include <string.h>

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
    "Symbol.st",
    "ByteArray.st",
    "Array.st",
    "OrderedCollection.st",
};

#define KERNEL_FILE_COUNT (sizeof(kernel_files) / sizeof(kernel_files[0]))

static void register_bootstrap_ivars(STA_FileInContext *ctx) {
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

int sta_kernel_load_all(STA_VM *vm, const char *kernel_dir) {
    if (!vm) return STA_ERR_INTERNAL;

    STA_FileInContext ctx = { .vm = vm };
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
