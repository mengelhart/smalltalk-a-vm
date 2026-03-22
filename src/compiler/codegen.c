/* src/compiler/codegen.c
 * Bytecode generator — see codegen.h for documentation.
 */
#include "codegen.h"
#include "../vm/interpreter.h"
#include "../vm/compiled_method.h"
#include "../vm/symbol_table.h"
#include "../vm/class_table.h"
#include "../vm/special_objects.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Bytecode buffer ─────────────────────────────────────────────────── */

typedef struct {
    uint8_t *bytes;
    uint32_t count;
    uint32_t capacity;
    bool     oom;       /* set on realloc failure; suppresses further writes */
} ByteBuf;

static void bb_init(ByteBuf *b) {
    b->bytes = NULL;
    b->count = 0;
    b->capacity = 0;
    b->oom = false;
}

static void bb_free(ByteBuf *b) {
    free(b->bytes);
    b->bytes = NULL;
    b->count = b->capacity = 0;
}

static bool bb_ensure(ByteBuf *b, uint32_t extra) {
    uint32_t need = b->count + extra;
    if (need <= b->capacity) return true;
    uint32_t cap = b->capacity ? b->capacity * 2 : 64;
    while (cap < need) cap *= 2;
    uint8_t *p = realloc(b->bytes, cap);
    if (!p) return false;
    b->bytes = p;
    b->capacity = cap;
    return true;
}

static void bb_emit(ByteBuf *b, uint8_t opcode, uint8_t operand) {
    if (b->oom) return;
    if (!bb_ensure(b, 2)) { b->oom = true; return; }
    b->bytes[b->count++] = opcode;
    b->bytes[b->count++] = operand;
}

static void bb_emit_wide(ByteBuf *b, uint8_t opcode, uint16_t operand) {
    if (b->oom) return;
    if (operand <= 255) {
        bb_emit(b, opcode, (uint8_t)operand);
    } else {
        if (!bb_ensure(b, 4)) { b->oom = true; return; }
        b->bytes[b->count++] = OP_WIDE;
        b->bytes[b->count++] = (uint8_t)(operand >> 8);
        b->bytes[b->count++] = opcode;
        b->bytes[b->count++] = (uint8_t)(operand & 0xFF);
    }
}

/* Patch a forward jump: write the offset into the operand slot. */
static void bb_patch_jump(ByteBuf *b, uint32_t jump_pc, uint32_t target_pc) {
    /* jump_pc points to the opcode byte of the jump instruction. */
    uint32_t offset = target_pc - (jump_pc + 2);
    /* Check if this was a wide jump (preceded by OP_WIDE). */
    if (jump_pc >= 2 && b->bytes[jump_pc - 2] == OP_WIDE) {
        /* Wide jump: high byte at jump_pc-1, low byte at jump_pc+1 */
        b->bytes[jump_pc - 1] = (uint8_t)(offset >> 8);
        b->bytes[jump_pc + 1] = (uint8_t)(offset & 0xFF);
    } else {
        b->bytes[jump_pc + 1] = (uint8_t)offset;
    }
}

/* ── Literal frame buffer ────────────────────────────────────────────── */

typedef struct {
    STA_OOP  *items;
    uint32_t  count;
    uint32_t  capacity;
} LitBuf;

static void lb_init(LitBuf *l) {
    l->items = NULL;
    l->count = 0;
    l->capacity = 0;
}

static void lb_free(LitBuf *l) {
    free(l->items);
    l->items = NULL;
    l->count = l->capacity = 0;
}

static uint16_t lb_add(LitBuf *l, STA_OOP oop) {
    /* Check for existing entry (dedup). */
    for (uint32_t i = 0; i < l->count; i++) {
        if (l->items[i] == oop) return (uint16_t)i;
    }
    if (l->count == l->capacity) {
        uint32_t cap = l->capacity ? l->capacity * 2 : 16;
        STA_OOP *p = realloc(l->items, cap * sizeof(STA_OOP));
        if (!p) return UINT16_MAX;
        l->items = p;
        l->capacity = cap;
    }
    uint16_t idx = (uint16_t)l->count;
    l->items[l->count++] = oop;
    return idx;
}

/* Add without dedup (e.g., block descriptors that are unique). */
static uint16_t lb_add_unique(LitBuf *l, STA_OOP oop) {
    if (l->count == l->capacity) {
        uint32_t cap = l->capacity ? l->capacity * 2 : 16;
        STA_OOP *p = realloc(l->items, cap * sizeof(STA_OOP));
        if (!p) return UINT16_MAX;
        l->items = p;
        l->capacity = cap;
    }
    uint16_t idx = (uint16_t)l->count;
    l->items[l->count++] = oop;
    return idx;
}

/* ── Capture analysis ────────────────────────────────────────────────── */

/* Walk an AST to determine if any non-inline block references a variable
 * defined in an outer scope.  This tells us whether the method needs a
 * heap-allocated context object.
 *
 * We also detect if any block contains a non-local return (NODE_RETURN
 * inside NODE_BLOCK), which requires the closure to carry a home-context
 * reference so the NLR knows where to return to.
 *
 * The analysis is conservative: if ANY non-inline block references a temp
 * from an outer scope, the method needs a context. */

/* Block-scope variable entry: a param/temp introduced by an enclosing block. */
typedef struct {
    const char *name;
    int         depth;   /* block nesting depth where this var is local */
} BlockScopeEntry;

typedef struct {
    /* Names of temps/args visible at method level. */
    const char **method_temps;
    uint32_t     method_temp_count;
    /* Block-scope variables (params/temps of enclosing non-inline blocks). */
    BlockScopeEntry *block_scope;
    uint32_t         block_scope_count;
    uint32_t         block_scope_capacity;
    bool         has_captures;     /* set true if any block captures */
    bool         has_nlr;          /* set true if any block has ^ */
} CaptureAnalysis;

static void capture_walk(CaptureAnalysis *ca, STA_AstNode *node,
                          int block_depth);

static void capture_walk_stmts(CaptureAnalysis *ca, STA_AstNode **stmts,
                                uint32_t count, int block_depth) {
    for (uint32_t i = 0; i < count; i++)
        capture_walk(ca, stmts[i], block_depth);
}

/* Check if name matches any method-level temp. */
static bool is_method_temp(CaptureAnalysis *ca, const char *name) {
    for (uint32_t i = 0; i < ca->method_temp_count; i++) {
        if (strcmp(ca->method_temps[i], name) == 0) return true;
    }
    return false;
}

/* Push a block-scope variable (param or temp of an enclosing block). */
static void ca_push_scope(CaptureAnalysis *ca, const char *name, int depth) {
    if (ca->block_scope_count == ca->block_scope_capacity) {
        uint32_t cap = ca->block_scope_capacity ? ca->block_scope_capacity * 2 : 16;
        BlockScopeEntry *p = realloc(ca->block_scope, cap * sizeof(BlockScopeEntry));
        if (!p) return;
        ca->block_scope = p;
        ca->block_scope_capacity = cap;
    }
    ca->block_scope[ca->block_scope_count].name = name;
    ca->block_scope[ca->block_scope_count].depth = depth;
    ca->block_scope_count++;
}

/* Check if name is a block-scope variable from an enclosing (shallower) depth. */
static bool is_enclosing_block_var(CaptureAnalysis *ca, const char *name,
                                    int current_depth) {
    for (uint32_t i = 0; i < ca->block_scope_count; i++) {
        if (ca->block_scope[i].depth < current_depth &&
            strcmp(ca->block_scope[i].name, name) == 0)
            return true;
    }
    return false;
}

/* Check if a send is an inlinable control structure (blocks inlined,
 * not real closures). We must match the same set as the codegen. */
static bool is_inlinable_send(STA_AstNode *node) {
    if (node->type != NODE_SEND) return false;
    const char *sel = node->as.send.selector;
    if (strcmp(sel, "ifTrue:") == 0 || strcmp(sel, "ifFalse:") == 0 ||
        strcmp(sel, "ifTrue:ifFalse:") == 0 || strcmp(sel, "ifFalse:ifTrue:") == 0 ||
        strcmp(sel, "whileTrue:") == 0 || strcmp(sel, "whileFalse:") == 0 ||
        strcmp(sel, "whileTrue") == 0 || strcmp(sel, "whileFalse") == 0 ||
        strcmp(sel, "and:") == 0 || strcmp(sel, "or:") == 0 ||
        strcmp(sel, "timesRepeat:") == 0)
        return true;
    return false;
}

/* Check if a block arg is inlined (part of an inlinable send). */
static bool is_inlined_block(STA_AstNode *block, STA_AstNode *parent) {
    if (!parent || !block || block->type != NODE_BLOCK) return false;
    if (!is_inlinable_send(parent)) return false;
    /* For loop sends, receiver block is also inlined. */
    if (block == parent->as.send.receiver) return true;
    for (uint32_t i = 0; i < parent->as.send.arg_count; i++) {
        if (block == parent->as.send.args[i]) return true;
    }
    return false;
}

static void capture_walk_with_parent(CaptureAnalysis *ca, STA_AstNode *node,
                                      int block_depth, STA_AstNode *parent) {
    if (!node) return;

    switch (node->type) {
    case NODE_VARIABLE:
        /* If we're inside a non-inline block and this variable is a
         * method-level temp, it's a capture. */
        if (block_depth > 0 && is_method_temp(ca, node->as.variable.name))
            ca->has_captures = true;
        /* If this variable belongs to an enclosing block (not the current
         * block), it's a block-in-block capture requiring a context. */
        if (block_depth > 0 &&
            is_enclosing_block_var(ca, node->as.variable.name, block_depth))
            ca->has_captures = true;
        break;

    case NODE_ASSIGN:
        capture_walk_with_parent(ca, node->as.assign.variable, block_depth, node);
        capture_walk_with_parent(ca, node->as.assign.value, block_depth, node);
        break;

    case NODE_RETURN:
        if (block_depth > 0)
            ca->has_nlr = true;
        capture_walk_with_parent(ca, node->as.ret.expr, block_depth, node);
        break;

    case NODE_SEND:
    case NODE_SUPER_SEND: {
        capture_walk_with_parent(ca, node->as.send.receiver, block_depth, node);
        for (uint32_t i = 0; i < node->as.send.arg_count; i++)
            capture_walk_with_parent(ca, node->as.send.args[i], block_depth, node);
        break;
    }

    case NODE_CASCADE:
        capture_walk_with_parent(ca, node->as.cascade.receiver, block_depth, node);
        for (uint32_t i = 0; i < node->as.cascade.msg_count; i++) {
            STA_CascadeMsg *m = &node->as.cascade.messages[i];
            for (uint32_t j = 0; j < m->arg_count; j++)
                capture_walk_with_parent(ca, m->args[j], block_depth, node);
        }
        break;

    case NODE_BLOCK:
        /* If this block is inlined as part of an inlinable send, it doesn't
         * create a real closure — treat it at same depth. */
        if (is_inlined_block(node, parent)) {
            capture_walk_stmts(ca, node->as.method.body,
                               node->as.method.body_count, block_depth);
        } else {
            /* Real closure block — increase depth.
             * Push this block's params/temps into block scope so nested
             * blocks can detect captures from enclosing blocks. */
            int new_depth = block_depth + 1;
            uint32_t saved_scope_count = ca->block_scope_count;
            for (uint32_t i = 0; i < node->as.method.arg_count; i++)
                ca_push_scope(ca, node->as.method.args[i], new_depth);
            for (uint32_t i = 0; i < node->as.method.temp_count; i++)
                ca_push_scope(ca, node->as.method.temps[i], new_depth);
            capture_walk_stmts(ca, node->as.method.body,
                               node->as.method.body_count, new_depth);
            ca->block_scope_count = saved_scope_count;
        }
        break;

    default:
        break;
    }
}

static void capture_walk(CaptureAnalysis *ca, STA_AstNode *node,
                          int block_depth) {
    capture_walk_with_parent(ca, node, block_depth, NULL);
}

/* Top-level analysis: does this method need a context object? */
static void analyze_captures(CaptureAnalysis *ca, STA_AstNode *method_ast) {
    ca->has_captures = false;
    ca->has_nlr = false;
    ca->block_scope = NULL;
    ca->block_scope_count = 0;
    ca->block_scope_capacity = 0;

    /* Build the list of method-level temp names (args + declared temps). */
    uint32_t total = method_ast->as.method.arg_count + method_ast->as.method.temp_count;
    const char **names = NULL;
    if (total > 0) {
        names = malloc(total * sizeof(char *));
        if (!names) return;
        for (uint32_t i = 0; i < method_ast->as.method.arg_count; i++)
            names[i] = method_ast->as.method.args[i];
        for (uint32_t i = 0; i < method_ast->as.method.temp_count; i++)
            names[method_ast->as.method.arg_count + i] = method_ast->as.method.temps[i];
    }
    ca->method_temps = names;
    ca->method_temp_count = total;

    /* Walk the body. We need to pass parent context for inlined-block detection.
     * Walk each statement at method level (depth=0). For sends with block args,
     * the capture_walk_with_parent handles inlining detection. */
    for (uint32_t i = 0; i < method_ast->as.method.body_count; i++)
        capture_walk_with_parent(ca, method_ast->as.method.body[i], 0, NULL);

    free(names);
    ca->method_temps = NULL;
    free(ca->block_scope);
    ca->block_scope = NULL;
}

/* ── Codegen state ───────────────────────────────────────────────────── */

typedef struct {
    STA_CodegenContext *ctx;
    ByteBuf   code;
    LitBuf    literals;

    /* Temp index mapping: args come first, then method temps,
     * then block temps for any inline blocks. */
    char    **temp_names;
    uint32_t  temp_count;
    uint32_t  temp_capacity;

    uint32_t  num_args;      /* method arg count */
    uint32_t  num_method_temps; /* declared method temps (excluding args) */
    uint32_t  peak_temp_count; /* track max temp_count for numTemps header */

    /* Closure support (Phase 2). */
    bool     needs_context;   /* method needs a heap context object */
    bool     has_nlr;         /* method contains non-local return in a block */
    int      block_depth;     /* 0 = method level, >0 = inside real block */
} Codegen;

static void cg_init(Codegen *cg, STA_CodegenContext *ctx) {
    cg->ctx = ctx;
    bb_init(&cg->code);
    lb_init(&cg->literals);
    cg->temp_names = NULL;
    cg->temp_count = 0;
    cg->temp_capacity = 0;
    cg->num_args = 0;
    cg->num_method_temps = 0;
    cg->peak_temp_count = 0;
    cg->needs_context = false;
    cg->has_nlr = false;
    cg->block_depth = 0;
}

static void cg_free(Codegen *cg) {
    bb_free(&cg->code);
    lb_free(&cg->literals);
    for (uint32_t i = 0; i < cg->temp_count; i++)
        free(cg->temp_names[i]);
    free(cg->temp_names);
    cg->temp_names = NULL;
    cg->temp_count = cg->temp_capacity = 0;
}

static void cg_error(Codegen *cg, const char *msg) {
    if (cg->ctx->had_error) return;
    cg->ctx->had_error = true;
    snprintf(cg->ctx->error_msg, sizeof(cg->ctx->error_msg), "%s", msg);
}

static int cg_add_temp(Codegen *cg, const char *name) {
    if (cg->temp_count == cg->temp_capacity) {
        uint32_t cap = cg->temp_capacity ? cg->temp_capacity * 2 : 16;
        char **p = realloc(cg->temp_names, cap * sizeof(char *));
        if (!p) return -1;
        cg->temp_names = p;
        cg->temp_capacity = cap;
    }
    cg->temp_names[cg->temp_count] = strdup(name);
    int idx = (int)cg->temp_count++;
    if (cg->temp_count > cg->peak_temp_count)
        cg->peak_temp_count = cg->temp_count;
    return idx;
}

static int cg_find_temp(Codegen *cg, const char *name) {
    for (uint32_t i = 0; i < cg->temp_count; i++) {
        if (strcmp(cg->temp_names[i], name) == 0) return (int)i;
    }
    return -1;
}

static int cg_find_instvar(Codegen *cg, const char *name) {
    for (uint32_t i = 0; i < cg->ctx->instvar_count; i++) {
        if (strcmp(cg->ctx->instvar_names[i], name) == 0) return (int)i;
    }
    return -1;
}

/* Intern a selector and add it to the literal frame. */
static uint16_t cg_intern_selector(Codegen *cg, const char *sel) {
    STA_OOP sym = sta_symbol_intern(cg->ctx->immutable_space,
                                     cg->ctx->symbol_table,
                                     sel, strlen(sel));
    if (sym == 0) {
        cg_error(cg, "failed to intern selector");
        return UINT16_MAX;
    }
    uint16_t idx = lb_add(&cg->literals, sym);
    if (idx == UINT16_MAX) {
        cg_error(cg, "out of memory adding literal");
    }
    return idx;
}

/* Look up a global Association from the SystemDictionary. */
static STA_OOP cg_lookup_global(Codegen *cg, const char *name) {
    if (cg->ctx->system_dict == 0) return 0;

    STA_OOP name_sym = sta_symbol_intern(cg->ctx->immutable_space,
                                          cg->ctx->symbol_table,
                                          name, strlen(name));
    if (name_sym == 0) return 0;

    /* Walk the SystemDictionary's backing array looking for the association
     * whose key matches name_sym. SystemDictionary has the same layout as
     * MethodDictionary: slot 0 = tally, slot 1 = backing array.
     * Backing array: [key0, assoc0, key1, assoc1, ...] */
    STA_ObjHeader *dh = (STA_ObjHeader *)(uintptr_t)cg->ctx->system_dict;
    STA_OOP *dp = sta_payload(dh);
    STA_OOP arr = dp[1];
    if (arr == 0 || STA_IS_SMALLINT(arr)) return 0;

    STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)arr;
    STA_OOP *ap = sta_payload(ah);
    uint32_t cap = ah->size;

    for (uint32_t i = 0; i < cap; i += 2) {
        if (ap[i] == name_sym) {
            return ap[i + 1]; /* the Association OOP */
        }
    }
    return 0;
}

/* ── Forward declarations ────────────────────────────────────────────── */

static void emit_node(Codegen *cg, STA_AstNode *node, bool for_value);
static void emit_statements(Codegen *cg, STA_AstNode **stmts, uint32_t count,
                             bool for_value);

/* ── Inline control structure detection ──────────────────────────────── */

/* Check if a node is a block literal (can be inlined). */
static bool is_block_literal(STA_AstNode *node) {
    return node && node->type == NODE_BLOCK;
}

/* Check if a send is an inlinable control structure. */
static bool is_inlinable_if(const char *sel) {
    return strcmp(sel, "ifTrue:") == 0 ||
           strcmp(sel, "ifFalse:") == 0 ||
           strcmp(sel, "ifTrue:ifFalse:") == 0 ||
           strcmp(sel, "ifFalse:ifTrue:") == 0;
}

static bool is_inlinable_loop(const char *sel) {
    return strcmp(sel, "whileTrue:") == 0 ||
           strcmp(sel, "whileFalse:") == 0 ||
           strcmp(sel, "whileTrue") == 0 ||
           strcmp(sel, "whileFalse") == 0;
}

static bool is_inlinable_logic(const char *sel) {
    return strcmp(sel, "and:") == 0 ||
           strcmp(sel, "or:") == 0;
}

static bool is_inlinable_times_repeat(const char *sel) {
    return strcmp(sel, "timesRepeat:") == 0;
}

/* ── Emit helpers ────────────────────────────────────────────────────── */

/* Emit block body inline (for inlined control structures).
 * The block node's body statements are emitted directly. */
static void emit_block_body_inline(Codegen *cg, STA_AstNode *block,
                                    bool for_value) {
    if (block->as.method.body_count == 0) {
        if (for_value)
            bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
        return;
    }
    /* Allocate temp indices for block temps (not args for clean inline). */
    uint32_t saved_temp_count = cg->temp_count;

    /* For inlined blocks, block args are NOT separate temps — the control
     * structure manages the value. Block temps, however, need slots. */
    for (uint32_t i = 0; i < block->as.method.temp_count; i++) {
        cg_add_temp(cg, block->as.method.temps[i]);
    }

    emit_statements(cg, block->as.method.body, block->as.method.body_count,
                     for_value);

    /* Restore temp count (block temps go out of scope). */
    for (uint32_t i = saved_temp_count; i < cg->temp_count; i++)
        free(cg->temp_names[i]);
    cg->temp_count = saved_temp_count;
}

/* ── Emit inlined control structures per §5.6 ────────────────────────── */

static bool try_emit_inlined_if(Codegen *cg, STA_AstNode *node,
                                 bool for_value) {
    const char *sel = node->as.send.selector;

    if (strcmp(sel, "ifTrue:") == 0 && node->as.send.arg_count == 1 &&
        is_block_literal(node->as.send.args[0])) {
        /* receiver (condition already on stack) */
        emit_node(cg, node->as.send.receiver, true);

        uint32_t jump_false_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_FALSE, 0x00); /* placeholder */

        emit_block_body_inline(cg, node->as.send.args[0], for_value);

        if (for_value) {
            /* Jump past the nil push. */
            uint32_t jump_end_pc = cg->code.count;
            bb_emit(&cg->code, OP_JUMP, 0x00);
            bb_patch_jump(&cg->code, jump_false_pc, cg->code.count);
            bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
            bb_patch_jump(&cg->code, jump_end_pc, cg->code.count);
        } else {
            bb_patch_jump(&cg->code, jump_false_pc, cg->code.count);
        }
        return true;
    }

    if (strcmp(sel, "ifFalse:") == 0 && node->as.send.arg_count == 1 &&
        is_block_literal(node->as.send.args[0])) {
        emit_node(cg, node->as.send.receiver, true);

        uint32_t jump_true_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_TRUE, 0x00);

        emit_block_body_inline(cg, node->as.send.args[0], for_value);

        if (for_value) {
            uint32_t jump_end_pc = cg->code.count;
            bb_emit(&cg->code, OP_JUMP, 0x00);
            bb_patch_jump(&cg->code, jump_true_pc, cg->code.count);
            bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
            bb_patch_jump(&cg->code, jump_end_pc, cg->code.count);
        } else {
            bb_patch_jump(&cg->code, jump_true_pc, cg->code.count);
        }
        return true;
    }

    if (strcmp(sel, "ifTrue:ifFalse:") == 0 && node->as.send.arg_count == 2 &&
        is_block_literal(node->as.send.args[0]) &&
        is_block_literal(node->as.send.args[1])) {
        emit_node(cg, node->as.send.receiver, true);

        uint32_t jump_false_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_FALSE, 0x00);

        emit_block_body_inline(cg, node->as.send.args[0], for_value);

        uint32_t jump_end_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP, 0x00);

        bb_patch_jump(&cg->code, jump_false_pc, cg->code.count);

        emit_block_body_inline(cg, node->as.send.args[1], for_value);

        bb_patch_jump(&cg->code, jump_end_pc, cg->code.count);
        return true;
    }

    if (strcmp(sel, "ifFalse:ifTrue:") == 0 && node->as.send.arg_count == 2 &&
        is_block_literal(node->as.send.args[0]) &&
        is_block_literal(node->as.send.args[1])) {
        emit_node(cg, node->as.send.receiver, true);

        uint32_t jump_false_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_FALSE, 0x00);

        /* true branch is args[1] for ifFalse:ifTrue: */
        emit_block_body_inline(cg, node->as.send.args[1], for_value);

        uint32_t jump_end_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP, 0x00);

        bb_patch_jump(&cg->code, jump_false_pc, cg->code.count);

        emit_block_body_inline(cg, node->as.send.args[0], for_value);

        bb_patch_jump(&cg->code, jump_end_pc, cg->code.count);
        return true;
    }

    return false;
}

static bool try_emit_inlined_loop(Codegen *cg, STA_AstNode *node,
                                   bool for_value) {
    const char *sel = node->as.send.selector;

    /* The receiver of whileTrue:/whileFalse: must be a block literal. */
    if (!is_block_literal(node->as.send.receiver)) return false;

    if (strcmp(sel, "whileTrue:") == 0 && node->as.send.arg_count == 1 &&
        is_block_literal(node->as.send.args[0])) {
        uint32_t loop_top = cg->code.count;

        emit_block_body_inline(cg, node->as.send.receiver, true);

        uint32_t jump_false_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_FALSE, 0x00);

        emit_block_body_inline(cg, node->as.send.args[0], false);

        /* OP_JUMP_BACK: operand = distance from after-this-instruction to loop_top. */
        uint32_t back_pc = cg->code.count + 2; /* pc after JUMP_BACK */
        uint32_t back_offset = back_pc - loop_top;
        bb_emit_wide(&cg->code, OP_JUMP_BACK, (uint16_t)back_offset);

        bb_patch_jump(&cg->code, jump_false_pc, cg->code.count);

        if (for_value) bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
        return true;
    }

    if (strcmp(sel, "whileFalse:") == 0 && node->as.send.arg_count == 1 &&
        is_block_literal(node->as.send.args[0])) {
        uint32_t loop_top = cg->code.count;

        emit_block_body_inline(cg, node->as.send.receiver, true);

        uint32_t jump_true_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_TRUE, 0x00);

        emit_block_body_inline(cg, node->as.send.args[0], false);

        uint32_t back_pc = cg->code.count + 2;
        uint32_t back_offset = back_pc - loop_top;
        bb_emit_wide(&cg->code, OP_JUMP_BACK, (uint16_t)back_offset);

        bb_patch_jump(&cg->code, jump_true_pc, cg->code.count);

        if (for_value) bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
        return true;
    }

    if (strcmp(sel, "whileTrue") == 0 && node->as.send.arg_count == 0) {
        uint32_t loop_top = cg->code.count;

        emit_block_body_inline(cg, node->as.send.receiver, true);

        uint32_t jump_false_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_FALSE, 0x00);

        uint32_t back_pc = cg->code.count + 2;
        uint32_t back_offset = back_pc - loop_top;
        bb_emit_wide(&cg->code, OP_JUMP_BACK, (uint16_t)back_offset);

        bb_patch_jump(&cg->code, jump_false_pc, cg->code.count);

        if (for_value) bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
        return true;
    }

    if (strcmp(sel, "whileFalse") == 0 && node->as.send.arg_count == 0) {
        uint32_t loop_top = cg->code.count;

        emit_block_body_inline(cg, node->as.send.receiver, true);

        uint32_t jump_true_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_TRUE, 0x00);

        uint32_t back_pc = cg->code.count + 2;
        uint32_t back_offset = back_pc - loop_top;
        bb_emit_wide(&cg->code, OP_JUMP_BACK, (uint16_t)back_offset);

        bb_patch_jump(&cg->code, jump_true_pc, cg->code.count);

        if (for_value) bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
        return true;
    }

    return false;
}

static bool try_emit_inlined_logic(Codegen *cg, STA_AstNode *node,
                                    bool for_value) {
    const char *sel = node->as.send.selector;
    if (node->as.send.arg_count != 1 ||
        !is_block_literal(node->as.send.args[0]))
        return false;

    if (strcmp(sel, "and:") == 0) {
        emit_node(cg, node->as.send.receiver, true);

        uint32_t jump_false_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_FALSE, 0x00);

        emit_block_body_inline(cg, node->as.send.args[0], true);

        uint32_t jump_end_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP, 0x00);

        bb_patch_jump(&cg->code, jump_false_pc, cg->code.count);
        bb_emit(&cg->code, OP_PUSH_FALSE, 0x00);

        bb_patch_jump(&cg->code, jump_end_pc, cg->code.count);

        if (!for_value) bb_emit(&cg->code, OP_POP, 0x00);
        return true;
    }

    if (strcmp(sel, "or:") == 0) {
        emit_node(cg, node->as.send.receiver, true);

        uint32_t jump_true_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP_TRUE, 0x00);

        emit_block_body_inline(cg, node->as.send.args[0], true);

        uint32_t jump_end_pc = cg->code.count;
        bb_emit(&cg->code, OP_JUMP, 0x00);

        bb_patch_jump(&cg->code, jump_true_pc, cg->code.count);
        bb_emit(&cg->code, OP_PUSH_TRUE, 0x00);

        bb_patch_jump(&cg->code, jump_end_pc, cg->code.count);

        if (!for_value) bb_emit(&cg->code, OP_POP, 0x00);
        return true;
    }

    return false;
}

static bool try_emit_inlined_times_repeat(Codegen *cg, STA_AstNode *node,
                                           bool for_value) {
    if (node->as.send.arg_count != 1 ||
        !is_block_literal(node->as.send.args[0]))
        return false;

    /* receiver timesRepeat: [ body ]
     * Emit as: | count i | count := receiver. i := 1.
     * [i <= count] whileTrue: [ body. i := i + 1 ] */
    emit_node(cg, node->as.send.receiver, true);

    /* Allocate hidden temps for count and i. */
    int count_idx = cg_add_temp(cg, "__tr_count");
    int i_idx = cg_add_temp(cg, "__tr_i");
    bb_emit_wide(&cg->code, OP_POP_STORE_TEMP, (uint16_t)count_idx);
    bb_emit(&cg->code, OP_PUSH_ONE, 0x00);
    bb_emit_wide(&cg->code, OP_POP_STORE_TEMP, (uint16_t)i_idx);

    uint32_t loop_top = cg->code.count;

    bb_emit_wide(&cg->code, OP_PUSH_TEMP, (uint16_t)i_idx);
    bb_emit_wide(&cg->code, OP_PUSH_TEMP, (uint16_t)count_idx);
    uint16_t lte_sel = cg_intern_selector(cg, "<=");
    bb_emit_wide(&cg->code, OP_SEND, lte_sel);

    uint32_t jump_false_pc = cg->code.count;
    bb_emit(&cg->code, OP_JUMP_FALSE, 0x00);

    emit_block_body_inline(cg, node->as.send.args[0], false);

    /* i := i + 1 */
    bb_emit_wide(&cg->code, OP_PUSH_TEMP, (uint16_t)i_idx);
    bb_emit(&cg->code, OP_PUSH_ONE, 0x00);
    uint16_t plus_sel = cg_intern_selector(cg, "+");
    bb_emit_wide(&cg->code, OP_SEND, plus_sel);
    bb_emit_wide(&cg->code, OP_POP_STORE_TEMP, (uint16_t)i_idx);

    uint32_t back_pc = cg->code.count + 2;
    uint32_t back_offset = back_pc - loop_top;
    bb_emit_wide(&cg->code, OP_JUMP_BACK, (uint16_t)back_offset);

    bb_patch_jump(&cg->code, jump_false_pc, cg->code.count);

    if (for_value) bb_emit(&cg->code, OP_PUSH_NIL, 0x00);

    /* Remove hidden temps. */
    free(cg->temp_names[cg->temp_count - 1]);
    free(cg->temp_names[cg->temp_count - 2]);
    cg->temp_count -= 2;

    return true;
}

/* ── Emit nodes ──────────────────────────────────────────────────────── */

static void emit_literal_int(Codegen *cg, int64_t value) {
    if (value == -1) {
        bb_emit(&cg->code, OP_PUSH_MINUS_ONE, 0x00);
    } else if (value == 0) {
        bb_emit(&cg->code, OP_PUSH_ZERO, 0x00);
    } else if (value == 1) {
        bb_emit(&cg->code, OP_PUSH_ONE, 0x00);
    } else if (value == 2) {
        bb_emit(&cg->code, OP_PUSH_TWO, 0x00);
    } else if (value >= 0 && value <= 65535) {
        bb_emit_wide(&cg->code, OP_PUSH_SMALLINT, (uint16_t)value);
    } else {
        /* Large integer — put in literal frame as SmallInt OOP. */
        STA_OOP oop = STA_SMALLINT_OOP(value);
        uint16_t idx = lb_add(&cg->literals, oop);
        bb_emit_wide(&cg->code, OP_PUSH_LIT, idx);
    }
}

static void emit_variable(Codegen *cg, STA_AstNode *node) {
    const char *name = node->as.variable.name;

    /* Check temps/args first. */
    int idx = cg_find_temp(cg, name);
    if (idx >= 0) {
        bb_emit_wide(&cg->code, OP_PUSH_TEMP, (uint16_t)idx);
        return;
    }

    /* Check instance variables. */
    idx = cg_find_instvar(cg, name);
    if (idx >= 0) {
        bb_emit_wide(&cg->code, OP_PUSH_INSTVAR, (uint16_t)idx);
        return;
    }

    /* Check globals (capitalized names). */
    if (name[0] >= 'A' && name[0] <= 'Z') {
        STA_OOP assoc = cg_lookup_global(cg, name);
        if (assoc != 0) {
            uint16_t lit_idx = lb_add(&cg->literals, assoc);
            bb_emit_wide(&cg->code, OP_PUSH_GLOBAL, lit_idx);
            return;
        }
    }

    /* Unknown variable. */
    char msg[300];
    snprintf(msg, sizeof(msg), "undeclared variable '%s'", name);
    cg_error(cg, msg);
}

static void emit_assignment(Codegen *cg, STA_AstNode *node, bool for_value) {
    const char *name = node->as.assign.variable->as.variable.name;

    /* Emit the value. */
    emit_node(cg, node->as.assign.value, true);

    /* Determine store target. */
    int idx = cg_find_temp(cg, name);
    if (idx >= 0) {
        if (for_value)
            bb_emit_wide(&cg->code, OP_STORE_TEMP, (uint16_t)idx);
        else
            bb_emit_wide(&cg->code, OP_POP_STORE_TEMP, (uint16_t)idx);
        return;
    }

    idx = cg_find_instvar(cg, name);
    if (idx >= 0) {
        if (for_value)
            bb_emit_wide(&cg->code, OP_STORE_INSTVAR, (uint16_t)idx);
        else
            bb_emit_wide(&cg->code, OP_POP_STORE_INSTVAR, (uint16_t)idx);
        return;
    }

    /* Global assignment. */
    if (name[0] >= 'A' && name[0] <= 'Z') {
        STA_OOP assoc = cg_lookup_global(cg, name);
        if (assoc != 0) {
            uint16_t lit_idx = lb_add(&cg->literals, assoc);
            if (for_value)
                bb_emit_wide(&cg->code, OP_STORE_GLOBAL, lit_idx);
            else
                bb_emit_wide(&cg->code, OP_POP_STORE_GLOBAL, lit_idx);
            return;
        }
    }

    char msg[300];
    snprintf(msg, sizeof(msg), "cannot assign to undeclared variable '%s'",
             name);
    cg_error(cg, msg);
}

static void emit_send(Codegen *cg, STA_AstNode *node, bool for_value) {
    /* Check for inlinable control structures. */
    if (node->type == NODE_SEND) {
        if (is_inlinable_if(node->as.send.selector) &&
            try_emit_inlined_if(cg, node, for_value))
            return;
        if (is_inlinable_loop(node->as.send.selector) &&
            try_emit_inlined_loop(cg, node, for_value))
            return;
        if (is_inlinable_logic(node->as.send.selector) &&
            try_emit_inlined_logic(cg, node, for_value))
            return;
        if (is_inlinable_times_repeat(node->as.send.selector) &&
            try_emit_inlined_times_repeat(cg, node, for_value))
            return;
    }

    /* Normal send. Emit receiver. */
    if (node->type == NODE_SUPER_SEND) {
        bb_emit(&cg->code, OP_PUSH_RECEIVER, 0x00);
    } else {
        emit_node(cg, node->as.send.receiver, true);
    }

    /* Emit arguments. */
    for (uint32_t i = 0; i < node->as.send.arg_count; i++) {
        emit_node(cg, node->as.send.args[i], true);
    }

    /* Emit send. */
    uint16_t sel_idx = cg_intern_selector(cg, node->as.send.selector);
    if (node->type == NODE_SUPER_SEND)
        bb_emit_wide(&cg->code, OP_SEND_SUPER, sel_idx);
    else
        bb_emit_wide(&cg->code, OP_SEND, sel_idx);

    if (!for_value) bb_emit(&cg->code, OP_POP, 0x00);
}

static void emit_cascade(Codegen *cg, STA_AstNode *node, bool for_value) {
    /* Emit receiver once. */
    emit_node(cg, node->as.cascade.receiver, true);

    for (uint32_t i = 0; i < node->as.cascade.msg_count; i++) {
        STA_CascadeMsg *m = &node->as.cascade.messages[i];
        bool is_last = (i == node->as.cascade.msg_count - 1);

        if (!is_last) {
            bb_emit(&cg->code, OP_DUP, 0x00);
        }

        /* Push arguments. */
        for (uint32_t j = 0; j < m->arg_count; j++) {
            emit_node(cg, m->args[j], true);
        }

        uint16_t sel_idx = cg_intern_selector(cg, m->selector);
        bb_emit_wide(&cg->code, OP_SEND, sel_idx);

        if (!is_last) {
            bb_emit(&cg->code, OP_POP, 0x00);
        }
    }

    if (!for_value) bb_emit(&cg->code, OP_POP, 0x00);
}

static void emit_block(Codegen *cg, STA_AstNode *node) {
    /* Create a block descriptor and add it to the literal frame.
     * The block body is emitted inline and the interpreter jumps past it.
     *
     * If the method needs a context (cg->needs_context), emit OP_CLOSURE_COPY
     * instead of OP_BLOCK_COPY. The difference:
     * - OP_BLOCK_COPY: clean block (no captures). BlockClosure has 5 slots.
     * - OP_CLOSURE_COPY: capturing block. BlockClosure has 6 slots (adds context).
     *   BlockDescriptor has 5 slots (adds numContext for context temp count). */

    /* Reserve literal slot for the block descriptor (we'll patch it). */
    uint16_t desc_idx = lb_add_unique(&cg->literals, 0); /* placeholder */

    /* Choose opcode based on whether the method has captures. */
    bool use_closure = cg->needs_context;
    uint8_t copy_op = use_closure ? OP_CLOSURE_COPY : OP_BLOCK_COPY;
    bb_emit_wide(&cg->code, copy_op, desc_idx);

    /* Record block body start. */
    uint32_t body_start = cg->code.count;

    /* Allocate temp indices for block args and temps. */
    uint32_t saved_temp_count = cg->temp_count;
    int saved_depth = cg->block_depth;
    cg->block_depth++;

    for (uint32_t i = 0; i < node->as.method.arg_count; i++) {
        cg_add_temp(cg, node->as.method.args[i]);
    }
    for (uint32_t i = 0; i < node->as.method.temp_count; i++) {
        cg_add_temp(cg, node->as.method.temps[i]);
    }

    /* Emit block body. */
    if (node->as.method.body_count == 0) {
        bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
    } else {
        emit_statements(cg, node->as.method.body, node->as.method.body_count,
                         true);
    }
    bb_emit(&cg->code, OP_RETURN_TOP, 0x00);

    uint32_t body_end = cg->code.count;
    uint32_t body_length = body_end - body_start;

    /* Restore temp scope and block depth. */
    for (uint32_t i = saved_temp_count; i < cg->temp_count; i++)
        free(cg->temp_names[i]);
    cg->temp_count = saved_temp_count;
    cg->block_depth = saved_depth;

    /* Create BlockDescriptor object. */
    uint32_t desc_slots = use_closure ? 5 : 4;
    STA_ObjHeader *bd = sta_heap_alloc(cg->ctx->heap,
                                        STA_CLS_BLOCKDESCRIPTOR, desc_slots);
    if (!bd) {
        cg_error(cg, "failed to allocate block descriptor");
        return;
    }
    STA_OOP *bdp = sta_payload(bd);
    bdp[0] = STA_SMALLINT_OOP(body_start);
    bdp[1] = STA_SMALLINT_OOP(body_length);
    bdp[2] = STA_SMALLINT_OOP(node->as.method.arg_count);
    bdp[3] = STA_SMALLINT_OOP(saved_temp_count);
    if (use_closure) {
        /* numContext: total number of temps in the method's context object.
         * This is the method's peak temp count (all temps go in the context
         * when the method has any captures). */
        bdp[4] = STA_SMALLINT_OOP(cg->num_args + cg->num_method_temps);
    }

    /* Patch the literal slot. */
    cg->literals.items[desc_idx] = (STA_OOP)(uintptr_t)bd;
}

static void emit_return(Codegen *cg, STA_AstNode *node) {
    STA_AstNode *expr = node->as.ret.expr;

    /* If we're inside a real block (block_depth > 0), this is a non-local
     * return. Emit the value then OP_NON_LOCAL_RETURN. */
    if (cg->block_depth > 0) {
        emit_node(cg, expr, true);
        bb_emit(&cg->code, OP_NON_LOCAL_RETURN, 0x00);
        return;
    }

    /* Optimized returns for common constants (method level only). */
    if (expr->type == NODE_LITERAL_NIL) {
        bb_emit(&cg->code, OP_RETURN_NIL, 0x00);
        return;
    }
    if (expr->type == NODE_LITERAL_BOOL) {
        if (expr->as.boolean.value)
            bb_emit(&cg->code, OP_RETURN_TRUE, 0x00);
        else
            bb_emit(&cg->code, OP_RETURN_FALSE, 0x00);
        return;
    }
    if (expr->type == NODE_SELF) {
        bb_emit(&cg->code, OP_RETURN_SELF, 0x00);
        return;
    }

    emit_node(cg, expr, true);
    bb_emit(&cg->code, OP_RETURN_TOP, 0x00);
}

static void emit_node(Codegen *cg, STA_AstNode *node, bool for_value) {
    if (cg->ctx->had_error) return;

    switch (node->type) {
    case NODE_SELF:
        if (for_value) bb_emit(&cg->code, OP_PUSH_RECEIVER, 0x00);
        break;

    case NODE_LITERAL_NIL:
        if (for_value) bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
        break;

    case NODE_LITERAL_BOOL:
        if (for_value) {
            if (node->as.boolean.value)
                bb_emit(&cg->code, OP_PUSH_TRUE, 0x00);
            else
                bb_emit(&cg->code, OP_PUSH_FALSE, 0x00);
        }
        break;

    case NODE_LITERAL_INT:
        if (for_value) emit_literal_int(cg, node->as.integer.value);
        break;

    case NODE_LITERAL_FLOAT:
        if (for_value) {
            /* Float objects are not yet supported in Phase 1 heap.
             * Store as a tagged representation or literal. For now,
             * we use the literal frame with a placeholder. */
            /* TODO: proper Float object creation. For now, error. */
            cg_error(cg, "float literals not yet supported");
        }
        break;

    case NODE_LITERAL_STRING:
        if (for_value) {
            /* Allocate a real String object (class_index = STA_CLS_STRING)
             * in immutable space. Symbols and Strings are distinct types —
             * Symbols have their own printString, hash behaviour, etc. */
            const char *str = node->as.variable.name;
            size_t len = strlen(str);
            uint32_t var_words = ((uint32_t)len + (uint32_t)(sizeof(STA_OOP) - 1))
                                 / (uint32_t)sizeof(STA_OOP);
            STA_ObjHeader *str_h = sta_immutable_alloc(
                cg->ctx->immutable_space, STA_CLS_STRING, var_words);
            if (!str_h) {
                cg_error(cg, "failed to allocate string literal");
                break;
            }
            uint8_t padding = (uint8_t)(var_words * sizeof(STA_OOP) - (uint32_t)len);
            str_h->reserved = padding;
            memset(sta_payload(str_h), 0, var_words * sizeof(STA_OOP));
            memcpy(sta_payload(str_h), str, len);
            STA_OOP str_oop = (STA_OOP)(uintptr_t)str_h;
            uint16_t idx = lb_add(&cg->literals, str_oop);
            bb_emit_wide(&cg->code, OP_PUSH_LIT, idx);
        }
        break;

    case NODE_LITERAL_SYMBOL:
        if (for_value) {
            const char *name = node->as.variable.name;
            STA_OOP sym = sta_symbol_intern(cg->ctx->immutable_space,
                                             cg->ctx->symbol_table,
                                             name, strlen(name));
            if (sym == 0) {
                cg_error(cg, "failed to intern symbol");
                break;
            }
            uint16_t idx = lb_add(&cg->literals, sym);
            bb_emit_wide(&cg->code, OP_PUSH_LIT, idx);
        }
        break;

    case NODE_LITERAL_CHAR:
        if (for_value) {
            STA_OOP ch = STA_CHAR_OOP((uint32_t)(unsigned char)node->as.character.value);
            uint16_t idx = lb_add(&cg->literals, ch);
            bb_emit_wide(&cg->code, OP_PUSH_LIT, idx);
        }
        break;

    case NODE_LITERAL_ARRAY:
        if (for_value) {
            /* Create an Array object in immutable space.
             * For Phase 1, allocate on heap. */
            uint32_t count = node->as.array.count;
            STA_ObjHeader *ah = sta_heap_alloc(cg->ctx->heap,
                                                STA_CLS_ARRAY, count);
            if (!ah) {
                cg_error(cg, "failed to allocate literal array");
                break;
            }
            STA_OOP *elems = sta_payload(ah);
            for (uint32_t i = 0; i < count; i++) {
                STA_AstNode *e = node->as.array.elements[i];
                switch (e->type) {
                case NODE_LITERAL_INT:
                    elems[i] = STA_SMALLINT_OOP(e->as.integer.value);
                    break;
                case NODE_LITERAL_BOOL:
                    elems[i] = e->as.boolean.value
                        ? sta_spc_get(SPC_TRUE)
                        : sta_spc_get(SPC_FALSE);
                    break;
                case NODE_LITERAL_NIL:
                    elems[i] = sta_spc_get(SPC_NIL);
                    break;
                case NODE_LITERAL_SYMBOL: {
                    STA_OOP s = sta_symbol_intern(
                        cg->ctx->immutable_space, cg->ctx->symbol_table,
                        e->as.variable.name, strlen(e->as.variable.name));
                    elems[i] = s;
                    break;
                }
                case NODE_LITERAL_STRING: {
                    STA_OOP s = sta_symbol_intern(
                        cg->ctx->immutable_space, cg->ctx->symbol_table,
                        e->as.variable.name, strlen(e->as.variable.name));
                    elems[i] = s;
                    break;
                }
                case NODE_LITERAL_CHAR:
                    elems[i] = STA_CHAR_OOP(
                        (uint32_t)(unsigned char)e->as.character.value);
                    break;
                default:
                    elems[i] = sta_spc_get(SPC_NIL);
                    break;
                }
            }
            uint16_t idx = lb_add(&cg->literals, (STA_OOP)(uintptr_t)ah);
            bb_emit_wide(&cg->code, OP_PUSH_LIT, idx);
        }
        break;

    case NODE_VARIABLE:
        if (for_value) emit_variable(cg, node);
        break;

    case NODE_ASSIGN:
        emit_assignment(cg, node, for_value);
        break;

    case NODE_SEND:
    case NODE_SUPER_SEND:
        emit_send(cg, node, for_value);
        break;

    case NODE_CASCADE:
        emit_cascade(cg, node, for_value);
        break;

    case NODE_RETURN:
        emit_return(cg, node);
        break;

    case NODE_BLOCK:
        if (for_value) emit_block(cg, node);
        break;

    case NODE_METHOD:
        cg_error(cg, "unexpected NODE_METHOD in expression position");
        break;
    }
}

static void emit_statements(Codegen *cg, STA_AstNode **stmts, uint32_t count,
                             bool for_value) {
    if (count == 0) {
        if (for_value) bb_emit(&cg->code, OP_PUSH_NIL, 0x00);
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        bool is_last = (i == count - 1);
        bool last_is_return = (stmts[i]->type == NODE_RETURN);

        if (is_last) {
            emit_node(cg, stmts[i], for_value || last_is_return);
        } else {
            /* Non-last statement: emit for side effect, pop if it left a value.
             * Returns don't leave a value (they exit). */
            if (stmts[i]->type == NODE_RETURN) {
                emit_node(cg, stmts[i], true);
                return; /* code after return is dead */
            }
            /* Statement context: assignments use POP_STORE variants when
             * for_value=false. For sends and other expressions, we emit
             * them for value then pop. */
            if (stmts[i]->type == NODE_ASSIGN) {
                emit_node(cg, stmts[i], false);
            } else {
                emit_node(cg, stmts[i], true);
                bb_emit(&cg->code, OP_POP, 0x00);
            }
        }
    }
}

/* ── Public API ──────────────────────────────────────────────────────── */

STA_OOP sta_codegen(STA_AstNode *method_ast, STA_CodegenContext *ctx) {
    if (!method_ast || method_ast->type != NODE_METHOD) {
        ctx->had_error = true;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "codegen requires a NODE_METHOD root");
        return 0;
    }

    /* Phase 2: capture analysis — determine if any blocks capture outer
     * temps or contain non-local returns. */
    CaptureAnalysis ca;
    memset(&ca, 0, sizeof(ca));
    analyze_captures(&ca, method_ast);

    Codegen cg;
    cg_init(&cg, ctx);
    cg.needs_context = ca.has_captures || ca.has_nlr;
    cg.has_nlr = ca.has_nlr;

    /* Set up temp index mapping: args first, then declared temps. */
    cg.num_args = method_ast->as.method.arg_count;
    for (uint32_t i = 0; i < method_ast->as.method.arg_count; i++) {
        cg_add_temp(&cg, method_ast->as.method.args[i]);
    }
    cg.num_method_temps = method_ast->as.method.temp_count;
    for (uint32_t i = 0; i < method_ast->as.method.temp_count; i++) {
        cg_add_temp(&cg, method_ast->as.method.temps[i]);
    }

    /* Emit method body. */
    STA_AstNode **body = method_ast->as.method.body;
    uint32_t body_count = method_ast->as.method.body_count;

    if (body_count == 0) {
        /* Empty body → implicit return self. */
        bb_emit(&cg.code, OP_RETURN_SELF, 0x00);
    } else {
        /* Check if last statement is a return. */
        bool has_explicit_return =
            (body[body_count - 1]->type == NODE_RETURN);

        emit_statements(&cg, body, body_count, !has_explicit_return);

        if (!has_explicit_return && !ctx->had_error) {
            /* Implicit return self: pop the last expression value,
             * then return self. But actually the spec says implicit
             * return pushes self then returns. The last expression
             * value should be discarded. Let's just pop whatever
             * was left and return self.
             * Actually per §5.7: methods that don't end with ^ emit
             * OP_PUSH_RECEIVER + OP_RETURN_TOP. But we already have
             * the last expression's value on the stack. We need to
             * pop it and push self, or just emit RETURN_SELF.
             * The simplest: pop the last value, emit RETURN_SELF. */
            bb_emit(&cg.code, OP_POP, 0x00);
            bb_emit(&cg.code, OP_RETURN_SELF, 0x00);
        }
    }

    if (cg.code.oom) {
        cg_error(&cg, "out of memory emitting bytecodes");
    }

    if (ctx->had_error) {
        cg_free(&cg);
        return 0;
    }

    /* Add owner class as the last literal (§4.4). */
    lb_add_unique(&cg.literals, ctx->class_oop);

    /* numTemps = peak temp count seen during codegen (includes args).
     * The interpreter expects numTemps to include args: it computes
     * locals = numTemps - numArgs (see interpreter.c OP_SEND handler).
     * peak_temp_count includes args because cg_add_temp is called for
     * args first (lines above), then method temps, then inline block
     * temps — and the peak tracks the high-water mark. */
    uint32_t num_temps = cg.peak_temp_count;

    /* Build the CompiledMethod.
     * If the method needs a context, set largeFrame bit (used as
     * needsContext flag — the interpreter allocates a heap context
     * when this bit is set). */
    uint8_t n_args = (uint8_t)cg.num_args;
    uint8_t n_lits = (uint8_t)cg.literals.count;

    STA_OOP method;
    if (cg.needs_context) {
        /* Build header manually with largeFrame=1 to signal needsContext. */
        STA_OOP header = STA_METHOD_HEADER(n_args, (uint8_t)num_temps,
                                            n_lits, 0, 0, 1);
        method = sta_compiled_method_create_with_header(
            ctx->immutable_space, header,
            cg.literals.items, n_lits,
            cg.code.bytes, cg.code.count);
    } else {
        method = sta_compiled_method_create(
            ctx->immutable_space,
            n_args,
            (uint8_t)num_temps,
            0, /* no primitive index */
            cg.literals.items, n_lits,
            cg.code.bytes, cg.code.count);
    }

    cg_free(&cg);

    if (method == 0) {
        ctx->had_error = true;
        snprintf(ctx->error_msg, sizeof(ctx->error_msg),
                 "failed to allocate CompiledMethod");
    }

    return method;
}
