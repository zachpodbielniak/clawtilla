/*
 * web-extras.c - Files, memories, ordering, attachments and integrations
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The pieces of the GTK client that did not fall naturally into one of
 * the seven views: the agent's own org files, its memory store, dragging
 * the sidebar into an order, attaching a file to a message, and the
 * integration editor with its Matrix login.
 *
 * They are here rather than spread across the view modules because they
 * were built together, to close the gaps tools/clawt-client-parity.sh
 * found -- and keeping them in one place makes it obvious what that check
 * is for.
 */

#include "web-pages.h"

#include <string.h>

/* ── The agent's own files ───────────────────────────────────────── */

void
clawt_web_add_files_card(ClawtWebApp *app, HtmxElement *parent,
                         const gchar *agent_id)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonArray *files;
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    guint i;

    clawt_web_payload_set(payload, "agent", agent_id);
    reply = clawt_web_app_call(app, "agent.files",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    root = clawt_web_root(reply);
    files = clawt_web_member_array(root, "files");

    card = clawt_web_card("Workspace files",
                          clawt_web_member(root, "workspace", NULL));
    body = clawt_web_card_body(card);

    if (files == NULL || json_array_get_length(files) == 0) {
        clawt_web_add(body, clawt_web_empty(
            "No files yet",
            "They are scaffolded when the agent first starts."));
    }

    for (i = 0; files != NULL && i < json_array_get_length(files); i++) {
        JsonObject *file = json_array_get_object_element(files, i);
        const gchar *name = clawt_web_member(file, "name", "?");
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxDiv) head = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
        htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

        {
            g_autoptr(HtmxSpan) label = htmx_span_new();

            htmx_element_add_class(HTMX_ELEMENT(label), "list-item-title");
            htmx_node_set_text_content(HTMX_NODE(label), name);
            htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
        }

        if (clawt_web_member_bool(file, "identity", FALSE))
            clawt_web_add(head, clawt_web_badge("identity", "info"));

        /*
         * A generated file has a region clawtilla owns and nothing else.
         * Saying so is what stops somebody editing the managed half and
         * wondering why it comes back.
         */
        if (clawt_web_member_bool(file, "generated", FALSE))
            clawt_web_add(head, clawt_web_badge("managed region", "warn"));

        if (!clawt_web_member_bool(file, "exists", TRUE))
            clawt_web_add(head, clawt_web_badge("missing", "bad"));

        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

        clawt_web_add(row, clawt_web_text(
            clawt_web_member(file, "title", ""), "list-item-sub"));
        clawt_web_add(row, clawt_web_text(
            clawt_web_member(file, "path", ""), "small muted mono"));

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* ── The memory store ────────────────────────────────────────────── */

void
clawt_web_add_memory_card(ClawtWebApp *app, HtmxElement *parent,
                          const gchar *agent_id, const gchar *query)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *memories;
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Memories",
        "The searchable per-agent store. Not the same thing as the "
        "MEMORY.md budget above -- that one is libreclaw's, and is only "
        "a size limit.");
    HtmxElement *body = clawt_web_card_body(card);
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    g_autofree gchar *action = g_strdup_printf("/a/%s/memories", escaped);
    g_autoptr(HtmxForm) form = clawt_web_form(action);
    guint i;

    clawt_web_payload_set(payload, "agent", agent_id);

    if (query != NULL && *query != '\0')
        clawt_web_payload_set(payload, "query", query);

    /*
     * A query typed by a person goes straight into FTS5's parser, where
     * a stray quote or a bare NOT is a syntax error rather than a search
     * -- and a failed search reports no matches, not an error, so it
     * looks exactly like an empty store. The daemon quotes it as a
     * phrase; this only has to say so when nothing comes back.
     */
    reply = clawt_web_app_call(
        app, (query != NULL && *query != '\0') ? "memory.search"
                                               : "memory.list",
        clawt_web_payload_take(g_steal_pointer(&payload)));

    clawt_web_add(form, clawt_web_field("Search", "q", query,
                                        "what to look for"));

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) go = clawt_web_button("Search", "default");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(go), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(go));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));

    memories = clawt_web_member_array(clawt_web_root(reply), "memories");

    if (reply == NULL) {
        clawt_web_add(body, clawt_web_notice(
            clawt_web_app_last_error(app), "bad"));
    } else if (memories == NULL || json_array_get_length(memories) == 0) {
        clawt_web_add(body, clawt_web_empty(
            (query != NULL && *query != '\0') ? "Nothing matched"
                                              : "Nothing remembered yet",
            "An agent writes here through its own tools. Memories are off "
            "unless memories.enabled is set."));
    }

    for (i = 0; memories != NULL && i < json_array_get_length(memories); i++) {
        JsonObject *memory = json_array_get_object_element(memories, i);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxDiv) head = htmx_div_new();
        g_autofree gchar *when = clawt_web_relative_time(
            clawt_web_member_int(memory, "created_at", 0));

        htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
        htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

        {
            g_autoptr(HtmxSpan) label = htmx_span_new();

            htmx_element_add_class(HTMX_ELEMENT(label), "list-item-title");
            htmx_node_set_text_content(
                HTMX_NODE(label), clawt_web_member(memory, "title",
                                                   clawt_web_member(memory,
                                                                    "id", "?")));
            htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
        }

        if (clawt_web_member(memory, "category", NULL) != NULL)
            clawt_web_add(head, clawt_web_badge(
                clawt_web_member(memory, "category", ""), "info"));

        if (clawt_web_member_bool(memory, "pinned", FALSE))
            clawt_web_add(head, clawt_web_badge("pinned", "warn"));

        if (*when != '\0') {
            g_autoptr(HtmxSpan) stamp = htmx_span_new();

            htmx_element_add_class(HTMX_ELEMENT(stamp), "muted small");
            htmx_node_set_text_content(HTMX_NODE(stamp), when);
            htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(stamp));
        }

        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));
        clawt_web_add(row, clawt_web_text(
            clawt_web_member(memory, "body", ""), "list-item-sub"));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* ── Ordering the fleet ──────────────────────────────────────────── */

/*
 * Move one agent up or down, by sending the whole list.
 *
 * The daemon numbers from what it is given, so one frame describes the
 * arrangement completely -- a stale client cannot produce a
 * half-applied reorder, and an id the daemon no longer has is skipped
 * rather than refused.
 *
 * Up and down buttons rather than dragging, because dragging needs
 * JavaScript to be worth anything and this page works without it. The
 * order itself lives in clawtilla.yaml either way, so an agent moved to
 * the top here is at the top in the GTK client too.
 */
static HtmxResponse *
on_reorder(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *direction = htmx_request_get_query_param(request,
                                                          "direction");
    g_autoptr(JsonNode) list = clawt_web_app_call(app, "agent.list", NULL);
    JsonArray *agents = clawt_web_member_array(clawt_web_root(list), "agents");
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(ClawtWebPayload) payload = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *joined = NULL;
    gint index = -1;
    gint swap_with;
    guint i;

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        const gchar *id = clawt_web_member(
            json_array_get_object_element(agents, i), "id", NULL);

        if (id == NULL)
            continue;

        if (g_strcmp0(id, agent_id) == 0)
            index = (gint)ids->len;

        g_ptr_array_add(ids, g_strdup(id));
    }

    if (index < 0)
        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_WEB_VIEW_CHAT, NULL);

    swap_with = (g_strcmp0(direction, "up") == 0) ? index - 1 : index + 1;

    if (swap_with < 0 || swap_with >= (gint)ids->len)
        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_WEB_VIEW_CHAT,
                                      "Already at the end.");

    {
        gpointer moved = g_ptr_array_index(ids, (guint)index);

        ids->pdata[index] = ids->pdata[swap_with];
        ids->pdata[swap_with] = moved;
    }

    g_ptr_array_add(ids, NULL);
    joined = g_strjoinv(",", (gchar **)ids->pdata);

    payload = clawt_web_payload_new();
    clawt_web_payload_set(payload, "agents", joined);

    reply = clawt_web_app_call(app, "agent.reorder",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_CHAT, NULL);
}

/*
 * Move one agent onto a team from the sidebar.
 *
 * The counterpart of the GTK client's drag onto a team heading.  Not a
 * drag here for the same reason the ordering is buttons: dragging needs
 * JavaScript to be worth anything, and this page works without it.  The
 * result is identical either way -- both write `agents.team`, so an
 * agent moved here has moved in the GTK client too.
 *
 * Registered from web-extras rather than beside the views, because
 * `/a/:id/:view` matches everything under an agent and answers 200 with
 * the chat page for anything it swallows.
 */
static HtmxResponse *
on_set_team(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *team = clawt_web_form_value(request, "team");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *said = NULL;

    /*
     * An absent field is a form that was not filled in; an empty one is
     * somebody choosing "No team", which is a value rather than a
     * refusal to answer.
     */
    if (team == NULL)
        return clawt_web_after_action(app, request, agent_id,
                                      CLAWT_WEB_VIEW_CHAT, NULL);

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "key", "team");
    clawt_web_payload_set(payload, "value", team);

    reply = clawt_web_app_call(app, "agent.set",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL) {
        /* Copied here: rendering the page below frees it and writes another. */
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT, why);
    }

    /*
     * Named the way the sidebar names it.  The form carries the id
     * because that is what `agents.team` holds, but "moved to research"
     * and a heading reading "Research" are two names for one thing, and
     * a person reading the toast has no way to know that.
     */
    if (*team != '\0') {
        g_autoptr(JsonNode) teams = clawt_web_app_call(app, "team.list", NULL);
        JsonArray *list = clawt_web_member_array(clawt_web_root(teams),
                                                 "teams");
        guint i;
        const gchar *name = team;

        for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
            JsonObject *one = json_array_get_object_element(list, i);

            if (g_strcmp0(clawt_web_member(one, "id", ""), team) == 0) {
                name = clawt_web_member(one, "name", team);
                break;
            }
        }

        said = g_strdup_printf("%s moved to %s.", agent_id, name);
    } else {
        said = g_strdup_printf("%s taken off its team.", agent_id);
    }

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_CHAT, said);
}

/* ── Attachments ─────────────────────────────────────────────────── */

/*
 * Attaches an uploaded file to the agent's next message.
 *
 * The daemon takes the bytes base64-encoded and writes them where the
 * agent can reach them, then reports both paths -- the host one and the
 * one inside its computer. Both are shown for the reason the first
 * attachment ever sent proved: an agent's own read/write run on the
 * *host*, and only computer_exec enters the container, so a path given
 * without saying which side it is on gets looked for on the wrong one.
 */
static HtmxResponse *
on_attach(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(GPtrArray) files = NULL;
    g_autoptr(GHashTable) fields = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *encoded = NULL;
    const HtmxUploadedFile *file;
    const gchar *name;
    JsonObject *root;
    g_autofree gchar *said = NULL;

    /*
     * A real file input, parsed out of the multipart body. The daemon
     * takes the bytes base64-encoded, and asking somebody to encode a
     * file by hand before pasting it into a text box is not a feature.
     */
    files = htmx_uploaded_file_parse_multipart(
        htmx_request_get_content_type(request),
        htmx_request_get_body_bytes(request), &fields, &error);

    if (files == NULL || files->len == 0)
        return clawt_web_error_page(
            app, request, agent_id, CLAWT_WEB_VIEW_CHAT,
            error != NULL ? error->message : "Choose a file first.");

    file = g_ptr_array_index(files, 0);
    name = htmx_uploaded_file_get_filename(file);

    {
        GBytes *bytes = htmx_uploaded_file_get_data(file);
        gsize size = 0;
        const guint8 *data;

        if (bytes == NULL)
            return clawt_web_error_page(app, request, agent_id,
                                        CLAWT_WEB_VIEW_CHAT,
                                        "That file is empty.");

        data = g_bytes_get_data(bytes, &size);

        if (data == NULL || size == 0)
            return clawt_web_error_page(app, request, agent_id,
                                        CLAWT_WEB_VIEW_CHAT,
                                        "That file is empty.");

        encoded = g_base64_encode(data, size);
    }

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "name", name);
    clawt_web_payload_set(payload, "data", encoded);

    reply = clawt_web_app_call(app, "attachment.put",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT,
                                    clawt_web_app_last_error(app));

    root = clawt_web_root(reply);
    said = g_strdup_printf(
        "Attached. On this machine: %s%s%s",
        clawt_web_member(root, "host_path",
                         clawt_web_member(root, "path", name)),
        clawt_web_member(root, "guest_path", NULL) != NULL
            ? "  ·  inside its computer: " : "",
        clawt_web_member(root, "guest_path", ""));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_CHAT, said);
}

static HtmxResponse *
on_attach_remove(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *name = htmx_request_get_query_param(request, "name");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "name", name);

    reply = clawt_web_app_call(app, "attachment.remove",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_CHAT,
                                    clawt_web_app_last_error(app));

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_CHAT, "Attachment removed.");
}

/* ── The integration editor ──────────────────────────────────────── */

/*
 * Every key an integration of this type has, from the schema.
 *
 * The daemon keeps no hand-written list of these and neither does this:
 * `integrations.matrix.*` in the schema *is* the list, and adding a key
 * there makes it appear in this form with no edit here. A copy would be
 * a setting that is accepted, reported as saved, and read from nowhere.
 */
static void
add_integration_fields(HtmxElement *form, const gchar *type,
                       JsonObject *values)
{
    g_autofree gchar *prefix = g_strdup_printf("integrations.%s.", type);
    g_autofree gchar *schema_prefix = g_strdup_printf("agents.%s", prefix);
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;
    gboolean any = FALSE;

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *leaf;
        const gchar *value;

        if (!g_str_has_prefix(entry->key, schema_prefix))
            continue;

        leaf = entry->key + strlen(schema_prefix);

        if (*leaf == '\0' || strchr(leaf, '.') != NULL)
            continue;

        if (entry->type == CLAWT_SCHEMA_SECTION)
            continue;

        any = TRUE;
        value = clawt_web_member(values, leaf, NULL);

        if (entry->type == CLAWT_SCHEMA_BOOLEAN) {
            clawt_web_add(form, clawt_web_switch_field(
                leaf, leaf, entry->doc, g_strcmp0(value, "true") == 0));
            continue;
        }

        if (entry->type == CLAWT_SCHEMA_SECRET) {
            clawt_web_add(form, clawt_web_field(
                leaf, leaf, NULL,
                "a secret reference; leave empty to keep what is set"));
            continue;
        }

        clawt_web_add(form, clawt_web_field(leaf, leaf, value, entry->doc));
    }

    if (!any)
        clawt_web_add(form, clawt_web_text(
            "This type declares no settings of its own.", "small muted"));
}

static HtmxElement *
integration_editor(ClawtWebApp *app, const gchar *name)
{
    g_autoptr(HtmxDiv) box = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "integration.list",
                                                   NULL);
    JsonArray *list = clawt_web_member_array(clawt_web_root(reply),
                                             "integrations");
    JsonObject *found = NULL;
    const gchar *type = "";
    g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);
    g_autofree gchar *action = NULL;
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    g_autoptr(HtmxForm) form = NULL;
    guint i;

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *entry = json_array_get_object_element(list, i);

        if (g_strcmp0(clawt_web_member(entry, "name", ""), name) == 0)
            found = entry;
    }

    if (found == NULL) {
        clawt_web_add(box, clawt_web_empty("No such integration", NULL));

        return HTMX_ELEMENT(g_steal_pointer(&box));
    }

    type = clawt_web_member(found, "type", "");
    action = g_strdup_printf("/settings/integrations/%s/save", escaped);
    card = clawt_web_card(name, type);
    body = clawt_web_card_body(card);
    form = clawt_web_form(action);

    clawt_web_add(form, clawt_web_switch_field(
        "Enabled", "enabled", NULL,
        clawt_web_member_bool(found, "enabled", TRUE)));

    add_integration_fields(HTMX_ELEMENT(form), type, found);

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) save = clawt_web_button("Save", "primary");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(card));

    if (g_strcmp0(type, "matrix") == 0) {
        g_autoptr(HtmxDiv) login = clawt_web_card(
            "Sign in to Matrix",
            "The password is used once and never stored. What is kept is "
            "the access token, in a 0600 file under secrets.dir -- the "
            "config gets the path, not the token.");
        HtmxElement *login_body = clawt_web_card_body(login);
        g_autofree gchar *login_action = g_strdup_printf(
            "/settings/integrations/%s/matrix-login", escaped);
        g_autofree gchar *rooms_action = g_strdup_printf(
            "/settings/integrations/%s/matrix-rooms", escaped);
        g_autoptr(HtmxForm) login_form = clawt_web_form(login_action);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) go = clawt_web_button("Sign in", "primary");
        g_autoptr(HtmxInput) password = htmx_input_new(HTMX_INPUT_PASSWORD);
        g_autoptr(HtmxDiv) field = htmx_div_new();
        g_autoptr(HtmxLabel) label = htmx_label_new_with_text("Password");

        clawt_web_add(login_form, clawt_web_field(
            "Homeserver", "homeserver",
            clawt_web_member(found, "homeserver", NULL),
            "https://matrix.example.org"));
        clawt_web_add(login_form, clawt_web_field(
            "User", "user", clawt_web_member(found, "user_id", NULL),
            "@bot:example.org"));

        htmx_element_add_class(HTMX_ELEMENT(field), "field");
        htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(label));
        htmx_input_set_name(password, "password");
        htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(password));
        htmx_node_add_child(HTMX_NODE(login_form), HTMX_NODE(field));

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(go), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(go));
        clawt_web_add(row, clawt_web_post_button(
            "List its rooms", rooms_action, "default", NULL));
        htmx_node_add_child(HTMX_NODE(login_form), HTMX_NODE(row));

        htmx_node_add_child(HTMX_NODE(login_body), HTMX_NODE(login_form));
        htmx_node_add_child(HTMX_NODE(box), HTMX_NODE(login));
    }

    return HTMX_ELEMENT(g_steal_pointer(&box));
}

/* ── Connector key ───────────────────────────────────────────────── */

static HtmxResponse *
on_connector_key(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "connector");
    const gchar *key = clawt_web_form_value(request, "key");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    if (key == NULL || *key == '\0')
        return clawt_web_redirect(request, "/settings/connectors");

    clawt_web_payload_set(payload, "name", name);
    clawt_web_payload_set(payload, "key", key);

    reply = clawt_web_app_call(app, "connector.key",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    (void)reply;

    /*
     * Redirected rather than re-rendered, so the key is not left in a
     * form field on a page that stays open, and a refresh does not post
     * it a second time.
     */
    return clawt_web_redirect(request, "/settings/connectors");
}

/* ── Routes ──────────────────────────────────────────────────────── */

/* ── Editing a workspace file ────────────────────────────────────── */

/*
 * The GTK client opens these in $EDITOR, which is a program on the
 * machine somebody is sitting at. A browser reached over the network has
 * no such thing, so the file comes over the wire and goes back the same
 * way -- agent.file_read and agent.file_write exist for this and nothing
 * else.
 */
static HtmxResponse *
on_file_editor(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *name = htmx_request_get_query_param(request, "name");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autofree gchar *html = NULL;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    if (name == NULL || *name == '\0') {
        clawt_web_add(pad, clawt_web_section_title("Workspace files"));
        clawt_web_add_files_card(app, HTMX_ELEMENT(pad), agent_id);
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

        html = clawt_web_page(app, agent_id, CLAWT_WEB_VIEW_AGENT, view,
                              request);

        return clawt_web_html_response(html);
    }

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "name", name);

    reply = clawt_web_app_call(app, "agent.file_read",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_AGENT,
                                    clawt_web_app_last_error(app));

    root = clawt_web_root(reply);

    {
        g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL,
                                                        FALSE);
        g_autofree gchar *action = g_strdup_printf("/a/%s/file", escaped);
        g_autoptr(HtmxDiv) card = clawt_web_card(
            name, clawt_web_member(root, "path", NULL));
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxForm) form = clawt_web_form(action);

        clawt_web_add(pad, clawt_web_section_title(name));

        /*
         * A file clawtilla writes into has a marked region it owns and
         * nothing else. Saying so here is what stops somebody editing
         * the managed half and wondering why it comes back.
         */
        clawt_web_add(pad, clawt_web_text(
            "This is the agent's own file. Where clawtilla manages part "
            "of one, it owns only the region between its BEGIN and END "
            "markers and rewrites that on every start -- the rest is "
            "yours.", "lede"));

        clawt_web_add(form, clawt_web_hidden("name", name));
        clawt_web_add(form, clawt_web_textarea_field(
            "Contents", "content", clawt_web_member(root, "content", ""),
            24));

        {
            g_autoptr(HtmxDiv) row = htmx_div_new();
            g_autoptr(HtmxButton) save = clawt_web_button("Save", "primary");
            g_autofree gchar *back = clawt_web_agent_url(
                agent_id, CLAWT_WEB_VIEW_AGENT);
            g_autoptr(HtmxA) cancel = htmx_a_new_with_href(back);

            htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
            htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));

            htmx_element_add_class(HTMX_ELEMENT(cancel), "btn");
            htmx_node_set_text_content(HTMX_NODE(cancel), "Back");
            htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(cancel));

            htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        }

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    html = clawt_web_page(app, agent_id, CLAWT_WEB_VIEW_AGENT, view, request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_file_save(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *name = clawt_web_form_value(request, "name");
    const gchar *content = clawt_web_form_value(request, "content");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *said = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "name", name);
    clawt_web_payload_set(payload, "content",
                          content != NULL ? content : "");

    reply = clawt_web_app_call(app, "agent.file_write",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id,
                                    CLAWT_WEB_VIEW_AGENT,
                                    clawt_web_app_last_error(app));

    said = g_strdup_printf(
        "Saved %s. An agent reads its identity files when its session "
        "starts, so restart it for this to reach the running one.",
        name != NULL ? name : "the file");

    return clawt_web_after_action(app, request, agent_id,
                                  CLAWT_WEB_VIEW_AGENT, said);
}

/* ── The fleet at a glance ───────────────────────────────────────── */

/*
 * What /agents shows: who is in the fleet, without picking one.
 */
static HtmxResponse *
on_fleet(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "agent.list", NULL);
    g_autoptr(JsonNode) teams = clawt_web_app_call(app, "team.list", NULL);
    JsonArray *agents = clawt_web_member_array(clawt_web_root(reply),
                                               "agents");
    g_autoptr(HtmxDiv) view = htmx_div_new();
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(HtmxElement) wrap = HTMX_ELEMENT(htmx_div_new());
    g_autoptr(HtmxTable) table = htmx_table_new();
    g_autoptr(HtmxThead) head = htmx_thead_new();
    g_autoptr(HtmxTbody) rows = htmx_tbody_new();
    g_autofree gchar *html = NULL;
    guint i;

    (void)params;

    htmx_element_add_class(HTMX_ELEMENT(view), "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("The fleet"));
    clawt_web_warnings(HTMX_ELEMENT(pad), teams);

    {
        g_autoptr(HtmxTr) tr = htmx_tr_new();

        clawt_web_add(tr, htmx_th_new_with_text("Agent"));
        clawt_web_add(tr, htmx_th_new_with_text("State"));
        clawt_web_add(tr, htmx_th_new_with_text("Team"));
        clawt_web_add(tr, htmx_th_new_with_text("Model"));
        clawt_web_add(tr, htmx_th_new_with_text("Computer"));
        clawt_web_add(tr, htmx_th_new_with_text("Queue"));
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(tr));
    }

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *id = clawt_web_member(agent, "id", "?");
        const gchar *state = clawt_web_member(agent, "state", "?");
        g_autoptr(HtmxTr) tr = htmx_tr_new();
        g_autoptr(HtmxTd) name_cell = htmx_td_new();
        g_autofree gchar *url = clawt_web_agent_url(id,
                                                    CLAWT_WEB_VIEW_CHAT);
        g_autofree gchar *depth = g_strdup_printf(
            "%" G_GINT64_FORMAT,
            clawt_web_member_int(agent, "mailbox_depth", 0));

        {
            g_autoptr(HtmxA) link = htmx_a_new_with_href(url);

            htmx_node_set_text_content(
                HTMX_NODE(link), clawt_web_member(agent, "name", id));
            htmx_node_add_child(HTMX_NODE(name_cell), HTMX_NODE(link));
        }

        htmx_node_add_child(HTMX_NODE(tr), HTMX_NODE(name_cell));

        {
            g_autoptr(HtmxTd) cell = htmx_td_new();

            clawt_web_add(cell, clawt_web_badge(
                state, clawt_web_state_tone(state)));
            htmx_node_add_child(HTMX_NODE(tr), HTMX_NODE(cell));
        }

        clawt_web_add(tr, htmx_td_new_with_text(
            clawt_web_member(agent, "team", "—")));
        clawt_web_add(tr, htmx_td_new_with_text(
            clawt_web_member(agent, "model", "—")));
        clawt_web_add(tr, htmx_td_new_with_text(
            clawt_web_member(agent, "computer", "none")));

        {
            g_autoptr(HtmxTd) cell = htmx_td_new_with_text(depth);

            htmx_element_add_class(HTMX_ELEMENT(cell), "num");
            htmx_node_add_child(HTMX_NODE(tr), HTMX_NODE(cell));
        }

        htmx_node_add_child(HTMX_NODE(rows), HTMX_NODE(tr));
    }

    htmx_node_add_child(HTMX_NODE(table), HTMX_NODE(head));
    htmx_node_add_child(HTMX_NODE(table), HTMX_NODE(rows));
    htmx_element_add_class(wrap, "table-wrap");
    htmx_node_add_child(HTMX_NODE(wrap), HTMX_NODE(table));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(wrap));
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    html = clawt_web_shell_page(app, "Fleet", HTMX_ELEMENT(view), request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_files_page(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autofree gchar *html = NULL;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Workspace files"));
    clawt_web_add_files_card(app, HTMX_ELEMENT(pad), agent_id);
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    html = clawt_web_page(app, agent_id, CLAWT_WEB_VIEW_AGENT, view, request);

    return clawt_web_html_response(html);
}



static HtmxResponse *
on_memories(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    const gchar *query = clawt_web_form_value(request, "q");

    /*
     * From the query string too, because /memory <words> arrives as a
     * redirect rather than as a form post.
     */
    if (query == NULL)
        query = htmx_request_get_query_param(request, "q");
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autofree gchar *html = NULL;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Memories"));
    clawt_web_add_memory_card(app, HTMX_ELEMENT(pad), agent_id, query);
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    html = clawt_web_page(app, agent_id, CLAWT_WEB_VIEW_AGENT, view, request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_integration_editor(HtmxRequest *request, GHashTable *params,
                      gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "integration");
    g_autoptr(HtmxElement) content = integration_editor(app, name);
    g_autofree gchar *html = NULL;
    g_autoptr(HtmxDiv) wrap = htmx_div_new();

    (void)request;

    htmx_element_add_class(HTMX_ELEMENT(wrap), "view");

    {
        g_autoptr(HtmxDiv) pad = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");
        clawt_web_add(pad, clawt_web_section_title(name));

        {
            g_autoptr(HtmxA) back = htmx_a_new_with_href(
                "/settings/integrations");

            htmx_element_add_class(HTMX_ELEMENT(back), "btn");
            htmx_node_set_text_content(HTMX_NODE(back), "← Integrations");
            htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(back));
        }

        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(content));
        htmx_node_add_child(HTMX_NODE(wrap), HTMX_NODE(pad));
    }

    html = clawt_web_shell_page(app, name, HTMX_ELEMENT(wrap), request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_integration_save(HtmxRequest *request, GHashTable *params,
                    gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "integration");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) list = clawt_web_app_call(app, "integration.list",
                                                  NULL);
    JsonArray *entries = clawt_web_member_array(clawt_web_root(list),
                                                "integrations");
    g_autoptr(JsonNode) reply = NULL;
    const gchar *type = "";
    g_autofree gchar *schema_prefix = NULL;
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;
    g_autofree gchar *url = NULL;

    for (i = 0; entries != NULL && i < json_array_get_length(entries); i++) {
        JsonObject *entry = json_array_get_object_element(entries, i);

        if (g_strcmp0(clawt_web_member(entry, "name", ""), name) == 0)
            type = clawt_web_member(entry, "type", "");
    }

    clawt_web_payload_set(payload, "name", name);
    clawt_web_payload_set_bool(payload, "enabled",
                               clawt_web_form_flag(request, "enabled"));

    schema_prefix = g_strdup_printf("agents.integrations.%s.", type);
    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *leaf;
        const gchar *value;

        if (!g_str_has_prefix(entry->key, schema_prefix))
            continue;

        leaf = entry->key + strlen(schema_prefix);

        if (*leaf == '\0' || strchr(leaf, '.') != NULL ||
            entry->type == CLAWT_SCHEMA_SECTION ||
            entry->type == CLAWT_SCHEMA_BOOLEAN)
            continue;

        value = clawt_web_form_value(request, leaf);

        if (value == NULL)
            continue;

        /*
         * An empty secret field means "keep what is set" rather than
         * "clear it". Clearing one by accident costs an authorization
         * nobody can see the reason for.
         */
        if (entry->type == CLAWT_SCHEMA_SECRET && *value == '\0')
            continue;

        if (entry->type == CLAWT_SCHEMA_INT) {
            clawt_web_payload_set_int(payload, leaf,
                                      g_ascii_strtoll(value, NULL, 10));
            continue;
        }

        clawt_web_payload_set(payload, leaf, value);
    }

    reply = clawt_web_app_call(app, "integration.update",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    (void)reply;

    {
        g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);

        url = g_strdup_printf("/settings/integrations/%s", escaped);
    }

    return clawt_web_redirect(request, url);
}

static HtmxResponse *
on_matrix_login(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "integration");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *url = NULL;
    g_autofree gchar *escaped = g_uri_escape_string(name, NULL, FALSE);

    clawt_web_payload_set(payload, "integration", name);
    clawt_web_payload_set(payload, "homeserver",
                          clawt_web_form_value(request, "homeserver"));
    clawt_web_payload_set(payload, "user",
                          clawt_web_form_value(request, "user"));
    clawt_web_payload_set(payload, "password",
                          clawt_web_form_value(request, "password"));

    /*
     * The reply names the file the token was written to and never the
     * token. Handing a live credential back to the client that asked for
     * the login would put it in every client's memory and in every
     * transcript of this exchange.
     */
    reply = clawt_web_app_call(app, "integration.matrix_login",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    (void)reply;

    url = g_strdup_printf("/settings/integrations/%s", escaped);

    return clawt_web_redirect(request, url);
}

static HtmxResponse *
on_matrix_rooms(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *name = clawt_web_param(params, "integration");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxDiv) wrap = htmx_div_new();
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    JsonArray *rooms;
    g_autofree gchar *html = NULL;
    g_autofree gchar *failure = NULL;
    guint i;

    clawt_web_payload_set(payload, "integration", name);

    reply = clawt_web_app_call(app, "integration.matrix_rooms",
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    /* Copied now; the app frees it on its next call. */
    if (reply == NULL)
        failure = g_strdup(clawt_web_app_last_error(app));

    htmx_element_add_class(HTMX_ELEMENT(wrap), "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");
    clawt_web_add(pad, clawt_web_section_title("Rooms"));

    card = clawt_web_card(name, "Rooms this account is in.");
    body = clawt_web_card_body(card);
    rooms = clawt_web_member_array(clawt_web_root(reply), "rooms");

    if (failure != NULL)
        clawt_web_add(body, clawt_web_notice(failure, "bad"));
    else if (rooms == NULL || json_array_get_length(rooms) == 0)
        clawt_web_add(body, clawt_web_empty("No rooms", NULL));

    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        g_autoptr(HtmxDiv) row = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
        clawt_web_add(row, clawt_web_text(
            clawt_web_member(room, "name",
                             clawt_web_member(room, "id", "?")), NULL));
        clawt_web_add(row, clawt_web_text(
            clawt_web_member(room, "id", ""), "small muted mono"));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    htmx_node_add_child(HTMX_NODE(wrap), HTMX_NODE(pad));

    html = clawt_web_shell_page(app, "Rooms", HTMX_ELEMENT(wrap), request);

    return clawt_web_html_response(html);
}

void
clawt_web_register_extras(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_post(router, "/a/:id/reorder", on_reorder, app);
    htmx_router_post(router, "/a/:id/team", on_set_team, app);
    htmx_router_post(router, "/a/:id/attach", on_attach, app);
    htmx_router_post(router, "/a/:id/attach/remove", on_attach_remove, app);
    htmx_router_post(router, "/a/:id/memories", on_memories, app);
    htmx_router_get(router, "/a/:id/memories", on_memories, app);
    htmx_router_get(router, "/a/:id/file", on_file_editor, app);
    htmx_router_post(router, "/a/:id/file", on_file_save, app);
    htmx_router_get(router, "/a/:id/files", on_files_page, app);
    htmx_router_get(router, "/fleet", on_fleet, app);

    htmx_router_get(router, "/settings/integrations/:integration",
                    on_integration_editor, app);
    htmx_router_post(router, "/settings/integrations/:integration/save",
                     on_integration_save, app);
    htmx_router_post(router,
                     "/settings/integrations/:integration/matrix-login",
                     on_matrix_login, app);
    htmx_router_post(router,
                     "/settings/integrations/:integration/matrix-rooms",
                     on_matrix_rooms, app);
    htmx_router_post(router, "/settings/connectors/:connector/key",
                     on_connector_key, app);
}
