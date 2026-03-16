/* examples/embed_basic/main.c
 * Minimal embedding example — uses only the public API (sta/vm.h).
 * Phase 1, Epic 11, Story 9.
 */
#include <sta/vm.h>
#include <stdio.h>

int main(void) {
    STA_VMConfig config = {
        .scheduler_threads = 1,
        .initial_heap_bytes = 4 * 1024 * 1024,
        .image_path = NULL   /* bootstrap from scratch */
    };

    STA_VM *vm = sta_vm_create(&config);
    if (!vm) {
        fprintf(stderr, "Failed to create VM: %s\n",
                sta_vm_last_error(NULL));
        return 1;
    }

    /* Evaluate a Smalltalk expression. */
    STA_Handle *result = sta_eval(vm, "3 + 4");
    if (result) {
        const char *str = sta_inspect_cstring(vm, result);
        printf("3 + 4 = %s\n", str);
        sta_handle_release(vm, result);
    } else {
        fprintf(stderr, "Eval failed: %s\n", sta_vm_last_error(vm));
    }

    /* Boolean conditional. */
    result = sta_eval(vm, "true ifTrue: [42] ifFalse: [0]");
    if (result) {
        const char *str = sta_inspect_cstring(vm, result);
        printf("true ifTrue: [42] ifFalse: [0] = %s\n", str);
        sta_handle_release(vm, result);
    }

    /* Factorial. */
    result = sta_eval(vm, "10 factorial");
    if (result) {
        const char *str = sta_inspect_cstring(vm, result);
        printf("10 factorial = %s\n", str);
        sta_handle_release(vm, result);
    }

    sta_vm_destroy(vm);
    printf("embed_basic: all examples completed\n");
    return 0;
}
