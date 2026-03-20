/* tests/test_supervisor_linkage.c
 * Phase 2 Epic 6, Story 1: Supervisor linkage and child specification.
 */
#include "actor/actor.h"
#include "actor/supervisor.h"
#include "vm/vm_state.h"
#include "vm/special_objects.h"
#include "vm/oop.h"
#include <sta/vm.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static STA_VM *g_vm;

static void setup(void) {
    STA_VMConfig cfg = {0};
    g_vm = sta_vm_create(&cfg);
    assert(g_vm);
}

static void teardown(void) {
    sta_vm_destroy(g_vm);
    g_vm = NULL;
}

/* ── Test: supervisor init ────────────────────────────────────────────── */

static void test_supervisor_init(void) {
    setup();
    struct STA_Actor *sup = sta_actor_create(g_vm, 128, 512);
    assert(sup);

    /* Before init, sup_data should be NULL. */
    assert(sup->sup_data == NULL);

    int rc = sta_supervisor_init(sup, 3, 5);
    assert(rc == 0);

    /* After init, sup_data should be non-NULL with correct defaults. */
    assert(sup->sup_data != NULL);
    assert(sup->sup_data->children == NULL);
    assert(sup->sup_data->child_count == 0);
    assert(sup->sup_data->max_restarts == 3);
    assert(sup->sup_data->max_seconds == 5);
    assert(sup->sup_data->restart_count == 0);
    assert(sup->sup_data->window_start_ns == 0);

    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_supervisor_init\n");
}

/* ── Test: add child ──────────────────────────────────────────────────── */

static void test_add_child(void) {
    setup();
    struct STA_Actor *sup = sta_actor_create(g_vm, 128, 512);
    assert(sup);
    sta_supervisor_init(sup, 3, 5);

    /* Use a dummy behavior class OOP (nil works for linkage testing). */
    STA_OOP behavior = g_vm->specials[SPC_NIL];

    struct STA_Actor *child = sta_supervisor_add_child(sup, behavior,
                                                        STA_RESTART_RESTART);
    assert(child != NULL);

    /* Verify child's supervisor pointer. */
    assert(child->supervisor == sup);
    assert(child->behavior_class == behavior);

    /* Verify supervisor's child list. */
    assert(sup->sup_data->child_count == 1);
    assert(sup->sup_data->children != NULL);
    assert(sup->sup_data->children->current_actor == child);
    assert(sup->sup_data->children->behavior_class == behavior);
    assert(sup->sup_data->children->strategy == STA_RESTART_RESTART);

    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_add_child\n");
}

/* ── Test: multiple children ──────────────────────────────────────────── */

static void test_multiple_children(void) {
    setup();
    struct STA_Actor *sup = sta_actor_create(g_vm, 128, 512);
    assert(sup);
    sta_supervisor_init(sup, 3, 5);

    STA_OOP behavior = g_vm->specials[SPC_NIL];

    struct STA_Actor *c1 = sta_supervisor_add_child(sup, behavior, STA_RESTART_RESTART);
    struct STA_Actor *c2 = sta_supervisor_add_child(sup, behavior, STA_RESTART_STOP);
    struct STA_Actor *c3 = sta_supervisor_add_child(sup, behavior, STA_RESTART_ESCALATE);

    assert(c1 && c2 && c3);
    assert(sup->sup_data->child_count == 3);

    /* All children point to the supervisor. */
    assert(c1->supervisor == sup);
    assert(c2->supervisor == sup);
    assert(c3->supervisor == sup);

    /* Each child has a distinct actor_id. */
    assert(c1->actor_id != c2->actor_id);
    assert(c2->actor_id != c3->actor_id);
    assert(c1->actor_id != c3->actor_id);

    /* Walk the child spec list — 3 entries (prepend order: c3, c2, c1). */
    STA_ChildSpec *spec = sup->sup_data->children;
    int count = 0;
    while (spec) {
        count++;
        assert(spec->current_actor != NULL);
        spec = spec->next;
    }
    assert(count == 3);

    sta_actor_destroy(sup);
    teardown();
    printf("  PASS: test_multiple_children\n");
}

/* ── Test: non-supervisor has NULL fields ──────────────────────────────── */

static void test_non_supervisor(void) {
    setup();
    struct STA_Actor *actor = sta_actor_create(g_vm, 128, 512);
    assert(actor);

    assert(actor->supervisor == NULL);
    assert(actor->sup_data == NULL);

    sta_actor_destroy(actor);
    teardown();
    printf("  PASS: test_non_supervisor\n");
}

/* ── Test: sizeof(STA_Actor) ──────────────────────────────────────────── */

static void test_sizeof_actor(void) {
    size_t sz = sizeof(struct STA_Actor);
    printf("  sizeof(STA_Actor) = %zu bytes\n", sz);

    /* Was 184 bytes (Epic 5). supervisor was already void* (8 bytes), now
     * typed as STA_Actor* (still 8). Added sup_data pointer (8) = 192. */
    assert(sz == 192);

    printf("  PASS: test_sizeof_actor\n");
}

/* ── Test: destroy supervisor tears down children ─────────────────────── */

static void test_destroy_tears_down_children(void) {
    setup();
    struct STA_Actor *sup = sta_actor_create(g_vm, 128, 512);
    assert(sup);
    sta_supervisor_init(sup, 3, 5);

    STA_OOP behavior = g_vm->specials[SPC_NIL];

    /* Add several children. */
    sta_supervisor_add_child(sup, behavior, STA_RESTART_RESTART);
    sta_supervisor_add_child(sup, behavior, STA_RESTART_STOP);

    /* Destroying the supervisor should free children too (ASan validates). */
    sta_actor_destroy(sup);

    teardown();
    printf("  PASS: test_destroy_tears_down_children\n");
}

/* ── Test: nested supervisors ─────────────────────────────────────────── */

static void test_nested_supervisors(void) {
    setup();

    /* Grandparent supervisor. */
    struct STA_Actor *gp = sta_actor_create(g_vm, 128, 512);
    assert(gp);
    sta_supervisor_init(gp, 3, 5);

    STA_OOP behavior = g_vm->specials[SPC_NIL];

    /* Parent supervisor is a child of grandparent. */
    struct STA_Actor *parent = sta_supervisor_add_child(gp, behavior,
                                                         STA_RESTART_RESTART);
    assert(parent);
    sta_supervisor_init(parent, 3, 5);

    /* Worker is a child of parent. */
    struct STA_Actor *worker = sta_supervisor_add_child(parent, behavior,
                                                         STA_RESTART_RESTART);
    assert(worker);

    /* Verify chain: worker → parent → grandparent. */
    assert(worker->supervisor == parent);
    assert(parent->supervisor == gp);
    assert(gp->supervisor == NULL);

    /* Destroying grandparent should cascade down. */
    sta_actor_destroy(gp);

    teardown();
    printf("  PASS: test_nested_supervisors\n");
}

/* ── Main ─────────────────────────────────────────────────────────────── */

int main(void) {
    printf("test_supervisor_linkage:\n");
    test_supervisor_init();
    test_add_child();
    test_multiple_children();
    test_non_supervisor();
    test_sizeof_actor();
    test_destroy_tears_down_children();
    test_nested_supervisors();
    printf("All supervisor linkage tests passed.\n");
    return 0;
}
