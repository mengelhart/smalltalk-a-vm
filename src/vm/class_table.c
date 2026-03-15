/* src/vm/class_table.c
 * Class table implementation.
 * Phase 1 — see class_table.h for interface documentation.
 */
#include "class_table.h"
#include <stdlib.h>
#include <string.h>

struct STA_ClassTable {
    _Atomic(STA_OOP *) entries;   /* atomic pointer for Phase 2 grow-swap */
    uint32_t           capacity;
};

STA_ClassTable *sta_class_table_create(void) {
    STA_ClassTable *ct = malloc(sizeof(*ct));
    if (!ct) return NULL;

    STA_OOP *entries = calloc(STA_CLASS_TABLE_INITIAL_CAPACITY, sizeof(STA_OOP));
    if (!entries) { free(ct); return NULL; }

    atomic_init(&ct->entries, entries);
    ct->capacity = STA_CLASS_TABLE_INITIAL_CAPACITY;
    return ct;
}

void sta_class_table_destroy(STA_ClassTable *ct) {
    if (!ct) return;
    STA_OOP *entries = atomic_load_explicit(&ct->entries, memory_order_relaxed);
    free(entries);
    free(ct);
}

int sta_class_table_set(STA_ClassTable *ct, uint32_t index, STA_OOP class_oop) {
    if (index == 0 || index >= ct->capacity) return -1;
    STA_OOP *entries = atomic_load_explicit(&ct->entries, memory_order_relaxed);
    entries[index] = class_oop;
    return 0;
}

STA_OOP sta_class_table_get(STA_ClassTable *ct, uint32_t index) {
    if (index >= ct->capacity) return 0;
    STA_OOP *entries = atomic_load_explicit(&ct->entries, memory_order_relaxed);
    return entries[index];
}

uint32_t sta_class_table_capacity(STA_ClassTable *ct) {
    return ct->capacity;
}

uint32_t sta_class_table_index_of(STA_ClassTable *ct, STA_OOP class_oop) {
    if (class_oop == 0) return 0;
    STA_OOP *entries = atomic_load_explicit(&ct->entries, memory_order_relaxed);
    for (uint32_t i = 1; i < ct->capacity; i++) {
        if (entries[i] == class_oop) return i;
    }
    return 0;
}

uint32_t sta_class_table_alloc_index(STA_ClassTable *ct) {
    STA_OOP *entries = atomic_load_explicit(&ct->entries, memory_order_relaxed);
    for (uint32_t i = STA_CLS_RESERVED_COUNT; i < ct->capacity; i++) {
        if (entries[i] == 0) return i;
    }
    return 0;
}
