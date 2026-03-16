/* src/vm/vm.c
 * Public API implementation — STA_VM lifecycle, image, source loading.
 * Phase 1, Epic 11.
 */
#include "vm/vm_internal.h"
#include "vm/primitive_table.h"
#include "vm/special_objects.h"
#include "vm/handler.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/kernel_load.h"
#include "bootstrap/filein.h"
#include "image/image.h"
#include <sta/vm.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Defaults ──────────────────────────────────────────────────────── */

#define DEFAULT_HEAP_BYTES      (4u * 1024u * 1024u)
#define DEFAULT_IMM_BYTES       (4u * 1024u * 1024u)
#define DEFAULT_SYMTAB_CAPACITY 512u
#define DEFAULT_SLAB_BYTES      (64u * 1024u)

/* Static fallback error for sta_vm_last_error(NULL) after failed create. */
static char g_last_error[512] = "";

/* ── Helpers ───────────────────────────────────────────────────────── */

static void set_error(STA_VM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->last_error, sizeof(vm->last_error), fmt, ap);
    va_end(ap);
}

static void wire_primitives(STA_VM *vm) {
    sta_primitive_table_init();
    sta_primitive_set_class_table(vm->class_table);
    sta_primitive_set_heap(vm->heap);
    sta_primitive_set_slab(vm->stack_slab);
    sta_primitive_set_symbol_table(vm->symbol_table);
    sta_primitive_set_immutable_space(vm->immutable_space);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* ── sta_vm_create ─────────────────────────────────────────────────── */

STA_VM* sta_vm_create(const STA_VMConfig* config) {
    if (!config) return NULL;

    STA_VM *vm = calloc(1, sizeof(STA_VM));
    if (!vm) return NULL;

    vm->config = *config;
    vm->last_error[0] = '\0';

    /* Allocate subsystems. */
    size_t heap_bytes = config->initial_heap_bytes > 0
                        ? config->initial_heap_bytes
                        : DEFAULT_HEAP_BYTES;

    vm->heap = sta_heap_create(heap_bytes);
    if (!vm->heap) { set_error(vm, "heap allocation failed"); goto fail; }

    vm->immutable_space = sta_immutable_space_create(DEFAULT_IMM_BYTES);
    if (!vm->immutable_space) { set_error(vm, "immutable space allocation failed"); goto fail; }

    vm->symbol_table = sta_symbol_table_create(DEFAULT_SYMTAB_CAPACITY);
    if (!vm->symbol_table) { set_error(vm, "symbol table allocation failed"); goto fail; }

    vm->class_table = sta_class_table_create();
    if (!vm->class_table) { set_error(vm, "class table allocation failed"); goto fail; }

    vm->stack_slab = sta_stack_slab_create(DEFAULT_SLAB_BYTES);
    if (!vm->stack_slab) { set_error(vm, "stack slab allocation failed"); goto fail; }

    /* Reset global handler state for clean startup. */
    sta_handler_set_top(NULL);

    /* Startup pipeline: load image or bootstrap from scratch. */
    if (config->image_path && file_exists(config->image_path)) {
        /* Load from saved image. */
        int rc = sta_image_load_from_file(config->image_path,
                                          vm->heap, vm->immutable_space,
                                          vm->symbol_table, vm->class_table);
        if (rc != 0) {
            set_error(vm, "image load failed: %s (code %d)",
                      config->image_path, rc);
            goto fail;
        }
        wire_primitives(vm);
    } else {
        /* First launch — bootstrap from scratch. */
        STA_BootstrapResult br = sta_bootstrap(vm->heap, vm->immutable_space,
                                               vm->symbol_table, vm->class_table);
        if (br.status != 0) {
            set_error(vm, "bootstrap failed: %s", br.error ? br.error : "unknown");
            goto fail;
        }

        /* Wire primitives before kernel load (kernel_load uses prim getters). */
        wire_primitives(vm);

        /* Load kernel .st files. KERNEL_DIR is compile-time for tests;
         * for the public API we look for a "kernel/" directory relative
         * to the working directory. Tests pass KERNEL_DIR via -D. */
#ifdef KERNEL_DIR
        const char *kernel_dir = KERNEL_DIR;
#else
        const char *kernel_dir = "kernel";
#endif
        int rc = sta_kernel_load_all(kernel_dir);
        if (rc != 0) {
            set_error(vm, "kernel load failed (code %d)", rc);
            goto fail;
        }

        /* Optionally save the image for next launch. */
        if (config->image_path) {
            rc = sta_image_save_to_file(config->image_path,
                                        vm->heap, vm->immutable_space,
                                        vm->symbol_table, vm->class_table);
            if (rc != 0) {
                set_error(vm, "image save failed (code %d)", rc);
                goto fail;
            }
        }
    }

    vm->bootstrapped = true;
    return vm;

fail:
    /* Tear down any partially-allocated subsystems. */
    if (vm->stack_slab)       sta_stack_slab_destroy(vm->stack_slab);
    if (vm->class_table)      sta_class_table_destroy(vm->class_table);
    if (vm->symbol_table)     sta_symbol_table_destroy(vm->symbol_table);
    if (vm->immutable_space)  sta_immutable_space_destroy(vm->immutable_space);
    if (vm->heap)             sta_heap_destroy(vm->heap);
    /* Copy error to static fallback so sta_vm_last_error(NULL) works. */
    memcpy(g_last_error, vm->last_error, sizeof(g_last_error));
    free(vm);
    return NULL;
}

/* ── sta_vm_destroy ────────────────────────────────────────────────── */

void sta_vm_destroy(STA_VM* vm) {
    if (!vm || vm->destroyed) return;

    vm->destroyed = true;

    /* Teardown in reverse order of creation. */
    if (vm->stack_slab)       sta_stack_slab_destroy(vm->stack_slab);
    if (vm->class_table)      sta_class_table_destroy(vm->class_table);
    if (vm->symbol_table)     sta_symbol_table_destroy(vm->symbol_table);
    if (vm->immutable_space)  sta_immutable_space_destroy(vm->immutable_space);
    if (vm->heap)             sta_heap_destroy(vm->heap);

    /* Reset globals so a subsequent sta_vm_create starts clean. */
    sta_handler_set_top(NULL);
    sta_special_objects_init();

    free(vm);
}

/* ── sta_vm_last_error ─────────────────────────────────────────────── */

const char* sta_vm_last_error(STA_VM* vm) {
    if (vm) return vm->last_error;
    return g_last_error;
}

/* ── sta_vm_load_image / sta_vm_save_image ─────────────────────────── */

int sta_vm_load_image(STA_VM* vm, const char* path) {
    if (!vm || !path) return STA_ERR_INVALID;
    int rc = sta_image_load_from_file(path, vm->heap, vm->immutable_space,
                                      vm->symbol_table, vm->class_table);
    if (rc != 0) {
        set_error(vm, "image load failed: %s (code %d)", path, rc);
        return STA_ERR_IO;
    }
    /* Re-wire primitive table after load (C function pointers not serialised). */
    wire_primitives(vm);
    return STA_OK;
}

int sta_vm_save_image(STA_VM* vm, const char* path) {
    if (!vm || !path) return STA_ERR_INVALID;
    int rc = sta_image_save_to_file(path, vm->heap, vm->immutable_space,
                                    vm->symbol_table, vm->class_table);
    if (rc != 0) {
        set_error(vm, "image save failed: %s (code %d)", path, rc);
    }
    return rc == 0 ? STA_OK : STA_ERR_IO;
}

/* ── sta_vm_load_source ────────────────────────────────────────────── */

int sta_vm_load_source(STA_VM* vm, const char* path) {
    if (!path) return STA_ERR_INVALID;

    /* Resolve subsystems: from VM struct if available, else from globals. */
    STA_Heap           *heap;
    STA_ImmutableSpace *imm;
    STA_SymbolTable    *syms;
    STA_ClassTable     *ct;

    if (vm) {
        if (!vm->bootstrapped) {
            set_error(vm, "kernel not bootstrapped");
            return STA_ERR_INTERNAL;
        }
        heap = vm->heap;
        imm  = vm->immutable_space;
        syms = vm->symbol_table;
        ct   = vm->class_table;
    } else {
        /* Legacy path: existing tests pass NULL. */
        heap = sta_primitive_get_heap();
        imm  = sta_primitive_get_immutable_space();
        syms = sta_primitive_get_symbol_table();
        ct   = sta_primitive_get_class_table();
    }

    if (!heap || !imm || !syms || !ct) {
        if (vm) set_error(vm, "kernel not bootstrapped");
        else snprintf(g_last_error, sizeof(g_last_error), "kernel not bootstrapped");
        return STA_ERR_INTERNAL;
    }

    STA_FileInContext ctx = {
        .heap = heap,
        .immutable_space = imm,
        .symbol_table = syms,
        .class_table = ct,
    };

    int rc = sta_filein_load(&ctx, path);
    if (rc != 0) {
        if (vm) set_error(vm, "%s", ctx.error_msg);
        else snprintf(g_last_error, sizeof(g_last_error), "%s", ctx.error_msg);
        if (rc == -3) return STA_ERR_IO;
        return STA_ERR_COMPILE;
    }
    return STA_OK;
}
