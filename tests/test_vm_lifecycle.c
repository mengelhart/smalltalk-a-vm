/* tests/test_vm_lifecycle.c
 * Tests for STA_VM lifecycle — create, bootstrap, image round-trip, destroy.
 * Phase 1, Epic 11, Story 6.
 */
#include <sta/vm.h>
#include "vm/vm_state.h"
#include "vm/oop.h"
#include "vm/special_objects.h"
#include "vm/symbol_table.h"
#include "vm/interpreter.h"
#include "compiler/compiler.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

/* Helper: evaluate an expression on a live VM using internal APIs. */
static STA_OOP eval(STA_VM *vm, const char *source) {
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);
    STA_CompileResult cr = sta_compile_expression(
        source, &vm->symbol_table, &vm->immutable_space, &vm->heap, sysdict);
    if (cr.had_error) {
        fprintf(stderr, "eval compile error: %s\n  source: %s\n",
                cr.error_msg, source);
        assert(!cr.had_error);
    }
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    return sta_interpret(vm, cr.method, nil_oop, NULL, 0);
}

/* ── Test 1: Create with bootstrap (no image) ─────────────────────── */

static void test_create_bootstrap(void) {
    STA_VMConfig config = {0};
    STA_VM *vm = sta_vm_create(&config);
    assert(vm != NULL);
    assert(vm->bootstrapped);

    /* Verify 3 + 4 = 7 works. */
    STA_OOP result = eval(vm, "3 + 4");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 7);

    sta_vm_destroy(vm);
}

/* ── Test 2: Create-save-destroy-create-from-image round-trip ──────── */

static void test_image_roundtrip(void) {
    const char *img = "/tmp/sta_test_vm_lifecycle.stai";

    /* Create VM, bootstrap, save image. */
    STA_VMConfig config1 = { .image_path = img };
    STA_VM *vm1 = sta_vm_create(&config1);
    assert(vm1 != NULL);

    /* Verify live. */
    STA_OOP r1 = eval(vm1, "3 + 4");
    assert(STA_IS_SMALLINT(r1));
    assert(STA_SMALLINT_VAL(r1) == 7);

    sta_vm_destroy(vm1);

    /* Create second VM from the saved image. */
    STA_VMConfig config2 = { .image_path = img };
    STA_VM *vm2 = sta_vm_create(&config2);
    assert(vm2 != NULL);
    assert(vm2->bootstrapped);

    /* Verify interpreter works after image load. */
    STA_OOP r2 = eval(vm2, "3 + 4");
    assert(STA_IS_SMALLINT(r2));
    assert(STA_SMALLINT_VAL(r2) == 7);

    /* Verify kernel methods work after image load. */
    STA_OOP r3 = eval(vm2, "true ifTrue: [42] ifFalse: [0]");
    assert(STA_IS_SMALLINT(r3));
    assert(STA_SMALLINT_VAL(r3) == 42);

    sta_vm_destroy(vm2);
    unlink(img);
}

/* ── Test 3: NULL config returns NULL ─────────────────────────────── */

static void test_null_config(void) {
    STA_VM *vm = sta_vm_create(NULL);
    assert(vm == NULL);
}

/* ── Test 4: Double destroy safety ────────────────────────────────── */

static void test_double_destroy(void) {
    STA_VMConfig config = {0};
    STA_VM *vm = sta_vm_create(&config);
    assert(vm != NULL);
    sta_vm_destroy(vm);
    /* Second destroy on freed memory is UB, but we can test NULL. */
    sta_vm_destroy(NULL);
}

/* ── Test 5: Last error on failure ────────────────────────────────── */

static void test_last_error(void) {
    /* sta_vm_last_error(NULL) should return non-NULL. */
    const char *err = sta_vm_last_error(NULL);
    assert(err != NULL);
}

/* ── Test 6: Kernel methods work (not just arithmetic) ────────────── */

static void test_kernel_methods(void) {
    STA_VMConfig config = {0};
    STA_VM *vm = sta_vm_create(&config);
    assert(vm != NULL);

    /* Test various kernel methods. */
    STA_OOP r1 = eval(vm, "nil isNil");
    assert(r1 == sta_spc_get(SPC_TRUE));

    STA_OOP r2 = eval(vm, "true not");
    assert(r2 == sta_spc_get(SPC_FALSE));

    STA_OOP r3 = eval(vm, "3 < 5");
    assert(r3 == sta_spc_get(SPC_TRUE));

    sta_vm_destroy(vm);
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_vm_lifecycle:\n");

    RUN(test_create_bootstrap);
    RUN(test_image_roundtrip);
    RUN(test_null_config);
    RUN(test_double_destroy);
    RUN(test_last_error);
    RUN(test_kernel_methods);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
