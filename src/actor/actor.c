#include "sta/vm.h"

STA_Actor* sta_actor_spawn(STA_VM* vm, STA_Handle* class_handle) {
    (void)vm; (void)class_handle;
    return NULL;
}

int sta_actor_send(STA_VM* vm, STA_Actor* actor, STA_Handle* message) {
    (void)vm; (void)actor; (void)message;
    return STA_ERR_INTERNAL;
}
