/* tests/test_image_format.c
 * Unit tests for image format structs, OOP encoding, and FNV-1a hash.
 * Phase 1, Epic 10.
 */
#include "image/image.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-60s", #name); \
    name(); \
    tests_passed++; tests_run++; \
    printf("PASS\n"); \
} while(0)

/* ── Struct size assertions ─────────────────────────────────────────────── */

static void test_header_size(void) {
    assert(sizeof(STA_ImageHeader) == 48);
}

static void test_objrecord_size(void) {
    assert(sizeof(STA_ObjRecord) == 16);
}

static void test_immutable_entry_size(void) {
    assert(sizeof(STA_ImmutableEntry) == 10);
}

static void test_reloc_entry_size(void) {
    assert(sizeof(STA_RelocEntry) == 8);
}

/* ── OOP encoding round-trip ────────────────────────────────────────────── */

static void test_snap_encode_roundtrip(void) {
    /* Test several object IDs. */
    uint32_t ids[] = { 0, 1, 42, 1000, 0xFFFFFF, UINT32_MAX >> 2 };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        uint64_t encoded = STA_SNAP_ENCODE(ids[i]);
        assert(STA_SNAP_IS_HEAP(encoded));
        assert(STA_SNAP_GET_ID(encoded) == ids[i]);
    }
}

static void test_snap_is_heap_false_for_smallint(void) {
    /* SmallInt OOP: bit 0 = 1. Only some will have bits 1:0 == 11, but
     * is_heap should only match tag 0x3. */
    STA_OOP si = STA_SMALLINT_OOP(42);
    /* SmallInt 42: (42 << 1) | 1 = 85, bits 1:0 = 01, not 11. */
    assert(!STA_SNAP_IS_HEAP(si));

    /* Odd SmallInt: STA_SMALLINT_OOP(1) = 3, bits 1:0 = 11.
     * This collides with SNAP_HEAP_TAG, which is why the reloc table
     * is the authoritative discriminator. But the macro itself does match. */
    STA_OOP si_odd = STA_SMALLINT_OOP(1);
    /* This is expected: (1 << 1) | 1 = 3, which matches SNAP_HEAP_TAG.
     * In practice, only reloc-table slots are decoded as heap OOPs. */
    (void)si_odd;
}

static void test_snap_is_heap_false_for_char(void) {
    STA_OOP ch = STA_CHAR_OOP('A');
    /* Character: bits 1:0 = 10. */
    assert(!STA_SNAP_IS_HEAP(ch));
}

/* ── FNV-1a hash ────────────────────────────────────────────────────────── */

static void test_fnv1a_nil(void) {
    uint32_t h = sta_fnv1a("nil", 3);
    /* FNV-1a of "nil": known deterministic value. */
    assert(h != 0);
    /* Verify idempotent. */
    assert(h == sta_fnv1a("nil", 3));
}

static void test_fnv1a_distinct(void) {
    uint32_t h_nil   = sta_fnv1a("nil", 3);
    uint32_t h_true  = sta_fnv1a("true", 4);
    uint32_t h_false = sta_fnv1a("false", 5);
    assert(h_nil != h_true);
    assert(h_nil != h_false);
    assert(h_true != h_false);
}

static void test_fnv1a_empty(void) {
    /* FNV-1a of empty string is the offset basis. */
    assert(sta_fnv1a("", 0) == 2166136261u);
}

/* ── Root table constants ───────────────────────────────────────────────── */

static void test_root_constants(void) {
    assert(STA_IMAGE_ROOT_SPECIAL_OBJECTS == 0);
    assert(STA_IMAGE_ROOT_CLASS_TABLE == 1);
    assert(STA_IMAGE_ROOT_GLOBALS == 2);
    assert(STA_IMAGE_ROOT_COUNT == 3);
}

/* ── Magic and version constants ────────────────────────────────────────── */

static void test_magic_bytes(void) {
    assert(STA_IMAGE_MAGIC_LEN == 4);
    assert(memcmp(STA_IMAGE_MAGIC, "\x53\x54\x41\x01", 4) == 0);
}

static void test_version(void) {
    assert(STA_IMAGE_VERSION == 1);
}

/* ── Main ───────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_image_format:\n");

    RUN(test_header_size);
    RUN(test_objrecord_size);
    RUN(test_immutable_entry_size);
    RUN(test_reloc_entry_size);
    RUN(test_snap_encode_roundtrip);
    RUN(test_snap_is_heap_false_for_smallint);
    RUN(test_snap_is_heap_false_for_char);
    RUN(test_fnv1a_nil);
    RUN(test_fnv1a_distinct);
    RUN(test_fnv1a_empty);
    RUN(test_root_constants);
    RUN(test_magic_bytes);
    RUN(test_version);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
