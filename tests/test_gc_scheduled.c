/* tests/test_gc_scheduled.c
 * Phase 2 Epic 5, Story 5: Scheduled actor GC — multi-threaded correctness.
 *
 * Tests GC under the real work-stealing scheduler with multiple actors
 * running concurrently on multiple threads.
 *
 * Key properties verified:
 *   - GC is per-actor — no cross-actor data races
 *   - GC only reads shared structures (immutable space, class table)
 *   - Retained objects survive GC and are correct after message processing
 *   - No corruption under concurrent GC across multiple scheduler threads
 *   - ASan and TSan clean
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#include "scheduler/scheduler.h"
#include "actor/actor.h"
#include "vm/vm_state.h"
#include "vm/interpreter.h"
#include "vm/special_objects.h"
#include "vm/symbol_table.h"
#include "vm/method_dict.h"
#include "vm/class_table.h"
#include "compiler/compiler.h"
#include "gc/gc.h"
#include <sta/vm.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(fn) do { \
    printf("  %-55s", #fn); \
    fflush(stdout); \
    tests_run++; \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

static STA_VM *make_vm(void) {
    STA_VMConfig cfg = { .image_path = NULL };
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);
    return vm;
}

static STA_OOP intern(STA_VM *vm, const char *name) {
    return sta_symbol_intern(
        &vm->immutable_space, &vm->symbol_table, name, strlen(name));
}

/* Create a child actor with a small heap to force GC, using Association
 * class (2 instVars: key, value) for state retention tests. */
static struct STA_Actor *make_gc_actor(STA_VM *vm, STA_OOP cls,
                                        uint32_t inst_vars,
                                        size_t heap_size) {
    struct STA_Actor *a = sta_actor_create(vm, heap_size, 4096);
    assert(a != NULL);
    a->behavior_class = cls;
    uint32_t cls_idx = sta_class_table_index_of(&vm->class_table, cls);
    assert(cls_idx != 0);
    STA_ObjHeader *obj_h = sta_heap_alloc(&a->heap, cls_idx, inst_vars);
    assert(obj_h != NULL);
    STA_OOP nil_oop = vm->specials[SPC_NIL];
    STA_OOP *slots = sta_payload(obj_h);
    for (uint32_t i = 0; i < inst_vars; i++)
        slots[i] = nil_oop;
    a->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    sta_actor_register(a);
    return a;
}

static void install_method_on(STA_VM *vm, STA_OOP cls,
                               const char *selector_name, const char *src,
                               const char **ivars, uint32_t ivar_count) {
    STA_OOP md = sta_class_method_dict(cls);
    if (md == 0) {
        md = sta_method_dict_create(&vm->root_actor->heap, 8);
        assert(md != 0);
        STA_ObjHeader *cls_h = (STA_ObjHeader *)(uintptr_t)cls;
        sta_payload(cls_h)[STA_CLASS_SLOT_METHODDICT] = md;
    }
    STA_CompileResult cr = sta_compile_method(
        src, cls, ivars, ivar_count,
        &vm->symbol_table, &vm->immutable_space,
        &vm->root_actor->heap, vm->specials[SPC_SMALLTALK]);
    if (cr.had_error) {
        fprintf(stderr, "compile error: %s\nsource: %s\n", cr.error_msg, src);
    }
    assert(!cr.had_error);
    assert(cr.method != 0);
    STA_OOP sel = intern(vm, selector_name);
    sta_method_dict_insert(&vm->root_actor->heap, md, sel, cr.method);
}

static bool wait_all_suspended(struct STA_Actor **actors, int count,
                                int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        bool all_done = true;
        for (int j = 0; j < count; j++) {
            if (atomic_load_explicit(&actors[j]->state,
                    memory_order_acquire) != STA_ACTOR_SUSPENDED) {
                all_done = false;
                break;
            }
        }
        if (all_done) return true;
        usleep(1000);
    }
    return false;
}

/* ── Test 1: 10 actors, 100 messages each, allocating garbage ─────────── */
/* Each message allocates several objects (garbage) and retains one in
 * an instVar. GC should trigger multiple times per actor. */

static void test_10_actors_100_gc_messages(void) {
    STA_VM *vm = make_vm();

    STA_OOP assoc_cls = sta_class_table_get(&vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Method: allocate garbage, retain one object in key. */
    install_method_on(vm, assoc_cls, "work",
        "work\n"
        "  | a b c |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  key := a",
        (const char *[]){"key", "value"}, 2);

    int rc = sta_scheduler_init(vm, 4);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    #define T1_ACTORS  10
    #define T1_MSGS    100
    struct STA_Actor *actors[T1_ACTORS];
    STA_OOP sel = intern(vm, "work");

    for (int i = 0; i < T1_ACTORS; i++) {
        /* 256-byte heap — forces GC frequently. */
        actors[i] = make_gc_actor(vm, assoc_cls, 2, 256);
    }

    /* Send T1_MSGS messages to each actor. */
    for (int msg = 0; msg < T1_MSGS; msg++) {
        for (int i = 0; i < T1_ACTORS; i++) {
            sta_actor_send_msg(vm, vm->root_actor, actors[i]->actor_id, sel, NULL, 0);
        }
    }

    bool done = wait_all_suspended(actors, T1_ACTORS, 10000);
    sta_scheduler_stop(vm);
    assert(done);

    /* All mailboxes empty. */
    for (int i = 0; i < T1_ACTORS; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
    }

    /* Each actor's key should hold a valid Object (from last message). */
    for (int i = 0; i < T1_ACTORS; i++) {
        STA_ObjHeader *beh = (STA_ObjHeader *)(uintptr_t)actors[i]->behavior_obj;
        STA_OOP key_val = sta_payload(beh)[0];
        /* key should be a heap object (Object new). */
        assert(STA_IS_HEAP(key_val));
        STA_ObjHeader *key_h = (STA_ObjHeader *)(uintptr_t)key_val;
        assert(key_h->class_index == STA_CLS_OBJECT);
    }

    sta_scheduler_destroy(vm);
    for (int i = 0; i < T1_ACTORS; i++) sta_actor_terminate(actors[i]);
    sta_vm_destroy(vm);
    #undef T1_ACTORS
    #undef T1_MSGS
}

/* ── Test 2: Mixed retention — some actors retain, some are pure compute ── */

static void test_mixed_retention(void) {
    STA_VM *vm = make_vm();

    STA_OOP assoc_cls = sta_class_table_get(&vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* "garbage" method: allocate many objects, retain nothing. */
    install_method_on(vm, assoc_cls, "garbage",
        "garbage\n"
        "  | a b c d e |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  d := Object new.\n"
        "  e := Object new.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    /* "retain" method: allocate and keep a reference. */
    install_method_on(vm, assoc_cls, "retain",
        "retain\n"
        "  | a b c |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  key := c.\n"
        "  value := b.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    int rc = sta_scheduler_init(vm, 4);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    #define T2_ACTORS  10
    struct STA_Actor *actors[T2_ACTORS];

    for (int i = 0; i < T2_ACTORS; i++) {
        actors[i] = make_gc_actor(vm, assoc_cls, 2, 256);
    }

    /* Even-indexed actors get "garbage" messages (pure compute).
     * Odd-indexed actors get "retain" messages (long-lived state). */
    for (int msg = 0; msg < 50; msg++) {
        for (int i = 0; i < T2_ACTORS; i++) {
            const char *sel_name = (i % 2 == 0) ? "garbage" : "retain";
            sta_actor_send_msg(vm, vm->root_actor, actors[i]->actor_id,
                                intern(vm, sel_name), NULL, 0);
        }
    }

    bool done = wait_all_suspended(actors, T2_ACTORS, 10000);
    sta_scheduler_stop(vm);
    assert(done);

    /* Verify odd actors retained valid objects. */
    for (int i = 1; i < T2_ACTORS; i += 2) {
        STA_ObjHeader *beh = (STA_ObjHeader *)(uintptr_t)actors[i]->behavior_obj;
        STA_OOP key_val = sta_payload(beh)[0];
        STA_OOP val_val = sta_payload(beh)[1];
        assert(STA_IS_HEAP(key_val));
        assert(STA_IS_HEAP(val_val));
        STA_ObjHeader *k = (STA_ObjHeader *)(uintptr_t)key_val;
        STA_ObjHeader *v = (STA_ObjHeader *)(uintptr_t)val_val;
        assert(k->class_index == STA_CLS_OBJECT);
        assert(v->class_index == STA_CLS_OBJECT);
    }

    /* Verify even actors' heaps are reasonable size (GC reclaimed garbage). */
    for (int i = 0; i < T2_ACTORS; i += 2) {
        /* After GC, heap shouldn't have grown excessively.
         * The actor only retains behavior_obj (2 slots) + whatever
         * survived the last message. Allow generous 2KB ceiling. */
        assert(actors[i]->heap.capacity <= 2048);
    }

    sta_scheduler_destroy(vm);
    for (int i = 0; i < T2_ACTORS; i++) sta_actor_terminate(actors[i]);
    sta_vm_destroy(vm);
    #undef T2_ACTORS
}

/* ── Test 3: GC with loop iteration under scheduler ───────────────────── */

static void test_gc_loop_scheduled(void) {
    STA_VM *vm = make_vm();

    STA_OOP assoc_cls = sta_class_table_get(&vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Loop that allocates objects per iteration — high GC pressure. */
    install_method_on(vm, assoc_cls, "loopAlloc",
        "loopAlloc\n"
        "  | i |\n"
        "  i := 0.\n"
        "  [i < 20] whileTrue: [\n"
        "    key := Object new.\n"
        "    i := i + 1\n"
        "  ].\n"
        "  ^i",
        (const char *[]){"key", "value"}, 2);

    int rc = sta_scheduler_init(vm, 4);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    #define T3_ACTORS 8
    struct STA_Actor *actors[T3_ACTORS];

    for (int i = 0; i < T3_ACTORS; i++) {
        actors[i] = make_gc_actor(vm, assoc_cls, 2, 256);
    }

    /* Each actor gets 10 messages, each running a 20-iteration loop. */
    STA_OOP sel = intern(vm, "loopAlloc");
    for (int msg = 0; msg < 10; msg++) {
        for (int i = 0; i < T3_ACTORS; i++) {
            sta_actor_send_msg(vm, vm->root_actor, actors[i]->actor_id, sel, NULL, 0);
        }
    }

    bool done = wait_all_suspended(actors, T3_ACTORS, 10000);
    sta_scheduler_stop(vm);
    assert(done);

    for (int i = 0; i < T3_ACTORS; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
    }

    sta_scheduler_destroy(vm);
    for (int i = 0; i < T3_ACTORS; i++) sta_actor_terminate(actors[i]);
    sta_vm_destroy(vm);
    #undef T3_ACTORS
}

/* ── Test 4: Stress — 100 actors × 500 messages, all cores ────────────── */
/* Each message allocates ~8 objects, retains 2, lets rest die.
 * After all messages processed, verify retained objects are correct. */

static void test_stress_100x500(void) {
    STA_VM *vm = make_vm();

    STA_OOP assoc_cls = sta_class_table_get(&vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Method: allocate ~8 objects, retain 2 in instVars, rest is garbage. */
    install_method_on(vm, assoc_cls, "churn",
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
        "  key := g.\n"
        "  value := h.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    /* Use all available cores. */
    int rc = sta_scheduler_init(vm, 0);
    assert(rc == 0);
    uint32_t nthreads = vm->scheduler->num_threads;
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    #define T4_ACTORS  100
    #define T4_MSGS    500

    struct STA_Actor **actors = calloc(T4_ACTORS, sizeof(struct STA_Actor *));
    assert(actors != NULL);
    STA_OOP sel = intern(vm, "churn");

    for (int i = 0; i < T4_ACTORS; i++) {
        /* 512-byte heap — room for behavior_obj + a few objects before GC. */
        actors[i] = make_gc_actor(vm, assoc_cls, 2, 512);
    }

    /* Send all messages. */
    for (int msg = 0; msg < T4_MSGS; msg++) {
        for (int i = 0; i < T4_ACTORS; i++) {
            sta_actor_send_msg(vm, vm->root_actor, actors[i]->actor_id, sel, NULL, 0);
        }
    }

    bool done = wait_all_suspended(actors, T4_ACTORS, 30000);
    sta_scheduler_stop(vm);
    assert(done);

    /* All mailboxes empty. */
    for (int i = 0; i < T4_ACTORS; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
    }

    /* Verify retained objects are correct. */
    for (int i = 0; i < T4_ACTORS; i++) {
        STA_ObjHeader *beh = (STA_ObjHeader *)(uintptr_t)actors[i]->behavior_obj;
        STA_OOP key_val = sta_payload(beh)[0];
        STA_OOP val_val = sta_payload(beh)[1];
        assert(STA_IS_HEAP(key_val));
        assert(STA_IS_HEAP(val_val));
        STA_ObjHeader *k = (STA_ObjHeader *)(uintptr_t)key_val;
        STA_ObjHeader *v = (STA_ObjHeader *)(uintptr_t)val_val;
        assert(k->class_index == STA_CLS_OBJECT);
        assert(v->class_index == STA_CLS_OBJECT);
    }

    /* Verify heap sizes are reasonable — after 500 messages of churning,
     * the heap shouldn't be enormous if GC is working. Each actor retains
     * behavior_obj (32B) + key (16B) + value (16B) = ~64B of live data.
     * Allow generous headroom for heap growth policy. */
    size_t max_heap = 0;
    for (int i = 0; i < T4_ACTORS; i++) {
        if (actors[i]->heap.capacity > max_heap)
            max_heap = actors[i]->heap.capacity;
    }
    /* If GC weren't working, 500 messages × 8 objects × 16 bytes = 64KB+.
     * With GC, each actor should stay well under 8KB. */
    assert(max_heap <= 8192);

    printf("(%d threads, max_heap=%zu, msgs=%llu) ",
           (int)nthreads, max_heap,
           (unsigned long long)((uint64_t)T4_ACTORS * T4_MSGS));

    sta_scheduler_destroy(vm);
    for (int i = 0; i < T4_ACTORS; i++) sta_actor_terminate(actors[i]);
    free(actors);
    sta_vm_destroy(vm);
    #undef T4_ACTORS
    #undef T4_MSGS
}

/* ── Test 5: GC during preemption ─────────────────────────────────────── */
/* Long-running actors that allocate in loops, preempted and resumed
 * multiple times. GC can trigger during any resume. */

static void test_gc_with_preemption(void) {
    STA_VM *vm = make_vm();

    STA_OOP assoc_cls = sta_class_table_get(&vm->class_table,
                                              STA_CLS_ASSOCIATION);

    /* Long loop (> reduction quota) that allocates each iteration. */
    install_method_on(vm, assoc_cls, "longChurn",
        "longChurn\n"
        "  | i |\n"
        "  i := 0.\n"
        "  [i < 2000] whileTrue: [\n"
        "    key := Object new.\n"
        "    i := i + 1\n"
        "  ].\n"
        "  ^i",
        (const char *[]){"key", "value"}, 2);

    int rc = sta_scheduler_init(vm, 4);
    assert(rc == 0);
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    #define T5_ACTORS 8
    struct STA_Actor *actors[T5_ACTORS];
    STA_OOP sel = intern(vm, "longChurn");

    for (int i = 0; i < T5_ACTORS; i++) {
        actors[i] = make_gc_actor(vm, assoc_cls, 2, 256);
        sta_actor_send_msg(vm, vm->root_actor, actors[i]->actor_id, sel, NULL, 0);
    }

    bool done = wait_all_suspended(actors, T5_ACTORS, 15000);
    sta_scheduler_stop(vm);
    assert(done);

    /* Each actor completed 2000 iterations with repeated GC + preemption. */
    for (int i = 0; i < T5_ACTORS; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
        /* key should be a valid Object (from last iteration). */
        STA_ObjHeader *beh = (STA_ObjHeader *)(uintptr_t)actors[i]->behavior_obj;
        STA_OOP key_val = sta_payload(beh)[0];
        assert(STA_IS_HEAP(key_val));
    }

    sta_scheduler_destroy(vm);
    for (int i = 0; i < T5_ACTORS; i++) sta_actor_terminate(actors[i]);
    sta_vm_destroy(vm);
    #undef T5_ACTORS
}

/* ── Test 6: Concurrent GC across all cores ────────────────────────────── */
/* Run many actors on all available cores, all triggering GC simultaneously.
 * GC reads shared structures (immutable space bounds, class table) but
 * never writes to them. This test exercises maximum GC concurrency.
 * If there were any shared-state corruption, TSan would flag it and
 * retained objects would be corrupted. */

static void test_concurrent_gc_all_cores(void) {
    STA_VM *vm = make_vm();

    STA_OOP assoc_cls = sta_class_table_get(&vm->class_table,
                                              STA_CLS_ASSOCIATION);

    install_method_on(vm, assoc_cls, "alloc",
        "alloc\n"
        "  | a b c d |\n"
        "  a := Object new.\n"
        "  b := Object new.\n"
        "  c := Object new.\n"
        "  d := Object new.\n"
        "  key := d.\n"
        "  ^self",
        (const char *[]){"key", "value"}, 2);

    /* Use all cores to maximize concurrent GC. */
    int rc = sta_scheduler_init(vm, 0);
    assert(rc == 0);
    uint32_t nthreads = vm->scheduler->num_threads;
    rc = sta_scheduler_start(vm);
    assert(rc == 0);

    /* Create twice as many actors as threads to ensure every thread
     * is running an actor that triggers GC. */
    uint32_t nactors = nthreads * 2;
    if (nactors < 8) nactors = 8;
    struct STA_Actor **actors = calloc(nactors, sizeof(struct STA_Actor *));
    assert(actors != NULL);
    STA_OOP sel = intern(vm, "alloc");

    for (uint32_t i = 0; i < nactors; i++) {
        actors[i] = make_gc_actor(vm, assoc_cls, 2, 256);
        /* Send many messages to sustain GC pressure. */
        for (int m = 0; m < 100; m++) {
            sta_actor_send_msg(vm, vm->root_actor, actors[i]->actor_id, sel, NULL, 0);
        }
    }

    bool done = wait_all_suspended(actors, (int)nactors, 15000);
    sta_scheduler_stop(vm);
    assert(done);

    /* Verify all actors retained valid objects. */
    for (uint32_t i = 0; i < nactors; i++) {
        assert(sta_mailbox_is_empty(&actors[i]->mailbox));
        STA_ObjHeader *beh = (STA_ObjHeader *)(uintptr_t)actors[i]->behavior_obj;
        STA_OOP key_val = sta_payload(beh)[0];
        assert(STA_IS_HEAP(key_val));
        STA_ObjHeader *key_h = (STA_ObjHeader *)(uintptr_t)key_val;
        assert(key_h->class_index == STA_CLS_OBJECT);
    }

    printf("(%d threads, %u actors) ", (int)nthreads, nactors);

    sta_scheduler_destroy(vm);
    for (uint32_t i = 0; i < nactors; i++) sta_actor_terminate(actors[i]);
    free(actors);
    sta_vm_destroy(vm);
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_gc_scheduled (Phase 2 Epic 5, Story 5):\n");

    RUN(test_10_actors_100_gc_messages);
    RUN(test_mixed_retention);
    RUN(test_gc_loop_scheduled);
    RUN(test_stress_100x500);
    RUN(test_gc_with_preemption);
    RUN(test_concurrent_gc_all_cores);

    printf("\n  %d/%d passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
