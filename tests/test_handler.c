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
#include "bootstrap/bootstrap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

/* ── Shared infrastructure ───────────────────────────────────────────── */

static STA_Heap *heap;
static STA_ImmutableSpace *imm;
static STA_SymbolTable *syms;
static STA_ClassTable *ct;

static void setup(void) {
    heap = sta_heap_create(1024 * 1024);
    imm  = sta_immutable_space_create(1024 * 1024);
    syms = sta_symbol_table_create(256);
    ct   = sta_class_table_create();

    STA_BootstrapResult br = sta_bootstrap(heap, imm, syms, ct);
    assert(br.status == 0);

    /* Reset handler chain to clean state. */
    sta_handler_set_top(NULL);
}

/* ── Test: empty chain ────────────────────────────────────────────────── */

static void test_empty_chain(void) {
    assert(sta_handler_top() == NULL);
}

/* ── Test: push and pop single entry ──────────────────────────────────── */

static void test_push_pop_single(void) {
    STA_HandlerEntry entry;
    memset(&entry, 0, sizeof(entry));

    STA_OOP exc_cls = sta_class_table_get(ct, STA_CLS_EXCEPTION);
    entry.exception_class = exc_cls;

    sta_handler_push(&entry);
    assert(sta_handler_top() == &entry);
    assert(entry.prev == NULL);

    sta_handler_pop();
    assert(sta_handler_top() == NULL);
}

/* ── Test: push 3 entries, walk, pop ──────────────────────────────────── */

static void test_push_three_walk_pop(void) {
    STA_OOP exc_cls = sta_class_table_get(ct, STA_CLS_EXCEPTION);
    STA_OOP err_cls = sta_class_table_get(ct, STA_CLS_ERROR);
    STA_OOP mnu_cls = sta_class_table_get(ct, STA_CLS_MESSAGENOTUNDERSTOOD);

    STA_HandlerEntry e1, e2, e3;
    memset(&e1, 0, sizeof(e1));
    memset(&e2, 0, sizeof(e2));
    memset(&e3, 0, sizeof(e3));

    e1.exception_class = exc_cls;       /* catches Exception (broadest) */
    e2.exception_class = err_cls;       /* catches Error */
    e3.exception_class = mnu_cls;       /* catches MessageNotUnderstood */

    sta_handler_push(&e1);
    sta_handler_push(&e2);
    sta_handler_push(&e3);

    assert(sta_handler_top() == &e3);
    assert(e3.prev == &e2);
    assert(e2.prev == &e1);
    assert(e1.prev == NULL);

    /* Pop all. */
    sta_handler_pop();
    assert(sta_handler_top() == &e2);
    sta_handler_pop();
    assert(sta_handler_top() == &e1);
    sta_handler_pop();
    assert(sta_handler_top() == NULL);
}

/* ── Test: find matches correct entry ────────────────────────────────── */

static void test_find_exact_match(void) {
    STA_OOP exc_cls = sta_class_table_get(ct, STA_CLS_EXCEPTION);
    STA_OOP err_cls = sta_class_table_get(ct, STA_CLS_ERROR);

    STA_HandlerEntry e1, e2;
    memset(&e1, 0, sizeof(e1));
    memset(&e2, 0, sizeof(e2));

    e1.exception_class = exc_cls;
    e2.exception_class = err_cls;

    sta_handler_push(&e1);
    sta_handler_push(&e2);

    /* Create an Error instance — should match e2 (Error handler). */
    STA_ObjHeader *err_h = sta_heap_alloc(heap, STA_CLS_ERROR, 2);
    assert(err_h);
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    sta_payload(err_h)[0] = nil_oop;
    sta_payload(err_h)[1] = nil_oop;
    STA_OOP err_inst = (STA_OOP)(uintptr_t)err_h;

    STA_HandlerEntry *found = sta_handler_find(err_inst, ct);
    assert(found == &e2);

    sta_handler_pop();
    sta_handler_pop();
}

/* ── Test: find with superclass match ────────────────────────────────── */

static void test_find_superclass_match(void) {
    STA_OOP exc_cls = sta_class_table_get(ct, STA_CLS_EXCEPTION);

    STA_HandlerEntry e1;
    memset(&e1, 0, sizeof(e1));
    e1.exception_class = exc_cls;

    sta_handler_push(&e1);

    /* Create an Error instance — should match Exception handler
     * because Error is a subclass of Exception. */
    STA_ObjHeader *err_h = sta_heap_alloc(heap, STA_CLS_ERROR, 2);
    assert(err_h);
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    sta_payload(err_h)[0] = nil_oop;
    sta_payload(err_h)[1] = nil_oop;
    STA_OOP err_inst = (STA_OOP)(uintptr_t)err_h;

    STA_HandlerEntry *found = sta_handler_find(err_inst, ct);
    assert(found == &e1);

    sta_handler_pop();
}

/* ── Test: find no match → NULL ──────────────────────────────────────── */

static void test_find_no_match(void) {
    STA_OOP mnu_cls = sta_class_table_get(ct, STA_CLS_MESSAGENOTUNDERSTOOD);

    STA_HandlerEntry e1;
    memset(&e1, 0, sizeof(e1));
    e1.exception_class = mnu_cls;

    sta_handler_push(&e1);

    /* Create a plain Exception instance — should NOT match MNU handler
     * because Exception is not a subclass of MessageNotUnderstood. */
    STA_ObjHeader *exc_h = sta_heap_alloc(heap, STA_CLS_EXCEPTION, 2);
    assert(exc_h);
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    sta_payload(exc_h)[0] = nil_oop;
    sta_payload(exc_h)[1] = nil_oop;
    STA_OOP exc_inst = (STA_OOP)(uintptr_t)exc_h;

    STA_HandlerEntry *found = sta_handler_find(exc_inst, ct);
    assert(found == NULL);

    sta_handler_pop();
}

/* ── Test: signal exception storage ──────────────────────────────────── */

static void test_signal_exception_storage(void) {
    STA_OOP val = STA_SMALLINT_OOP(42);
    sta_handler_set_signaled_exception(val);
    assert(sta_handler_get_signaled_exception() == val);

    sta_handler_set_signaled_exception(0);
    assert(sta_handler_get_signaled_exception() == 0);
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
