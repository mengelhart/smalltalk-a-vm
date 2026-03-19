/* src/vm/handle.c
 * Handle table — reference-counted, growable slab allocator, per-VM.
 * See handle.h for documentation.
 *
 * Slabs are never freed until table destroy — all STA_Handle* pointers
 * remain stable for the table's lifetime. Growth appends a new slab.
 * Free slots are threaded via a flat-index free list across all slabs.
 */
#include "handle.h"
#include "vm_state.h"
#include <sta/vm.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Index ↔ pointer conversion ───────────────────────────────────────── */

/* Convert a flat index to a pointer by walking the slab chain. */
static struct STA_Handle *index_to_entry(STA_HandleTable *t, uint32_t idx) {
    uint32_t slab_idx = idx / STA_HANDLE_SLAB_SIZE;
    uint32_t slot     = idx % STA_HANDLE_SLAB_SIZE;

    STA_HandleSlab *s = t->first;
    for (uint32_t i = 0; i < slab_idx; i++) {
        s = s->next;
    }
    return &s->entries[slot];
}

/* Convert a pointer back to a flat index. */
static uint32_t entry_to_index(STA_HandleTable *t, struct STA_Handle *h) {
    uint32_t slab_idx = 0;
    STA_HandleSlab *s = t->first;
    while (s) {
        if (h >= s->entries && h < s->entries + STA_HANDLE_SLAB_SIZE) {
            uint32_t slot = (uint32_t)(h - s->entries);
            return slab_idx * STA_HANDLE_SLAB_SIZE + slot;
        }
        s = s->next;
        slab_idx++;
    }
    assert(0 && "handle not in any slab");
    return UINT32_MAX;
}

/* ── Slab allocation ──────────────────────────────────────────────────── */

static STA_HandleSlab *alloc_slab(void) {
    STA_HandleSlab *s = calloc(1, sizeof(STA_HandleSlab));
    return s;
}

static void thread_slab_free_list(STA_HandleTable *t, uint32_t base_index) {
    /* Thread all slots in this slab into the free list. */
    for (uint32_t i = 0; i < STA_HANDLE_SLAB_SIZE - 1; i++) {
        STA_HandleSlab *s = t->last;
        s->entries[i].refcount  = 0;
        s->entries[i].next_free = base_index + i + 1;
    }
    t->last->entries[STA_HANDLE_SLAB_SIZE - 1].refcount  = 0;
    t->last->entries[STA_HANDLE_SLAB_SIZE - 1].next_free = t->free_head;
    t->free_head = base_index;
}

/* ── Table lifecycle ──────────────────────────────────────────────────── */

int sta_handle_table_init(STA_HandleTable *t) {
    STA_HandleSlab *s = alloc_slab();
    if (!s) return -1;

    t->first      = s;
    t->last       = s;
    t->slab_count = 1;
    t->count      = 0;
    t->free_head  = UINT32_MAX; /* will be set by thread_slab_free_list */

    thread_slab_free_list(t, 0);
    return 0;
}

void sta_handle_table_destroy(STA_HandleTable *t) {
    STA_HandleSlab *s = t->first;
    while (s) {
        STA_HandleSlab *next = s->next;
        free(s);
        s = next;
    }
    t->first      = NULL;
    t->last       = NULL;
    t->slab_count = 0;
    t->count      = 0;
    t->free_head  = UINT32_MAX;
}

/* ── Grow ─────────────────────────────────────────────────────────────── */

static int handle_table_grow(STA_HandleTable *t) {
    STA_HandleSlab *s = alloc_slab();
    if (!s) return -1;

    t->last->next = s;
    t->last = s;
    uint32_t base = t->slab_count * STA_HANDLE_SLAB_SIZE;
    t->slab_count++;

    thread_slab_free_list(t, base);
    return 0;
}

/* ── Operations ───────────────────────────────────────────────────────── */

struct STA_Handle *sta_handle_create(STA_HandleTable *t, STA_OOP oop) {
    if (t->free_head == UINT32_MAX) {
        if (handle_table_grow(t) != 0) return NULL;
    }

    uint32_t idx = t->free_head;
    struct STA_Handle *h = index_to_entry(t, idx);
    t->free_head = h->next_free;

    h->oop       = oop;
    h->refcount  = 1;
    h->next_free = UINT32_MAX;
    t->count++;

    return h;
}

STA_OOP sta_handle_get(struct STA_Handle *h) {
    assert(h && h->refcount > 0);
    return h->oop;
}

struct STA_Handle *sta_handle_retain_entry(STA_HandleTable *t, struct STA_Handle *h) {
    (void)t;
    assert(h && h->refcount > 0);
    h->refcount++;
    return h;
}

void sta_handle_release_entry(STA_HandleTable *t, struct STA_Handle *h) {
    assert(h && h->refcount > 0);
    h->refcount--;
    if (h->refcount == 0) {
        h->oop       = 0;
        h->next_free = t->free_head;
        t->free_head = entry_to_index(t, h);
        t->count--;
    }
}

/* ── Public API wrappers (sta/vm.h declarations) ──────────────────────── */

STA_Handle *sta_handle_retain(STA_VM *vm, STA_Handle *handle) {
    if (!vm || !handle) return handle;
    return sta_handle_retain_entry(&vm->handles, handle);
}

void sta_handle_release(STA_VM *vm, STA_Handle *handle) {
    if (!vm || !handle) return;
    sta_handle_release_entry(&vm->handles, handle);
}
