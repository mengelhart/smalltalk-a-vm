/* src/vm/special_selectors.c
 * Bootstrap interning of special selectors — see special_selectors.h.
 */
#include "special_selectors.h"
#include "special_objects.h"
#include "class_table.h"
#include <string.h>

/* The 32 special selectors from bytecode spec §10.7, indexed 0–31. */
static const char *const special_selector_names[32] = {
    "+",                    /*  0 */
    "-",                    /*  1 */
    "<",                    /*  2 */
    ">",                    /*  3 */
    "<=",                   /*  4 */
    ">=",                   /*  5 */
    "=",                    /*  6 */
    "~=",                   /*  7 */
    "*",                    /*  8 */
    "/",                    /*  9 */
    "\\",                   /* 10 — modulo */
    "@",                    /* 11 */
    "bitShift:",            /* 12 */
    "//",                   /* 13 */
    "bitAnd:",              /* 14 */
    "bitOr:",               /* 15 */
    "at:",                  /* 16 */
    "at:put:",              /* 17 */
    "size",                 /* 18 */
    "next",                 /* 19 */
    "nextPut:",             /* 20 */
    "atEnd",                /* 21 */
    "==",                   /* 22 */
    "class",                /* 23 */
    "value",                /* 24 */
    "value:",               /* 25 */
    "do:",                  /* 26 */
    "new",                  /* 27 */
    "new:",                 /* 28 */
    "yourself",             /* 29 */
    "doesNotUnderstand:",   /* 30 */
    "mustBeBoolean",        /* 31 */
};

int sta_intern_special_selectors(STA_ImmutableSpace *sp, STA_SymbolTable *st) {
    /* Allocate a 32-element Array in immutable space. */
    STA_ObjHeader *arr_hdr = sta_immutable_alloc(sp, STA_CLS_ARRAY, 32);
    if (!arr_hdr) return -1;

    STA_OOP arr_oop = (STA_OOP)(uintptr_t)arr_hdr;
    STA_OOP *arr_slots = sta_payload(arr_hdr);

    /* Intern each special selector and store in the array. */
    for (int i = 0; i < 32; i++) {
        const char *name = special_selector_names[i];
        STA_OOP sym = sta_symbol_intern(sp, st, name, strlen(name));
        if (sym == 0) return -1;
        arr_slots[i] = sym;
    }

    /* Store the Array at SPC_SPECIAL_SELECTORS. */
    sta_spc_set(SPC_SPECIAL_SELECTORS, arr_oop);

    /* Store individual symbol OOPs at their special object indices.
     * doesNotUnderstand: is already interned as special selector index 30.
     * mustBeBoolean is already interned as special selector index 31. */
    sta_spc_set(SPC_DOES_NOT_UNDERSTAND, arr_slots[30]);
    sta_spc_set(SPC_MUST_BE_BOOLEAN, arr_slots[31]);

    /* Intern additional symbols not in the special selector table. */
    STA_OOP sym;

    sym = sta_symbol_intern(sp, st, "cannotReturn:", 13);
    if (sym == 0) return -1;
    sta_spc_set(SPC_CANNOT_RETURN, sym);

    sym = sta_symbol_intern(sp, st, "startUp", 7);
    if (sym == 0) return -1;
    sta_spc_set(SPC_STARTUP, sym);

    sym = sta_symbol_intern(sp, st, "shutDown", 8);
    if (sym == 0) return -1;
    sta_spc_set(SPC_SHUTDOWN, sym);

    sym = sta_symbol_intern(sp, st, "run", 3);
    if (sym == 0) return -1;
    sta_spc_set(SPC_RUN, sym);

    sym = sta_symbol_intern(sp, st, "childFailed:reason:", 19);
    if (sym == 0) return -1;
    sta_spc_set(SPC_CHILD_FAILED_REASON, sym);

    return 0;
}
