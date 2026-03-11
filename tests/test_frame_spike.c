/* tests/test_frame_spike.c
 * SPIKE CODE — NOT FOR PRODUCTION
 * Phase 0 spike: activation frame layout and tail-call optimisation (TCO).
 * See docs/spikes/spike-004-frame-layout.md and ADR 010.
 *
 * Test inventory:
 *   1. Layout assertions        (sizeof, offsetof, alignment)
 *   2. Stack push/pop           (chain integrity, canary values)
 *   3. GC walk correctness      (all slots visited exactly once)
 *   4. TCO: constant depth      (1,000,000 countdown, max_depth == 1)
 *   5. No-TCO: stack growth     (1,000 countdown, max_depth == 1,001)
 *   6. Reduction counter        (10,000 countdown → 10,000 decrements)
 *   7. Preemption count         (10,000 countdown, quota=1,000 → 10 preemptions)
 *   8. Frame state preservation (args[0] correct after each preemption)
 *   9. Actor density checkpoint (sizeof table, projected creation cost)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

#include "../src/vm/frame_spike.h"
#include "../src/scheduler/scheduler_spike.h"   /* STA_ActorRevised, for density */

/* ── Test harness ──────────────────────────────────────────────────────────*/

static int g_failures = 0;

#define CHECK(cond, msg)                                                \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "  FAIL: %s\n", (msg));                    \
            g_failures++;                                               \
        } else {                                                        \
            printf("    OK: %s\n", (msg));                              \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b, msg)                                             \
    do {                                                                \
        if ((a) != (b)) {                                               \
            fprintf(stderr, "  FAIL: %s (got %lld, expected %lld)\n",  \
                    (msg), (long long)(a), (long long)(b));             \
            g_failures++;                                               \
        } else {                                                        \
            printf("    OK: %s\n", (msg));                              \
        }                                                               \
    } while (0)

/* ── Stub bytecode arrays ──────────────────────────────────────────────────
 *
 * countdown_tco: tail-recursive countdown (TCO path)
 *
 *   PC= 0: OP_PUSH_ARG 0          push n (arg[0])          [2 bytes]
 *   PC= 2: OP_BRANCH_IF_ZERO 7,0  if n==0 jump to PC=12    [3 bytes]
 *   PC= 5: OP_PUSH_SELF            push self (new receiver)  [1 byte]
 *   PC= 6: OP_PUSH_ARG 0          push n                    [2 bytes]
 *   PC= 8: OP_DEC                  n-1                       [1 byte]
 *   PC= 9: OP_SEND 1               tail call (TCO detected:  [2 bytes]
 *   PC=11: OP_RETURN_TOP            next instr is RETURN_TOP) [1 byte]
 *   PC=12: OP_PUSH_SELF            base case: push self      [1 byte]
 *   PC=13: OP_RETURN_TOP            return self              [1 byte]
 *
 * TCO detection: bytecode[9 + STA_SEND_WIDTH] = bytecode[11] = OP_RETURN_TOP ✓
 * BRANCH target: 2 + 3 + 7 = 12 ✓
 */
static const uint8_t countdown_tco[] = {
    OP_PUSH_ARG,       0,    /* PC= 0 */
    OP_BRANCH_IF_ZERO, 7, 0, /* PC= 2 */
    OP_PUSH_SELF,            /* PC= 5 */
    OP_PUSH_ARG,       0,    /* PC= 6 */
    OP_DEC,                  /* PC= 8 */
    OP_SEND,           1,    /* PC= 9 — TCO: next is OP_RETURN_TOP */
    OP_RETURN_TOP,           /* PC=11 */
    OP_PUSH_SELF,            /* PC=12 — base case */
    OP_RETURN_TOP,           /* PC=13 */
};

/*
 * countdown_notail: non-tail variant — OP_NOOP between SEND and RETURN_TOP
 * breaks the TCO lookahead, so every call pushes a new frame.
 *
 *   PC= 0: OP_PUSH_ARG 0          push n                    [2 bytes]
 *   PC= 2: OP_BRANCH_IF_ZERO 8,0  if n==0 jump to PC=13    [3 bytes]
 *   PC= 5: OP_PUSH_SELF                                      [1 byte]
 *   PC= 6: OP_PUSH_ARG 0                                     [2 bytes]
 *   PC= 8: OP_DEC                                             [1 byte]
 *   PC= 9: OP_SEND 1               normal call               [2 bytes]
 *   PC=11: OP_NOOP                  NOT RETURN_TOP → no TCO  [1 byte]
 *   PC=12: OP_RETURN_TOP                                      [1 byte]
 *   PC=13: OP_PUSH_SELF            base case                  [1 byte]
 *   PC=14: OP_RETURN_TOP                                      [1 byte]
 *
 * BRANCH target: 2 + 3 + 8 = 13 ✓
 * After non-TCO SEND: frame->pc set to 11 (OP_NOOP), not 9+2=11 wait...
 * SEND at PC=9, width=2, so frame->pc = 9+2 = 11 after save. ✓
 * Callee returns: eval_stack has result. OP_NOOP at PC=11: no-op. OP_RETURN_TOP: return.
 */
static const uint8_t countdown_notail[] = {
    OP_PUSH_ARG,       0,    /* PC= 0 */
    OP_BRANCH_IF_ZERO, 8, 0, /* PC= 2 */
    OP_PUSH_SELF,            /* PC= 5 */
    OP_PUSH_ARG,       0,    /* PC= 6 */
    OP_DEC,                  /* PC= 8 */
    OP_SEND,           1,    /* PC= 9 — no TCO: next is OP_NOOP */
    OP_NOOP,                 /* PC=11 */
    OP_RETURN_TOP,           /* PC=12 */
    OP_PUSH_SELF,            /* PC=13 — base case */
    OP_RETURN_TOP,           /* PC=14 */
};

/* ── Fake heap OOP (16-byte aligned, low bit clear) ────────────────────────*/
_Alignas(16) static uint8_t fake_obj_storage[64];

static STA_OOP make_heap_oop(size_t offset) {
    /* offset must keep alignment (multiples of 16 for safety) */
    return (STA_OOP)(uintptr_t)(fake_obj_storage + offset);
}

/* ── Test 1: Layout assertions ─────────────────────────────────────────────*/

static void test_layout(void) {
    printf("\n=== Test 1: Layout assertions ===\n");

    /* Option A: STA_Frame */
    printf("  sizeof(STA_Frame)                 = %zu (expect 40)\n",
           sizeof(STA_Frame));
    printf("  offsetof(method_bytecode)         = %zu (expect  0)\n",
           offsetof(STA_Frame, method_bytecode));
    printf("  offsetof(receiver)                = %zu (expect  8)\n",
           offsetof(STA_Frame, receiver));
    printf("  offsetof(sender_fp)               = %zu (expect 16)\n",
           offsetof(STA_Frame, sender_fp));
    printf("  offsetof(pc)                      = %zu (expect 24)\n",
           offsetof(STA_Frame, pc));
    printf("  offsetof(arg_count)               = %zu (expect 28)\n",
           offsetof(STA_Frame, arg_count));
    printf("  offsetof(local_count)             = %zu (expect 30)\n",
           offsetof(STA_Frame, local_count));
    printf("  offsetof(reduction_hook)          = %zu (expect 32)\n",
           offsetof(STA_Frame, reduction_hook));

    CHECK(sizeof(STA_Frame) == 40u,          "STA_Frame is 40 bytes");
    CHECK(sizeof(STA_Frame) % 8u == 0u,      "STA_Frame size is OOP-aligned (multiple of 8)");
    CHECK(offsetof(STA_Frame, method_bytecode) ==  0u, "method_bytecode at offset 0");
    CHECK(offsetof(STA_Frame, receiver)        ==  8u, "receiver at offset 8");
    CHECK(offsetof(STA_Frame, sender_fp)       == 16u, "sender_fp at offset 16");
    CHECK(offsetof(STA_Frame, pc)              == 24u, "pc at offset 24");
    CHECK(offsetof(STA_Frame, arg_count)       == 28u, "arg_count at offset 28");
    CHECK(offsetof(STA_Frame, local_count)     == 30u, "local_count at offset 30");
    CHECK(offsetof(STA_Frame, reduction_hook)  == 32u, "reduction_hook at offset 32");

    /* Payload alignment: slots immediately after header must be 8-byte aligned.
     * Frame address from slab is malloc-aligned (≥ 16 bytes). Since
     * sizeof(STA_Frame) = 40 (multiple of 8), slots land at base + 40. */
    _Alignas(16) char slab_buf[256];
    STA_Frame *f = (STA_Frame *)slab_buf;
    STA_OOP   *payload = sta_frame_slots(f);
    CHECK(((uintptr_t)payload % 8u) == 0u, "payload OOP slots are 8-byte aligned");

    /* Allocation size helper */
    CHECK(sta_frame_alloc_size(0u)  == 40u,  "alloc_size(0 slots)  = 40");
    CHECK(sta_frame_alloc_size(1u)  == 48u,  "alloc_size(1 slot)   = 48");
    CHECK(sta_frame_alloc_size(5u)  == 80u,  "alloc_size(5 slots)  = 80");
    CHECK(sta_frame_alloc_size(10u) == 120u, "alloc_size(10 slots) = 120");

    /* Option B: STA_FrameAlt */
    printf("  sizeof(STA_FrameAlt)              = %zu (expect 72)\n",
           sizeof(STA_FrameAlt));
    CHECK(sizeof(STA_FrameAlt) == 72u,
          "STA_FrameAlt is 72 bytes (16 hdr alloc unit + 7 * 8 OOP fields)");

    /* SEND instruction lookahead: TCO detection constant */
    CHECK(STA_SEND_WIDTH == 2u, "STA_SEND_WIDTH is 2 (opcode + nargs)");

    /* Verify countdown_tco bytecode: TCO marker at correct offset */
    CHECK(countdown_tco[9 + STA_SEND_WIDTH] == (uint8_t)OP_RETURN_TOP,
          "countdown_tco: SEND+SEND_WIDTH is OP_RETURN_TOP (TCO detected)");

    /* Verify countdown_notail: no TCO at same relative position */
    CHECK(countdown_notail[9 + STA_SEND_WIDTH] == (uint8_t)OP_NOOP,
          "countdown_notail: SEND+SEND_WIDTH is OP_NOOP (no TCO)");
}

/* ── Test 2: Stack push/pop correctness ────────────────────────────────────*/

static void test_push_pop(void) {
    printf("\n=== Test 2: Stack push/pop correctness ===\n");

    STA_FrameSlab slab;
    int rc = sta_frame_slab_init(&slab, 4096);
    CHECK(rc == 0, "slab init succeeds");

    STA_OOP self_oop = make_heap_oop(0);

    /* Push 5 frames with distinct receivers and arg values */
    STA_OOP receivers[5];
    STA_OOP arg_vals[5];
    STA_Frame *frames[5];

    static const uint8_t dummy_bc[] = { OP_HALT };

    STA_Frame *prev = NULL;
    for (int i = 0; i < 5; i++) {
        receivers[i] = STA_SMALLINT_OOP(i + 100);
        arg_vals[i]  = STA_SMALLINT_OOP(i + 200);
        frames[i]    = sta_frame_push(&slab, prev, receivers[i],
                                       dummy_bc, 1u, 1u, &arg_vals[i]);
        prev = frames[i];
    }

    CHECK(slab.depth    == 5u, "depth == 5 after 5 pushes");
    CHECK(slab.max_depth == 5u, "max_depth == 5");

    /* Verify chain is correctly linked */
    CHECK(frames[0]->sender_fp == NULL,       "frame[0] sender_fp is NULL");
    CHECK(frames[1]->sender_fp == frames[0],  "frame[1] sender_fp → frame[0]");
    CHECK(frames[4]->sender_fp == frames[3],  "frame[4] sender_fp → frame[3]");

    /* Verify receivers and arg slots */
    for (int i = 0; i < 5; i++) {
        CHECK(frames[i]->receiver == receivers[i], "receiver OOP correct");
        STA_OOP *slots = sta_frame_slots(frames[i]);
        CHECK(slots[0] == arg_vals[i], "arg[0] correct");
        CHECK(slots[1] == STA_OOP_ZERO, "local[0] zero-initialised");
    }
    (void)self_oop;

    /* Pop all 5 frames (LIFO order) */
    for (int i = 4; i >= 0; i--) {
        sta_frame_pop(&slab, frames[i]);
    }
    CHECK(slab.depth == 0u, "depth == 0 after 5 pops");
    CHECK(slab.used  == 0u, "used  == 0 after 5 pops (full reclaim)");

    /* Push again to confirm slab is reusable */
    STA_OOP arg = STA_SMALLINT_OOP(42);
    STA_Frame *f = sta_frame_push(&slab, NULL, STA_OOP_ZERO, dummy_bc, 1u, 0u, &arg);
    CHECK(f != NULL,                  "slab reusable after full pop");
    CHECK(f->arg_count == 1u,         "arg_count == 1");
    CHECK(sta_frame_slots(f)[0] == STA_SMALLINT_OOP(42), "arg[0] == 42");
    sta_frame_pop(&slab, f);

    sta_frame_slab_destroy(&slab);
}

/* ── Test 3: GC walk correctness ───────────────────────────────────────────*/

/* Visitor that records addresses of visited OOP slots. */
#define MAX_VISITS 64
static STA_OOP *g_visited[MAX_VISITS];
static int      g_visit_count;

static void record_visit(STA_OOP *slot, void *ctx) {
    (void)ctx;
    assert(g_visit_count < MAX_VISITS);
    g_visited[g_visit_count++] = slot;
}

static int ptr_in_visited(STA_OOP *ptr) {
    for (int i = 0; i < g_visit_count; i++) {
        if (g_visited[i] == ptr) return 1;
    }
    return 0;
}

static void test_gc_walk(void) {
    printf("\n=== Test 3: GC walk correctness ===\n");

    STA_FrameSlab slab;
    sta_frame_slab_init(&slab, 4096);

    /* Frame 0 (outermost): receiver=A, arg=B, local=C */
    STA_OOP oop_A = STA_SMALLINT_OOP(1001);
    STA_OOP oop_B = STA_SMALLINT_OOP(1002);
    STA_OOP oop_C = STA_SMALLINT_OOP(1003);
    /* Frame 1 (middle):    receiver=D, args={E,F} */
    STA_OOP oop_D = STA_SMALLINT_OOP(1004);
    STA_OOP oop_E = STA_SMALLINT_OOP(1005);
    STA_OOP oop_F = STA_SMALLINT_OOP(1006);
    /* Frame 2 (innermost): receiver=G, args={H}, locals={I,J} */
    STA_OOP oop_G = STA_SMALLINT_OOP(1007);
    STA_OOP oop_H = STA_SMALLINT_OOP(1008);
    STA_OOP oop_I = STA_SMALLINT_OOP(1009);
    STA_OOP oop_J = STA_SMALLINT_OOP(1010);

    static const uint8_t dummy_bc[] = { OP_HALT };

    STA_OOP f0_args[] = { oop_B };
    STA_OOP f1_args[] = { oop_E, oop_F };
    STA_OOP f2_args[] = { oop_H };

    STA_Frame *f0 = sta_frame_push(&slab, NULL, oop_A, dummy_bc, 1u, 1u, f0_args);
    STA_Frame *f1 = sta_frame_push(&slab, f0,   oop_D, dummy_bc, 2u, 0u, f1_args);
    STA_Frame *f2 = sta_frame_push(&slab, f1,   oop_G, dummy_bc, 1u, 2u, f2_args);

    /* Manually set local slots for f0 and f2 (initialised to zero by push) */
    sta_frame_slots(f0)[1] = oop_C;
    sta_frame_slots(f2)[1] = oop_I;
    sta_frame_slots(f2)[2] = oop_J;

    /* Walk from innermost frame */
    g_visit_count = 0;
    sta_frame_gc_roots(f2, record_visit, NULL);

    /* Expected: 10 visits (f2: G,H,I,J / f1: D,E,F / f0: A,B,C) */
    int expected_count = 10;
    CHECK_EQ(g_visit_count, expected_count, "GC walk visits exactly 10 OOP slots");

    /* Every expected slot address must appear exactly once */
    CHECK(ptr_in_visited(&f2->receiver),       "f2 receiver (G) visited");
    CHECK(ptr_in_visited(&sta_frame_slots(f2)[0]), "f2 arg[0] (H) visited");
    CHECK(ptr_in_visited(&sta_frame_slots(f2)[1]), "f2 local[0] (I) visited");
    CHECK(ptr_in_visited(&sta_frame_slots(f2)[2]), "f2 local[1] (J) visited");
    CHECK(ptr_in_visited(&f1->receiver),       "f1 receiver (D) visited");
    CHECK(ptr_in_visited(&sta_frame_slots(f1)[0]), "f1 arg[0] (E) visited");
    CHECK(ptr_in_visited(&sta_frame_slots(f1)[1]), "f1 arg[1] (F) visited");
    CHECK(ptr_in_visited(&f0->receiver),       "f0 receiver (A) visited");
    CHECK(ptr_in_visited(&sta_frame_slots(f0)[0]), "f0 arg[0] (B) visited");
    CHECK(ptr_in_visited(&sta_frame_slots(f0)[1]), "f0 local[0] (C) visited");

    /* Verify correct OOP values were in the visited slots */
    CHECK(*g_visited[0] == oop_G, "first visit is f2->receiver (innermost first)");

    sta_frame_slab_destroy(&slab);
}

/* ── Test 4: TCO — constant stack depth ────────────────────────────────────*/

static void test_tco_constant_depth(void) {
    printf("\n=== Test 4: TCO constant stack depth (countdown from 1,000,000) ===\n");

    /* One frame (48 bytes) is all that's ever needed with TCO. */
    STA_FrameSlab slab;
    sta_frame_slab_init(&slab, 512);

    STA_OOP self_oop   = make_heap_oop(0);
    STA_OOP start_n    = STA_SMALLINT_OOP(1000000);
    STA_Frame *f = sta_frame_push(&slab, NULL, self_oop,
                                   countdown_tco, 1u, 0u, &start_n);

    uint32_t reductions = UINT32_MAX;   /* effectively infinite quota */
    STA_ExecStatus status = sta_exec_actor(&slab, f, &reductions);

    CHECK(status == STA_EXEC_HALT, "TCO countdown halts cleanly");
    CHECK_EQ((long long)slab.max_depth, 1LL,
             "max_depth == 1 (TCO reuses the single frame throughout)");
    CHECK_EQ((long long)slab.used, 0LL,
             "slab.used == 0 after halt (frame popped on outermost return)");

    sta_frame_slab_destroy(&slab);
}

/* ── Test 5: No-TCO — stack grows to expected depth ────────────────────────*/

static void test_notco_stack_growth(void) {
    printf("\n=== Test 5: No-TCO stack growth (countdown from 1,000) ===\n");

    /* countdown from N: pushes N+1 frames total (initial + N recursive).
     * Each frame: 40 (header) + 1*8 (one arg) = 48 bytes.
     * For N=1000: 1001 frames × 48 bytes = 48,048 bytes. */
    size_t slab_cap = 1001u * 48u + 4096u;  /* headroom for safety */
    STA_FrameSlab slab;
    sta_frame_slab_init(&slab, slab_cap);

    STA_OOP self_oop = make_heap_oop(0);
    STA_OOP start_n  = STA_SMALLINT_OOP(1000);
    STA_Frame *f = sta_frame_push(&slab, NULL, self_oop,
                                   countdown_notail, 1u, 0u, &start_n);

    uint32_t reductions = UINT32_MAX;
    STA_ExecStatus status = sta_exec_actor(&slab, f, &reductions);

    CHECK(status == STA_EXEC_HALT, "non-TCO countdown halts cleanly");
    /* countdown from 1000: initial frame (depth 1) + 1000 recursive frames = 1001 */
    CHECK_EQ((long long)slab.max_depth, 1001LL,
             "max_depth == 1001 (initial frame + 1000 recursive frames)");

    sta_frame_slab_destroy(&slab);
}

/* ── Test 6: Reduction counter — exact decrement count ─────────────────────*/

static void test_reduction_count(void) {
    printf("\n=== Test 6: Reduction counter (countdown from 10,000) ===\n");

    STA_FrameSlab slab;
    sta_frame_slab_init(&slab, 512);

    STA_OOP self_oop = make_heap_oop(0);
    STA_OOP start_n  = STA_SMALLINT_OOP(10000);
    STA_Frame *f = sta_frame_push(&slab, NULL, self_oop,
                                   countdown_tco, 1u, 0u, &start_n);

    /* Use a large initial value so we never preempt. */
    uint32_t initial_reductions = UINT32_MAX;
    uint32_t reductions         = initial_reductions;

    STA_ExecStatus status = sta_exec_actor(&slab, f, &reductions);

    CHECK(status == STA_EXEC_HALT, "countdown halts cleanly");

    uint32_t decrements = initial_reductions - reductions;
    printf("  total reduction decrements = %u (expect 10000)\n", decrements);
    CHECK_EQ((long long)decrements, 10000LL,
             "exactly 10,000 reduction decrements for 10,000 tail calls");

    sta_frame_slab_destroy(&slab);
}

/* ── Test 7 + 8: Preemption count and frame state preservation ─────────────*/

static void test_preemption(void) {
    printf("\n=== Tests 7+8: Preemption (countdown=10,000, quota=1,000) ===\n");

    STA_FrameSlab slab;
    sta_frame_slab_init(&slab, 512);

    STA_OOP self_oop = make_heap_oop(0);
    STA_OOP start_n  = STA_SMALLINT_OOP(10000);
    STA_Frame *f = sta_frame_push(&slab, NULL, self_oop,
                                   countdown_tco, 1u, 0u, &start_n);

    uint32_t quota = (uint32_t)STA_REDUCTION_QUOTA;  /* 1000, from ADR 009 */
    uint32_t reductions = quota;

    int preempt_count = 0;
    STA_ExecStatus status;

    do {
        status = sta_exec_actor(&slab, f, &reductions);

        if (status == STA_EXEC_PREEMPTED) {
            preempt_count++;

            /* Test 8: Verify frame state after each preemption.
             * After k preemptions, n = 10000 - k * 1000.
             * Uses CHECK_EQ so any mismatch registers as a hard failure. */
            STA_Frame *cur = slab.top_frame;
            STA_OOP   *slots = sta_frame_slots(cur);
            intptr_t   n_actual   = STA_SMALLINT_VAL(slots[0]);
            intptr_t   n_expected = (intptr_t)(10000 - preempt_count * 1000);

            printf("  after preemption %d: ", preempt_count);
            CHECK_EQ(n_actual, n_expected, "args[0] == 10000 - k*1000");

            /* Resume: refill quota, resume from slab->top_frame */
            reductions = quota;
            f = slab.top_frame;
        }
    } while (status == STA_EXEC_PREEMPTED);

    CHECK(status == STA_EXEC_HALT, "countdown halts after all preemptions");
    CHECK_EQ((long long)preempt_count, 10LL,
             "exactly 10 preemptions for 10,000 countdown at quota=1,000");
    /* Test 8: all per-preemption CHECK_EQ calls above feed g_failures directly;
     * any frame-state mismatch will cause the final g_failures != 0 exit. */

    /* Final result: self_oop (base case returns self unchanged) */
    CHECK(slab.result == self_oop, "final result is self_oop (base case)");

    sta_frame_slab_destroy(&slab);
}

/* ── Test 9: Actor density checkpoint ──────────────────────────────────────*/

static void test_density(void) {
    printf("\n=== Test 9: Actor density checkpoint ===\n");

    size_t frame_a     = sizeof(STA_Frame);           /* Option A header */
    size_t frame_b     = sizeof(STA_FrameAlt);        /* Option B header */
    size_t frame_5slot = sta_frame_alloc_size(5u);    /* header + 5 OOP slots */
    size_t stack_10    = 10u * frame_5slot;           /* 10 frames, 5 slots each */

    /* STA_ActorRevised from scheduler_spike.h (ADR 009): 120 bytes.
     * Adding stack_base + stack_top (2 × 8 bytes) = 16 bytes of new fields.
     * Projected struct size: 136 bytes. */
    size_t actor_revised_current = sizeof(STA_ActorRevised);
    size_t actor_with_stack_ptrs = actor_revised_current + 16u; /* stack_base + stack_top */
    size_t nursery_slab          = 128u;
    size_t identity_obj          = (size_t)STA_HEADER_ALIGNED + 0u;
    size_t creation_cost         = actor_with_stack_ptrs + nursery_slab + identity_obj;

    printf("\n  ── Frame layout comparison ───────────────────────────────\n");
    printf("  sizeof(STA_Frame)    Option A (C struct):  %zu bytes\n", frame_a);
    printf("  sizeof(STA_FrameAlt) Option B (ObjHeader): %zu bytes\n", frame_b);
    printf("  Per-frame cost at 5 slots (Option A):      %zu bytes\n", frame_5slot);
    printf("  Stack footprint at 10 frames × 5 slots:    %zu bytes\n", stack_10);

    printf("\n  ── Actor density (projected after ADR 010 consequences) ──\n");
    printf("  sizeof(STA_ActorRevised) current:          %zu bytes\n", actor_revised_current);
    printf("  + stack_base + stack_top (2×8):          + 16 bytes\n");
    printf("  Projected STA_Actor with stack fields:     %zu bytes\n", actor_with_stack_ptrs);
    printf("  Initial nursery slab:                      %zu bytes\n", nursery_slab);
    printf("  Actor identity object (0-slot header):     %zu bytes\n", identity_obj);
    printf("  ─────────────────────────────────────────────────────────\n");
    printf("  Projected total creation cost:             %zu bytes (target: ~300)\n",
           creation_cost);

    CHECK(frame_a == 40u,     "Option A STA_Frame is 40 bytes");
    CHECK(frame_b == 72u,     "Option B STA_FrameAlt is 72 bytes");
    CHECK(frame_b > frame_a,  "Option B has higher per-frame overhead than Option A");

    /* The stack is lazily allocated; creation cost excludes the stack slab.
     * The density target applies to the zero-frame creation cost. */
    CHECK(creation_cost <= 300u,
          "projected creation cost (actor + nursery + identity) ≤ 300 bytes");

    printf("\n  Stack is allocated lazily at first call — not a creation cost.\n");
    printf("  Per-actor creation cost budget:\n");
    printf("    At depth 0 (creation):    %zu bytes ← density target applies here\n",
           creation_cost);
    printf("    Stack at depth 10:       +%zu bytes ← runtime cost, not creation\n",
           stack_10);
}

/* ── Main ───────────────────────────────────────────────────────────────────*/

int main(void) {
    printf("=== Spike 004: Activation Frame Layout and TCO ===\n");
    printf("Platform: arm64 M4 Max, Apple clang 17, C17\n");

    test_layout();
    test_push_pop();
    test_gc_walk();
    test_tco_constant_depth();
    test_notco_stack_growth();
    test_reduction_count();
    test_preemption();
    test_density();

    printf("\n=== Spike 004 summary ===\n");
    if (g_failures == 0) {
        printf("All checks passed. Record decisions in ADR 010.\n\n");
        printf("Key numbers for ADR 010:\n");
        printf("  STA_Frame (Option A):          %zu bytes\n", sizeof(STA_Frame));
        printf("  STA_FrameAlt (Option B):       %zu bytes\n", sizeof(STA_FrameAlt));
        printf("  TCO stack depth at 1M:         1 frame\n");
        printf("  Non-TCO stack at 1000:         1001 frames\n");
        printf("  Reductions for 10k countdown:  10,000\n");
        printf("  Preemptions (10k, quota=1000): 10\n");
        return 0;
    } else {
        fprintf(stderr, "\n%d check(s) FAILED. Fix before writing ADR 010.\n",
                g_failures);
        return 1;
    }
}
