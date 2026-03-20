/* tests/test_actor_lifecycle.c
 * Phase 2 Epic 2 Story 1: Basic actor struct lifecycle tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "actor/actor.h"
#include "vm/heap.h"
#include "vm/frame.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-50s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
    tests_run++; \
} while (0)

/* ── Tests ───────────────────────────────────────────────────────────── */

static void test_create_destroy(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 2048);
    assert(a != NULL);
    assert(a->state == STA_ACTOR_CREATED);
    assert(a->handler_top == NULL);
    assert(a->signaled_exception == 0);
    assert(sta_mailbox_is_empty(&a->mailbox));
    assert(a->supervisor == NULL);
    assert(a->behavior_class == 0);
    assert(a->vm == NULL);
    sta_actor_terminate(a);
}

static void test_heap_initialized(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 2048);
    assert(a != NULL);
    assert(sta_heap_capacity(&a->heap) >= 4096);
    assert(sta_heap_used(&a->heap) == 0);
    sta_actor_terminate(a);
}

static void test_slab_initialized(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 2048);
    assert(a != NULL);
    assert(a->slab.base != NULL);
    assert(a->slab.end > a->slab.base);
    assert(a->slab.top == a->slab.base);
    assert(a->slab.sp == a->slab.base);
    sta_actor_terminate(a);
}

static void test_state_constants(void) {
    assert(STA_ACTOR_CREATED == 0);
    assert(STA_ACTOR_READY == 1);
    assert(STA_ACTOR_RUNNING == 2);
    assert(STA_ACTOR_SUSPENDED == 3);
    assert(STA_ACTOR_TERMINATED == 4);
}

static void test_actor_id_default(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 2048);
    assert(a != NULL);
    assert(a->actor_id == 0);
    a->actor_id = 42;
    assert(a->actor_id == 42);
    sta_actor_terminate(a);
}

static void test_destroy_null(void) {
    /* Should not crash. */
    sta_actor_terminate(NULL);
}

static void test_sizeof_actor(void) {
    printf("\n    sizeof(STA_Actor) = %zu bytes\n    ", sizeof(struct STA_Actor));
    /* Informational — no hard assertion, just report. */
}

static void test_heap_alloc_on_actor(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 2048);
    assert(a != NULL);

    /* Allocate an object on the actor's heap. */
    STA_ObjHeader *h = sta_heap_alloc(&a->heap, 1, 2);
    assert(h != NULL);
    assert(h->class_index == 1);
    assert(h->size == 2);
    assert(sta_heap_used(&a->heap) > 0);

    sta_actor_terminate(a);
}

static void test_two_actors_independent(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 2048);
    struct STA_Actor *b = sta_actor_create(NULL, 4096, 2048);
    assert(a != NULL);
    assert(b != NULL);

    /* Allocate on A's heap. */
    STA_ObjHeader *ha = sta_heap_alloc(&a->heap, 1, 1);
    assert(ha != NULL);

    /* B's heap should still be empty. */
    assert(sta_heap_used(&b->heap) == 0);

    /* Allocate on B's heap. */
    STA_ObjHeader *hb = sta_heap_alloc(&b->heap, 2, 1);
    assert(hb != NULL);
    assert(hb != ha);

    /* Heaps are at different addresses. */
    assert(a->heap.base != b->heap.base);

    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_actor_lifecycle\n");
    RUN(test_create_destroy);
    RUN(test_heap_initialized);
    RUN(test_slab_initialized);
    RUN(test_state_constants);
    RUN(test_actor_id_default);
    RUN(test_destroy_null);
    RUN(test_sizeof_actor);
    RUN(test_heap_alloc_on_actor);
    RUN(test_two_actors_independent);
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
