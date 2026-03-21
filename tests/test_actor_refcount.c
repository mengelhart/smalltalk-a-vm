/* tests/test_actor_refcount.c
 * Actor reference counting tests — fixes #316, #317.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>

#include "actor/actor.h"
#include "actor/registry.h"
#include "actor/supervisor.h"
#include "actor/mailbox.h"
#include "actor/mailbox_msg.h"
#include "vm/vm_state.h"
#include "vm/oop.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/special_objects.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include <sta/vm.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static STA_VM *make_vm(void) {
    STA_VMConfig cfg = {0};
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);
    return vm;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 1: Create actor → refcount == 1
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_create_refcount(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *a = sta_actor_create(vm, 128, 512);
    assert(a != NULL);
    sta_actor_register(a);

    uint32_t rc = atomic_load_explicit(&a->refcount, memory_order_acquire);
    assert(rc == 1);

    printf("  PASS: test_create_refcount (refcount=%u)\n", rc);

    sta_actor_terminate(a);
    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 2: Registry lookup → refcount == 2; release → refcount == 1
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_lookup_increments_refcount(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *a = sta_actor_create(vm, 128, 512);
    assert(a != NULL);
    sta_actor_register(a);
    uint32_t id = a->actor_id;

    /* Lookup increments refcount. */
    struct STA_Actor *found = sta_registry_lookup(vm->registry, id);
    assert(found == a);
    uint32_t rc = atomic_load_explicit(&a->refcount, memory_order_acquire);
    assert(rc == 2);

    /* Release decrements. */
    sta_actor_release(found);
    rc = atomic_load_explicit(&a->refcount, memory_order_acquire);
    assert(rc == 1);

    printf("  PASS: test_lookup_increments_refcount\n");

    sta_actor_terminate(a);
    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 3: Terminate actor → unregistered but struct alive if ref held
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_terminate_with_held_ref(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *a = sta_actor_create(vm, 128, 512);
    assert(a != NULL);
    sta_actor_register(a);
    uint32_t id = a->actor_id;

    /* Acquire a reference (simulating in-flight use). */
    struct STA_Actor *ref = sta_registry_lookup(vm->registry, id);
    assert(ref == a);
    /* refcount = 2 (registry + our lookup) */

    /* Terminate — unregisters, decrements registry ref. */
    sta_actor_terminate(a);

    /* refcount should be 1 (our held reference). */
    uint32_t rc = atomic_load_explicit(&ref->refcount, memory_order_acquire);
    assert(rc == 1);

    /* State should be TERMINATED. */
    uint32_t state = atomic_load_explicit(&ref->state, memory_order_acquire);
    assert(state == STA_ACTOR_TERMINATED);

    /* Lookup should return NULL (unregistered). */
    struct STA_Actor *gone = sta_registry_lookup(vm->registry, id);
    assert(gone == NULL);

    printf("  PASS: test_terminate_with_held_ref\n");

    /* Release our reference — this should free the actor. */
    sta_actor_release(ref);

    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 4: Terminate + release last reference → actor freed (ASan validates)
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_terminate_frees_when_no_refs(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *a = sta_actor_create(vm, 128, 512);
    assert(a != NULL);
    sta_actor_register(a);

    /* Terminate with no extra references → refcount hits 0 → freed. */
    sta_actor_terminate(a);
    /* If ASan is enabled, accessing 'a' here would trigger use-after-free. */

    printf("  PASS: test_terminate_frees_when_no_refs\n");
    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 5: Multiple concurrent lookups → refcount tracks correctly
 * ══════════════════════════════════════════════════════════════════════════ */

#define NUM_LOOKUP_THREADS 8

typedef struct {
    STA_ActorRegistry *reg;
    uint32_t actor_id;
    struct STA_Actor *result;
} LookupArg;

static void *lookup_thread(void *arg) {
    LookupArg *la = arg;
    la->result = sta_registry_lookup(la->reg, la->actor_id);
    return NULL;
}

static void test_concurrent_lookups(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *a = sta_actor_create(vm, 128, 512);
    assert(a != NULL);
    sta_actor_register(a);
    uint32_t id = a->actor_id;

    pthread_t threads[NUM_LOOKUP_THREADS];
    LookupArg args[NUM_LOOKUP_THREADS];

    for (int i = 0; i < NUM_LOOKUP_THREADS; i++) {
        args[i].reg = vm->registry;
        args[i].actor_id = id;
        args[i].result = NULL;
        pthread_create(&threads[i], NULL, lookup_thread, &args[i]);
    }

    for (int i = 0; i < NUM_LOOKUP_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* All lookups should have found the actor. */
    for (int i = 0; i < NUM_LOOKUP_THREADS; i++) {
        assert(args[i].result == a);
    }

    /* refcount = 1 (registry) + NUM_LOOKUP_THREADS. */
    uint32_t rc = atomic_load_explicit(&a->refcount, memory_order_acquire);
    assert(rc == 1 + NUM_LOOKUP_THREADS);

    /* Release all lookup references. */
    for (int i = 0; i < NUM_LOOKUP_THREADS; i++) {
        sta_actor_release(args[i].result);
    }

    rc = atomic_load_explicit(&a->refcount, memory_order_acquire);
    assert(rc == 1);

    printf("  PASS: test_concurrent_lookups (max_refcount=%u)\n",
           1 + NUM_LOOKUP_THREADS);

    sta_actor_terminate(a);
    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 6: Send to terminated-but-not-freed actor → STA_ERR_ACTOR_DEAD
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_send_to_terminated(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *sender = sta_actor_create(vm, 128, 512);
    struct STA_Actor *target = sta_actor_create(vm, 128, 512);
    assert(sender && target);
    sta_actor_register(sender);
    sta_actor_register(target);

    uint32_t target_id = target->actor_id;

    /* Acquire extra reference to keep target alive. */
    struct STA_Actor *ref = sta_registry_lookup(vm->registry, target_id);
    assert(ref == target);

    /* Terminate target. */
    sta_actor_terminate(target);

    /* Send to the terminated actor — should get STA_ERR_ACTOR_DEAD.
     * The actor is unregistered, so lookup returns NULL. */
    STA_OOP sel = sta_symbol_intern(&vm->immutable_space,
                                      &vm->symbol_table, "test", 4);
    int rc = sta_actor_send_msg(vm, sender, target_id, sel, NULL, 0);
    assert(rc == STA_ERR_ACTOR_DEAD);

    printf("  PASS: test_send_to_terminated\n");

    sta_actor_release(ref);
    sta_actor_terminate(sender);
    sta_vm_destroy(vm);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Test 7: Registry lookup after unregister → returns NULL
 * ══════════════════════════════════════════════════════════════════════════ */

static void test_lookup_after_unregister(void) {
    STA_VM *vm = make_vm();

    struct STA_Actor *a = sta_actor_create(vm, 128, 512);
    assert(a != NULL);
    sta_actor_register(a);
    uint32_t id = a->actor_id;

    /* Manually unregister. */
    sta_registry_unregister(vm->registry, id);

    /* Lookup should return NULL. */
    struct STA_Actor *found = sta_registry_lookup(vm->registry, id);
    assert(found == NULL);

    printf("  PASS: test_lookup_after_unregister\n");

    /* Re-register so terminate can unregister cleanly. */
    sta_registry_register(vm->registry, a);
    sta_actor_terminate(a);
    sta_vm_destroy(vm);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_actor_refcount:\n");
    test_create_refcount();
    test_lookup_increments_refcount();
    test_terminate_with_held_ref();
    test_terminate_frees_when_no_refs();
    test_concurrent_lookups();
    test_send_to_terminated();
    test_lookup_after_unregister();
    printf("All actor refcount tests passed.\n");
    return 0;
}
