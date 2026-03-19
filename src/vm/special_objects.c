/* src/vm/special_objects.c
 * Special object table implementation.
 * Phase 2 — redirectable pointer to VM's specials array.
 */
#include "special_objects.h"
#include <string.h>

/* Fallback array for pre-VM-creation state (bootstrap tests that don't
 * use STA_VM). Once a VM is created, sta_special_objects points to
 * vm->specials instead. */
static STA_OOP g_fallback_specials[STA_SPECIAL_OBJECTS_COUNT];

STA_OOP *sta_special_objects = g_fallback_specials;

void sta_special_objects_init(void) {
    memset(sta_special_objects, 0,
           STA_SPECIAL_OBJECTS_COUNT * sizeof(STA_OOP));
}

void sta_special_objects_bind(STA_OOP *specials_array) {
    if (specials_array) {
        sta_special_objects = specials_array;
    } else {
        sta_special_objects = g_fallback_specials;
    }
}
