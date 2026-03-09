#include <sta/vm.h>
#include <stdio.h>

int main(void) {
    STA_VMConfig config = {0};

    STA_VM* vm = sta_vm_create(&config);
    const char* err = sta_vm_last_error(vm);
    printf("last_error (stub): \"%s\"\n", err);

    sta_vm_destroy(vm);

    printf("embed_basic: smoke test passed\n");
    return 0;
}
