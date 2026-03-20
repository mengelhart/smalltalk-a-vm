/* src/vm/interpreter.c
 * Bytecode interpreter — Phase 2 dispatch loop.
 * See interpreter.h, bytecode spec §§2–5, 10, ADR 010, ADR 014.
 */
#include "interpreter.h"
#include "vm_state.h"
#include "actor/actor.h"
#include "compiled_method.h"
#include "selector.h"
#include "primitive_table.h"
#include "method_dict.h"
#include "special_objects.h"
#include "handler.h"
#include "class_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── BlockClosure slot layout ──────────────────────────────────────────── */
/* Clean block (OP_BLOCK_COPY):  5 slots [startPC, numArgs, method, receiver, tempOffset]
 * Closure    (OP_CLOSURE_COPY): 6 slots [startPC, numArgs, method, receiver, tempOffset, context] */
#define BC_SLOT_START_PC    0
#define BC_SLOT_NUM_ARGS    1
#define BC_SLOT_METHOD      2
#define BC_SLOT_RECEIVER    3
#define BC_SLOT_TEMP_OFFSET 4
#define BC_SLOT_CONTEXT     5   /* only present in closures (6-slot BlockClosure) */

/* ── Internal helpers ──────────────────────────────────────────────────── */

static uint32_t oop_class_index(STA_OOP oop) {
    if (STA_IS_SMALLINT(oop)) return STA_CLS_SMALLINTEGER;
    if (STA_IS_CHAR(oop))     return STA_CLS_CHARACTER;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)oop;
    return h->class_index;
}

static STA_OOP method_lookup(STA_ClassTable *ct, uint32_t class_index,
                              STA_OOP selector) {
    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
    STA_OOP cls = sta_class_table_get(ct, class_index);
    while (cls != 0 && cls != nil_oop) {
        STA_OOP md = sta_class_method_dict(cls);
        if (md != 0) {
            STA_OOP method = sta_method_dict_lookup(md, selector);
            if (method != 0) return method;
        }
        cls = sta_class_superclass(cls);
    }
    return 0;
}

static size_t frame_byte_size(uint16_t arg_count, uint16_t local_count) {
    return sizeof(STA_Frame) +
           ((size_t)arg_count + (size_t)local_count) * sizeof(STA_OOP);
}

/* Get the effective slots pointer for a frame.
 * If the frame has a context, slots point to the context object's payload.
 * Otherwise, slots are the inline frame slots. */
static inline STA_OOP *effective_slots(STA_Frame *frame) {
    if (frame->context != 0) {
        STA_ObjHeader *ctx_h = (STA_ObjHeader *)(uintptr_t)frame->context;
        return sta_payload(ctx_h);
    }
    return sta_frame_slots(frame);
}

/* Sentinel OOP returned by interpret_loop_ex when preempted.
 * Uses an odd non-SmallInt bit pattern that can never be a valid OOP:
 * bit 0 set (tagged), but bits 1..63 all set — no valid SmallInt equals this. */
#define STA_OOP_PREEMPTED  ((STA_OOP)UINTPTR_MAX)

/* ── Dispatch loop ─────────────────────────────────────────────────────── */

/* sched_actor: if non-NULL, the interpreter runs on this actor's resources
 * and supports preemption. If NULL, runs on root_actor (sta_eval path,
 * no preemption). */
static STA_OOP interpret_loop_ex(STA_VM *vm, STA_Frame *frame,
                                  struct STA_Actor *sched_actor)
{
    /* Resolve slab and heap. Scheduled actors use their own resources;
     * the sta_eval path uses root_actor (or VM fallback for bootstrap). */
    struct STA_Actor *actor = sched_actor ? sched_actor : vm->root_actor;
    STA_StackSlab *slab = actor ? &actor->slab : &vm->slab;
    STA_Heap *heap = actor ? &actor->heap : &vm->heap;
    STA_ClassTable *ct = &vm->class_table;

    /* Construct execution context on the stack — passed to every primitive. */
    STA_ExecContext exec_ctx = { .vm = vm, .actor = actor };

    /* GC_SAVE_FRAME: save the current frame on the actor before any operation
     * that may trigger GC (allocation). This allows the GC to walk the frame
     * chain for root enumeration. After the allocation, saved_frame is stale
     * but harmless — it's only read during GC. */
#define GC_SAVE_FRAME() do { if (actor) actor->saved_frame = frame; } while (0)

    uint32_t reductions = 0;
    STA_OOP result = 0;

    const uint8_t *bytecodes = sta_method_bytecodes(frame->method);
    STA_OOP *slots = effective_slots(frame);

    while (frame != NULL) {
        uint8_t opcode  = bytecodes[frame->pc];
        uint16_t operand = bytecodes[frame->pc + 1];
        uint8_t is_wide = 0;

        if (opcode == OP_WIDE) {
            uint16_t high = operand;
            opcode  = bytecodes[frame->pc + 2];
            operand = (high << 8) | bytecodes[frame->pc + 3];
            is_wide = 1;
        }

        uint32_t insn_len = is_wide ? 4u : 2u;

        switch (opcode) {

        case OP_NOP:
            frame->pc += insn_len;
            break;

        case OP_PUSH_RECEIVER:
            sta_stack_push(slab, frame->receiver);
            frame->pc += insn_len;
            break;

        case OP_PUSH_NIL:
            sta_stack_push(slab, sta_spc_get(SPC_NIL));
            frame->pc += insn_len;
            break;

        case OP_PUSH_TRUE:
            sta_stack_push(slab, sta_spc_get(SPC_TRUE));
            frame->pc += insn_len;
            break;

        case OP_PUSH_FALSE:
            sta_stack_push(slab, sta_spc_get(SPC_FALSE));
            frame->pc += insn_len;
            break;

        case OP_PUSH_LIT:
            sta_stack_push(slab, sta_method_literal(frame->method, (uint8_t)operand));
            frame->pc += insn_len;
            break;

        case OP_PUSH_TEMP:
            sta_stack_push(slab, slots[operand]);
            frame->pc += insn_len;
            break;

        case OP_PUSH_INSTVAR: {
            STA_ObjHeader *recv_h = (STA_ObjHeader *)(uintptr_t)frame->receiver;
            sta_stack_push(slab, sta_payload(recv_h)[operand]);
            frame->pc += insn_len;
            break;
        }

        case OP_PUSH_GLOBAL: {
            STA_OOP assoc = sta_method_literal(frame->method, (uint8_t)operand);
            STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)assoc;
            sta_stack_push(slab, sta_payload(ah)[1]);
            frame->pc += insn_len;
            break;
        }

        case OP_PUSH_SMALLINT:
            sta_stack_push(slab, STA_SMALLINT_OOP((intptr_t)operand));
            frame->pc += insn_len;
            break;

        case OP_PUSH_MINUS_ONE:
            sta_stack_push(slab, STA_SMALLINT_OOP(-1));
            frame->pc += insn_len;
            break;

        case OP_PUSH_ZERO:
            sta_stack_push(slab, STA_SMALLINT_OOP(0));
            frame->pc += insn_len;
            break;

        case OP_PUSH_ONE:
            sta_stack_push(slab, STA_SMALLINT_OOP(1));
            frame->pc += insn_len;
            break;

        case OP_PUSH_TWO:
            sta_stack_push(slab, STA_SMALLINT_OOP(2));
            frame->pc += insn_len;
            break;

        case OP_STORE_TEMP:
            slots[operand] = sta_stack_peek(slab);
            frame->pc += insn_len;
            break;

        case OP_STORE_INSTVAR: {
            STA_ObjHeader *recv_h = (STA_ObjHeader *)(uintptr_t)frame->receiver;
            sta_payload(recv_h)[operand] = sta_stack_peek(slab);
            frame->pc += insn_len;
            break;
        }

        case OP_STORE_GLOBAL: {
            STA_OOP assoc = sta_method_literal(frame->method, (uint8_t)operand);
            STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)assoc;
            sta_payload(ah)[1] = sta_stack_peek(slab);
            frame->pc += insn_len;
            break;
        }

        case OP_POP_STORE_TEMP:
            slots[operand] = sta_stack_pop(slab);
            frame->pc += insn_len;
            break;

        case OP_POP_STORE_INSTVAR: {
            STA_ObjHeader *recv_h = (STA_ObjHeader *)(uintptr_t)frame->receiver;
            sta_payload(recv_h)[operand] = sta_stack_pop(slab);
            frame->pc += insn_len;
            break;
        }

        case OP_POP_STORE_GLOBAL: {
            STA_OOP assoc = sta_method_literal(frame->method, (uint8_t)operand);
            STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)assoc;
            sta_payload(ah)[1] = sta_stack_pop(slab);
            frame->pc += insn_len;
            break;
        }

        case OP_SEND:
        case OP_SEND_SUPER: {
            /* Preemption check: if reduction quota exhausted and we're
             * running a scheduled actor, save state and yield.
             * PC still points to this send — on resume, it retries. */
            reductions++;
            if (sched_actor && reductions >= STA_REDUCTION_QUOTA) {
                sched_actor->saved_frame = frame;
                return STA_OOP_PREEMPTED;
            }
            if (reductions >= STA_REDUCTION_QUOTA) reductions = 0;

            STA_OOP selector = sta_method_literal(frame->method, (uint8_t)operand);
            uint8_t arity = sta_selector_arity(selector);

            STA_OOP send_args[256];
            for (int i = arity - 1; i >= 0; i--)
                send_args[i] = sta_stack_pop(slab);
            STA_OOP send_receiver = sta_stack_pop(slab);

            STA_OOP found_method = 0;
            if (opcode == OP_SEND_SUPER) {
                STA_OOP cur_header = sta_method_header(frame->method);
                uint8_t cur_nlits = STA_METHOD_NUM_LITERALS(cur_header);
                STA_OOP owner_cls = sta_method_literal(frame->method, cur_nlits - 1);
                STA_OOP super_cls = sta_class_superclass(owner_cls);
                STA_OOP nil_oop_ss = sta_spc_get(SPC_NIL);
                STA_OOP cls = super_cls;
                while (cls != 0 && cls != nil_oop_ss) {
                    STA_OOP md = sta_class_method_dict(cls);
                    if (md != 0) {
                        found_method = sta_method_dict_lookup(md, selector);
                        if (found_method != 0) break;
                    }
                    cls = sta_class_superclass(cls);
                }
            } else {
                uint32_t lookup_cls_idx = oop_class_index(send_receiver);
                found_method = method_lookup(ct, lookup_cls_idx, selector);
            }

            if (found_method == 0) {
                STA_OOP dnu_sel = sta_spc_get(SPC_DOES_NOT_UNDERSTAND);
                if (dnu_sel != 0) {
                    uint32_t recv_cls_idx = oop_class_index(send_receiver);
                    STA_OOP dnu_method = method_lookup(ct, recv_cls_idx, dnu_sel);
                    if (dnu_method != 0) {
                        found_method = dnu_method;
                        /* GC safety: push send_receiver + send_args back
                         * onto the expression stack to root them across
                         * the Message/Array allocations below. */
                        sta_stack_push(slab, send_receiver);
                        for (uint8_t i = 0; i < arity; i++)
                            sta_stack_push(slab, send_args[i]);
                        GC_SAVE_FRAME();
                        STA_ObjHeader *msg_h = sta_heap_alloc(heap, STA_CLS_MESSAGE, 3);
                        STA_ObjHeader *arr_h = msg_h
                            ? sta_heap_alloc(heap, STA_CLS_ARRAY, arity)
                            : NULL;
                        /* Re-read rooted args from expression stack. */
                        for (int i = (int)arity - 1; i >= 0; i--)
                            send_args[i] = sta_stack_pop(slab);
                        send_receiver = sta_stack_pop(slab);
                        if (msg_h) {
                            STA_OOP *msg_slots = sta_payload(msg_h);
                            msg_slots[0] = selector; /* Symbol — immutable */
                            if (arr_h) {
                                STA_OOP *arr_slots = sta_payload(arr_h);
                                for (uint8_t i = 0; i < arity; i++)
                                    arr_slots[i] = send_args[i];
                                msg_slots[1] = (STA_OOP)(uintptr_t)arr_h;
                            } else {
                                msg_slots[1] = sta_spc_get(SPC_NIL);
                            }
                            msg_slots[2] = sta_class_table_get(ct, recv_cls_idx);
                            send_args[0] = (STA_OOP)(uintptr_t)msg_h;
                        } else {
                            send_args[0] = selector;
                        }
                        arity = 1;
                    }
                }
                if (found_method == 0) {
                    size_t sel_len;
                    const char *sel_str = sta_symbol_get_bytes(selector, &sel_len);
                    fprintf(stderr, "FATAL: doesNotUnderstand: #%.*s\n",
                            (int)sel_len, sel_str);
                    abort();
                }
            }

            frame->pc += insn_len;

            int is_tail = (bytecodes[frame->pc] == OP_RETURN_TOP);

            STA_OOP mh = sta_method_header(found_method);
            uint8_t callee_has_prim = STA_METHOD_HAS_PRIM(mh);
            uint16_t callee_prim_idx = (uint16_t)STA_METHOD_PRIM_INDEX(mh);
            uint8_t callee_nargs = STA_METHOD_NUM_ARGS(mh);
            uint8_t callee_ntemps = STA_METHOD_NUM_TEMPS(mh);
            uint16_t callee_locals = (callee_ntemps > callee_nargs)
                                      ? (uint16_t)(callee_ntemps - callee_nargs)
                                      : 0;

            int prim_failed = 0;
            int prim_fail_code = 0;
            if (callee_has_prim) {
                uint16_t prim_idx = callee_prim_idx;
                if (callee_prim_idx == 0) {
                    const uint8_t *callee_bc = sta_method_bytecodes(found_method);
                    if (callee_bc[0] == OP_WIDE) {
                        prim_idx = ((uint16_t)callee_bc[1] << 8) | callee_bc[3];
                    } else if (callee_bc[0] == OP_PRIMITIVE) {
                        prim_idx = callee_bc[1];
                    }
                }
                /* Block activation: prims 81-84. */
                if (prim_idx >= 81 && prim_idx <= 84 &&
                    STA_IS_HEAP(send_receiver) &&
                    ((STA_ObjHeader *)(uintptr_t)send_receiver)->class_index
                        == STA_CLS_BLOCKCLOSURE) {
                    STA_ObjHeader *bc_h = (STA_ObjHeader *)(uintptr_t)send_receiver;
                    STA_OOP *bc_slots = sta_payload(bc_h);
                    uint32_t start_pc = (uint32_t)STA_SMALLINT_VAL(bc_slots[BC_SLOT_START_PC]);
                    STA_OOP home_method = bc_slots[BC_SLOT_METHOD];
                    uint32_t temp_off = (uint32_t)STA_SMALLINT_VAL(bc_slots[BC_SLOT_TEMP_OFFSET]);

                    /* Check if this is a closure (6-slot) with context. */
                    bool has_ctx = (bc_h->size >= 6 && bc_slots[BC_SLOT_CONTEXT] != 0);

                    STA_Frame *block_frame = sta_frame_push(slab, home_method,
                        frame->receiver, frame, NULL, 0);
                    if (!block_frame) {
                        fprintf(stderr, "FATAL: stack overflow in block activation\n");
                        abort();
                    }
                    block_frame->pc = start_pc;

                    if (has_ctx) {
                        /* Closure block: share the context object.
                         * Args go into the context at tempOffset. */
                        block_frame->context = bc_slots[BC_SLOT_CONTEXT];
                        STA_OOP *ctx_slots = sta_payload(
                            (STA_ObjHeader *)(uintptr_t)block_frame->context);
                        for (uint8_t i = 0; i < arity; i++)
                            ctx_slots[temp_off + i] = send_args[i];
                    } else {
                        /* Clean block: args go into inline frame slots. */
                        STA_OOP *blk_slots = sta_frame_slots(block_frame);
                        for (uint8_t i = 0; i < arity; i++)
                            blk_slots[temp_off + i] = send_args[i];
                    }
                    frame = block_frame;
                    bytecodes = sta_method_bytecodes(frame->method);
                    slots = effective_slots(frame);
                    break;
                }

                STA_OOP prim_args_buf[257];
                prim_args_buf[0] = send_receiver;
                for (uint8_t i = 0; i < arity; i++)
                    prim_args_buf[1 + i] = send_args[i];
                STA_OOP prim_result;
                GC_SAVE_FRAME();
                STA_PrimFn fn = (prim_idx < STA_PRIM_TABLE_SIZE)
                                 ? sta_primitives[prim_idx] : NULL;
                if (fn) {
                    int rc = fn(&exec_ctx, prim_args_buf, arity, &prim_result);
                    if (rc == 0) {
                        sta_stack_push(slab, prim_result);
                        break;
                    }
                    prim_failed = 1;
                    prim_fail_code = rc;
                } else {
                    prim_failed = 1;
                    prim_fail_code = STA_PRIM_NOT_AVAILABLE;
                }
            }

            /* Does the callee need a context? (largeFrame bit = needsContext) */
            int callee_needs_ctx = STA_METHOD_LARGE_FRAME(mh);

            /* TCO check — skip TCO for context methods (context lifecycle
             * is too complex to handle in frame reuse). */
            if (is_tail && frame->sender != NULL && !callee_needs_ctx) {
                size_t cur_size = frame_byte_size(frame->arg_count, frame->local_count);
                size_t new_size = frame_byte_size(arity, callee_locals);
                if (new_size <= cur_size) {
                    frame->method = found_method;
                    frame->receiver = send_receiver;
                    frame->pc = 0;
                    frame->arg_count = arity;
                    frame->local_count = callee_locals;
                    frame->context = 0;

                    STA_OOP *new_slots = sta_frame_slots(frame);
                    for (uint8_t i = 0; i < arity; i++)
                        new_slots[i] = send_args[i];
                    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
                    for (uint16_t i = 0; i < callee_locals; i++)
                        new_slots[arity + i] = nil_oop;

                    if (prim_failed && callee_locals > 0)
                        new_slots[arity] = STA_SMALLINT_OOP(prim_fail_code);

                    slab->sp = (uint8_t *)&new_slots[arity + callee_locals];
                    bytecodes = sta_method_bytecodes(frame->method);
                    slots = new_slots;

                    if (callee_has_prim)
                        frame->pc = (callee_prim_idx != 0) ? 2u : 4u;
                    break;
                }
            }

            /* Normal frame push. */
            STA_Frame *new_frame = sta_frame_push(slab, found_method,
                send_receiver, frame, send_args, arity);
            if (!new_frame) {
                fprintf(stderr, "FATAL: stack overflow\n");
                abort();
            }

            /* If the callee needs a context, allocate one on the heap.
             * All temps (args + locals) go in the context. The inline
             * slab slots remain allocated to provide expression stack space. */
            if (callee_needs_ctx) {
                uint32_t ctx_size = (uint32_t)arity + (uint32_t)callee_locals;
                GC_SAVE_FRAME();
                STA_ObjHeader *ctx_h = sta_heap_alloc(heap, STA_CLS_ARRAY, ctx_size);
                if (!ctx_h) {
                    fprintf(stderr, "FATAL: failed to allocate context object\n");
                    abort();
                }
                STA_OOP *ctx_slots = sta_payload(ctx_h);
                /* Copy args and temps from inline slots into context. */
                STA_OOP *inline_slots = sta_frame_slots(new_frame);
                for (uint32_t ci = 0; ci < ctx_size; ci++)
                    ctx_slots[ci] = inline_slots[ci];
                new_frame->context = (STA_OOP)(uintptr_t)ctx_h;
                /* Mark as home method frame for NLR targeting. */
                new_frame->flags |= STA_FRAME_FLAG_MARKER;
            }

            frame = new_frame;
            bytecodes = sta_method_bytecodes(frame->method);
            slots = effective_slots(frame);

            if (prim_failed && frame->local_count > 0)
                slots[frame->arg_count] = STA_SMALLINT_OOP(prim_fail_code);

            if (callee_has_prim)
                frame->pc = (callee_prim_idx != 0) ? 2u : 4u;
            break;
        }

        case OP_RETURN_TOP:
            result = sta_stack_pop(slab);
            goto do_return;

        case OP_RETURN_SELF:
            result = frame->receiver;
            goto do_return;

        case OP_RETURN_NIL:
            result = sta_spc_get(SPC_NIL);
            goto do_return;

        case OP_RETURN_TRUE:
            result = sta_spc_get(SPC_TRUE);
            goto do_return;

        case OP_RETURN_FALSE:
            result = sta_spc_get(SPC_FALSE);
            goto do_return;

        do_return: {
            STA_Frame *sender = frame->sender;
            sta_frame_pop(slab, frame);
            frame = sender;
            if (frame != NULL) {
                bytecodes = sta_method_bytecodes(frame->method);
                slots = effective_slots(frame);
                sta_stack_push(slab, result);
            }
            break;
        }

        case OP_JUMP:
            frame->pc += insn_len + operand;
            break;

        case OP_JUMP_TRUE: {
            STA_OOP val = sta_stack_pop(slab);
            if (val == sta_spc_get(SPC_TRUE))
                frame->pc += insn_len + operand;
            else if (val == sta_spc_get(SPC_FALSE))
                frame->pc += insn_len;
            else { fprintf(stderr, "FATAL: mustBeBoolean in JUMP_TRUE\n"); abort(); }
            break;
        }

        case OP_JUMP_FALSE: {
            STA_OOP val = sta_stack_pop(slab);
            if (val == sta_spc_get(SPC_FALSE))
                frame->pc += insn_len + operand;
            else if (val == sta_spc_get(SPC_TRUE))
                frame->pc += insn_len;
            else { fprintf(stderr, "FATAL: mustBeBoolean in JUMP_FALSE\n"); abort(); }
            break;
        }

        case OP_JUMP_BACK:
            frame->pc = frame->pc + insn_len - operand;
            reductions++;
            if (sched_actor && reductions >= STA_REDUCTION_QUOTA) {
                sched_actor->saved_frame = frame;
                return STA_OOP_PREEMPTED;
            }
            if (reductions >= STA_REDUCTION_QUOTA) reductions = 0;
            break;

        case OP_POP:
            (void)sta_stack_pop(slab);
            frame->pc += insn_len;
            break;

        case OP_DUP:
            sta_stack_push(slab, sta_stack_peek(slab));
            frame->pc += insn_len;
            break;

        case OP_PRIMITIVE: {
            uint16_t prim_idx = operand;
            GC_SAVE_FRAME();
            STA_PrimFn fn = (prim_idx < STA_PRIM_TABLE_SIZE)
                             ? sta_primitives[prim_idx] : NULL;
            if (fn) {
                uint8_t na = frame->arg_count;
                STA_OOP prim_args[257];
                prim_args[0] = frame->receiver;
                for (uint8_t i = 0; i < na; i++)
                    prim_args[1 + i] = slots[i];
                STA_OOP prim_result;
                int rc = fn(&exec_ctx, prim_args, na, &prim_result);
                if (rc == 0) {
                    sta_stack_push(slab, prim_result);
                } else {
                    if (frame->local_count > 0)
                        slots[frame->arg_count] = STA_SMALLINT_OOP(rc);
                }
            }
            frame->pc += insn_len;
            break;
        }

        case OP_BLOCK_COPY: {
            /* Clean block (no captures). BlockClosure: 5 slots. */
            STA_OOP desc = sta_method_literal(frame->method, (uint8_t)operand);
            STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)desc;
            STA_OOP *dp = sta_payload(dh);
            STA_OOP start_pc_oop  = dp[0];
            STA_OOP body_len_oop  = dp[1];
            STA_OOP num_args_oop  = dp[2];
            STA_OOP temp_off_oop  = dp[3];
            uint32_t blk_start  = (uint32_t)STA_SMALLINT_VAL(start_pc_oop);
            uint32_t blk_length = (uint32_t)STA_SMALLINT_VAL(body_len_oop);

            GC_SAVE_FRAME();
            STA_ObjHeader *bc_h = sta_heap_alloc(heap, STA_CLS_BLOCKCLOSURE, 5);
            if (!bc_h) {
                fprintf(stderr, "FATAL: failed to allocate BlockClosure\n");
                abort();
            }
            STA_OOP *bc_s = sta_payload(bc_h);
            bc_s[BC_SLOT_START_PC]    = start_pc_oop;
            bc_s[BC_SLOT_NUM_ARGS]    = num_args_oop;
            bc_s[BC_SLOT_METHOD]      = frame->method;
            bc_s[BC_SLOT_RECEIVER]    = frame->receiver;
            bc_s[BC_SLOT_TEMP_OFFSET] = temp_off_oop;

            sta_stack_push(slab, (STA_OOP)(uintptr_t)bc_h);
            frame->pc = blk_start + blk_length;
            break;
        }

        case OP_CLOSURE_COPY: {
            /* Capturing block. BlockClosure: 6 slots (adds context).
             * BlockDescriptor: 5 slots (adds numContext). */
            STA_OOP desc = sta_method_literal(frame->method, (uint8_t)operand);
            STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)desc;
            STA_OOP *dp = sta_payload(dh);
            STA_OOP start_pc_oop  = dp[0];
            STA_OOP body_len_oop  = dp[1];
            STA_OOP num_args_oop  = dp[2];
            STA_OOP temp_off_oop  = dp[3];
            /* dp[4] = numContext (total temps in context) — used if we need
             * to allocate a fresh context for nested blocks in the future.
             * For now, the block shares the method's context. */
            uint32_t blk_start  = (uint32_t)STA_SMALLINT_VAL(start_pc_oop);
            uint32_t blk_length = (uint32_t)STA_SMALLINT_VAL(body_len_oop);

            GC_SAVE_FRAME();
            STA_ObjHeader *bc_h = sta_heap_alloc(heap, STA_CLS_BLOCKCLOSURE, 6);
            if (!bc_h) {
                fprintf(stderr, "FATAL: failed to allocate BlockClosure\n");
                abort();
            }
            STA_OOP *bc_s = sta_payload(bc_h);
            bc_s[BC_SLOT_START_PC]    = start_pc_oop;
            bc_s[BC_SLOT_NUM_ARGS]    = num_args_oop;
            bc_s[BC_SLOT_METHOD]      = frame->method;
            bc_s[BC_SLOT_RECEIVER]    = frame->receiver;
            bc_s[BC_SLOT_TEMP_OFFSET] = temp_off_oop;
            bc_s[BC_SLOT_CONTEXT]     = frame->context;

            sta_stack_push(slab, (STA_OOP)(uintptr_t)bc_h);
            frame->pc = blk_start + blk_length;
            break;
        }

        case OP_NON_LOCAL_RETURN: {
            /* ^ inside a block: return from the home method's frame.
             * The home method is the frame that originally created the context.
             * Walk the sender chain to find a frame whose context matches
             * this frame's context and which is NOT a block frame (i.e.,
             * it's the method that allocated the context).
             *
             * Strategy: walk sender chain looking for the frame with
             * STA_FRAME_FLAG_MARKER set (the home method frame). We set
             * this flag when allocating a context for a method. */
            result = sta_stack_pop(slab);
            STA_OOP home_ctx = frame->context;

            /* Walk sender chain to find the home method frame. */
            STA_Frame *target = frame->sender;
            while (target != NULL) {
                if ((target->flags & STA_FRAME_FLAG_MARKER) &&
                    target->context == home_ctx) {
                    break;
                }
                target = target->sender;
            }

            if (target == NULL) {
                /* Home method already returned — signal BlockCannotReturn.
                 * Create a BlockCannotReturn exception and signal it.
                 * GC safety: push result onto expression stack to root it. */
                sta_stack_push(slab, result);
                GC_SAVE_FRAME();
                STA_ObjHeader *bcr_h = sta_heap_alloc(heap,
                    STA_CLS_BLOCKCANNOTRETURN, 4);
                result = sta_stack_pop(slab);  /* re-read after potential GC */
                if (bcr_h) {
                    STA_OOP *bcr_slots = sta_payload(bcr_h);
                    bcr_slots[0] = sta_spc_get(SPC_NIL); /* messageText */
                    bcr_slots[1] = sta_spc_get(SPC_NIL); /* signalContext */
                    bcr_slots[2] = result;                /* result */
                    bcr_slots[3] = sta_spc_get(SPC_NIL); /* deadHome */
                    STA_OOP bcr_oop = (STA_OOP)(uintptr_t)bcr_h;

                    /* Try to signal the exception via the handler chain. */
                    STA_HandlerEntry *entry = sta_handler_find_ctx(&exec_ctx, bcr_oop);
                    if (entry) {
                        STA_HandlerEntry **htop = sta_handler_top_ptr(&exec_ctx);
                        *htop = entry->prev;
                        sta_handler_set_signaled_ctx(&exec_ctx, bcr_oop);
                        longjmp(entry->jmp, 1);
                    }
                }
                fprintf(stderr, "FATAL: BlockCannotReturn — home method already returned\n");
                abort();
            }

            /* Unwind: pop all frames from current up to (and including)
             * the home method frame. The home method's sender gets the
             * return value pushed. */
            /* First, fire ensure: blocks along the unwind path. */
            /* (Story 5 will add ensure: firing here.) */

            /* Pop frames up to and including target. */
            while (frame != target) {
                STA_Frame *s = frame->sender;
                sta_frame_pop(slab, frame);
                frame = s;
            }
            /* Now frame == target (the home method). Pop it too. */
            STA_Frame *home_sender = frame->sender;
            sta_frame_pop(slab, frame);
            frame = home_sender;

            if (frame != NULL) {
                bytecodes = sta_method_bytecodes(frame->method);
                slots = effective_slots(frame);
                sta_stack_push(slab, result);
            }
            break;
        }

        case OP_PUSH_OUTER_TEMP: {
            /* Operand encoding: high 4 bits = depth, low 4 bits = slot index.
             * For now we use a simple scheme: operand byte encodes
             * (depth << 4) | slot_index, supporting depth 0-15 and slot 0-15.
             * If we need larger ranges, OP_WIDE provides 16-bit operand. */
            /* Actually, for simplicity and to match common Smalltalk VMs,
             * we use a two-byte encoding: the operand byte is the slot index,
             * and the depth is encoded in an additional byte after the operand.
             * BUT we only have a single operand byte in our instruction format.
             *
             * Simpler: encode depth in high nibble, slot in low nibble.
             * With OP_WIDE this gives depth 0-255 and slot 0-255. But we
             * only have one operand... Let's use: operand = (depth << 8) | slot
             * which works with OP_WIDE (16-bit operand). For non-wide, depth
             * is in high nibble, slot in low nibble of the 8-bit operand. */
            uint8_t depth = (uint8_t)(operand >> 4);
            uint8_t slot  = (uint8_t)(operand & 0x0F);
            if (is_wide) {
                depth = (uint8_t)(operand >> 8);
                slot  = (uint8_t)(operand & 0xFF);
            }
            /* Walk context chain depth levels. */
            STA_OOP ctx = frame->context;
            for (uint8_t d = 0; d < depth; d++) {
                if (ctx == 0) {
                    fprintf(stderr, "FATAL: outer temp context chain broken at depth %u\n", d);
                    abort();
                }
                /* Context chain: slot 0 of a context could be the outer context.
                 * But in our design, the context is shared — nested blocks share
                 * the same context as the method. For depth > 1, we'd need
                 * context chaining. For now, depth > 0 means walk up sender
                 * chain to find a frame with a different context. */
                /* TODO: implement full context chain for deeply nested blocks. */
            }
            if (ctx == 0) {
                fprintf(stderr, "FATAL: OP_PUSH_OUTER_TEMP with no context\n");
                abort();
            }
            STA_OOP *ctx_slots = sta_payload((STA_ObjHeader *)(uintptr_t)ctx);
            sta_stack_push(slab, ctx_slots[slot]);
            frame->pc += insn_len;
            break;
        }

        case OP_STORE_OUTER_TEMP:
        case OP_POP_STORE_OUTER_TEMP: {
            uint8_t depth = (uint8_t)(operand >> 4);
            uint8_t slot  = (uint8_t)(operand & 0x0F);
            if (is_wide) {
                depth = (uint8_t)(operand >> 8);
                slot  = (uint8_t)(operand & 0xFF);
            }
            STA_OOP ctx = frame->context;
            for (uint8_t d = 0; d < depth; d++) {
                if (ctx == 0) {
                    fprintf(stderr, "FATAL: outer temp context chain broken\n");
                    abort();
                }
            }
            if (ctx == 0) {
                fprintf(stderr, "FATAL: OP_STORE_OUTER_TEMP with no context\n");
                abort();
            }
            STA_OOP *ctx_slots = sta_payload((STA_ObjHeader *)(uintptr_t)ctx);
            STA_OOP val = (opcode == OP_POP_STORE_OUTER_TEMP)
                          ? sta_stack_pop(slab)
                          : sta_stack_peek(slab);
            ctx_slots[slot] = val;
            frame->pc += insn_len;
            break;
        }

        case OP_SEND_ASYNC:
        case OP_ASK:
        case OP_ACTOR_SPAWN:
        case OP_SELF_ADDRESS:
            fprintf(stderr, "FATAL: opcode 0x%02x not yet implemented\n", opcode);
            abort();
            break;

        case OP_PUSH_CONTEXT:
            fprintf(stderr, "FATAL: OP_PUSH_CONTEXT not yet implemented\n");
            abort();
            break;

        default:
            fprintf(stderr, "FATAL: unknown opcode 0x%02x at pc=%u\n",
                    opcode, frame->pc);
            abort();
        }
    }

    return result;
}

/* ── Public entry points ──────────────────────────────────────────────── */

STA_OOP sta_interpret(STA_VM *vm,
                      STA_OOP method, STA_OOP receiver,
                      STA_OOP *args, uint8_t nargs)
{
    /* Resolve slab and heap from the root actor when available. */
    struct STA_Actor *actor = vm->root_actor;
    STA_StackSlab *slab = actor ? &actor->slab : &vm->slab;
    STA_Heap *heap = actor ? &actor->heap : &vm->heap;

    STA_Frame *frame = sta_frame_push(slab, method, receiver, NULL,
                                       args, nargs);
    if (!frame) {
        fprintf(stderr, "sta_interpret: failed to push initial frame\n");
        abort();
    }

    /* If the method needs a context (largeFrame bit), allocate one.
     * Same logic as the OP_SEND handler for context methods. */
    STA_OOP mh = sta_method_header(method);
    if (STA_METHOD_LARGE_FRAME(mh)) {
        uint32_t ctx_size = (uint32_t)frame->arg_count + (uint32_t)frame->local_count;
        /* GC safety: frame is on the slab with args/temps rooted.
         * Set saved_frame so GC can walk the stack. */
        if (actor) actor->saved_frame = frame;
        STA_ObjHeader *ctx_h = sta_heap_alloc(heap, STA_CLS_ARRAY, ctx_size);
        if (!ctx_h) {
            fprintf(stderr, "sta_interpret: failed to allocate context\n");
            abort();
        }
        STA_OOP *ctx_slots = sta_payload(ctx_h);
        STA_OOP *inline_slots = sta_frame_slots(frame);
        for (uint32_t i = 0; i < ctx_size; i++)
            ctx_slots[i] = inline_slots[i];
        frame->context = (STA_OOP)(uintptr_t)ctx_h;
        frame->flags |= STA_FRAME_FLAG_MARKER;
    }

    return interpret_loop_ex(vm, frame, NULL);
}

STA_OOP sta_eval_block(STA_VM *vm,
                        STA_OOP block_closure,
                        STA_OOP *args, uint8_t nargs)
{
    STA_ObjHeader *bc_h = (STA_ObjHeader *)(uintptr_t)block_closure;
    STA_OOP *bc_slots = sta_payload(bc_h);
    uint32_t start_pc = (uint32_t)STA_SMALLINT_VAL(bc_slots[BC_SLOT_START_PC]);
    STA_OOP home_method = bc_slots[BC_SLOT_METHOD];
    STA_OOP receiver = bc_slots[BC_SLOT_RECEIVER];
    uint32_t temp_off = (uint32_t)STA_SMALLINT_VAL(bc_slots[BC_SLOT_TEMP_OFFSET]);

    /* Check if this is a closure (6-slot) with context. */
    bool has_ctx = (bc_h->size >= 6 && bc_slots[BC_SLOT_CONTEXT] != 0);

    /* Resolve slab from the root actor when available. */
    struct STA_Actor *actor = vm->root_actor;
    STA_StackSlab *use_slab = actor ? &actor->slab : &vm->slab;

    STA_Frame *frame = sta_frame_push(use_slab, home_method, receiver, NULL,
                                       NULL, 0);
    if (!frame) {
        fprintf(stderr, "sta_eval_block: stack overflow\n");
        abort();
    }

    if (has_ctx) {
        frame->context = bc_slots[BC_SLOT_CONTEXT];
        if (nargs > 0 && args) {
            STA_OOP *ctx_slots = sta_payload(
                (STA_ObjHeader *)(uintptr_t)frame->context);
            for (uint8_t i = 0; i < nargs; i++)
                ctx_slots[temp_off + i] = args[i];
        }
    } else {
        if (nargs > 0 && args) {
            STA_OOP *blk_slots = sta_frame_slots(frame);
            for (uint8_t i = 0; i < nargs; i++)
                blk_slots[temp_off + i] = args[i];
        }
    }
    frame->pc = start_pc;

    return interpret_loop_ex(vm, frame, NULL);
}

/* ── Scheduled actor execution (with preemption) ─────────────────────── */

int sta_interpret_actor(STA_VM *vm, struct STA_Actor *actor,
                        STA_OOP method, STA_OOP receiver,
                        STA_OOP *args, uint8_t nargs)
{
    STA_StackSlab *slab = &actor->slab;
    STA_Heap *heap = &actor->heap;

    STA_Frame *frame = sta_frame_push(slab, method, receiver, NULL,
                                       args, nargs);
    if (!frame) {
        fprintf(stderr, "sta_interpret_actor: failed to push initial frame\n");
        return -1;
    }

    /* If the method needs a context, allocate one. */
    STA_OOP mh = sta_method_header(method);
    if (STA_METHOD_LARGE_FRAME(mh)) {
        uint32_t ctx_size = (uint32_t)frame->arg_count + (uint32_t)frame->local_count;
        /* GC safety: frame is on slab with args/temps rooted. */
        actor->saved_frame = frame;
        STA_ObjHeader *ctx_h = sta_heap_alloc(heap, STA_CLS_ARRAY, ctx_size);
        if (!ctx_h) return -1;
        STA_OOP *ctx_slots = sta_payload(ctx_h);
        STA_OOP *inline_slots = sta_frame_slots(frame);
        for (uint32_t i = 0; i < ctx_size; i++)
            ctx_slots[i] = inline_slots[i];
        frame->context = (STA_OOP)(uintptr_t)ctx_h;
        frame->flags |= STA_FRAME_FLAG_MARKER;
    }

    actor->saved_frame = NULL;

    /* Install a catch-all exception handler for supervision (Epic 6).
     * If an unhandled exception reaches this handler, prim_signal will
     * longjmp here instead of calling abort(). ensure: blocks fire
     * during normal unwinding before we reach this point. */
    STA_ExecContext exc_ctx = { .vm = vm, .actor = actor };
    STA_HandlerEntry catch_all;
    memset(&catch_all, 0, sizeof(catch_all));
    catch_all.exception_class = sta_class_table_get(&vm->class_table,
                                                      STA_CLS_EXCEPTION);
    catch_all.saved_slab_top = slab->top;
    catch_all.saved_slab_sp  = slab->sp;

    if (setjmp(catch_all.jmp) == 0) {
        sta_handler_push_ctx(&exc_ctx, &catch_all);
        STA_OOP result = interpret_loop_ex(vm, frame, actor);
        sta_handler_pop_ctx(&exc_ctx);

        if (result == STA_OOP_PREEMPTED) {
            return STA_INTERPRET_PREEMPTED;
        }
        actor->saved_frame = NULL;
        return STA_INTERPRET_COMPLETED;
    } else {
        /* Unhandled exception caught. Restore slab state. */
        slab->top = catch_all.saved_slab_top;
        slab->sp  = catch_all.saved_slab_sp;
        actor->saved_frame = NULL;
        return STA_INTERPRET_EXCEPTION;
    }
}

int sta_interpret_resume(STA_VM *vm, struct STA_Actor *actor)
{
    STA_Frame *frame = actor->saved_frame;
    if (!frame) return STA_INTERPRET_COMPLETED;

    STA_StackSlab *slab = &actor->slab;
    actor->saved_frame = NULL;

    /* Same catch-all handler as sta_interpret_actor. */
    STA_ExecContext exc_ctx = { .vm = vm, .actor = actor };
    STA_HandlerEntry catch_all;
    memset(&catch_all, 0, sizeof(catch_all));
    catch_all.exception_class = sta_class_table_get(&vm->class_table,
                                                      STA_CLS_EXCEPTION);
    catch_all.saved_slab_top = slab->top;
    catch_all.saved_slab_sp  = slab->sp;

    if (setjmp(catch_all.jmp) == 0) {
        sta_handler_push_ctx(&exc_ctx, &catch_all);
        STA_OOP result = interpret_loop_ex(vm, frame, actor);
        sta_handler_pop_ctx(&exc_ctx);

        if (result == STA_OOP_PREEMPTED) {
            return STA_INTERPRET_PREEMPTED;
        }
        actor->saved_frame = NULL;
        return STA_INTERPRET_COMPLETED;
    } else {
        slab->top = catch_all.saved_slab_top;
        slab->sp  = catch_all.saved_slab_sp;
        actor->saved_frame = NULL;
        return STA_INTERPRET_EXCEPTION;
    }
}
