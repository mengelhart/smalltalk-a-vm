/* tests/test_selector.c
 * Tests for selector arity helper.
 */
#include "vm/selector.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

static STA_OOP intern(STA_ImmutableSpace *sp, STA_SymbolTable *st,
                       const char *name) {
    return sta_symbol_intern(sp, st, name, strlen(name));
}

static void test_arity(void) {
    printf("  selector arity...");

    STA_ImmutableSpace *sp = sta_immutable_space_create(64 * 1024);
    STA_SymbolTable *st = sta_symbol_table_create(64);

    /* Binary selectors. */
    assert(sta_selector_arity(intern(sp, st, "+")) == 1);
    assert(sta_selector_arity(intern(sp, st, "==")) == 1);
    assert(sta_selector_arity(intern(sp, st, "<")) == 1);
    assert(sta_selector_arity(intern(sp, st, "~=")) == 1);

    /* Unary selectors. */
    assert(sta_selector_arity(intern(sp, st, "size")) == 0);
    assert(sta_selector_arity(intern(sp, st, "yourself")) == 0);

    /* Keyword selectors. */
    assert(sta_selector_arity(intern(sp, st, "at:put:")) == 2);
    assert(sta_selector_arity(intern(sp, st, "doesNotUnderstand:")) == 1);
    assert(sta_selector_arity(intern(sp, st, "inject:into:")) == 2);

    sta_symbol_table_destroy(st);
    sta_immutable_space_destroy(sp);
    printf(" ok\n");
}

int main(void) {
    printf("test_selector:\n");
    test_arity();
    printf("All selector tests passed.\n");
    return 0;
}
