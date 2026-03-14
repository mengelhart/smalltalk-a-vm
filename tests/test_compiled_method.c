/* tests/test_compiled_method.c
 * Tests for CompiledMethod layout, header accessors, and builder.
 */
#include "vm/compiled_method.h"
#include "vm/class_table.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static void test_header_encoding_decoding(void) {
    printf("  header encoding/decoding...");

    STA_OOP h = STA_METHOD_HEADER(2, 5, 3, 42, 1, 0);
    assert(STA_IS_SMALLINT(h));
    assert(STA_METHOD_NUM_ARGS(h) == 2);
    assert(STA_METHOD_NUM_TEMPS(h) == 5);
    assert(STA_METHOD_NUM_LITERALS(h) == 3);
    assert(STA_METHOD_PRIM_INDEX(h) == 42);
    assert(STA_METHOD_HAS_PRIM(h) == 1);
    assert(STA_METHOD_LARGE_FRAME(h) == 0);

    /* Test with largeFrame = 1. */
    h = STA_METHOD_HEADER(0, 0, 1, 0, 0, 1);
    assert(STA_METHOD_NUM_ARGS(h) == 0);
    assert(STA_METHOD_NUM_TEMPS(h) == 0);
    assert(STA_METHOD_NUM_LITERALS(h) == 1);
    assert(STA_METHOD_PRIM_INDEX(h) == 0);
    assert(STA_METHOD_HAS_PRIM(h) == 0);
    assert(STA_METHOD_LARGE_FRAME(h) == 1);

    /* Max values. */
    h = STA_METHOD_HEADER(255, 255, 255, 255, 1, 1);
    assert(STA_METHOD_NUM_ARGS(h) == 255);
    assert(STA_METHOD_NUM_TEMPS(h) == 255);
    assert(STA_METHOD_NUM_LITERALS(h) == 255);
    assert(STA_METHOD_PRIM_INDEX(h) == 255);
    assert(STA_METHOD_HAS_PRIM(h) == 1);
    assert(STA_METHOD_LARGE_FRAME(h) == 1);

    printf(" ok\n");
}

static void test_create_and_access(void) {
    printf("  create and access...");

    STA_ImmutableSpace *sp = sta_immutable_space_create(64 * 1024);
    assert(sp);

    STA_OOP lits[] = { STA_SMALLINT_OOP(42), STA_SMALLINT_OOP(99) };
    uint8_t bc[] = { 0x02, 0x00, 0x30, 0x00 };  /* PUSH_RECEIVER, RETURN_TOP */

    STA_OOP method = sta_compiled_method_create(sp,
        1, 3, 0,
        lits, 2,
        bc, 4);
    assert(method != 0);

    /* Verify header. */
    STA_OOP h = sta_method_header(method);
    assert(STA_IS_SMALLINT(h));
    assert(STA_METHOD_NUM_ARGS(h) == 1);
    assert(STA_METHOD_NUM_TEMPS(h) == 3);
    assert(STA_METHOD_NUM_LITERALS(h) == 2);
    assert(STA_METHOD_PRIM_INDEX(h) == 0);
    assert(STA_METHOD_HAS_PRIM(h) == 0);

    /* Verify literals. */
    assert(sta_method_literal(method, 0) == STA_SMALLINT_OOP(42));
    assert(sta_method_literal(method, 1) == STA_SMALLINT_OOP(99));

    /* Verify bytecodes. */
    const uint8_t *bcs = sta_method_bytecodes(method);
    assert(bcs[0] == 0x02);
    assert(bcs[1] == 0x00);
    assert(bcs[2] == 0x30);
    assert(bcs[3] == 0x00);
    assert(sta_method_bytecode_count(method) == 8);  /* rounded to OOP words */

    /* Verify object header. */
    STA_ObjHeader *oh = (STA_ObjHeader *)(uintptr_t)method;
    assert(oh->class_index == STA_CLS_COMPILEDMETHOD);
    assert(oh->obj_flags & STA_OBJ_IMMUTABLE);

    sta_immutable_space_destroy(sp);
    printf(" ok\n");
}

static void test_primitive_method(void) {
    printf("  primitive method...");

    STA_ImmutableSpace *sp = sta_immutable_space_create(64 * 1024);
    assert(sp);

    STA_OOP lits[] = { STA_SMALLINT_OOP(0) };
    uint8_t bc[] = { 0x52, 0x01 };  /* OP_PRIMITIVE 1 */

    STA_OOP method = sta_compiled_method_create(sp,
        1, 1, 1,  /* numArgs=1, numTemps=1, primIndex=1 */
        lits, 1,
        bc, 2);
    assert(method != 0);

    STA_OOP h = sta_method_header(method);
    assert(STA_METHOD_HAS_PRIM(h) == 1);
    assert(STA_METHOD_PRIM_INDEX(h) == 1);

    sta_immutable_space_destroy(sp);
    printf(" ok\n");
}

int main(void) {
    printf("test_compiled_method:\n");
    test_header_encoding_decoding();
    test_create_and_access();
    test_primitive_method();
    printf("All compiled_method tests passed.\n");
    return 0;
}
