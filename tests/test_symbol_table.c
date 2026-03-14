/* tests/test_symbol_table.c
 * Phase 1: Symbol table and special selectors tests.
 * Epic 2 — Symbol Table and Method Dictionary.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "vm/oop.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include "vm/special_objects.h"
#include "vm/special_selectors.h"

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("  OK: %s\n", msg); } } while(0)

int main(void) {
    int failures = 0;

    /* ── Setup ────────────────────────────────────────────── */
    STA_ImmutableSpace *sp = sta_immutable_space_create(1024 * 1024);
    CHECK(sp != NULL, "immutable space created");

    STA_SymbolTable *st = sta_symbol_table_create(16);
    CHECK(st != NULL, "symbol table created");

    /* ── Intern and lookup ────────────────────────────────── */
    printf("\n=== Intern and lookup ===\n");
    STA_OOP hello = sta_symbol_intern(sp, st, "hello", 5);
    CHECK(hello != 0, "intern 'hello' succeeds");

    STA_OOP found = sta_symbol_lookup(st, "hello", 5);
    CHECK(found == hello, "lookup returns same OOP as intern");

    /* ── Idempotent intern ────────────────────────────────── */
    printf("\n=== Idempotent intern ===\n");
    STA_OOP hello2 = sta_symbol_intern(sp, st, "hello", 5);
    CHECK(hello2 == hello, "second intern returns same OOP");

    /* ── Different strings get different OOPs ─────────────── */
    printf("\n=== Different strings ===\n");
    STA_OOP world = sta_symbol_intern(sp, st, "world", 5);
    CHECK(world != 0, "intern 'world' succeeds");
    CHECK(world != hello, "different strings get different OOPs");

    /* ── Hash stored in payload slot 0 ────────────────────── */
    printf("\n=== Hash verification ===\n");
    uint32_t expected_hash = sta_symbol_hash("hello", 5);
    uint32_t stored_hash   = sta_symbol_get_hash(hello);
    CHECK(stored_hash == expected_hash,
          "stored hash matches sta_symbol_hash()");

    uint32_t world_hash = sta_symbol_hash("world", 5);
    CHECK(sta_symbol_get_hash(world) == world_hash,
          "world hash also matches");

    /* ── Round-trip bytes ─────────────────────────────────── */
    printf("\n=== Byte round-trip ===\n");
    size_t len = 0;
    const char *bytes = sta_symbol_get_bytes(hello, &len);
    CHECK(len == 5, "hello length == 5");
    CHECK(memcmp(bytes, "hello", 5) == 0, "hello bytes match");

    bytes = sta_symbol_get_bytes(world, &len);
    CHECK(len == 5, "world length == 5");
    CHECK(memcmp(bytes, "world", 5) == 0, "world bytes match");

    /* ── NUL-terminated for C convenience ──────────────────── */
    printf("\n=== NUL termination ===\n");
    bytes = sta_symbol_get_bytes(hello, NULL);
    CHECK(bytes[5] == '\0', "hello is NUL-terminated");

    /* ── Table growth ─────────────────────────────────────── */
    printf("\n=== Table growth ===\n");
    /* Start with capacity 16; 70% threshold = ~11 entries.
     * Intern 50 symbols to force at least one growth. */
    STA_OOP syms[50];
    char buf[32];
    for (int i = 0; i < 50; i++) {
        int n = snprintf(buf, sizeof(buf), "sym_%03d", i);
        syms[i] = sta_symbol_intern(sp, st, buf, (size_t)n);
        CHECK(syms[i] != 0, "intern sym succeeds");
    }

    /* Verify all 50 are still findable. */
    int all_found = 1;
    for (int i = 0; i < 50; i++) {
        int n = snprintf(buf, sizeof(buf), "sym_%03d", i);
        STA_OOP f = sta_symbol_lookup(st, buf, (size_t)n);
        if (f != syms[i]) { all_found = 0; break; }
    }
    CHECK(all_found, "all 50 symbols still findable after growth");

    /* ── Lookup returns 0 for absent symbol ────────────────── */
    printf("\n=== Absent lookup ===\n");
    STA_OOP absent = sta_symbol_lookup(st, "nonexistent", 11);
    CHECK(absent == 0, "lookup returns 0 for absent symbol");

    /* ── Immutable flags ──────────────────────────────────── */
    printf("\n=== Immutable flags ===\n");
    STA_ObjHeader *hdr = (STA_ObjHeader *)(uintptr_t)hello;
    CHECK((hdr->obj_flags & STA_OBJ_IMMUTABLE) != 0,
          "symbol has STA_OBJ_IMMUTABLE");
    CHECK((hdr->obj_flags & STA_OBJ_SHARED_IMM) != 0,
          "symbol has STA_OBJ_SHARED_IMM");
    CHECK(hdr->class_index == 7, "symbol class_index == STA_CLS_SYMBOL (7)");

    /* ── Special selectors ────────────────────────────────── */
    printf("\n=== Special selectors ===\n");
    sta_special_objects_init();
    int rc = sta_intern_special_selectors(sp, st);
    CHECK(rc == 0, "sta_intern_special_selectors succeeds");

    /* SPC_SPECIAL_SELECTORS should be a 32-element Array. */
    STA_OOP sel_arr = sta_spc_get(SPC_SPECIAL_SELECTORS);
    CHECK(sel_arr != 0, "SPC_SPECIAL_SELECTORS is set");

    STA_ObjHeader *sel_hdr = (STA_ObjHeader *)(uintptr_t)sel_arr;
    CHECK(sel_hdr->class_index == 9, "special selectors Array class_index == 9");
    CHECK(sel_hdr->size == 32, "special selectors Array size == 32");

    /* Verify a few selectors by content. */
    STA_OOP *sel_slots = sta_payload(sel_hdr);

    const char *plus_bytes = sta_symbol_get_bytes(sel_slots[0], &len);
    CHECK(len == 1 && plus_bytes[0] == '+', "selector[0] == '+'");

    const char *at_bytes = sta_symbol_get_bytes(sel_slots[16], &len);
    CHECK(len == 3 && memcmp(at_bytes, "at:", 3) == 0, "selector[16] == 'at:'");

    const char *dnu_bytes = sta_symbol_get_bytes(sel_slots[30], &len);
    CHECK(len == 18 && memcmp(dnu_bytes, "doesNotUnderstand:", 18) == 0,
          "selector[30] == 'doesNotUnderstand:'");

    const char *mbb_bytes = sta_symbol_get_bytes(sel_slots[31], &len);
    CHECK(len == 13 && memcmp(mbb_bytes, "mustBeBoolean", 13) == 0,
          "selector[31] == 'mustBeBoolean'");

    /* Verify individual special object entries. */
    CHECK(sta_spc_get(SPC_DOES_NOT_UNDERSTAND) == sel_slots[30],
          "SPC_DOES_NOT_UNDERSTAND matches selector[30]");
    CHECK(sta_spc_get(SPC_MUST_BE_BOOLEAN) == sel_slots[31],
          "SPC_MUST_BE_BOOLEAN matches selector[31]");

    STA_OOP cr = sta_spc_get(SPC_CANNOT_RETURN);
    CHECK(cr != 0, "SPC_CANNOT_RETURN is set");
    const char *cr_bytes = sta_symbol_get_bytes(cr, &len);
    CHECK(len == 13 && memcmp(cr_bytes, "cannotReturn:", 13) == 0,
          "SPC_CANNOT_RETURN == 'cannotReturn:'");

    STA_OOP su = sta_spc_get(SPC_STARTUP);
    CHECK(su != 0, "SPC_STARTUP is set");
    const char *su_bytes = sta_symbol_get_bytes(su, &len);
    CHECK(len == 7 && memcmp(su_bytes, "startUp", 7) == 0,
          "SPC_STARTUP == 'startUp'");

    STA_OOP sd = sta_spc_get(SPC_SHUTDOWN);
    CHECK(sd != 0, "SPC_SHUTDOWN is set");
    const char *sd_bytes = sta_symbol_get_bytes(sd, &len);
    CHECK(len == 8 && memcmp(sd_bytes, "shutDown", 8) == 0,
          "SPC_SHUTDOWN == 'shutDown'");

    STA_OOP run = sta_spc_get(SPC_RUN);
    CHECK(run != 0, "SPC_RUN is set");
    const char *run_bytes = sta_symbol_get_bytes(run, &len);
    CHECK(len == 3 && memcmp(run_bytes, "run", 3) == 0,
          "SPC_RUN == 'run'");

    /* Each element of the special selector array is a Symbol. */
    int all_symbols = 1;
    for (int i = 0; i < 32; i++) {
        if (sel_slots[i] == 0) { all_symbols = 0; break; }
        STA_ObjHeader *sh = (STA_ObjHeader *)(uintptr_t)sel_slots[i];
        if (sh->class_index != 7) { all_symbols = 0; break; }
    }
    CHECK(all_symbols, "all 32 special selector entries are Symbols");

    /* ── Cleanup ──────────────────────────────────────────── */
    sta_symbol_table_destroy(st);
    sta_immutable_space_destroy(sp);

    printf("\n");
    if (failures == 0) { printf("All checks passed.\n"); return 0; }
    printf("%d check(s) FAILED.\n", failures);
    return 1;
}
