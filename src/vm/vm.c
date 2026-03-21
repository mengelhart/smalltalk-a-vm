/* src/vm/vm.c
 * Public API implementation — STA_VM lifecycle, image, source loading.
 * Phase 2, Epic 0: all mutable state is inline in STA_VM.
 */
#include "vm/vm_state.h"
#include "vm/special_objects.h"
#include "vm/class_table.h"
#include "actor/actor.h"
#include "actor/registry.h"
#include "actor/supervisor.h"
#include "scheduler/scheduler.h"
#include "bootstrap/bootstrap.h"
#include "bootstrap/kernel_load.h"
#include "bootstrap/filein.h"
#include "image/image.h"
#include <sta/vm.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Defaults ──────────────────────────────────────────────────────── */

#define DEFAULT_HEAP_BYTES      (4u * 1024u * 1024u)
#define DEFAULT_IMM_BYTES       (4u * 1024u * 1024u)
#define DEFAULT_SYMTAB_CAPACITY 512u
#define DEFAULT_SLAB_BYTES      (64u * 1024u)

/* Static fallback error for sta_vm_last_error(NULL) after failed create. */
static char g_last_error[512] = "";

/* ── Helpers ───────────────────────────────────────────────────────── */

static void set_error(STA_VM *vm, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(vm->last_error, sizeof(vm->last_error), fmt, ap);
    va_end(ap);
}

/* Track which subsystems have been initialized for cleanup on failure. */
#define INIT_HEAP    (1u << 0)
#define INIT_IMM     (1u << 1)
#define INIT_SYMTAB  (1u << 2)
#define INIT_CLASSTAB (1u << 3)
#define INIT_SLAB    (1u << 4)
#define INIT_HANDLES  (1u << 5)
#define INIT_REGISTRY (1u << 6)

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) { fclose(f); return 1; }
    return 0;
}

/* ── sta_vm_create ─────────────────────────────────────────────────── */

STA_VM* sta_vm_create(const STA_VMConfig* config) {
    if (!config) return NULL;

    STA_VM *vm = calloc(1, sizeof(STA_VM));
    if (!vm) return NULL;

    vm->config = *config;
    vm->last_error[0] = '\0';
    unsigned inited = 0;

    /* Initialize inline subsystems. */
    size_t heap_bytes = config->initial_heap_bytes > 0
                        ? config->initial_heap_bytes
                        : DEFAULT_HEAP_BYTES;

    if (sta_heap_init(&vm->heap, heap_bytes) != 0) {
        set_error(vm, "heap allocation failed"); goto fail;
    }
    inited |= INIT_HEAP;

    if (sta_immutable_space_init(&vm->immutable_space, DEFAULT_IMM_BYTES) != 0) {
        set_error(vm, "immutable space allocation failed"); goto fail;
    }
    inited |= INIT_IMM;

    if (sta_symbol_table_init(&vm->symbol_table, DEFAULT_SYMTAB_CAPACITY) != 0) {
        set_error(vm, "symbol table allocation failed"); goto fail;
    }
    inited |= INIT_SYMTAB;

    if (sta_class_table_init(&vm->class_table) != 0) {
        set_error(vm, "class table allocation failed"); goto fail;
    }
    inited |= INIT_CLASSTAB;

    if (sta_stack_slab_init(&vm->slab, DEFAULT_SLAB_BYTES) != 0) {
        set_error(vm, "stack slab allocation failed"); goto fail;
    }
    inited |= INIT_SLAB;

    if (sta_handle_table_init(&vm->handles) != 0) {
        set_error(vm, "handle table allocation failed"); goto fail;
    }
    inited |= INIT_HANDLES;

    vm->registry = sta_registry_create(64);
    if (!vm->registry) {
        set_error(vm, "actor registry allocation failed"); goto fail;
    }
    inited |= INIT_REGISTRY;

    /* Initialize actor ID counter. Root actor (manually created) gets ID 1;
     * all subsequent actors get IDs from this counter via sta_actor_create. */
    atomic_store_explicit(&vm->next_actor_id, 2, memory_order_relaxed);

    /* Bind special objects to this VM's inline array. */
    sta_special_objects_bind(vm->specials);

    /* Handler state starts clean (calloc zeroes handler_top and signaled_exception). */

    /* Startup pipeline: load image or bootstrap from scratch. */
    if (config->image_path && file_exists(config->image_path)) {
        int rc = sta_image_load_from_file(config->image_path,
                                          &vm->heap, &vm->immutable_space,
                                          &vm->symbol_table, &vm->class_table);
        if (rc != 0) {
            set_error(vm, "image load failed: %s (code %d)",
                      config->image_path, rc);
            goto fail;
        }
        sta_primitive_table_init();
    } else {
        STA_BootstrapResult br = sta_bootstrap(&vm->heap, &vm->immutable_space,
                                               &vm->symbol_table, &vm->class_table);
        if (br.status != 0) {
            set_error(vm, "bootstrap failed: %s", br.error ? br.error : "unknown");
            goto fail;
        }

        sta_primitive_table_init();

#ifdef KERNEL_DIR
        const char *kernel_dir = KERNEL_DIR;
#else
        const char *kernel_dir = "kernel";
#endif
        int rc = sta_kernel_load_all(vm, kernel_dir);
        if (rc != 0) {
            set_error(vm, "kernel load failed (code %d)", rc);
            goto fail;
        }

        if (config->image_path) {
            rc = sta_image_save_to_file(config->image_path,
                                        &vm->heap, &vm->immutable_space,
                                        &vm->symbol_table, &vm->class_table);
            if (rc != 0) {
                set_error(vm, "image save failed (code %d)", rc);
                goto fail;
            }
        }
    }

    vm->bootstrapped = true;

    /* Create root actor — all execution runs inside it.
     * The root actor takes ownership of the VM's heap and slab:
     * bootstrap objects are in shared immutable space, mutable objects
     * (from kernel load) are on vm->heap which we transfer. */
    {
        struct STA_Actor *root = calloc(1, sizeof(struct STA_Actor));
        if (!root) {
            set_error(vm, "failed to allocate root actor");
            goto fail;
        }
        root->vm = vm;
        atomic_store_explicit(&root->state, STA_ACTOR_READY, memory_order_relaxed);
        root->actor_id = 1;

        /* Transfer heap: move VM's heap contents to root actor. */
        root->heap = vm->heap;
        /* Re-initialize VM heap as empty — bootstrap is done, all future
         * mutable allocation goes through the root actor's heap. */
        memset(&vm->heap, 0, sizeof(vm->heap));

        /* Transfer slab: move VM's stack slab to root actor. */
        root->slab = vm->slab;
        memset(&vm->slab, 0, sizeof(vm->slab));

        /* Handler chain: move VM's handler state to root actor. */
        root->handler_top = vm->handler_top;
        root->signaled_exception = vm->signaled_exception;
        vm->handler_top = NULL;
        vm->signaled_exception = 0;

        /* Initialize mailbox (Epic 3). */
        if (sta_mailbox_init(&root->mailbox, STA_MAILBOX_DEFAULT_CAPACITY) != 0) {
            free(root);
            set_error(vm, "failed to initialize root actor mailbox");
            goto fail;
        }

        vm->root_actor = root;

        /* Register root actor in the registry. */
        sta_registry_register(vm->registry, root);
    }

    /* Create root supervisor — top of the supervision tree.
     * Generous defaults: 10 restarts within 10 seconds. */
    {
        STA_OOP obj_cls = sta_class_table_get(&vm->class_table, STA_CLS_OBJECT);
        struct STA_Actor *rsup = sta_actor_create(vm, 16384, 2048);
        if (!rsup) {
            set_error(vm, "failed to allocate root supervisor");
            goto fail;
        }
        rsup->behavior_class = obj_cls;
        STA_ObjHeader *obj_h = sta_heap_alloc(&rsup->heap, STA_CLS_OBJECT, 0);
        if (!obj_h) {
            sta_actor_terminate(rsup);
            set_error(vm, "failed to allocate root supervisor behavior obj");
            goto fail;
        }
        rsup->behavior_obj = (STA_OOP)(uintptr_t)obj_h;
        rsup->supervisor = NULL;  /* root of the tree */
        atomic_store_explicit(&rsup->state, STA_ACTOR_SUSPENDED,
                              memory_order_relaxed);

        if (sta_supervisor_init(rsup, 10, 10) != 0) {
            sta_actor_terminate(rsup);
            set_error(vm, "failed to init root supervisor data");
            goto fail;
        }

        /* Register after full initialization (#320). */
        sta_actor_register(rsup);

        vm->root_supervisor = rsup;
    }

    return vm;

fail:
    if (inited & INIT_REGISTRY) sta_registry_destroy(vm->registry);
    if (inited & INIT_HANDLES) sta_handle_table_destroy(&vm->handles);
    if (inited & INIT_SLAB)     sta_stack_slab_deinit(&vm->slab);
    if (inited & INIT_CLASSTAB) sta_class_table_deinit(&vm->class_table);
    if (inited & INIT_SYMTAB)   sta_symbol_table_deinit(&vm->symbol_table);
    if (inited & INIT_IMM)      sta_immutable_space_deinit(&vm->immutable_space);
    if (inited & INIT_HEAP)     sta_heap_deinit(&vm->heap);
    memcpy(g_last_error, vm->last_error, sizeof(g_last_error));
    free(vm);
    return NULL;
}

/* ── sta_vm_destroy ────────────────────────────────────────────────── */

void sta_vm_destroy(STA_VM* vm) {
    if (!vm || vm->destroyed) return;

    vm->destroyed = true;

    /* Stop and destroy scheduler if running. */
    if (vm->scheduler) {
        sta_scheduler_stop(vm);
        sta_scheduler_destroy(vm);
    }

    /* Teardown in reverse order of initialization. */

    /* Tear down supervision tree BEFORE root actor — no actor should be
     * processing messages during teardown. sta_actor_terminate on the root
     * supervisor recursively destroys all children depth-first. */
    if (vm->root_supervisor) {
        sta_actor_terminate(vm->root_supervisor);
        vm->root_supervisor = NULL;
    }

    sta_handle_table_destroy(&vm->handles);

    /* Destroy root actor (owns heap and slab). */
    if (vm->root_actor) {
        sta_actor_terminate(vm->root_actor);
        vm->root_actor = NULL;
    } else {
        /* No root actor — VM still owns heap and slab (failed early). */
        sta_stack_slab_deinit(&vm->slab);
        sta_heap_deinit(&vm->heap);
    }

    /* Destroy registry AFTER all actors — unregister calls during
     * actor teardown need the registry to still be alive. */
    if (vm->registry) {
        sta_registry_destroy(vm->registry);
        vm->registry = NULL;
    }

    sta_class_table_deinit(&vm->class_table);
    sta_symbol_table_deinit(&vm->symbol_table);
    sta_immutable_space_deinit(&vm->immutable_space);

    /* Reset globals so a subsequent sta_vm_create starts clean. */
    sta_special_objects_bind(NULL);
    sta_special_objects_init();

    free(vm);
}

/* ── sta_vm_last_error ─────────────────────────────────────────────── */

const char* sta_vm_last_error(STA_VM* vm) {
    if (vm) return vm->last_error;
    return g_last_error;
}

/* ── sta_vm_load_image / sta_vm_save_image ─────────────────────────── */

int sta_vm_load_image(STA_VM* vm, const char* path) {
    if (!vm || !path) return STA_ERR_INVALID;
    STA_Heap *heap = vm->root_actor ? &vm->root_actor->heap : &vm->heap;
    int rc = sta_image_load_from_file(path, heap, &vm->immutable_space,
                                      &vm->symbol_table, &vm->class_table);
    if (rc != 0) {
        set_error(vm, "image load failed: %s (code %d)", path, rc);
        return STA_ERR_IO;
    }
    sta_primitive_table_init();
    return STA_OK;
}

int sta_vm_save_image(STA_VM* vm, const char* path) {
    if (!vm || !path) return STA_ERR_INVALID;
    STA_Heap *heap = vm->root_actor ? &vm->root_actor->heap : &vm->heap;
    int rc = sta_image_save_to_file(path, heap, &vm->immutable_space,
                                    &vm->symbol_table, &vm->class_table);
    if (rc != 0) {
        set_error(vm, "image save failed: %s (code %d)", path, rc);
    }
    return rc == 0 ? STA_OK : STA_ERR_IO;
}

/* ── sta_vm_load_source ────────────────────────────────────────────── */

int sta_vm_load_source(STA_VM* vm, const char* path) {
    if (!vm || !path) return STA_ERR_INVALID;

    if (!vm->bootstrapped) {
        set_error(vm, "kernel not bootstrapped");
        return STA_ERR_INTERNAL;
    }

    STA_FileInContext ctx = { .vm = vm };

    int rc = sta_filein_load(&ctx, path);
    if (rc != 0) {
        set_error(vm, "%s", ctx.error_msg);
        if (rc == -3) return STA_ERR_IO;
        return STA_ERR_COMPILE;
    }
    return STA_OK;
}

/* ── sta_vm_spawn_supervised ───────────────────────────────────────── */

STA_Actor* sta_vm_spawn_supervised(STA_VM* vm, STA_OOP behavior_class,
                                    STA_RestartStrategy strategy) {
    if (!vm || !vm->root_supervisor) return NULL;
    return sta_supervisor_add_child(vm->root_supervisor,
                                     behavior_class, strategy);
}

/* ── Event callbacks ───────────────────────────────────────────────── */

#define STA_EVENT_CB_MAX 16u

int sta_event_register(STA_VM* vm, STA_EventCallback callback, void* ctx) {
    if (!vm || !callback) return STA_ERR_INVALID;
    if (vm->event_cb_count >= STA_EVENT_CB_MAX) return STA_ERR_OOM;

    vm->event_cbs[vm->event_cb_count].callback = callback;
    vm->event_cbs[vm->event_cb_count].ctx = ctx;
    vm->event_cb_count++;
    return STA_OK;
}

void sta_event_unregister(STA_VM* vm, STA_EventCallback callback, void* ctx) {
    if (!vm || !callback) return;

    for (uint8_t i = 0; i < vm->event_cb_count; i++) {
        if (vm->event_cbs[i].callback == callback &&
            vm->event_cbs[i].ctx == ctx) {
            /* Shift remaining entries down. */
            for (uint8_t j = i; j + 1 < vm->event_cb_count; j++)
                vm->event_cbs[j] = vm->event_cbs[j + 1];
            vm->event_cb_count--;
            return;
        }
    }
}

/* Fire an event to all registered callbacks. Internal use only. */
void sta_vm_fire_event(STA_VM *vm, STA_EventType type, const char *message) {
    if (!vm || vm->event_cb_count == 0) return;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    STA_Event event = {
        .type = type,
        .actor = NULL,
        .message = message,
        .timestamp_ns = (uint64_t)ts.tv_sec * 1000000000ULL +
                        (uint64_t)ts.tv_nsec,
    };

    for (uint8_t i = 0; i < vm->event_cb_count; i++) {
        vm->event_cbs[i].callback(vm, &event, vm->event_cbs[i].ctx);
    }
}
