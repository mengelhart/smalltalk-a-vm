/* tests/test_load_source.c
 * Tests for sta_vm_load_source() (Phase 1, Epic 9, Story 3).
 */
#include <sta/vm.h>
#include "vm/interpreter.h"
#include "vm/primitive_table.h"
#include "vm/method_dict.h"
#include "vm/symbol_table.h"
#include "vm/special_objects.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/frame.h"
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_VM *g_vm;
static STA_ExecContext g_ectx;

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);

    sta_heap_init(&g_vm->heap, 4 * 1024 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 4 * 1024 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 512);
    sta_class_table_init(&g_vm->class_table);
    sta_stack_slab_init(&g_vm->slab, 64 * 1024);

    sta_special_objects_bind(g_vm->specials);
    sta_primitive_table_init();
    g_ectx.vm = g_vm;
    g_ectx.actor = NULL;

    STA_BootstrapResult br = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space, &g_vm->symbol_table, &g_vm->class_table);
    assert(br.status == 0);
    g_vm->bootstrapped = true;
}

/* Helper: look up a class in SystemDictionary by name. */
static STA_OOP sysdict_lookup(const char *name) {
    STA_OOP dict = sta_spc_get(SPC_SMALLTALK);
    STA_OOP name_sym = sta_symbol_intern(&g_vm->immutable_space, &g_vm->symbol_table, name, strlen(name));
    STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)dict;
    STA_OOP arr = sta_payload(dh)[1];
    STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)arr;
    uint32_t cap = ah->size / 2;
    STA_OOP *slots = sta_payload(ah);
    uint32_t hash = sta_symbol_get_hash(name_sym);
    uint32_t idx = hash % cap;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t pos = ((idx + i) % cap) * 2;
        if (slots[pos] == name_sym) {
            STA_OOP assoc = slots[pos + 1];
            return sta_payload((STA_ObjHeader *)(uintptr_t)assoc)[1];
        }
        if (slots[pos] == 0) break;
    }
    return 0;
}

/* ── Test 1: Load .st file via public API ──────────────────────────────── */

static void test_load_source_simple(void) {
    int rc = sta_vm_load_source(g_vm, FIXTURES_DIR "/simple.st");
    if (rc != STA_OK) {
        fprintf(stderr, "load_source error: %s\n", sta_vm_last_error(g_vm));
    }
    assert(rc == STA_OK);

    /* Verify class was created and method is callable. */
    STA_OOP cls = sysdict_lookup("SimpleTestClass");
    assert(cls != 0);

    STA_OOP sel = sta_symbol_intern(&g_vm->immutable_space, &g_vm->symbol_table, "answer", 6);
    STA_OOP md = sta_class_method_dict(cls);
    STA_OOP method = sta_method_dict_lookup(md, sel);
    assert(method != 0);

    /* Create instance, call method, verify result. */
    STA_OOP prim_args[1] = { cls };
    STA_OOP instance = 0;
    int prc = sta_primitives[31](&g_ectx, prim_args, 0, &instance);
    assert(prc == 0);

    STA_OOP result = sta_interpret(g_vm, method, instance, NULL, 0);
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 42);
}

/* ── Test 2: Nonexistent file returns STA_ERR_IO ──────────────────────── */

static void test_load_source_io_error(void) {
    int rc = sta_vm_load_source(g_vm, "/nonexistent/path.st");
    assert(rc == STA_ERR_IO);

    const char *err = sta_vm_last_error(g_vm);
    assert(strlen(err) > 0);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    setup();
    printf("test_load_source:\n");

    RUN(test_load_source_simple);
    RUN(test_load_source_io_error);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);

    sta_stack_slab_deinit(&g_vm->slab);
    sta_class_table_deinit(&g_vm->class_table);
    sta_heap_deinit(&g_vm->heap);
    sta_symbol_table_deinit(&g_vm->symbol_table);
    sta_immutable_space_deinit(&g_vm->immutable_space);
    free(g_vm);
    return (tests_passed == tests_run) ? 0 : 1;
}
