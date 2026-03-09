/* tests/test_objheader_spike.c
 * Phase 0: ObjHeader layout and OOP tagging spike.
 * Run with: cmake --build build && ./build/tests/test_objheader_spike
 */
#include <stdio.h>
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "../src/vm/oop_spike.h"
#include "../src/vm/actor_spike.h"

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("  OK: %s\n", msg); } } while(0)

int main(void) {
    int failures = 0;

    /* ── Header layout ─────────────────────────────────── */
    printf("\n=== ObjHeader layout ===\n");
    printf("  sizeof(STA_ObjHeader)  = %zu (expect 12)\n", sizeof(STA_ObjHeader));
    printf("  STA_HEADER_ALIGNED     = %u  (expect 16)\n", STA_HEADER_ALIGNED);
    printf("  offsetof class_index   = %zu (expect 0)\n",  offsetof(STA_ObjHeader, class_index));
    printf("  offsetof size          = %zu (expect 4)\n",  offsetof(STA_ObjHeader, size));
    printf("  offsetof gc_flags      = %zu (expect 8)\n",  offsetof(STA_ObjHeader, gc_flags));
    printf("  offsetof obj_flags     = %zu (expect 9)\n",  offsetof(STA_ObjHeader, obj_flags));
    printf("  offsetof reserved      = %zu (expect 10)\n", offsetof(STA_ObjHeader, reserved));

    CHECK(sizeof(STA_ObjHeader) == 12, "ObjHeader is 12 bytes");
    CHECK(offsetof(STA_ObjHeader, class_index) == 0,  "class_index at offset 0");
    CHECK(offsetof(STA_ObjHeader, size)        == 4,  "size at offset 4");
    CHECK(offsetof(STA_ObjHeader, gc_flags)    == 8,  "gc_flags at offset 8");
    CHECK(offsetof(STA_ObjHeader, obj_flags)   == 9,  "obj_flags at offset 9");
    CHECK(offsetof(STA_ObjHeader, reserved)    == 10, "reserved at offset 10");

    /* ── Payload alignment ──────────────────────────────── */
    printf("\n=== Payload alignment ===\n");
    /* Simulate an allocation: 16-byte aligned base, header at offset 0,
     * first payload word at offset STA_HEADER_ALIGNED. */
    _Alignas(16) char buf[64];
    STA_ObjHeader *h = (STA_ObjHeader *)buf;
    STA_OOP *payload = sta_payload(h);
    uintptr_t payload_addr = (uintptr_t)payload;

    printf("  buf address            = %p\n", (void*)buf);
    printf("  payload address        = %p (offset %td from header)\n",
           (void*)payload, (char*)payload - (char*)h);
    printf("  payload 8-byte aligned = %s\n", (payload_addr % 8 == 0) ? "yes" : "NO");

    CHECK((payload_addr % 8) == 0, "payload is 8-byte aligned");
    CHECK((payload_addr % 16) == 0, "payload is also 16-byte aligned (bonus)");

    /* Allocation size helper */
    CHECK(sta_alloc_size(0) == 16, "alloc_size(0 slots) = 16 (header only)");
    CHECK(sta_alloc_size(1) == 24, "alloc_size(1 slot)  = 24");
    CHECK(sta_alloc_size(4) == 48, "alloc_size(4 slots) = 48");

    /* ── OOP tagging ────────────────────────────────────── */
    printf("\n=== OOP tagging ===\n");

    /* SmallInt round-trips */
    intptr_t test_vals[] = { 0, 1, -1, 42, -42, 1000000,
                              INT32_MAX, INT32_MIN,
                              (intptr_t)4611686018427387903LL,   /* 2^62 - 1 */
                              (intptr_t)(-4611686018427387904LL) /* -(2^62) */ };
    for (size_t i = 0; i < sizeof(test_vals)/sizeof(test_vals[0]); i++) {
        STA_OOP oop = STA_SMALLINT_OOP(test_vals[i]);
        intptr_t back = STA_SMALLINT_VAL(oop);
        CHECK(STA_IS_SMALLINT(oop),        "smallint tag set");
        CHECK(!STA_IS_HEAP(oop),           "smallint is not heap");
        CHECK(back == test_vals[i],        "smallint round-trips");
    }

    /* Heap OOP (simulated aligned pointer) */
    _Alignas(8) char obj_buf[32];
    STA_OOP heap_oop = (STA_OOP)(uintptr_t)obj_buf;
    CHECK(!STA_IS_SMALLINT(heap_oop),      "heap pointer: not smallint");
    CHECK(STA_IS_HEAP(heap_oop),           "heap pointer: is heap");
    CHECK((heap_oop & 1u) == 0,            "heap pointer: low bit clear");

    /* ── GC and obj flag bits ───────────────────────────── */
    printf("\n=== Flag bits ===\n");

    STA_ObjHeader hdr;
    memset(&hdr, 0, sizeof(hdr));

    /* GC colors */
    hdr.gc_flags = STA_GC_WHITE;
    CHECK((hdr.gc_flags & STA_GC_COLOR_MASK) == STA_GC_WHITE, "GC white");
    hdr.gc_flags = STA_GC_GREY;
    CHECK((hdr.gc_flags & STA_GC_COLOR_MASK) == STA_GC_GREY,  "GC grey");
    hdr.gc_flags = STA_GC_BLACK;
    CHECK((hdr.gc_flags & STA_GC_COLOR_MASK) == STA_GC_BLACK, "GC black");

    /* Forwarding and remembered-set don't alias the color bits */
    hdr.gc_flags = STA_GC_FORWARDED | STA_GC_BLACK;
    CHECK(hdr.gc_flags & STA_GC_FORWARDED, "forwarded bit set alongside black");
    CHECK((hdr.gc_flags & STA_GC_RESERVED) == 0, "no reserved gc bits set");

    /* obj_flags */
    hdr.obj_flags = STA_OBJ_IMMUTABLE | STA_OBJ_ACTOR_LOCAL;
    CHECK(hdr.obj_flags & STA_OBJ_IMMUTABLE,   "immutable bit");
    CHECK(hdr.obj_flags & STA_OBJ_ACTOR_LOCAL, "actor-local bit");
    CHECK(!(hdr.obj_flags & STA_OBJ_PINNED),   "pinned not set");
    CHECK((hdr.obj_flags & STA_OBJ_RESERVED) == 0, "no reserved obj bits set");

    /* ── Actor density ──────────────────────────────────── */
    printf("\n=== Actor density ===\n");
    size_t actor_struct  = sizeof(STA_Actor);
    size_t initial_slab  = 128;   /* minimum nursery slab at creation */
    size_t identity_obj  = STA_HEADER_ALIGNED + 0; /* zero-slot identity object */
    size_t total_creation = actor_struct + initial_slab + identity_obj;

    printf("  sizeof(STA_Actor)      = %zu bytes\n", actor_struct);
    printf("  initial nursery slab   = %zu bytes\n", initial_slab);
    printf("  actor identity object  = %zu bytes\n", identity_obj);
    printf("  ──────────────────────────────────\n");
    printf("  total creation cost    = %zu bytes (target: ~300)\n", total_creation);

    if (actor_struct > 128) {
        printf("  WARNING: STA_Actor struct exceeds 128-byte sub-budget.\n");
        printf("           Review fields — see ADR 007 rationale.\n");
    }
    if (total_creation > 320) {
        printf("  WARNING: total creation cost %zu > 320 bytes.\n", total_creation);
        printf("           Either justify the overage or reduce field count.\n");
    } else {
        printf("  OK: creation cost within ~300-byte target.\n");
    }

    /* ── Summary ──────────────────────────────────────── */
    printf("\n=== Spike complete ===\n");
    if (failures == 0) {
        printf("All checks passed. Record layout decisions in ADR 007.\n\n");
        printf("Key numbers for ADR 007:\n");
        printf("  ObjHeader:        %2zu bytes (padded to %u for alignment)\n",
               sizeof(STA_ObjHeader), STA_HEADER_ALIGNED);
        printf("  STA_Actor struct: %2zu bytes\n", actor_struct);
        printf("  Total per-actor:  %2zu bytes (struct + 128-byte slab + identity)\n",
               total_creation);
        return 0;
    } else {
        printf("%d check(s) FAILED. Fix before writing ADR 007.\n", failures);
        return 1;
    }
}
