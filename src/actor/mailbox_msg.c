/* src/actor/mailbox_msg.c
 * Message envelope allocation — Phase 2 Epic 3.
 * See mailbox_msg.h.
 */
#include "mailbox_msg.h"
#include <stdlib.h>
#include <string.h>

STA_MailboxMsg *sta_mailbox_msg_create(STA_OOP selector,
                                        STA_OOP *args,
                                        uint8_t arg_count,
                                        uint32_t sender_id) {
    STA_MailboxMsg *msg = calloc(1, sizeof(STA_MailboxMsg));
    if (!msg) return NULL;

    msg->selector  = selector;
    msg->args      = args;
    msg->arg_count = arg_count;
    msg->sender_id = sender_id;

    return msg;
}

void sta_mailbox_msg_destroy(STA_MailboxMsg *msg) {
    if (!msg) return;
    /* Free args if the message owns them (zero-copy send path). */
    if (msg->args_owned && msg->args)
        free(msg->args);
    free(msg);
}
