/*
 * clawt-draft-store.c - Composer text somebody has not sent yet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "chat/clawt-draft-store.h"

#include <glib/gstdio.h>
#include <yaml-glib.h>

static gchar *
resolve_path(const gchar *path)
{
    if (path != NULL && *path != '\0')
        return g_strdup(path);

    return clawt_draft_store_default_path();
}

gchar *
clawt_draft_store_default_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "clawtilla",
                            "drafts.yaml", NULL);
}

gchar *
clawt_draft_key(const gchar *profile, const gchar *room_id)
{
    g_return_val_if_fail(room_id != NULL, NULL);

    /*
     * A slash, because an agent id cannot contain one -- clawt_is_valid_id()
     * allows lowercase letters, digits, "-" and "_" and nothing else -- so
     * the two halves can always be told apart again.
     */
    return g_strdup_printf("%s/%s",
                           (profile != NULL && *profile != '\0')
                               ? profile : "local",
                           room_id);
}

GHashTable *
clawt_draft_store_load(const gchar *path, GError **error)
{
    g_autofree gchar *resolved = resolve_path(path);
    g_autofree gchar *text = NULL;
    g_autoptr(GHashTable) drafts = NULL;
    g_autoptr(YamlParser) parser = NULL;
    YamlNode *root;
    YamlNode *node;
    YamlMapping *mapping;
    GList *members;
    GList *l;

    drafts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    /*
     * A file that is not there is an empty set of drafts.  Nobody has
     * typed anything yet is the ordinary case, and reporting it as a
     * failure would put a warning in front of somebody on their first
     * run.
     */
    if (!g_file_test(resolved, G_FILE_TEST_EXISTS))
        return g_steal_pointer(&drafts);

    if (!g_file_get_contents(resolved, &text, NULL, error))
        return NULL;

    /* An empty file is an empty set of drafts, not a parse error. */
    if (*text == '\0')
        return g_steal_pointer(&drafts);

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_data(parser, text, -1, error))
        return NULL;

    root = yaml_parser_get_root(parser);

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
        return g_steal_pointer(&drafts);

    node = yaml_mapping_get_member(yaml_node_get_mapping(root), "drafts");

    if (node == NULL || yaml_node_get_node_type(node) != YAML_NODE_MAPPING)
        return g_steal_pointer(&drafts);

    mapping = yaml_node_get_mapping(node);
    members = yaml_mapping_get_members(mapping);

    for (l = members; l != NULL; l = l->next) {
        const gchar *room = l->data;
        YamlNode *value = yaml_mapping_get_member(mapping, room);
        const gchar *stored;

        if (value == NULL ||
            yaml_node_get_node_type(value) != YAML_NODE_SCALAR)
            continue;

        stored = yaml_node_get_string(value);

        if (stored == NULL)
            continue;

        /*
         * Unescaped on the way out because it was escaped on the way in;
         * see the comment on the writer.
         */
        g_hash_table_insert(drafts, g_strdup(room), g_strcompress(stored));
    }

    g_list_free(members);

    return g_steal_pointer(&drafts);
}

gboolean
clawt_draft_store_save(const gchar *path, GHashTable *drafts, GError **error)
{
    g_autofree gchar *resolved = resolve_path(path);
    g_autofree gchar *dir = NULL;
    g_autoptr(GString) out = NULL;
    g_autoptr(GList) rooms = NULL;
    GList *l;

    g_return_val_if_fail(drafts != NULL, FALSE);

    dir = g_path_get_dirname(resolved);

    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;

    out = g_string_new(
        "# clawtilla composer drafts\n"
        "#\n"
        "# What was in a composer when the conversation was last left.\n"
        "# This is the client's own file, beside connections.yaml -- a\n"
        "# half-typed message belongs to the person rather than to the\n"
        "# fleet, and a laptop reaching a workstation may have no fleet\n"
        "# at all.\n"
        "#\n"
        "# The text is C-escaped inside a single-quoted scalar, so a\n"
        "# draft with newlines in it round-trips exactly. YAML would fold\n"
        "# a real newline inside a quoted scalar into a space, which\n"
        "# silently reflows what somebody wrote.\n"
        "\n"
        "drafts:\n");

    rooms = g_hash_table_get_keys(drafts);

    /*
     * Sorted, so writing the same drafts twice produces the same bytes.
     * A file that reorders itself on every save is a file nobody can
     * keep in version control or diff against yesterday.
     */
    rooms = g_list_sort(rooms, (GCompareFunc)g_strcmp0);

    if (rooms == NULL)
        g_string_append(out, "  {}\n");

    for (l = rooms; l != NULL; l = l->next) {
        const gchar *room = l->data;
        const gchar *text = g_hash_table_lookup(drafts, room);
        g_autofree gchar *escaped_text = NULL;
        const gchar *p;

        if (text == NULL || *text == '\0')
            continue;

        escaped_text = g_strescape(text, NULL);

        g_string_append(out, "  '");

        /* A single-quoted YAML scalar escapes only by doubling the quote. */
        for (p = room; *p != '\0'; p++) {
            if (*p == '\'')
                g_string_append(out, "''");
            else
                g_string_append_c(out, *p);
        }

        g_string_append(out, "': '");

        for (p = escaped_text; *p != '\0'; p++) {
            if (*p == '\'')
                g_string_append(out, "''");
            else
                g_string_append_c(out, *p);
        }

        g_string_append(out, "'\n");
    }

    return clawt_write_file_atomic(resolved, out->str, -1, 0600, FALSE, error);
}

gchar *
clawt_draft_store_get(const gchar *path, const gchar *room_id)
{
    g_autoptr(GHashTable) drafts = NULL;
    const gchar *text;

    if (room_id == NULL)
        return NULL;

    drafts = clawt_draft_store_load(path, NULL);

    if (drafts == NULL)
        return NULL;

    text = g_hash_table_lookup(drafts, room_id);

    return (text != NULL) ? g_strdup(text) : NULL;
}

gboolean
clawt_draft_store_set(const gchar  *path,
                      const gchar  *room_id,
                      const gchar  *text,
                      GError      **error)
{
    g_autoptr(GHashTable) drafts = NULL;

    g_return_val_if_fail(room_id != NULL, FALSE);

    drafts = clawt_draft_store_load(path, NULL);

    if (drafts == NULL)
        drafts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                       g_free, g_free);

    if (text == NULL || *text == '\0')
        g_hash_table_remove(drafts, room_id);
    else
        g_hash_table_insert(drafts, g_strdup(room_id), g_strdup(text));

    return clawt_draft_store_save(path, drafts, error);
}
