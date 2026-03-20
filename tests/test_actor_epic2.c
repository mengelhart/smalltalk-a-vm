/* tests/test_actor_epic2.c
 * Phase 2 Epic 2 Story 6: Comprehensive actor tests and density measurement.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include "actor/actor.h"
#include "vm/vm_state.h"
#include "vm/heap.h"
#include "vm/frame.h"
#include "vm/handler.h"
#include "vm/oop.h"
#include "vm/special_objects.h"
#include "vm/interpreter.h"
#include <sta/vm.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(name) do { \
    printf("  %-55s", #name); \
    name(); \
    tests_passed++; \
    printf("PASS\n"); \
    tests_run++; \
} while (0)

/* ── Actor lifecycle tests ───────────────────────────────────────────── */

static void test_create_state_created(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 2048);
    assert(a != NULL);
    assert(a->state == STA_ACTOR_CREATED);
    sta_actor_terminate(a);
}

static void test_heap_independent(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 512);
    struct STA_Actor *b = sta_actor_create(NULL, 4096, 512);
    assert(a && b);

    STA_ObjHeader *ha = sta_heap_alloc(&a->heap, 1, 2);
    assert(ha != NULL);
    assert(sta_heap_used(&b->heap) == 0);

    STA_ObjHeader *hb = sta_heap_alloc(&b->heap, 2, 3);
    assert(hb != NULL);

    /* Objects on different heaps. */
    uintptr_t a_lo = (uintptr_t)a->heap.base;
    uintptr_t a_hi = a_lo + a->heap.capacity;
    uintptr_t hb_addr = (uintptr_t)hb;
    assert(hb_addr < a_lo || hb_addr >= a_hi);

    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

static void test_handler_chain_per_actor(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 512);
    struct STA_Actor *b = sta_actor_create(NULL, 4096, 512);
    assert(a && b);

    /* Both start with NULL handler chains. */
    assert(a->handler_top == NULL);
    assert(b->handler_top == NULL);

    /* Push a handler on A. */
    STA_HandlerEntry entry;
    memset(&entry, 0, sizeof(entry));
    entry.prev = a->handler_top;
    a->handler_top = &entry;

    /* B's handler chain is unaffected. */
    assert(b->handler_top == NULL);
    assert(a->handler_top == &entry);

    a->handler_top = entry.prev;
    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

static void test_destroy_cleanup(void) {
    struct STA_Actor *a = sta_actor_create(NULL, 4096, 2048);
    assert(a != NULL);

    /* Allocate objects to fill heap. */
    for (int i = 0; i < 20; i++) {
        sta_heap_alloc(&a->heap, 1, 4);
    }

    /* Destroy should free all resources (ASan/LSan validates). */
    sta_actor_terminate(a);
}

/* ── Execution inside actors (via public API) ────────────────────────── */

static void test_eval_runs_inside_root_actor(void) {
    STA_VMConfig config = {0};
    STA_VM *vm = sta_vm_create(&config);
    assert(vm != NULL);
    assert(vm->root_actor != NULL);
    assert(vm->root_actor->state == STA_ACTOR_READY);
    assert(vm->root_actor->actor_id == 1);
    sta_vm_destroy(vm);
}

static void test_eval_arithmetic_in_actor(void) {
    STA_VMConfig config = {0};
    STA_VM *vm = sta_vm_create(&config);
    assert(vm != NULL);

    STA_Handle *h = sta_eval(vm, "3 + 4");
    assert(h != NULL);
    const char *s = sta_inspect_cstring(vm, h);
    assert(strcmp(s, "7") == 0);

    sta_vm_destroy(vm);
}

static void test_object_alloc_on_root_actor_heap(void) {
    STA_VMConfig config = {0};
    STA_VM *vm = sta_vm_create(&config);
    assert(vm != NULL);

    size_t before = sta_heap_used(&vm->root_actor->heap);

    /* basicNew allocates on the actor's heap. */
    STA_Handle *h = sta_eval(vm, "Object new");
    assert(h != NULL);

    size_t after = sta_heap_used(&vm->root_actor->heap);
    assert(after > before);

    /* VM heap is empty (transferred to root actor). */
    assert(vm->heap.base == NULL);

    sta_vm_destroy(vm);
}

static void test_exception_handling_in_actor(void) {
    STA_VMConfig config = {0};
    STA_VM *vm = sta_vm_create(&config);
    assert(vm != NULL);

    /* Signal an Error and catch it via on:do:. */
    STA_Handle *h = sta_eval(vm,
        "[Error new signal] on: Error do: [:e | 42]");
    assert(h != NULL);
    const char *s = sta_inspect_cstring(vm, h);
    assert(strcmp(s, "42") == 0);

    sta_vm_destroy(vm);
}

static void test_closures_in_actor(void) {
    STA_VMConfig config = {0};
    STA_VM *vm = sta_vm_create(&config);
    assert(vm != NULL);

    STA_Handle *h = sta_eval(vm, "| x | x := 10. [x + 5] value");
    assert(h != NULL);
    const char *s = sta_inspect_cstring(vm, h);
    assert(strcmp(s, "15") == 0);

    sta_vm_destroy(vm);
}

static void test_printstring_in_actor(void) {
    STA_VMConfig config = {0};
    STA_VM *vm = sta_vm_create(&config);
    assert(vm != NULL);

    STA_Handle *h = sta_eval(vm, "42 printString");
    assert(h != NULL);
    const char *s = sta_inspect_cstring(vm, h);
    assert(strcmp(s, "'42'") == 0);

    sta_vm_destroy(vm);
}

static void test_image_roundtrip_with_actor(void) {
    const char *img_path = "/tmp/test_epic2_image.sta";

    /* Save. */
    {
        STA_VMConfig config = { .image_path = img_path };
        STA_VM *vm = sta_vm_create(&config);
        assert(vm != NULL);
        int rc = sta_vm_save_image(vm, img_path);
        assert(rc == STA_OK);
        sta_vm_destroy(vm);
    }

    /* Load and eval. */
    {
        STA_VMConfig config = { .image_path = img_path };
        STA_VM *vm = sta_vm_create(&config);
        assert(vm != NULL);
        assert(vm->root_actor != NULL);

        STA_Handle *h = sta_eval(vm, "3 + 4");
        assert(h != NULL);
        const char *s = sta_inspect_cstring(vm, h);
        assert(strcmp(s, "7") == 0);

        sta_vm_destroy(vm);
    }

    remove(img_path);
}

/* ── Density measurement (informational) ─────────────────────────────── */

static void test_density_measurement(void) {
    printf("\n");
    printf("    ── Actor density measurement ──\n");
    printf("    sizeof(STA_Actor)      = %3zu bytes\n", sizeof(struct STA_Actor));
    printf("    sizeof(STA_Heap)       = %3zu bytes\n", sizeof(STA_Heap));
    printf("    sizeof(STA_StackSlab)  = %3zu bytes\n", sizeof(STA_StackSlab));
    printf("    sizeof(STA_ObjHeader)  = %3zu bytes\n", sizeof(STA_ObjHeader));

    /* Simulate creation cost: struct + heap slab + stack slab + identity object. */
    size_t heap_slab = 128;   /* minimum viable nursery */
    size_t stack_slab = 512;  /* initial stack segment per ADR 014 */
    size_t identity_obj = 16; /* 0-slot ObjHeader */
    size_t creation_cost = sizeof(struct STA_Actor) + heap_slab + stack_slab + identity_obj;

    printf("    ── Creation cost breakdown ──\n");
    printf("    STA_Actor struct       = %3zu bytes\n", sizeof(struct STA_Actor));
    printf("    Nursery slab (min)     = %3zu bytes\n", heap_slab);
    printf("    Stack slab (initial)   = %3zu bytes\n", stack_slab);
    printf("    Identity object        = %3zu bytes\n", identity_obj);
    printf("    Total creation cost    = %3zu bytes\n", creation_cost);
    printf("    ── Spike targets (reference) ──\n");
    printf("    ADR 014 struct target  = 164 bytes\n");
    printf("    ADR 014 creation       = 308 bytes (spawn)\n");
    printf("    ADR 014 execution      = 820 bytes (spawn + initial stack)\n");
    printf("    BEAM comparison        = 2704 bytes\n");

    size_t execution_cost = creation_cost;
    float beam_ratio = 2704.0f / (float)execution_cost;
    printf("    Smalltalk/A execution  = %3zu bytes (%.1fx more compact than BEAM)\n",
           execution_cost, beam_ratio);
    printf("    ");
}

/* ── Main ────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_actor_epic2 (Phase 2 Epic 2 Story 6)\n");

    printf("\n  -- Actor lifecycle --\n");
    RUN(test_create_state_created);
    RUN(test_heap_independent);
    RUN(test_handler_chain_per_actor);
    RUN(test_destroy_cleanup);

    printf("\n  -- Execution in actors --\n");
    RUN(test_eval_runs_inside_root_actor);
    RUN(test_eval_arithmetic_in_actor);
    RUN(test_object_alloc_on_root_actor_heap);
    RUN(test_exception_handling_in_actor);
    RUN(test_closures_in_actor);
    RUN(test_printstring_in_actor);
    RUN(test_image_roundtrip_with_actor);

    printf("\n  -- Density --\n");
    RUN(test_density_measurement);

    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
