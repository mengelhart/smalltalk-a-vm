/* tests/test_actor_heap.c
 * Phase 2 Epic 2 Story 2: Per-actor heap allocator isolation tests.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include "actor/actor.h"
#include "vm/heap.h"
#include "vm/oop.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-55s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
    tests_run++; \
} while (0)

/* ── Tests ───────────────────────────────────────────────────────────── */

static void test_heap_independent_backing_memory(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 512);
    struct STA_Actor *b = sta_actor_create(NULL, 4096, 512);
    assert(a && b);

    /* Backing slabs are at different addresses. */
    assert(a->heap.base != b->heap.base);

    /* Capacities are independent. */
    assert(sta_heap_capacity(&a->heap) >= 4096);
    assert(sta_heap_capacity(&b->heap) >= 4096);

    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_alloc_does_not_affect_other_actor(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 512);
    struct STA_Actor *b = sta_actor_create(NULL, 4096, 512);
    assert(a && b);

    /* Allocate several objects on A. */
    for (int i = 0; i < 10; i++) {
        STA_ObjHeader *h = sta_heap_alloc(&a->heap, 1, 2);
        assert(h != NULL);
    }

    /* B's heap is untouched. */
    assert(sta_heap_used(&b->heap) == 0);

    /* Allocate on B. */
    STA_ObjHeader *hb = sta_heap_alloc(&b->heap, 2, 3);
    assert(hb != NULL);

    /* A's used doesn't change from B's allocation. */
    size_t a_used = sta_heap_used(&a->heap);
    STA_ObjHeader *hb2 = sta_heap_alloc(&b->heap, 2, 1);
    assert(hb2 != NULL);
    assert(sta_heap_used(&a->heap) == a_used);

    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_objects_on_correct_heap(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 512);
    struct STA_Actor *b = sta_actor_create(NULL, 4096, 512);
    assert(a && b);

    STA_ObjHeader *ha = sta_heap_alloc(&a->heap, 1, 1);
    STA_ObjHeader *hb = sta_heap_alloc(&b->heap, 2, 1);
    assert(ha && hb);

    /* Object on A is within A's backing slab. */
    uintptr_t a_base = (uintptr_t)a->heap.base;
    uintptr_t a_end  = a_base + a->heap.capacity;
    uintptr_t ha_addr = (uintptr_t)ha;
    assert(ha_addr >= a_base && ha_addr < a_end);

    /* Object on B is within B's backing slab. */
    uintptr_t b_base = (uintptr_t)b->heap.base;
    uintptr_t b_end  = b_base + b->heap.capacity;
    uintptr_t hb_addr = (uintptr_t)hb;
    assert(hb_addr >= b_base && hb_addr < b_end);

    /* Object on A is NOT within B's slab, and vice versa. */
    assert(ha_addr < b_base || ha_addr >= b_end);
    assert(hb_addr < a_base || hb_addr >= a_end);

    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_heap_exhaustion_independent(void) {
    /* Small heaps — 256 bytes each. */
    struct STA_Actor *a = sta_actor_create(NULL, 256, 512);
    struct STA_Actor *b = sta_actor_create(NULL, 256, 512);
    assert(a && b);

    /* Fill A's heap. */
    int a_count = 0;
    while (sta_heap_alloc(&a->heap, 1, 1) != NULL)
        a_count++;
    assert(a_count > 0);

    /* B's heap should still have full capacity. */
    assert(sta_heap_used(&b->heap) == 0);
    STA_ObjHeader *hb = sta_heap_alloc(&b->heap, 2, 1);
    assert(hb != NULL);

    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_different_heap_sizes(void) {
    struct STA_Actor *small = sta_actor_create(NULL, 128, 512);
    struct STA_Actor *large = sta_actor_create(NULL, 8192, 512);
    assert(small && large);

    assert(sta_heap_capacity(&small->heap) >= 128);
    assert(sta_heap_capacity(&large->heap) >= 8192);
    assert(sta_heap_capacity(&large->heap) > sta_heap_capacity(&small->heap));

    sta_actor_destroy(small);
    sta_actor_destroy(large);
}

static void test_destroy_frees_heap(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 512);
    assert(a != NULL);

    /* Allocate objects to consume heap space. */
    for (int i = 0; i < 5; i++) {
        STA_ObjHeader *h = sta_heap_alloc(&a->heap, 1, 4);
        assert(h != NULL);
    }

    /* Destroy — should not leak (ASan will catch). */
    sta_actor_destroy(a);
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_actor_heap\n");
    RUN(test_heap_independent_backing_memory);
    RUN(test_alloc_does_not_affect_other_actor);
    RUN(test_objects_on_correct_heap);
    RUN(test_heap_exhaustion_independent);
    RUN(test_different_heap_sizes);
    RUN(test_destroy_frees_heap);
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
