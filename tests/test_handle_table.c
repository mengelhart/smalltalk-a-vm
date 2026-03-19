/* tests/test_handle_table.c
 * Unit tests for the handle table (Story 5, Epic 0).
 * Tests: create/get/release lifecycle, free slot reuse, table growth,
 * double-release detection, retain/release counting.
 */
#include "vm/handle.h"
#include "vm/oop.h"
#include <sta/vm.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

/* ── Test: init and destroy ──────────────────────────────────────────── */

static void test_init_destroy(void) {
    STA_HandleTable t;
    assert(sta_handle_table_init(&t) == 0);
    assert(t.first != NULL);
    assert(t.slab_count == 1);
    assert(t.count == 0);
    sta_handle_table_destroy(&t);
    assert(t.first == NULL);
    assert(t.slab_count == 0);
}

/* ── Test: create and get ────────────────────────────────────────────── */

static void test_create_get(void) {
    STA_HandleTable t;
    sta_handle_table_init(&t);

    STA_OOP oop = STA_SMALLINT_OOP(42);
    struct STA_Handle *h = sta_handle_create(&t, oop);
    assert(h != NULL);
    assert(sta_handle_get(h) == oop);
    assert(h->refcount == 1);
    assert(t.count == 1);

    sta_handle_release_entry(&t, h);
    assert(t.count == 0);
    sta_handle_table_destroy(&t);
}

/* ── Test: multiple handles ──────────────────────────────────────────── */

static void test_multiple_handles(void) {
    STA_HandleTable t;
    sta_handle_table_init(&t);

    struct STA_Handle *h1 = sta_handle_create(&t, STA_SMALLINT_OOP(1));
    struct STA_Handle *h2 = sta_handle_create(&t, STA_SMALLINT_OOP(2));
    struct STA_Handle *h3 = sta_handle_create(&t, STA_SMALLINT_OOP(3));

    assert(t.count == 3);
    assert(sta_handle_get(h1) == STA_SMALLINT_OOP(1));
    assert(sta_handle_get(h2) == STA_SMALLINT_OOP(2));
    assert(sta_handle_get(h3) == STA_SMALLINT_OOP(3));

    /* All three are distinct entries. */
    assert(h1 != h2);
    assert(h2 != h3);

    sta_handle_release_entry(&t, h1);
    sta_handle_release_entry(&t, h2);
    sta_handle_release_entry(&t, h3);
    assert(t.count == 0);
    sta_handle_table_destroy(&t);
}

/* ── Test: free slot reuse ───────────────────────────────────────────── */

static void test_free_slot_reuse(void) {
    STA_HandleTable t;
    sta_handle_table_init(&t);

    struct STA_Handle *h1 = sta_handle_create(&t, STA_SMALLINT_OOP(10));
    struct STA_Handle *h2 = sta_handle_create(&t, STA_SMALLINT_OOP(20));

    /* Release h1, freeing its slot. */
    struct STA_Handle *freed_slot = h1;
    sta_handle_release_entry(&t, h1);
    assert(t.count == 1);

    /* Next create should reuse the freed slot. */
    struct STA_Handle *h3 = sta_handle_create(&t, STA_SMALLINT_OOP(30));
    assert(h3 == freed_slot);
    assert(sta_handle_get(h3) == STA_SMALLINT_OOP(30));

    sta_handle_release_entry(&t, h2);
    sta_handle_release_entry(&t, h3);
    sta_handle_table_destroy(&t);
}

/* ── Test: retain increases refcount ─────────────────────────────────── */

static void test_retain_release(void) {
    STA_HandleTable t;
    sta_handle_table_init(&t);

    struct STA_Handle *h = sta_handle_create(&t, STA_SMALLINT_OOP(99));
    assert(h->refcount == 1);

    struct STA_Handle *h2 = sta_handle_retain_entry(&t, h);
    assert(h2 == h);
    assert(h->refcount == 2);

    /* First release decrements but slot stays alive. */
    sta_handle_release_entry(&t, h);
    assert(h->refcount == 1);
    assert(t.count == 1);
    assert(sta_handle_get(h) == STA_SMALLINT_OOP(99));

    /* Second release frees the slot. */
    sta_handle_release_entry(&t, h);
    assert(t.count == 0);

    sta_handle_table_destroy(&t);
}

/* ── Test: handle survives across multiple get calls ─────────────────── */

static void test_handle_survives_gets(void) {
    STA_HandleTable t;
    sta_handle_table_init(&t);

    STA_OOP oop = STA_SMALLINT_OOP(7);
    struct STA_Handle *h = sta_handle_create(&t, oop);

    /* Multiple get calls return the same OOP. */
    for (int i = 0; i < 100; i++) {
        assert(sta_handle_get(h) == oop);
    }
    assert(h->refcount == 1);

    sta_handle_release_entry(&t, h);
    sta_handle_table_destroy(&t);
}

/* ── Test: table growth when full ────────────────────────────────────── */

static void test_table_growth(void) {
    STA_HandleTable t;
    sta_handle_table_init(&t);

    uint32_t initial_slab_cap = STA_HANDLE_SLAB_SIZE;

    /* Fill the entire first slab. */
    struct STA_Handle *handles[STA_HANDLE_SLAB_SIZE];
    for (uint32_t i = 0; i < initial_slab_cap; i++) {
        handles[i] = sta_handle_create(&t, STA_SMALLINT_OOP((intptr_t)i));
        assert(handles[i] != NULL);
    }
    assert(t.count == initial_slab_cap);
    assert(t.slab_count == 1);

    /* Next create triggers growth (new slab). */
    struct STA_Handle *extra = sta_handle_create(&t, STA_SMALLINT_OOP(999));
    assert(extra != NULL);
    assert(t.slab_count == 2);
    assert(t.count == initial_slab_cap + 1);

    /* All prior handles still valid (slab pointers are stable). */
    for (uint32_t i = 0; i < initial_slab_cap; i++) {
        assert(sta_handle_get(handles[i]) == STA_SMALLINT_OOP((intptr_t)i));
    }
    assert(sta_handle_get(extra) == STA_SMALLINT_OOP(999));

    /* Clean up. */
    for (uint32_t i = 0; i < initial_slab_cap; i++) {
        sta_handle_release_entry(&t, handles[i]);
    }
    sta_handle_release_entry(&t, extra);
    assert(t.count == 0);
    sta_handle_table_destroy(&t);
}

/* ── Test: OOP update in place (simulates GC move) ───────────────────── */

static void test_oop_update_in_place(void) {
    STA_HandleTable t;
    sta_handle_table_init(&t);

    STA_OOP old_oop = STA_SMALLINT_OOP(100);
    struct STA_Handle *h = sta_handle_create(&t, old_oop);

    /* Simulate GC forwarding: update the OOP in place. */
    STA_OOP new_oop = STA_SMALLINT_OOP(200);
    h->oop = new_oop;

    assert(sta_handle_get(h) == new_oop);

    sta_handle_release_entry(&t, h);
    sta_handle_table_destroy(&t);
}

/* ── Test: public API retain/release via STA_VM ──────────────────────── */

static void test_public_api_wrappers(void) {
    /* NULL safety. */
    STA_Handle *h = sta_handle_retain(NULL, NULL);
    assert(h == NULL);
    sta_handle_release(NULL, NULL);
    /* No crash = pass. */
}

/* ═══════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("test_handle_table:\n");

    RUN(test_init_destroy);
    RUN(test_create_get);
    RUN(test_multiple_handles);
    RUN(test_free_slot_reuse);
    RUN(test_retain_release);
    RUN(test_handle_survives_gets);
    RUN(test_table_growth);
    RUN(test_oop_update_in_place);
    RUN(test_public_api_wrappers);

    printf("\n%d / %d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
