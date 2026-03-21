/* src/actor/future_table.c
 * VM-wide future registry — Phase 2 Epic 7A.
 * See future_table.h for documentation.
 */
#include "future_table.h"
#include <stdlib.h>
#include <string.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

static uint32_t next_pow2(uint32_t v) {
    if (v == 0) return 1;
    v--;
    v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
    v |= v >> 8;  v |= v >> 16;
    return v + 1;
}

/* Grow at 70% load. */
static int needs_grow(const STA_FutureTable *t) {
    return t->count * 10 >= t->capacity * 7;
}

/* Hash: future_id % capacity. IDs are unique monotonic integers,
 * so low bits give good distribution. */
static uint32_t slot_index(uint32_t future_id, uint32_t mask) {
    return future_id & mask;
}

/* Rehash into a new table of new_cap. */
static int table_rehash(STA_FutureTable *t, uint32_t new_cap) {
    STA_Future **old = t->buckets;
    uint32_t old_cap = t->capacity;

    STA_Future **fresh = calloc(new_cap, sizeof(STA_Future *));
    if (!fresh) return -1;

    uint32_t mask = new_cap - 1;
    for (uint32_t i = 0; i < old_cap; i++) {
        if (!old[i]) continue;
        uint32_t idx = slot_index(old[i]->future_id, mask);
        while (fresh[idx])
            idx = (idx + 1) & mask;
        fresh[idx] = old[i];
    }

    free(old);
    t->buckets  = fresh;
    t->capacity = new_cap;
    return 0;
}

/* ── Public API ────────────────────────────────────────────────────── */

STA_FutureTable *sta_future_table_create(uint32_t initial_capacity) {
    STA_FutureTable *t = calloc(1, sizeof(STA_FutureTable));
    if (!t) return NULL;

    uint32_t cap = next_pow2(initial_capacity < 8 ? 8 : initial_capacity);
    t->buckets = calloc(cap, sizeof(STA_Future *));
    if (!t->buckets) {
        free(t);
        return NULL;
    }

    t->capacity = cap;
    t->count    = 0;
    t->next_id  = 1;  /* 0 = no future */

    if (pthread_mutex_init(&t->lock, NULL) != 0) {
        free(t->buckets);
        free(t);
        return NULL;
    }

    return t;
}

void sta_future_table_destroy(STA_FutureTable *table) {
    if (!table) return;

    /* Release all remaining futures. */
    for (uint32_t i = 0; i < table->capacity; i++) {
        if (table->buckets[i]) {
            sta_future_release(table->buckets[i]);
            table->buckets[i] = NULL;
        }
    }

    pthread_mutex_destroy(&table->lock);
    free(table->buckets);
    free(table);
}

STA_Future *sta_future_table_new(STA_FutureTable *table, uint32_t sender_id) {
    if (!table) return NULL;

    STA_Future *f = calloc(1, sizeof(STA_Future));
    if (!f) return NULL;

    pthread_mutex_lock(&table->lock);

    /* Grow if at load factor threshold. */
    if (needs_grow(table)) {
        if (table_rehash(table, table->capacity * 2) != 0) {
            pthread_mutex_unlock(&table->lock);
            free(f);
            return NULL;
        }
    }

    f->future_id = table->next_id++;
    f->sender_id = sender_id;
    atomic_store_explicit(&f->state, STA_FUTURE_PENDING, memory_order_relaxed);
    atomic_store_explicit(&f->refcount, 2, memory_order_relaxed);  /* caller + table */
    atomic_store_explicit(&f->waiter_actor_id, 0, memory_order_relaxed);

    /* Insert into table. */
    uint32_t mask = table->capacity - 1;
    uint32_t idx = slot_index(f->future_id, mask);
    while (table->buckets[idx])
        idx = (idx + 1) & mask;
    table->buckets[idx] = f;
    table->count++;

    pthread_mutex_unlock(&table->lock);

    return f;
}

STA_Future *sta_future_table_lookup(STA_FutureTable *table, uint32_t future_id) {
    if (!table || future_id == 0) return NULL;

    pthread_mutex_lock(&table->lock);

    uint32_t mask = table->capacity - 1;
    uint32_t idx = slot_index(future_id, mask);
    STA_Future *result = NULL;

    for (uint32_t i = 0; i < table->capacity; i++) {
        uint32_t pos = (idx + i) & mask;
        if (!table->buckets[pos]) break;  /* empty — not found */
        if (table->buckets[pos]->future_id == future_id) {
            result = table->buckets[pos];
            sta_future_retain(result);
            break;
        }
    }

    pthread_mutex_unlock(&table->lock);
    return result;
}

void sta_future_table_remove(STA_FutureTable *table, uint32_t future_id) {
    if (!table || future_id == 0) return;

    pthread_mutex_lock(&table->lock);

    uint32_t mask = table->capacity - 1;
    uint32_t idx = slot_index(future_id, mask);

    for (uint32_t i = 0; i < table->capacity; i++) {
        uint32_t pos = (idx + i) & mask;
        if (!table->buckets[pos]) break;  /* not found */
        if (table->buckets[pos]->future_id == future_id) {
            /* Shift-back deletion to maintain linear probing invariant. */
            table->buckets[pos] = NULL;
            table->count--;

            /* Re-insert any entries that follow in the same cluster. */
            uint32_t next = (pos + 1) & mask;
            while (table->buckets[next]) {
                STA_Future *displaced = table->buckets[next];
                table->buckets[next] = NULL;
                table->count--;

                uint32_t target = slot_index(displaced->future_id, mask);
                while (table->buckets[target])
                    target = (target + 1) & mask;
                table->buckets[target] = displaced;
                table->count++;

                next = (next + 1) & mask;
            }
            break;
        }
    }

    pthread_mutex_unlock(&table->lock);
}
