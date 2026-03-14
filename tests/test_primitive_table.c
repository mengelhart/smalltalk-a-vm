/* tests/test_primitive_table.c
 * Tests for primitive table and kernel primitives.
 */
#include "vm/primitive_table.h"
#include "vm/special_objects.h"
#include "vm/immutable_space.h"
#include "vm/heap.h"
#include "vm/class_table.h"
#include <stdio.h>
#include <assert.h>
#include <limits.h>

/* Helper: set up special objects for boolean results. */
static STA_ImmutableSpace *g_sp;
static void setup_specials(void) {
    sta_special_objects_init();
    g_sp = sta_immutable_space_create(64 * 1024);

    /* Allocate nil, true, false objects. */
    STA_ObjHeader *nil_h = sta_immutable_alloc(g_sp, STA_CLS_UNDEFINEDOBJ, 0);
    STA_ObjHeader *true_h = sta_immutable_alloc(g_sp, STA_CLS_TRUE, 0);
    STA_ObjHeader *false_h = sta_immutable_alloc(g_sp, STA_CLS_FALSE, 0);

    sta_spc_set(SPC_NIL, (STA_OOP)(uintptr_t)nil_h);
    sta_spc_set(SPC_TRUE, (STA_OOP)(uintptr_t)true_h);
    sta_spc_set(SPC_FALSE, (STA_OOP)(uintptr_t)false_h);

    sta_primitive_table_init();
}

static void test_smallint_add(void) {
    printf("  SmallInt #+...");
    STA_OOP args[] = { STA_SMALLINT_OOP(3), STA_SMALLINT_OOP(4) };
    STA_OOP result;
    int rc = sta_primitives[1](args, 1, &result);
    assert(rc == 0);
    assert(result == STA_SMALLINT_OOP(7));

    /* Negative. */
    args[0] = STA_SMALLINT_OOP(-5);
    args[1] = STA_SMALLINT_OOP(3);
    rc = sta_primitives[1](args, 1, &result);
    assert(rc == 0);
    assert(STA_SMALLINT_VAL(result) == -2);

    /* Type mismatch. */
    args[0] = STA_CHAR_OOP('A');
    rc = sta_primitives[1](args, 1, &result);
    assert(rc == STA_PRIM_BAD_RECEIVER);

    printf(" ok\n");
}

static void test_smallint_sub(void) {
    printf("  SmallInt #-...");
    STA_OOP args[] = { STA_SMALLINT_OOP(10), STA_SMALLINT_OOP(3) };
    STA_OOP result;
    int rc = sta_primitives[2](args, 1, &result);
    assert(rc == 0);
    assert(result == STA_SMALLINT_OOP(7));
    printf(" ok\n");
}

static void test_smallint_mul(void) {
    printf("  SmallInt #*...");
    STA_OOP args[] = { STA_SMALLINT_OOP(6), STA_SMALLINT_OOP(7) };
    STA_OOP result;
    int rc = sta_primitives[9](args, 1, &result);
    assert(rc == 0);
    assert(result == STA_SMALLINT_OOP(42));
    printf(" ok\n");
}

static void test_smallint_lt(void) {
    printf("  SmallInt #<...");
    STA_OOP args[] = { STA_SMALLINT_OOP(3), STA_SMALLINT_OOP(5) };
    STA_OOP result;
    int rc = sta_primitives[3](args, 1, &result);
    assert(rc == 0);
    assert(result == sta_spc_get(SPC_TRUE));

    args[0] = STA_SMALLINT_OOP(5);
    args[1] = STA_SMALLINT_OOP(3);
    rc = sta_primitives[3](args, 1, &result);
    assert(rc == 0);
    assert(result == sta_spc_get(SPC_FALSE));
    printf(" ok\n");
}

static void test_smallint_gt(void) {
    printf("  SmallInt #>...");
    STA_OOP args[] = { STA_SMALLINT_OOP(5), STA_SMALLINT_OOP(3) };
    STA_OOP result;
    int rc = sta_primitives[4](args, 1, &result);
    assert(rc == 0);
    assert(result == sta_spc_get(SPC_TRUE));
    printf(" ok\n");
}

static void test_smallint_eq(void) {
    printf("  SmallInt #=...");
    STA_OOP args[] = { STA_SMALLINT_OOP(42), STA_SMALLINT_OOP(42) };
    STA_OOP result;
    int rc = sta_primitives[7](args, 1, &result);
    assert(rc == 0);
    assert(result == sta_spc_get(SPC_TRUE));

    args[1] = STA_SMALLINT_OOP(99);
    rc = sta_primitives[7](args, 1, &result);
    assert(rc == 0);
    assert(result == sta_spc_get(SPC_FALSE));
    printf(" ok\n");
}

static void test_identity(void) {
    printf("  Object #==...");
    STA_OOP args[] = { STA_SMALLINT_OOP(5), STA_SMALLINT_OOP(5) };
    STA_OOP result;
    int rc = sta_primitives[29](args, 1, &result);
    assert(rc == 0);
    assert(result == sta_spc_get(SPC_TRUE));

    args[1] = STA_SMALLINT_OOP(6);
    rc = sta_primitives[29](args, 1, &result);
    assert(rc == 0);
    assert(result == sta_spc_get(SPC_FALSE));
    printf(" ok\n");
}

static void test_array_at(void) {
    printf("  Array #at:...");
    STA_Heap *heap = sta_heap_create(8192);
    STA_ObjHeader *arr = sta_heap_alloc(heap, STA_CLS_ARRAY, 3);
    STA_OOP *payload = sta_payload(arr);
    payload[0] = STA_SMALLINT_OOP(10);
    payload[1] = STA_SMALLINT_OOP(20);
    payload[2] = STA_SMALLINT_OOP(30);

    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr;
    STA_OOP result;

    /* at: 1 → 10 */
    STA_OOP args_at[] = { arr_oop, STA_SMALLINT_OOP(1) };
    int rc = sta_primitives[51](args_at, 1, &result);
    assert(rc == 0);
    assert(result == STA_SMALLINT_OOP(10));

    /* at: 3 → 30 */
    args_at[1] = STA_SMALLINT_OOP(3);
    rc = sta_primitives[51](args_at, 1, &result);
    assert(rc == 0);
    assert(result == STA_SMALLINT_OOP(30));

    /* Bounds check: at: 0 → fail */
    args_at[1] = STA_SMALLINT_OOP(0);
    rc = sta_primitives[51](args_at, 1, &result);
    assert(rc == STA_PRIM_OUT_OF_RANGE);

    /* Bounds check: at: 4 → fail */
    args_at[1] = STA_SMALLINT_OOP(4);
    rc = sta_primitives[51](args_at, 1, &result);
    assert(rc == STA_PRIM_OUT_OF_RANGE);

    sta_heap_destroy(heap);
    printf(" ok\n");
}

static void test_array_at_put(void) {
    printf("  Array #at:put:...");
    STA_Heap *heap = sta_heap_create(8192);
    STA_ObjHeader *arr = sta_heap_alloc(heap, STA_CLS_ARRAY, 2);
    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr;
    STA_OOP result;

    STA_OOP args_atp[] = { arr_oop, STA_SMALLINT_OOP(1), STA_SMALLINT_OOP(99) };
    int rc = sta_primitives[52](args_atp, 2, &result);
    assert(rc == 0);
    assert(result == STA_SMALLINT_OOP(99));
    assert(sta_payload(arr)[0] == STA_SMALLINT_OOP(99));

    sta_heap_destroy(heap);
    printf(" ok\n");
}

static void test_array_size(void) {
    printf("  Array #size...");
    STA_Heap *heap = sta_heap_create(8192);
    STA_ObjHeader *arr = sta_heap_alloc(heap, STA_CLS_ARRAY, 5);
    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr;
    STA_OOP result;

    STA_OOP args_sz[] = { arr_oop };
    int rc = sta_primitives[53](args_sz, 0, &result);
    assert(rc == 0);
    assert(result == STA_SMALLINT_OOP(5));

    sta_heap_destroy(heap);
    printf(" ok\n");
}

int main(void) {
    printf("test_primitive_table:\n");
    setup_specials();

    test_smallint_add();
    test_smallint_sub();
    test_smallint_mul();
    test_smallint_lt();
    test_smallint_gt();
    test_smallint_eq();
    test_identity();
    test_array_at();
    test_array_at_put();
    test_array_size();

    sta_immutable_space_destroy(g_sp);
    printf("All primitive_table tests passed.\n");
    return 0;
}
