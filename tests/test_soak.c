/* tests/test_soak.c
 * Long-running soak test for the actor runtime.
 * Exercises supervision, crash/restart, GC, registry, and scheduler
 * under sustained load.  NOT part of the default ctest suite.
 *
 * Usage:
 *   ./build/tests/test_soak                       # 60-second default
 *   SOAK_DURATION=3600 ./build/tests/test_soak    # 1-hour run
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>
#include <pthread.h>
#include <time.h>
#include <mach/mach.h>

#include "scheduler/scheduler.h"
#include "actor/actor.h"
#include "actor/registry.h"
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

/* ── Configuration ──────────────────────────────────────────────────── */

#define SOAK_DURATION_SEC_DEFAULT  60
#define SOAK_NUM_SUPERVISORS       4
#define SOAK_WORKERS_PER_SUP       50
#define SOAK_TOTAL_WORKERS         (SOAK_NUM_SUPERVISORS * SOAK_WORKERS_PER_SUP)
#define SOAK_CRASH_EVERY_N         100
#define SOAK_WORKER_HEAP           8192

/* ── Atomic counters ────────────────────────────────────────────────── */

static _Atomic uint64_t g_msgs_sent;
static _Atomic uint64_t g_restarts;
static _Atomic uint64_t g_sends_dead;   /* STA_ERR_ACTOR_DEAD count */
static _Atomic int       g_stop;
static _Atomic size_t    g_peak_rss;

/* ── RSS (macOS) ────────────────────────────────────────────────────── */

static size_t get_rss_bytes(void) {
    struct mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                  (task_info_t)&info, &count) == KERN_SUCCESS) {
        return info.resident_size;
    }
    return 0;
}

/* ── Helpers (same pattern as test_supervision_stress.c) ────────────── */

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

static struct STA_Actor *add_supervised_child(struct STA_Actor *sup,
                                                STA_VM *vm,
                                                STA_RestartStrategy strategy,
                                                size_t heap_size) {
    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
    struct STA_Actor *child = sta_supervisor_add_child(sup, obj_cls, strategy);
    if (!child) return NULL;

    if (heap_size > child->heap.capacity) {
        sta_heap_grow(&child->heap, heap_size);
    }

    STA_ObjHeader *obj_h = sta_heap_alloc(&child->heap, STA_CLS_OBJECT, 0);
    assert(obj_h);
    child->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
    return child;
}

/* Restart counting: read supervisor restart_count fields.
 * Uses __atomic_load_n to avoid TSan false positive — the field is
 * non-atomic in the struct but we only need an approximate count. */

/* ── Pump thread ────────────────────────────────────────────────────── */

typedef struct {
    STA_VM            *vm;
    struct STA_Actor  *pump_actor;    /* sender identity for messages */
    struct STA_Actor  *supervisors[SOAK_NUM_SUPERVISORS];
    int                worker_count;
    STA_OOP            work_sel;
    STA_OOP            crash_sel;
} PumpCtx;

/* Collect current worker IDs from supervisor child specs.
 * Returns the number of IDs written to ids[]. */
static int collect_worker_ids(PumpCtx *ctx, uint32_t *ids, int max) {
    int n = 0;
    for (int s = 0; s < SOAK_NUM_SUPERVISORS && n < max; s++) {
        if (!ctx->supervisors[s] || !ctx->supervisors[s]->sup_data) continue;
        STA_ChildSpec *spec = ctx->supervisors[s]->sup_data->children;
        while (spec && n < max) {
            uint32_t id = atomic_load_explicit(&spec->current_actor_id,
                                                 memory_order_acquire);
            if (id != 0) ids[n++] = id;
            spec = spec->next;
        }
    }
    return n;
}

static void *pump_thread(void *arg) {
    PumpCtx *ctx = (PumpCtx *)arg;
    uint64_t local_sent = 0;
    uint64_t local_dead = 0;
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)(uintptr_t)pthread_self();

    uint32_t ids[SOAK_TOTAL_WORKERS];
    int id_count = 0;
    int refresh_counter = 0;

    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        /* Refresh IDs from specs every 500 batches (~25ms). */
        if (refresh_counter == 0 || id_count == 0) {
            id_count = collect_worker_ids(ctx, ids, SOAK_TOTAL_WORKERS);
            refresh_counter = 500;
        }
        refresh_counter--;

        if (id_count == 0) {
            usleep(1000);
            continue;
        }

        /* Send a batch of 50 messages, then yield briefly. */
        for (int b = 0; b < 50 && id_count > 0; b++) {
            int idx = rand_r(&seed) % id_count;
            uint32_t target_id = ids[idx];

            /* Decide: crash or work. */
            STA_OOP sel;
            if ((local_sent % SOAK_CRASH_EVERY_N) == (SOAK_CRASH_EVERY_N - 1)) {
                sel = ctx->crash_sel;
            } else {
                sel = ctx->work_sel;
            }

            int rc = sta_actor_send_msg(ctx->vm, ctx->pump_actor, target_id,
                                         sel, NULL, 0);
            if (rc == STA_ERR_ACTOR_DEAD) {
                local_dead++;
            }
            local_sent++;
        }

        /* Flush to globals periodically, yield. */
        atomic_fetch_add_explicit(&g_msgs_sent, 50, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_sends_dead, local_dead, memory_order_relaxed);
        local_dead = 0;
        usleep(100);  /* 100 µs — prevent mailbox flood */
    }

    return NULL;
}

/* ── Health thread ──────────────────────────────────────────────────── */

typedef struct {
    STA_VM            *vm;
    struct STA_Actor  *supervisors[SOAK_NUM_SUPERVISORS];
    time_t             start_time;
} HealthCtx;

/* Count total restarts by summing restart_count from all supervisors.
 * The restart_count field is non-atomic in production code; the read here
 * is a benign diagnostic race (approximate count is fine).
 * Suppress TSan with an attribute on this function. */
__attribute__((no_sanitize("thread")))
static uint64_t count_restarts(HealthCtx *ctx) {
    uint64_t total = 0;
    for (int s = 0; s < SOAK_NUM_SUPERVISORS; s++) {
        if (ctx->supervisors[s] && ctx->supervisors[s]->sup_data) {
            total += ctx->supervisors[s]->sup_data->restart_count;
        }
    }
    return total;
}

static void *health_thread(void *arg) {
    HealthCtx *ctx = (HealthCtx *)arg;

    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        sleep(5);
        if (atomic_load_explicit(&g_stop, memory_order_acquire)) break;

        time_t elapsed = time(NULL) - ctx->start_time;
        uint64_t msgs = atomic_load_explicit(&g_msgs_sent, memory_order_relaxed);
        uint64_t rsts = count_restarts(ctx);
        atomic_store_explicit(&g_restarts, rsts, memory_order_relaxed);
        uint32_t reg_count = sta_registry_count(ctx->vm->registry);
        size_t rss = get_rss_bytes();

        /* Track peak RSS. */
        size_t prev_peak = atomic_load_explicit(&g_peak_rss, memory_order_relaxed);
        while (rss > prev_peak) {
            if (atomic_compare_exchange_weak_explicit(
                    &g_peak_rss, &prev_peak, rss,
                    memory_order_relaxed, memory_order_relaxed))
                break;
        }

        printf("[%3lds] restarts=%llu msgs=%llu dead=%llu registry=%u rss=%.1fMB\n",
               (long)elapsed, (unsigned long long)rsts,
               (unsigned long long)msgs,
               (unsigned long long)atomic_load(&g_sends_dead),
               reg_count,
               (double)rss / (1024.0 * 1024.0));
        fflush(stdout);
    }

    return NULL;
}

/* ── Main ───────────────────────────────────────────────────────────── */

int main(void) {
    /* Read duration from environment. */
    int duration = SOAK_DURATION_SEC_DEFAULT;
    const char *env = getenv("SOAK_DURATION");
    if (env) duration = atoi(env);
    if (duration <= 0) duration = SOAK_DURATION_SEC_DEFAULT;

    printf("=== SOAK TEST ===\n");
    printf("Duration: %ds, Supervisors: %d, Workers/sup: %d, Total: %d\n",
           duration, SOAK_NUM_SUPERVISORS, SOAK_WORKERS_PER_SUP, SOAK_TOTAL_WORKERS);
    printf("Crash every %d messages, Worker heap: %d bytes\n\n",
           SOAK_CRASH_EVERY_N, SOAK_WORKER_HEAP);

    /* Reset counters. */
    atomic_store(&g_msgs_sent, 0);
    atomic_store(&g_restarts, 0);
    atomic_store(&g_sends_dead, 0);
    atomic_store(&g_stop, 0);
    atomic_store(&g_peak_rss, 0);

    /* 1. Create VM. */
    STA_VMConfig cfg = {0};
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);

    /* 2. Install methods. */
    install_method(vm, "work",
        "work\n"
        "  | a |\n"
        "  a := Array new: 5.\n"
        "  a at: 1 put: 42.\n"
        "  a at: 2 put: 99.\n"
        "  ^a");
    install_method(vm, "crash", "crash\n  Error new signal");
    install_method(vm, "initialize", "initialize\n  ^self");

    STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);

    /* 3. Create supervision tree: 4 supervisors × 50 workers. */
    struct STA_Actor *supervisors[SOAK_NUM_SUPERVISORS];
    uint32_t worker_ids[SOAK_TOTAL_WORKERS];
    int wid = 0;

    for (int s = 0; s < SOAK_NUM_SUPERVISORS; s++) {
        supervisors[s] = sta_vm_spawn_supervised(vm, obj_cls,
                                                    STA_RESTART_RESTART);
        assert(supervisors[s]);
        STA_ObjHeader *s_obj = sta_heap_alloc(&supervisors[s]->heap,
                                                STA_CLS_OBJECT, 0);
        assert(s_obj);
        supervisors[s]->behavior_obj = (STA_OOP)(uintptr_t)s_obj;
        /* Very high intensity: allow unlimited restarts over the soak duration.
         * 1M restarts within the full duration + buffer. */
        sta_supervisor_init(supervisors[s], 1000000, (uint32_t)duration + 60);

        for (int w = 0; w < SOAK_WORKERS_PER_SUP; w++) {
            struct STA_Actor *child = add_supervised_child(
                supervisors[s], vm, STA_RESTART_RESTART, SOAK_WORKER_HEAP);
            assert(child);
            worker_ids[wid++] = child->actor_id;
        }
    }
    assert(wid == SOAK_TOTAL_WORKERS);

    /* Create a lightweight pump actor (sender identity for messages). */
    struct STA_Actor *pump_actor = sta_actor_create(vm, 128, 128);
    assert(pump_actor);
    pump_actor->behavior_class = obj_cls;
    sta_actor_register(pump_actor);

    /* 4. Start scheduler. */
    sta_scheduler_init(vm, 0);
    sta_scheduler_start(vm);

    /* Enqueue all workers with an initial work message. */
    STA_OOP work_sel = intern(vm, "work");
    for (int i = 0; i < SOAK_TOTAL_WORKERS; i++) {
        sta_actor_send_msg(vm, pump_actor, worker_ids[i], work_sel, NULL, 0);
    }

    /* 6. Launch pump and health threads. */
    time_t start_time = time(NULL);

    PumpCtx pump_ctx = {
        .vm = vm,
        .pump_actor = pump_actor,
        .worker_count = SOAK_TOTAL_WORKERS,
        .work_sel = work_sel,
        .crash_sel = intern(vm, "crash"),
    };
    for (int s = 0; s < SOAK_NUM_SUPERVISORS; s++)
        pump_ctx.supervisors[s] = supervisors[s];

    HealthCtx health_ctx = {
        .vm = vm,
        .start_time = start_time,
    };
    for (int s = 0; s < SOAK_NUM_SUPERVISORS; s++)
        health_ctx.supervisors[s] = supervisors[s];

    pthread_t pump_tid, health_tid;
    pthread_create(&pump_tid, NULL, pump_thread, &pump_ctx);
    pthread_create(&health_tid, NULL, health_thread, &health_ctx);

    /* 7. Run for the configured duration. */
    sleep((unsigned)duration);

    /* 8. Stop everything. */
    atomic_store_explicit(&g_stop, 1, memory_order_release);
    pthread_join(pump_tid, NULL);
    pthread_join(health_tid, NULL);

    sta_scheduler_stop(vm);

    /* 9. Collect final stats. */
    uint64_t total_msgs = atomic_load(&g_msgs_sent);
    uint64_t total_restarts = count_restarts(&health_ctx);
    uint64_t total_dead = atomic_load(&g_sends_dead);
    size_t final_rss = get_rss_bytes();
    size_t peak_rss = atomic_load(&g_peak_rss);
    if (final_rss > peak_rss) peak_rss = final_rss;
    uint32_t final_reg = sta_registry_count(vm->registry);

    /* Final restart count from supervisors. */
    total_restarts = count_restarts(&health_ctx);

    sta_vm_destroy(vm);

    /* 10. Print summary. */
    printf("\n=== SOAK TEST COMPLETE ===\n");
    printf("Duration:       %ds\n", duration);
    printf("Total messages: %llu\n", (unsigned long long)total_msgs);
    printf("Total restarts: %llu\n", (unsigned long long)total_restarts);
    printf("Dead sends:     %llu\n", (unsigned long long)total_dead);
    printf("Peak RSS:       %.1fMB\n", (double)peak_rss / (1024.0 * 1024.0));
    printf("Final RSS:      %.1fMB\n", (double)final_rss / (1024.0 * 1024.0));
    printf("Final registry: %u\n", final_reg);
    printf("Messages/sec:   %.0f\n",
           duration > 0 ? (double)total_msgs / duration : 0.0);
    printf("Restarts/sec:   %.1f\n",
           duration > 0 ? (double)total_restarts / duration : 0.0);

    if (peak_rss > 0 && final_rss > 2 * peak_rss) {
        printf("\nWARNING: Final RSS (%.1fMB) > 2x peak RSS (%.1fMB) "
               "— potential memory leak!\n",
               (double)final_rss / (1024.0 * 1024.0),
               (double)peak_rss / (1024.0 * 1024.0));
    }

    printf("\nAll done.\n");
    return 0;
}
