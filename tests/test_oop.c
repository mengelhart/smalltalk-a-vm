/* tests/test_oop.c
 * Phase 1: Production OOP and STA_ObjHeader layout tests.
 * Story 1 of Epic 1 — Object Memory and Allocator.
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "vm/oop.h"

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("  OK: %s\n", msg); } } while(0)

int main(void) {
    int failures = 0;

    /* ── Header layout ─────────────────────────────────── */
    printf("\n=== STA_ObjHeader layout ===\n");
    CHECK(sizeof(STA_ObjHeader) == 12,               "sizeof(STA_ObjHeader) == 12");
    CHECK(STA_HEADER_SIZE == 12,                     "STA_HEADER_SIZE == 12");
    CHECK(STA_ALLOC_UNIT == 16,                      "STA_ALLOC_UNIT == 16");
    CHECK(offsetof(STA_ObjHeader, class_index) == 0, "class_index at offset 0");
    CHECK(offsetof(STA_ObjHeader, size)        == 4, "size at offset 4");
    CHECK(offsetof(STA_ObjHeader, gc_flags)    == 8, "gc_flags at offset 8");
    CHECK(offsetof(STA_ObjHeader, obj_flags)   == 9, "obj_flags at offset 9");
    CHECK(offsetof(STA_ObjHeader, reserved)    == 10,"reserved at offset 10");
    CHECK(sizeof(STA_OOP) == 8,                      "sizeof(STA_OOP) == 8");

    /* ── sta_alloc_size ──────────────────────────────────── */
    printf("\n=== sta_alloc_size ===\n");
    CHECK(sta_alloc_size(0) == 16,  "alloc_size(0) == 16");
    CHECK(sta_alloc_size(1) == 24,  "alloc_size(1) == 24");
    CHECK(sta_alloc_size(4) == 48,  "alloc_size(4) == 48");
    CHECK(sta_alloc_size(8) == 80,  "alloc_size(8) == 80");

    /* ── Payload alignment ───────────────────────────────── */
    printf("\n=== Payload alignment ===\n");
    _Alignas(16) char buf[64];
    STA_ObjHeader *h = (STA_ObjHeader *)buf;
    STA_OOP *payload  = sta_payload(h);
    CHECK(((uintptr_t)payload % 16) == 0,    "payload 16-byte aligned");
    CHECK((char *)payload - (char *)h == 16, "payload at offset 16 from header");

    /* ── SmallInt tagging ────────────────────────────────── */
    printf("\n=== SmallInt tagging ===\n");
    {
        intptr_t sv[] = {
            0, 1, -1, 42, -42, 1000000,
            INT32_MAX, INT32_MIN,
            (intptr_t) 4611686018427387903LL,   /* 2^62 - 1  */
            (intptr_t)-4611686018427387904LL,   /* -(2^62)   */
        };
        for (size_t i = 0; i < sizeof(sv)/sizeof(sv[0]); i++) {
            STA_OOP oop = STA_SMALLINT_OOP(sv[i]);
            CHECK(STA_IS_SMALLINT(oop),            "smallint: IS_SMALLINT");
            CHECK(!STA_IS_CHAR(oop),               "smallint: not IS_CHAR");
            CHECK(!STA_IS_HEAP(oop),               "smallint: not IS_HEAP");
            CHECK(STA_IS_IMMEDIATE(oop),           "smallint: IS_IMMEDIATE");
            CHECK(STA_SMALLINT_VAL(oop) == sv[i],  "smallint: round-trip");
        }
    }

    /* ── Character tagging ───────────────────────────────── */
    printf("\n=== Character tagging ===\n");
    {
        uint32_t cp[] = { 0, 65, 0x0041, 0x1F600, 0x10FFFF };
        for (size_t i = 0; i < sizeof(cp)/sizeof(cp[0]); i++) {
            STA_OOP oop = STA_CHAR_OOP(cp[i]);
            CHECK(STA_IS_CHAR(oop),                "char: IS_CHAR");
            CHECK(!STA_IS_SMALLINT(oop),           "char: not IS_SMALLINT");
            CHECK(!STA_IS_HEAP(oop),               "char: not IS_HEAP");
            CHECK(STA_IS_IMMEDIATE(oop),           "char: IS_IMMEDIATE");
            CHECK(STA_CHAR_VAL(oop) == cp[i],      "char: round-trip");
        }
        /* Character has bit-0 == 0; must NOT pass IS_HEAP (bits 1:0 must be 00) */
        STA_OOP char_a = STA_CHAR_OOP('A');
        CHECK(!STA_IS_HEAP(char_a), "char OOP is NOT a heap pointer");
    }

    /* ── Heap pointer detection ─────────────────────────── */
    printf("\n=== Heap pointer detection ===\n");
    {
        _Alignas(16) char obj[32];
        STA_OOP heap_oop = (STA_OOP)(uintptr_t)obj;
        CHECK(STA_IS_HEAP(heap_oop),       "heap ptr: IS_HEAP");
        CHECK(!STA_IS_SMALLINT(heap_oop),  "heap ptr: not IS_SMALLINT");
        CHECK(!STA_IS_CHAR(heap_oop),      "heap ptr: not IS_CHAR");
        CHECK(!STA_IS_IMMEDIATE(heap_oop), "heap ptr: not IS_IMMEDIATE");
    }

    /* ── gc_flags bits ──────────────────────────────────── */
    printf("\n=== gc_flags ===\n");
    {
        STA_ObjHeader hdr;
        memset(&hdr, 0, sizeof(hdr));

        hdr.gc_flags = STA_GC_WHITE;
        CHECK((hdr.gc_flags & STA_GC_COLOR_MASK) == STA_GC_WHITE, "GC white");
        hdr.gc_flags = STA_GC_GREY;
        CHECK((hdr.gc_flags & STA_GC_COLOR_MASK) == STA_GC_GREY,  "GC grey");
        hdr.gc_flags = STA_GC_BLACK;
        CHECK((hdr.gc_flags & STA_GC_COLOR_MASK) == STA_GC_BLACK, "GC black");
        hdr.gc_flags = (uint8_t)(STA_GC_FORWARDED | STA_GC_BLACK);
        CHECK((hdr.gc_flags & STA_GC_FORWARDED) != 0, "forwarded + black");
        CHECK((hdr.gc_flags & 0xF0u) == 0,            "gc reserved bits zero");
        hdr.gc_flags = STA_GC_REMEMBERED;
        CHECK((hdr.gc_flags & STA_GC_REMEMBERED) != 0,"remembered bit");
    }

    /* ── obj_flags bits ─────────────────────────────────── */
    printf("\n=== obj_flags ===\n");
    {
        STA_ObjHeader hdr;
        memset(&hdr, 0, sizeof(hdr));

        hdr.obj_flags = (uint8_t)(STA_OBJ_IMMUTABLE | STA_OBJ_SHARED_IMM);
        CHECK((hdr.obj_flags & STA_OBJ_IMMUTABLE) != 0,  "IMMUTABLE bit");
        CHECK((hdr.obj_flags & STA_OBJ_SHARED_IMM) != 0, "SHARED_IMM bit");
        CHECK((hdr.obj_flags & STA_OBJ_PINNED) == 0,     "PINNED not set");
        CHECK((hdr.obj_flags & 0xE0u) == 0,               "obj reserved bits zero");
        hdr.obj_flags = STA_OBJ_FINALIZE;
        CHECK((hdr.obj_flags & STA_OBJ_FINALIZE) != 0,   "FINALIZE bit");
    }

    /* ── nil/true/false: heap objects, no special tag ───── */
    printf("\n=== nil/true/false identity ===\n");
    {
        /* Per ADR 007: nil/true/false are heap objects in the shared immutable
         * region. isNil is a pointer compare against a known address. */
        _Alignas(16) char nil_buf[16], true_buf[16], false_buf[16];
        STA_OOP nil_oop   = (STA_OOP)(uintptr_t)nil_buf;
        STA_OOP true_oop  = (STA_OOP)(uintptr_t)true_buf;
        STA_OOP false_oop = (STA_OOP)(uintptr_t)false_buf;
        CHECK(STA_IS_HEAP(nil_oop),    "nil is a heap object");
        CHECK(STA_IS_HEAP(true_oop),   "true is a heap object");
        CHECK(STA_IS_HEAP(false_oop),  "false is a heap object");
        CHECK(nil_oop != true_oop,     "nil != true");
        CHECK(nil_oop != false_oop,    "nil != false");
        CHECK(true_oop != false_oop,   "true != false");
    }

    /* ── Summary ─────────────────────────────────────────── */
    printf("\n");
    if (failures == 0) { printf("All checks passed.\n"); return 0; }
    printf("%d check(s) FAILED.\n", failures);
    return 1;
}
