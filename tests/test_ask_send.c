/* tests/test_ask_send.c
 * Tests for sta_actor_ask_msg — Phase 2 Epic 7A Story 2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdatomic.h>

#include "actor/actor.h"
#include "actor/future.h"
#include "actor/future_table.h"
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

static STA_OOP make_array(STA_Heap *heap, int count) {
    STA_ObjHeader *h = sta_heap_alloc(heap, STA_CLS_ARRAY, (uint32_t)count);
    assert(h != NULL);
    STA_OOP *slots = sta_payload(h);
    for (int i = 0; i < count; i++)
        slots[i] = STA_SMALLINT_OOP(i + 1);
    return (STA_OOP)(uintptr_t)h;
}

/* ── Test 1: ask creates a future ─────────────────────────────────── */

static void test_ask_creates_future(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);

    STA_OOP sel = intern("ping");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b->actor_id,
                                       sel, NULL, 0, &err);
    assert(f != NULL);
    assert(err == 0);
    assert(atomic_load(&f->state) == STA_FUTURE_PENDING);
    assert(f->sender_id == a->actor_id);
    assert(f->future_id > 0);

    sta_future_table_remove(g_vm->future_table, f->future_id);
    sta_future_release(f);
    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 2: ask dead actor returns NULL ──────────────────────────── */

static void test_ask_dead_actor(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);
    uint32_t b_id = b->actor_id;
    sta_actor_terminate(b);

    STA_OOP sel = intern("ping");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b_id,
                                       sel, NULL, 0, &err);
    assert(f == NULL);
    assert(err == STA_ERR_ACTOR_DEAD);

    /* Future table should be empty — no future was created. */
    assert(g_vm->future_table->count == 0);

    sta_actor_terminate(a);
}

/* ── Test 3: ask mailbox full returns NULL, no future created ─────── */

static void test_ask_mailbox_full(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = sta_actor_create(g_vm, 4096, 512);
    assert(b != NULL);
    sta_actor_register(b);

    /* Reinit mailbox with tiny capacity. */
    sta_mailbox_destroy(&b->mailbox);
    sta_mailbox_init(&b->mailbox, 2);

    STA_OOP sel = intern("ping");

    /* Fill the mailbox with fire-and-forget sends. */
    assert(sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0) == 0);
    assert(sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0) == 0);

    /* Ask should fail — mailbox full. */
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b->actor_id,
                                       sel, NULL, 0, &err);
    assert(f == NULL);
    assert(err == STA_ERR_MAILBOX_FULL);

    /* No orphaned future in the table. */
    assert(g_vm->future_table->count == 0);

    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 4: ask envelope has future_id ───────────────────────────── */

static void test_ask_envelope_has_future_id(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);

    STA_OOP sel = intern("ping");
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b->actor_id,
                                       sel, NULL, 0, &err);
    assert(f != NULL);

    /* Dequeue from target mailbox manually. */
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    assert(msg != NULL);
    assert(msg->future_id == f->future_id);
    assert(msg->future_id > 0);
    sta_mailbox_msg_destroy(msg);

    sta_future_table_remove(g_vm->future_table, f->future_id);
    sta_future_release(f);
    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 5: ask args are deep copied ─────────────────────────────── */

static void test_ask_args_deep_copied(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);

    /* Create a mutable Array on a's heap. */
    STA_OOP arr = make_array(&a->heap, 3);

    STA_OOP sel = intern("doStuff:");
    STA_OOP args[1] = { arr };
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, a->actor_id, b->actor_id,
                                       sel, args, 1, &err);
    assert(f != NULL);

    /* Dequeue and verify deep copy. */
    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    assert(msg != NULL);
    assert(msg->arg_count == 1);
    assert(msg->args[0] != arr);  /* different OOP — deep copied */

    /* Verify contents match. */
    STA_ObjHeader *orig_h = (STA_ObjHeader *)(uintptr_t)arr;
    STA_ObjHeader *copy_h = (STA_ObjHeader *)(uintptr_t)msg->args[0];
    assert(copy_h->size == orig_h->size);
    STA_OOP *orig_slots = sta_payload(orig_h);
    STA_OOP *copy_slots = sta_payload(copy_h);
    for (uint32_t i = 0; i < orig_h->size; i++)
        assert(copy_slots[i] == orig_slots[i]);

    sta_mailbox_msg_destroy(msg);
    sta_future_table_remove(g_vm->future_table, f->future_id);
    sta_future_release(f);
    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Test 6: fire-and-forget has future_id == 0 ──────────────────── */

static void test_fire_and_forget_future_id_zero(void) {
    struct STA_Actor *a = make_actor(4096);
    struct STA_Actor *b = make_actor(4096);

    STA_OOP sel = intern("ping");
    int rc = sta_actor_send_msg(g_vm, a, b->actor_id, sel, NULL, 0);
    assert(rc == 0);

    STA_MailboxMsg *msg = sta_mailbox_dequeue(&b->mailbox);
    assert(msg != NULL);
    assert(msg->future_id == 0);
    sta_mailbox_msg_destroy(msg);

    sta_actor_terminate(a);
    sta_actor_terminate(b);
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(void) {
    printf("sizeof(STA_MailboxMsg) = %zu bytes\n\n", sizeof(STA_MailboxMsg));

    setup();
    printf("test_ask_send:\n");
    RUN(test_ask_creates_future);
    RUN(test_ask_dead_actor);
    RUN(test_ask_mailbox_full);
    RUN(test_ask_envelope_has_future_id);
    RUN(test_ask_args_deep_copied);
    RUN(test_fire_and_forget_future_id_zero);
    teardown();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
