/* tests/test_actor_send.c
 * Unit tests for sta_actor_send_msg — Phase 2 Epic 3 Story 4.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "actor/actor.h"
#include "actor/mailbox.h"
#include "actor/mailbox_msg.h"
#include "actor/registry.h"
#include "vm/vm_state.h"
#include "vm/heap.h"
#include "vm/oop.h"
#include "vm/class_table.h"
#include "vm/symbol_table.h"
#include "vm/immutable_space.h"
#include "vm/special_objects.h"
#include "vm/interpreter.h"
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

static struct STA_Actor *make_actor(size_t heap_size) {
    struct STA_Actor *a = sta_actor_create(g_vm, heap_size, 512);
    assert(a != NULL);
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

static STA_OOP make_string(STA_Heap *heap, const char *text) {
    size_t len = strlen(text);
    uint32_t var_words = ((uint32_t)len + (uint32_t)(sizeof(STA_OOP) - 1))
                         / (uint32_t)sizeof(STA_OOP);
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
    uintptr_t base = (uintptr_t)heap->base;
    return addr >= base && addr < base + heap->capacity;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

static void test_send_no_args(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);

    STA_OOP sel = intern("run");
    int rc = sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0);
    assert(rc == 0);

    /* Message should be in B's mailbox. */
    assert(!sta_mailbox_is_empty(&b->mailbox));
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    assert(msg != NULL);
    assert(msg->selector == sel);
    assert(msg->arg_count == 0);
    assert(msg->args == NULL);
    assert(msg->sender_id == a->actor_id);

    sta_mailbox_msg_destroy(msg);
    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_send_with_smallint_arg(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);

    STA_OOP sel = intern("add:");
    STA_OOP args[1] = { STA_SMALLINT_OOP(42) };

    int rc = sta_actor_send_msg(g_vm, a, b->actor_id, sel, args, 1);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    assert(msg != NULL);
    assert(msg->selector == sel);
    assert(msg->arg_count == 1);
    assert(msg->args != NULL);
    /* SmallInt is an immediate — passes through unchanged. */
    assert(msg->args[0] == STA_SMALLINT_OOP(42));

    sta_mailbox_msg_destroy(msg);
    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_send_deep_copies_mutable_arg(void) {
    struct STA_Actor *a = make_actor(8192);
    struct STA_Actor *b = make_actor(8192);

    STA_OOP sel = intern("process:");
    STA_OOP arr = make_array(&a->heap, 3);
    STA_OOP args[1] = { arr };

    int rc = sta_actor_send_msg(g_vm, a, b->actor_id, sel, args, 1);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    assert(msg != NULL);
    assert(msg->arg_count == 1);

    /* The arg should be a deep copy — different address, on B's heap. */
    STA_OOP copied = msg->args[0];
    assert(copied != arr);
    assert(on_heap(&b->heap, copied));
    assert(!on_heap(&a->heap, copied));

    /* Contents should match. */
    STA_ObjHeader *ch = (STA_ObjHeader *)(uintptr_t)copied;
    assert(ch->class_index == STA_CLS_ARRAY);
    assert(ch->size == 3);
    assert(sta_payload(ch)[0] == STA_SMALLINT_OOP(1));
    assert(sta_payload(ch)[1] == STA_SMALLINT_OOP(2));
    assert(sta_payload(ch)[2] == STA_SMALLINT_OOP(3));

    sta_mailbox_msg_destroy(msg);
    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_send_symbol_arg_shared(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);

    STA_OOP sel = intern("handle:");
    STA_OOP sym_arg = intern("foo");
    STA_OOP args[1] = { sym_arg };

    int rc = sta_actor_send_msg(g_vm, a, b->actor_id, sel, args, 1);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    /* Symbol is immutable — shared by pointer, same OOP. */
    assert(msg->args[0] == sym_arg);

    sta_mailbox_msg_destroy(msg);
    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_send_multiple_args(void) {
    struct STA_Actor *a = make_actor(8192);
    struct STA_Actor *b = make_actor(8192);

    STA_OOP sel = intern("at:put:");
    STA_OOP str = make_string(&a->heap, "hello");
    STA_OOP args[2] = { STA_SMALLINT_OOP(1), str };

    int rc = sta_actor_send_msg(g_vm, a, b->actor_id, sel, args, 2);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    assert(msg->arg_count == 2);
    assert(msg->args[0] == STA_SMALLINT_OOP(1));
    /* String was mutable → deep copied onto B's heap. */
    assert(msg->args[1] != str);
    assert(on_heap(&b->heap, msg->args[1]));

    STA_ObjHeader *sh = (STA_ObjHeader *)(uintptr_t)msg->args[1];
    uint32_t bc = sh->size * (uint32_t)sizeof(STA_OOP) - STA_BYTE_PADDING(sh);
    assert(bc == 5);
    assert(memcmp(sta_payload(sh), "hello", 5) == 0);

    sta_mailbox_msg_destroy(msg);
    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_send_mailbox_full(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = sta_actor_create(g_vm, 4096, 512);
    assert(b != NULL);

    /* Destroy default mailbox and reinit with tiny capacity. */
    sta_mailbox_destroy(&b->mailbox);
    sta_mailbox_init(&b->mailbox, 2);

    STA_OOP sel = intern("ping");

    /* Fill the mailbox. */
    assert(sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0) == 0);
    assert(sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0) == 0);

    /* Third send should fail. */
    int rc = sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0);
    assert(rc == STA_ERR_MAILBOX_FULL);

    /* Drain one, then send should succeed. */
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    sta_mailbox_msg_destroy(msg);

    rc = sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0);
    assert(rc == 0);

    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_send_preserves_sender_id(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);

    STA_OOP sel = intern("hello");
    sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    assert(msg->sender_id == a->actor_id);

    sta_mailbox_msg_destroy(msg);
    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_send_heap_isolation(void) {
    struct STA_Actor *a = make_actor(8192);
    struct STA_Actor *b = make_actor(8192);

    STA_OOP sel = intern("mutate:");
    STA_OOP arr = make_array(&a->heap, 2);
    STA_OOP args[1] = { arr };

    sta_actor_send_msg(g_vm, a, b->actor_id, sel, args, 1);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    STA_OOP copy = msg->args[0];

    /* Mutate the copy on B — shouldn't affect A. */
    STA_ObjHeader *ch = (STA_ObjHeader *)(uintptr_t)copy;
    sta_payload(ch)[0] = STA_SMALLINT_OOP(999);

    /* Original on A is unchanged. */
    STA_ObjHeader *orig_h = (STA_ObjHeader *)(uintptr_t)arr;
    assert(sta_payload(orig_h)[0] == STA_SMALLINT_OOP(1));

    sta_mailbox_msg_destroy(msg);
    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_send_fifo_order(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);

    for (int i = 0; i < 10; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "msg%d", i);
        STA_OOP sel = intern(buf);
        sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0);
    }

    /* Messages should arrive in FIFO order. */
    for (int i = 0; i < 10; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "msg%d", i);
        STA_OOP expected = intern(buf);

        STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
        assert(msg != NULL);
        assert(msg->selector == expected);
        sta_mailbox_msg_destroy(msg);
    }

    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

static void test_send_to_dead_actor(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);
    uint32_t b_id = b->actor_id;

    /* Destroy the target — this should unregister it from the registry. */
    sta_actor_destroy(b);

    STA_OOP sel = intern("ping");
    int rc = sta_actor_send_msg(g_vm, a, b_id, sel, NULL, 0);
    assert(rc == STA_ERR_ACTOR_DEAD);

    sta_actor_destroy(a);
}

static void test_send_after_unregister(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);
    uint32_t b_id = b->actor_id;

    /* Manually unregister the target from the registry. */
    sta_registry_unregister(g_vm->registry, b_id);

    STA_OOP sel = intern("ping");
    int rc = sta_actor_send_msg(g_vm, a, b_id, sel, NULL, 0);
    assert(rc == STA_ERR_ACTOR_DEAD);

    /* Re-register before destroy so sta_actor_destroy doesn't double-unregister. */
    sta_registry_register(g_vm->registry, b);
    sta_actor_destroy(a);
    sta_actor_destroy(b);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_actor_send:\n");
    setup();

    RUN(test_send_no_args);
    RUN(test_send_with_smallint_arg);
    RUN(test_send_deep_copies_mutable_arg);
    RUN(test_send_symbol_arg_shared);
    RUN(test_send_multiple_args);
    RUN(test_send_mailbox_full);
    RUN(test_send_preserves_sender_id);
    RUN(test_send_heap_isolation);
    RUN(test_send_fifo_order);
    RUN(test_send_to_dead_actor);
    RUN(test_send_after_unregister);

    teardown();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
