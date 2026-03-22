/* tests/test_ask_mailbox_full.c
 * Regression test for GitHub #43: future not failed on MAILBOX_FULL.
 *
 * Before the fix, sta_actor_ask_msg removed and released the future
 * without calling sta_future_fail on the MAILBOX_FULL path.  A holder
 * of the future would hang or get DNU instead of a clean failure.
 * After the fix, the future is failed with #mailboxFull before cleanup.
 *
 * Since sta_actor_ask_msg returns NULL on MAILBOX_FULL (the caller
 * never receives the future), we verify the fix indirectly:
 *  - The function returns NULL with *err = STA_ERR_MAILBOX_FULL
 *  - The future table count is unchanged (no leaked entries)
 *  - ASan confirms no memory leaks
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

/* ── Tests ────────────────────────────────────────────────────────────── */

/* ask: to a full mailbox should return NULL + STA_ERR_MAILBOX_FULL
 * and not leak the future. */
static void test_ask_mailbox_full_returns_error(void) {
    struct STA_Actor *sender = make_actor(4096);
    struct STA_Actor *target = sta_actor_create(g_vm, 4096, 512);
    assert(target != NULL);
    sta_actor_register(target);

    /* Reinit with tiny mailbox. */
    sta_mailbox_destroy(&target->mailbox);
    sta_mailbox_init(&target->mailbox, 2);

    STA_OOP sel = intern("ping");

    /* Fill the mailbox with fire-and-forget sends. */
    assert(sta_actor_send_msg(g_vm, sender, target->actor_id,
                               sel, NULL, 0) == 0);
    assert(sta_actor_send_msg(g_vm, sender, target->actor_id,
                               sel, NULL, 0) == 0);

    /* Record future table count before the ask. */
    uint32_t count_before = g_vm->future_table->count;

    /* ask: should fail with MAILBOX_FULL. */
    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, sender->actor_id,
                                       target->actor_id,
                                       sel, NULL, 0, &err);
    assert(f == NULL);
    assert(err == STA_ERR_MAILBOX_FULL);

    /* Future table count should be unchanged — future was cleaned up. */
    assert(g_vm->future_table->count == count_before);

    sta_actor_terminate(sender);
    sta_actor_terminate(target);
}

/* ask: to a dead actor should return NULL + STA_ERR_ACTOR_DEAD. */
static void test_ask_dead_actor(void) {
    struct STA_Actor *sender = make_actor(4096);
    struct STA_Actor *target = make_actor(4096);
    uint32_t target_id = target->actor_id;

    sta_actor_terminate(target);

    int err = 0;
    STA_Future *f = sta_actor_ask_msg(g_vm, sender->actor_id,
                                       target_id, intern("ping"),
                                       NULL, 0, &err);
    assert(f == NULL);
    assert(err == STA_ERR_ACTOR_DEAD);

    sta_actor_terminate(sender);
}

/* Successful ask: followed by failed ask: on the same target. */
static void test_ask_success_then_full(void) {
    struct STA_Actor *sender = make_actor(4096);
    struct STA_Actor *target = sta_actor_create(g_vm, 4096, 512);
    assert(target != NULL);
    sta_actor_register(target);

    /* Tiny mailbox: capacity 1. */
    sta_mailbox_destroy(&target->mailbox);
    sta_mailbox_init(&target->mailbox, 1);

    STA_OOP sel = intern("doWork");

    /* First ask: should succeed. */
    int err = 0;
    STA_Future *f1 = sta_actor_ask_msg(g_vm, sender->actor_id,
                                        target->actor_id,
                                        sel, NULL, 0, &err);
    assert(f1 != NULL);
    assert(err == 0);

    uint32_t count_after_first = g_vm->future_table->count;

    /* Second ask: should fail — mailbox is full. */
    err = 0;
    STA_Future *f2 = sta_actor_ask_msg(g_vm, sender->actor_id,
                                        target->actor_id,
                                        sel, NULL, 0, &err);
    assert(f2 == NULL);
    assert(err == STA_ERR_MAILBOX_FULL);

    /* Future table count should be the same as after the first ask
     * (the second future was created then cleaned up). */
    assert(g_vm->future_table->count == count_after_first);

    /* Clean up f1 — release caller's ref. */
    sta_future_table_remove(g_vm->future_table, f1->future_id);
    sta_future_release(f1);

    sta_actor_terminate(sender);
    sta_actor_terminate(target);
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_ask_mailbox_full:\n");
    setup();

    RUN(test_ask_mailbox_full_returns_error);
    RUN(test_ask_dead_actor);
    RUN(test_ask_success_then_full);

    teardown();
    printf("\n%d/%d tests passed\n", tests_passed, tests_run);
    return (tests_passed == tests_run) ? 0 : 1;
}
