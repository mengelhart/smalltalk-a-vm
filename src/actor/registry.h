/* src/actor/registry.h
 * VM-wide actor registry — actor_id → STA_Actor* indirection layer.
 *
 * Mutex-protected open-addressing hash map. Non-owning pointers:
 * the registry does not own actors — it holds pointers for lookup.
 * sta_registry_destroy frees the table, not the actors.
 *
 * Internal header — not part of the public API.
 */
#pragma once

#include <stdint.h>
#include <pthread.h>

struct STA_Actor;

typedef struct STA_ActorRegistry STA_ActorRegistry;

/* Lifecycle — called from STA_VM create/destroy. */
STA_ActorRegistry *sta_registry_create(uint32_t initial_capacity);
void               sta_registry_destroy(STA_ActorRegistry *reg);

/* Registration — called from actor create/destroy.
 * register: inserts actor_id → actor mapping. If the ID already exists,
 * the pointer is updated (defensive — should not happen in practice).
 * unregister: removes the mapping for actor_id. */
void               sta_registry_register(STA_ActorRegistry *reg,
                                          struct STA_Actor *actor);
void               sta_registry_unregister(STA_ActorRegistry *reg,
                                            uint32_t actor_id);

/* Lookup — returns NULL if actor does not exist or was unregistered. */
struct STA_Actor  *sta_registry_lookup(STA_ActorRegistry *reg,
                                        uint32_t actor_id);

/* Diagnostics — number of live entries. */
uint32_t           sta_registry_count(STA_ActorRegistry *reg);
