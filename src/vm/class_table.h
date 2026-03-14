/* src/vm/class_table.h
 * Class table — maps class_index (uint32_t) to class OOP.
 * Phase 1 — permanent. See bytecode spec section 11.5, ADR 012 section
 * "Class identifier portability".
 *
 * Reserved indices 0–31 are hardcoded by the bytecode spec.
 * The backing array pointer is _Atomic so Phase 2 can grow the table
 * via atomic swap without restructuring call sites.
 *
 * Phase 1: single-threaded — no concurrent access. The atomic pointer is
 * structural preparation only.
 */
#pragma once
#include "oop.h"
#include <stdint.h>
#include <stdatomic.h>

/* ── Initial capacity ───────────────────────────────────────────────────── */
#define STA_CLASS_TABLE_INITIAL_CAPACITY  256u

/* ── Reserved class indices (bytecode spec section 11.5) ────────────────── */
#define STA_CLS_INVALID                0u  /* invalid / unassigned           */
#define STA_CLS_SMALLINTEGER           1u  /* SmallInteger (tagged immediate)*/
#define STA_CLS_OBJECT                 2u  /* Object                         */
#define STA_CLS_UNDEFINEDOBJ           3u  /* UndefinedObject (nil's class)  */
#define STA_CLS_TRUE                   4u  /* True                           */
#define STA_CLS_FALSE                  5u  /* False                          */
#define STA_CLS_CHARACTER              6u  /* Character                      */
#define STA_CLS_SYMBOL                 7u  /* Symbol                         */
#define STA_CLS_STRING                 8u  /* String                         */
#define STA_CLS_ARRAY                  9u  /* Array                          */
#define STA_CLS_BYTEARRAY            10u  /* ByteArray                      */
#define STA_CLS_FLOAT                 11u  /* Float                          */
#define STA_CLS_COMPILEDMETHOD        12u  /* CompiledMethod                 */
#define STA_CLS_BLOCKCLOSURE          13u  /* BlockClosure                   */
#define STA_CLS_ASSOCIATION           14u  /* Association                    */
#define STA_CLS_METHODDICTIONARY      15u  /* MethodDictionary               */
#define STA_CLS_CLASS                 16u  /* Class                          */
#define STA_CLS_METACLASS             17u  /* Metaclass                      */
#define STA_CLS_BEHAVIOR              18u  /* Behavior                       */
#define STA_CLS_CLASSDESCRIPTION      19u  /* ClassDescription               */
#define STA_CLS_BLOCKDESCRIPTOR       20u  /* BlockDescriptor                */
#define STA_CLS_MESSAGE               21u  /* Message                        */
#define STA_CLS_NUMBER                22u  /* Number                         */
#define STA_CLS_MAGNITUDE             23u  /* Magnitude                      */
#define STA_CLS_COLLECTION            24u  /* Collection                     */
#define STA_CLS_SEQCOLLECTION         25u  /* SequenceableCollection         */
#define STA_CLS_ARRAYEDCOLLECTION     26u  /* ArrayedCollection              */
#define STA_CLS_EXCEPTION             27u  /* Exception                      */
#define STA_CLS_ERROR                 28u  /* Error                          */
#define STA_CLS_MESSAGENOTUNDERSTOOD  29u  /* MessageNotUnderstood           */
#define STA_CLS_BLOCKCANNOTRETURN     30u  /* BlockCannotReturn              */
#define STA_CLS_SYSTEMDICTIONARY      31u  /* SystemDictionary               */
#define STA_CLS_RESERVED_COUNT        32u  /* first dynamically-assigned idx */

/* ── Opaque type ────────────────────────────────────────────────────────── */
typedef struct STA_ClassTable STA_ClassTable;

/* ── Lifecycle ──────────────────────────────────────────────────────────── */

/* Create a class table with STA_CLASS_TABLE_INITIAL_CAPACITY entries.
 * All entries initialized to 0 (null OOP). Returns NULL on failure. */
STA_ClassTable *sta_class_table_create(void);

/* Destroy the class table and free all memory. */
void sta_class_table_destroy(STA_ClassTable *ct);

/* ── Access ─────────────────────────────────────────────────────────────── */

/* Register class_oop at the given index.
 * Index must be > 0 (index 0 is the invalid sentinel) and < capacity.
 * Returns 0 on success, -1 if index is out of range or index == 0. */
int sta_class_table_set(STA_ClassTable *ct, uint32_t index, STA_OOP class_oop);

/* Look up the class OOP at the given index.
 * Returns 0 (null OOP) if index is out of range or unregistered. */
STA_OOP sta_class_table_get(STA_ClassTable *ct, uint32_t index);

/* Return the current capacity. */
uint32_t sta_class_table_capacity(STA_ClassTable *ct);
