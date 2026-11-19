/*
 * web-files.c - Files sent with a message, read and named
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The parts of sending a file that do not need a daemon: reading the
 * composer's multipart body, deciding which agent a room's upload is
 * filed under, and turning what attachment.put answered into the block
 * the message carries.  Kept apart from the routes so a test can drive
 * them with a hand-written body and a fake put, and assert on the exact
 * message an agent would receive -- the routes themselves need a live
 * daemon, which no unit test may touch.
 */

#include "clawtilla.h"

#include <string.h>

#include "web-pages.h"

GPtrArray *
clawt_web_composer_read(const gchar  *content_type,
                        GBytes       *raw,
                        gchar       **body,
                        GError      **error)
{
    g_autoptr(GPtrArray) parsed = NULL;
    g_autoptr(GHashTable) fields = NULL;
    GPtrArray *files;
    guint i;

    g_return_val_if_fail(body != NULL, NULL);

    *body = NULL;

    if (content_type == NULL ||
        !g_str_has_prefix(content_type, "multipart/form-data"))
        return g_ptr_array_new_with_free_func(
            (GDestroyNotify)htmx_uploaded_file_free);

    if (raw == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "the message arrived with no body");
        return NULL;
    }

    parsed = htmx_uploaded_file_parse_multipart(content_type, raw, &fields,
                                                error);

    if (parsed == NULL)
        return NULL;

    if (fields != NULL)
        *body = g_strdup(g_hash_table_lookup(fields, "body"));

    /*
     * Every file input is in the body whether or not anybody chose a
     * file: an empty one arrives as a part with `filename=""` and no
     * bytes.  Sending that on would put an unnamed "(could not be
     * saved)" line under every plain message, so it is dropped here --
     * and only that: a real file that happens to be empty is still
     * passed on, for attachment.put to refuse in words.
     */
    files = g_ptr_array_new_with_free_func(
        (GDestroyNotify)htmx_uploaded_file_free);

    for (i = 0; i < parsed->len; i++) {
        HtmxUploadedFile *file = g_ptr_array_index(parsed, i);
        const gchar *name = htmx_uploaded_file_get_filename(file);

        if (name == NULL || *name == '\0')
            continue;

        g_ptr_array_add(files, htmx_uploaded_file_copy(file));
    }

    return files;
}

gchar *
clawt_web_room_file_agent(JsonArray *members)
{
    const gchar *agent = NULL;
    guint i;

    /*
     * Exactly one agent, or nobody.  attachment.put writes into one
     * agent's exchange directory, so a room with two agents has no
     * answer to "whose workspace does this go in" that would not hide
     * the file from the other -- and a file only one of the people the
     * message is addressed to can open is worse than no Attach button.
     */
    for (i = 0; members != NULL && i < json_array_get_length(members); i++) {
        const gchar *member = json_array_get_string_element(members, i);

        if (member == NULL || g_strcmp0(member, "user") == 0)
            continue;

        if (agent != NULL && g_strcmp0(agent, member) != 0)
            return NULL;

        agent = member;
    }

    return g_strdup(agent);
}

gchar *
clawt_web_body_with_files(const gchar      *agent_id,
                          const gchar      *body,
                          GPtrArray        *files,
                          ClawtWebPutFunc   put,
                          gpointer          user_data)
{
    GString *out;
    guint i;

    g_return_val_if_fail(put != NULL, NULL);

    if (files == NULL || files->len == 0)
        return g_strdup(body != NULL ? body : "");

    /* The shared builder: one spelling of the block for every client. */
    out = clawt_attachment_block_new(body);

    for (i = 0; i < files->len; i++) {
        const HtmxUploadedFile *file = g_ptr_array_index(files, i);
        const gchar *name = htmx_uploaded_file_get_filename(file);
        GBytes *bytes = htmx_uploaded_file_get_data(file);
        g_autoptr(JsonNode) reply = NULL;
        g_autofree gchar *encoded = NULL;
        const guint8 *data = NULL;
        gsize size = 0;
        JsonObject *root;

        if (bytes != NULL)
            data = g_bytes_get_data(bytes, &size);

        /*
         * Empty, or past what the socket carries well.  Listed rather
         * than put: base64 over IPC grows a file by a third, and the
         * daemon reads a frame whole, so a very large one stalls every
         * other client waiting behind it.  The same 32 MB the lrg
         * client refuses at.
         */
        if (data == NULL || size == 0 || size > CLAWT_WEB_ATTACHMENT_MAX) {
            clawt_attachment_block_add_unsaved(out, name);
            continue;
        }

        encoded = g_base64_encode(data, size);
        reply = put(agent_id, name, encoded, user_data);

        if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply)) {
            clawt_attachment_block_add_unsaved(out, name);
            continue;
        }

        /*
         * attachment.put's own answer, read without assuming every
         * member is there: `path` and `id` are absent from older
         * daemons, and the block builder leaves out what is missing.
         * Either the payload itself or a whole frame carrying one, so
         * a put function may hand back whichever its transport has.
         */
        root = json_node_get_object(reply);

        if (json_object_has_member(root, "payload") &&
            JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "payload")))
            root = json_object_get_object_member(root, "payload");

        clawt_attachment_block_add(
            out,
            clawt_ipc_payload_string(root, "name") != NULL
                ? clawt_ipc_payload_string(root, "name") : name,
            clawt_ipc_payload_string(root, "host_path"),
            clawt_ipc_payload_string(root, "path"),
            clawt_ipc_payload_string(root, "id"));
    }

    return g_string_free(out, FALSE);
}
