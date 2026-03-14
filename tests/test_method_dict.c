/* tests/test_method_dict.c
 * Phase 1: Method dictionary tests.
 * Epic 2 — Symbol Table and Method Dictionary.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "vm/oop.h"
#include "vm/symbol_table.h"
#include "vm/method_dict.h"
#include "vm/immutable_space.h"
#include "vm/heap.h"

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("  OK: %s\n", msg); } } while(0)

int main(void) {
    int failures = 0;

    /* ── Setup ────────────────────────────────────────────── */
    STA_ImmutableSpace *sp = sta_immutable_space_create(256 * 1024);
    CHECK(sp != NULL, "immutable space created");

    STA_SymbolTable *st = sta_symbol_table_create(64);
    CHECK(st != NULL, "symbol table created");

    STA_Heap *heap = sta_heap_create(256 * 1024);
    CHECK(heap != NULL, "heap created");

    /* ── Create a method dictionary ───────────────────────── */
    printf("\n=== Create ===\n");
    STA_OOP dict = sta_method_dict_create(heap, 8);
    CHECK(dict != 0, "method dict created");

    /* Verify class index. */
    STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)dict;
    CHECK(dh->class_index == 15, "dict class_index == STA_CLS_METHODDICTIONARY (15)");

    /* Tally should be 0. */
    STA_OOP *dpayload = sta_payload(dh);
    CHECK(dpayload[0] == STA_SMALLINT_OOP(0), "initial tally == SmallInt(0)");

    /* ── Insert and lookup ────────────────────────────────── */
    printf("\n=== Insert and lookup ===\n");
    STA_OOP sel_plus = sta_symbol_intern(sp, st, "+", 1);
    CHECK(sel_plus != 0, "intern '+' succeeds");

    /* Use a SmallInt as a stub method OOP. */
    STA_OOP method_42 = STA_SMALLINT_OOP(42);
    int rc = sta_method_dict_insert(heap, dict, sel_plus, method_42);
    CHECK(rc == 0, "insert succeeds");

    STA_OOP found = sta_method_dict_lookup(dict, sel_plus);
    CHECK(found == method_42, "lookup returns correct method");

    /* Tally should be 1. */
    CHECK(dpayload[0] == STA_SMALLINT_OOP(1), "tally == SmallInt(1) after insert");

    /* ── Insert multiple entries ──────────────────────────── */
    printf("\n=== Multiple entries ===\n");
    const char *names[] = {"-", "<", ">", "=", "size"};
    STA_OOP sels[5];
    STA_OOP methods[5];

    for (int i = 0; i < 5; i++) {
        sels[i] = sta_symbol_intern(sp, st, names[i], strlen(names[i]));
        CHECK(sels[i] != 0, "intern selector succeeds");
        methods[i] = STA_SMALLINT_OOP(100 + i);
        rc = sta_method_dict_insert(heap, dict, sels[i], methods[i]);
        CHECK(rc == 0, "insert succeeds");
    }

    /* Verify all findable (including the first one). */
    CHECK(sta_method_dict_lookup(dict, sel_plus) == method_42,
          "'+' still findable after more inserts");
    for (int i = 0; i < 5; i++) {
        CHECK(sta_method_dict_lookup(dict, sels[i]) == methods[i],
              "lookup returns correct method for each selector");
    }

    /* Tally should be 6 (1 + 5). */
    CHECK(dpayload[0] == STA_SMALLINT_OOP(6), "tally == SmallInt(6)");

    /* ── Absent selector returns 0 ────────────────────────── */
    printf("\n=== Absent lookup ===\n");
    STA_OOP absent_sel = sta_symbol_intern(sp, st, "nonexistent", 11);
    CHECK(absent_sel != 0, "intern absent selector succeeds");
    CHECK(sta_method_dict_lookup(dict, absent_sel) == 0,
          "lookup returns 0 for absent selector");

    /* ── Update existing entry ────────────────────────────── */
    printf("\n=== Update existing ===\n");
    STA_OOP new_method = STA_SMALLINT_OOP(999);
    rc = sta_method_dict_insert(heap, dict, sel_plus, new_method);
    CHECK(rc == 0, "update insert succeeds");
    CHECK(sta_method_dict_lookup(dict, sel_plus) == new_method,
          "lookup returns updated method");
    /* Tally should still be 6 (update, not new entry). */
    CHECK(dpayload[0] == STA_SMALLINT_OOP(6), "tally unchanged after update");

    /* ── Growth ───────────────────────────────────────────── */
    printf("\n=== Growth ===\n");
    /* Create a fresh dict with capacity 4 to force growth quickly. */
    STA_OOP small_dict = sta_method_dict_create(heap, 4);
    CHECK(small_dict != 0, "small dict created");

    STA_OOP growth_sels[20];
    STA_OOP growth_methods[20];
    char buf[32];

    for (int i = 0; i < 20; i++) {
        int n = snprintf(buf, sizeof(buf), "method_%d:", i);
        growth_sels[i] = sta_symbol_intern(sp, st, buf, (size_t)n);
        CHECK(growth_sels[i] != 0, "intern growth selector succeeds");
        growth_methods[i] = STA_SMALLINT_OOP(200 + i);
        rc = sta_method_dict_insert(heap, small_dict, growth_sels[i], growth_methods[i]);
        CHECK(rc == 0, "insert into growing dict succeeds");
    }

    /* Verify all 20 are still findable after multiple growths. */
    int all_found = 1;
    for (int i = 0; i < 20; i++) {
        STA_OOP m = sta_method_dict_lookup(small_dict, growth_sels[i]);
        if (m != growth_methods[i]) { all_found = 0; break; }
    }
    CHECK(all_found, "all 20 entries findable after growth");

    /* Verify tally. */
    STA_ObjHeader *sdh = (STA_ObjHeader *)(uintptr_t)small_dict;
    STA_OOP *sdp = sta_payload(sdh);
    CHECK(sdp[0] == STA_SMALLINT_OOP(20), "tally == SmallInt(20) after 20 inserts");

    /* ── Cleanup ──────────────────────────────────────────── */
    sta_symbol_table_destroy(st);
    sta_immutable_space_destroy(sp);
    sta_heap_destroy(heap);

    printf("\n");
    if (failures == 0) { printf("All checks passed.\n"); return 0; }
    printf("%d check(s) FAILED.\n", failures);
    return 1;
}
