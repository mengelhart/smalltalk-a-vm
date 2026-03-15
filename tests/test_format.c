/* tests/test_format.c
 * Table-driven tests for class format field decoding (Phase 1, Epic 5.1).
 * Verifies that the format decoder correctly extracts instSize, isIndexable,
 * isBytes, and isInstantiable for all 32 kernel classes.
 */
#include "vm/format.h"
#include "vm/class_table.h"
#include "vm/heap.h"
#include "vm/immutable_space.h"
#include "vm/symbol_table.h"
#include "vm/interpreter.h"
#include "bootstrap/bootstrap.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

/* ── Shared test infrastructure ────────────────────────────────────────── */

static STA_ImmutableSpace *g_sp;
static STA_SymbolTable    *g_st;
static STA_Heap           *g_heap;
static STA_ClassTable     *g_ct;

static void setup(void) {
    g_sp   = sta_immutable_space_create(512 * 1024);
    g_st   = sta_symbol_table_create(512);
    g_heap = sta_heap_create(512 * 1024);
    g_ct   = sta_class_table_create();

    STA_BootstrapResult r = sta_bootstrap(g_heap, g_sp, g_st, g_ct);
    assert(r.status == 0);
}

static void teardown(void) {
    sta_class_table_destroy(g_ct);
    sta_heap_destroy(g_heap);
    sta_symbol_table_destroy(g_st);
    sta_immutable_space_destroy(g_sp);
}

/* ── Expected format data for each kernel class ────────────────────────── */

typedef struct {
    uint32_t    class_index;
    const char *name;
    uint8_t     inst_vars;
    bool        indexable;
    bool        bytes;
    bool        instantiable;
} FormatExpectation;

static const FormatExpectation expectations[] = {
    /* Tier 0 */
    { STA_CLS_OBJECT,              "Object",                 0, false, false, true  },
    { STA_CLS_BEHAVIOR,            "Behavior",               0, false, false, true  },
    { STA_CLS_CLASSDESCRIPTION,    "ClassDescription",       0, false, false, true  },
    { STA_CLS_CLASS,               "Class",                  4, false, false, true  },
    { STA_CLS_METACLASS,           "Metaclass",              4, false, false, true  },

    /* Magnitude hierarchy */
    { STA_CLS_MAGNITUDE,           "Magnitude",              0, false, false, true  },
    { STA_CLS_NUMBER,              "Number",                 0, false, false, true  },
    { STA_CLS_SMALLINTEGER,        "SmallInteger",           0, false, false, false },
    { STA_CLS_FLOAT,               "Float",                  0, false, false, false },

    /* Character */
    { STA_CLS_CHARACTER,           "Character",              0, false, false, false },

    /* Collection hierarchy */
    { STA_CLS_COLLECTION,          "Collection",             0, false, false, true  },
    { STA_CLS_SEQCOLLECTION,       "SequenceableCollection", 0, false, false, true  },
    { STA_CLS_ARRAYEDCOLLECTION,   "ArrayedCollection",      0, true,  false, true  },
    { STA_CLS_ARRAY,               "Array",                  0, true,  false, true  },
    { STA_CLS_STRING,              "String",                 0, true,  true,  true  },
    { STA_CLS_SYMBOL,              "Symbol",                 0, true,  true,  true  },
    { STA_CLS_BYTEARRAY,           "ByteArray",              0, true,  true,  true  },

    /* Singleton classes */
    { STA_CLS_UNDEFINEDOBJ,        "UndefinedObject",        0, false, false, true  },
    { STA_CLS_TRUE,                "True",                   0, false, false, true  },
    { STA_CLS_FALSE,               "False",                  0, false, false, true  },

    /* Internal classes */
    { STA_CLS_ASSOCIATION,         "Association",            2, false, false, true  },
    { STA_CLS_COMPILEDMETHOD,      "CompiledMethod",         0, false, false, false },
    { STA_CLS_METHODDICTIONARY,    "MethodDictionary",       2, false, false, true  },
    { STA_CLS_BLOCKCLOSURE,        "BlockClosure",           4, false, false, true  },
    { STA_CLS_BLOCKDESCRIPTOR,     "BlockDescriptor",        3, false, false, true  },
    { STA_CLS_MESSAGE,             "Message",                3, false, false, true  },

    /* Exception hierarchy */
    { STA_CLS_EXCEPTION,           "Exception",              2, false, false, true  },
    { STA_CLS_ERROR,               "Error",                  0, false, false, true  },
    { STA_CLS_MESSAGENOTUNDERSTOOD,"MessageNotUnderstood",   2, false, false, true  },
    { STA_CLS_BLOCKCANNOTRETURN,   "BlockCannotReturn",      2, false, false, true  },

    /* SystemDictionary */
    { STA_CLS_SYSTEMDICTIONARY,    "SystemDictionary",       2, false, false, true  },
};

#define NUM_EXPECTATIONS (sizeof(expectations) / sizeof(expectations[0]))

/* ── Test 1: Decode instVarCount for all kernel classes ────────────────── */

static void test_inst_vars(void) {
    printf("  inst var counts...");
    for (size_t i = 0; i < NUM_EXPECTATIONS; i++) {
        const FormatExpectation *e = &expectations[i];
        STA_OOP cls = sta_class_table_get(g_ct, e->class_index);
        assert(cls != 0);

        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)cls;
        STA_OOP fmt = sta_payload(h)[STA_CLASS_SLOT_FORMAT];
        uint8_t inst_vars = STA_FORMAT_INST_VARS(fmt);

        if (inst_vars != e->inst_vars) {
            printf(" FAIL: %s expected instVars=%u, got %u\n",
                   e->name, e->inst_vars, inst_vars);
            assert(0);
        }
    }
    printf(" ok (%zu classes)\n", NUM_EXPECTATIONS);
}

/* ── Test 2: isIndexable for all kernel classes ────────────────────────── */

static void test_is_indexable(void) {
    printf("  isIndexable...");
    for (size_t i = 0; i < NUM_EXPECTATIONS; i++) {
        const FormatExpectation *e = &expectations[i];
        STA_OOP cls = sta_class_table_get(g_ct, e->class_index);
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)cls;
        STA_OOP fmt = sta_payload(h)[STA_CLASS_SLOT_FORMAT];
        bool indexable = sta_format_is_indexable(fmt);

        if (indexable != e->indexable) {
            printf(" FAIL: %s expected indexable=%d, got %d\n",
                   e->name, e->indexable, indexable);
            assert(0);
        }
    }
    printf(" ok\n");
}

/* ── Test 3: isBytes for all kernel classes ────────────────────────────── */

static void test_is_bytes(void) {
    printf("  isBytes...");
    for (size_t i = 0; i < NUM_EXPECTATIONS; i++) {
        const FormatExpectation *e = &expectations[i];
        STA_OOP cls = sta_class_table_get(g_ct, e->class_index);
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)cls;
        STA_OOP fmt = sta_payload(h)[STA_CLASS_SLOT_FORMAT];
        bool bytes = sta_format_is_bytes(fmt);

        if (bytes != e->bytes) {
            printf(" FAIL: %s expected bytes=%d, got %d\n",
                   e->name, e->bytes, bytes);
            assert(0);
        }
    }
    printf(" ok\n");
}

/* ── Test 4: isInstantiable for all kernel classes ─────────────────────── */

static void test_is_instantiable(void) {
    printf("  isInstantiable...");
    for (size_t i = 0; i < NUM_EXPECTATIONS; i++) {
        const FormatExpectation *e = &expectations[i];
        STA_OOP cls = sta_class_table_get(g_ct, e->class_index);
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)cls;
        STA_OOP fmt = sta_payload(h)[STA_CLASS_SLOT_FORMAT];
        bool instantiable = sta_format_is_instantiable(fmt);

        if (instantiable != e->instantiable) {
            printf(" FAIL: %s expected instantiable=%d, got %d\n",
                   e->name, e->instantiable, instantiable);
            assert(0);
        }
    }
    printf(" ok\n");
}

/* ── Test 5: Encode/decode round-trip ──────────────────────────────────── */

static void test_encode_decode_roundtrip(void) {
    printf("  encode/decode round-trip...");

    /* Test all valid combinations. */
    for (uint32_t fmtType = 0; fmtType <= 6; fmtType++) {
        for (uint32_t iv = 0; iv <= 255; iv++) {
            STA_OOP fmt = STA_FORMAT_ENCODE(iv, fmtType);
            assert(STA_IS_SMALLINT(fmt));
            assert(STA_FORMAT_INST_VARS(fmt) == iv);
            assert(STA_FORMAT_TYPE(fmt) == fmtType);
        }
    }
    printf(" ok (7 types x 256 instVars)\n");
}

/* ── Test 6: Query helpers on raw format values ────────────────────────── */

static void test_query_helpers_raw(void) {
    printf("  query helpers on raw formats...");

    /* NORMAL: not indexable, not bytes, instantiable. */
    STA_OOP f_normal = STA_FORMAT_ENCODE(3, STA_FMT_NORMAL);
    assert(!sta_format_is_indexable(f_normal));
    assert(!sta_format_is_bytes(f_normal));
    assert(sta_format_is_instantiable(f_normal));

    /* VARIABLE_OOP: indexable, not bytes, instantiable. */
    STA_OOP f_var_oop = STA_FORMAT_ENCODE(0, STA_FMT_VARIABLE_OOP);
    assert(sta_format_is_indexable(f_var_oop));
    assert(!sta_format_is_bytes(f_var_oop));
    assert(sta_format_is_instantiable(f_var_oop));

    /* VARIABLE_BYTE: indexable, bytes, instantiable. */
    STA_OOP f_var_byte = STA_FORMAT_ENCODE(0, STA_FMT_VARIABLE_BYTE);
    assert(sta_format_is_indexable(f_var_byte));
    assert(sta_format_is_bytes(f_var_byte));
    assert(sta_format_is_instantiable(f_var_byte));

    /* VARIABLE_WORD: indexable, not bytes, instantiable. */
    STA_OOP f_var_word = STA_FORMAT_ENCODE(0, STA_FMT_VARIABLE_WORD);
    assert(sta_format_is_indexable(f_var_word));
    assert(!sta_format_is_bytes(f_var_word));
    assert(sta_format_is_instantiable(f_var_word));

    /* WEAK: indexable, not bytes, instantiable. */
    STA_OOP f_weak = STA_FORMAT_ENCODE(0, STA_FMT_WEAK);
    assert(sta_format_is_indexable(f_weak));
    assert(!sta_format_is_bytes(f_weak));
    assert(sta_format_is_instantiable(f_weak));

    /* IMMEDIATE: not indexable, not bytes, NOT instantiable. */
    STA_OOP f_imm = STA_FORMAT_ENCODE(0, STA_FMT_IMMEDIATE);
    assert(!sta_format_is_indexable(f_imm));
    assert(!sta_format_is_bytes(f_imm));
    assert(!sta_format_is_instantiable(f_imm));

    /* COMPILED_METHOD: not indexable, not bytes, NOT instantiable. */
    STA_OOP f_cm = STA_FORMAT_ENCODE(0, STA_FMT_COMPILED_METHOD);
    assert(!sta_format_is_indexable(f_cm));
    assert(!sta_format_is_bytes(f_cm));
    assert(!sta_format_is_instantiable(f_cm));

    printf(" ok\n");
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_format:\n");

    /* Tests 5-6 don't need bootstrap. */
    test_encode_decode_roundtrip();
    test_query_helpers_raw();

    /* Tests 1-4 need bootstrap for real class objects. */
    setup();
    test_inst_vars();
    test_is_indexable();
    test_is_bytes();
    test_is_instantiable();
    teardown();

    printf("  all format tests passed.\n");
    return 0;
}
