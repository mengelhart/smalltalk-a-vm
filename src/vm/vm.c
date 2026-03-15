#include <sta/vm.h>
#include "vm/primitive_table.h"
#include "bootstrap/filein.h"
#include <string.h>
#include <stdio.h>

static char g_last_error[512] = "";

STA_VM* sta_vm_create(const STA_VMConfig* config) {
    (void)config;
    return NULL;
}

void sta_vm_destroy(STA_VM* vm) {
    (void)vm;
}

int sta_vm_load_image(STA_VM* vm, const char* path) {
    (void)vm; (void)path;
    return STA_ERR_INTERNAL;
}

int sta_vm_save_image(STA_VM* vm, const char* path) {
    (void)vm; (void)path;
    return STA_ERR_INTERNAL;
}

const char* sta_vm_last_error(STA_VM* vm) {
    (void)vm;
    return g_last_error;
}

int sta_vm_load_source(STA_VM* vm, const char* path) {
    (void)vm;

    STA_Heap           *heap = sta_primitive_get_heap();
    STA_ImmutableSpace *imm  = sta_primitive_get_immutable_space();
    STA_SymbolTable    *syms = sta_primitive_get_symbol_table();
    STA_ClassTable     *ct   = sta_primitive_get_class_table();

    if (!heap || !imm || !syms || !ct) {
        snprintf(g_last_error, sizeof(g_last_error),
                 "kernel not bootstrapped");
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
        snprintf(g_last_error, sizeof(g_last_error), "%s", ctx.error_msg);
        if (rc == -3) return STA_ERR_IO;
        return STA_ERR_COMPILE;
    }
    return STA_OK;
}
