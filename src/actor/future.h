/* src/actor/future.h
 * Future struct for asynchronous request-response — Phase 2 Epic 7A.
 * VM-wide, refcounted, CAS-based state machine.
 *
 * Internal header — not part of the public API.
 */
#ifndef STA_FUTURE_H
#define STA_FUTURE_H

#include "vm/oop.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    STA_FUTURE_PENDING   = 0,
    STA_FUTURE_RESOLVED  = 1,
    STA_FUTURE_FAILED    = 2,
    STA_FUTURE_TIMED_OUT = 3,
    STA_FUTURE_RESOLVING = 4,  /* transient: CAS winner storing fields */
    STA_FUTURE_FAILING   = 5   /* transient: CAS winner storing fields */
} STA_FutureState;

typedef struct STA_Heap STA_Heap;   /* forward decl */

typedef struct STA_Future {
    _Atomic uint32_t    state;             /* STA_FutureState via CAS */
    uint32_t            future_id;         /* globally unique, monotonic */
    uint32_t            sender_id;         /* actor_id of requesting actor */
    _Atomic uint32_t    refcount;          /* sender + resolution path */
    STA_OOP            *result_buf;        /* malloc'd OOP array (transfer buffer) */
    uint32_t            result_count;      /* number of OOPs in result_buf */
    _Atomic uint32_t    waiter_actor_id;   /* actor suspended on wait, 0=none (Epic 7B) */
    STA_Heap           *transfer_heap;     /* malloc'd heap for mutable results, NULL if imm */
} STA_Future;

/* Lifecycle — retain/release are thread-safe */
STA_Future *sta_future_retain(STA_Future *f);
void        sta_future_release(STA_Future *f);

/* State transitions — return true if THIS call won the CAS.
   buf is a malloc'd OOP array that the future takes ownership of.
   If CAS fails (already terminal), buf is freed by this function. */
bool sta_future_resolve(STA_Future *f, STA_OOP *buf, uint32_t count,
                        STA_Heap *transfer_heap);
bool sta_future_fail(STA_Future *f, STA_OOP *buf, uint32_t count);

#endif /* STA_FUTURE_H */
