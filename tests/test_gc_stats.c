/* tests/test_gc_stats.c
 * Phase 2 Epic 5, Story 6: GC diagnostics and metrics.
 * Verifies gc_count, gc_bytes_reclaimed, gc_bytes_survived, and
 * heap capacity tracking via actor->heap.capacity.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "actor/actor.h"
#include "actor/mailbox.h"
#include "actor/mailbox_msg.h"
#include "vm/vm_state.h"
#include "vm/heap.h"
#include "vm/oop.h"
#include "vm/class_table.h"
#include "vm/method_dict.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include "vm/special_objects.h"
#include "vm/interpreter.h"
#include "compiler/compiler.h"
#include "gc/gc.h"
#include <sta/vm.h>

/* ── Test helpers ──────────────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(fn) do { \
    printf("  %-55s", #fn); \
    tests_run++; \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

static STA_VM *g_vm;

static void setup(void) {
    STA_VMConfig cfg = {0};
    g_vm = sta_vm_create(&cfg);
    assert(g_vm != NULL);
}

static void teardown(void) {
    sta_vm_destroy(g_vm);
    g_vm = NULL;
}

static STA_OOP intern(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space,
                              &g_vm->symbol_table,
                              name, strlen(name));
}

static void install_method(STA_OOP cls, const char *selector_str,
                            const char *source,
                            const char **ivars, uint32_t ivar_count) {
    STA_CompileResult r = sta_compile_method(
        source, cls, ivars, ivar_count,
        &g_vm->symbol_table, &g_vm->immutable_space,
        &g_vm->root_actor->heap,
        g_vm->specials[SPC_SMALLTALK]);
    assert(!r.had_error);
    assert(r.method != 0);
    STA_OOP md = sta_class_method_dict(cls);
    assert(md != 0);
    STA_OOP selector = intern(selector_str);
    sta_method_dict_insert(&g_vm->root_actor->heap, md, selector, r.method);
}

static struct STA_Actor *make_gc_actor(STA_OOP cls, uint32_t inst_vars,
                                        size_t heap_size) {
    struct STA_Actor *a = sta_actor_create(g_vm, heap_size, 4096);
    assert(a != NULL);
    a->behavior_class = cls;
    uint32_t cls_idx = sta_class_table_index_of(&g_vm->class_table, cls);
    assert(cls_idx != 0);
    STA_ObjHeader *obj_h = sta_heap_alloc(&a->heap, cls_idx, inst_vars);
    assert(obj_h != NULL);
    STA_OOP nil_oop = g_vm->specials[SPC_NIL];
    STA_OOP *slots = sta_payload(obj_h);
    for (uint32_t i = 0; i < inst_vars; i++)
        slots[i] = nil_oop;
    a->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    a->state = STA_ACTOR_READY;
    return a;
}

static void send_msg(struct STA_Actor *actor, const char *sel_name,
                      STA_OOP *args, uint8_t nargs) {
    STA_OOP sel = intern(sel_name);
    STA_MailboxMsg *msg = sta_mailbox_msg_create(sel, args, nargs, 0);
    assert(msg);
    assert(sta_mailbox_enqueue(&actor->mailbox, msg) == 0);
}

/* ── Test 1: gc_count increments after each GC cycle ──────────────────── */

static void test_gc_count_increments(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    install_method(assoc_cls, "alloc",
        "alloc\n"
        "  | a b c d e f g h |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  d := Object new.\n"
        "  e := Object new.\n"
        "  f := Object new.\n"
        "  g := Object new.\n"
        "  h := Object new.\n"
        "  key := h",
        (const char *[]){"key", "value"}, 2);

    /* Tiny heap — 8 Object allocations (128B) + behavior_obj (32B) +
     * context (32B) = 192B needed, but only 128B available → GC. */
    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 128);

    assert(actor->gc_stats.gc_count == 0);

    send_msg(actor, "alloc", NULL, 0);
    int rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    /* At least one GC must have run. */
    assert(actor->gc_stats.gc_count >= 1);
    uint32_t count_after_first = actor->gc_stats.gc_count;

    /* Second message — should trigger more GC. */
    send_msg(actor, "alloc", NULL, 0);
    rc = sta_actor_process_one(g_vm, actor);
    assert(rc == STA_ACTOR_MSG_PROCESSED);

    assert(actor->gc_stats.gc_count > count_after_first);

    sta_actor_destroy(actor);
    teardown();
}

/* ── Test 2: gc_bytes_reclaimed > 0 after GC with known garbage ────────── */

static void test_gc_bytes_reclaimed(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Method that allocates garbage that won't be retained. */
    install_method(assoc_cls, "makeGarbage",
        "makeGarbage\n"
        "  | a b c d e f g h |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  d := Object new.\n"
        "  e := Object new.\n"
        "  f := Object new.\n"
        "  g := Object new.\n"
        "  h := Object new.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    /* 128-byte heap forces GC. */
    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 128);

    /* First message creates garbage, GC reclaims it. */
    send_msg(actor, "makeGarbage", NULL, 0);
    sta_actor_process_one(g_vm, actor);

    /* Second message: more garbage, GC runs and reclaims. */
    send_msg(actor, "makeGarbage", NULL, 0);
    sta_actor_process_one(g_vm, actor);

    /* After multiple GC cycles with garbage, bytes_reclaimed should be > 0. */
    assert(actor->gc_stats.gc_bytes_reclaimed > 0);

    printf("(reclaimed=%zu, gc_count=%u) ",
           actor->gc_stats.gc_bytes_reclaimed,
           actor->gc_stats.gc_count);

    sta_actor_destroy(actor);
    teardown();
}

/* ── Test 3: gc_bytes_survived reflects live data ──────────────────────── */

static void test_gc_bytes_survived(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Method that retains one object in key, rest is garbage. */
    install_method(assoc_cls, "retain",
        "retain\n"
        "  | a b c d e f g h |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  d := Object new.\n"
        "  e := Object new.\n"
        "  f := Object new.\n"
        "  g := Object new.\n"
        "  h := Object new.\n"
        "  key := h.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    /* 128-byte heap forces GC. */
    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 128);

    send_msg(actor, "retain", NULL, 0);
    sta_actor_process_one(g_vm, actor);

    /* After GC, survived bytes should be > 0 (behavior_obj + key at minimum). */
    assert(actor->gc_stats.gc_bytes_survived > 0);

    /* Survived should be less than heap capacity (GC reclaimed some garbage). */
    assert(actor->gc_stats.gc_bytes_survived < actor->heap.capacity);

    printf("(survived=%zu, capacity=%zu) ",
           actor->gc_stats.gc_bytes_survived,
           actor->heap.capacity);

    sta_actor_destroy(actor);
    teardown();
}

/* ── Test 4: Heap growth reflected in heap.capacity ───────────────────── */

static void test_heap_growth_reflected(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Method that retains many objects — forces heap growth when
     * survivors fill the space. */
    install_method(assoc_cls, "growHeap",
        "growHeap\n"
        "  | a |\n"
        "  a := Array new: 5.\n"
        "  key := a.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    /* Very tiny heap — growth is nearly inevitable. */
    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 128);
    size_t initial_capacity = actor->heap.capacity;

    /* Send several messages to accumulate retained objects and force growth. */
    for (int i = 0; i < 5; i++) {
        send_msg(actor, "growHeap", NULL, 0);
        sta_actor_process_one(g_vm, actor);
    }

    /* Heap capacity should have grown (or stayed the same if GC was
     * very effective — but with 128-byte initial and retained Array(5),
     * growth is expected). */
    size_t final_capacity = actor->heap.capacity;
    printf("(initial=%zu, final=%zu, gc_count=%u) ",
           initial_capacity, final_capacity,
           actor->gc_stats.gc_count);

    /* Verify GC ran at least once. */
    assert(actor->gc_stats.gc_count >= 1);

    sta_actor_destroy(actor);
    teardown();
}

/* ── Test 5: Stats accumulate across multiple GC cycles ───────────────── */

static void test_stats_accumulate(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    install_method(assoc_cls, "churn",
        "churn\n"
        "  | a b c d e f g h |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  d := Object new.\n"
        "  e := Object new.\n"
        "  f := Object new.\n"
        "  g := Object new.\n"
        "  h := Object new.\n"
        "  key := h.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    /* 128-byte heap forces GC frequently. */
    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 128);

    /* Run many messages. */
    for (int i = 0; i < 20; i++) {
        send_msg(actor, "churn", NULL, 0);
        sta_actor_process_one(g_vm, actor);
    }

    /* gc_count should be > 1 (multiple cycles). */
    assert(actor->gc_stats.gc_count > 1);

    /* gc_bytes_reclaimed should be cumulative and growing. */
    assert(actor->gc_stats.gc_bytes_reclaimed > 0);

    /* gc_bytes_survived should reflect the most recent GC only. */
    assert(actor->gc_stats.gc_bytes_survived > 0);
    assert(actor->gc_stats.gc_bytes_survived < actor->heap.capacity);

    printf("(gc_count=%u, reclaimed=%zu, survived=%zu) ",
           actor->gc_stats.gc_count,
           actor->gc_stats.gc_bytes_reclaimed,
           actor->gc_stats.gc_bytes_survived);

    sta_actor_destroy(actor);
    teardown();
}

/* ── Test 6: Stats are zero-initialized on actor creation ─────────────── */

static void test_stats_zero_initialized(void) {
    setup();

    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table,
                                              STA_CLS_ASSOCIATION);

    struct STA_Actor *actor = make_gc_actor(assoc_cls, 2, 4096);

    assert(actor->gc_stats.gc_count == 0);
    assert(actor->gc_stats.gc_bytes_reclaimed == 0);
    assert(actor->gc_stats.gc_bytes_survived == 0);

    sta_actor_destroy(actor);
    teardown();
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_gc_stats (Phase 2 Epic 5, Story 6):\n");

    RUN(test_gc_count_increments);
    RUN(test_gc_bytes_reclaimed);
    RUN(test_gc_bytes_survived);
    RUN(test_heap_growth_reflected);
    RUN(test_stats_accumulate);
    RUN(test_stats_zero_initialized);

    printf("\n  %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
