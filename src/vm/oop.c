/* src/vm/oop.c
 * Compile-time layout verification for STA_ObjHeader.
 * All checks are _Static_assert — they fire at compile time, not runtime.
 * Phase 1 — see ADR 007.
 */
#include "oop.h"
#include <stddef.h>

_Static_assert(sizeof(STA_ObjHeader) == 12,
               "STA_ObjHeader must be exactly 12 bytes (ADR 007)");

_Static_assert(offsetof(STA_ObjHeader, class_index) == 0,
               "class_index must be at offset 0");
_Static_assert(offsetof(STA_ObjHeader, size) == 4,
               "size must be at offset 4");
_Static_assert(offsetof(STA_ObjHeader, gc_flags) == 8,
               "gc_flags must be at offset 8");
_Static_assert(offsetof(STA_ObjHeader, obj_flags) == 9,
               "obj_flags must be at offset 9");
_Static_assert(offsetof(STA_ObjHeader, reserved) == 10,
               "reserved must be at offset 10");

_Static_assert(sizeof(STA_OOP) == 8,
               "STA_OOP must be 8 bytes (arm64 64-bit)");

_Static_assert(STA_ALLOC_UNIT == 16u,
               "STA_ALLOC_UNIT must be 16");

_Static_assert(STA_HEADER_SIZE == 12u,
               "STA_HEADER_SIZE must be 12");
