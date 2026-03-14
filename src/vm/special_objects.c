/* src/vm/special_objects.c
 * Special object table implementation.
 * Phase 1 — see special_objects.h.
 */
#include "special_objects.h"
#include <string.h>

STA_OOP sta_special_objects[STA_SPECIAL_OBJECTS_COUNT];

void sta_special_objects_init(void) {
    memset(sta_special_objects, 0, sizeof(sta_special_objects));
}
