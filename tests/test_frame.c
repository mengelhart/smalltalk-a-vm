/* tests/test_frame.c
 * Tests for frame and stack slab operations.
 */
#include "vm/frame.h"
#include "vm/compiled_method.h"
#include "vm/special_objects.h"
#include "vm/class_table.h"
#include <stdio.h>
#include <assert.h>

/* Helper: create a simple method with given args/temps and a RETURN_SELF. */
static STA_OOP make_method(STA_ImmutableSpace *sp,
                            uint8_t nargs, uint8_t ntemps) {
    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };  /* owner class placeholder */
    uint8_t bc[] = { 0x31, 0x00 };  /* RETURN_SELF */
    return sta_compiled_method_create(sp, nargs, ntemps, 0,
                                       lits, 1, bc, 2);
}

static void test_slab_create_destroy(void) {
    printf("  slab create/destroy...");
    STA_StackSlab *slab = sta_stack_slab_create(4096);
    assert(slab);
    assert(slab->base != NULL);
    assert(slab->top == slab->base);
    sta_stack_slab_destroy(slab);
    printf(" ok\n");
}

static void test_push_pop_frame(void) {
    printf("  push/pop frame...");

    sta_special_objects_init();
    STA_ImmutableSpace *sp = sta_immutable_space_create(64 * 1024);
    STA_StackSlab *slab = sta_stack_slab_create(8192);

    STA_OOP method = make_method(sp, 2, 4);  /* 2 args, 4 temps total */
    STA_OOP args[] = { STA_SMALLINT_OOP(10), STA_SMALLINT_OOP(20) };

    STA_Frame *f = sta_frame_push(slab, method, STA_SMALLINT_OOP(99),
                                    NULL, args, 2);
    assert(f);
    assert(f->arg_count == 2);
    assert(f->local_count == 2);  /* 4 temps - 2 args */
    assert(f->pc == 0);
    assert(f->receiver == STA_SMALLINT_OOP(99));
    assert(f->sender == NULL);

    STA_OOP *s = sta_frame_slots(f);
    assert(s[0] == STA_SMALLINT_OOP(10));
    assert(s[1] == STA_SMALLINT_OOP(20));
    assert(s[2] == 0);  /* nil = 0 */
    assert(s[3] == 0);

    sta_frame_pop(slab, f);
    assert(slab->top == slab->base);

    sta_stack_slab_destroy(slab);
    sta_immutable_space_destroy(sp);
    printf(" ok\n");
}

static void test_stack_ops(void) {
    printf("  stack push/pop/peek/depth...");

    sta_special_objects_init();
    STA_ImmutableSpace *sp = sta_immutable_space_create(64 * 1024);
    STA_StackSlab *slab = sta_stack_slab_create(8192);

    STA_OOP method = make_method(sp, 0, 0);
    STA_Frame *f = sta_frame_push(slab, method, STA_SMALLINT_OOP(1),
                                    NULL, NULL, 0);
    assert(f);

    assert(sta_stack_depth(slab, f) == 0);

    sta_stack_push(slab, STA_SMALLINT_OOP(42));
    assert(sta_stack_depth(slab, f) == 1);
    assert(sta_stack_peek(slab) == STA_SMALLINT_OOP(42));

    sta_stack_push(slab, STA_SMALLINT_OOP(99));
    assert(sta_stack_depth(slab, f) == 2);

    STA_OOP v = sta_stack_pop(slab);
    assert(v == STA_SMALLINT_OOP(99));
    assert(sta_stack_depth(slab, f) == 1);

    v = sta_stack_pop(slab);
    assert(v == STA_SMALLINT_OOP(42));
    assert(sta_stack_depth(slab, f) == 0);

    sta_stack_slab_destroy(slab);
    sta_immutable_space_destroy(sp);
    printf(" ok\n");
}

static int gc_root_count;
static void count_visitor(STA_OOP *slot, void *ctx) {
    (void)ctx;
    (void)slot;
    gc_root_count++;
}

static void test_gc_roots(void) {
    printf("  GC root enumeration...");

    sta_special_objects_init();
    STA_ImmutableSpace *sp = sta_immutable_space_create(64 * 1024);
    STA_StackSlab *slab = sta_stack_slab_create(8192);

    /* Frame with 1 arg + 1 temp = 2 slots + method + receiver = 4 roots. */
    STA_OOP method = make_method(sp, 1, 2);
    STA_OOP args[] = { STA_SMALLINT_OOP(5) };
    STA_Frame *f = sta_frame_push(slab, method, STA_SMALLINT_OOP(1),
                                    NULL, args, 1);
    assert(f);

    gc_root_count = 0;
    sta_frame_gc_roots(f, slab, count_visitor, NULL);
    /* method + receiver + 1 arg + 1 temp = 4 */
    assert(gc_root_count == 4);

    sta_stack_slab_destroy(slab);
    sta_immutable_space_destroy(sp);
    printf(" ok\n");
}

int main(void) {
    printf("test_frame:\n");
    test_slab_create_destroy();
    test_push_pop_frame();
    test_stack_ops();
    test_gc_roots();
    printf("All frame tests passed.\n");
    return 0;
}
