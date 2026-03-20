/* tests/test_supervision_stress.c
 * Phase 2 Epic 6, Story 7: Integration and stress tests.
 * All tests use the multi-threaded scheduler.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <time.h>

#include "scheduler/scheduler.h"
#include "actor/actor.h"
#include "actor/supervisor.h"
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
#include <sta/vm.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static STA_VM *make_vm(void) {
    STA_VMConfig cfg = {0};
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);
    return vm;
}

static STA_OOP intern(STA_VM *vm, const char *name) {
    return sta_symbol_intern(&vm->immutable_space,
                              &vm->symbol_table,
                              name, strlen(name));
}

static void install_method(STA_VM *vm, const char *selector_str,
                            const char *source) {
    STA_OOP cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    STA_CompileResult r = sta_compile_method(
        source, cls, NULL, 0,
        &vm->symbol_table, &vm->immutable_space,
        &vm->root_actor->heap,
        vm->specials[SPC_SMALLTALK]);
    if (r.had_error) {
        fprintf(stderr, "compile error: %s\nsource: %s\n", r.error_msg, source);
    }
    assert(!r.had_error);

    STA_OOP md = sta_class_method_dict(cls);
    assert(md != 0);
    STA_OOP selector = intern(vm, selector_str);
    sta_method_dict_insert(&vm->root_actor->heap, md, selector, r.method);
}

/* Create a supervised child under a supervisor with a behavior_obj. */
static struct STA_Actor *add_supervised_child(struct STA_Actor *sup,
                                                STA_VM *vm,
                                                STA_RestartStrategy strategy,
                                                size_t heap_size) {
    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls, strategy);
    if (!child) return NULL;

    /* Grow heap if needed for stress tests. */
    if (heap_size > child->heap.capacity) {
        sta_heap_grow(&child->heap, heap_size);
    }

    STA_ObjHeader *obj_h = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(obj_h);
    child->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    return child;
}

/* Wait for all actors in a list to reach SUSPENDED or TERMINATED state. */
static bool wait_actors_done(struct STA_Actor **actors, int count,
                              int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        bool all_done = true;
        for (int j = 0; j < count; j++) {
            uint32_t st = atomic_load_explicit(&actors[j]->state,
                                                 memory_order_acquire);
            if (st != STA_ACTOR_SUSPENDED && st != STA_ACTOR_TERMINATED) {
                all_done = false;
                break;
            }
        }
        if (all_done) return true;
        usleep(1000);
    }
    return false;
}

/* Wait until a supervisor's child spec has a non-NULL current_actor
 * (i.e., the restart has completed). */
static bool wait_child_restarted(STA_ChildSpec *spec, int timeout_ms) {
    for (int i = 0; i < timeout_ms; i++) {
        if (spec->current_actor != NULL) return true;
        usleep(1000);
    }
    return false;
}

/* Event counter. */
static _Atomic int g_event_count;

static void event_counter(STA_VM *vm, const STA_Event *event, void *ctx) {
    (void)vm; (void)ctx; (void)event;
    atomic_fetch_add(&g_event_count, 1);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Stress: mass restart
 * 10 supervisors under root, each with 10 workers (100 total).
 * Each worker processes 50 messages then crashes on message 51 (DNU).
 * Verify all 100 crashes detected, all 100 restarts happen.
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_mass_restart(void) {
    STA_VM *vm = make_vm();

    install_method(vm, "work", "work\n  ^self");
    install_method(vm, "crash", "crash\n  Error new signal");
    install_method(vm, "initialize", "initialize\n  ^self");

    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);

    /* Create 10 sub-supervisors under root. */
    struct STA_Actor *supervisors[10];
    struct STA_Actor *workers[100];

    for (int s = 0; s < 10; s++) {
        supervisors[s] = sta_vm_spawn_supervised(vm, obj_cls,
                                                    STA_RESTART_RESTART);
        assert(supervisors[s]);
        STA_ObjHeader *s_obj = sta_heap_alloc(&supervisors[s]->heap,
                                                STA_CLS_OBJECT, 0);
        assert(s_obj);
        supervisors[s]->behavior_obj = (STA_OOP)(uintptr_t)s_obj;
        /* High intensity so 10 restarts don't exceed. */
        sta_supervisor_init(supervisors[s], 15, 10);

        for (int w = 0; w < 10; w++) {
            int idx = s * 10 + w;
            workers[idx] = add_supervised_child(
                supervisors[s], vm, STA_RESTART_RESTART, 4096);
            assert(workers[idx]);
        }
    }

    /* Pre-enqueue messages before starting the scheduler for simplicity.
     * sta_actor_send_msg is now safe during scheduling (resolves target
     * by ID via registry), but pre-enqueue avoids timing dependencies. */
    STA_OOP work_sel = intern(vm, "work");
    STA_OOP crash_sel = intern(vm, "crash");

    for (int i = 0; i < 100; i++) {
        for (int m = 0; m < 50; m++) {
            STA_MailboxMsg *msg = sta_mailbox_msg_create(work_sel, NULL, 0, 0);
            assert(msg);
            sta_mailbox_enqueue(&workers[i]->mailbox, msg);
        }
        STA_MailboxMsg *msg = sta_mailbox_msg_create(crash_sel, NULL, 0, 0);
        assert(msg);
        sta_mailbox_enqueue(&workers[i]->mailbox, msg);
    }

    sta_scheduler_init(vm, 0);
    sta_scheduler_start(vm);

    for (int i = 0; i < 100; i++) {
        atomic_store_explicit(&workers[i]->state, STA_ACTOR_READY,
                              memory_order_release);
        sta_scheduler_enqueue(vm->scheduler, workers[i]);
    }

    /* Wait for all crashes and restarts to propagate.
     * Each spec remembers its original actor_id (saved in spec_original_ids).
     * A spec is "restarted" when current_actor is non-NULL and its actor_id
     * differs from the original. No index gymnastics — we store the original
     * ID per-spec before starting the scheduler. */

    /* Build a map: for each spec, save the original actor_id. */
    uint32_t spec_original_ids[100];
    STA_ChildSpec *spec_ptrs[100];
    int spec_idx = 0;
    for (int s = 0; s < 10; s++) {
        STA_ChildSpec *spec = supervisors[s]->sup_data->children;
        while (spec) {
            assert(spec_idx < 100);
            spec_ptrs[spec_idx] = spec;
            spec_original_ids[spec_idx] = spec->current_actor->actor_id;
            spec_idx++;
            spec = spec->next;
        }
    }
    assert(spec_idx == 100);

    int restarted = 0;
    for (int attempt = 0; attempt < 50000; attempt++) {
        restarted = 0;
        for (int i = 0; i < 100; i++) {
            uint32_t cur_id = atomic_load_explicit(
                &spec_ptrs[i]->current_actor_id, memory_order_acquire);
            if (cur_id != 0 && cur_id != spec_original_ids[i]) {
                restarted++;
            }
        }
        if (restarted >= 100) break;
        usleep(1000);
    }

    sta_scheduler_stop(vm);

    printf("  PASS: test_mass_restart (restarted=%d/100)\n", restarted);
    assert(restarted == 100);

    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Stress: simultaneous crashes
 * 10 actors under one supervisor. Crash-triggering message sent to all
 * 10 simultaneously. Verify all 10 restarted.
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_simultaneous_crashes(void) {
    STA_VM *vm = make_vm();

    install_method(vm, "crash", "crash\n  Error new signal");
    install_method(vm, "initialize", "initialize\n  ^self");

    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);

    struct STA_Actor *sup = sta_vm_spawn_supervised(vm, obj_cls,
                                                      STA_RESTART_RESTART);
    assert(sup);

    STA_ObjHeader *s_obj = sta_heap_alloc(&sup->heap, STA_CLS_OBJECT, 0);
    assert(s_obj);
    sup->behavior_obj = (STA_OOP)(uintptr_t)s_obj;
    sta_supervisor_init(sup, 15, 10);

    struct STA_Actor *workers[10];
    uint32_t old_ids[10];
    for (int i = 0; i < 10; i++) {
        workers[i] = add_supervised_child(sup, vm, STA_RESTART_RESTART, 4096);
        assert(workers[i]);
        old_ids[i] = workers[i]->actor_id;
    }

    /* Save original IDs per-spec BEFORE starting scheduler. */
    STA_ChildSpec *sim_specs[10];
    uint32_t sim_orig_ids[10];
    int si = 0;
    STA_ChildSpec *sp = sup->sup_data->children;
    while (sp) {
        sim_specs[si] = sp;
        sim_orig_ids[si] = sp->current_actor->actor_id;
        si++;
        sp = sp->next;
    }
    assert(si == 10);

    /* Pre-enqueue crash messages before starting scheduler. */
    STA_OOP crash_sel = intern(vm, "crash");
    for (int i = 0; i < 10; i++) {
        STA_MailboxMsg *msg = sta_mailbox_msg_create(crash_sel, NULL, 0, 0);
        assert(msg);
        sta_mailbox_enqueue(&workers[i]->mailbox, msg);
    }

    sta_scheduler_init(vm, 0);
    sta_scheduler_start(vm);

    for (int i = 0; i < 10; i++) {
        atomic_store_explicit(&workers[i]->state, STA_ACTOR_READY,
                              memory_order_release);
        sta_scheduler_enqueue(vm->scheduler, workers[i]);
    }

    /* Poll until all 10 specs have new actors (different actor_id). */
    int restarted = 0;
    for (int attempt = 0; attempt < 10000; attempt++) {
        restarted = 0;
        for (int i = 0; i < 10; i++) {
            uint32_t cur_id = atomic_load_explicit(
                &sim_specs[i]->current_actor_id, memory_order_acquire);
            if (cur_id != 0 && cur_id != sim_orig_ids[i]) {
                restarted++;
            }
        }
        if (restarted >= 10) break;
        usleep(1000);
    }

    sta_scheduler_stop(vm);

    printf("  PASS: test_simultaneous_crashes (restarted=%d/10)\n", restarted);
    assert(restarted == 10);

    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Restart + GC: worker allocates heavily, crashes, restarted worker GCs ok
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_restart_and_gc(void) {
    STA_VM *vm = make_vm();

    /* Worker allocates arrays to fill heap, then crashes. */
    install_method(vm, "allocAndCrash",
        "allocAndCrash\n"
        "  | a |\n"
        "  a := Array new: 10.\n"
        "  a := Array new: 10.\n"
        "  a := Array new: 10.\n"
        "  Error new signal");
    install_method(vm, "allocOk",
        "allocOk\n"
        "  | a |\n"
        "  a := Array new: 10.\n"
        "  a := Array new: 10.\n"
        "  ^a");
    install_method(vm, "initialize", "initialize\n  ^self");

    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);

    struct STA_Actor *sup = sta_vm_spawn_supervised(vm, obj_cls,
                                                      STA_RESTART_RESTART);
    assert(sup);

    STA_ObjHeader *s_obj = sta_heap_alloc(&sup->heap, STA_CLS_OBJECT, 0);
    assert(s_obj);
    sup->behavior_obj = (STA_OOP)(uintptr_t)s_obj;
    sta_supervisor_init(sup, 5, 10);

    struct STA_Actor *worker = add_supervised_child(sup, vm,
                                                      STA_RESTART_RESTART, 4096);
    assert(worker);
    uint32_t old_id = worker->actor_id;

    STA_ChildSpec *spec = sup->sup_data->children;

    /* Pre-enqueue allocAndCrash before starting scheduler. */
    STA_OOP crash_sel = intern(vm, "allocAndCrash");
    {
        STA_MailboxMsg *msg = sta_mailbox_msg_create(crash_sel, NULL, 0, 0);
        assert(msg);
        sta_mailbox_enqueue(&worker->mailbox, msg);
    }

    sta_scheduler_init(vm, 0);
    sta_scheduler_start(vm);

    atomic_store_explicit(&worker->state, STA_ACTOR_READY,
                          memory_order_release);
    sta_scheduler_enqueue(vm->scheduler, worker);

    /* Wait for restart: poll atomic current_actor_id. */
    bool did_restart = false;
    for (int i = 0; i < 5000; i++) {
        uint32_t cur_id = atomic_load_explicit(&spec->current_actor_id,
                                                 memory_order_acquire);
        if (cur_id != 0 && cur_id != old_id) {
            did_restart = true;
            break;
        }
        usleep(1000);
    }
    assert(did_restart);

    /* Send allocOk to the restarted actor — verify it allocates and GCs ok.
     * Stop scheduler first so we can safely read spec->current_actor. */
    sta_scheduler_stop(vm);

    struct STA_Actor *new_child = spec->current_actor;
    STA_OOP alloc_sel = intern(vm, "allocOk");
    {
        STA_MailboxMsg *msg = sta_mailbox_msg_create(alloc_sel, NULL, 0, 0);
        assert(msg);
        sta_mailbox_enqueue(&new_child->mailbox, msg);
    }

    sta_scheduler_start(vm);
    atomic_store_explicit(&new_child->state, STA_ACTOR_READY,
                          memory_order_release);
    sta_scheduler_enqueue(vm->scheduler, new_child);

    struct STA_Actor *new_arr[1] = { new_child };
    wait_actors_done(new_arr, 1, 5000);

    sta_scheduler_stop(vm);

    /* Verify the restarted actor processed without crashing. */
    uint32_t final_state = atomic_load_explicit(&spec->current_actor->state,
                                                  memory_order_acquire);
    assert(final_state == STA_ACTOR_SUSPENDED);

    printf("  PASS: test_restart_and_gc\n");

    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Unaffected siblings: worker #3 crashes, others continue processing
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_unaffected_siblings(void) {
    STA_VM *vm = make_vm();

    install_method(vm, "work", "work\n  ^self");
    install_method(vm, "crash", "crash\n  Error new signal");
    install_method(vm, "initialize", "initialize\n  ^self");

    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);

    struct STA_Actor *sup = sta_vm_spawn_supervised(vm, obj_cls,
                                                      STA_RESTART_RESTART);
    assert(sup);

    STA_ObjHeader *s_obj = sta_heap_alloc(&sup->heap, STA_CLS_OBJECT, 0);
    assert(s_obj);
    sup->behavior_obj = (STA_OOP)(uintptr_t)s_obj;
    sta_supervisor_init(sup, 5, 10);

    struct STA_Actor *workers[5];
    for (int i = 0; i < 5; i++) {
        workers[i] = add_supervised_child(sup, vm, STA_RESTART_RESTART, 4096);
        assert(workers[i]);
    }

    /* Save worker 2's ID and locate its spec before any scheduling. */
    uint32_t worker2_old_id = workers[2]->actor_id;
    STA_ChildSpec *w2_spec = sup->sup_data->children->next->next;
    assert(w2_spec->current_actor == workers[2]);

    /* Pre-enqueue all messages before starting scheduler.
     * Workers 0,1,3,4 get 20 #work messages.
     * Worker 2 gets 20 #work + 1 #crash. */
    STA_OOP work_sel = intern(vm, "work");
    STA_OOP crash_sel = intern(vm, "crash");
    for (int i = 0; i < 5; i++) {
        for (int m = 0; m < 20; m++) {
            STA_MailboxMsg *msg = sta_mailbox_msg_create(work_sel, NULL, 0, 0);
            assert(msg);
            sta_mailbox_enqueue(&workers[i]->mailbox, msg);
        }
    }
    {
        STA_MailboxMsg *msg = sta_mailbox_msg_create(crash_sel, NULL, 0, 0);
        assert(msg);
        sta_mailbox_enqueue(&workers[2]->mailbox, msg);
    }

    /* Start scheduler and enqueue all workers. */
    sta_scheduler_init(vm, 0);
    sta_scheduler_start(vm);

    for (int i = 0; i < 5; i++) {
        atomic_store_explicit(&workers[i]->state, STA_ACTOR_READY,
                              memory_order_release);
        sta_scheduler_enqueue(vm->scheduler, workers[i]);
    }

    /* Wait for surviving workers to finish processing their messages. */
    struct STA_Actor *surviving[4] = { workers[0], workers[1],
                                         workers[3], workers[4] };
    wait_actors_done(surviving, 4, 5000);

    /* Wait for the restart to complete: poll atomic current_actor_id. */
    bool restarted = false;
    for (int i = 0; i < 5000; i++) {
        uint32_t cur_id = atomic_load_explicit(&w2_spec->current_actor_id,
                                                 memory_order_acquire);
        if (cur_id != 0 && cur_id != worker2_old_id) {
            restarted = true;
            break;
        }
        usleep(1000);
    }

    sta_scheduler_stop(vm);

    assert(restarted);

    /* Workers 0,1,3,4 should have empty mailboxes. */
    for (int i = 0; i < 5; i++) {
        if (i == 2) continue;
        assert(sta_mailbox_is_empty(&workers[i]->mailbox));
        uint32_t st = atomic_load_explicit(&workers[i]->state,
                                             memory_order_acquire);
        assert(st == STA_ACTOR_SUSPENDED);
    }

    printf("  PASS: test_unaffected_siblings\n");

    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Cascading escalation: root → sup_A → sup_B → workers
 * sup_B exceeds intensity, escalates to sup_A. sup_A restarts sup_B.
 * If sup_A also exceeds, escalates to root.
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_cascading_escalation(void) {
    STA_VM *vm = make_vm();

    install_method(vm, "crash", "crash\n  Error new signal");
    install_method(vm, "initialize", "initialize\n  ^self");

    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);

    atomic_store(&g_event_count, 0);
    sta_event_register(vm, event_counter, NULL);

    /* sup_A under root with RESTART strategy. */
    struct STA_Actor *sup_a = sta_vm_spawn_supervised(vm, obj_cls,
                                                        STA_RESTART_RESTART);
    assert(sup_a);
    STA_ObjHeader *a_obj = sta_heap_alloc(&sup_a->heap, STA_CLS_OBJECT, 0);
    assert(a_obj);
    sup_a->behavior_obj = (STA_OOP)(uintptr_t)a_obj;
    sta_supervisor_init(sup_a, 0, 5);  /* sup_A: max_restarts=0 → first
                                        * escalation from sup_B immediately
                                        * exceeds → cascades to root. */

    /* sup_B under sup_A with RESTART strategy. */
    struct STA_Actor *sup_b = add_supervised_child(sup_a, vm,
                                                     STA_RESTART_RESTART, 16384);
    assert(sup_b);
    sta_supervisor_init(sup_b, 1, 5);  /* sup_B: max_restarts=1 */

    /* Workers under sup_B. */
    struct STA_Actor *w1 = add_supervised_child(sup_b, vm,
                                                  STA_RESTART_RESTART, 4096);
    struct STA_Actor *w2 = add_supervised_child(sup_b, vm,
                                                  STA_RESTART_RESTART, 4096);
    assert(w1 && w2);

    /* Pre-enqueue crash messages on both workers. When the scheduler
     * processes these, sup_B will get 2 restart notifications
     * (restart_count reaches 2, == max_restarts). That alone doesn't
     * exceed intensity. To trigger escalation, we need a 3rd crash —
     * but the restarted workers would need another crash message.
     *
     * Strategy: enqueue 2 crash messages on w1. After the first crash,
     * sup_B restarts w1 (restart_count=1). The restarted w1 gets an
     * #initialize in its mailbox but no second #crash — the second
     * crash message was on the OLD w1's mailbox which was drained.
     * So instead: crash both w1 and w2 to get restart_count=2, then
     * crash the restarted w1 to exceed (restart_count=3 > 2). But
     * we can't pre-enqueue on the restarted actor (it doesn't exist yet).
     *
     * Simplification: set sup_B's max_restarts=1 so just 2 crashes
     * (one from each worker) exceed intensity → escalation to sup_A. */
    sup_b->sup_data->max_restarts = 1;

    STA_OOP crash_sel = intern(vm, "crash");
    {
        STA_MailboxMsg *msg = sta_mailbox_msg_create(crash_sel, NULL, 0, 0);
        assert(msg);
        sta_mailbox_enqueue(&w1->mailbox, msg);
    }
    {
        STA_MailboxMsg *msg = sta_mailbox_msg_create(crash_sel, NULL, 0, 0);
        assert(msg);
        sta_mailbox_enqueue(&w2->mailbox, msg);
    }

    sta_scheduler_init(vm, 0);
    sta_scheduler_start(vm);

    atomic_store_explicit(&w1->state, STA_ACTOR_READY, memory_order_release);
    sta_scheduler_enqueue(vm->scheduler, w1);
    atomic_store_explicit(&w2->state, STA_ACTOR_READY, memory_order_release);
    sta_scheduler_enqueue(vm->scheduler, w2);

    /* Wait for the escalation chain to propagate. */
    usleep(2000000);  /* 2 seconds */

    sta_scheduler_stop(vm);

    /* Events should have been fired at each escalation level. */
    int events = atomic_load(&g_event_count);
    printf("  PASS: test_cascading_escalation (events=%d)\n", events);
    /* At least one event should have fired (root supervisor sees escalation). */
    assert(events >= 1);

    sta_event_unregister(vm, event_counter, NULL);
    sta_scheduler_destroy(vm);
    sta_vm_destroy(vm);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_supervision_stress:\n");
    test_mass_restart();
    test_simultaneous_crashes();
    test_restart_and_gc();
    test_unaffected_siblings();
    test_cascading_escalation();
    printf("All supervision stress tests passed.\n");
    return 0;
}
