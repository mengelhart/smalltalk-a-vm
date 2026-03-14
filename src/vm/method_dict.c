/* src/vm/method_dict.c
 * MethodDictionary — see method_dict.h for documentation.
 */
#include "method_dict.h"
#include "symbol_table.h"
#include "class_table.h"
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Read the tally (slot 0) as a C integer. */
static uint32_t dict_tally(STA_OOP dict) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)dict;
    STA_OOP *payload = sta_payload(h);
    return (uint32_t)STA_SMALLINT_VAL(payload[0]);
}

/* Write the tally as a SmallInt into slot 0. */
static void dict_set_tally(STA_OOP dict, uint32_t tally) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)dict;
    STA_OOP *payload = sta_payload(h);
    payload[0] = STA_SMALLINT_OOP((intptr_t)tally);
}

/* Get the backing Array OOP from slot 1. */
static STA_OOP dict_array(STA_OOP dict) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)dict;
    STA_OOP *payload = sta_payload(h);
    return payload[1];
}

/* Set the backing Array OOP in slot 1. */
static void dict_set_array(STA_OOP dict, STA_OOP array_oop) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)dict;
    STA_OOP *payload = sta_payload(h);
    payload[1] = array_oop;
}

/* Get the capacity (number of pairs) from the backing Array's size. */
static uint32_t array_capacity(STA_OOP array_oop) {
    STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)array_oop;
    return ah->size / 2;
}

/* Get a pointer to the backing Array's payload slots. */
static STA_OOP *array_slots(STA_OOP array_oop) {
    STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)array_oop;
    return sta_payload(ah);
}

/* Allocate a backing Array with the given number of pairs. */
static STA_OOP alloc_array(STA_Heap *heap, uint32_t capacity) {
    uint32_t nwords = capacity * 2;
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_ARRAY, nwords);
    if (!h) return 0;
    return (STA_OOP)(uintptr_t)h;
}

/* ── Create ────────────────────────────────────────────────────────────── */

STA_OOP sta_method_dict_create(STA_Heap *heap, uint32_t capacity) {
    if (capacity == 0) capacity = 4;

    /* Allocate the MethodDictionary object (2 payload slots: tally + array). */
    STA_ObjHeader *dh = sta_heap_alloc(heap, STA_CLS_METHODDICTIONARY, 2);
    if (!dh) return 0;

    STA_OOP dict = (STA_OOP)(uintptr_t)dh;

    /* Allocate the backing Array. */
    STA_OOP arr = alloc_array(heap, capacity);
    if (arr == 0) return 0;

    dict_set_tally(dict, 0);
    dict_set_array(dict, arr);
    return dict;
}

/* ── Lookup ────────────────────────────────────────────────────────────── */

STA_OOP sta_method_dict_lookup(STA_OOP dict, STA_OOP selector) {
    STA_OOP arr_oop = dict_array(dict);
    uint32_t cap    = array_capacity(arr_oop);
    STA_OOP *slots  = array_slots(arr_oop);

    uint32_t hash = sta_symbol_get_hash(selector);
    uint32_t idx  = hash % cap;

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t pos = ((idx + i) % cap) * 2;
        STA_OOP sel  = slots[pos];
        if (sel == 0) return 0;            /* empty slot — not found   */
        if (sel == selector)               /* identity comparison      */
            return slots[pos + 1];         /* method OOP               */
    }
    return 0;  /* full table, not found */
}

/* ── Insert ────────────────────────────────────────────────────────────── */

/* Insert into a specific array without growth checks. */
static void raw_insert(STA_OOP *slots, uint32_t cap,
                       STA_OOP selector, STA_OOP method) {
    uint32_t hash = sta_symbol_get_hash(selector);
    uint32_t idx  = hash % cap;

    for (uint32_t i = 0; i < cap; i++) {
        uint32_t pos = ((idx + i) % cap) * 2;
        STA_OOP sel  = slots[pos];
        if (sel == 0 || sel == selector) {
            slots[pos]     = selector;
            slots[pos + 1] = method;
            return;
        }
    }
}

int sta_method_dict_insert(STA_Heap *heap, STA_OOP dict,
                           STA_OOP selector, STA_OOP method) {
    STA_OOP arr_oop = dict_array(dict);
    uint32_t cap    = array_capacity(arr_oop);
    STA_OOP *slots  = array_slots(arr_oop);
    uint32_t tally  = dict_tally(dict);

    /* Check if selector already present (update in place). */
    {
        uint32_t hash = sta_symbol_get_hash(selector);
        uint32_t idx  = hash % cap;
        for (uint32_t i = 0; i < cap; i++) {
            uint32_t pos = ((idx + i) % cap) * 2;
            STA_OOP sel  = slots[pos];
            if (sel == 0) break;
            if (sel == selector) {
                slots[pos + 1] = method;
                return 0;  /* updated — tally unchanged */
            }
        }
    }

    /* Check load factor: (tally + 1) / cap > 0.7 → grow. */
    if ((tally + 1) * 10 > cap * 7) {
        uint32_t new_cap = cap * 2;
        STA_OOP new_arr  = alloc_array(heap, new_cap);
        if (new_arr == 0) return -1;

        STA_OOP *new_slots = array_slots(new_arr);

        /* Rehash existing entries. */
        for (uint32_t i = 0; i < cap; i++) {
            uint32_t pos = i * 2;
            if (slots[pos] != 0) {
                raw_insert(new_slots, new_cap, slots[pos], slots[pos + 1]);
            }
        }

        dict_set_array(dict, new_arr);
        arr_oop = new_arr;
        cap     = new_cap;
        slots   = new_slots;
    }

    /* Insert the new entry. */
    raw_insert(slots, cap, selector, method);
    dict_set_tally(dict, tally + 1);
    return 0;
}
