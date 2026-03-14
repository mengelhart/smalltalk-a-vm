/* src/vm/frame.h
 * Activation frame and stack slab — Phase 1 single-slab variant.
 * Phase 1 — permanent. See ADR 010 (frame layout), ADR 014 (slab growth).
 *
 * STA_Frame is a C struct embedded in a contiguous stack slab (Option A).
 * Payload slots (args + temps + stack) follow immediately after the struct.
 *
 * Phase 1 uses a single fixed-size slab. Phase 2 will add linked segments
 * per ADR 014.
 */
#pragma once
#include "oop.h"
#include <stdint.h>
#include <stddef.h>

/* ── Frame struct (40 bytes, ADR 010) ──────────────────────────────────── */

typedef struct STA_Frame {
    STA_OOP           method;         /* CompiledMethod OOP                 */
    STA_OOP           receiver;       /* receiver OOP (self)                */
    struct STA_Frame *sender;         /* calling frame (NULL for bottom)    */
    uint32_t          pc;             /* program counter (byte offset)      */
    uint16_t          arg_count;      /* number of argument slots           */
    uint16_t          local_count;    /* number of temp slots               */
    uint32_t          flags;          /* bit 0: marker, others reserved     */
    uint32_t          _pad;           /* align payload to 8-byte boundary   */
} STA_Frame;                          /* sizeof = 40 bytes                  */

/* ── Stack slab ────────────────────────────────────────────────────────── */

typedef struct STA_StackSlab {
    uint8_t  *base;        /* start of usable area                         */
    uint8_t  *end;         /* one past last usable byte                    */
    uint8_t  *top;         /* current bump pointer (next free byte)        */
    uint8_t  *sp;          /* stack pointer for expression stack ops       */
} STA_StackSlab;

/* Create a slab with the given capacity in bytes. Returns NULL on failure. */
STA_StackSlab *sta_stack_slab_create(size_t capacity);

/* Destroy the slab and free all memory. */
void sta_stack_slab_destroy(STA_StackSlab *slab);

/* ── Slot access ───────────────────────────────────────────────────────── */

/* Pointer to payload[0] — args, then temps, then expression stack. */
static inline STA_OOP *sta_frame_slots(STA_Frame *frame) {
    return (STA_OOP *)(frame + 1);
}

/* Total allocated slot count for a frame (args + temps). */
static inline uint32_t sta_frame_slot_count(STA_Frame *frame) {
    return (uint32_t)frame->arg_count + (uint32_t)frame->local_count;
}

/* ── Frame operations ──────────────────────────────────────────────────── */

/* Push a new frame onto the slab.
 * Reads numTemps from the method header. Allocates frame + slot space.
 * Copies args into arg slots. Initializes temps to nil. Sets pc = 0.
 * Returns NULL if the slab is full. */
STA_Frame *sta_frame_push(STA_StackSlab *slab, STA_OOP method,
                           STA_OOP receiver, STA_Frame *sender,
                           const STA_OOP *args, uint8_t nargs);

/* Pop a frame — reclaim space by retracting the bump pointer. */
void sta_frame_pop(STA_StackSlab *slab, STA_Frame *frame);

/* ── Expression stack operations (within a frame) ──────────────────────── */

/* Push an OOP onto the expression stack. */
static inline void sta_stack_push(STA_StackSlab *slab, STA_OOP oop) {
    *(STA_OOP *)slab->sp = oop;
    slab->sp += sizeof(STA_OOP);
}

/* Pop an OOP from the expression stack. */
static inline STA_OOP sta_stack_pop(STA_StackSlab *slab) {
    slab->sp -= sizeof(STA_OOP);
    return *(STA_OOP *)slab->sp;
}

/* Peek at the top of the expression stack without popping. */
static inline STA_OOP sta_stack_peek(STA_StackSlab *slab) {
    return *(STA_OOP *)(slab->sp - sizeof(STA_OOP));
}

/* Current expression stack depth (in OOP-sized elements). */
static inline uint32_t sta_stack_depth(STA_StackSlab *slab,
                                        STA_Frame *frame) {
    uint8_t *stack_base = (uint8_t *)&sta_frame_slots(frame)[
        sta_frame_slot_count(frame)];
    return (uint32_t)((slab->sp - stack_base) / sizeof(STA_OOP));
}

/* ── GC root enumeration ──────────────────────────────────────────────── */

typedef void (*STA_RootVisitor)(STA_OOP *slot, void *ctx);

/* Walk all live OOP slots in the frame chain: method, receiver,
 * args, temps, and expression stack up to sp. */
void sta_frame_gc_roots(STA_Frame *frame, STA_StackSlab *slab,
                         STA_RootVisitor visitor, void *ctx);
