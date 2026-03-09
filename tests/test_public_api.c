#include <sta/vm.h>
#include <assert.h>
#include <stddef.h>

int main(void) {
    STA_VM* vm = sta_vm_create(NULL);
    assert(vm == NULL);

    const char* err = sta_vm_last_error(NULL);
    assert(err != NULL);

    sta_vm_destroy(NULL);
    sta_handle_release(NULL, NULL);

    return 0;
}
