/* src/actor/registry.c
 * VM-wide actor registry — mutex-protected open-addressing hash map.
 * See registry.h for documentation.
 */
#include "registry.h"
#include "actor.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* ── Hash table entry ──────────────────────────────────────────────── */

/* Sentinel values for empty and tombstone slots. */
#define ENTRY_EMPTY     0u
#define ENTRY_TOMBSTONE UINT32_MAX

typedef struct {
    uint32_t         actor_id;  /* ENTRY_EMPTY or ENTRY_TOMBSTONE = unused */
    struct STA_Actor *actor;
} RegistryEntry;

/* ── STA_ActorRegistry ─────────────────────────────────────────────── */

struct STA_ActorRegistry {
    RegistryEntry   *entries;
    uint32_t         capacity;  /* always a power of two */
    uint32_t         count;     /* live entries (not counting tombstones) */
    uint32_t         used;      /* live + tombstones (for load factor) */
    pthread_mutex_t  lock;
};

/* ── Helpers ───────────────────────────────────────────────────────── */

static uint32_t next_pow2(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
    v |= v >> 8;  v |= v >> 16;
    return v + 1;
}

/* Grow at 70% load (live + tombstones). */
static int needs_grow(const STA_ActorRegistry *reg) {
    return reg->used * 10 >= reg->capacity * 7;
}

/* Linear probe index. */
static uint32_t probe(uint32_t id, uint32_t mask) {
    return id & mask;
}

/* Rehash all live entries into a new table. Frees the old table. */
static int registry_rehash(STA_ActorRegistry *reg, uint32_t new_cap) {
    RegistryEntry *old = reg->entries;
    uint32_t old_cap   = reg->capacity;

    RegistryEntry *fresh = calloc(new_cap, sizeof(RegistryEntry));
    if (!fresh) return -1;

    uint32_t mask = new_cap - 1;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (old[i].actor_id == ENTRY_EMPTY ||
            old[i].actor_id == ENTRY_TOMBSTONE)
            continue;

        uint32_t idx = probe(old[i].actor_id, mask);
        while (fresh[idx].actor_id != ENTRY_EMPTY)
            idx = (idx + 1) & mask;

        fresh[idx] = old[i];
    }

    free(old);
    reg->entries  = fresh;
    reg->capacity = new_cap;
    reg->used     = reg->count;  /* tombstones eliminated */
    return 0;
}

/* ── Public API ────────────────────────────────────────────────────── */

STA_ActorRegistry *sta_registry_create(uint32_t initial_capacity) {
    STA_ActorRegistry *reg = calloc(1, sizeof(STA_ActorRegistry));
    if (!reg) return NULL;

    uint32_t cap = next_pow2(initial_capacity < 8 ? 8 : initial_capacity);
    reg->entries = calloc(cap, sizeof(RegistryEntry));
    if (!reg->entries) {
        free(reg);
        return NULL;
    }

    reg->capacity = cap;
    reg->count    = 0;
    reg->used     = 0;

    if (pthread_mutex_init(&reg->lock, NULL) != 0) {
        free(reg->entries);
        free(reg);
        return NULL;
    }

    return reg;
}

void sta_registry_destroy(STA_ActorRegistry *reg) {
    if (!reg) return;
    pthread_mutex_destroy(&reg->lock);
    free(reg->entries);
    free(reg);
}

void sta_registry_register(STA_ActorRegistry *reg, struct STA_Actor *actor) {
    if (!reg || !actor) return;

    uint32_t id = actor->actor_id;
    /* actor_id 0 is the default unassigned value — skip registration. */
    if (id == ENTRY_EMPTY || id == ENTRY_TOMBSTONE) return;

    pthread_mutex_lock(&reg->lock);

    /* Grow if needed BEFORE inserting. */
    if (needs_grow(reg)) {
        if (registry_rehash(reg, reg->capacity * 2) != 0) {
            pthread_mutex_unlock(&reg->lock);
            return;  /* OOM — silent drop */
        }
    }

    uint32_t mask = reg->capacity - 1;
    uint32_t idx  = probe(id, mask);
    uint32_t first_tombstone = UINT32_MAX;

    while (1) {
        uint32_t eid = reg->entries[idx].actor_id;
        if (eid == ENTRY_EMPTY) {
            /* Insert at first tombstone if we passed one, else here. */
            if (first_tombstone != UINT32_MAX) {
                idx = first_tombstone;
                /* Reusing a tombstone: count stays same, used stays same. */
            } else {
                reg->used++;
            }
            reg->entries[idx].actor_id = id;
            reg->entries[idx].actor    = actor;
            reg->count++;
            break;
        }
        if (eid == ENTRY_TOMBSTONE && first_tombstone == UINT32_MAX) {
            first_tombstone = idx;
        }
        if (eid == id) {
            /* Duplicate ID — update pointer (defensive). */
            reg->entries[idx].actor = actor;
            break;
        }
        idx = (idx + 1) & mask;
    }

    pthread_mutex_unlock(&reg->lock);
}

void sta_registry_unregister(STA_ActorRegistry *reg, uint32_t actor_id) {
    if (!reg) return;
    if (actor_id == ENTRY_EMPTY || actor_id == ENTRY_TOMBSTONE) return;

    pthread_mutex_lock(&reg->lock);

    uint32_t mask = reg->capacity - 1;
    uint32_t idx  = probe(actor_id, mask);

    while (1) {
        uint32_t eid = reg->entries[idx].actor_id;
        if (eid == ENTRY_EMPTY) break;  /* not found */
        if (eid == actor_id) {
            reg->entries[idx].actor_id = ENTRY_TOMBSTONE;
            reg->entries[idx].actor    = NULL;
            reg->count--;
            /* used stays the same — tombstone still occupies a slot */
            break;
        }
        idx = (idx + 1) & mask;
    }

    pthread_mutex_unlock(&reg->lock);
}

struct STA_Actor *sta_registry_lookup(STA_ActorRegistry *reg, uint32_t actor_id) {
    if (!reg) return NULL;
    if (actor_id == ENTRY_EMPTY || actor_id == ENTRY_TOMBSTONE) return NULL;

    pthread_mutex_lock(&reg->lock);

    uint32_t mask = reg->capacity - 1;
    uint32_t idx  = probe(actor_id, mask);
    struct STA_Actor *result = NULL;

    while (1) {
        uint32_t eid = reg->entries[idx].actor_id;
        if (eid == ENTRY_EMPTY) break;
        if (eid == actor_id) {
            result = reg->entries[idx].actor;
            break;
        }
        idx = (idx + 1) & mask;
    }

    /* Increment refcount UNDER the mutex — prevents TOCTOU where the
     * actor is found, the mutex is released, and then the actor is freed
     * before the caller uses it. Fixes #317. */
    if (result) {
        atomic_fetch_add_explicit(&result->refcount, 1, memory_order_relaxed);
    }

    pthread_mutex_unlock(&reg->lock);
    return result;
}

uint32_t sta_registry_count(STA_ActorRegistry *reg) {
    if (!reg) return 0;
    pthread_mutex_lock(&reg->lock);
    uint32_t c = reg->count;
    pthread_mutex_unlock(&reg->lock);
    return c;
}
