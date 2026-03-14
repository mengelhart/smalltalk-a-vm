/* tests/test_special_objects.c
 * Phase 1: Special object table tests.
 * Story 4 of Epic 1 — Object Memory and Allocator.
 */
#include <stdio.h>
#include <stdint.h>
#include "vm/oop.h"
#include "vm/special_objects.h"

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("  OK: %s\n", msg); } } while(0)

int main(void) {
    int failures = 0;

    /* ── Index constants match spec ──────────────────────── */
    printf("\n=== Index constants ===\n");
    CHECK(SPC_NIL == 0,                   "SPC_NIL == 0");
    CHECK(SPC_TRUE == 1,                  "SPC_TRUE == 1");
    CHECK(SPC_FALSE == 2,                 "SPC_FALSE == 2");
    CHECK(SPC_SMALLTALK == 3,             "SPC_SMALLTALK == 3");
    CHECK(SPC_SPECIAL_SELECTORS == 4,     "SPC_SPECIAL_SELECTORS == 4");
    CHECK(SPC_CHARACTER_TABLE == 5,        "SPC_CHARACTER_TABLE == 5");
    CHECK(SPC_DOES_NOT_UNDERSTAND == 6,    "SPC_DOES_NOT_UNDERSTAND == 6");
    CHECK(SPC_CANNOT_RETURN == 7,          "SPC_CANNOT_RETURN == 7");
    CHECK(SPC_MUST_BE_BOOLEAN == 8,        "SPC_MUST_BE_BOOLEAN == 8");
    CHECK(SPC_STARTUP == 9,               "SPC_STARTUP == 9");
    CHECK(SPC_SHUTDOWN == 10,              "SPC_SHUTDOWN == 10");
    CHECK(SPC_RUN == 11,                   "SPC_RUN == 11");
    CHECK(STA_SPECIAL_OBJECTS_COUNT == 32, "table size == 32");

    /* ── Init zeros all entries ──────────────────────────── */
    printf("\n=== Init ===\n");
    sta_special_objects_init();
    for (uint32_t i = 0; i < STA_SPECIAL_OBJECTS_COUNT; i++) {
        CHECK(sta_spc_get(i) == 0, "entry zeroed after init");
    }

    /* ── Set and get ─────────────────────────────────────── */
    printf("\n=== Set/get ===\n");
    /* Simulate setting nil to a known address. */
    _Alignas(16) char nil_buf[16];
    STA_OOP nil_oop = (STA_OOP)(uintptr_t)nil_buf;
    sta_spc_set(SPC_NIL, nil_oop);
    CHECK(sta_spc_get(SPC_NIL) == nil_oop, "get(SPC_NIL) returns set value");

    /* Set true and false. */
    STA_OOP true_oop  = nil_oop + 16;
    STA_OOP false_oop = nil_oop + 32;
    sta_spc_set(SPC_TRUE, true_oop);
    sta_spc_set(SPC_FALSE, false_oop);
    CHECK(sta_spc_get(SPC_TRUE) == true_oop,   "get(SPC_TRUE) correct");
    CHECK(sta_spc_get(SPC_FALSE) == false_oop, "get(SPC_FALSE) correct");

    /* Setting one entry doesn't affect others. */
    CHECK(sta_spc_get(SPC_SMALLTALK) == 0,     "SPC_SMALLTALK still 0");
    CHECK(sta_spc_get(SPC_RUN) == 0,           "SPC_RUN still 0");

    /* Reserved entries (12–31) are still 0. */
    for (uint32_t i = 12; i < STA_SPECIAL_OBJECTS_COUNT; i++) {
        CHECK(sta_spc_get(i) == 0, "reserved entry still 0");
    }

    /* ── SmallInt stored in special objects ───────────────── */
    printf("\n=== SmallInt in table ===\n");
    STA_OOP sm = STA_SMALLINT_OOP(99);
    sta_spc_set(SPC_STARTUP, sm);
    CHECK(sta_spc_get(SPC_STARTUP) == sm, "SmallInt stored and retrieved");
    CHECK(STA_SMALLINT_VAL(sta_spc_get(SPC_STARTUP)) == 99,
          "SmallInt value round-trips through table");

    /* ── Re-init clears everything ───────────────────────── */
    printf("\n=== Re-init ===\n");
    sta_special_objects_init();
    CHECK(sta_spc_get(SPC_NIL) == 0,     "SPC_NIL cleared after re-init");
    CHECK(sta_spc_get(SPC_TRUE) == 0,    "SPC_TRUE cleared after re-init");
    CHECK(sta_spc_get(SPC_STARTUP) == 0, "SPC_STARTUP cleared after re-init");

    printf("\n");
    if (failures == 0) { printf("All checks passed.\n"); return 0; }
    printf("%d check(s) FAILED.\n", failures);
    return 1;
}
