/* tests/test_image_roundtrip.c
 * Acid test: bootstrap → save → fresh state → load → interpreter works.
 * Phase 1, Epic 10, Story 6.
 */
#include <sta/vm.h>
#include "vm/interpreter.h"
#include "vm/primitive_table.h"
#include "vm/symbol_table.h"
#include "vm/special_objects.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/frame.h"
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/kernel_load.h"
#include "compiler/compiler.h"
#include "image/image.h"
#include <stdio.h>
#include <stdlib.h>
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

/* ── State ──────────────────────────────────────────────────────────── */

static const char *image_path = "/tmp/sta_test_roundtrip.stai";

/* Post-load state (used by all verification tests). */
static STA_VM *load_vm;

/* Helper: evaluate an expression after image load. */
static STA_OOP eval(const char *source) {
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);
    STA_CompileResult cr = sta_compile_expression(
        source, &load_vm->symbol_table, &load_vm->immutable_space, &load_vm->heap, sysdict);
    if (cr.had_error) {
        fprintf(stderr, "eval compile error: %s\n  source: %s\n",
                cr.error_msg, source);
        assert(!cr.had_error);
    }
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    return sta_interpret(load_vm, cr.method, nil_oop, NULL, 0);
}

/* ── Phase 1: Bootstrap + save ────────────────────────────────────── */

static void test_save_image(void) {
    STA_VM *save_vm = calloc(1, sizeof(STA_VM));
    assert(save_vm);

    sta_heap_init(&save_vm->heap, 4 * 1024 * 1024);
    sta_immutable_space_init(&save_vm->immutable_space, 4 * 1024 * 1024);
    sta_symbol_table_init(&save_vm->symbol_table, 512);
    sta_class_table_init(&save_vm->class_table);
    sta_stack_slab_init(&save_vm->slab, 64 * 1024);

    sta_special_objects_bind(save_vm->specials);
    sta_primitive_table_init();

    STA_BootstrapResult br = sta_bootstrap(&save_vm->heap, &save_vm->immutable_space, &save_vm->symbol_table, &save_vm->class_table);
    assert(br.status == 0);

    int rc = sta_kernel_load_all(save_vm, KERNEL_DIR);
    assert(rc == STA_OK);

    rc = sta_image_save_to_file(image_path, &save_vm->heap, &save_vm->immutable_space, &save_vm->symbol_table, &save_vm->class_table);
    assert(rc == STA_OK);

    /* Leak the save_vm — we don't need cleanup for this test. */
}

/* ── Phase 2: Fresh state + load ──────────────────────────────────── */

static void test_load_image(void) {
    load_vm = calloc(1, sizeof(STA_VM));
    assert(load_vm);

    sta_heap_init(&load_vm->heap, 4 * 1024 * 1024);
    sta_immutable_space_init(&load_vm->immutable_space, 4 * 1024 * 1024);
    sta_symbol_table_init(&load_vm->symbol_table, 512);
    sta_class_table_init(&load_vm->class_table);
    sta_stack_slab_init(&load_vm->slab, 64 * 1024);

    sta_special_objects_bind(load_vm->specials);

    int rc = sta_image_load_from_file(image_path, &load_vm->heap, &load_vm->immutable_space,
                                       &load_vm->symbol_table, &load_vm->class_table);
    assert(rc == STA_OK);

    /* Re-initialise primitive table (C function pointers are not serialised). */
    sta_primitive_table_init();
}

/* ── Phase 3: Verify special objects ──────────────────────────────── */

static void test_nil_restored(void) {
    STA_OOP nil = sta_spc_get(SPC_NIL);
    assert(nil != 0);
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)nil;
    assert(h->class_index == STA_CLS_UNDEFINEDOBJ);
}

static void test_true_restored(void) {
    STA_OOP t = sta_spc_get(SPC_TRUE);
    assert(t != 0);
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)t;
    assert(h->class_index == STA_CLS_TRUE);
}

static void test_false_restored(void) {
    STA_OOP f = sta_spc_get(SPC_FALSE);
    assert(f != 0);
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)f;
    assert(h->class_index == STA_CLS_FALSE);
}

/* ── Phase 4: Verify class table ──────────────────────────────────── */

static void test_class_table_restored(void) {
    STA_OOP si = sta_class_table_get(&load_vm->class_table, STA_CLS_SMALLINTEGER);
    assert(si != 0);

    STA_OOP arr = sta_class_table_get(&load_vm->class_table, STA_CLS_ARRAY);
    assert(arr != 0);

    STA_OOP sym = sta_class_table_get(&load_vm->class_table, STA_CLS_SYMBOL);
    assert(sym != 0);

    STA_OOP assoc = sta_class_table_get(&load_vm->class_table, STA_CLS_ASSOCIATION);
    assert(assoc != 0);

    STA_OOP md = sta_class_table_get(&load_vm->class_table, STA_CLS_METHODDICTIONARY);
    assert(md != 0);
}

/* ── Phase 5: Verify symbol table ─────────────────────────────────── */

static void test_symbol_table_rebuilt(void) {
    /* Lookup existing symbol. */
    STA_OOP plus = sta_symbol_lookup(&load_vm->symbol_table, "+", 1);
    assert(plus != 0);

    /* Lookup ifTrue:ifFalse: */
    STA_OOP iftf = sta_symbol_lookup(&load_vm->symbol_table, "ifTrue:ifFalse:", 15);
    assert(iftf != 0);

    /* Intern a new symbol — should allocate, not find existing. */
    STA_OOP hello = sta_symbol_intern(&load_vm->immutable_space, &load_vm->symbol_table, "helloNewSymbol", 14);
    assert(hello != 0);
}

/* ── Phase 6: Verify globals ──────────────────────────────────────── */

static void test_globals_restored(void) {
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);
    assert(sysdict != 0);
    STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)sysdict;
    assert(dh->class_index == STA_CLS_SYSTEMDICTIONARY);
}

/* ── Phase 7: Acid test — interpreter works ───────────────────────── */

static void test_arithmetic(void) {
    STA_OOP result = eval("3 + 4");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 7);
}

static void test_boolean_conditional(void) {
    STA_OOP result = eval("true ifTrue: [42] ifFalse: [0]");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 42);
}

static void test_multiplication(void) {
    STA_OOP result = eval("(4 * 5) + 2");
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 22);
}

static void test_comparison(void) {
    STA_OOP result = eval("3 < 5");
    assert(result == sta_spc_get(SPC_TRUE));
}

static void test_nil_identity(void) {
    STA_OOP result = eval("nil == nil");
    assert(result == sta_spc_get(SPC_TRUE));
}

/* ── Phase 8: Error path tests ────────────────────────────────────── */

static void test_load_bad_magic(void) {
    const char *tmp = "/tmp/sta_test_bad_magic.stai";
    FILE *f = fopen(tmp, "wb");
    assert(f);
    uint8_t bad[48] = {0};
    bad[0] = 'X'; bad[1] = 'X'; bad[2] = 'X'; bad[3] = 'X';
    fwrite(bad, 1, 48, f);
    fclose(f);

    STA_VM *tmp_vm = calloc(1, sizeof(STA_VM));
    sta_heap_init(&tmp_vm->heap, 4096);
    sta_immutable_space_init(&tmp_vm->immutable_space, 4096);
    sta_symbol_table_init(&tmp_vm->symbol_table, 16);
    sta_class_table_init(&tmp_vm->class_table);

    int rc = sta_image_load_from_file(tmp, &tmp_vm->heap, &tmp_vm->immutable_space, &tmp_vm->symbol_table, &tmp_vm->class_table);
    assert(rc == STA_ERR_IMAGE_MAGIC);
    unlink(tmp);

    sta_heap_deinit(&tmp_vm->heap);
    sta_immutable_space_deinit(&tmp_vm->immutable_space);
    sta_symbol_table_deinit(&tmp_vm->symbol_table);
    sta_class_table_deinit(&tmp_vm->class_table);
    free(tmp_vm);
}

static void test_load_nonexistent(void) {
    STA_VM *tmp_vm = calloc(1, sizeof(STA_VM));
    sta_heap_init(&tmp_vm->heap, 4096);
    sta_immutable_space_init(&tmp_vm->immutable_space, 4096);
    sta_symbol_table_init(&tmp_vm->symbol_table, 16);
    sta_class_table_init(&tmp_vm->class_table);

    int rc = sta_image_load_from_file("/tmp/no_such_file_xyz.stai", &tmp_vm->heap, &tmp_vm->immutable_space, &tmp_vm->symbol_table, &tmp_vm->class_table);
    assert(rc == STA_ERR_IMAGE_IO);

    sta_heap_deinit(&tmp_vm->heap);
    sta_immutable_space_deinit(&tmp_vm->immutable_space);
    sta_symbol_table_deinit(&tmp_vm->symbol_table);
    sta_class_table_deinit(&tmp_vm->class_table);
    free(tmp_vm);
}

/* ── Cleanup ──────────────────────────────────────────────────────── */

static void test_cleanup(void) {
    unlink(image_path);
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_image_roundtrip:\n");

    /* Phase 1: Bootstrap + save */
    RUN(test_save_image);

    /* Phase 2: Fresh state + load */
    RUN(test_load_image);

    /* Phase 3: Verify special objects */
    RUN(test_nil_restored);
    RUN(test_true_restored);
    RUN(test_false_restored);

    /* Phase 4: Verify class table */
    RUN(test_class_table_restored);

    /* Phase 5: Verify symbol table */
    RUN(test_symbol_table_rebuilt);

    /* Phase 6: Verify globals */
    RUN(test_globals_restored);

    /* Phase 7: Acid test — interpreter works */
    RUN(test_arithmetic);
    RUN(test_boolean_conditional);
    RUN(test_multiplication);
    RUN(test_comparison);
    RUN(test_nil_identity);

    /* Phase 8: Error paths */
    RUN(test_load_bad_magic);
    RUN(test_load_nonexistent);

    /* Cleanup */
    RUN(test_cleanup);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
