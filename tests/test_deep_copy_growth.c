/* tests/test_deep_copy_growth.c
 * Tests for pre-flight size estimation and heap growth during
 * cross-actor deep copy — GitHub #295.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "actor/actor.h"
#include "actor/deep_copy.h"
#include "actor/mailbox.h"
#include "actor/mailbox_msg.h"
#include "vm/vm_state.h"
#include "vm/heap.h"
#include "vm/oop.h"
#include "vm/class_table.h"
#include "vm/format.h"
#include "vm/interpreter.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include "vm/special_objects.h"
#include <sta/vm.h>

static int tests_run = 0;
static int tests_passed = 0;

#define RUN(fn) do { \
    printf("  %-55s", #fn); \
    tests_run++; \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

/* ── Helpers ─────────────────────────────────────────────────────────── */

static STA_VM *g_vm;

static void setup(void) {
    STA_VMConfig cfg = {0};
    g_vm = sta_vm_create(&cfg);
    assert(g_vm != NULL);
}

static void teardown(void) {
    sta_vm_destroy(g_vm);
    g_vm = NULL;
}

static struct STA_Actor *make_actor(size_t heap_size) {
    struct STA_Actor *a = sta_actor_create(g_vm, heap_size, 512);
    assert(a != NULL);
    sta_actor_register(a);
    return a;
}

static STA_OOP intern(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space,
                              &g_vm->symbol_table,
                              name, strlen(name));
}

static STA_OOP make_array(STA_Heap *heap, int count) {
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_ARRAY, (uint32_t)count);
    assert(h != NULL);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < count; i++)
        slots[i] = STA_SMALLINT_OOP(i + 1);
    return (STA_OOP)(uintptr_t)h;
}

/* Allocate an Association (2 OOP slots: key, value) on the heap. */
static STA_OOP make_assoc(STA_Heap *heap, STA_OOP key, STA_OOP value) {
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_ASSOCIATION, 2);
    assert(h != NULL);
    sta_payload(h)[0] = key;
    sta_payload(h)[1] = value;
    return (STA_OOP)(uintptr_t)h;
}

static bool on_heap(STA_Heap *heap, STA_OOP oop) {
    if (STA_IS_IMMEDIATE(oop) || oop == 0) return false;
    uintptr_t addr = (uintptr_t)oop;
    uintptr_t base = (uintptr_t)heap->base;
    return addr >= base && addr < base + heap->capacity;
}

/* ── Estimation tests ─────────────────────────────────────────────────── */

static void test_estimate_immediate_zero(void) {
    /* Immediates and nil require zero bytes. */
    assert(sta_deep_copy_estimate(STA_SMALLINT_OOP(42), &g_vm->class_table) == 0);
    assert(sta_deep_copy_estimate(STA_CHAR_OOP('A'), &g_vm->class_table) == 0);
    assert(sta_deep_copy_estimate(0, &g_vm->class_table) == 0);
}

static void test_estimate_immutable_zero(void) {
    /* Symbols and nil/true/false are immutable — zero copy cost. */
    STA_OOP sym = intern("estSym");
    assert(sta_deep_copy_estimate(sym, &g_vm->class_table) == 0);

    STA_OOP nil_oop = g_vm->specials[SPC_NIL];
    assert(sta_deep_copy_estimate(nil_oop, &g_vm->class_table) == 0);
}

static void test_estimate_single_array(void) {
    struct STA_Actor *src = make_actor(4096);
    STA_OOP arr = make_array(&src->heap, 5);

    size_t est = sta_deep_copy_estimate(arr, &g_vm->class_table);
    /* Array(5): raw = 16 + 5*8 = 56, aligned to 16 → 64 bytes. */
    size_t expected = (sta_alloc_size(5) + 15u) & ~(size_t)15u;
    assert(est == expected);

    sta_actor_terminate(src);
}

static void test_estimate_nested(void) {
    struct STA_Actor *src = make_actor(4096);

    STA_OOP inner = make_array(&src->heap, 2);
    STA_ObjHeader *oh = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    sta_payload(oh)[0] = inner;
    STA_OOP outer = (STA_OOP)(uintptr_t)oh;

    size_t est = sta_deep_copy_estimate(outer, &g_vm->class_table);
    size_t outer_sz = (sta_alloc_size(1) + 15u) & ~(size_t)15u;
    size_t inner_sz = (sta_alloc_size(2) + 15u) & ~(size_t)15u;
    assert(est == outer_sz + inner_sz);

    sta_actor_terminate(src);
}

static void test_estimate_shared_not_double_counted(void) {
    struct STA_Actor *src = make_actor(4096);

    /* Shared sub-object. */
    STA_OOP shared = make_array(&src->heap, 1);  /* 24 bytes */

    /* Two parents each pointing to shared. */
    STA_ObjHeader *ah = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    sta_payload(ah)[0] = shared;
    STA_OOP a = (STA_OOP)(uintptr_t)ah;

    STA_ObjHeader *bh = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    sta_payload(bh)[0] = shared;
    STA_OOP b = (STA_OOP)(uintptr_t)bh;

    /* Estimate both as roots — shared should be counted once. */
    STA_OOP roots[2] = { a, b };
    size_t est = sta_deep_copy_estimate_roots(roots, 2, &g_vm->class_table);
    size_t obj1 = (sta_alloc_size(1) + 15u) & ~(size_t)15u;
    /* 3 objects of size 1: a, b, shared */
    assert(est == 3 * obj1);

    sta_actor_terminate(src);
}

static void test_estimate_cycle(void) {
    struct STA_Actor *src = make_actor(4096);

    STA_ObjHeader *ah = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    STA_ObjHeader *bh = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    STA_OOP a = (STA_OOP)(uintptr_t)ah;
    STA_OOP b = (STA_OOP)(uintptr_t)bh;
    sta_payload(ah)[0] = b;
    sta_payload(bh)[0] = a;

    size_t est = sta_deep_copy_estimate(a, &g_vm->class_table);
    size_t obj1 = (sta_alloc_size(1) + 15u) & ~(size_t)15u;
    assert(est == 2 * obj1);

    sta_actor_terminate(src);
}

/* ── Large payload send tests ─────────────────────────────────────────── */

static void test_send_large_payload_to_small_heap(void) {
    /* Create source with big heap, target with 128-byte heap.
     * Send a payload (~640+ bytes of mutable data) that exceeds
     * the target's initial capacity. Pre-flight should grow it. */
    struct STA_Actor *src = make_actor(8192);
    struct STA_Actor *tgt = make_actor(128);

    /* Build an Array of 20 Associations on the source heap.
     * Each Association = 32 bytes (header 16 + 2 slots * 8).
     * Array(20) = 16 + 20*8 = 176 bytes.
     * Total mutable data: 176 + 20*32 = 816 bytes. */
    STA_OOP arr = make_array(&src->heap, 20);
    STA_ObjHeader *arr_h = (STA_ObjHeader *)(uintptr_t)arr;
    for (int i = 0; i < 20; i++) {
        STA_OOP assoc = make_assoc(&src->heap,
                                    STA_SMALLINT_OOP(i),
                                    STA_SMALLINT_OOP(i * 10));
        sta_payload(arr_h)[i] = assoc;
    }

    STA_OOP sel = intern("process:");
    STA_OOP args[1] = { arr };

    int rc = sta_actor_send_msg(g_vm, src, tgt->actor_id, sel, args, 1);
    assert(rc == 0);

    /* Dequeue and verify the message arrived intact. */
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&tgt->mailbox);
    assert(msg != NULL);
    assert(msg->arg_count == 1);

    STA_OOP copied = msg->args[0];
    assert(copied != 0);
    assert(on_heap(&tgt->heap, copied));

    STA_ObjHeader *ch = (STA_ObjHeader *)(uintptr_t)copied;
    assert(ch->class_index == STA_CLS_ARRAY);
    assert(ch->size == 20);

    /* Verify each Association was deep copied. */
    for (int i = 0; i < 20; i++) {
        STA_OOP slot = sta_payload(ch)[i];
        assert(!STA_IS_IMMEDIATE(slot) && slot != 0);
        assert(on_heap(&tgt->heap, slot));

        STA_ObjHeader *ah = (STA_ObjHeader *)(uintptr_t)slot;
        assert(ah->class_index == STA_CLS_ASSOCIATION);
        assert(sta_payload(ah)[0] == STA_SMALLINT_OOP(i));
        assert(sta_payload(ah)[1] == STA_SMALLINT_OOP(i * 10));
    }

    /* Target heap should have grown well beyond 128 bytes. */
    assert(tgt->heap.capacity > 128);

    sta_mailbox_msg_destroy(msg);
    sta_actor_terminate(src);
    sta_actor_terminate(tgt);
}

static void test_send_multiple_large_messages(void) {
    /* Send and drain large messages in sequence. After the first
     * send grows the heap, subsequent sends should succeed because
     * the heap is now large enough (or grows further as needed).
     *
     * We drain between sends because STA_MailboxMsg.args holds raw
     * pointers into the target heap — if the heap relocates (grow),
     * those pointers become stale. In production, the dispatcher
     * processes messages promptly; queuing multiple large messages
     * with heap growth between them is an edge case to be addressed
     * when mailbox pointers become GC-aware. */
    struct STA_Actor *src = make_actor(16384);
    struct STA_Actor *tgt = make_actor(128);

    STA_OOP sel = intern("batch:");

    for (int msg_num = 0; msg_num < 5; msg_num++) {
        STA_OOP arr = make_array(&src->heap, 10);
        STA_ObjHeader *arr_h = (STA_ObjHeader *)(uintptr_t)arr;
        for (int i = 0; i < 10; i++) {
            STA_OOP assoc = make_assoc(&src->heap,
                                        STA_SMALLINT_OOP(msg_num * 10 + i),
                                        STA_SMALLINT_OOP(42));
            sta_payload(arr_h)[i] = assoc;
        }

        STA_OOP args[1] = { arr };
        int rc = sta_actor_send_msg(g_vm, src, tgt->actor_id, sel, args, 1);
        assert(rc == 0);

        /* Drain immediately — verify the message. */
        STA_MailboxMsg *msg = sta_mailbox_dequeue(&tgt->mailbox);
        assert(msg != NULL);
        assert(msg->arg_count == 1);

        STA_OOP copied = msg->args[0];
        assert(copied != 0);
        STA_ObjHeader *ch = (STA_ObjHeader *)(uintptr_t)copied;
        assert(ch->class_index == STA_CLS_ARRAY);
        assert(ch->size == 10);

        /* Verify first Association's key. */
        STA_OOP first = sta_payload(ch)[0];
        STA_ObjHeader *fh = (STA_ObjHeader *)(uintptr_t)first;
        assert(fh->class_index == STA_CLS_ASSOCIATION);
        assert(sta_payload(fh)[0] == STA_SMALLINT_OOP(msg_num * 10));

        sta_mailbox_msg_destroy(msg);
    }

    /* Heap should have grown from the initial 128 bytes. */
    assert(tgt->heap.capacity > 128);

    sta_actor_terminate(src);
    sta_actor_terminate(tgt);
}

static void test_send_deeply_nested_structure(void) {
    /* Build a 10-level deep chain: each node is an Array(1) wrapping
     * the next level. Bottom is a SmallInt. */
    struct STA_Actor *src = make_actor(8192);
    struct STA_Actor *tgt = make_actor(128);

    STA_OOP inner = STA_SMALLINT_OOP(999);
    for (int i = 0; i < 10; i++) {
        STA_ObjHeader *h = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
        assert(h != NULL);
        sta_payload(h)[0] = inner;
        inner = (STA_OOP)(uintptr_t)h;
    }

    STA_OOP sel = intern("deep:");
    STA_OOP args[1] = { inner };

    int rc = sta_actor_send_msg(g_vm, src, tgt->actor_id, sel, args, 1);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&tgt->mailbox);
    assert(msg != NULL);

    /* Walk 10 levels down in the copy. */
    STA_OOP current = msg->args[0];
    for (int i = 0; i < 10; i++) {
        assert(!STA_IS_IMMEDIATE(current) && current != 0);
        assert(on_heap(&tgt->heap, current));
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)current;
        assert(h->class_index == STA_CLS_ARRAY);
        assert(h->size == 1);
        current = sta_payload(h)[0];
    }
    assert(current == STA_SMALLINT_OOP(999));

    sta_mailbox_msg_destroy(msg);
    sta_actor_terminate(src);
    sta_actor_terminate(tgt);
}

static void test_send_small_payload_no_regression(void) {
    /* Existing small-payload sends should still work fine
     * with the pre-flight overhead. */
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *tgt = make_actor(4096);
    size_t cap_before = tgt->heap.capacity;

    STA_OOP sel = intern("small:");
    STA_OOP args[1] = { STA_SMALLINT_OOP(42) };

    int rc = sta_actor_send_msg(g_vm, src, tgt->actor_id, sel, args, 1);
    assert(rc == 0);

    /* Heap should not have grown. */
    assert(tgt->heap.capacity == cap_before);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&tgt->mailbox);
    assert(msg != NULL);
    assert(msg->args[0] == STA_SMALLINT_OOP(42));

    sta_mailbox_msg_destroy(msg);
    sta_actor_terminate(src);
    sta_actor_terminate(tgt);
}

static void test_send_all_immediates_no_growth(void) {
    /* If all args are immediates/immutables, estimated size is just
     * the args array itself — no unnecessary growth. */
    struct STA_Actor *src = make_actor(4096);
    struct STA_Actor *tgt = make_actor(4096);
    size_t cap_before = tgt->heap.capacity;

    STA_OOP sel = intern("imm:");
    STA_OOP sym = intern("hello");
    STA_OOP args[3] = {
        STA_SMALLINT_OOP(1),
        STA_CHAR_OOP('x'),
        sym
    };

    int rc = sta_actor_send_msg(g_vm, src, tgt->actor_id, sel, args, 3);
    assert(rc == 0);

    /* Heap should not have grown (args array + 3 immediates fits easily). */
    assert(tgt->heap.capacity == cap_before);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&tgt->mailbox);
    assert(msg != NULL);
    assert(msg->args[0] == STA_SMALLINT_OOP(1));
    assert(msg->args[1] == STA_CHAR_OOP('x'));
    assert(msg->args[2] == sym);

    sta_mailbox_msg_destroy(msg);
    sta_actor_terminate(src);
    sta_actor_terminate(tgt);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_deep_copy_growth:\n");
    setup();

    /* Estimation unit tests. */
    RUN(test_estimate_immediate_zero);
    RUN(test_estimate_immutable_zero);
    RUN(test_estimate_single_array);
    RUN(test_estimate_nested);
    RUN(test_estimate_shared_not_double_counted);
    RUN(test_estimate_cycle);

    /* Send with growth tests (GitHub #295). */
    RUN(test_send_large_payload_to_small_heap);
    RUN(test_send_multiple_large_messages);
    RUN(test_send_deeply_nested_structure);
    RUN(test_send_small_payload_no_regression);
    RUN(test_send_all_immediates_no_growth);

    teardown();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
