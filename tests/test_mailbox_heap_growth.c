/* tests/test_mailbox_heap_growth.c
 * Regression test for GitHub #340: STA_MailboxMsg.args must survive
 * target heap growth.
 *
 * The original bug: deep-copy path allocated an Array on the target
 * heap and stored its payload pointer as msg->args. A subsequent
 * sta_actor_send_msg to the same target could trigger sta_heap_grow
 * (via pre-flight sizing), reallocating the heap buffer. The old
 * msg->args pointer then dangled.
 *
 * The fix: always malloc the args array (never allocate it on the
 * target heap). Deep-copied objects still live on the target heap,
 * but the args array (which holds the OOP values) is in malloc'd
 * memory, immune to heap buffer reallocation.
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
    sta_actor_register(a);
    return a;
}

static STA_OOP intern(const char *name) {
    return sta_symbol_intern(&g_vm->immutable_space,
                              &g_vm->symbol_table,
                              name, strlen(name));
}

static STA_OOP make_array(STA_Heap *heap, int count, int base_val) {
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_ARRAY, (uint32_t)count);
    assert(h != NULL);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < count; i++)
        slots[i] = STA_SMALLINT_OOP(base_val + i);
    return (STA_OOP)(uintptr_t)h;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

/* Verify that deep-copy path always sets args_owned=true (the fix).
 * Before the fix, deep-copied args were stored in a heap Array with
 * args_owned=false — msg->args pointed into the heap. */
static void test_deep_copy_args_are_malloc_owned(void) {
    struct STA_Actor *sender = make_actor(8192);
    struct STA_Actor *target = make_actor(4096);

    STA_OOP sel = intern("handle:");
    STA_OOP arr = make_array(&sender->heap, 2, 100);
    STA_OOP args[1] = { arr };

    int rc = sta_actor_send_msg(g_vm, sender, target->actor_id,
                                 sel, args, 1);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&target->mailbox);
    assert(msg != NULL);

    /* After the fix, args are always malloc'd — args_owned must be true. */
    assert(msg->args_owned == true);

    /* The deep-copied object itself is on the target heap. */
    STA_OOP arg0 = msg->args[0];
    assert(!STA_IS_IMMEDIATE(arg0));
    uintptr_t addr = (uintptr_t)arg0;
    uintptr_t base = (uintptr_t)target->heap.base;
    assert(addr >= base && addr < base + target->heap.capacity);

    /* Contents are correct. */
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)arg0;
    assert(h->class_index == STA_CLS_ARRAY);
    assert(h->size == 2);
    assert(sta_payload(h)[0] == STA_SMALLINT_OOP(100));
    assert(sta_payload(h)[1] == STA_SMALLINT_OOP(101));

    sta_mailbox_msg_destroy(msg);
    sta_actor_terminate(sender);
    sta_actor_terminate(target);
}

/* Core regression test: send multiple messages with mutable args to a
 * target with a large-enough heap that no growth occurs between sends.
 * Verify all messages have valid args and correct contents.
 * Run under ASan to catch any memory errors. */
static void test_multiple_sends_mutable_args(void) {
    struct STA_Actor *sender = make_actor(8192);
    /* Use a heap large enough to hold all deep-copied args without growth. */
    struct STA_Actor *target = make_actor(4096);

    STA_OOP sel = intern("process:");
    int num_msgs = 5;

    for (int i = 0; i < num_msgs; i++) {
        STA_OOP arr = make_array(&sender->heap, 3, i * 10);
        STA_OOP args[1] = { arr };

        int rc = sta_actor_send_msg(g_vm, sender, target->actor_id,
                                     sel, args, 1);
        assert(rc == 0);
    }

    /* Dequeue all messages and verify args are valid. */
    for (int i = 0; i < num_msgs; i++) {
        STA_MailboxMsg *msg = sta_mailbox_dequeue(&target->mailbox);
        assert(msg != NULL);
        assert(msg->selector == sel);
        assert(msg->arg_count == 1);
        assert(msg->args != NULL);
        assert(msg->args_owned == true);

        STA_OOP arg0 = msg->args[0];
        assert(arg0 != 0);

        STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)arg0;
        assert(h->class_index == STA_CLS_ARRAY);
        assert(h->size == 3);
        STA_OOP *slots = sta_payload(h);
        assert(slots[0] == STA_SMALLINT_OOP(i * 10));
        assert(slots[1] == STA_SMALLINT_OOP(i * 10 + 1));
        assert(slots[2] == STA_SMALLINT_OOP(i * 10 + 2));

        sta_mailbox_msg_destroy(msg);
    }

    assert(sta_mailbox_is_empty(&target->mailbox));

    sta_actor_terminate(sender);
    sta_actor_terminate(target);
}

/* Immediate-only args still use the zero-copy path with args_owned=true. */
static void test_immediate_args_still_owned(void) {
    struct STA_Actor *sender = make_actor(4096);
    struct STA_Actor *target = make_actor(4096);

    STA_OOP sel = intern("add:to:");
    STA_OOP args[2] = { STA_SMALLINT_OOP(42), STA_SMALLINT_OOP(99) };

    int rc = sta_actor_send_msg(g_vm, sender, target->actor_id,
                                 sel, args, 2);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&target->mailbox);
    assert(msg != NULL);
    assert(msg->args_owned == true);
    assert(msg->args[0] == STA_SMALLINT_OOP(42));
    assert(msg->args[1] == STA_SMALLINT_OOP(99));

    sta_mailbox_msg_destroy(msg);
    sta_actor_terminate(sender);
    sta_actor_terminate(target);
}

/* Mixed args: one immediate, one mutable — both paths result in
 * args_owned=true since the deep-copy path now mallocs the array. */
static void test_mixed_args_owned(void) {
    struct STA_Actor *sender = make_actor(8192);
    struct STA_Actor *target = make_actor(4096);

    STA_OOP sel = intern("at:put:");
    STA_OOP arr = make_array(&sender->heap, 2, 50);
    STA_OOP args[2] = { STA_SMALLINT_OOP(7), arr };

    int rc = sta_actor_send_msg(g_vm, sender, target->actor_id,
                                 sel, args, 2);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&target->mailbox);
    assert(msg != NULL);
    assert(msg->arg_count == 2);
    assert(msg->args_owned == true);

    /* SmallInt unchanged. */
    assert(msg->args[0] == STA_SMALLINT_OOP(7));

    /* Mutable array deep-copied and readable. */
    STA_ObjHeader *h = (STA_ObjHeader *)(uintptr_t)msg->args[1];
    assert(h->class_index == STA_CLS_ARRAY);
    assert(h->size == 2);
    assert(sta_payload(h)[0] == STA_SMALLINT_OOP(50));
    assert(sta_payload(h)[1] == STA_SMALLINT_OOP(51));

    sta_mailbox_msg_destroy(msg);
    sta_actor_terminate(sender);
    sta_actor_terminate(target);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_mailbox_heap_growth:\n");
    setup();

    RUN(test_deep_copy_args_are_malloc_owned);
    RUN(test_multiple_sends_mutable_args);
    RUN(test_immediate_args_still_owned);
    RUN(test_mixed_args_owned);

    teardown();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
