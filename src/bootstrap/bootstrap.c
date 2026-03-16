/* src/bootstrap/bootstrap.c
 * Kernel bootstrap — see bootstrap.h for documentation.
 *
 * Implements the full bootstrap sequence:
 *   Step 1: Allocate nil, true, false
 *   Step 2: Intern kernel symbols
 *   Step 3: Create Tier 0 classes (metaclass circularity)
 *   Step 4: Create Tier 1 classes
 *   Step 5: Create character table
 *   Step 6: Create global dictionary
 *   Step 7: Install kernel primitive methods
 */
#include "bootstrap.h"
#include "../vm/interpreter.h"
#include "../vm/compiled_method.h"
#include "../vm/method_dict.h"
#include "../vm/special_objects.h"
#include "../vm/special_selectors.h"
#include "../vm/primitive_table.h"
#include "../compiler/compiler.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

/* ── Bootstrap state ──────────────────────────────────────────────────── */

#define BS_MAX_CLASSES 64

typedef struct {
    STA_Heap           *heap;
    STA_ImmutableSpace *sp;
    STA_SymbolTable    *st;
    STA_ClassTable     *ct;
    uint32_t            next_meta_index;  /* next free metaclass index   */

    STA_OOP nil_oop;
    STA_OOP true_oop;
    STA_OOP false_oop;

    /* Tier 0 instance-side classes. */
    STA_OOP cls_object;
    STA_OOP cls_behavior;
    STA_OOP cls_class_desc;
    STA_OOP cls_class;
    STA_OOP cls_metaclass;

    /* Tier 0 metaclasses. */
    STA_OOP meta_object;
    STA_OOP meta_behavior;
    STA_OOP meta_class_desc;
    STA_OOP meta_class;
    STA_OOP meta_metaclass;

    /* All classes for dictionary population. */
    STA_OOP     classes[BS_MAX_CLASSES];
    const char *class_names[BS_MAX_CLASSES];
    uint32_t    num_classes;
} BS;

/* ── Helpers ──────────────────────────────────────────────────────────── */

static STA_OOP bs_intern(BS *bs, const char *s) {
    return sta_symbol_intern(bs->sp, bs->st, s, strlen(s));
}

/* Track a class for later global dictionary population. */
static void bs_track(BS *bs, const char *name, STA_OOP cls) {
    if (bs->num_classes < BS_MAX_CLASSES) {
        bs->classes[bs->num_classes] = cls;
        bs->class_names[bs->num_classes] = name;
        bs->num_classes++;
    }
}

/* Set the four standard class slots. */
static void set_class_slots(STA_OOP cls, STA_OOP superclass,
                             STA_OOP method_dict, STA_OOP format,
                             STA_OOP name) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)cls;
    STA_OOP *s = sta_payload(h);
    s[STA_CLASS_SLOT_SUPERCLASS] = superclass;
    s[STA_CLASS_SLOT_METHODDICT] = method_dict;
    s[STA_CLASS_SLOT_FORMAT]     = format;
    s[STA_CLASS_SLOT_NAME]       = name;
}

/* ── Step 1: Allocate nil, true, false ─────────────────────────────────── */

static int step1_singletons(BS *bs) {
    STA_ObjHeader *nil_h  = sta_immutable_alloc(bs->sp, STA_CLS_UNDEFINEDOBJ, 0);
    STA_ObjHeader *true_h = sta_immutable_alloc(bs->sp, STA_CLS_TRUE, 0);
    STA_ObjHeader *false_h= sta_immutable_alloc(bs->sp, STA_CLS_FALSE, 0);
    if (!nil_h || !true_h || !false_h) return -1;

    bs->nil_oop  = (STA_OOP)(uintptr_t)nil_h;
    bs->true_oop = (STA_OOP)(uintptr_t)true_h;
    bs->false_oop= (STA_OOP)(uintptr_t)false_h;

    sta_spc_set(SPC_NIL,   bs->nil_oop);
    sta_spc_set(SPC_TRUE,  bs->true_oop);
    sta_spc_set(SPC_FALSE, bs->false_oop);

    /* Initialize remaining SPC entries 3–31 to nil. */
    for (uint32_t i = 3; i < STA_SPECIAL_OBJECTS_COUNT; i++) {
        sta_spc_set(i, bs->nil_oop);
    }

    return 0;
}

/* ── Step 2: Intern kernel symbols ────────────────────────────────────── */

static int step2_symbols(BS *bs) {
    /* Special selectors (populates SPC_SPECIAL_SELECTORS etc.). */
    if (sta_intern_special_selectors(bs->sp, bs->st) != 0) return -1;

    /* Additional selectors for boolean methods. */
    static const char *extra_selectors[] = {
        "ifTrue:", "ifFalse:", "ifTrue:ifFalse:",
        "yourself", "respondsTo:",
    };
    for (size_t i = 0; i < sizeof(extra_selectors) / sizeof(extra_selectors[0]); i++) {
        if (bs_intern(bs, extra_selectors[i]) == 0) return -1;
    }

    /* Class name symbols. */
    static const char *class_names[] = {
        "Object", "Behavior", "ClassDescription", "Class", "Metaclass",
        "UndefinedObject", "True", "False",
        "SmallInteger", "Number", "Magnitude", "Float", "Character",
        "Symbol", "String", "Array", "ByteArray",
        "ArrayedCollection", "SequenceableCollection", "Collection",
        "CompiledMethod", "BlockClosure", "Association", "MethodDictionary",
        "BlockDescriptor", "Message", "MessageNotUnderstood",
        "BlockCannotReturn", "Error", "Exception", "SystemDictionary",
    };
    for (size_t i = 0; i < sizeof(class_names) / sizeof(class_names[0]); i++) {
        if (bs_intern(bs, class_names[i]) == 0) return -1;
    }

    return 0;
}

/* ── Step 3: Create Tier 0 classes (metaclass circularity) ────────────── */

static int step3_tier0(BS *bs) {
    STA_OOP nil = bs->nil_oop;

    /* Metaclass indices for the 5 Tier 0 classes. */
    uint32_t meta_obj_idx = 32;
    uint32_t meta_beh_idx = 33;
    uint32_t meta_cd_idx  = 34;
    uint32_t meta_cls_idx = 35;
    uint32_t meta_mc_idx  = 36;
    bs->next_meta_index = 37;

    /* ── Pass 1: Allocate all 10 objects ──────────────────────────────── */

    /* Instance-side classes: ObjHeader class_index → their metaclass index. */
    STA_ObjHeader *obj_h = sta_heap_alloc(bs->heap, meta_obj_idx, 4);
    STA_ObjHeader *beh_h = sta_heap_alloc(bs->heap, meta_beh_idx, 4);
    STA_ObjHeader *cd_h  = sta_heap_alloc(bs->heap, meta_cd_idx,  4);
    STA_ObjHeader *cls_h = sta_heap_alloc(bs->heap, meta_cls_idx, 4);
    STA_ObjHeader *mc_h  = sta_heap_alloc(bs->heap, meta_mc_idx,  4);

    /* Metaclasses: all have class_index = STA_CLS_METACLASS (17). */
    STA_ObjHeader *obj_mc_h = sta_heap_alloc(bs->heap, STA_CLS_METACLASS, 4);
    STA_ObjHeader *beh_mc_h = sta_heap_alloc(bs->heap, STA_CLS_METACLASS, 4);
    STA_ObjHeader *cd_mc_h  = sta_heap_alloc(bs->heap, STA_CLS_METACLASS, 4);
    STA_ObjHeader *cls_mc_h = sta_heap_alloc(bs->heap, STA_CLS_METACLASS, 4);
    STA_ObjHeader *mc_mc_h  = sta_heap_alloc(bs->heap, STA_CLS_METACLASS, 4);

    if (!obj_h || !beh_h || !cd_h || !cls_h || !mc_h ||
        !obj_mc_h || !beh_mc_h || !cd_mc_h || !cls_mc_h || !mc_mc_h)
        return -1;

    /* Convert to OOPs. */
    bs->cls_object     = (STA_OOP)(uintptr_t)obj_h;
    bs->cls_behavior   = (STA_OOP)(uintptr_t)beh_h;
    bs->cls_class_desc = (STA_OOP)(uintptr_t)cd_h;
    bs->cls_class      = (STA_OOP)(uintptr_t)cls_h;
    bs->cls_metaclass  = (STA_OOP)(uintptr_t)mc_h;

    bs->meta_object     = (STA_OOP)(uintptr_t)obj_mc_h;
    bs->meta_behavior   = (STA_OOP)(uintptr_t)beh_mc_h;
    bs->meta_class_desc = (STA_OOP)(uintptr_t)cd_mc_h;
    bs->meta_class      = (STA_OOP)(uintptr_t)cls_mc_h;
    bs->meta_metaclass  = (STA_OOP)(uintptr_t)mc_mc_h;

    /* Register all 10 in the class table. */
    sta_class_table_set(bs->ct, STA_CLS_OBJECT,           bs->cls_object);
    sta_class_table_set(bs->ct, STA_CLS_BEHAVIOR,         bs->cls_behavior);
    sta_class_table_set(bs->ct, STA_CLS_CLASSDESCRIPTION, bs->cls_class_desc);
    sta_class_table_set(bs->ct, STA_CLS_CLASS,            bs->cls_class);
    sta_class_table_set(bs->ct, STA_CLS_METACLASS,        bs->cls_metaclass);

    sta_class_table_set(bs->ct, meta_obj_idx, bs->meta_object);
    sta_class_table_set(bs->ct, meta_beh_idx, bs->meta_behavior);
    sta_class_table_set(bs->ct, meta_cd_idx,  bs->meta_class_desc);
    sta_class_table_set(bs->ct, meta_cls_idx, bs->meta_class);
    sta_class_table_set(bs->ct, meta_mc_idx,  bs->meta_metaclass);

    /* ── Pass 2: Wire pointers ────────────────────────────────────────── */

    /* Create empty method dictionaries. */
    STA_OOP obj_md = sta_method_dict_create(bs->heap, 32);
    STA_OOP beh_md = sta_method_dict_create(bs->heap, 8);
    STA_OOP cd_md  = sta_method_dict_create(bs->heap, 8);
    STA_OOP cls_md = sta_method_dict_create(bs->heap, 8);
    STA_OOP mc_md  = sta_method_dict_create(bs->heap, 8);
    STA_OOP obj_mc_md = sta_method_dict_create(bs->heap, 4);
    STA_OOP beh_mc_md = sta_method_dict_create(bs->heap, 4);
    STA_OOP cd_mc_md  = sta_method_dict_create(bs->heap, 4);
    STA_OOP cls_mc_md = sta_method_dict_create(bs->heap, 4);
    STA_OOP mc_mc_md  = sta_method_dict_create(bs->heap, 4);

    if (!obj_md || !beh_md || !cd_md || !cls_md || !mc_md ||
        !obj_mc_md || !beh_mc_md || !cd_mc_md || !cls_mc_md || !mc_mc_md)
        return -1;

    /* Name symbols. */
    STA_OOP n_object    = bs_intern(bs, "Object");
    STA_OOP n_behavior  = bs_intern(bs, "Behavior");
    STA_OOP n_classdesc = bs_intern(bs, "ClassDescription");
    STA_OOP n_class     = bs_intern(bs, "Class");
    STA_OOP n_metaclass = bs_intern(bs, "Metaclass");

    /* Instance-side superclass chain:
     * Object→nil, Behavior→Object, ClassDescription→Behavior,
     * Class→ClassDescription, Metaclass→ClassDescription. */
    set_class_slots(bs->cls_object,     nil,              obj_md, STA_FORMAT_ENCODE(0, STA_FMT_NORMAL), n_object);
    set_class_slots(bs->cls_behavior,   bs->cls_object,   beh_md, STA_FORMAT_ENCODE(0, STA_FMT_NORMAL), n_behavior);
    set_class_slots(bs->cls_class_desc, bs->cls_behavior, cd_md,  STA_FORMAT_ENCODE(0, STA_FMT_NORMAL), n_classdesc);
    set_class_slots(bs->cls_class,      bs->cls_class_desc, cls_md, STA_FORMAT_ENCODE(4, STA_FMT_NORMAL), n_class);
    set_class_slots(bs->cls_metaclass,  bs->cls_class_desc, mc_md,  STA_FORMAT_ENCODE(4, STA_FMT_NORMAL), n_metaclass);

    /* Class-side superclass chain:
     * Object class→Class, Behavior class→Object class,
     * ClassDescription class→Behavior class,
     * Class class→ClassDescription class,
     * Metaclass class→ClassDescription class. */
    set_class_slots(bs->meta_object,     bs->cls_class,      obj_mc_md, STA_FORMAT_ENCODE(4, STA_FMT_NORMAL), nil);
    set_class_slots(bs->meta_behavior,   bs->meta_object,    beh_mc_md, STA_FORMAT_ENCODE(4, STA_FMT_NORMAL), nil);
    set_class_slots(bs->meta_class_desc, bs->meta_behavior,  cd_mc_md,  STA_FORMAT_ENCODE(4, STA_FMT_NORMAL), nil);
    set_class_slots(bs->meta_class,      bs->meta_class_desc, cls_mc_md, STA_FORMAT_ENCODE(4, STA_FMT_NORMAL), nil);
    set_class_slots(bs->meta_metaclass,  bs->meta_class_desc, mc_mc_md,  STA_FORMAT_ENCODE(4, STA_FMT_NORMAL), nil);

    /* Track for dictionary. */
    bs_track(bs, "Object",           bs->cls_object);
    bs_track(bs, "Behavior",         bs->cls_behavior);
    bs_track(bs, "ClassDescription", bs->cls_class_desc);
    bs_track(bs, "Class",            bs->cls_class);
    bs_track(bs, "Metaclass",        bs->cls_metaclass);

    return 0;
}

/* ── Step 4: Create Tier 1 classes ────────────────────────────────────── */

/* Create a class and its metaclass, wire chains, register in class table. */
static STA_OOP create_class(BS *bs, const char *name,
                              STA_OOP superclass_oop,
                              uint32_t reserved_index,
                              uint16_t inst_var_count,
                              uint8_t format_type) {
    STA_OOP name_sym = bs_intern(bs, name);
    if (name_sym == 0) return 0;

    uint32_t meta_index = bs->next_meta_index++;
    uint32_t cls_index  = reserved_index;

    /* Allocate class (class_index → its metaclass). */
    STA_ObjHeader *cls_h = sta_heap_alloc(bs->heap, meta_index, 4);
    if (!cls_h) return 0;
    STA_OOP cls = (STA_OOP)(uintptr_t)cls_h;

    /* Allocate metaclass (class_index = Metaclass). */
    STA_ObjHeader *meta_h = sta_heap_alloc(bs->heap, STA_CLS_METACLASS, 4);
    if (!meta_h) return 0;
    STA_OOP meta = (STA_OOP)(uintptr_t)meta_h;

    /* Method dictionaries. */
    STA_OOP cls_md  = sta_method_dict_create(bs->heap, 8);
    STA_OOP meta_md = sta_method_dict_create(bs->heap, 4);
    if (!cls_md || !meta_md) return 0;

    /* Metaclass superclass = superclass's metaclass.
     * The superclass's ObjHeader class_index is its metaclass's ct index. */
    STA_OOP meta_super = bs->nil_oop;
    if (superclass_oop != 0 && superclass_oop != bs->nil_oop) {
        STA_ObjHeader *super_h = (STA_ObjHeader *)(uintptr_t)superclass_oop;
        uint32_t super_meta_idx = super_h->class_index;
        meta_super = sta_class_table_get(bs->ct, super_meta_idx);
    }

    set_class_slots(cls,  superclass_oop, cls_md,
                    STA_FORMAT_ENCODE(inst_var_count, format_type), name_sym);
    set_class_slots(meta, meta_super, meta_md,
                    STA_FORMAT_ENCODE(4, STA_FMT_NORMAL), bs->nil_oop);

    sta_class_table_set(bs->ct, cls_index, cls);
    sta_class_table_set(bs->ct, meta_index, meta);

    bs_track(bs, name, cls);
    return cls;
}

static int step4_tier1(BS *bs) {
    STA_OOP obj = bs->cls_object;

    /* Magnitude hierarchy. */
    STA_OOP magnitude  = create_class(bs, "Magnitude", obj, STA_CLS_MAGNITUDE, 0, STA_FMT_NORMAL);
    STA_OOP number     = create_class(bs, "Number", magnitude, STA_CLS_NUMBER, 0, STA_FMT_NORMAL);
    STA_OOP smallint   = create_class(bs, "SmallInteger", number, STA_CLS_SMALLINTEGER, 0, STA_FMT_IMMEDIATE);
    STA_OOP flt        = create_class(bs, "Float", number, STA_CLS_FLOAT, 0, STA_FMT_IMMEDIATE);
    (void)flt;

    /* Character. */
    STA_OOP character  = create_class(bs, "Character", obj, STA_CLS_CHARACTER, 0, STA_FMT_IMMEDIATE);
    (void)character;

    /* Collection hierarchy. */
    STA_OOP collection = create_class(bs, "Collection", obj, STA_CLS_COLLECTION, 0, STA_FMT_NORMAL);
    STA_OOP seqcoll    = create_class(bs, "SequenceableCollection", collection, STA_CLS_SEQCOLLECTION, 0, STA_FMT_NORMAL);
    STA_OOP arrcoll    = create_class(bs, "ArrayedCollection", seqcoll, STA_CLS_ARRAYEDCOLLECTION, 0, STA_FMT_VARIABLE_OOP);
    STA_OOP array      = create_class(bs, "Array", arrcoll, STA_CLS_ARRAY, 0, STA_FMT_VARIABLE_OOP);
    STA_OOP string     = create_class(bs, "String", arrcoll, STA_CLS_STRING, 0, STA_FMT_VARIABLE_BYTE);
    STA_OOP symbol     = create_class(bs, "Symbol", string, STA_CLS_SYMBOL, 1, STA_FMT_VARIABLE_BYTE);
    STA_OOP bytearray  = create_class(bs, "ByteArray", arrcoll, STA_CLS_BYTEARRAY, 0, STA_FMT_VARIABLE_BYTE);
    (void)array; (void)symbol; (void)bytearray;

    /* Singleton classes. */
    STA_OOP undef_obj  = create_class(bs, "UndefinedObject", obj, STA_CLS_UNDEFINEDOBJ, 0, STA_FMT_NORMAL);
    STA_OOP true_cls   = create_class(bs, "True", obj, STA_CLS_TRUE, 0, STA_FMT_NORMAL);
    STA_OOP false_cls  = create_class(bs, "False", obj, STA_CLS_FALSE, 0, STA_FMT_NORMAL);
    (void)undef_obj;

    /* Internal classes. */
    STA_OOP assoc      = create_class(bs, "Association", obj, STA_CLS_ASSOCIATION, 2, STA_FMT_NORMAL);
    STA_OOP cm         = create_class(bs, "CompiledMethod", obj, STA_CLS_COMPILEDMETHOD, 0, STA_FMT_COMPILED_METHOD);
    STA_OOP methdict   = create_class(bs, "MethodDictionary", obj, STA_CLS_METHODDICTIONARY, 2, STA_FMT_NORMAL);
    STA_OOP blockclo   = create_class(bs, "BlockClosure", obj, STA_CLS_BLOCKCLOSURE, 5, STA_FMT_NORMAL);
    STA_OOP blockdesc  = create_class(bs, "BlockDescriptor", obj, STA_CLS_BLOCKDESCRIPTOR, 4, STA_FMT_NORMAL);
    STA_OOP message    = create_class(bs, "Message", obj, STA_CLS_MESSAGE, 3, STA_FMT_NORMAL);
    (void)assoc; (void)cm; (void)methdict; (void)blockclo;
    (void)blockdesc; (void)message;

    /* Exception hierarchy. */
    STA_OOP exception  = create_class(bs, "Exception", obj, STA_CLS_EXCEPTION, 2, STA_FMT_NORMAL);
    STA_OOP error      = create_class(bs, "Error", exception, STA_CLS_ERROR, 0, STA_FMT_NORMAL);
    STA_OOP mnu        = create_class(bs, "MessageNotUnderstood", error, STA_CLS_MESSAGENOTUNDERSTOOD, 2, STA_FMT_NORMAL);
    STA_OOP bcr        = create_class(bs, "BlockCannotReturn", error, STA_CLS_BLOCKCANNOTRETURN, 2, STA_FMT_NORMAL);
    (void)mnu; (void)bcr;

    /* SystemDictionary. */
    STA_OOP sysdict    = create_class(bs, "SystemDictionary", obj, STA_CLS_SYSTEMDICTIONARY, 2, STA_FMT_NORMAL);
    (void)sysdict;

    /* Verify all expected classes were created. */
    if (!smallint || !true_cls || !false_cls) return -1;

    return 0;
}

/* ── Step 5: Character table ──────────────────────────────────────────── */

static int step5_char_table(BS *bs) {
    STA_ObjHeader *arr_h = sta_immutable_alloc(bs->sp, STA_CLS_ARRAY, 256);
    if (!arr_h) return -1;

    STA_OOP *slots = sta_payload(arr_h);
    for (uint32_t i = 0; i < 256; i++) {
        slots[i] = STA_CHAR_OOP(i);
    }

    sta_spc_set(SPC_CHARACTER_TABLE, (STA_OOP)(uintptr_t)arr_h);
    return 0;
}

/* ── Step 6: Global dictionary ────────────────────────────────────────── */

/* Create an Association (key, value) on the heap. */
static STA_OOP make_association(BS *bs, STA_OOP key, STA_OOP value) {
    STA_ObjHeader *h = sta_heap_alloc(bs->heap, STA_CLS_ASSOCIATION, 2);
    if (!h) return 0;
    sta_payload(h)[0] = key;
    sta_payload(h)[1] = value;
    return (STA_OOP)(uintptr_t)h;
}

/* Insert (symbol, association) into a SystemDictionary. */
static int sysdict_put(BS *bs, STA_OOP dict, STA_OOP key_sym, STA_OOP value) {
    STA_OOP assoc = make_association(bs, key_sym, value);
    if (assoc == 0) return -1;

    /* Dict payload: slot 0 = tally, slot 1 = backing array. */
    STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)dict;
    STA_OOP *dp = sta_payload(dh);
    STA_OOP arr = dp[1];
    uint32_t tally = (uint32_t)STA_SMALLINT_VAL(dp[0]);

    STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)arr;
    uint32_t cap = ah->size / 2;
    STA_OOP *slots = sta_payload(ah);

    uint32_t hash = sta_symbol_get_hash(key_sym);
    uint32_t idx = hash % cap;

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t pos = ((idx + i) % cap) * 2;
        if (slots[pos] == 0) {
            slots[pos]     = key_sym;
            slots[pos + 1] = assoc;
            dp[0] = STA_SMALLINT_OOP((intptr_t)(tally + 1));
            return 0;
        }
        if (slots[pos] == key_sym) {
            slots[pos + 1] = assoc;
            return 0;
        }
    }
    return -1;  /* table full */
}

static int step6_global_dict(BS *bs) {
    /* SystemDictionary object: 2 payload slots (tally, array). */
    STA_ObjHeader *dh = sta_heap_alloc(bs->heap, STA_CLS_SYSTEMDICTIONARY, 2);
    if (!dh) return -1;
    STA_OOP dict = (STA_OOP)(uintptr_t)dh;

    /* Backing array — 128 pairs capacity = 256 slots. */
    STA_ObjHeader *ah = sta_heap_alloc(bs->heap, STA_CLS_ARRAY, 256);
    if (!ah) return -1;

    sta_payload(dh)[0] = STA_SMALLINT_OOP(0);
    sta_payload(dh)[1] = (STA_OOP)(uintptr_t)ah;

    /* Insert class bindings. */
    for (uint32_t i = 0; i < bs->num_classes; i++) {
        STA_OOP name_sym = bs_intern(bs, bs->class_names[i]);
        if (name_sym == 0) return -1;
        if (sysdict_put(bs, dict, name_sym, bs->classes[i]) != 0) return -1;
    }

    /* Self-referential: #Smalltalk → dict. */
    STA_OOP smalltalk_sym = bs_intern(bs, "Smalltalk");
    if (smalltalk_sym == 0) return -1;
    if (sysdict_put(bs, dict, smalltalk_sym, dict) != 0) return -1;

    /* Placeholder: #Transcript → nil. */
    STA_OOP transcript_sym = bs_intern(bs, "Transcript");
    if (transcript_sym == 0) return -1;
    if (sysdict_put(bs, dict, transcript_sym, bs->nil_oop) != 0) return -1;

    sta_spc_set(SPC_SMALLTALK, dict);
    return 0;
}

/* ── Step 7: Install kernel primitive methods ─────────────────────────── */

/* Create a primitive method and install it in the class's method dict. */
static int install_prim_method(BS *bs, STA_OOP class_oop,
                                const char *selector_str,
                                uint8_t prim_index, uint8_t num_args) {
    STA_OOP selector = bs_intern(bs, selector_str);
    if (selector == 0) return -1;

    STA_OOP lits[2] = { selector, class_oop };
    uint8_t bc[4] = { OP_PRIMITIVE, prim_index, OP_RETURN_SELF, 0x00 };
    uint8_t num_temps = num_args + 1;  /* extra temp for prim failure code */

    STA_OOP method = sta_compiled_method_create(bs->sp,
        num_args, num_temps, prim_index,
        lits, 2, bc, 4);
    if (method == 0) return -1;

    STA_OOP md = sta_class_method_dict(class_oop);
    return sta_method_dict_insert(bs->heap, md, selector, method);
}

/* Create a bytecoded (non-primitive) method and install it. */
static int install_bc_method(BS *bs, STA_OOP class_oop,
                              const char *selector_str,
                              uint8_t num_args, uint8_t num_temps,
                              const STA_OOP *literals, uint8_t num_literals,
                              const uint8_t *bytecodes, uint32_t num_bytecodes) {
    STA_OOP selector = bs_intern(bs, selector_str);
    if (selector == 0) return -1;

    STA_OOP method = sta_compiled_method_create(bs->sp,
        num_args, num_temps, 0,  /* no primitive */
        literals, num_literals,
        bytecodes, num_bytecodes);
    if (method == 0) return -1;

    STA_OOP md = sta_class_method_dict(class_oop);
    return sta_method_dict_insert(bs->heap, md, selector, method);
}

static int step7_methods(BS *bs) {
    STA_OOP obj_cls = bs->cls_object;

    /* Look up Tier 1 classes from the class table. */
    STA_OOP si_cls    = sta_class_table_get(bs->ct, STA_CLS_SMALLINTEGER);
    STA_OOP arr_cls   = sta_class_table_get(bs->ct, STA_CLS_ARRAY);
    STA_OOP bc_cls    = sta_class_table_get(bs->ct, STA_CLS_BLOCKCLOSURE);
    STA_OOP true_cls  = sta_class_table_get(bs->ct, STA_CLS_TRUE);
    STA_OOP false_cls = sta_class_table_get(bs->ct, STA_CLS_FALSE);

    /* ── Object methods ───────────────────────────────────────────────── */
    if (install_prim_method(bs, obj_cls, "==",                  29, 1) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "class",               30, 0) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "basicAt:",             33, 1) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "basicAt:put:",        34, 2) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "basicSize",           35, 0) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "hash",                36, 0) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "become:",             37, 1) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "instVarAt:",          38, 1) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "instVarAt:put:",      39, 2) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "identityHash",        40, 0) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "shallowCopy",         41, 0) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "yourself",            42, 0) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "respondsTo:",        120, 1) != 0) return -1;
    if (install_prim_method(bs, obj_cls, "doesNotUnderstand:", 121, 1) != 0) return -1;

    /* ── SmallInteger methods ─────────────────────────────────────────── */
    if (install_prim_method(bs, si_cls, "+", 1, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "-", 2, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "<", 3, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, ">", 4, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "<=", 5, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, ">=", 6, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "=", 7, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "~=", 8, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "*", 9, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "/", 10, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "\\\\", 11, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "//", 12, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "quo:", 13, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "bitAnd:", 14, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "bitOr:", 15, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "bitXor:", 16, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "bitShift:", 17, 1) != 0) return -1;
    if (install_prim_method(bs, si_cls, "printString", 200, 0) != 0) return -1;

    /* ── ArrayedCollection methods ───────────────────────────────────── */
    STA_OOP ac_cls = sta_class_table_get(bs->ct, STA_CLS_ARRAYEDCOLLECTION);
    if (install_prim_method(bs, ac_cls, "size",    53, 0) != 0) return -1;

    /* ── Array methods ────────────────────────────────────────────────── */
    if (install_prim_method(bs, arr_cls, "at:",     51, 1) != 0) return -1;
    if (install_prim_method(bs, arr_cls, "at:put:", 52, 2) != 0) return -1;

    /* ── ByteArray methods ─────────────────────────────────────────────── */
    STA_OOP ba_cls = sta_class_table_get(bs->ct, STA_CLS_BYTEARRAY);
    if (install_prim_method(bs, ba_cls, "basicAt:",    60, 1) != 0) return -1;
    if (install_prim_method(bs, ba_cls, "basicAt:put:",61, 2) != 0) return -1;
    if (install_prim_method(bs, ba_cls, "basicSize",   62, 0) != 0) return -1;
    if (install_prim_method(bs, ba_cls, "at:",          60, 1) != 0) return -1;
    if (install_prim_method(bs, ba_cls, "at:put:",      61, 2) != 0) return -1;

    /* ── String methods ────────────────────────────────────────────────── */
    STA_OOP str_cls = sta_class_table_get(bs->ct, STA_CLS_STRING);
    if (install_prim_method(bs, str_cls, "at:",     63, 1) != 0) return -1;
    if (install_prim_method(bs, str_cls, "at:put:", 64, 2) != 0) return -1;

    /* ── Character methods ──────────────────────────────────────────────── */
    STA_OOP char_cls = sta_class_table_get(bs->ct, STA_CLS_CHARACTER);
    if (install_prim_method(bs, char_cls, "value", 94, 0) != 0) return -1;

    /* Character class >> value: — install on the metaclass */
    {
        STA_ObjHeader *char_cls_h = (STA_ObjHeader *)(uintptr_t)char_cls;
        STA_OOP char_meta = sta_class_table_get(bs->ct, char_cls_h->class_index);
        if (install_prim_method(bs, char_meta, "value:", 95, 1) != 0) return -1;
    }

    /* ── BlockClosure methods ─────────────────────────────────────────── */
    if (install_prim_method(bs, bc_cls, "value",  81, 0) != 0) return -1;
    if (install_prim_method(bs, bc_cls, "value:", 82, 1) != 0) return -1;

    /* ── Object >> initialize (default — returns self) ──────────────── */
    {
        uint8_t bc[] = { OP_RETURN_SELF, 0x00 };
        if (install_bc_method(bs, obj_cls, "initialize",
                              0, 0, &obj_cls, 1, bc, 2) != 0) return -1;
    }

    /* ── Behavior methods (basicNew, basicNew:, new, new:) ───────────── */
    STA_OOP beh_cls = bs->cls_behavior;

    /* Behavior >> basicNew — primitive 31 */
    if (install_prim_method(bs, beh_cls, "basicNew",  31, 0) != 0) return -1;

    /* Behavior >> basicNew: — primitive 32 */
    if (install_prim_method(bs, beh_cls, "basicNew:", 32, 1) != 0) return -1;

    /* Shared selector for Behavior>>new and new: */
    STA_OOP init_sel = bs_intern(bs, "initialize");
    if (init_sel == 0) return -1;

    /* Behavior >> new — self basicNew initialize */
    {
        STA_OOP basicNew_sel = bs_intern(bs, "basicNew");
        if (basicNew_sel == 0) return -1;

        STA_OOP lits[] = { basicNew_sel, init_sel, beh_cls };
        uint8_t bc[] = {
            OP_PUSH_RECEIVER,  0x00,   /* push self (the class)         */
            OP_SEND,           0x00,   /* send #basicNew (lit 0)        */
            OP_SEND,           0x01,   /* send #initialize (lit 1)      */
            OP_RETURN_TOP,     0x00,   /* return the initialized object */
        };
        if (install_bc_method(bs, beh_cls, "new",
                              0, 0, lits, 3, bc, 8) != 0) return -1;
    }

    /* Behavior >> new: — (self basicNew: anInteger) initialize */
    {
        STA_OOP basicNewSize_sel = bs_intern(bs, "basicNew:");
        if (basicNewSize_sel == 0) return -1;

        STA_OOP lits[] = { basicNewSize_sel, init_sel, beh_cls };
        uint8_t bc[] = {
            OP_PUSH_RECEIVER,  0x00,   /* push self (the class)         */
            OP_PUSH_TEMP,      0x00,   /* push argument (the size)      */
            OP_SEND,           0x00,   /* send #basicNew: (lit 0)       */
            OP_SEND,           0x01,   /* send #initialize (lit 1)      */
            OP_RETURN_TOP,     0x00,   /* return the initialized object */
        };
        if (install_bc_method(bs, beh_cls, "new:",
                              1, 1, lits, 3, bc, 10) != 0) return -1;
    }

    /* ── Boolean conditional methods (hand-assembled bytecodes) ───────── */
    STA_OOP value_sel = bs_intern(bs, "value");
    if (value_sel == 0) return -1;

    /* True >> ifTrue: aBlock  →  aBlock value */
    {
        STA_OOP lits[] = { value_sel, true_cls };
        uint8_t bc[] = { OP_PUSH_TEMP, 0, OP_SEND, 0, OP_RETURN_TOP, 0 };
        if (install_bc_method(bs, true_cls, "ifTrue:", 1, 1, lits, 2, bc, 6) != 0) return -1;
    }
    /* True >> ifFalse: aBlock  →  nil */
    {
        STA_OOP lits[] = { value_sel, true_cls };
        uint8_t bc[] = { OP_RETURN_NIL, 0 };
        if (install_bc_method(bs, true_cls, "ifFalse:", 1, 1, lits, 2, bc, 2) != 0) return -1;
    }
    /* True >> ifTrue:ifFalse: trueBlock falseBlock  →  trueBlock value */
    {
        STA_OOP lits[] = { value_sel, true_cls };
        uint8_t bc[] = { OP_PUSH_TEMP, 0, OP_SEND, 0, OP_RETURN_TOP, 0 };
        if (install_bc_method(bs, true_cls, "ifTrue:ifFalse:", 2, 2, lits, 2, bc, 6) != 0) return -1;
    }
    /* False >> ifTrue: aBlock  →  nil */
    {
        STA_OOP lits[] = { value_sel, false_cls };
        uint8_t bc[] = { OP_RETURN_NIL, 0 };
        if (install_bc_method(bs, false_cls, "ifTrue:", 1, 1, lits, 2, bc, 2) != 0) return -1;
    }
    /* False >> ifFalse: aBlock  →  aBlock value */
    {
        STA_OOP lits[] = { value_sel, false_cls };
        uint8_t bc[] = { OP_PUSH_TEMP, 0, OP_SEND, 0, OP_RETURN_TOP, 0 };
        if (install_bc_method(bs, false_cls, "ifFalse:", 1, 1, lits, 2, bc, 6) != 0) return -1;
    }
    /* False >> ifTrue:ifFalse: trueBlock falseBlock  →  falseBlock value */
    {
        STA_OOP lits[] = { value_sel, false_cls };
        uint8_t bc[] = { OP_PUSH_TEMP, 1, OP_SEND, 0, OP_RETURN_TOP, 0 };
        if (install_bc_method(bs, false_cls, "ifTrue:ifFalse:", 2, 2, lits, 2, bc, 6) != 0) return -1;
    }

    /* ── Class >> subclass:instanceVariableNames:classVariableNames:
     *              poolDictionaries:category: — primitive 122 ────────────── */
    STA_OOP cls_cls = bs->cls_class;
    if (install_prim_method(bs, cls_cls,
            "subclass:instanceVariableNames:classVariableNames:poolDictionaries:category:",
            122, 5) != 0) return -1;

    return 0;
}

/* ── Step 8: Exception handling methods ─────────────────────────────────── */

/* Helper: compile a method from source and install it on a class.
 * Uses the compiler (scanner → parser → codegen → CompiledMethod). */
static int install_source_method(BS *bs, STA_OOP class_oop,
                                   const char *source,
                                   const char *selector_str,
                                   const char **instvar_names,
                                   uint32_t instvar_count) {
    STA_OOP sysdict = sta_spc_get(SPC_SMALLTALK);
    STA_CompileResult cr = sta_compile_method(
        source, class_oop, instvar_names, instvar_count,
        bs->st, bs->sp, bs->heap, sysdict);
    if (cr.had_error) {
        fprintf(stderr, "bootstrap: compile error for %s: %s\n",
                selector_str, cr.error_msg);
        return -1;
    }

    STA_OOP sel = bs_intern(bs, selector_str);
    if (sel == 0) return -1;

    STA_OOP md = sta_class_method_dict(class_oop);
    return sta_method_dict_insert(bs->heap, md, sel, cr.method);
}

static int step8_exception_methods(BS *bs) {
    STA_OOP obj_cls = bs->cls_object;
    STA_OOP bc_cls  = sta_class_table_get(bs->ct, STA_CLS_BLOCKCLOSURE);
    STA_OOP exc_cls = sta_class_table_get(bs->ct, STA_CLS_EXCEPTION);
    STA_OOP err_cls = sta_class_table_get(bs->ct, STA_CLS_ERROR);
    STA_OOP mnu_cls = sta_class_table_get(bs->ct, STA_CLS_MESSAGENOTUNDERSTOOD);

    /* ── Exception accessor methods ──────────────────────────────────── */
    /* Exception instVars: messageText (0), signalContext (1) */
    static const char *exc_ivars[] = { "messageText", "signalContext" };

    if (install_source_method(bs, exc_cls,
            "messageText ^messageText", "messageText",
            exc_ivars, 2) != 0) return -1;
    if (install_source_method(bs, exc_cls,
            "messageText: aString messageText := aString", "messageText:",
            exc_ivars, 2) != 0) return -1;

    /* Exception >> signal — primitive 89 */
    if (install_prim_method(bs, exc_cls, "signal", 89, 0) != 0) return -1;

    /* ── Error (no additional methods needed for Phase 1) ────────────── */
    (void)err_cls;

    /* ── MessageNotUnderstood accessor methods ───────────────────────── */
    /* MNU instVars: messageText (0), signalContext (1), message (2), receiver (3) */
    static const char *mnu_ivars[] = {
        "messageText", "signalContext", "message", "receiver"
    };

    if (install_source_method(bs, mnu_cls,
            "message ^message", "message",
            mnu_ivars, 4) != 0) return -1;
    if (install_source_method(bs, mnu_cls,
            "message: aMessage message := aMessage", "message:",
            mnu_ivars, 4) != 0) return -1;
    if (install_source_method(bs, mnu_cls,
            "receiver ^receiver", "receiver",
            mnu_ivars, 4) != 0) return -1;
    if (install_source_method(bs, mnu_cls,
            "receiver: anObject receiver := anObject", "receiver:",
            mnu_ivars, 4) != 0) return -1;

    /* ── BlockClosure >> on:do: — primitive 88 ───────────────────────── */
    if (install_prim_method(bs, bc_cls, "on:do:", 88, 2) != 0) return -1;

    /* ── BlockClosure >> ensure: — primitive 90 ──────────────────────── */
    if (install_prim_method(bs, bc_cls, "ensure:", 90, 1) != 0) return -1;

    /* ── Object >> doesNotUnderstand: — real Smalltalk method ─────────── */
    /* Replaces the prim 121 stub. Creates MessageNotUnderstood and signals. */
    if (install_source_method(bs, obj_cls,
            "doesNotUnderstand: aMessage "
              "| ex | "
              "ex := MessageNotUnderstood new. "
              "ex receiver: self. "
              "ex message: aMessage. "
              "ex signal",
            "doesNotUnderstand:",
            NULL, 0) != 0) return -1;

    return 0;
}

/* ── Bootstrap entry point ────────────────────────────────────────────── */

STA_BootstrapResult sta_bootstrap(
    STA_Heap *heap,
    STA_ImmutableSpace *immutable_space,
    STA_SymbolTable *symbol_table,
    STA_ClassTable *class_table)
{
    BS bs;
    memset(&bs, 0, sizeof(bs));
    bs.heap = heap;
    bs.sp   = immutable_space;
    bs.st   = symbol_table;
    bs.ct   = class_table;

    sta_special_objects_init();
    sta_primitive_table_init();

    if (step1_singletons(&bs) != 0)
        return (STA_BootstrapResult){ -1, "step 1: failed to allocate nil/true/false" };

    if (step2_symbols(&bs) != 0)
        return (STA_BootstrapResult){ -1, "step 2: failed to intern kernel symbols" };

    if (step3_tier0(&bs) != 0)
        return (STA_BootstrapResult){ -1, "step 3: failed to create Tier 0 classes" };

    if (step4_tier1(&bs) != 0)
        return (STA_BootstrapResult){ -1, "step 4: failed to create Tier 1 classes" };

    if (step5_char_table(&bs) != 0)
        return (STA_BootstrapResult){ -1, "step 5: failed to create character table" };

    if (step6_global_dict(&bs) != 0)
        return (STA_BootstrapResult){ -1, "step 6: failed to create global dictionary" };

    if (step7_methods(&bs) != 0)
        return (STA_BootstrapResult){ -1, "step 7: failed to install kernel methods" };

    /* Set the class table, heap, symbol table, and immutable space for primitives. */
    sta_primitive_set_class_table(class_table);
    sta_primitive_set_heap(heap);
    sta_primitive_set_symbol_table(symbol_table);
    sta_primitive_set_immutable_space(immutable_space);

    if (step8_exception_methods(&bs) != 0)
        return (STA_BootstrapResult){ -1, "step 8: failed to install exception methods" };

    return (STA_BootstrapResult){ 0, NULL };
}
