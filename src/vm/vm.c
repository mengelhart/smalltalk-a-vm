#include <sta/vm.h>

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
    return "";
}
