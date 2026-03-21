/* tests/test_class_table.c
 * Phase 1: Class table tests.
 * Story 5 of Epic 1 — Object Memory and Allocator.
 */
#include <stdio.h>
#include <stdint.h>
#include "vm/oop.h"
#include "vm/class_table.h"

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("  OK: %s\n", msg); } } while(0)

int main(void) {
    int failures = 0;

    /* ── Reserved index constants match bytecode spec ────── */
    printf("\n=== Reserved class indices ===\n");
    CHECK(STA_CLS_INVALID == 0,               "CLS_INVALID == 0");
    CHECK(STA_CLS_SMALLINTEGER == 1,           "CLS_SMALLINTEGER == 1");
    CHECK(STA_CLS_OBJECT == 2,                "CLS_OBJECT == 2");
    CHECK(STA_CLS_UNDEFINEDOBJ == 3,           "CLS_UNDEFINEDOBJ == 3");
    CHECK(STA_CLS_TRUE == 4,                  "CLS_TRUE == 4");
    CHECK(STA_CLS_FALSE == 5,                 "CLS_FALSE == 5");
    CHECK(STA_CLS_CHARACTER == 6,              "CLS_CHARACTER == 6");
    CHECK(STA_CLS_SYMBOL == 7,                "CLS_SYMBOL == 7");
    CHECK(STA_CLS_STRING == 8,                "CLS_STRING == 8");
    CHECK(STA_CLS_ARRAY == 9,                 "CLS_ARRAY == 9");
    CHECK(STA_CLS_BYTEARRAY == 10,             "CLS_BYTEARRAY == 10");
    CHECK(STA_CLS_FLOAT == 11,                "CLS_FLOAT == 11");
    CHECK(STA_CLS_COMPILEDMETHOD == 12,        "CLS_COMPILEDMETHOD == 12");
    CHECK(STA_CLS_BLOCKCLOSURE == 13,          "CLS_BLOCKCLOSURE == 13");
    CHECK(STA_CLS_ASSOCIATION == 14,           "CLS_ASSOCIATION == 14");
    CHECK(STA_CLS_METHODDICTIONARY == 15,      "CLS_METHODDICTIONARY == 15");
    CHECK(STA_CLS_CLASS == 16,                "CLS_CLASS == 16");
    CHECK(STA_CLS_METACLASS == 17,             "CLS_METACLASS == 17");
    CHECK(STA_CLS_BEHAVIOR == 18,              "CLS_BEHAVIOR == 18");
    CHECK(STA_CLS_CLASSDESCRIPTION == 19,      "CLS_CLASSDESCRIPTION == 19");
    CHECK(STA_CLS_BLOCKDESCRIPTOR == 20,       "CLS_BLOCKDESCRIPTOR == 20");
    CHECK(STA_CLS_MESSAGE == 21,              "CLS_MESSAGE == 21");
    CHECK(STA_CLS_NUMBER == 22,               "CLS_NUMBER == 22");
    CHECK(STA_CLS_MAGNITUDE == 23,             "CLS_MAGNITUDE == 23");
    CHECK(STA_CLS_COLLECTION == 24,            "CLS_COLLECTION == 24");
    CHECK(STA_CLS_SEQCOLLECTION == 25,         "CLS_SEQCOLLECTION == 25");
    CHECK(STA_CLS_ARRAYEDCOLLECTION == 26,     "CLS_ARRAYEDCOLLECTION == 26");
    CHECK(STA_CLS_EXCEPTION == 27,             "CLS_EXCEPTION == 27");
    CHECK(STA_CLS_ERROR == 28,                "CLS_ERROR == 28");
    CHECK(STA_CLS_MESSAGENOTUNDERSTOOD == 29,  "CLS_MESSAGENOTUNDERSTOOD == 29");
    CHECK(STA_CLS_BLOCKCANNOTRETURN == 30,     "CLS_BLOCKCANNOTRETURN == 30");
    CHECK(STA_CLS_SYSTEMDICTIONARY == 31,      "CLS_SYSTEMDICTIONARY == 31");
    CHECK(STA_CLS_FUTURE == 32,                "CLS_FUTURE == 32");
    CHECK(STA_CLS_FUTUREFAILURE == 33,         "CLS_FUTUREFAILURE == 33");
    CHECK(STA_CLS_RESERVED_COUNT == 34,        "CLS_RESERVED_COUNT == 34");

    /* ── Create ──────────────────────────────────────────── */
    printf("\n=== Create ===\n");
    STA_ClassTable *ct = sta_class_table_create();
    CHECK(ct != NULL, "create succeeds");
    CHECK(sta_class_table_capacity(ct) == STA_CLASS_TABLE_INITIAL_CAPACITY,
          "initial capacity == 256");

    /* All entries start at 0. */
    for (uint32_t i = 0; i < STA_CLS_RESERVED_COUNT; i++) {
        CHECK(sta_class_table_get(ct, i) == 0, "initial entry == 0");
    }

    /* ── Index 0 is the invalid sentinel ─────────────────── */
    printf("\n=== Index 0 sentinel ===\n");
    CHECK(sta_class_table_get(ct, 0) == 0, "get(0) returns 0");
    CHECK(sta_class_table_set(ct, 0, STA_SMALLINT_OOP(1)) == -1,
          "set(0, ...) returns -1 (rejected)");
    CHECK(sta_class_table_get(ct, 0) == 0, "get(0) still 0 after rejected set");

    /* ── Register and look up a class ────────────────────── */
    printf("\n=== Set/get ===\n");
    /* Simulate registering the SmallInteger class at index 1. */
    _Alignas(16) char class_buf[64];
    STA_OOP si_class = (STA_OOP)(uintptr_t)class_buf;

    CHECK(sta_class_table_set(ct, STA_CLS_SMALLINTEGER, si_class) == 0,
          "set SmallInteger class succeeds");
    CHECK(sta_class_table_get(ct, STA_CLS_SMALLINTEGER) == si_class,
          "get SmallInteger class returns correct OOP");

    /* Other entries unaffected. */
    CHECK(sta_class_table_get(ct, STA_CLS_OBJECT) == 0,
          "Object entry still 0");

    /* ── Register at a dynamic index (>= 32) ─────────────── */
    printf("\n=== Dynamic index ===\n");
    STA_OOP dyn_class = (STA_OOP)(uintptr_t)(class_buf + 16);
    CHECK(sta_class_table_set(ct, 42, dyn_class) == 0,
          "set at index 42 succeeds");
    CHECK(sta_class_table_get(ct, 42) == dyn_class,
          "get at index 42 returns correct OOP");

    /* ── Out-of-range access ─────────────────────────────── */
    printf("\n=== Out-of-range ===\n");
    CHECK(sta_class_table_get(ct, 999) == 0,
          "get(999) returns 0 (out of range)");
    CHECK(sta_class_table_set(ct, 999, si_class) == -1,
          "set(999, ...) returns -1 (out of range)");

    /* ── Multiple classes ────────────────────────────────── */
    printf("\n=== Multiple classes ===\n");
    for (uint32_t i = STA_CLS_SMALLINTEGER; i < STA_CLS_RESERVED_COUNT; i++) {
        STA_OOP fake_class = STA_SMALLINT_OOP((intptr_t)i * 100);
        CHECK(sta_class_table_set(ct, i, fake_class) == 0, "set succeeds");
    }
    for (uint32_t i = STA_CLS_SMALLINTEGER; i < STA_CLS_RESERVED_COUNT; i++) {
        STA_OOP expected = STA_SMALLINT_OOP((intptr_t)i * 100);
        CHECK(sta_class_table_get(ct, i) == expected, "get matches set");
    }

    sta_class_table_destroy(ct);

    printf("\n");
    if (failures == 0) { printf("All checks passed.\n"); return 0; }
    printf("%d check(s) FAILED.\n", failures);
    return 1;
}
