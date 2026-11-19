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

/* ── A conversation's files, as a grid ─────────────────────────────── */

/* "scout · 2026-11-18 14:02 · 1.2 MB", for under a tile's name. */
static gchar *
tile_meta(JsonObject *item)
{
    g_autoptr(GDateTime) when = NULL;
    g_autofree gchar *date = NULL;
    g_autofree gchar *size = NULL;
    const gchar *sender = json_object_get_string_member_with_default(
        item, "sender", "");
    gint64 ts = json_object_get_int_member_with_default(item, "ts", 0);
    gint64 bytes = json_object_get_int_member_with_default(item, "bytes", 0);

    if (ts > 0)
        when = g_date_time_new_from_unix_local(ts);

    if (when != NULL)
        date = g_date_time_format(when, "%Y-%m-%d %H:%M");

    if (bytes > 0)
        size = g_format_size((guint64)bytes);

    /* the operator's own files are "you", as the transcript says */
    return g_strdup_printf("%s%s%s%s%s",
                           g_strcmp0(sender, "user") == 0 ? "you" : sender,
                           date != NULL ? " \xc2\xb7 " : "",
                           date != NULL ? date : "",
                           size != NULL ? " \xc2\xb7 " : "",
                           size != NULL ? size : "");
}

/*
 * The picture part of a tile.  Images are their own thumbnail, fetched
 * through /f/attachment/<id>; a PDF's first page comes from
 * /f/attachment/<id>/thumb; a text file shows the lines the page asked
 * the daemon for; anything else, or a file that is gone, is a label.
 */
static HtmxElement *
tile_preview(JsonObject *item, GHashTable *texts)
{
    const gchar *id = json_object_get_string_member_with_default(
        item, "id", NULL);
    const gchar *name = json_object_get_string_member_with_default(
        item, "name", "file");
    const gchar *kind = json_object_get_string_member_with_default(
        item, "kind", "other");
    gboolean exists = json_object_get_boolean_member_with_default(
        item, "exists", FALSE);
    const gchar *text = (id != NULL && texts != NULL)
        ? g_hash_table_lookup(texts, id) : NULL;
    g_autofree gchar *escaped = NULL;
    g_autofree gchar *url = NULL;

    if (id == NULL || !exists) {
        g_autoptr(HtmxSpan) label = htmx_span_new();

        htmx_element_add_class(HTMX_ELEMENT(label), "file-tile-label");
        htmx_node_set_text_content(
            HTMX_NODE(label),
            !exists ? "gone" : "only on the daemon's machine");

        return HTMX_ELEMENT(g_steal_pointer(&label));
    }

    escaped = g_uri_escape_string(id, NULL, FALSE);

    if (g_strcmp0(kind, "image") == 0 || g_strcmp0(kind, "pdf") == 0) {
        g_autoptr(HtmxImg) picture = NULL;

        url = g_strcmp0(kind, "pdf") == 0
            ? g_strdup_printf("/f/attachment/%s/thumb?size=256", escaped)
            : g_strdup_printf("/f/attachment/%s", escaped);
        picture = htmx_img_new_with_src(url, name);
        htmx_element_set_attribute(HTMX_ELEMENT(picture), "loading", "lazy");
        htmx_element_add_class(HTMX_ELEMENT(picture), "file-tile-thumb");

        return HTMX_ELEMENT(g_steal_pointer(&picture));
    }

    if (text != NULL) {
        g_autoptr(HtmxPre) pre = htmx_pre_new_with_text(text);

        htmx_element_add_class(HTMX_ELEMENT(pre), "file-tile-text");

        return HTMX_ELEMENT(g_steal_pointer(&pre));
    }

    {
        g_autoptr(HtmxSpan) label = htmx_span_new();

        htmx_element_add_class(HTMX_ELEMENT(label), "file-tile-label");
        htmx_node_set_text_content(HTMX_NODE(label), kind);

        return HTMX_ELEMENT(g_steal_pointer(&label));
    }
}

HtmxElement *
clawt_web_files_grid(JsonArray *items, GHashTable *texts)
{
    g_autoptr(HtmxDiv) grid = htmx_div_new();
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(grid), "file-grid");

    if (items == NULL || json_array_get_length(items) == 0) {
        g_autoptr(HtmxP) none = htmx_p_new();

        htmx_element_add_class(HTMX_ELEMENT(none), "lede");
        htmx_node_set_text_content(HTMX_NODE(none),
                                   "No files have been sent in this "
                                   "conversation yet.");
        htmx_node_add_child(HTMX_NODE(grid), HTMX_NODE(none));

        return HTMX_ELEMENT(g_steal_pointer(&grid));
    }

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *item = json_array_get_object_element(items, i);
        g_autoptr(HtmxDiv) tile = htmx_div_new();
        g_autoptr(HtmxDiv) caption = htmx_div_new();
        g_autoptr(HtmxSpan) meta = htmx_span_new();
        g_autofree gchar *said = NULL;
        const gchar *id;
        const gchar *name;
        gboolean exists;

        if (item == NULL)
            continue;

        id = json_object_get_string_member_with_default(item, "id", NULL);
        name = json_object_get_string_member_with_default(item, "name",
                                                          "file");
        exists = json_object_get_boolean_member_with_default(item, "exists",
                                                             FALSE);

        htmx_element_add_class(HTMX_ELEMENT(tile), "file-tile");
        htmx_element_set_attribute(
            HTMX_ELEMENT(tile), "data-kind",
            json_object_get_string_member_with_default(item, "kind",
                                                       "other"));

        if (!exists)
            htmx_element_add_class(HTMX_ELEMENT(tile), "file-gone");

        /* add_child takes its own reference; this one is ours to drop */
        {
            g_autoptr(HtmxElement) preview = tile_preview(item, texts);

            htmx_node_add_child(HTMX_NODE(tile), HTMX_NODE(preview));
        }

        htmx_element_add_class(HTMX_ELEMENT(caption), "file-tile-caption");

        /*
         * The name opens or downloads the file through the server, the
         * way a transcript preview does -- the browser may not be on
         * the daemon's machine.  A file named only by a host path, or
         * one that is gone, is a name and nothing to click.
         */
        if (id != NULL && exists) {
            g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
            g_autofree gchar *url = g_strdup_printf("/f/attachment/%s",
                                                    escaped);
            g_autoptr(HtmxA) link = htmx_a_new_with_href(url);

            htmx_element_add_class(HTMX_ELEMENT(link), "file-tile-name");
            htmx_element_set_attribute(HTMX_ELEMENT(link), "target",
                                       "_blank");
            htmx_element_set_attribute(HTMX_ELEMENT(link), "rel",
                                       "noopener");
            htmx_node_set_text_content(HTMX_NODE(link), name);
            htmx_node_add_child(HTMX_NODE(caption), HTMX_NODE(link));
        } else {
            g_autoptr(HtmxSpan) label = htmx_span_new();
            const gchar *path = json_object_get_string_member_with_default(
                item, "path", NULL);

            htmx_element_add_class(HTMX_ELEMENT(label), "file-tile-name");
            htmx_node_set_text_content(HTMX_NODE(label), name);

            if (path != NULL)
                htmx_element_set_attribute(HTMX_ELEMENT(label), "title",
                                           path);

            htmx_node_add_child(HTMX_NODE(caption), HTMX_NODE(label));
        }

        said = tile_meta(item);
        htmx_element_add_class(HTMX_ELEMENT(meta), "file-tile-meta");
        htmx_node_set_text_content(HTMX_NODE(meta), said);
        htmx_node_add_child(HTMX_NODE(caption), HTMX_NODE(meta));

        htmx_node_add_child(HTMX_NODE(tile), HTMX_NODE(caption));
        htmx_node_add_child(HTMX_NODE(grid), HTMX_NODE(tile));
    }

    return HTMX_ELEMENT(g_steal_pointer(&grid));
}
