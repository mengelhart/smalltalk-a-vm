/* tests/test_class_creation.c
 * Tests for the class creation primitive (prim 122).
 * Phase 1, Epic 9, Story 1.
 */
#include "vm/interpreter.h"
#include "vm/compiled_method.h"
#include "vm/primitive_table.h"
#include "vm/method_dict.h"
#include "vm/symbol_table.h"
#include "vm/special_objects.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/format.h"
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include "compiler/compiler.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_VM *g_vm;
static STA_ExecContext g_ctx;

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

static STA_OOP sym(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space, &g_vm->symbol_table, name, strlen(name));
}

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);
    sta_heap_init(&g_vm->heap, 4 * 1024 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 4 * 1024 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 512);
    sta_class_table_init(&g_vm->class_table);
    sta_stack_slab_init(&g_vm->slab, 64 * 1024);
    sta_special_objects_bind(g_vm->specials);

    STA_BootstrapResult br = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space,
                                            &g_vm->symbol_table, &g_vm->class_table);
    assert(br.status == 0);
    sta_primitive_table_init();
    g_ctx.vm = g_vm;
    g_ctx.actor = NULL;
}

/* Helper: look up a class in SystemDictionary by name. */
static STA_OOP sysdict_lookup(const char *name) {
    STA_OOP dict = sta_spc_get(SPC_SMALLTALK);
    STA_OOP name_sym = sym(name);
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

/* Helper: call the class creation primitive directly. */
static STA_OOP create_class_via_prim(STA_OOP superclass,
                                       const char *name,
                                       const char *ivar_names) {
    STA_OOP name_sym = sym(name);

    /* Create a String for instvar names. */
    size_t len = strlen(ivar_names);
    uint32_t words = (uint32_t)((len + sizeof(STA_OOP) - 1) / sizeof(STA_OOP));
    if (words == 0 && len == 0) words = 0;
    STA_ObjHeader *str_h = sta_heap_alloc(&g_vm->heap, STA_CLS_STRING, words);
    assert(str_h);
    if (len > 0) {
        memcpy(sta_payload(str_h), ivar_names, len);
    }
    uint32_t padding = words * (uint32_t)sizeof(STA_OOP) - (uint32_t)len;
    str_h->reserved = (uint16_t)(padding & STA_BYTE_PADDING_MASK);
    STA_OOP ivar_str = (STA_OOP)(uintptr_t)str_h;

    /* Create empty strings for classVarNames, poolDicts, category. */
    STA_ObjHeader *empty_h = sta_heap_alloc(&g_vm->heap, STA_CLS_STRING, 0);
    assert(empty_h);
    STA_OOP empty_str = (STA_OOP)(uintptr_t)empty_h;

    STA_OOP prim_args[6] = {
        superclass, name_sym, ivar_str, empty_str, empty_str, empty_str
    };
    STA_OOP result = 0;
    int rc = sta_primitives[122](&g_ctx, prim_args, 5, &result);
    assert(rc == STA_PRIM_SUCCESS);
    return result;
}

/* ── Test 1: Create a class via the primitive ──────────────────────────── */

static void test_create_class_basic(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP point = create_class_via_prim(obj_cls, "TestPoint", "x y");
    assert(point != 0);
    assert(STA_IS_HEAP(point));
}

/* ── Test 2: Class exists in class table ───────────────────────────────── */

static void test_class_in_class_table(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP point = create_class_via_prim(obj_cls, "TestPoint2", "x y");
    uint32_t idx = sta_class_table_index_of(&g_vm->class_table, point);
    assert(idx != 0);
    assert(sta_class_table_get(&g_vm->class_table, idx) == point);
}

/* ── Test 3: Superclass is correct ─────────────────────────────────────── */

static void test_superclass_correct(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP point = create_class_via_prim(obj_cls, "TestPoint3", "x y");
    STA_OOP super = sta_class_superclass(point);
    assert(super == obj_cls);
}

/* ── Test 4: Instance variable count is correct ────────────────────────── */

static void test_instvar_count(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP point = create_class_via_prim(obj_cls, "TestPoint4", "x y");

    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)point;
    STA_OOP fmt = sta_payload(cls_h)[STA_CLASS_SLOT_FORMAT];
    uint8_t ivars = STA_FORMAT_INST_VARS(fmt);
    assert(ivars == 2);
}

/* ── Test 5: New instance has nil default slots ────────────────────────── */

static void test_new_instance_nil_slots(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP point = create_class_via_prim(obj_cls, "TestPoint5", "x y");

    /* Create an instance via basicNew (prim 31). */
    STA_OOP prim_args[1] = { point };
    STA_OOP instance = 0;
    int rc = sta_primitives[31](&g_ctx, prim_args, 0, &instance);
    assert(rc == STA_PRIM_SUCCESS);
    assert(instance != 0);

    /* Check instVarAt:1 and instVarAt:2 are nil. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_ObjHeader *inst_h = (STA_ObjHeader *)(uintptr_t)instance;
    STA_OOP *slots = sta_payload(inst_h);
    assert(slots[0] == nil_oop);
    assert(slots[1] == nil_oop);
}

/* ── Test 6: Class is in SystemDictionary ──────────────────────────────── */

static void test_class_in_sysdict(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP point = create_class_via_prim(obj_cls, "TestPoint6", "x y");
    STA_OOP found = sysdict_lookup("TestPoint6");
    assert(found == point);
}

/* ── Test 7: Inheriting instvars from superclass ───────────────────────── */

static void test_inherited_instvars(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    /* Create a class with 2 instvars. */
    STA_OOP base = create_class_via_prim(obj_cls, "TestBase", "a b");
    /* Create a subclass with 1 more instvar. */
    STA_OOP sub = create_class_via_prim(base, "TestSub", "c");

    STA_ObjHeader *sub_h = (STA_ObjHeader *)(uintptr_t)sub;
    STA_OOP fmt = sta_payload(sub_h)[STA_CLASS_SLOT_FORMAT];
    uint8_t ivars = STA_FORMAT_INST_VARS(fmt);
    assert(ivars == 3);  /* 2 inherited + 1 own */
}

/* ── Test 8: Metaclass wiring ──────────────────────────────────────────── */

static void test_metaclass_wiring(void) {
    STA_OOP obj_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    STA_OOP point = create_class_via_prim(obj_cls, "TestPoint8", "x y");

    /* The class's class_index should point to its metaclass. */
    STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)point;
    uint32_t meta_idx = cls_h->class_index;
    STA_OOP meta = sta_class_table_get(&g_vm->class_table, meta_idx);
    assert(meta != 0);

    /* The metaclass's class_index should be STA_CLS_METACLASS. */
    STA_ObjHeader *meta_h = (STA_ObjHeader *)(uintptr_t)meta;
    assert(meta_h->class_index == STA_CLS_METACLASS);

    /* The metaclass's superclass should be Object's metaclass. */
    STA_OOP meta_super = sta_class_superclass(meta);
    STA_ObjHeader *obj_h = (STA_ObjHeader *)(uintptr_t)obj_cls;
    STA_OOP obj_meta = sta_class_table_get(&g_vm->class_table, obj_h->class_index);
    assert(meta_super == obj_meta);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    setup();
    printf("test_class_creation:\n");

    RUN(test_create_class_basic);
    RUN(test_class_in_class_table);
    RUN(test_superclass_correct);
    RUN(test_instvar_count);
    RUN(test_new_instance_nil_slots);
    RUN(test_class_in_sysdict);
    RUN(test_inherited_instvars);
    RUN(test_metaclass_wiring);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);

    sta_stack_slab_deinit(&g_vm->slab);
    sta_class_table_deinit(&g_vm->class_table);
    sta_heap_deinit(&g_vm->heap);
    sta_symbol_table_deinit(&g_vm->symbol_table);
    sta_immutable_space_deinit(&g_vm->immutable_space);
    sta_special_objects_bind(NULL);
    free(g_vm);
    return (tests_passed == tests_run) ? 0 : 1;
}
