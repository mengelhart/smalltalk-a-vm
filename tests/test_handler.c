/* tests/test_handler.c
 * Unit tests for the exception handler stack data structure.
 * Epic 8 Story 1.
 */
#include "vm/handler.h"
#include "vm/oop.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/symbol_table.h"
#include "vm/class_table.h"
#include "vm/special_objects.h"
#include "vm/vm_state.h"
#include "bootstrap/bootstrap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_VM *g_vm;

static void setup(void) {
    g_vm = calloc(1, sizeof(STA_VM));
    assert(g_vm);

    sta_heap_init(&g_vm->heap, 1024 * 1024);
    sta_immutable_space_init(&g_vm->immutable_space, 1024 * 1024);
    sta_symbol_table_init(&g_vm->symbol_table, 256);
    sta_class_table_init(&g_vm->class_table);

    sta_special_objects_bind(g_vm->specials);

    STA_BootstrapResult br = sta_bootstrap(&g_vm->heap, &g_vm->immutable_space,
                                           &g_vm->symbol_table, &g_vm->class_table);
    assert(br.status == 0);
}

/* ── Test: empty chain ────────────────────────────────────────────────── */

static void test_empty_chain(void) {
    assert(g_vm->handler_top == NULL);
}

/* ── Test: push and pop single entry ──────────────────────────────────── */

static void test_push_pop_single(void) {
    STA_HandlerEntry entry;
    memset(&entry, 0, sizeof(entry));

    STA_OOP exc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_EXCEPTION);
    entry.exception_class = exc_cls;

    sta_handler_push(g_vm, &entry);
    assert(g_vm->handler_top == &entry);
    assert(entry.prev == NULL);

    sta_handler_pop(g_vm);
    assert(g_vm->handler_top == NULL);
}

/* ── Test: push 3 entries, walk, pop ──────────────────────────────────── */

static void test_push_three_walk_pop(void) {
    STA_OOP exc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_EXCEPTION);
    STA_OOP err_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ERROR);
    STA_OOP mnu_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_MESSAGENOTUNDERSTOOD);

    STA_HandlerEntry e1, e2, e3;
    memset(&e1, 0, sizeof(e1));
    memset(&e2, 0, sizeof(e2));
    memset(&e3, 0, sizeof(e3));

    e1.exception_class = exc_cls;       /* catches Exception (broadest) */
    e2.exception_class = err_cls;       /* catches Error */
    e3.exception_class = mnu_cls;       /* catches MessageNotUnderstood */

    sta_handler_push(g_vm, &e1);
    sta_handler_push(g_vm, &e2);
    sta_handler_push(g_vm, &e3);

    assert(g_vm->handler_top == &e3);
    assert(e3.prev == &e2);
    assert(e2.prev == &e1);
    assert(e1.prev == NULL);

    /* Pop all. */
    sta_handler_pop(g_vm);
    assert(g_vm->handler_top == &e2);
    sta_handler_pop(g_vm);
    assert(g_vm->handler_top == &e1);
    sta_handler_pop(g_vm);
    assert(g_vm->handler_top == NULL);
}

/* ── Test: find matches correct entry ────────────────────────────────── */

static void test_find_exact_match(void) {
    STA_OOP exc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_EXCEPTION);
    STA_OOP err_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ERROR);

    STA_HandlerEntry e1, e2;
    memset(&e1, 0, sizeof(e1));
    memset(&e2, 0, sizeof(e2));

    e1.exception_class = exc_cls;
    e2.exception_class = err_cls;

    sta_handler_push(g_vm, &e1);
    sta_handler_push(g_vm, &e2);

    /* Create an Error instance — should match e2 (Error handler). */
    STA_ObjHeader *err_h = sta_heap_alloc(&g_vm->heap, STA_CLS_ERROR, 2);
    assert(err_h);
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    sta_payload(err_h)[0] = nil_oop;
    sta_payload(err_h)[1] = nil_oop;
    STA_OOP err_inst = (STA_OOP)(uintptr_t)err_h;

    STA_HandlerEntry *found = sta_handler_find(g_vm, err_inst);
    assert(found == &e2);

    sta_handler_pop(g_vm);
    sta_handler_pop(g_vm);
}

/* ── Test: find with superclass match ────────────────────────────────── */

static void test_find_superclass_match(void) {
    STA_OOP exc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_EXCEPTION);

    STA_HandlerEntry e1;
    memset(&e1, 0, sizeof(e1));
    e1.exception_class = exc_cls;

    sta_handler_push(g_vm, &e1);

    /* Create an Error instance — should match Exception handler
     * because Error is a subclass of Exception. */
    STA_ObjHeader *err_h = sta_heap_alloc(&g_vm->heap, STA_CLS_ERROR, 2);
    assert(err_h);
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    sta_payload(err_h)[0] = nil_oop;
    sta_payload(err_h)[1] = nil_oop;
    STA_OOP err_inst = (STA_OOP)(uintptr_t)err_h;

    STA_HandlerEntry *found = sta_handler_find(g_vm, err_inst);
    assert(found == &e1);

    sta_handler_pop(g_vm);
}

/* ── Test: find no match → NULL ──────────────────────────────────────── */

static void test_find_no_match(void) {
    STA_OOP mnu_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_MESSAGENOTUNDERSTOOD);

    STA_HandlerEntry e1;
    memset(&e1, 0, sizeof(e1));
    e1.exception_class = mnu_cls;

    sta_handler_push(g_vm, &e1);

    /* Create a plain Exception instance — should NOT match MNU handler
     * because Exception is not a subclass of MessageNotUnderstood. */
    STA_ObjHeader *exc_h = sta_heap_alloc(&g_vm->heap, STA_CLS_EXCEPTION, 2);
    assert(exc_h);
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    sta_payload(exc_h)[0] = nil_oop;
    sta_payload(exc_h)[1] = nil_oop;
    STA_OOP exc_inst = (STA_OOP)(uintptr_t)exc_h;

    STA_HandlerEntry *found = sta_handler_find(g_vm, exc_inst);
    assert(found == NULL);

    sta_handler_pop(g_vm);
}

/* ── Test: signal exception storage ──────────────────────────────────── */

static void test_signal_exception_storage(void) {
    STA_OOP val = STA_SMALLINT_OOP(42);
    sta_handler_set_signaled_exception(g_vm, val);
    assert(sta_handler_get_signaled_exception(g_vm) == val);

    sta_handler_set_signaled_exception(g_vm, 0);
    assert(sta_handler_get_signaled_exception(g_vm) == 0);
}

/* ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    setup();
    printf("test_handler:\n");

    RUN(test_empty_chain);
    RUN(test_push_pop_single);
    RUN(test_push_three_walk_pop);
    RUN(test_find_exact_match);
    RUN(test_find_superclass_match);
    RUN(test_find_no_match);
    RUN(test_signal_exception_storage);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
