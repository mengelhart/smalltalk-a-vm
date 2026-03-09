#include <sta/vm.h>

STA_Handle* sta_handle_retain(STA_VM* vm, STA_Handle* handle) {
    (void)vm;
    return handle;
}

void sta_handle_release(STA_VM* vm, STA_Handle* handle) {
    (void)vm; (void)handle;
}
