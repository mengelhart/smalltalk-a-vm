/* tests/test_immutable_space.c
 * Phase 1: Shared immutable region allocator tests.
 * Story 2 of Epic 1 — Object Memory and Allocator.
 */
#include <stdio.h>
#include <stdint.h>
#include "vm/oop.h"
#include "vm/immutable_space.h"

#define CHECK(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("  OK: %s\n", msg); } } while(0)

int main(void) {
    int failures = 0;

    /* ── Create and basic allocation ───────────────────── */
    printf("\n=== Immutable space: create and alloc ===\n");
    STA_ImmutableSpace *sp = sta_immutable_space_create(4096);
    CHECK(sp != NULL, "create succeeds");
    CHECK(sta_immutable_space_base(sp) != NULL, "base is non-null");
    CHECK(sta_immutable_space_used(sp) == 0,    "initial used == 0");
    CHECK(!sta_immutable_space_is_sealed(sp),    "not sealed initially");

    /* Allocate a zero-slot object (like nil). */
    STA_ObjHeader *nil_obj = sta_immutable_alloc(sp, 3, 0); /* class 3 = UndefinedObject */
    CHECK(nil_obj != NULL,                  "alloc nil succeeds");
    CHECK(nil_obj->class_index == 3,        "nil class_index == 3");
    CHECK(nil_obj->size == 0,               "nil size == 0");
    CHECK(nil_obj->gc_flags == STA_GC_WHITE,"nil gc_flags == WHITE");
    CHECK((nil_obj->obj_flags & STA_OBJ_IMMUTABLE) != 0,  "nil IMMUTABLE set");
    CHECK((nil_obj->obj_flags & STA_OBJ_SHARED_IMM) != 0, "nil SHARED_IMM set");
    CHECK(nil_obj->reserved == 0,           "nil reserved == 0");

    /* Allocate an object with payload (like a symbol with 1 word). */
    STA_ObjHeader *sym = sta_immutable_alloc(sp, 7, 1); /* class 7 = Symbol */
    CHECK(sym != NULL,                      "alloc symbol succeeds");
    CHECK(sym->class_index == 7,            "symbol class_index == 7");
    CHECK(sym->size == 1,                   "symbol size == 1");
    CHECK((sym->obj_flags & STA_OBJ_IMMUTABLE) != 0,  "symbol IMMUTABLE set");
    CHECK((sym->obj_flags & STA_OBJ_SHARED_IMM) != 0, "symbol SHARED_IMM set");

    /* Payload should be zeroed. */
    STA_OOP *sym_payload = sta_payload(sym);
    CHECK(*sym_payload == 0, "symbol payload word zeroed");

    /* ── Contiguity ──────────────────────────────────────── */
    printf("\n=== Contiguity ===\n");
    const char *base = (const char *)sta_immutable_space_base(sp);
    CHECK((const char *)nil_obj >= base, "nil_obj >= base");
    CHECK((const char *)sym > (const char *)nil_obj, "sym after nil_obj");
    CHECK((const char *)sym < base + sta_immutable_space_used(sp),
          "sym within used region");

    /* ── 16-byte alignment ───────────────────────────────── */
    printf("\n=== Alignment ===\n");
    CHECK(((uintptr_t)nil_obj % 16) == 0, "nil_obj 16-byte aligned");
    CHECK(((uintptr_t)sym % 16) == 0,     "sym 16-byte aligned");

    /* Allocate a few more and check alignment. */
    STA_ObjHeader *a = sta_immutable_alloc(sp, 9, 3);
    STA_ObjHeader *b = sta_immutable_alloc(sp, 9, 0);
    CHECK(a != NULL && ((uintptr_t)a % 16) == 0, "3-slot obj 16-byte aligned");
    CHECK(b != NULL && ((uintptr_t)b % 16) == 0, "0-slot obj 16-byte aligned");

    /* ── Seal ────────────────────────────────────────────── */
    printf("\n=== Seal ===\n");
    int seal_result = sta_immutable_space_seal(sp);
    CHECK(seal_result == 0,                    "seal returns 0 (success)");
    CHECK(sta_immutable_space_is_sealed(sp),   "is_sealed == true");

    /* Allocation must fail after seal. */
    STA_ObjHeader *after_seal = sta_immutable_alloc(sp, 9, 0);
    CHECK(after_seal == NULL, "alloc after seal returns NULL");

    /* Sealed region is still readable. */
    CHECK(nil_obj->class_index == 3, "can still read nil class_index after seal");
    CHECK(sym->class_index == 7,     "can still read sym class_index after seal");

    /* Double-seal is harmless. */
    CHECK(sta_immutable_space_seal(sp) == 0, "double-seal returns 0");

    /* ── Cleanup ─────────────────────────────────────────── */
    sta_immutable_space_destroy(sp);

    /* ── Capacity overflow ───────────────────────────────── */
    printf("\n=== Capacity overflow ===\n");
    STA_ImmutableSpace *tiny = sta_immutable_space_create(64);
    CHECK(tiny != NULL, "create tiny space");
    /* Fill up — allocate 0-slot objects until it returns NULL. */
    int count = 0;
    while (sta_immutable_alloc(tiny, 9, 0) != NULL) { count++; }
    CHECK(count > 0, "allocated at least one object before full");
    /* One more 0-slot should also fail. */
    CHECK(sta_immutable_alloc(tiny, 9, 0) == NULL, "alloc fails when full");
    sta_immutable_space_destroy(tiny);

    /* ── Summary ─────────────────────────────────────────── */
    printf("\n");
    if (failures == 0) { printf("All checks passed.\n"); return 0; }
    printf("%d check(s) FAILED.\n", failures);
    return 1;
}
