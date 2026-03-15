/* src/vm/interpreter.c
 * Bytecode interpreter — Phase 1 dispatch loop.
 * See interpreter.h, bytecode spec §§2–5, 10, ADR 010, ADR 014.
 */
#include "interpreter.h"
#include "compiled_method.h"
#include "selector.h"
#include "primitive_table.h"
#include "method_dict.h"
#include "special_objects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Internal helpers ──────────────────────────────────────────────────── */

/* Determine class index for any OOP (SmallInt, Character, or heap object). */
static uint32_t oop_class_index(STA_OOP oop) {
    if (STA_IS_SMALLINT(oop)) return STA_CLS_SMALLINTEGER;
    if (STA_IS_CHAR(oop))     return STA_CLS_CHARACTER;
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)oop;
    return h->class_index;
}

/* Look up a selector in a class's method dictionary, walking the superclass
 * chain. Returns the method OOP, or 0 if not found (DNU). */
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
    return 0;  /* not found */
}

/* Compute bytes for a frame (struct + slots) to check TCO fit. */
static size_t frame_byte_size(uint16_t arg_count, uint16_t local_count) {
    return sizeof(STA_Frame) +
           ((size_t)arg_count + (size_t)local_count) * sizeof(STA_OOP);
}

/* ── Dispatch loop (extracted for reuse by sta_eval_block) ─────────────── */

static STA_OOP interpret_loop(STA_StackSlab *slab, STA_Heap *heap,
                                STA_ClassTable *ct, STA_Frame *frame)
{
    /* Set slab for primitives that need it (on:do:, ensure:). */
    sta_primitive_set_slab(slab);

    uint32_t reductions = 0;
    STA_OOP result = 0;

    /* Cache frequently accessed pointers. */
    const uint8_t *bytecodes = sta_method_bytecodes(frame->method);
    STA_OOP *slots = sta_frame_slots(frame);

    /* ── Main dispatch loop ────────────────────────────────────────────── */
    while (frame != NULL) {
        uint8_t opcode  = bytecodes[frame->pc];
        uint16_t operand = bytecodes[frame->pc + 1];
        uint8_t is_wide = 0;

        /* Handle OP_WIDE prefix. */
        if (opcode == OP_WIDE) {
            uint16_t high = operand;
            opcode  = bytecodes[frame->pc + 2];
            operand = (high << 8) | bytecodes[frame->pc + 3];
            is_wide = 1;
        }

        uint32_t insn_len = is_wide ? 4u : 2u;

        switch (opcode) {

        /* ── NOP ───────────────────────────────────────────────────────── */
        case OP_NOP:
            frame->pc += insn_len;
            break;

        /* ── Push group ────────────────────────────────────────────────── */
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

        case OP_PUSH_TEMP: {
            sta_stack_push(slab, slots[operand]);
            frame->pc += insn_len;
            break;
        }

        case OP_PUSH_INSTVAR: {
            STA_ObjHeader *recv_h = (STA_ObjHeader *)(uintptr_t)frame->receiver;
            STA_OOP *recv_slots = sta_payload(recv_h);
            sta_stack_push(slab, recv_slots[operand]);
            frame->pc += insn_len;
            break;
        }

        case OP_PUSH_GLOBAL: {
            /* lit[operand] is an Association; its value is in slot 1. */
            STA_OOP assoc = sta_method_literal(frame->method, (uint8_t)operand);
            STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)assoc;
            STA_OOP value = sta_payload(ah)[1];  /* Association value slot */
            sta_stack_push(slab, value);
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

        /* ── Store group ───────────────────────────────────────────────── */
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

        /* ── Send group ────────────────────────────────────────────────── */
        case OP_SEND:
        case OP_SEND_SUPER: {
            STA_OOP selector = sta_method_literal(frame->method, (uint8_t)operand);
            uint8_t arity = sta_selector_arity(selector);

            /* Pop arguments (right to left) and receiver. */
            STA_OOP send_args[256];
            for (int i = arity - 1; i >= 0; i--) {
                send_args[i] = sta_stack_pop(slab);
            }
            STA_OOP send_receiver = sta_stack_pop(slab);

            /* Look up the method. */
            STA_OOP found_method = 0;
            if (opcode == OP_SEND_SUPER) {
                /* Super send: start at superclass of owner class.
                 * Owner class is last literal in current method. */
                STA_OOP cur_header = sta_method_header(frame->method);
                uint8_t cur_nlits = STA_METHOD_NUM_LITERALS(cur_header);
                STA_OOP owner_cls = sta_method_literal(frame->method,
                                                        cur_nlits - 1);
                STA_OOP super_cls = sta_class_superclass(owner_cls);
                /* Walk from super_cls directly. */
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
                /* Normal send — lookup from receiver's class. */
                uint32_t lookup_cls_idx = oop_class_index(send_receiver);
                found_method = method_lookup(ct, lookup_cls_idx, selector);
            }

            if (found_method == 0) {
                /* doesNotUnderstand: protocol (§10.9).
                 * Create a Message object and send #doesNotUnderstand:. */
                STA_OOP dnu_sel = sta_spc_get(SPC_DOES_NOT_UNDERSTAND);
                if (dnu_sel != 0) {
                    uint32_t recv_cls_idx = oop_class_index(send_receiver);
                    STA_OOP dnu_method = method_lookup(ct, recv_cls_idx, dnu_sel);
                    if (dnu_method != 0) {
                        found_method = dnu_method;

                        /* Build a Message object: selector, args, lookupClass. */
                        STA_ObjHeader *msg_h = sta_heap_alloc(heap,
                            STA_CLS_MESSAGE, 3);
                        if (msg_h) {
                            STA_OOP *msg_slots = sta_payload(msg_h);
                            msg_slots[0] = selector;

                            /* Create args Array. */
                            STA_ObjHeader *arr_h = sta_heap_alloc(heap,
                                STA_CLS_ARRAY, arity);
                            if (arr_h) {
                                STA_OOP *arr_slots = sta_payload(arr_h);
                                for (uint8_t i = 0; i < arity; i++)
                                    arr_slots[i] = send_args[i];
                                msg_slots[1] = (STA_OOP)(uintptr_t)arr_h;
                            } else {
                                msg_slots[1] = sta_spc_get(SPC_NIL);
                            }

                            msg_slots[2] = sta_class_table_get(ct,
                                recv_cls_idx);
                            send_args[0] = (STA_OOP)(uintptr_t)msg_h;
                        } else {
                            /* Fallback: pass selector if allocation fails. */
                            send_args[0] = selector;
                        }
                        arity = 1;
                    }
                }
                if (found_method == 0) {
                    size_t sel_len;
                    const char *sel_str = sta_symbol_get_bytes(selector, &sel_len);
                    fprintf(stderr,
                        "FATAL: doesNotUnderstand: #%.*s\n",
                        (int)sel_len, sel_str);
                    abort();
                }
            }

            /* Advance PC past this send instruction. */
            frame->pc += insn_len;

            /* Reduction counting. */
            reductions++;
            if (reductions >= STA_REDUCTION_QUOTA) {
                reductions = 0;
            }

            /* Check for TCO: is the next instruction OP_RETURN_TOP? */
            int is_tail = (bytecodes[frame->pc] == OP_RETURN_TOP);

            STA_OOP mh = sta_method_header(found_method);
            uint8_t callee_has_prim = STA_METHOD_HAS_PRIM(mh);
            uint16_t callee_prim_idx = (uint16_t)STA_METHOD_PRIM_INDEX(mh);
            uint8_t callee_nargs = STA_METHOD_NUM_ARGS(mh);
            uint8_t callee_ntemps = STA_METHOD_NUM_TEMPS(mh);
            uint16_t callee_locals = (callee_ntemps > callee_nargs)
                                      ? (uint16_t)(callee_ntemps - callee_nargs)
                                      : 0;

            /* Try primitive if method has one. */
            int prim_failed = 0;
            int prim_fail_code = 0;
            if (callee_has_prim) {
                uint16_t prim_idx = callee_prim_idx;
                if (callee_prim_idx == 0) {
                    /* Extended primitive: read from bytecodes. */
                    const uint8_t *callee_bc = sta_method_bytecodes(found_method);
                    if (callee_bc[0] == OP_WIDE) {
                        prim_idx = ((uint16_t)callee_bc[1] << 8) | callee_bc[3];
                    } else if (callee_bc[0] == OP_PRIMITIVE) {
                        prim_idx = callee_bc[1];
                    }
                }
                /* Block activation: prims 81-84 (#value, #value:, etc.).
                 * If the receiver is a BlockClosure, create a block frame
                 * and enter the block body instead of calling the C prim. */
                if (prim_idx >= 81 && prim_idx <= 84 &&
                    STA_IS_HEAP(send_receiver) &&
                    ((STA_ObjHeader *)(uintptr_t)send_receiver)->class_index
                        == STA_CLS_BLOCKCLOSURE) {
                    STA_OOP *bc_slots = sta_payload(
                        (STA_ObjHeader *)(uintptr_t)send_receiver);
                    uint32_t start_pc = (uint32_t)STA_SMALLINT_VAL(bc_slots[0]);
                    STA_OOP home_method = bc_slots[2];
                    uint32_t temp_off = (uint32_t)STA_SMALLINT_VAL(bc_slots[4]);

                    /* Push frame with 0 args so all numTemps become locals
                     * (initialized to nil), then place block args at offset. */
                    STA_Frame *block_frame = sta_frame_push(slab, home_method,
                        frame->receiver, frame, NULL, 0);
                    if (!block_frame) {
                        fprintf(stderr, "FATAL: stack overflow in block activation\n");
                        abort();
                    }
                    block_frame->pc = start_pc;
                    /* Place block args at the correct temp offset. */
                    STA_OOP *blk_slots = sta_frame_slots(block_frame);
                    for (uint8_t i = 0; i < arity; i++)
                        blk_slots[temp_off + i] = send_args[i];
                    frame = block_frame;
                    bytecodes = sta_method_bytecodes(frame->method);
                    slots = sta_frame_slots(frame);
                    break;
                }

                STA_OOP prim_args_buf[257];
                prim_args_buf[0] = send_receiver;
                for (uint8_t i = 0; i < arity; i++)
                    prim_args_buf[1 + i] = send_args[i];
                STA_OOP prim_result;
                STA_PrimFn fn = (prim_idx < STA_PRIM_TABLE_SIZE)
                                 ? sta_primitives[prim_idx] : NULL;
                if (fn) {
                    int rc = fn(prim_args_buf, arity, &prim_result);
                    if (rc == 0) {
                        /* Primitive success — push result, continue. */
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

            /* TCO check: reuse frame if callee fits. */
            if (is_tail && frame->sender != NULL) {
                size_t cur_size = frame_byte_size(frame->arg_count,
                                                   frame->local_count);
                size_t new_size = frame_byte_size(arity, callee_locals);
                if (new_size <= cur_size) {
                    /* Reuse current frame. */
                    frame->method = found_method;
                    frame->receiver = send_receiver;
                    frame->pc = 0;
                    frame->arg_count = arity;
                    frame->local_count = callee_locals;

                    STA_OOP *new_slots = sta_frame_slots(frame);
                    for (uint8_t i = 0; i < arity; i++)
                        new_slots[i] = send_args[i];
                    STA_OOP nil_oop = sta_spc_get(SPC_NIL);
                    for (uint16_t i = 0; i < callee_locals; i++)
                        new_slots[arity + i] = nil_oop;

                    /* Store primitive failure code if applicable. */
                    if (prim_failed && callee_locals > 0) {
                        new_slots[arity] = STA_SMALLINT_OOP(prim_fail_code);
                    }

                    /* Reset expression stack. */
                    slab->sp = (uint8_t *)&new_slots[arity + callee_locals];

                    bytecodes = sta_method_bytecodes(frame->method);
                    slots = new_slots;

                    /* Skip primitive preamble if present. */
                    if (callee_has_prim) {
                        frame->pc = (callee_prim_idx != 0) ? 2u : 4u;
                    }
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
            frame = new_frame;
            bytecodes = sta_method_bytecodes(frame->method);
            slots = sta_frame_slots(frame);

            /* Store primitive failure code if applicable. */
            if (prim_failed && frame->local_count > 0) {
                slots[frame->arg_count] = STA_SMALLINT_OOP(prim_fail_code);
            }

            /* Skip primitive preamble if present (entering fallback path). */
            if (callee_has_prim) {
                frame->pc = (callee_prim_idx != 0) ? 2u : 4u;
            }
            break;
        }

        /* ── Return group ──────────────────────────────────────────────── */
        case OP_RETURN_TOP: {
            result = sta_stack_pop(slab);
            goto do_return;
        }

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
                /* Push result onto sender's expression stack. */
                /* Restore sp to after sender's slots + existing stack. */
                bytecodes = sta_method_bytecodes(frame->method);
                slots = sta_frame_slots(frame);
                sta_stack_push(slab, result);
            }
            break;
        }

        /* ── Jump group ────────────────────────────────────────────────── */
        case OP_JUMP:
            frame->pc += insn_len + operand;
            break;

        case OP_JUMP_TRUE: {
            STA_OOP val = sta_stack_pop(slab);
            if (val == sta_spc_get(SPC_TRUE)) {
                frame->pc += insn_len + operand;
            } else if (val == sta_spc_get(SPC_FALSE)) {
                frame->pc += insn_len;
            } else {
                /* mustBeBoolean — abort for Phase 1. */
                fprintf(stderr, "FATAL: mustBeBoolean in JUMP_TRUE\n");
                abort();
            }
            break;
        }

        case OP_JUMP_FALSE: {
            STA_OOP val = sta_stack_pop(slab);
            if (val == sta_spc_get(SPC_FALSE)) {
                frame->pc += insn_len + operand;
            } else if (val == sta_spc_get(SPC_TRUE)) {
                frame->pc += insn_len;
            } else {
                fprintf(stderr, "FATAL: mustBeBoolean in JUMP_FALSE\n");
                abort();
            }
            break;
        }

        case OP_JUMP_BACK:
            frame->pc = frame->pc + insn_len - operand;
            /* Reduction counting on backward jump. */
            reductions++;
            if (reductions >= STA_REDUCTION_QUOTA) {
                reductions = 0;
            }
            break;

        /* ── Stack and miscellaneous ───────────────────────────────────── */
        case OP_POP:
            (void)sta_stack_pop(slab);
            frame->pc += insn_len;
            break;

        case OP_DUP:
            sta_stack_push(slab, sta_stack_peek(slab));
            frame->pc += insn_len;
            break;

        case OP_PRIMITIVE: {
            /* OP_PRIMITIVE encountered mid-method (not as preamble).
             * This shouldn't normally happen in well-formed bytecode,
             * but handle it: try the primitive. */
            uint16_t prim_idx = operand;
            STA_PrimFn fn = (prim_idx < STA_PRIM_TABLE_SIZE)
                             ? sta_primitives[prim_idx] : NULL;
            if (fn) {
                /* Build args from frame slots. */
                uint8_t na = frame->arg_count;
                STA_OOP prim_args[257];
                prim_args[0] = frame->receiver;
                for (uint8_t i = 0; i < na; i++)
                    prim_args[1 + i] = slots[i];
                STA_OOP prim_result;
                int rc = fn(prim_args, na, &prim_result);
                if (rc == 0) {
                    sta_stack_push(slab, prim_result);
                } else {
                    /* Store failure code in temp[numArgs]. */
                    if (frame->local_count > 0) {
                        slots[frame->arg_count] = STA_SMALLINT_OOP(rc);
                    }
                }
            }
            frame->pc += insn_len;
            break;
        }

        /* ── Block / closure — Phase 1 ─────────────────────────────────── */
        case OP_BLOCK_COPY: {
            /* operand = literal index of BlockDescriptor.
             * BlockDescriptor slots: [startPC, bodyLength, numArgs, tempOffset].
             * Create a BlockClosure object on the heap:
             *   slot 0: startPC (SmallInt)
             *   slot 1: numArgs (SmallInt)
             *   slot 2: homeMethod (OOP)
             *   slot 3: receiver (OOP)
             *   slot 4: tempOffset (SmallInt) — temp index for first block arg
             * Then jump past the block body. */
            STA_OOP desc = sta_method_literal(frame->method, (uint8_t)operand);
            STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)desc;
            STA_OOP *dp = sta_payload(dh);
            STA_OOP start_pc_oop  = dp[0];
            STA_OOP body_len_oop  = dp[1];
            STA_OOP num_args_oop  = dp[2];
            STA_OOP temp_off_oop  = dp[3];
            uint32_t blk_start  = (uint32_t)STA_SMALLINT_VAL(start_pc_oop);
            uint32_t blk_length = (uint32_t)STA_SMALLINT_VAL(body_len_oop);

            STA_ObjHeader *bc_h = sta_heap_alloc(heap, STA_CLS_BLOCKCLOSURE, 5);
            if (!bc_h) {
                fprintf(stderr, "FATAL: failed to allocate BlockClosure\n");
                abort();
            }
            STA_OOP *bc_slots = sta_payload(bc_h);
            bc_slots[0] = start_pc_oop;        /* startPC */
            bc_slots[1] = num_args_oop;         /* numArgs */
            bc_slots[2] = frame->method;        /* homeMethod */
            bc_slots[3] = frame->receiver;      /* receiver */
            bc_slots[4] = temp_off_oop;         /* tempOffset */

            sta_stack_push(slab, (STA_OOP)(uintptr_t)bc_h);

            /* Jump past the block body. */
            frame->pc = blk_start + blk_length;
            break;
        }

        /* ── Phase 2 opcodes — NYI ─────────────────────────────────────── */
        case OP_STORE_OUTER_TEMP:
        case OP_POP_STORE_OUTER_TEMP:
        case OP_NON_LOCAL_RETURN:
        case OP_CLOSURE_COPY:
        case OP_PUSH_OUTER_TEMP:
        case OP_SEND_ASYNC:
        case OP_ASK:
        case OP_ACTOR_SPAWN:
        case OP_SELF_ADDRESS:
            fprintf(stderr, "FATAL: opcode 0x%02x not yet implemented\n", opcode);
            abort();
            break;

        /* ── Phase 3 opcode — NYI ──────────────────────────────────────── */
        case OP_PUSH_CONTEXT:
            fprintf(stderr, "FATAL: OP_PUSH_CONTEXT not yet implemented\n");
            abort();
            break;

        /* ── Unknown / reserved opcodes ────────────────────────────────── */
        default:
            fprintf(stderr, "FATAL: unknown opcode 0x%02x at pc=%u\n",
                    opcode, frame->pc);
            abort();
        }
    }

    return result;
}

/* ── Public entry points ──────────────────────────────────────────────── */

STA_OOP sta_interpret(STA_StackSlab *slab, STA_Heap *heap,
                      STA_ClassTable *ct,
                      STA_OOP method, STA_OOP receiver,
                      STA_OOP *args, uint8_t nargs)
{
    /* Push the initial frame. */
    STA_Frame *frame = sta_frame_push(slab, method, receiver, NULL,
                                       args, nargs);
    if (!frame) {
        fprintf(stderr, "sta_interpret: failed to push initial frame\n");
        abort();
    }

    return interpret_loop(slab, heap, ct, frame);
}

STA_OOP sta_eval_block(STA_StackSlab *slab, STA_Heap *heap,
                        STA_ClassTable *ct,
                        STA_OOP block_closure,
                        STA_OOP *args, uint8_t nargs)
{
    STA_OOP *bc_slots = sta_payload(
        (STA_ObjHeader *)(uintptr_t)block_closure);
    uint32_t start_pc = (uint32_t)STA_SMALLINT_VAL(bc_slots[0]);
    STA_OOP home_method = bc_slots[2];
    STA_OOP receiver = bc_slots[3];
    uint32_t temp_off = (uint32_t)STA_SMALLINT_VAL(bc_slots[4]);

    /* Push frame with 0 args so all numTemps become locals (nil-filled),
     * then place block args at the correct temp offset. */
    STA_Frame *frame = sta_frame_push(slab, home_method, receiver, NULL,
                                       NULL, 0);
    if (!frame) {
        fprintf(stderr, "sta_eval_block: stack overflow\n");
        abort();
    }
    /* Place block args at the correct temp offset. */
    if (nargs > 0 && args) {
        STA_OOP *blk_slots = sta_frame_slots(frame);
        for (uint8_t i = 0; i < nargs; i++)
            blk_slots[temp_off + i] = args[i];
    }
    frame->pc = start_pc;

    return interpret_loop(slab, heap, ct, frame);
}
