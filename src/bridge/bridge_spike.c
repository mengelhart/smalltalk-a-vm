/* src/bridge/bridge_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: native bridge between the C runtime and the SwiftUI IDE.
 * See docs/spikes/spike-007-native-bridge.md and ADR 013.
 *
 * This file provides implementations of:
 *   - All functions declared in sta/vm.h (replacing the stubs in vm.c,
 *     handle.c, and eval.c for the spike test binary).
 *   - Spike-internal helpers declared in bridge_spike.h.
 *
 * Stub functions (returning STA_ERR_INTERNAL or NULL) are clearly marked.
 * Real implementations carry no such comment.
 */

#include "src/bridge/bridge_spike.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Size assertions ──────────────────────────────────────────────────── */

_Static_assert(sizeof(struct STA_Handle) == 16,
               "STA_Handle must be 16 bytes");
_Static_assert(sizeof(STA_ActorEntry) == 48,
               "STA_ActorEntry must be 48 bytes");

/* ── Timing helper ────────────────────────────────────────────────────── */

uint64_t sta_bridge_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ── set_last_error — caller must hold htbl_lock ──────────────────────── */

static void set_last_error(STA_VM *vm, const char *msg) {
    snprintf(vm->last_error, sizeof(vm->last_error), "%s", msg);
}

/* ── VM lifecycle ─────────────────────────────────────────────────────── */

STA_VM *sta_vm_create(const STA_VMConfig *config) {
    (void)config;

    STA_VM *vm = calloc(1, sizeof(struct STA_VM));
    if (!vm) return NULL;

    /* Initialise well-known root objects. */
    vm->nil_obj.class_index  = STA_STUB_NIL_CLASS;
    vm->nil_obj.size         = 0;
    vm->nil_obj.obj_flags    = STA_OBJ_SHARED_IMM;

    vm->true_obj.class_index = STA_STUB_TRUE_CLASS;
    vm->true_obj.size        = 0;
    vm->true_obj.obj_flags   = STA_OBJ_SHARED_IMM;

    vm->false_obj.class_index = STA_STUB_FALSE_CLASS;
    vm->false_obj.size        = 0;
    vm->false_obj.obj_flags   = STA_OBJ_SHARED_IMM;

    /* Initialise locks. */
    pthread_mutex_init(&vm->htbl_lock,    NULL);
    pthread_mutex_init(&vm->install_lock, NULL);
    pthread_mutex_init(&vm->actor_lock,   NULL);

    /* Register stub classes. */
    const char *defaults[] = {
        "Array", "String", "Object", "SmallInt", "Character",
        "Boolean", "Symbol", "OrderedCollection", NULL
    };
    for (int i = 0; defaults[i] != NULL; i++) {
        uint32_t n = vm->class_registry.count;
        if (n >= STA_STUB_CLASS_CAPACITY) break;
        strncpy(vm->class_registry.entries[n].name,
                defaults[i],
                sizeof(vm->class_registry.entries[n].name) - 1);
        vm->class_registry.entries[n].identity.class_index = STA_STUB_CLASS_CLASS;
        vm->class_registry.entries[n].identity.size        = 0;
        vm->class_registry.entries[n].identity.obj_flags   = STA_OBJ_SHARED_IMM;
        vm->class_registry.count++;
    }

    vm->actor_registry.next_id = 1;

    return vm;
}

void sta_vm_destroy(STA_VM *vm) {
    if (!vm) return;
    pthread_mutex_destroy(&vm->htbl_lock);
    pthread_mutex_destroy(&vm->install_lock);
    pthread_mutex_destroy(&vm->actor_lock);
    free(vm);
}

const char *sta_vm_last_error(STA_VM *vm) {
    if (!vm) return "NULL vm";
    return vm->last_error;
}

/* STUB */ int sta_vm_load_image(STA_VM *vm, const char *path) {
    (void)vm; (void)path;
    return STA_ERR_INTERNAL;
}

/* STUB */ int sta_vm_save_image(STA_VM *vm, const char *path) {
    (void)vm; (void)path;
    return STA_ERR_INTERNAL;
}

/* ── Handle table — internal ──────────────────────────────────────────── */

/*
 * Find a free slot and initialise it. Caller must hold htbl_lock.
 * Returns NULL if the table is full.
 */
STA_Handle *sta_handle_alloc_locked(STA_VM *vm, STA_OOP oop) {
    for (uint32_t i = 0; i < STA_HANDLE_TABLE_CAPACITY; i++) {
        struct STA_Handle *h = &vm->htbl[i];
        if (h->refcount == 0) {
            h->oop      = oop;
            h->refcount = 1;
            return h;
        }
    }
    return NULL;
}

/*
 * Acquire the lock, alloc a handle, release.
 */
STA_Handle *sta_handle_alloc(STA_VM *vm, STA_OOP oop) {
    pthread_mutex_lock(&vm->htbl_lock);
    STA_Handle *h = sta_handle_alloc_locked(vm, oop);
    if (!h) set_last_error(vm, "handle table full");
    pthread_mutex_unlock(&vm->htbl_lock);
    return h;
}

/* ── Handle lifecycle (public API) ───────────────────────────────────── */

STA_Handle *sta_handle_retain(STA_VM *vm, STA_Handle *handle) {
    if (!vm || !handle) return handle;
    pthread_mutex_lock(&vm->htbl_lock);
    handle->refcount++;
    pthread_mutex_unlock(&vm->htbl_lock);
    return handle;
}

void sta_handle_release(STA_VM *vm, STA_Handle *handle) {
    if (!vm || !handle) return;
    pthread_mutex_lock(&vm->htbl_lock);
    if (handle->refcount > 0) {
        handle->refcount--;
        if (handle->refcount == 0) {
            handle->oop  = 0;
            handle->_pad = 0;
        }
    }
    pthread_mutex_unlock(&vm->htbl_lock);
}

/* ── Well-known roots ─────────────────────────────────────────────────── */

STA_Handle *sta_vm_nil(STA_VM *vm) {
    if (!vm) return NULL;
    return sta_handle_alloc(vm, (STA_OOP)&vm->nil_obj);
}

STA_Handle *sta_vm_true(STA_VM *vm) {
    if (!vm) return NULL;
    return sta_handle_alloc(vm, (STA_OOP)&vm->true_obj);
}

STA_Handle *sta_vm_false(STA_VM *vm) {
    if (!vm) return NULL;
    return sta_handle_alloc(vm, (STA_OOP)&vm->false_obj);
}

STA_Handle *sta_vm_lookup_class(STA_VM *vm, const char *name) {
    if (!vm || !name) return NULL;

    /* Class registry is read-only after init — no lock needed. */
    for (uint32_t i = 0; i < vm->class_registry.count; i++) {
        if (strncmp(vm->class_registry.entries[i].name, name, 63) == 0) {
            STA_OOP oop = (STA_OOP)&vm->class_registry.entries[i].identity;
            return sta_handle_alloc(vm, oop);
        }
    }

    pthread_mutex_lock(&vm->htbl_lock);
    set_last_error(vm, "class not found");
    pthread_mutex_unlock(&vm->htbl_lock);
    return NULL;
}

/* ── Inspection ───────────────────────────────────────────────────────── */

/*
 * NOT THREAD-SAFE — single-caller-at-a-time by contract.
 * The lock serialises writes to inspect_buf but does NOT extend the
 * validity of the returned pointer beyond the next call.
 */
const char *sta_inspect_cstring(STA_VM *vm, STA_Handle *handle) {
    if (!vm || !handle) return NULL;

    pthread_mutex_lock(&vm->htbl_lock);

    STA_OOP oop = handle->oop;
    const char *result = vm->inspect_buf;

    if (oop == 0) {
        snprintf(vm->inspect_buf, sizeof(vm->inspect_buf), "nil");
    } else {
        STA_ObjHeader *obj = (STA_ObjHeader *)(uintptr_t)oop;
        switch (obj->class_index) {
        case STA_STUB_NIL_CLASS:
            snprintf(vm->inspect_buf, sizeof(vm->inspect_buf), "nil");
            break;
        case STA_STUB_TRUE_CLASS:
            snprintf(vm->inspect_buf, sizeof(vm->inspect_buf), "true");
            break;
        case STA_STUB_FALSE_CLASS:
            snprintf(vm->inspect_buf, sizeof(vm->inspect_buf), "false");
            break;
        case STA_STUB_CLASS_CLASS: {
            /* Find the class name in the registry. */
            const char *cname = "UnknownClass";
            for (uint32_t i = 0; i < vm->class_registry.count; i++) {
                if (oop == (STA_OOP)&vm->class_registry.entries[i].identity) {
                    cname = vm->class_registry.entries[i].name;
                    break;
                }
            }
            snprintf(vm->inspect_buf, sizeof(vm->inspect_buf), "%s", cname);
            break;
        }
        case STA_STUB_ACTOR_CLASS: {
            /* Find the actor_id in the registry. */
            uint32_t aid = 0;
            for (uint32_t i = 0; i < STA_ACTOR_REGISTRY_CAPACITY; i++) {
                STA_ActorEntry *e = &vm->actor_registry.entries[i];
                if (e->active && oop == (STA_OOP)&e->identity) {
                    aid = e->actor_id;
                    break;
                }
            }
            snprintf(vm->inspect_buf, sizeof(vm->inspect_buf),
                     "an Actor (#%u)", aid);
            break;
        }
        default:
            snprintf(vm->inspect_buf, sizeof(vm->inspect_buf),
                     "a StubObject (class_index=%u)", obj->class_index);
            break;
        }
    }

    pthread_mutex_unlock(&vm->htbl_lock);
    return result;
}

/* STUB */ STA_Handle *sta_inspect(STA_VM *vm, STA_Handle *object) {
    (void)vm; (void)object;
    return NULL;
}

/* STUB */ STA_Handle *sta_eval(STA_VM *vm, const char *expression) {
    (void)vm; (void)expression;
    return NULL;
}

/* ── Live update ──────────────────────────────────────────────────────── */

int sta_method_install(STA_VM     *vm,
                       STA_Handle *class_handle,
                       const char *selector,
                       const char *source) {
    if (!vm || !class_handle || !selector) return STA_ERR_INVALID;

    pthread_mutex_lock(&vm->install_lock);

    if (vm->method_log.count >= STA_METHOD_LOG_CAPACITY) {
        pthread_mutex_unlock(&vm->install_lock);
        return STA_ERR_INTERNAL;
    }

    /* Resolve class name from the handle's OOP. */
    char class_name[64] = "<unknown>";
    STA_OOP oop = class_handle->oop;
    for (uint32_t i = 0; i < vm->class_registry.count; i++) {
        if (oop == (STA_OOP)&vm->class_registry.entries[i].identity) {
            strncpy(class_name, vm->class_registry.entries[i].name,
                    sizeof(class_name) - 1);
            break;
        }
    }

    STA_MethodLogEntry *e = &vm->method_log.entries[vm->method_log.count];
    strncpy(e->class_name, class_name, sizeof(e->class_name) - 1);
    strncpy(e->selector,   selector,   sizeof(e->selector)   - 1);
    if (source)
        strncpy(e->source, source, sizeof(e->source) - 1);
    vm->method_log.count++;

    pthread_mutex_unlock(&vm->install_lock);
    return STA_OK;
}

int sta_class_define(STA_VM *vm, const char *source) {
    if (!vm || !source) return STA_ERR_INVALID;

    pthread_mutex_lock(&vm->install_lock);

    if (vm->class_def_log.count >= STA_CLASS_DEF_LOG_CAPACITY) {
        pthread_mutex_unlock(&vm->install_lock);
        return STA_ERR_INTERNAL;
    }

    STA_ClassDefEntry *e = &vm->class_def_log.entries[vm->class_def_log.count];
    strncpy(e->source, source, sizeof(e->source) - 1);
    vm->class_def_log.count++;

    pthread_mutex_unlock(&vm->install_lock);
    return STA_OK;
}

uint32_t sta_method_log_count(STA_VM *vm) {
    if (!vm) return 0;
    pthread_mutex_lock(&vm->install_lock);
    uint32_t n = vm->method_log.count;
    pthread_mutex_unlock(&vm->install_lock);
    return n;
}

uint32_t sta_class_def_log_count(STA_VM *vm) {
    if (!vm) return 0;
    pthread_mutex_lock(&vm->install_lock);
    uint32_t n = vm->class_def_log.count;
    pthread_mutex_unlock(&vm->install_lock);
    return n;
}

/* ── Actor registry ───────────────────────────────────────────────────── */

int sta_actor_registry_add(STA_VM   *vm,
                           uint32_t  sched_flags,
                           uint32_t  mailbox_depth,
                           int       has_supervisor) {
    if (!vm) return -1;

    pthread_mutex_lock(&vm->actor_lock);

    if (vm->actor_registry.count >= STA_ACTOR_REGISTRY_CAPACITY) {
        pthread_mutex_unlock(&vm->actor_lock);
        return -1;
    }

    /* Find a free slot (first tombstoned or beyond count). */
    int idx = -1;
    for (uint32_t i = 0; i < STA_ACTOR_REGISTRY_CAPACITY; i++) {
        if (!vm->actor_registry.entries[i].active) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        pthread_mutex_unlock(&vm->actor_lock);
        return -1;
    }

    STA_ActorEntry *e = &vm->actor_registry.entries[idx];
    memset(e, 0, sizeof(*e));

    e->actor_id             = vm->actor_registry.next_id++;
    e->identity.class_index = STA_STUB_ACTOR_CLASS;
    e->identity.size        = 0;
    e->identity.obj_flags   = STA_OBJ_ACTOR_LOCAL;
    atomic_store_explicit(&e->sched_flags,    sched_flags,    memory_order_relaxed);
    atomic_store_explicit(&e->mailbox_depth,  mailbox_depth,  memory_order_relaxed);

    if (has_supervisor) {
        e->has_supervisor            = 1;
        e->sup_identity.class_index  = STA_STUB_ACTOR_CLASS;
        e->sup_identity.size         = 0;
        e->sup_identity.obj_flags    = STA_OBJ_ACTOR_LOCAL;
    }

    e->active = 1;
    vm->actor_registry.count++;

    pthread_mutex_unlock(&vm->actor_lock);
    return idx;
}

void sta_actor_registry_remove(STA_VM *vm, int index) {
    if (!vm || index < 0 || (uint32_t)index >= STA_ACTOR_REGISTRY_CAPACITY)
        return;

    pthread_mutex_lock(&vm->actor_lock);
    if (vm->actor_registry.entries[index].active) {
        vm->actor_registry.entries[index].active = 0;
        if (vm->actor_registry.count > 0)
            vm->actor_registry.count--;
    }
    pthread_mutex_unlock(&vm->actor_lock);
}

/* ── Actor enumeration (public API) ───────────────────────────────────── */

int sta_actor_enumerate(STA_VM *vm, STA_ActorVisitor visitor, void *ctx) {
    if (!vm || !visitor) return STA_ERR_INVALID;

    /*
     * Snapshot protocol:
     *   1. Acquire actor_lock.
     *   2. Copy active entry indices into a local stack array.
     *   3. Release actor_lock.
     *   4. For each index, alloc handles (under htbl_lock), call visitor.
     */

    int snapshot[STA_ACTOR_REGISTRY_CAPACITY];
    uint32_t snap_count = 0;

    pthread_mutex_lock(&vm->actor_lock);
    for (uint32_t i = 0; i < STA_ACTOR_REGISTRY_CAPACITY; i++) {
        if (vm->actor_registry.entries[i].active) {
            snapshot[snap_count++] = (int)i;
        }
    }
    pthread_mutex_unlock(&vm->actor_lock);

    for (uint32_t i = 0; i < snap_count; i++) {
        int idx = snapshot[i];
        STA_ActorEntry *e = &vm->actor_registry.entries[idx];

        STA_ActorInfo info;
        memset(&info, 0, sizeof(info));

        /* Alloc handles under htbl_lock. */
        pthread_mutex_lock(&vm->htbl_lock);
        info.actor_handle = sta_handle_alloc_locked(vm, (STA_OOP)&e->identity);
        if (e->has_supervisor)
            info.supervisor_handle =
                sta_handle_alloc_locked(vm, (STA_OOP)&e->sup_identity);
        pthread_mutex_unlock(&vm->htbl_lock);

        if (!info.actor_handle) continue; /* table full — skip */

        info.mailbox_depth = atomic_load_explicit(&e->mailbox_depth,
                                                   memory_order_acquire);
        info.sched_flags   = atomic_load_explicit(&e->sched_flags,
                                                   memory_order_acquire);

        visitor(&info, ctx);
        /* visitor is responsible for releasing both handles. */
    }

    return (int)snap_count;
}

/* ── Event callbacks (public API) ─────────────────────────────────────── */

int sta_event_register(STA_VM *vm, STA_EventCallback callback, void *ctx) {
    if (!vm || !callback) return STA_ERR_INVALID;

    pthread_mutex_lock(&vm->htbl_lock);

    if (vm->events.count >= STA_EVENT_CALLBACK_CAPACITY) {
        set_last_error(vm, "event callback table full");
        pthread_mutex_unlock(&vm->htbl_lock);
        return STA_ERR_OOM;
    }

    vm->events.entries[vm->events.count].callback = callback;
    vm->events.entries[vm->events.count].ctx       = ctx;
    vm->events.count++;

    pthread_mutex_unlock(&vm->htbl_lock);
    return STA_OK;
}

void sta_event_unregister(STA_VM *vm, STA_EventCallback callback, void *ctx) {
    if (!vm || !callback) return;

    pthread_mutex_lock(&vm->htbl_lock);

    for (uint32_t i = 0; i < vm->events.count; i++) {
        if (vm->events.entries[i].callback == callback &&
            vm->events.entries[i].ctx      == ctx) {
            /* Compact: move last entry into this slot. */
            uint32_t last = vm->events.count - 1;
            if (i != last)
                vm->events.entries[i] = vm->events.entries[last];
            vm->events.count--;
            break;
        }
    }

    pthread_mutex_unlock(&vm->htbl_lock);
}

void sta_event_dispatch(STA_VM       *vm,
                        STA_EventType type,
                        STA_OOP       actor_oop,
                        const char   *message) {
    if (!vm) return;

    /*
     * Dispatch protocol:
     *   1. Acquire htbl_lock; copy callback list to local stack array;
     *      release lock. Calling user code under the lock could deadlock
     *      if the callback re-enters the API (even though re-entrancy is
     *      documented as prohibited, we enforce the unlock defensively).
     *   2. For each callback: alloc a fresh actor handle (htbl_lock);
     *      fill STA_Event; call callback outside lock.
     */

    STA_EventCallbackEntry local_cbs[STA_EVENT_CALLBACK_CAPACITY];
    uint32_t               local_count;

    pthread_mutex_lock(&vm->htbl_lock);
    local_count = vm->events.count;
    for (uint32_t i = 0; i < local_count; i++)
        local_cbs[i] = vm->events.entries[i];
    pthread_mutex_unlock(&vm->htbl_lock);

    for (uint32_t i = 0; i < local_count; i++) {
        STA_Handle *actor_handle = NULL;
        if (actor_oop != 0) {
            pthread_mutex_lock(&vm->htbl_lock);
            actor_handle = sta_handle_alloc_locked(vm, actor_oop);
            pthread_mutex_unlock(&vm->htbl_lock);
        }

        STA_Event evt;
        evt.type         = type;
        evt.actor        = actor_handle; /* callback must release */
        evt.message      = message ? message : "";
        evt.timestamp_ns = sta_bridge_now_ns();

        local_cbs[i].callback(vm, &evt, local_cbs[i].ctx);
    }
}

/* ── Actor interface — stubs ──────────────────────────────────────────── */

/* STUB */ STA_Actor *sta_actor_spawn(STA_VM *vm, STA_Handle *class_handle) {
    (void)vm; (void)class_handle;
    return NULL;
}

/* STUB */ int sta_actor_send(STA_VM   *vm,
                              STA_Actor *actor,
                              STA_Handle *message) {
    (void)vm; (void)actor; (void)message;
    return STA_ERR_INTERNAL;
}
