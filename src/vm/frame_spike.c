/* src/vm/frame_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: activation frame layout and tail-call optimisation (TCO).
 * See docs/spikes/spike-004-frame-layout.md and ADR 010.
 */

#include "frame_spike.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── Slab lifecycle ────────────────────────────────────────────────────────*/

int sta_frame_slab_init(STA_FrameSlab *slab, size_t capacity) {
    slab->base       = (char *)malloc(capacity);
    slab->capacity   = capacity;
    slab->used       = 0;
    slab->high_water = 0;
    slab->depth      = 0;
    slab->max_depth  = 0;
    slab->top_frame  = NULL;
    slab->result     = STA_OOP_ZERO;
    return (slab->base != NULL) ? 0 : -1;
}

void sta_frame_slab_destroy(STA_FrameSlab *slab) {
    free(slab->base);
    slab->base      = NULL;
    slab->capacity  = 0;
    slab->used      = 0;
    slab->top_frame = NULL;
}

/* ── Frame push ────────────────────────────────────────────────────────────*/

STA_Frame *sta_frame_push(STA_FrameSlab *slab,
                           STA_Frame    *sender,
                           STA_OOP       receiver,
                           const uint8_t *method,
                           uint16_t       arg_count,
                           uint16_t       local_count,
                           const STA_OOP *args) {
    uint16_t nslots      = (uint16_t)(arg_count + local_count);
    size_t   frame_bytes = sta_frame_alloc_size(nslots);

    assert(slab->used + frame_bytes <= slab->capacity &&
           "frame slab exhausted — increase capacity or implement growth");

    STA_Frame *frame = (STA_Frame *)((char *)slab->base + slab->used);
    slab->used += frame_bytes;
    if (slab->used > slab->high_water) {
        slab->high_water = slab->used;
    }
    slab->depth++;
    if (slab->depth > slab->max_depth) {
        slab->max_depth = slab->depth;
    }

    frame->method_bytecode = method;
    frame->receiver        = receiver;
    frame->sender_fp       = sender;
    frame->pc              = 0;
    frame->arg_count       = arg_count;
    frame->local_count     = local_count;
    frame->reduction_hook  = 0;
    frame->_pad            = 0;

    STA_OOP *slots = sta_frame_slots(frame);
    for (uint16_t i = 0; i < arg_count; i++) {
        slots[i] = args[i];
    }
    for (uint16_t i = arg_count; i < nslots; i++) {
        slots[i] = STA_OOP_ZERO;
    }

    return frame;
}

/* ── Frame pop ─────────────────────────────────────────────────────────────*/

void sta_frame_pop(STA_FrameSlab *slab, STA_Frame *frame) {
    /* Retract bump pointer to reclaim this frame's memory.
     * frame MUST be the topmost allocated frame (LIFO invariant). */
    slab->used = (size_t)((char *)frame - slab->base);
    slab->depth--;
}

/* ── Tail-call update ──────────────────────────────────────────────────────*/

void sta_frame_tail_call(STA_Frame     *frame,
                          STA_OOP        new_receiver,
                          const uint8_t *new_method,
                          uint16_t       new_arg_count,
                          const STA_OOP *new_args) {
    /* Spike limitation: new_arg_count must not exceed the existing slot
     * capacity (arg_count + local_count). The production implementation must
     * handle callee methods with more locals than the current frame; see the
     * stack slab growth policy in ADR 010. */
    assert(new_arg_count <= (uint16_t)(frame->arg_count + frame->local_count) &&
           "TCO: callee needs more slots than current frame — see ADR 010");

    frame->receiver        = new_receiver;
    frame->method_bytecode = new_method;
    frame->pc              = 0;
    frame->arg_count       = new_arg_count;
    /* local_count is preserved; remaining locals retain stale values.
     * Production: zero new locals if callee's local_count > caller's. */

    STA_OOP *slots = sta_frame_slots(frame);
    for (uint16_t i = 0; i < new_arg_count; i++) {
        slots[i] = new_args[i];
    }
    /* sender_fp is intentionally unchanged — tail call inherits return address. */
}

/* ── GC stack-walk ─────────────────────────────────────────────────────────*/

void sta_frame_gc_roots(STA_Frame       *top_frame,
                         sta_gc_visit_fn  visit,
                         void            *ctx) {
    /* Walk the sender_fp chain from the innermost (current) frame to the
     * outermost (sender_fp == NULL). For each frame, visit the receiver OOP
     * and every arg/local slot.
     *
     * GC compatibility notes (ADR 007, ADR 008):
     * - Every OOP slot in every active frame is visited exactly once.
     * - The receiver is a heap OOP or SmallInt; both are valid STA_OOP values.
     * - Arg/local slots may be heap OOPs (mutable objects on this actor's
     *   nursery), SmallInts, or OOP nil — all are reported to the visitor.
     * - TCO-reused frames are walked identically to freshly-pushed frames;
     *   the sender_fp and slot counts are always current after any TCO update.
     * - No cross-actor OOP references can appear in local slots under the
     *   deep-copy semantics of ADR 008. */
    for (STA_Frame *f = top_frame; f != NULL; f = f->sender_fp) {
        visit(&f->receiver, ctx);
        STA_OOP *slots = sta_frame_slots(f);
        uint16_t nslots = (uint16_t)(f->arg_count + f->local_count);
        for (uint16_t i = 0; i < nslots; i++) {
            visit(&slots[i], ctx);
        }
    }
}

/* ── Stub dispatch loop ────────────────────────────────────────────────────*/

STA_ExecStatus sta_exec_actor(STA_FrameSlab *slab,
                                STA_Frame     *start_frame,
                                uint32_t      *reductions) {
    STA_Frame *frame   = start_frame;
    STA_OOP    eval_stack[STA_EVAL_STACK_SIZE];
    int        eval_top = 0;

    for (;;) {
        uint8_t raw_op = frame->method_bytecode[frame->pc];

        switch ((STA_Opcode)raw_op) {

        /* ── OP_HALT ─────────────────────────────────────────────────── */
        case OP_HALT:
            sta_frame_pop(slab, frame);
            slab->top_frame = NULL;
            slab->result    = (eval_top > 0) ? eval_stack[eval_top - 1]
                                              : STA_OOP_ZERO;
            return STA_EXEC_HALT;

        /* ── OP_PUSH_SELF ────────────────────────────────────────────── */
        case OP_PUSH_SELF:
            assert(eval_top < (int)STA_EVAL_STACK_SIZE);
            eval_stack[eval_top++] = frame->receiver;
            frame->pc += 1u;
            break;

        /* ── OP_PUSH_ARG ─────────────────────────────────────────────── */
        case OP_PUSH_ARG: {
            uint8_t idx = frame->method_bytecode[frame->pc + 1u];
            assert(idx < frame->arg_count);
            assert(eval_top < (int)STA_EVAL_STACK_SIZE);
            eval_stack[eval_top++] = sta_frame_slots(frame)[idx];
            frame->pc += 2u;
            break;
        }

        /* ── OP_PUSH_INT ─────────────────────────────────────────────── */
        case OP_PUSH_INT: {
            int32_t val;
            memcpy(&val, &frame->method_bytecode[frame->pc + 1u], sizeof(int32_t));
            assert(eval_top < (int)STA_EVAL_STACK_SIZE);
            eval_stack[eval_top++] = STA_SMALLINT_OOP(val);
            frame->pc += 5u;
            break;
        }

        /* ── OP_DEC ──────────────────────────────────────────────────── */
        case OP_DEC: {
            assert(eval_top > 0);
            STA_OOP top = eval_stack[eval_top - 1];
            assert(STA_IS_SMALLINT(top));
            eval_stack[eval_top - 1] = STA_SMALLINT_OOP(STA_SMALLINT_VAL(top) - 1);
            frame->pc += 1u;
            break;
        }

        /* ── OP_BRANCH_IF_ZERO ───────────────────────────────────────── */
        case OP_BRANCH_IF_ZERO: {
            int16_t offset;
            memcpy(&offset, &frame->method_bytecode[frame->pc + 1u], sizeof(int16_t));
            assert(eval_top > 0);
            STA_OOP top = eval_stack[--eval_top];
            /* Jump target: pc + sizeof(instruction=3) + offset */
            if (STA_IS_SMALLINT(top) && STA_SMALLINT_VAL(top) == 0) {
                frame->pc = (uint32_t)((int32_t)frame->pc + 3 + (int32_t)offset);
            } else {
                frame->pc += 3u;
            }
            break;
        }

        /* ── OP_SEND ─────────────────────────────────────────────────── */
        case OP_SEND: {
            uint8_t nargs = frame->method_bytecode[frame->pc + 1u];

            /* One-instruction lookahead: is the next instruction OP_RETURN_TOP?
             * (next instr is at pc + STA_SEND_WIDTH, i.e. pc + 2) */
            bool is_tail = (frame->method_bytecode[frame->pc + STA_SEND_WIDTH]
                            == (uint8_t)OP_RETURN_TOP);

            /* Pop arguments: last-pushed arg is at top of eval stack. */
            assert(eval_top >= (int)(nargs + 1u));
            STA_OOP args[STA_MAX_SEND_ARGS];
            assert(nargs <= STA_MAX_SEND_ARGS);
            for (int i = (int)nargs - 1; i >= 0; i--) {
                args[i] = eval_stack[--eval_top];
            }
            STA_OOP new_receiver = eval_stack[--eval_top];

            /* Decrement reduction counter (tail call costs the same as normal). */
            (*reductions)--;

            if (is_tail) {
                /* TCO path: reuse the current frame in-place.
                 * sender_fp is preserved — tail call inherits the return address.
                 * Spike: the callee method is always the same as the current method
                 * (self-recursive countdown). Production: method lookup goes here. */
                sta_frame_tail_call(frame, new_receiver,
                                    frame->method_bytecode, nargs, args);
                eval_top        = 0;
                slab->top_frame = frame;
            } else {
                /* Normal path: save return address, push new callee frame. */
                frame->pc += STA_SEND_WIDTH;   /* return address = after SEND */
                frame = sta_frame_push(slab, frame, new_receiver,
                                       frame->method_bytecode,
                                       nargs, 0u /* no locals in stub */,
                                       args);
                eval_top        = 0;
                slab->top_frame = frame;
            }

            /* Check preemption after committing the send.
             * The frame is in a consistent state at PC=0, so resume is safe. */
            if (*reductions == 0u) {
                return STA_EXEC_PREEMPTED;
            }
            break;
        }

        /* ── OP_RETURN_TOP ───────────────────────────────────────────── */
        case OP_RETURN_TOP: {
            STA_OOP result = (eval_top > 0) ? eval_stack[--eval_top] : STA_OOP_ZERO;

            if (frame->sender_fp == NULL) {
                /* Outermost frame: pop it, then halt. */
                sta_frame_pop(slab, frame);
                slab->top_frame = NULL;
                slab->result    = result;
                return STA_EXEC_HALT;
            }

            /* Return to caller: pop callee frame, push result onto eval stack. */
            STA_Frame *caller = frame->sender_fp;
            sta_frame_pop(slab, frame);
            frame           = caller;
            slab->top_frame = frame;
            eval_top        = 0;
            eval_stack[eval_top++] = result;
            break;
        }

        /* ── OP_NOOP ─────────────────────────────────────────────────── */
        case OP_NOOP:
            frame->pc += 1u;
            break;

        default:
            fprintf(stderr, "frame_spike: unknown opcode 0x%02x at pc=%u\n",
                    raw_op, frame->pc);
            return STA_EXEC_ERROR;
        }
    }
}
