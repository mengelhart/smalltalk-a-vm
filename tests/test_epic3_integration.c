/* tests/test_epic3_integration.c
 * End-to-end integration tests for Epic 3: Mailbox & Message Send.
 * Phase 2 Epic 3 Story 6.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "actor/actor.h"
#include "actor/mailbox.h"
#include "actor/mailbox_msg.h"
#include "actor/deep_copy.h"
#include "vm/vm_state.h"
#include "vm/heap.h"
#include "vm/oop.h"
#include "vm/class_table.h"
#include "vm/method_dict.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include "vm/special_objects.h"
#include "vm/interpreter.h"
#include "vm/compiled_method.h"
#include "compiler/compiler.h"
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

static STA_OOP intern(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space,
                              &g_vm->symbol_table,
                              name, strlen(name));
}

static void install_method(STA_OOP cls, const char *sel_str,
                            const char *source,
                            const char **ivars, uint32_t ivar_count) {
    STA_CompileResult r = sta_compile_method(
        source, cls, ivars, ivar_count,
        &g_vm->symbol_table, &g_vm->immutable_space,
        &g_vm->root_actor->heap,
        g_vm->specials[SPC_SMALLTALK]);
    assert(!r.had_error);
    STA_OOP md = sta_class_method_dict(cls);
    sta_method_dict_insert(&g_vm->root_actor->heap, md, intern(sel_str), r.method);
}

static struct STA_Actor *make_actor_with_behavior(STA_OOP cls, uint32_t ivars) {
    struct STA_Actor *a = sta_actor_create(g_vm, 32768, 2048);
    assert(a != NULL);
    uint32_t idx = sta_class_table_index_of(&g_vm->class_table, cls);
    STA_ObjHeader *obj = sta_heap_alloc(&a->heap, idx, ivars);
    assert(obj != NULL);
    STA_OOP nil_oop = g_vm->specials[SPC_NIL];
    for (uint32_t i = 0; i < ivars; i++)
        sta_payload(obj)[i] = nil_oop;
    a->behavior_obj = (STA_OOP)(uintptr_t)obj;
    a->behavior_class = cls;
    a->state = STA_ACTOR_READY;
    return a;
}

static STA_OOP make_string(STA_Heap *heap, const char *text) {
    size_t len = strlen(text);
    uint32_t var_words = ((uint32_t)len + 7u) / (uint32_t)sizeof(STA_OOP);
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_STRING, var_words);
    assert(h != NULL);
    h->reserved = (uint16_t)(var_words * sizeof(STA_OOP) - (uint32_t)len);
    memset(sta_payload(h), 0, var_words * sizeof(STA_OOP));
    memcpy(sta_payload(h), text, len);
    return (STA_OOP)(uintptr_t)h;
}

static bool on_heap(STA_Heap *heap, STA_OOP oop) {
    if (STA_IS_IMMEDIATE(oop) || oop == 0) return false;
    uintptr_t addr = (uintptr_t)oop;
    return addr >= (uintptr_t)heap->base &&
           addr < (uintptr_t)heap->base + heap->capacity;
}

/* ── End-to-end: A sends to B, B processes ───────────────────────────── */

static void test_e2e_send_and_process(void) {
    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ASSOCIATION);
    const char *ivars[] = {"key", "value"};
    install_method(assoc_cls, "setValue:", "setValue: v value := v", ivars, 2);

    struct STA_Actor *a = sta_actor_create(g_vm, 8192, 512);
    assert(a != NULL);
    a->actor_id = 1;

    struct STA_Actor *b = make_actor_with_behavior(assoc_cls, 2);
    b->actor_id = 2;

    /* A sends setValue: 42 to B. */
    STA_OOP args[] = { STA_SMALLINT_OOP(42) };
    int rc = sta_actor_send_msg(a, b, intern("setValue:"), args, 1);
    assert(rc == 0);

    /* B processes the message. */
    rc = sta_actor_process_one(g_vm, b);
    assert(rc == 1);

    /* B's behavior_obj value instVar should be 42. */
    STA_ObjHeader *bh = (STA_ObjHeader *)(uintptr_t)b->behavior_obj;
    assert(sta_payload(bh)[1] == STA_SMALLINT_OOP(42));

    /* A's heap is unchanged — no side effects. */
    /* (A didn't have any mutable state to check, but heap used is stable.) */

    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_e2e_deep_copy_isolation(void) {
    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ASSOCIATION);
    const char *ivars[] = {"key", "value"};
    install_method(assoc_cls, "setKey:", "setKey: k key := k", ivars, 2);

    struct STA_Actor *a = sta_actor_create(g_vm, 16384, 512);
    a->actor_id = 1;
    struct STA_Actor *b = make_actor_with_behavior(assoc_cls, 2);
    b->actor_id = 2;

    /* Create a mutable array on A's heap. */
    STA_ObjHeader *arr_h = sta_heap_alloc(&a->heap, STA_CLS_ARRAY, 3);
    sta_payload(arr_h)[0] = STA_SMALLINT_OOP(10);
    sta_payload(arr_h)[1] = STA_SMALLINT_OOP(20);
    sta_payload(arr_h)[2] = STA_SMALLINT_OOP(30);
    STA_OOP arr = (STA_OOP)(uintptr_t)arr_h;

    /* Send it to B. */
    STA_OOP args[] = { arr };
    sta_actor_send_msg(a, b, intern("setKey:"), args, 1);
    sta_actor_process_one(g_vm, b);

    /* B's key instVar should be a copy on B's heap. */
    STA_ObjHeader *bh = (STA_ObjHeader *)(uintptr_t)b->behavior_obj;
    STA_OOP copy = sta_payload(bh)[0];
    assert(copy != arr);
    assert(on_heap(&b->heap, copy));
    assert(!on_heap(&a->heap, copy));

    /* Mutate the copy on B — original on A unchanged. */
    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)copy;
    sta_payload(copy_h)[0] = STA_SMALLINT_OOP(999);
    assert(sta_payload(arr_h)[0] == STA_SMALLINT_OOP(10));

    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

/* ── Bounded mailbox ──────────────────────────────────────────────────── */

static void test_bounded_mailbox_flow(void) {
    struct STA_Actor *a = sta_actor_create(g_vm, 4096, 512);
    a->actor_id = 1;
    struct STA_Actor *b = sta_actor_create(g_vm, 4096, 512);

    /* Reinit B's mailbox with capacity 3. */
    sta_mailbox_destroy(&b->mailbox);
    sta_mailbox_init(&b->mailbox, 3);

    STA_OOP sel = intern("x");

    assert(sta_actor_send_msg(a, b, sel, NULL, 0) == 0);
    assert(sta_actor_send_msg(a, b, sel, NULL, 0) == 0);
    assert(sta_actor_send_msg(a, b, sel, NULL, 0) == 0);
    assert(sta_actor_send_msg(a, b, sel, NULL, 0) == STA_ERR_MAILBOX_FULL);

    /* Dequeue one manually, send should succeed again. */
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    sta_mailbox_msg_destroy(msg);

    assert(sta_actor_send_msg(a, b, sel, NULL, 0) == 0);

    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

/* ── Deep copy edge cases ─────────────────────────────────────────────── */

static void test_deep_copy_large_graph(void) {
    struct STA_Actor *src = sta_actor_create(g_vm, 65536, 512);
    struct STA_Actor *dst = sta_actor_create(g_vm, 65536, 512);

    /* Build a linked list of 100 arrays. */
    STA_OOP prev = STA_SMALLINT_OOP(0);  /* terminal */
    for (int i = 0; i < 100; i++) {
        STA_ObjHeader *h = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 2);
        assert(h != NULL);
        sta_payload(h)[0] = STA_SMALLINT_OOP(i);
        sta_payload(h)[1] = prev;
        prev = (STA_OOP)(uintptr_t)h;
    }

    STA_OOP copy = sta_deep_copy(prev, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0);

    /* Walk and verify: 99, 98, ..., 0. */
    STA_OOP cur = copy;
    for (int i = 99; i >= 0; i--) {
        assert(!STA_IS_IMMEDIATE(cur) && cur != 0);
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)cur;
        assert(sta_payload(h)[0] == STA_SMALLINT_OOP(i));
        cur = sta_payload(h)[1];
    }
    assert(cur == STA_SMALLINT_OOP(0));

    sta_actor_destroy(src);
    sta_actor_destroy(dst);
}

static void test_deep_copy_deeply_nested(void) {
    struct STA_Actor *src = sta_actor_create(g_vm, 65536, 512);
    struct STA_Actor *dst = sta_actor_create(g_vm, 65536, 512);

    /* 25 levels deep (exceeds the spec's 20+). */
    STA_OOP inner = STA_SMALLINT_OOP(7);
    for (int i = 0; i < 25; i++) {
        STA_ObjHeader *h = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
        sta_payload(h)[0] = inner;
        inner = (STA_OOP)(uintptr_t)h;
    }

    STA_OOP copy = sta_deep_copy(inner, &src->heap, &dst->heap,
                                  &g_vm->class_table);
    assert(copy != 0);

    STA_OOP cur = copy;
    for (int i = 0; i < 25; i++) {
        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)cur;
        cur = sta_payload(h)[0];
    }
    assert(cur == STA_SMALLINT_OOP(7));

    sta_actor_destroy(src);
    sta_actor_destroy(dst);
}

static void test_deep_copy_multiple_cycles(void) {
    struct STA_Actor *src = sta_actor_create(g_vm, 16384, 512);
    struct STA_Actor *dst = sta_actor_create(g_vm, 16384, 512);

    /* Cycle 1: A ↔ B */
    STA_ObjHeader *ah = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 2);
    STA_ObjHeader *bh = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 2);
    STA_OOP a = (STA_OOP)(uintptr_t)ah;
    STA_OOP b = (STA_OOP)(uintptr_t)bh;
    sta_payload(ah)[0] = b;
    sta_payload(bh)[0] = a;

    /* Cycle 2: C → D → C */
    STA_ObjHeader *ch = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    STA_ObjHeader *dh = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 1);
    STA_OOP c = (STA_OOP)(uintptr_t)ch;
    STA_OOP d = (STA_OOP)(uintptr_t)dh;
    sta_payload(ch)[0] = d;
    sta_payload(dh)[0] = c;

    /* Root points to both cycles. */
    sta_payload(ah)[1] = c;
    sta_payload(bh)[1] = d;

    STA_OOP copy_a = sta_deep_copy(a, &src->heap, &dst->heap,
                                    &g_vm->class_table);
    assert(copy_a != 0);

    /* Verify structure: copy_a[0] = copy_b, copy_b[0] = copy_a. */
    STA_OOP *ca = sta_payload((STA_ObjHeader *)(uintptr_t)copy_a);
    STA_OOP copy_b = ca[0];
    STA_OOP *cb = sta_payload((STA_ObjHeader *)(uintptr_t)copy_b);
    assert(cb[0] == copy_a);

    /* copy_a[1] = copy_c, copy_c[0] = copy_d, copy_d[0] = copy_c. */
    STA_OOP copy_c = ca[1];
    STA_OOP copy_d = cb[1];
    assert(sta_payload((STA_ObjHeader *)(uintptr_t)copy_c)[0] == copy_d);
    assert(sta_payload((STA_ObjHeader *)(uintptr_t)copy_d)[0] == copy_c);

    sta_actor_destroy(src);
    sta_actor_destroy(dst);
}

static void test_deep_copy_mixed_graph(void) {
    struct STA_Actor *src = sta_actor_create(g_vm, 16384, 512);
    struct STA_Actor *dst = sta_actor_create(g_vm, 16384, 512);

    STA_OOP sym = intern("shared");
    STA_OOP str = make_string(&src->heap, "mutable");

    /* Array with Symbol (shared), String (copied), SmallInt, nil. */
    STA_ObjHeader *ah = sta_heap_alloc(&src->heap, STA_CLS_ARRAY, 4);
    sta_payload(ah)[0] = sym;
    sta_payload(ah)[1] = str;
    sta_payload(ah)[2] = STA_SMALLINT_OOP(99);
    sta_payload(ah)[3] = g_vm->specials[SPC_NIL];

    STA_OOP copy = sta_deep_copy((STA_OOP)(uintptr_t)ah,
                                  &src->heap, &dst->heap, &g_vm->class_table);
    STA_OOP *cs = sta_payload((STA_ObjHeader *)(uintptr_t)copy);

    assert(cs[0] == sym);           /* Symbol shared */
    assert(cs[1] != str);           /* String copied */
    assert(on_heap(&dst->heap, cs[1]));
    assert(cs[2] == STA_SMALLINT_OOP(99));
    assert(cs[3] == g_vm->specials[SPC_NIL]);  /* nil shared */

    sta_actor_destroy(src);
    sta_actor_destroy(dst);
}

/* ── Multi-actor: chain A → B → C ────────────────────────────────────── */

static void test_three_actor_chain(void) {
    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ASSOCIATION);
    const char *ivars[] = {"key", "value"};
    /* Already installed from earlier tests: setValue:, setKey: */

    struct STA_Actor *a = sta_actor_create(g_vm, 8192, 512);
    a->actor_id = 1;
    struct STA_Actor *b = make_actor_with_behavior(assoc_cls, 2);
    b->actor_id = 2;
    struct STA_Actor *c = make_actor_with_behavior(assoc_cls, 2);
    c->actor_id = 3;

    /* A sends setValue: 100 to B. */
    STA_OOP args1[] = { STA_SMALLINT_OOP(100) };
    sta_actor_send_msg(a, b, intern("setValue:"), args1, 1);
    sta_actor_process_one(g_vm, b);

    /* B sends setValue: 200 to C (manually, as B doesn't have send logic). */
    STA_OOP args2[] = { STA_SMALLINT_OOP(200) };
    sta_actor_send_msg(b, c, intern("setValue:"), args2, 1);
    sta_actor_process_one(g_vm, c);

    /* Verify B has value=100, C has value=200. */
    STA_ObjHeader *bh = (STA_ObjHeader *)(uintptr_t)b->behavior_obj;
    STA_ObjHeader *ch2 = (STA_ObjHeader *)(uintptr_t)c->behavior_obj;
    assert(sta_payload(bh)[1] == STA_SMALLINT_OOP(100));
    assert(sta_payload(ch2)[1] == STA_SMALLINT_OOP(200));

    sta_actor_destroy(a);
    sta_actor_destroy(b);
    sta_actor_destroy(c);
}

static void test_same_message_to_multiple_actors(void) {
    STA_OOP assoc_cls = sta_class_table_get(&g_vm->class_table, STA_CLS_ASSOCIATION);

    struct STA_Actor *a = sta_actor_create(g_vm, 8192, 512);
    a->actor_id = 1;
    struct STA_Actor *b = make_actor_with_behavior(assoc_cls, 2);
    struct STA_Actor *c = make_actor_with_behavior(assoc_cls, 2);

    /* Create a mutable array on A's heap. */
    STA_ObjHeader *arr_h = sta_heap_alloc(&a->heap, STA_CLS_ARRAY, 2);
    sta_payload(arr_h)[0] = STA_SMALLINT_OOP(1);
    sta_payload(arr_h)[1] = STA_SMALLINT_OOP(2);
    STA_OOP arr = (STA_OOP)(uintptr_t)arr_h;

    /* Send the same array to B and C. */
    STA_OOP args[] = { arr };
    sta_actor_send_msg(a, b, intern("setKey:"), args, 1);
    sta_actor_send_msg(a, c, intern("setKey:"), args, 1);

    sta_actor_process_one(g_vm, b);
    sta_actor_process_one(g_vm, c);

    /* B and C should each have independent copies. */
    STA_OOP b_key = sta_payload((STA_ObjHeader *)(uintptr_t)b->behavior_obj)[0];
    STA_OOP c_key = sta_payload((STA_ObjHeader *)(uintptr_t)c->behavior_obj)[0];

    assert(b_key != arr);
    assert(c_key != arr);
    assert(b_key != c_key);  /* independent copies */
    assert(on_heap(&b->heap, b_key));
    assert(on_heap(&c->heap, c_key));

    sta_actor_destroy(a);
    sta_actor_destroy(b);
    sta_actor_destroy(c);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_epic3_integration:\n");
    setup();

    /* End-to-end messaging */
    RUN(test_e2e_send_and_process);
    RUN(test_e2e_deep_copy_isolation);

    /* Bounded mailbox */
    RUN(test_bounded_mailbox_flow);

    /* Deep copy edge cases */
    RUN(test_deep_copy_large_graph);
    RUN(test_deep_copy_deeply_nested);
    RUN(test_deep_copy_multiple_cycles);
    RUN(test_deep_copy_mixed_graph);

    /* Multi-actor */
    RUN(test_three_actor_chain);
    RUN(test_same_message_to_multiple_actors);

    teardown();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
