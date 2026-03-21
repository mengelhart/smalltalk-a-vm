/* tests/test_actor_registry.c
 * Actor registry unit tests — Story 1 of the actor-registry-safe-send fix.
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "actor/registry.h"
#include "actor/actor.h"
#include "vm/vm_state.h"
#include <sta/vm.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
    tests_run++; \
} while (0)

/* ── Helpers ───────────────────────────────────────────────────────── */

/* Create a minimal actor with a given ID for registry testing.
 * Uses NULL vm so no auto-registration happens. */
static struct STA_Actor *make_stub(uint32_t id) {
    struct STA_Actor *a = sta_actor_create(NULL, 128, 128);
    assert(a != NULL);
    a->actor_id = id;
    return a;
}

/* ── Tests ─────────────────────────────────────────────────────────── */

static void test_register_and_lookup(void) {
    STA_ActorRegistry *reg = sta_registry_create(64);
    assert(reg != NULL);

    struct STA_Actor *a = make_stub(10);
    sta_registry_register(reg, a);

    struct STA_Actor *found = sta_registry_lookup(reg, 10);
    assert(found == a);

    sta_actor_terminate(a);
    sta_registry_destroy(reg);
}

static void test_lookup_nonexistent(void) {
    STA_ActorRegistry *reg = sta_registry_create(64);
    assert(reg != NULL);

    struct STA_Actor *found = sta_registry_lookup(reg, 999);
    assert(found == NULL);

    sta_registry_destroy(reg);
}

static void test_register_unregister_lookup(void) {
    STA_ActorRegistry *reg = sta_registry_create(64);
    struct STA_Actor *a = make_stub(20);

    sta_registry_register(reg, a);
    assert(sta_registry_lookup(reg, 20) == a);

    sta_registry_unregister(reg, 20);
    assert(sta_registry_lookup(reg, 20) == NULL);

    sta_actor_terminate(a);
    sta_registry_destroy(reg);
}

static void test_register_100_actors(void) {
    STA_ActorRegistry *reg = sta_registry_create(64);
    struct STA_Actor *actors[100];

    for (int i = 0; i < 100; i++) {
        actors[i] = make_stub((uint32_t)(i + 1));
        sta_registry_register(reg, actors[i]);
    }

    for (int i = 0; i < 100; i++) {
        struct STA_Actor *found = sta_registry_lookup(reg, (uint32_t)(i + 1));
        assert(found == actors[i]);
    }

    for (int i = 0; i < 100; i++)
        sta_actor_terminate(actors[i]);
    sta_registry_destroy(reg);
}

static void test_unregister_every_other(void) {
    STA_ActorRegistry *reg = sta_registry_create(64);
    struct STA_Actor *actors[100];

    for (int i = 0; i < 100; i++) {
        actors[i] = make_stub((uint32_t)(i + 1));
        sta_registry_register(reg, actors[i]);
    }

    /* Unregister even-indexed actors. */
    for (int i = 0; i < 100; i += 2)
        sta_registry_unregister(reg, (uint32_t)(i + 1));

    for (int i = 0; i < 100; i++) {
        struct STA_Actor *found = sta_registry_lookup(reg, (uint32_t)(i + 1));
        if (i % 2 == 0)
            assert(found == NULL);
        else
            assert(found == actors[i]);
    }

    for (int i = 0; i < 100; i++)
        sta_actor_terminate(actors[i]);
    sta_registry_destroy(reg);
}

static void test_growth_beyond_load_factor(void) {
    /* Initial capacity 64, 70% load = 44 entries trigger growth.
     * Insert 50 actors — must trigger at least one rehash. */
    STA_ActorRegistry *reg = sta_registry_create(64);
    struct STA_Actor *actors[50];

    for (int i = 0; i < 50; i++) {
        actors[i] = make_stub((uint32_t)(i + 1));
        sta_registry_register(reg, actors[i]);
    }

    /* All still findable after growth. */
    for (int i = 0; i < 50; i++) {
        struct STA_Actor *found = sta_registry_lookup(reg, (uint32_t)(i + 1));
        assert(found == actors[i]);
    }

    for (int i = 0; i < 50; i++)
        sta_actor_terminate(actors[i]);
    sta_registry_destroy(reg);
}

static void test_duplicate_id_updates_pointer(void) {
    STA_ActorRegistry *reg = sta_registry_create(64);
    struct STA_Actor *a = make_stub(42);
    struct STA_Actor *b = make_stub(42);

    sta_registry_register(reg, a);
    assert(sta_registry_lookup(reg, 42) == a);

    /* Second registration with same ID updates the pointer. */
    sta_registry_register(reg, b);
    assert(sta_registry_lookup(reg, 42) == b);

    /* Count should be 1, not 2. */
    assert(sta_registry_count(reg) == 1);

    sta_actor_terminate(a);
    sta_actor_terminate(b);
    sta_registry_destroy(reg);
}

static void test_count(void) {
    STA_ActorRegistry *reg = sta_registry_create(64);
    assert(sta_registry_count(reg) == 0);

    struct STA_Actor *a = make_stub(1);
    struct STA_Actor *b = make_stub(2);
    struct STA_Actor *c = make_stub(3);

    sta_registry_register(reg, a);
    assert(sta_registry_count(reg) == 1);

    sta_registry_register(reg, b);
    sta_registry_register(reg, c);
    assert(sta_registry_count(reg) == 3);

    sta_registry_unregister(reg, 2);
    assert(sta_registry_count(reg) == 2);

    sta_registry_unregister(reg, 1);
    sta_registry_unregister(reg, 3);
    assert(sta_registry_count(reg) == 0);

    sta_actor_terminate(a);
    sta_actor_terminate(b);
    sta_actor_terminate(c);
    sta_registry_destroy(reg);
}

static void test_vm_integration(void) {
    /* Full VM lifecycle — registry is created and destroyed properly,
     * root actor and root supervisor are registered. */
    STA_VMConfig cfg = { .scheduler_threads = 0, .image_path = NULL };
    STA_VM *vm = sta_vm_create(&cfg);
    assert(vm != NULL);
    assert(vm->registry != NULL);

    /* Root actor (ID 1) should be findable. */
    struct STA_Actor *root = sta_registry_lookup(vm->registry,
                                                   vm->root_actor->actor_id);
    assert(root == vm->root_actor);

    /* Root supervisor should be findable. */
    struct STA_Actor *rsup = sta_registry_lookup(vm->registry,
                                                    vm->root_supervisor->actor_id);
    assert(rsup == vm->root_supervisor);

    /* At least 2 actors registered. */
    assert(sta_registry_count(vm->registry) >= 2);

    sta_vm_destroy(vm);
}

static void test_sizeof_registry(void) {
    /* STA_ActorRegistry is opaque — we can't sizeof it here.
     * Report per-entry cost: uint32_t key + pointer = 12 bytes on 64-bit. */
    printf("\n    per-entry overhead = %zu bytes (uint32_t + pointer)\n    ",
           sizeof(uint32_t) + sizeof(void *));
}

/* ── Main ────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_actor_registry\n");
    RUN(test_register_and_lookup);
    RUN(test_lookup_nonexistent);
    RUN(test_register_unregister_lookup);
    RUN(test_register_100_actors);
    RUN(test_unregister_every_other);
    RUN(test_growth_beyond_load_factor);
    RUN(test_duplicate_id_updates_pointer);
    RUN(test_count);
    RUN(test_vm_integration);
    RUN(test_sizeof_registry);
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
