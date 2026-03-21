/* src/actor/future_table.h
 * VM-wide future registry — future_id → STA_Future* lookup table.
 * Mutex-protected open-addressing hash map, same pattern as actor registry.
 *
 * Internal header — not part of the public API.
 */
#ifndef STA_FUTURE_TABLE_H
#define STA_FUTURE_TABLE_H

#include "future.h"
#include <pthread.h>

typedef struct STA_FutureTable {
    STA_Future **buckets;       /* open-addressing, linear probe */
    uint32_t     capacity;      /* power of two */
    uint32_t     count;         /* live entries */
    uint32_t     next_id;       /* monotonic, starts at 1 (0 = no future) */
    pthread_mutex_t lock;
} STA_FutureTable;

STA_FutureTable *sta_future_table_create(uint32_t initial_capacity);
void             sta_future_table_destroy(STA_FutureTable *table);

/* Create a PENDING future, insert into table. Returns retained ref.
   Caller must eventually sta_future_release(). */
STA_Future *sta_future_table_new(STA_FutureTable *table, uint32_t sender_id);

/* Lookup by future_id. Returns retained ref or NULL.
   Caller must sta_future_release() when done. */
STA_Future *sta_future_table_lookup(STA_FutureTable *table, uint32_t future_id);

/* Remove entry from table. Does NOT release — caller's ref still valid.
   Called after sender reads result or on cleanup. */
void sta_future_table_remove(STA_FutureTable *table, uint32_t future_id);

#endif /* STA_FUTURE_TABLE_H */
