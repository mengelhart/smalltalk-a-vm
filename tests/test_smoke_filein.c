/* tests/test_smoke_filein.c
 * Smoke test: load a .st file with class + methods, exercise via interpreter.
 * Phase 1, Epic 9, Story 4.
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
#include "vm/format.h"
#include "bootstrap/bootstrap.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_ImmutableSpace *imm;
static STA_SymbolTable    *syms;
static STA_Heap           *heap;
static STA_ClassTable     *ct;
static STA_StackSlab      *slab;

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static void setup(void) {
    heap = sta_heap_create(4 * 1024 * 1024);
    imm  = sta_immutable_space_create(4 * 1024 * 1024);
    syms = sta_symbol_table_create(512);
    ct   = sta_class_table_create();
    slab = sta_stack_slab_create(64 * 1024);

    STA_BootstrapResult br = sta_bootstrap(heap, imm, syms, ct);
    assert(br.status == 0);
}

/* Helper: look up a class in SystemDictionary by name. */
static STA_OOP sysdict_lookup(const char *name) {
    STA_OOP dict = sta_spc_get(SPC_SMALLTALK);
    STA_OOP name_sym = sta_symbol_intern(imm, syms, name, strlen(name));
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

/* ── Tests ─────────────────────────────────────────────────────────────── */

static STA_OOP tp_cls;

static void test_load_smoke(void) {
    int rc = sta_vm_load_source(NULL, FIXTURES_DIR "/smoke.st");
    if (rc != STA_OK) {
        fprintf(stderr, "load error: %s\n", sta_vm_last_error(NULL));
    }
    assert(rc == STA_OK);

    tp_cls = sysdict_lookup("TestPoint");
    assert(tp_cls != 0);
}

static void test_create_instance(void) {
    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)tp_cls;
    STA_OOP fmt = sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];
    assert(STA_FORMAT_INST_VARS(fmt) == 2);

    STA_OOP prim_args[1] = { tp_cls };
    STA_OOP instance = 0;
    int prc = sta_primitives[31](prim_args, 0, &instance);
    assert(prc == 0);
    assert(instance != 0);
    assert(!STA_IS_IMMEDIATE(instance));
}

static void test_setter_getter(void) {
    /* Create instance via primitives, call methods via interpreter. */
    STA_OOP prim_args[1] = { tp_cls };
    STA_OOP instance = 0;
    int prc = sta_primitives[31](prim_args, 0, &instance);
    assert(prc == 0);

    STA_OOP md = sta_class_method_dict(tp_cls);

    /* Call x: with argument 3 */
    STA_OOP sel_x_put = sta_symbol_intern(imm, syms, "x:", 2);
    STA_OOP method_x_put = sta_method_dict_lookup(md, sel_x_put);
    assert(method_x_put != 0);

    STA_OOP arg3 = STA_SMALLINT_OOP(3);
    sta_primitive_set_slab(slab);
    sta_interpret(slab, heap, ct, method_x_put, instance, &arg3, 1);

    /* Call x getter — should return 3 */
    STA_OOP sel_x = sta_symbol_intern(imm, syms, "x", 1);
    STA_OOP method_x = sta_method_dict_lookup(md, sel_x);
    assert(method_x != 0);

    STA_OOP result = sta_interpret(slab, heap, ct, method_x, instance, NULL, 0);
    assert(STA_IS_SMALLINT(result));
    assert(STA_SMALLINT_VAL(result) == 3);

    /* Call y: with argument 7, then y getter */
    STA_OOP sel_y_put = sta_symbol_intern(imm, syms, "y:", 2);
    STA_OOP method_y_put = sta_method_dict_lookup(md, sel_y_put);
    assert(method_y_put != 0);

    STA_OOP arg7 = STA_SMALLINT_OOP(7);
    sta_interpret(slab, heap, ct, method_y_put, instance, &arg7, 1);

    STA_OOP sel_y = sta_symbol_intern(imm, syms, "y", 1);
    STA_OOP method_y = sta_method_dict_lookup(md, sel_y);
    assert(method_y != 0);

    STA_OOP ry = sta_interpret(slab, heap, ct, method_y, instance, NULL, 0);
    assert(STA_IS_SMALLINT(ry));
    assert(STA_SMALLINT_VAL(ry) == 7);
}

/* TODO: test_accessors_via_eval and test_distance_squared deferred
 * to next session — requires fixing compile_expression handling of
 * multi-statement keyword sends via the DNU/eval path. */

static void test_class_in_sysdict(void) {
    STA_OOP cls = sysdict_lookup("TestPoint");
    assert(cls != 0);
    assert(cls == tp_cls);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    setup();
    printf("test_smoke_filein:\n");

    RUN(test_load_smoke);
    RUN(test_create_instance);
    RUN(test_setter_getter);
    RUN(test_class_in_sysdict);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);

    sta_stack_slab_destroy(slab);
    sta_class_table_destroy(ct);
    sta_heap_destroy(heap);
    sta_symbol_table_destroy(syms);
    sta_immutable_space_destroy(imm);
    return (tests_passed == tests_run) ? 0 : 1;
}
