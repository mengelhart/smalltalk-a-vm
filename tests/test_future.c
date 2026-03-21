/* tests/test_future.c
 * Tests for STA_Future and STA_FutureTable — Phase 2 Epic 7A Story 1.
 */
#include "actor/future.h"
#include "actor/future_table.h"
#include "vm/oop.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    printf("  %-50s", #fn); \
    tests_run++; \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

/* ── Test 1: Create a PENDING future ──────────────────────────────── */

static void test_future_create_pending(void) {
    STA_FutureTable *table = sta_future_table_create(256);
    assert(table);

    STA_Future *f = sta_future_table_new(table, 42);
    assert(f);
    assert(atomic_load(&f->state) == STA_FUTURE_PENDING);
    assert(f->sender_id == 42);
    assert(f->future_id > 0);
    assert(atomic_load(&f->refcount) == 2);  /* caller + table */

    sta_future_release(f);
    sta_future_table_destroy(table);
}

/* ── Test 2: Resolve a future ─────────────────────────────────────── */

static void test_future_resolve(void) {
    STA_FutureTable *table = sta_future_table_create(256);
    STA_Future *f = sta_future_table_new(table, 1);

    STA_OOP *buf = malloc(sizeof(STA_OOP));
    buf[0] = STA_SMALLINT_OOP(42);

    bool won = sta_future_resolve(f, buf, 1, NULL);
    assert(won);
    assert(atomic_load(&f->state) == STA_FUTURE_RESOLVED);
    assert(f->result_buf[0] == STA_SMALLINT_OOP(42));
    assert(f->result_count == 1);
    assert(f->transfer_heap == NULL);

    sta_future_release(f);  /* release caller ref */
    sta_future_table_destroy(table);  /* releases table ref */
}

/* ── Test 3: Fail a future ────────────────────────────────────────── */

static void test_future_fail(void) {
    STA_FutureTable *table = sta_future_table_create(256);
    STA_Future *f = sta_future_table_new(table, 1);

    STA_OOP *buf = malloc(sizeof(STA_OOP));
    buf[0] = STA_SMALLINT_OOP(99);

    bool won = sta_future_fail(f, buf, 1);
    assert(won);
    assert(atomic_load(&f->state) == STA_FUTURE_FAILED);
    assert(f->result_buf[0] == STA_SMALLINT_OOP(99));
    assert(f->result_count == 1);

    sta_future_release(f);  /* caller ref */
    sta_future_table_destroy(table);  /* table ref */
}

/* ── Test 4: Double resolve — second returns false ────────────────── */

static void test_future_double_resolve(void) {
    STA_FutureTable *table = sta_future_table_create(256);
    STA_Future *f = sta_future_table_new(table, 1);

    STA_OOP *buf1 = malloc(sizeof(STA_OOP));
    buf1[0] = STA_SMALLINT_OOP(10);
    bool won1 = sta_future_resolve(f, buf1, 1, NULL);
    assert(won1);

    STA_OOP *buf2 = malloc(sizeof(STA_OOP));
    buf2[0] = STA_SMALLINT_OOP(20);
    bool won2 = sta_future_resolve(f, buf2, 1, NULL);
    assert(!won2);

    /* First result preserved. */
    assert(atomic_load(&f->state) == STA_FUTURE_RESOLVED);
    assert(f->result_buf[0] == STA_SMALLINT_OOP(10));

    sta_future_release(f);
    sta_future_table_destroy(table);
}

/* ── Test 5: Resolve vs fail race (multi-threaded) ────────────────── */

typedef struct {
    STA_Future *future;
    _Atomic int resolve_wins;
    _Atomic int fail_wins;
} RaceCtx;

static void *race_resolver(void *arg) {
    RaceCtx *ctx = (RaceCtx *)arg;
    STA_OOP *buf = malloc(sizeof(STA_OOP));
    buf[0] = STA_SMALLINT_OOP(1);
    if (sta_future_resolve(ctx->future, buf, 1, NULL))
        atomic_fetch_add(&ctx->resolve_wins, 1);
    return NULL;
}

static void *race_failer(void *arg) {
    RaceCtx *ctx = (RaceCtx *)arg;
    STA_OOP *buf = malloc(sizeof(STA_OOP));
    buf[0] = STA_SMALLINT_OOP(2);
    if (sta_future_fail(ctx->future, buf, 1))
        atomic_fetch_add(&ctx->fail_wins, 1);
    return NULL;
}

static void test_future_resolve_vs_fail(void) {
    int total_resolve = 0, total_fail = 0;

    for (int iter = 0; iter < 100; iter++) {
        STA_FutureTable *table = sta_future_table_create(8);
        STA_Future *f = sta_future_table_new(table, 1);

        RaceCtx ctx = { .future = f };
        atomic_store(&ctx.resolve_wins, 0);
        atomic_store(&ctx.fail_wins, 0);

        pthread_t t1, t2;
        pthread_create(&t1, NULL, race_resolver, &ctx);
        pthread_create(&t2, NULL, race_failer, &ctx);
        pthread_join(t1, NULL);
        pthread_join(t2, NULL);

        uint32_t state = atomic_load(&f->state);
        assert(state == STA_FUTURE_RESOLVED || state == STA_FUTURE_FAILED);
        assert(atomic_load(&ctx.resolve_wins) + atomic_load(&ctx.fail_wins) == 1);

        total_resolve += atomic_load(&ctx.resolve_wins);
        total_fail += atomic_load(&ctx.fail_wins);

        sta_future_release(f);
        sta_future_table_destroy(table);
    }

    printf("[resolve=%d fail=%d] ", total_resolve, total_fail);
}

/* ── Test 6: Refcount retain/release ──────────────────────────────── */

static void test_future_refcount(void) {
    STA_FutureTable *table = sta_future_table_create(256);
    STA_Future *f = sta_future_table_new(table, 1);

    assert(atomic_load(&f->refcount) == 2);  /* caller + table */

    sta_future_retain(f);
    assert(atomic_load(&f->refcount) == 3);

    sta_future_release(f);
    assert(atomic_load(&f->refcount) == 2);

    /* Remove from table (does not release), then release caller ref. */
    sta_future_table_remove(table, f->future_id);
    /* Now only caller's ref remains (refcount=2 from table_new, but
     * table no longer holds a pointer — destroy won't release it).
     * We need to release twice: once for table's ref, once for ours.
     * Actually, remove doesn't decrement refcount — it just removes
     * the pointer. The future still has refcount=2. Release both. */
    sta_future_release(f);  /* table's conceptual ref */
    sta_future_release(f);  /* caller's ref — frees */

    sta_future_table_destroy(table);
}

/* ── Test 7: Table lookup ─────────────────────────────────────────── */

static void test_future_table_lookup(void) {
    STA_FutureTable *table = sta_future_table_create(256);
    STA_Future *f = sta_future_table_new(table, 1);
    uint32_t id = f->future_id;

    STA_Future *found = sta_future_table_lookup(table, id);
    assert(found != NULL);
    assert(found->future_id == id);
    sta_future_release(found);  /* release lookup ref */

    /* Non-existent ID. */
    STA_Future *missing = sta_future_table_lookup(table, 999999);
    assert(missing == NULL);

    sta_future_release(f);
    sta_future_table_destroy(table);
}

/* ── Test 8: Table remove ─────────────────────────────────────────── */

static void test_future_table_remove(void) {
    STA_FutureTable *table = sta_future_table_create(256);
    STA_Future *f = sta_future_table_new(table, 1);
    uint32_t id = f->future_id;

    sta_future_table_remove(table, id);

    STA_Future *found = sta_future_table_lookup(table, id);
    assert(found == NULL);

    /* f is still valid (caller's ref), release it. */
    sta_future_release(f);
    sta_future_table_destroy(table);
}

/* ── Test 9: Table grow (insert 200 into capacity 256) ────────────── */

static void test_future_table_grow(void) {
    STA_FutureTable *table = sta_future_table_create(256);
    uint32_t ids[200];

    for (int i = 0; i < 200; i++) {
        STA_Future *f = sta_future_table_new(table, 1);
        assert(f);
        ids[i] = f->future_id;
        /* Table holds the ref; we keep the pointer for the ID only.
         * We need to release our ref from sta_future_table_new. */
        sta_future_release(f);
    }

    /* Verify all 200 can be looked up. */
    for (int i = 0; i < 200; i++) {
        STA_Future *found = sta_future_table_lookup(table, ids[i]);
        assert(found != NULL);
        assert(found->future_id == ids[i]);
        sta_future_release(found);
    }

    sta_future_table_destroy(table);
}

/* ── Test 10: Table destroy releases all futures ──────────────────── */

static void test_future_table_destroy_cleanup(void) {
    STA_FutureTable *table = sta_future_table_create(256);

    for (int i = 0; i < 10; i++) {
        STA_Future *f = sta_future_table_new(table, 1);
        assert(f);
        /* Release the caller's ref. Table still holds its ref (refcount=1). */
        sta_future_release(f);
    }

    /* Destroy table — releases the table's ref for each (refcount→0, freed).
     * ASan validates all 10 futures are freed with no leaks. */
    sta_future_table_destroy(table);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    printf("sizeof(STA_Future)      = %zu bytes\n", sizeof(STA_Future));
    printf("sizeof(STA_FutureTable) = %zu bytes\n", sizeof(STA_FutureTable));
    printf("\n");

    printf("test_future:\n");
    RUN_TEST(test_future_create_pending);
    RUN_TEST(test_future_resolve);
    RUN_TEST(test_future_fail);
    RUN_TEST(test_future_double_resolve);
    RUN_TEST(test_future_resolve_vs_fail);
    RUN_TEST(test_future_refcount);
    RUN_TEST(test_future_table_lookup);
    RUN_TEST(test_future_table_remove);
    RUN_TEST(test_future_table_grow);
    RUN_TEST(test_future_table_destroy_cleanup);

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
