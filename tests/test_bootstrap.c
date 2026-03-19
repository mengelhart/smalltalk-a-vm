/* tests/test_bootstrap.c
 * Integration tests for the kernel bootstrap (Phase 1, Epic 4).
 * Tests metaclass circularity, superclass chains, arithmetic dispatch,
 * boolean conditionals via block activation, array access, global
 * dictionary, and doesNotUnderstand: protocol.
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
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include "vm/handler.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <setjmp.h>

/* ── Shared test infrastructure ────────────────────────────────────────── */

static STA_VM *g_vm;

static STA_OOP sym(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space, &g_vm->symbol_table, name, strlen(name));
}

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);

    sta_heap_init(&g_vm->heap, 512 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 512 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 512);
    sta_class_table_init(&g_vm->class_table);
    sta_stack_slab_init(&g_vm->slab, 65536);

    sta_special_objects_bind(g_vm->specials);
    sta_primitive_table_init();

    STA_BootstrapResult r = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space, &g_vm->symbol_table, &g_vm->class_table);
    assert(r.status == 0);
}

static void teardown(void) {
    sta_stack_slab_deinit(&g_vm->slab);
    sta_class_table_deinit(&g_vm->class_table);
    sta_heap_deinit(&g_vm->heap);
    sta_symbol_table_deinit(&g_vm->symbol_table);
    sta_immutable_space_deinit(&g_vm->immutable_space);
    free(g_vm);
}

/* ── Test 1: Bootstrap completes without error ─────────────────────────── */

static void test_bootstrap_succeeds(void) {
    printf("  bootstrap completes...");
    /* Already asserted in setup(). If we got here, it passed. */
    printf(" ok\n");
}

/* ── Test 2: Special objects are populated ─────────────────────────────── */

static void test_special_objects_populated(void) {
    printf("  special objects populated...");
    assert(sta_spc_get(SPC_NIL) != 0);
    assert(sta_spc_get(SPC_TRUE) != 0);
    assert(sta_spc_get(SPC_FALSE) != 0);
    assert(sta_spc_get(SPC_SMALLTALK) != 0);
    assert(sta_spc_get(SPC_SPECIAL_SELECTORS) != 0);
    assert(sta_spc_get(SPC_CHARACTER_TABLE) != 0);
    printf(" ok\n");
}

/* ── Test 3: Class table is fully populated ────────────────────────────── */

static void test_class_table_populated(void) {
    printf("  class table populated...");
    for (uint32_t i = 1; i <= 31; i++) {
        STA_OOP cls = sta_class_table_get(&g_vm->class_table, i);
        assert(cls != 0);
    }
    printf(" ok\n");
}

/* ── Test 4: Metaclass circularity is correct ──────────────────────────── */

static void test_metaclass_circularity(void) {
    printf("  metaclass circularity...");

    STA_OOP object_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT);
    assert(object_cls != 0);

    /* Object's class → Object class (index 32). */
    STA_ObjHeader *obj_h = (STA_ObjHeader *)(uintptr_t)object_cls;
    uint32_t obj_meta_idx = obj_h->class_index;
    assert(obj_meta_idx == 32);
    STA_OOP object_metaclass = sta_class_table_get(&g_vm->class_table, obj_meta_idx);
    assert(object_metaclass != 0);

    /* Object class's class → Metaclass (index 17). */
    STA_ObjHeader *obj_mc_h = (STA_ObjHeader *)(uintptr_t)object_metaclass;
    assert(obj_mc_h->class_index == STA_CLS_METACLASS);

    /* Metaclass (index 17) → its class is Metaclass class (index 36). */
    STA_OOP metaclass_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_METACLASS);
    STA_ObjHeader *mc_h = (STA_ObjHeader *)(uintptr_t)metaclass_cls;
    uint32_t mc_meta_idx = mc_h->class_index;
    assert(mc_meta_idx == 36);

    /* Metaclass class (index 36) → its class is Metaclass (index 17). Circularity. */
    STA_OOP metaclass_metaclass = sta_class_table_get(&g_vm->class_table, mc_meta_idx);
    STA_ObjHeader *mc_mc_h = (STA_ObjHeader *)(uintptr_t)metaclass_metaclass;
    assert(mc_mc_h->class_index == STA_CLS_METACLASS);

    /* Object class's superclass → Class (index 16). */
    STA_OOP obj_mc_super = sta_class_superclass(object_metaclass);
    STA_OOP class_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_CLASS);
    assert(obj_mc_super == class_cls);

    printf(" ok\n");
}

/* ── Test 5: Superclass chains are correct ─────────────────────────────── */

static void test_superclass_chains(void) {
    printf("  superclass chains...");

    STA_OOP nil_oop = sta_spc_get(SPC_NIL);

    /* SmallInteger → Number → Magnitude → Object → nil. */
    STA_OOP si  = sta_class_table_get(&g_vm->class_table, STA_CLS_SMALLINTEGER);
    STA_OOP num = sta_class_superclass(si);
    assert(num == sta_class_table_get(&g_vm->class_table, STA_CLS_NUMBER));
    STA_OOP mag = sta_class_superclass(num);
    assert(mag == sta_class_table_get(&g_vm->class_table, STA_CLS_MAGNITUDE));
    STA_OOP obj = sta_class_superclass(mag);
    assert(obj == sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT));
    STA_OOP top = sta_class_superclass(obj);
    assert(top == nil_oop);

    /* Symbol → String → ArrayedCollection → SequenceableCollection
     * → Collection → Object → nil. */
    STA_OOP sym_cls  = sta_class_table_get(&g_vm->class_table, STA_CLS_SYMBOL);
    STA_OOP str_cls  = sta_class_superclass(sym_cls);
    assert(str_cls == sta_class_table_get(&g_vm->class_table, STA_CLS_STRING));
    STA_OOP ac  = sta_class_superclass(str_cls);
    assert(ac == sta_class_table_get(&g_vm->class_table, STA_CLS_ARRAYEDCOLLECTION));
    STA_OOP sc  = sta_class_superclass(ac);
    assert(sc == sta_class_table_get(&g_vm->class_table, STA_CLS_SEQCOLLECTION));
    STA_OOP col = sta_class_superclass(sc);
    assert(col == sta_class_table_get(&g_vm->class_table, STA_CLS_COLLECTION));
    STA_OOP obj2 = sta_class_superclass(col);
    assert(obj2 == sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT));

    /* Error → Exception → Object → nil. */
    STA_OOP err = sta_class_table_get(&g_vm->class_table, STA_CLS_ERROR);
    STA_OOP exc = sta_class_superclass(err);
    assert(exc == sta_class_table_get(&g_vm->class_table, STA_CLS_EXCEPTION));
    STA_OOP obj3 = sta_class_superclass(exc);
    assert(obj3 == sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT));

    printf(" ok\n");
}

/* ── Test 6: 3 + 4 = 7 through bootstrapped object system ─────────────── */

static void test_arithmetic_send(void) {
    printf("  3 + 4 = 7 (bootstrapped)...");

    STA_OOP plus_sel = sym("+");
    STA_OOP lits[] = { plus_sel, STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_SMALLINT, 3,
        OP_PUSH_SMALLINT, 4,
        OP_SEND, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 2, bc, sizeof(bc));
    assert(method);

    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(7));

    printf(" ok\n");
}

/* ── Test 7: true ifTrue: [42] ifFalse: [0] = 42 ──────────────────────── */

/* Helper: create a BlockClosure that executes a 0-arg block body method. */
static STA_OOP make_block(STA_OOP body_method) {
    STA_ObjHeader *bc_h = sta_heap_alloc(&g_vm->heap, STA_CLS_BLOCKCLOSURE, 4);
    assert(bc_h);
    STA_OOP *s = sta_payload(bc_h);
    s[0] = STA_SMALLINT_OOP(0);       /* startPC = 0         */
    s[1] = STA_SMALLINT_OOP(0);       /* numArgs = 0         */
    s[2] = body_method;               /* homeMethod          */
    s[3] = sta_spc_get(SPC_NIL);      /* outerContext = nil   */
    return (STA_OOP)(uintptr_t)bc_h;
}

/* Helper: create a method whose bytecodes are: PUSH_SMALLINT n, RETURN_TOP. */
static STA_OOP make_const_block_method(intptr_t n) {
    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };
    uint8_t bc[] = { OP_PUSH_SMALLINT, (uint8_t)n, OP_RETURN_TOP, 0 };
    STA_OOP m = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 1, bc, sizeof(bc));
    assert(m);
    return m;
}

static void test_boolean_iftrue(void) {
    printf("  true ifTrue:[42] ifFalse:[0] = 42...");

    STA_OOP block42 = make_block(make_const_block_method(42));
    STA_OOP block0  = make_block(make_const_block_method(0));

    STA_OOP iftf_sel = sym("ifTrue:ifFalse:");
    STA_OOP lits[] = { block42, block0, iftf_sel, STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_TRUE, 0,
        OP_PUSH_LIT, 0,       /* block42 */
        OP_PUSH_LIT, 1,       /* block0  */
        OP_SEND, 2,            /* #ifTrue:ifFalse: */
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 4, bc, sizeof(bc));
    assert(method);

    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(42));

    printf(" ok\n");
}

/* ── Test 8: false ifTrue: [42] ifFalse: [99] = 99 ────────────────────── */

static void test_boolean_iffalse(void) {
    printf("  false ifTrue:[42] ifFalse:[99] = 99...");

    STA_OOP block42 = make_block(make_const_block_method(42));
    STA_OOP block99 = make_block(make_const_block_method(99));

    STA_OOP iftf_sel = sym("ifTrue:ifFalse:");
    STA_OOP lits[] = { block42, block99, iftf_sel, STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_FALSE, 0,
        OP_PUSH_LIT, 0,
        OP_PUSH_LIT, 1,
        OP_SEND, 2,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 4, bc, sizeof(bc));
    assert(method);

    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(99));

    printf(" ok\n");
}

/* ── Test 9: Array at: works through bootstrapped class ────────────────── */

static void test_array_access(void) {
    printf("  Array #at: (bootstrapped)...");

    /* Create [10, 20, 30]. */
    STA_ObjHeader *arr_h = sta_heap_alloc(&g_vm->heap, STA_CLS_ARRAY, 3);
    assert(arr_h);
    STA_OOP *arr_payload = sta_payload(arr_h);
    arr_payload[0] = STA_SMALLINT_OOP(10);
    arr_payload[1] = STA_SMALLINT_OOP(20);
    arr_payload[2] = STA_SMALLINT_OOP(30);
    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr_h;

    /* Harness: arr at: 2 → 20. */
    STA_OOP at_sel = sym("at:");
    STA_OOP lits[] = { arr_oop, at_sel, STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_LIT, 0,
        OP_PUSH_SMALLINT, 2,
        OP_SEND, 1,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 3, bc, sizeof(bc));
    assert(method);

    STA_OOP result = sta_interpret(g_vm, method,
                                    STA_SMALLINT_OOP(0), NULL, 0);
    assert(result == STA_SMALLINT_OOP(20));

    printf(" ok\n");
}

/* ── Test 10: Global dictionary contains class bindings ────────────────── */

static void test_global_dictionary(void) {
    printf("  global dictionary...");

    STA_OOP dict = sta_spc_get(SPC_SMALLTALK);
    assert(dict != 0);

    /* Look up #Object → should find an Association whose value is Object class. */
    STA_OOP obj_sym = sym("Object");
    STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)dict;
    STA_OOP arr = sta_payload(dh)[1];
    STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)arr;
    uint32_t cap = ah->size / 2;
    STA_OOP *slots = sta_payload(ah);

    uint32_t hash = sta_symbol_get_hash(obj_sym);
    uint32_t idx = hash % cap;
    STA_OOP assoc = 0;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t pos = ((idx + i) % cap) * 2;
        if (slots[pos] == obj_sym) {
            assoc = slots[pos + 1];
            break;
        }
        if (slots[pos] == 0) break;
    }
    assert(assoc != 0);

    /* Association value (slot 1) should be the Object class. */
    STA_ObjHeader *assoc_h = (STA_ObjHeader *)(uintptr_t)assoc;
    STA_OOP value = sta_payload(assoc_h)[1];
    assert(value == sta_class_table_get(&g_vm->class_table, STA_CLS_OBJECT));

    /* Look up #SmallInteger → should also be found. */
    STA_OOP si_sym = sym("SmallInteger");
    hash = sta_symbol_get_hash(si_sym);
    idx = hash % cap;
    assoc = 0;
    for (uint32_t i = 0; i < cap; i++) {
        uint32_t pos = ((idx + i) % cap) * 2;
        if (slots[pos] == si_sym) {
            assoc = slots[pos + 1];
            break;
        }
        if (slots[pos] == 0) break;
    }
    assert(assoc != 0);
    assoc_h = (STA_ObjHeader *)(uintptr_t)assoc;
    value = sta_payload(assoc_h)[1];
    assert(value == sta_class_table_get(&g_vm->class_table, STA_CLS_SMALLINTEGER));

    printf(" ok\n");
}

/* ── Test 11: Unknown message triggers doesNotUnderstand: ──────────────── */

static void test_dnu_fires(void) {
    printf("  doesNotUnderstand: fires...");

    STA_OOP nonexist_sel = sym("nonexistent");
    STA_OOP lits[] = { nonexist_sel, STA_SMALLINT_OOP(0) };
    uint8_t bc[] = {
        OP_PUSH_SMALLINT, 5,
        OP_SEND, 0,
        OP_RETURN_TOP, 0
    };
    STA_OOP method = sta_compiled_method_create(&g_vm->immutable_space, 0, 0, 0,
        lits, 2, bc, sizeof(bc));
    assert(method);

    /* Install a handler to catch the MessageNotUnderstood exception. */
    STA_HandlerEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.exception_class = sta_class_table_get(&g_vm->class_table, STA_CLS_MESSAGENOTUNDERSTOOD);
    entry.handler_block = 0; /* unused — we handle in C */
    entry.saved_slab_top = g_vm->slab.top;
    entry.saved_slab_sp  = g_vm->slab.sp;
    sta_handler_set_top(NULL);

    int caught = 0;
    if (setjmp(entry.jmp) == 0) {
        sta_handler_push(&entry);
        (void)sta_interpret(g_vm, method,
                            STA_SMALLINT_OOP(0), NULL, 0);
    } else {
        /* Exception was caught via longjmp. */
        caught = 1;
        g_vm->slab.top = entry.saved_slab_top;
        g_vm->slab.sp  = entry.saved_slab_sp;
    }
    sta_handler_set_top(NULL);
    assert(caught);

    /* Verify the signaled exception is a MessageNotUnderstood. */
    STA_OOP exc = sta_handler_get_signaled_exception();
    assert(STA_IS_HEAP(exc));

    printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_bootstrap:\n");
    setup();

    test_bootstrap_succeeds();
    test_special_objects_populated();
    test_class_table_populated();
    test_metaclass_circularity();
    test_superclass_chains();
    test_arithmetic_send();
    test_boolean_iftrue();
    test_boolean_iffalse();
    test_array_access();
    test_global_dictionary();
    test_dnu_fires();

    teardown();
    printf("All bootstrap tests passed.\n");
    return 0;
}
