/* src/vm/frame.c
 * Activation frame and stack slab — see frame.h for documentation.
 */
#include "frame.h"
#include "compiled_method.h"
#include "special_objects.h"
#include <stdlib.h>
#include <string.h>

/* ── Stack slab ────────────────────────────────────────────────────────── */

STA_StackSlab *sta_stack_slab_create(size_t capacity) {
    STA_StackSlab *slab = malloc(sizeof(*slab));
    if (!slab) return NULL;

    slab->base = malloc(capacity);
    if (!slab->base) { free(slab); return NULL; }

    memset(slab->base, 0, capacity);
    slab->end = slab->base + capacity;
    slab->top = slab->base;
    slab->sp  = slab->base;
    return slab;
}

void sta_stack_slab_destroy(STA_StackSlab *slab) {
    if (!slab) return;
    free(slab->base);
    free(slab);
}

/* ── Frame push ────────────────────────────────────────────────────────── */

STA_Frame *sta_frame_push(STA_StackSlab *slab, STA_OOP method,
                           STA_OOP receiver, STA_Frame *sender,
                           const STA_OOP *args, uint8_t nargs)
{
    /* Read numTemps from the method header. numTemps includes args per spec. */
    STA_OOP header = sta_method_header(method);
    uint8_t num_temps = STA_METHOD_NUM_TEMPS(header);

    /* local_count = numTemps - numArgs (temps beyond args). */
    uint16_t local_count = (num_temps > nargs) ? (uint16_t)(num_temps - nargs) : 0;
    uint32_t total_slots = (uint32_t)nargs + (uint32_t)local_count;

    /* Check space: frame struct + slots. */
    size_t frame_bytes = sizeof(STA_Frame) + total_slots * sizeof(STA_OOP);
    if (slab->top + frame_bytes > slab->end) return NULL;

    STA_Frame *frame = (STA_Frame *)slab->top;
    slab->top += frame_bytes;

    /* Initialize frame fields. */
    frame->method      = method;
    frame->receiver    = receiver;
    frame->sender      = sender;
    frame->pc          = 0;
    frame->arg_count   = nargs;
    frame->local_count = local_count;
    frame->flags       = 0;
    frame->_pad        = 0;

    STA_OOP *slots = sta_frame_slots(frame);

    /* Copy args. */
    if (nargs > 0 && args) {
        memcpy(slots, args, (size_t)nargs * sizeof(STA_OOP));
    }

    /* Zero temps (nil). Nil OOP = sta_spc_get(SPC_NIL), which may be 0. */
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    for (uint16_t i = 0; i < local_count; i++) {
        slots[nargs + i] = nil_oop;
    }

    /* Set expression stack pointer to just after the slots. */
    slab->sp = (uint8_t *)&slots[total_slots];

    return frame;
}

/* ── Frame pop ─────────────────────────────────────────────────────────── */

void sta_frame_pop(STA_StackSlab *slab, STA_Frame *frame) {
    slab->top = (uint8_t *)frame;
    /* Reset sp to the expression stack base of the sender frame, if any.
     * The caller is responsible for setting sp appropriately after pop. */
}

/* ── GC root enumeration ──────────────────────────────────────────────── */

void sta_frame_gc_roots(STA_Frame *frame, STA_StackSlab *slab,
                         STA_RootVisitor visitor, void *ctx)
{
    while (frame) {
        visitor(&frame->method, ctx);
        visitor(&frame->receiver, ctx);

        STA_OOP *slots = sta_frame_slots(frame);
        uint32_t slot_count = sta_frame_slot_count(frame);

        /* Visit args + temps. */
        for (uint32_t i = 0; i < slot_count; i++) {
            visitor(&slots[i], ctx);
        }

        /* Visit expression stack slots (only for the innermost frame,
         * since the slab->sp only tracks the current frame's stack).
         * For non-innermost frames, the expression stack is not live. */
        if (frame->sender == NULL || frame == (STA_Frame *)slab->base ||
            (uint8_t *)frame == slab->top - sizeof(STA_Frame)) {
            /* This is the innermost frame — walk expression stack. */
            STA_OOP *stack_base = &slots[slot_count];
            STA_OOP *stack_top = (STA_OOP *)slab->sp;
            for (STA_OOP *p = stack_base; p < stack_top; p++) {
                visitor(p, ctx);
            }
        }

        frame = frame->sender;
    }
}
