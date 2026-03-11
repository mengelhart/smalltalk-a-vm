/* src/vm/frame_spike.h
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: activation frame layout and tail-call optimisation (TCO).
 * See docs/spikes/spike-004-frame-layout.md and ADR 010.
 *
 * Two frame layout options are defined here for sizeof comparison:
 *   STA_Frame    — Option A: plain C struct in a contiguous per-actor stack slab
 *   STA_FrameAlt — Option B: heap-allocated STA_ObjHeader object (for comparison only)
 *
 * Option A is used throughout the spike implementation.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>

#include "oop_spike.h"   /* STA_OOP, STA_ObjHeader, STA_SMALLINT_OOP, etc. */

/* ── Option A: frame as a plain C struct ───────────────────────────────────
 *
 * Layout (arm64, C17):
 *   method_bytecode  offset  0  8 bytes  pointer to bytecode array
 *   receiver         offset  8  8 bytes  receiver OOP (self)
 *   sender_fp        offset 16  8 bytes  pointer to sender frame (null = outermost)
 *   pc               offset 24  4 bytes  current bytecode offset
 *   arg_count        offset 28  2 bytes  number of argument slots
 *   local_count      offset 30  2 bytes  number of local variable slots
 *   reduction_hook   offset 32  4 bytes  reserved for scheduler integration (ADR 009)
 *   _pad             offset 36  4 bytes  pad to multiple of 8 bytes
 *                    total     40 bytes
 *
 * Payload slots follow immediately after the struct (at frame + 1):
 *   slots[0..arg_count-1]              — argument OOPs
 *   slots[arg_count..arg_count+local_count-1] — local variable OOPs
 *
 * Payload is 8-byte aligned: sizeof(STA_Frame) = 40 (multiple of 8), and
 * the slab base is malloc-aligned (≥ 16 bytes). Each frame's total allocation
 * is sizeof(STA_Frame) + (arg_count + local_count) * sizeof(STA_OOP), always
 * a multiple of 8.
 */
typedef struct STA_Frame {
    const uint8_t    *method_bytecode; /* 8 — pointer to bytecode array        */
    STA_OOP           receiver;        /* 8 — receiver OOP (self)              */
    struct STA_Frame *sender_fp;       /* 8 — sender frame (null = outermost)  */
    uint32_t          pc;              /* 4 — current bytecode offset          */
    uint16_t          arg_count;       /* 2 — number of argument slots         */
    uint16_t          local_count;     /* 2 — number of local variable slots   */
    uint32_t          reduction_hook;  /* 4 — reserved for scheduler (ADR 009) */
    uint32_t          _pad;            /* 4 — align payload to 8-byte boundary */
} STA_Frame;                           /* sizeof = 40 bytes                    */

/* Inline payload accessor: first OOP slot immediately after the frame header. */
static inline STA_OOP *sta_frame_slots(STA_Frame *f) {
    return (STA_OOP *)(f + 1);
}

/* Total bytes to allocate for a frame with n_slots arg+local OOPs. */
static inline size_t sta_frame_alloc_size(uint16_t n_slots) {
    return sizeof(STA_Frame) + (size_t)n_slots * sizeof(STA_OOP);
}

/* ── Option B: frame as a heap-allocated STA_ObjHeader object ──────────────
 *
 * Defined here for sizeof comparison only — not used in the spike
 * implementation. The OOP payload slots (method, receiver, sender_fp, pc,
 * arg_count, local_count, reduction_hook) follow the header, with additional
 * arg and local slots after them.
 *
 * The compiler inserts 4 bytes of implicit padding between hdr (12 bytes)
 * and the first OOP field (requires 8-byte alignment), making the effective
 * header allocation 16 bytes — identical to STA_HEADER_ALIGNED.
 *
 * sizeof(STA_FrameAlt) = 16 (hdr + pad) + 7 * 8 (fixed OOP fields) = 72 bytes
 * for a frame with zero args and locals.
 */
typedef struct {
    STA_ObjHeader hdr;             /* 12 bytes + 4 implicit pad = 16 alloc unit */
    /* OOP-typed payload at offset 16 (= STA_HEADER_ALIGNED): */
    STA_OOP       method_oop;      /* [0] compiled method OOP              */
    STA_OOP       receiver;        /* [1] self                             */
    STA_OOP       sender_fp_oop;   /* [2] sender frame OOP (nil = none)    */
    STA_OOP       pc_word;         /* [3] SmallInt bytecode offset         */
    STA_OOP       arg_count_word;  /* [4] SmallInt arg count               */
    STA_OOP       local_count_word;/* [5] SmallInt local count             */
    STA_OOP       reduction_hook;  /* [6] SmallInt (reserved)              */
    /* variable: STA_OOP arg_and_locals[] follow in heap allocation         */
} STA_FrameAlt;
/* sizeof(STA_FrameAlt) should be 72 bytes; verified by spike test. */

/* ── Stack slab ────────────────────────────────────────────────────────────
 *
 * Contiguous malloc-backed buffer serving as the actor's activation stack.
 * Frames are bump-allocated from the bottom (base) upward. Deallocation is
 * a LIFO bump-pointer retract — the call stack is strictly LIFO for all
 * non-closure sends. (Closures with non-local returns are a Phase 1 concern.)
 *
 * Spike limitation: fixed-capacity slab; no growth or chaining.
 * Production implementation: see ADR 010 for growth policy.
 */
typedef struct {
    char      *base;        /* malloc-backed buffer (≥ 16-byte aligned)   */
    size_t     capacity;    /* total buffer size in bytes                  */
    size_t     used;        /* bytes currently in use (bump pointer)       */
    size_t     high_water;  /* maximum bytes ever used (for density report)*/
    uint32_t   depth;       /* current frame depth                         */
    uint32_t   max_depth;   /* maximum frame depth ever reached            */
    STA_Frame *top_frame;   /* current executing frame (valid after PREEMPTED) */
    STA_OOP    result;      /* final return value (valid after HALT)       */
} STA_FrameSlab;

/* ── Executor status ───────────────────────────────────────────────────────*/
typedef enum {
    STA_EXEC_HALT,       /* outermost frame returned; slab->result is valid */
    STA_EXEC_PREEMPTED,  /* reduction quota exhausted; slab->top_frame is valid */
    STA_EXEC_ERROR,      /* unrecognised opcode or stack underflow          */
} STA_ExecStatus;

/* ── Stub bytecode opcodes ─────────────────────────────────────────────────
 *
 * Minimal opcode set sufficient to prototype a tail-recursive countdown.
 * Instruction encodings:
 *   OP_HALT           1 byte
 *   OP_PUSH_SELF      1 byte
 *   OP_PUSH_ARG       2 bytes: opcode + uint8_t arg_index
 *   OP_PUSH_INT       5 bytes: opcode + int32_t value (little-endian)
 *   OP_DEC            1 byte  (decrement TOS SmallInt by 1)
 *   OP_BRANCH_IF_ZERO 3 bytes: opcode + int16_t offset (from end of instr)
 *   OP_SEND           2 bytes: opcode + uint8_t arg_count
 *   OP_RETURN_TOP     1 byte
 *   OP_NOOP           1 byte  (used to break TCO detection in non-tail variant)
 */
typedef enum {
    OP_HALT           = 0x00,
    OP_PUSH_SELF      = 0x01,
    OP_PUSH_ARG       = 0x02,
    OP_PUSH_INT       = 0x03,
    OP_DEC            = 0x04,
    OP_BRANCH_IF_ZERO = 0x05,
    OP_SEND           = 0x06,
    OP_RETURN_TOP     = 0x07,
    OP_NOOP           = 0x08,
} STA_Opcode;

/* Width of the SEND instruction in bytes (opcode + nargs operand). */
#define STA_SEND_WIDTH   2u

/* Evaluation stack size (per sta_exec_actor call; stack-allocated). */
#define STA_EVAL_STACK_SIZE  16u

/* Maximum arguments per stub send. */
#define STA_MAX_SEND_ARGS    8u

/* ── GC stack-walk callback ────────────────────────────────────────────────*/
/* Called once per live OOP slot in the frame chain.
 * slot: address of the STA_OOP field (writable — GC may update for relocation)
 * ctx:  caller-provided context pointer                                     */
typedef void (*sta_gc_visit_fn)(STA_OOP *slot, void *ctx);

/* ── Function declarations ─────────────────────────────────────────────────*/

/* Slab lifecycle */
int  sta_frame_slab_init   (STA_FrameSlab *slab, size_t capacity);
void sta_frame_slab_destroy(STA_FrameSlab *slab);

/* Frame push: carve a new frame from the slab, link it to sender.
 * args[0..arg_count-1] are copied into the new frame's argument slots.
 * Local slots are zero-initialised.
 * Returns NULL if the slab is full (spike: assert-fails in debug). */
STA_Frame *sta_frame_push(STA_FrameSlab *slab,
                           STA_Frame    *sender,
                           STA_OOP       receiver,
                           const uint8_t *method,
                           uint16_t       arg_count,
                           uint16_t       local_count,
                           const STA_OOP *args);

/* Frame pop: reclaim the topmost frame by retracting the slab bump pointer.
 * frame MUST be the topmost frame (LIFO invariant). */
void sta_frame_pop(STA_FrameSlab *slab, STA_Frame *frame);

/* Tail-call update: reuse frame in-place for a self-recursive tail call.
 * Updates receiver, method, pc (→ 0), arg_count, and argument slots.
 * sender_fp and local_count are preserved.
 * Spike limitation: new_arg_count must be <= existing slot capacity.
 * Production: see ADR 010 for the general policy. */
void sta_frame_tail_call(STA_Frame     *frame,
                          STA_OOP        new_receiver,
                          const uint8_t *new_method,
                          uint16_t       new_arg_count,
                          const STA_OOP *new_args);

/* GC stack-walk: visit every live OOP slot reachable from top_frame.
 * Walks the sender_fp chain to the outermost frame (sender_fp == NULL).
 * For each frame: visits receiver, then arg and local slots in index order.
 * Compatible with ADR 007 OOP scheme and ADR 008 per-actor heap isolation. */
void sta_frame_gc_roots(STA_Frame       *top_frame,
                         sta_gc_visit_fn  visit,
                         void            *ctx);

/* Stub dispatch loop with TCO and reduction-based preemption.
 *
 * Executes bytecodes starting from start_frame->pc, using a local
 * evaluation stack. TCO is detected by one-instruction lookahead:
 * if bytecode[pc + STA_SEND_WIDTH] == OP_RETURN_TOP, the SEND is a tail
 * call and sta_frame_tail_call() is used instead of sta_frame_push().
 *
 * The reduction counter (*reductions) is decremented once per SEND,
 * regardless of whether it is a tail call. When it reaches zero,
 * slab->top_frame is set to the current frame and STA_EXEC_PREEMPTED
 * is returned. The caller resets *reductions and calls again to resume.
 *
 * On STA_EXEC_HALT: slab->result holds the final return value.
 * On STA_EXEC_PREEMPTED: slab->top_frame is the frame to resume from. */
STA_ExecStatus sta_exec_actor(STA_FrameSlab *slab,
                                STA_Frame     *start_frame,
                                uint32_t      *reductions);
