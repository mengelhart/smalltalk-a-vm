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
#include "bootstrap/bootstrap.h"
#include "bootstrap/kernel_load.h"
#include "compiler/compiler.h"
#include "image/image.h"
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

/* ── State ──────────────────────────────────────────────────────────── */

static const char *image_path = "/tmp/sta_test_roundtrip.stai";

/* Post-load state (used by all verification tests). */
static STA_Heap           *load_heap;
static STA_ImmutableSpace *load_imm;
static STA_SymbolTable    *load_syms;
static STA_ClassTable     *load_ct;
static STA_StackSlab      *load_slab;

/* Helper: evaluate an expression after image load. */
static STA_OOP eval(const char *source) {
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);
    STA_CompileResult cr = sta_compile_expression(
        source, load_syms, load_imm, load_heap, sysdict);
    if (cr.had_error) {
        fprintf(stderr, "eval compile error: %s\n  source: %s\n",
                cr.error_msg, source);
        assert(!cr.had_error);
    }
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    return sta_interpret(load_slab, load_heap, load_ct, cr.method, nil_oop, NULL, 0);
}

/* ── Phase 1: Bootstrap + save ────────────────────────────────────── */

static void test_save_image(void) {
    STA_Heap *heap = sta_heap_create(4 * 1024 * 1024);
    STA_ImmutableSpace *imm = sta_immutable_space_create(4 * 1024 * 1024);
    STA_SymbolTable *syms = sta_symbol_table_create(512);
    STA_ClassTable *ct = sta_class_table_create();

    STA_BootstrapResult br = sta_bootstrap(heap, imm, syms, ct);
    assert(br.status == 0);

    int rc = sta_kernel_load_all(KERNEL_DIR);
    assert(rc == STA_OK);

    rc = sta_image_save_to_file(image_path, heap, imm, syms, ct);
    assert(rc == STA_OK);

    /* Don't destroy — we don't need this state anymore, but destroying
     * would unmap immutable space which might cause issues if the test
     * binary is somehow referencing it. Just leak for this test. */
}

/* ── Phase 2: Fresh state + load ──────────────────────────────────── */

static void test_load_image(void) {
    load_heap = sta_heap_create(4 * 1024 * 1024);
    load_imm  = sta_immutable_space_create(4 * 1024 * 1024);
    load_syms = sta_symbol_table_create(512);
    load_ct   = sta_class_table_create();
    load_slab = sta_stack_slab_create(64 * 1024);

    int rc = sta_image_load_from_file(image_path, load_heap, load_imm,
                                       load_syms, load_ct);
    assert(rc == STA_OK);

    /* Re-initialise primitive table (C function pointers are not serialised). */
    sta_primitive_table_init();
    sta_primitive_set_class_table(load_ct);
    sta_primitive_set_heap(load_heap);
    sta_primitive_set_symbol_table(load_syms);
    sta_primitive_set_immutable_space(load_imm);
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
    STA_OOP si = sta_class_table_get(load_ct, STA_CLS_SMALLINTEGER);
    assert(si != 0);

    STA_OOP arr = sta_class_table_get(load_ct, STA_CLS_ARRAY);
    assert(arr != 0);

    STA_OOP sym = sta_class_table_get(load_ct, STA_CLS_SYMBOL);
    assert(sym != 0);

    STA_OOP assoc = sta_class_table_get(load_ct, STA_CLS_ASSOCIATION);
    assert(assoc != 0);

    STA_OOP md = sta_class_table_get(load_ct, STA_CLS_METHODDICTIONARY);
    assert(md != 0);
}

/* ── Phase 5: Verify symbol table ─────────────────────────────────── */

static void test_symbol_table_rebuilt(void) {
    /* Lookup existing symbol. */
    STA_OOP plus = sta_symbol_lookup(load_syms, "+", 1);
    assert(plus != 0);

    /* Lookup ifTrue:ifFalse: */
    STA_OOP iftf = sta_symbol_lookup(load_syms, "ifTrue:ifFalse:", 15);
    assert(iftf != 0);

    /* Intern a new symbol — should allocate, not find existing. */
    STA_OOP hello = sta_symbol_intern(load_imm, load_syms, "helloNewSymbol", 14);
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

    STA_Heap *h = sta_heap_create(4096);
    STA_ImmutableSpace *s = sta_immutable_space_create(4096);
    STA_SymbolTable *st = sta_symbol_table_create(16);
    STA_ClassTable *c = sta_class_table_create();
    int rc = sta_image_load_from_file(tmp, h, s, st, c);
    assert(rc == STA_ERR_IMAGE_MAGIC);
    unlink(tmp);
    sta_heap_destroy(h);
    sta_immutable_space_destroy(s);
    sta_symbol_table_destroy(st);
    sta_class_table_destroy(c);
}

static void test_load_nonexistent(void) {
    STA_Heap *h = sta_heap_create(4096);
    STA_ImmutableSpace *s = sta_immutable_space_create(4096);
    STA_SymbolTable *st = sta_symbol_table_create(16);
    STA_ClassTable *c = sta_class_table_create();
    int rc = sta_image_load_from_file("/tmp/no_such_file_xyz.stai", h, s, st, c);
    assert(rc == STA_ERR_IMAGE_IO);
    sta_heap_destroy(h);
    sta_immutable_space_destroy(s);
    sta_symbol_table_destroy(st);
    sta_class_table_destroy(c);
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
