/*
 * daemon-mailbox.c - The client surface: mailbox.*
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

JsonNode *
clawt_daemon_handle_mailbox(
    ClawtDaemon  *self,
    const gchar  *kind,
    JsonNode     *request,
    JsonObject   *payload,
    gboolean     *handled
)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;

    builder = json_builder_new();
    *handled = TRUE;

    /* ── mailboxes ── */
    if (g_strcmp0(kind, "mailbox.list") == 0 ||
        g_strcmp0(kind, "mailbox.dead") == 0) {
        ClawtMailbox *mailbox = clawt_daemon_mailbox_for(self, payload,
                                                         &error);
        g_autoptr(GPtrArray) items = NULL;
        guint i;

        if (mailbox == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (g_strcmp0(kind, "mailbox.dead") == 0) {
            items = clawt_mailbox_dead_letters(mailbox);
        } else {
            ClawtMailboxFilter filter = { CLAWT_MAILBOX_PENDING, 50, TRUE };

            filter.limit = (guint)clawt_ipc_payload_int(payload, "limit", 50);
            items = clawt_mailbox_list(mailbox, &filter);
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "items");
        json_builder_begin_array(builder);

        for (i = 0; i < items->len; i++)
            clawt_daemon_add_mailbox_item(builder,
                                          g_ptr_array_index(items, i));

        json_builder_end_array(builder);
        json_builder_set_member_name(builder, "depth");
        json_builder_add_int_value(builder, clawt_mailbox_depth(mailbox));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "mailbox.ack") == 0 ||
        g_strcmp0(kind, "mailbox.requeue") == 0) {
        ClawtMailbox *mailbox = clawt_daemon_mailbox_for(self, payload,
                                                         &error);
        const gchar *item_id = clawt_ipc_payload_string(payload, "item");
        gboolean ok;

        if (mailbox == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (item_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which message?");

        ok = (g_strcmp0(kind, "mailbox.ack") == 0)
             ? clawt_mailbox_ack(mailbox, item_id, &error)
             : clawt_mailbox_requeue(mailbox, item_id, &error);

        if (!ok)
            return clawt_ipc_error_new(request, error->code, error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "mailbox.purge") == 0) {
        ClawtMailbox *mailbox = clawt_daemon_mailbox_for(self, payload,
                                                         &error);

        if (mailbox == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "purged");
        json_builder_add_int_value(builder,
                                   clawt_mailbox_purge_expired(mailbox));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    *handled = FALSE;
    return NULL;
}
