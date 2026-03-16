/* src/bootstrap/kernel_load.c
 * Load kernel .st files in dependency order — see kernel_load.h.
 */
#include "kernel_load.h"
#include <sta/vm.h>
#include <stdio.h>
#include <string.h>

/* Kernel files in dependency order.
 * Object must come first (base methods for all classes).
 * True/False next (Boolean operations used by Object>>~=).
 * UndefinedObject last (overrides Object>>isNil etc.). */
static const char *kernel_files[] = {
    "Object.st",
    "True.st",
    "False.st",
    "UndefinedObject.st",
};

#define KERNEL_FILE_COUNT (sizeof(kernel_files) / sizeof(kernel_files[0]))

int sta_kernel_load_all(const char *kernel_dir) {
    char path[1024];

    for (size_t i = 0; i < KERNEL_FILE_COUNT; i++) {
        snprintf(path, sizeof(path), "%s/%s", kernel_dir, kernel_files[i]);

        int rc = sta_vm_load_source(NULL, path);
        if (rc != STA_OK) {
            fprintf(stderr, "kernel_load: failed to load %s: %s\n",
                    kernel_files[i], sta_vm_last_error(NULL));
            return rc;
        }
    }

    return STA_OK;
}
