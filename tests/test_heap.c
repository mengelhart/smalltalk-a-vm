/* tests/test_heap.c
 * Phase 1: Actor-local heap allocator tests.
 * Story 3 of Epic 1 — Object Memory and Allocator.
 */
#include <stdio.h>
#include <stdint.h>
#include "vm/oop.h"
#include "vm/heap.h"

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("  OK: %s\n", msg); } } while(0)

int main(void) {
    int failures = 0;

    /* ── Create ──────────────────────────────────────────── */
    printf("\n=== Heap: create ===\n");
    STA_Heap *heap = sta_heap_create(4096);
    CHECK(heap != NULL,                "create succeeds");
    CHECK(sta_heap_used(heap) == 0,    "initial used == 0");
    CHECK(sta_heap_capacity(heap) >= 4096, "capacity >= requested");

    /* ── basicNew (0 slots) ──────────────────────────────── */
    printf("\n=== basicNew (0 slots) ===\n");
    STA_ObjHeader *obj0 = sta_heap_alloc(heap, 3, 0);  /* UndefinedObject */
    CHECK(obj0 != NULL,                 "alloc 0-slot succeeds");
    CHECK(obj0->class_index == 3,       "class_index correct");
    CHECK(obj0->size == 0,              "size == 0");
    CHECK(obj0->gc_flags == STA_GC_WHITE, "gc_flags == WHITE");
    CHECK(obj0->obj_flags == 0,         "obj_flags == 0 (mutable, not shared)");
    CHECK(obj0->reserved == 0,          "reserved == 0");
    CHECK(((uintptr_t)obj0 % 16) == 0, "16-byte aligned");
    CHECK(sta_heap_used(heap) == 16,    "used == 16 after 0-slot alloc");

    /* ── basicNew: (4 slots) ─────────────────────────────── */
    printf("\n=== basicNew: (4 slots) ===\n");
    STA_ObjHeader *obj4 = sta_heap_alloc(heap, 9, 4);  /* Array */
    CHECK(obj4 != NULL,                 "alloc 4-slot succeeds");
    CHECK(obj4->class_index == 9,       "class_index == 9 (Array)");
    CHECK(obj4->size == 4,              "size == 4");
    CHECK(((uintptr_t)obj4 % 16) == 0, "16-byte aligned");

    /* Payload should be zeroed. */
    STA_OOP *payload = sta_payload(obj4);
    for (uint32_t i = 0; i < 4; i++) {
        CHECK(payload[i] == 0, "payload slot zeroed");
    }

    /* Write and read back a payload slot. */
    payload[0] = STA_SMALLINT_OOP(42);
    CHECK(STA_SMALLINT_VAL(payload[0]) == 42, "payload write/read round-trip");

    /* ── Various sizes ───────────────────────────────────── */
    printf("\n=== Various sizes ===\n");
    uint32_t sizes[] = { 1, 2, 3, 5, 8, 16 };
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        STA_ObjHeader *obj = sta_heap_alloc(heap, 9, sizes[i]);
        CHECK(obj != NULL, "alloc succeeds");
        CHECK(obj->size == sizes[i], "size field matches");
        CHECK(((uintptr_t)obj % 16) == 0, "16-byte aligned");
    }

    /* ── Capacity exhaustion ─────────────────────────────── */
    printf("\n=== Capacity exhaustion ===\n");
    STA_Heap *tiny = sta_heap_create(64);
    CHECK(tiny != NULL, "create tiny heap");
    /* Fill with 0-slot objects (16 bytes each). 64 / 16 = 4 objects max. */
    int count = 0;
    while (sta_heap_alloc(tiny, 2, 0) != NULL) { count++; }
    CHECK(count >= 1, "at least one object fits in 64 bytes");
    CHECK(sta_heap_used(tiny) == 64, "used == capacity after filling");
    CHECK(sta_heap_alloc(tiny, 2, 0) == NULL, "alloc returns NULL when full");
    sta_heap_destroy(tiny);

    /* ── Cleanup ─────────────────────────────────────────── */
    sta_heap_destroy(heap);

    printf("\n");
    if (failures == 0) { printf("All checks passed.\n"); return 0; }
    printf("%d check(s) FAILED.\n", failures);
    return 1;
}
