/* src/vm/interpreter.h
 * Bytecode interpreter — Phase 1 dispatch loop.
 * Phase 1 — permanent. See bytecode spec §§2–5, 10, ADR 010, ADR 014.
 */
#pragma once
#include "oop.h"
#include "frame.h"
#include "heap.h"
#include "class_table.h"
#include <stdint.h>

/* ── Opcode constants (bytecode spec §3, §12) ─────────────────────────── */

#define OP_NOP              0x00u
#define OP_WIDE             0x01u
#define OP_PUSH_RECEIVER    0x02u
#define OP_PUSH_NIL         0x03u
#define OP_PUSH_TRUE        0x04u
#define OP_PUSH_FALSE       0x05u
#define OP_PUSH_LIT         0x06u
#define OP_PUSH_TEMP        0x07u
#define OP_PUSH_INSTVAR     0x08u
#define OP_PUSH_GLOBAL      0x09u
#define OP_PUSH_CONTEXT     0x0Au
#define OP_PUSH_SMALLINT    0x0Bu
#define OP_PUSH_MINUS_ONE   0x0Cu
#define OP_PUSH_ZERO        0x0Du
#define OP_PUSH_ONE         0x0Eu
#define OP_PUSH_TWO         0x0Fu

#define OP_STORE_TEMP           0x10u
#define OP_STORE_INSTVAR        0x11u
#define OP_STORE_GLOBAL         0x12u
#define OP_POP_STORE_TEMP       0x13u
#define OP_POP_STORE_INSTVAR    0x14u
#define OP_POP_STORE_GLOBAL     0x15u
#define OP_STORE_OUTER_TEMP     0x16u
#define OP_POP_STORE_OUTER_TEMP 0x17u

#define OP_SEND             0x20u
#define OP_SEND_SUPER       0x21u

#define OP_RETURN_TOP       0x30u
#define OP_RETURN_SELF      0x31u
#define OP_RETURN_NIL       0x32u
#define OP_RETURN_TRUE      0x33u
#define OP_RETURN_FALSE     0x34u
#define OP_NON_LOCAL_RETURN 0x35u

#define OP_JUMP             0x40u
#define OP_JUMP_TRUE        0x41u
#define OP_JUMP_FALSE       0x42u
#define OP_JUMP_BACK        0x43u

#define OP_POP              0x50u
#define OP_DUP              0x51u
#define OP_PRIMITIVE        0x52u

#define OP_BLOCK_COPY       0x60u
#define OP_CLOSURE_COPY     0x61u
#define OP_PUSH_OUTER_TEMP  0x62u

#define OP_SEND_ASYNC       0x70u
#define OP_ASK              0x71u
#define OP_ACTOR_SPAWN      0x72u
#define OP_SELF_ADDRESS     0x73u

/* ── Reduction quota ───────────────────────────────────────────────────── */

#define STA_REDUCTION_QUOTA 1000u

/* ── Class object slot layout (contract with Epic 4 bootstrap) ─────────── */

#define STA_CLASS_SLOT_SUPERCLASS   0
#define STA_CLASS_SLOT_METHODDICT   1
#define STA_CLASS_SLOT_FORMAT       2
#define STA_CLASS_SLOT_NAME         3

/* ── Class object accessors ────────────────────────────────────────────── */

/* Read superclass OOP from a class object. */
static inline STA_OOP sta_class_superclass(STA_OOP class_oop) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)class_oop;
    return sta_payload(h)[STA_CLASS_SLOT_SUPERCLASS];
}

/* Read method dictionary OOP from a class object. */
static inline STA_OOP sta_class_method_dict(STA_OOP class_oop) {
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)class_oop;
    return sta_payload(h)[STA_CLASS_SLOT_METHODDICT];
}

/* ── Interpreter entry point ───────────────────────────────────────────── */

/* Execute a method on a receiver with arguments.
 * Creates the initial frame, enters the dispatch loop, returns when
 * the top-level frame returns.
 *
 * The class_table is needed for message dispatch (class lookup).
 * Returns the result OOP (the value returned by the top-level method). */
STA_OOP sta_interpret(STA_StackSlab *slab, STA_Heap *heap,
                      STA_ClassTable *ct,
                      STA_OOP method, STA_OOP receiver,
                      STA_OOP *args, uint8_t nargs);
