/*
 * clawt-venture.c - A staged VENTURE write, as something to decide
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "integration/clawt-venture.h"

#include <json-glib/json-glib.h>

#include <string.h>

/* ── The record ──────────────────────────────────────────────────── */

G_DEFINE_BOXED_TYPE(ClawtVentureConfirmation, clawt_venture_confirmation,
                    clawt_venture_confirmation_copy,
                    clawt_venture_confirmation_free)

void
clawt_venture_confirmation_free(ClawtVentureConfirmation *self)
{
    if (self == NULL)
        return;

    g_free(self->id);
    g_free(self->summary);
    g_free(self->action);
    g_free(self->state);
    g_free(self->record_type);
    g_free(self->label);
    g_free(self->origin_kind);
    g_free(self->origin_name);
    g_free(self->origin_via);
    g_free(self->created_at);
    g_free(self->expires_at);
    g_free(self->diff);

    g_free(self);
}

ClawtVentureConfirmation *
clawt_venture_confirmation_copy(ClawtVentureConfirmation *self)
{
    ClawtVentureConfirmation *out;

    g_return_val_if_fail(self != NULL, NULL);

    out = g_new0(ClawtVentureConfirmation, 1);

    out->id = g_strdup(self->id);
    out->summary = g_strdup(self->summary);
    out->action = g_strdup(self->action);
    out->state = g_strdup(self->state);
    out->record_type = g_strdup(self->record_type);
    out->label = g_strdup(self->label);
    out->record_id = self->record_id;
    out->origin_kind = g_strdup(self->origin_kind);
    out->origin_name = g_strdup(self->origin_name);
    out->origin_via = g_strdup(self->origin_via);
    out->created_at = g_strdup(self->created_at);
    out->expires_at = g_strdup(self->expires_at);
    out->diff = g_strdup(self->diff);

    return out;
}

/* ── Reading the queue ───────────────────────────────────────────── */

static gchar *
member_string(JsonObject *object, const gchar *key)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, key))
        return NULL;

    node = json_object_get_member(object, key);

    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return NULL;

    if (json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return g_strdup(json_node_get_string(node));
}

/*
 * One field of the staged object, as a line somebody can read.
 *
 * venture's `diff` carries whatever the record's own fields are, and
 * some of them are objects -- an amount is `{amount, currency}`.  A
 * nested value is serialised back to compact JSON rather than flattened
 * further: a person answering "record $250 for cover art" needs to see
 * the figure, and inventing a rendering for every shape venture might
 * grow would be a second copy of its data model, which is the thing the
 * whole integration is built to avoid.
 */
static void
append_diff_line(GString *out, const gchar *name, JsonNode *value)
{
    g_autofree gchar *text = NULL;

    if (JSON_NODE_HOLDS_VALUE(value)) {
        GType type = json_node_get_value_type(value);

        if (type == G_TYPE_STRING)
            text = g_strdup(json_node_get_string(value));
        else if (type == G_TYPE_BOOLEAN)
            text = g_strdup(json_node_get_boolean(value) ? "true" : "false");
        else if (type == G_TYPE_INT64)
            text = g_strdup_printf("%" G_GINT64_FORMAT,
                                   json_node_get_int(value));
        else if (type == G_TYPE_DOUBLE)
            text = g_strdup_printf("%g", json_node_get_double(value));
    } else if (JSON_NODE_HOLDS_NULL(value)) {
        text = g_strdup("(cleared)");
    }

    if (text == NULL) {
        g_autoptr(JsonGenerator) generator = json_generator_new();

        json_generator_set_root(generator, value);
        text = json_generator_to_data(generator, NULL);
    }

    g_string_append_printf(out, "  %s: %s\n", name,
                           text != NULL ? text : "?");
}

static gchar *
render_diff(JsonObject *object)
{
    g_autoptr(GString) out = NULL;
    JsonNode *node;
    JsonObject *diff;
    g_autoptr(GList) members = NULL;
    GList *iter;

    if (object == NULL || !json_object_has_member(object, "diff"))
        return NULL;

    node = json_object_get_member(object, "diff");

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    diff = json_node_get_object(node);

    /*
     * A JsonNode can hold the object *type* and no object, so the type
     * check above is not a pointer check.
     */
    if (diff == NULL)
        return NULL;

    out = g_string_new(NULL);
    members = json_object_get_members(diff);

    for (iter = members; iter != NULL; iter = iter->next) {
        const gchar *name = iter->data;

        append_diff_line(out, name, json_object_get_member(diff, name));
    }

    if (out->len == 0)
        return NULL;

    return g_strdup(out->str);
}

static ClawtVentureConfirmation *
confirmation_from_object(JsonObject *object)
{
    ClawtVentureConfirmation *out;
    g_autofree gchar *id = member_string(object, "id");

    if (id == NULL || *id == '\0')
        return NULL;

    out = g_new0(ClawtVentureConfirmation, 1);

    out->id = g_steal_pointer(&id);
    out->summary = member_string(object, "summary");
    out->action = member_string(object, "action");
    out->state = member_string(object, "state");
    out->record_type = member_string(object, "type");
    out->label = member_string(object, "label");
    out->created_at = member_string(object, "created_at");
    out->expires_at = member_string(object, "expires_at");
    out->diff = render_diff(object);

    if (json_object_has_member(object, "record_id")) {
        JsonNode *node = json_object_get_member(object, "record_id");

        if (node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
            json_node_get_value_type(node) == G_TYPE_INT64)
            out->record_id = json_node_get_int(node);
    }

    if (json_object_has_member(object, "origin")) {
        JsonNode *node = json_object_get_member(object, "origin");

        if (node != NULL && JSON_NODE_HOLDS_OBJECT(node)) {
            JsonObject *origin = json_node_get_object(node);

            out->origin_kind = member_string(origin, "kind");
            out->origin_name = member_string(origin, "name");
            out->origin_via = member_string(origin, "via");
        }
    }

    return out;
}

GPtrArray *
clawt_venture_confirmations_parse(const gchar  *json,
                                  gssize        length,
                                  GError      **error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) parse_error = NULL;
    GPtrArray *out;
    JsonNode *root;
    JsonArray *array;
    guint i;
    guint count;

    g_return_val_if_fail(json != NULL, NULL);

    if (!json_parser_load_from_data(parser, json, length, &parse_error)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "venture answered with something that is not JSON: %s",
                    parse_error->message);
        return NULL;
    }

    root = json_parser_get_root(parser);

    /*
     * Not an array is not an empty queue.
     *
     * A proxy's error page, a login redirect and the wrong port all
     * parse as something, and reading any of them as "nothing is
     * waiting" would report a healthy inbox about a server nobody
     * reached -- which is the one answer that stops anybody looking.
     */
    if (root == NULL || !JSON_NODE_HOLDS_ARRAY(root)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "venture's confirmations endpoint answered with "
                            "something other than a list; check the URL and "
                            "that the token has the viewer role");
        return NULL;
    }

    array = json_node_get_array(root);
    out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_venture_confirmation_free);

    if (array == NULL)
        return out;

    count = json_array_get_length(array);

    for (i = 0; i < count; i++) {
        JsonNode *node = json_array_get_element(array, i);
        ClawtVentureConfirmation *entry = NULL;

        if (node != NULL && JSON_NODE_HOLDS_OBJECT(node))
            entry = confirmation_from_object(json_node_get_object(node));

        /*
         * Skipped, not fatal.  The next poll would fail identically, so
         * one unreadable card would mean an inbox that quietly stopped
         * updating -- and every other change waiting on somebody would
         * be invisible for as long as it took anybody to notice.
         */
        if (entry == NULL) {
            g_warning("venture: confirmation %u could not be read and was "
                      "skipped; the rest of the queue was kept", i);
            continue;
        }

        g_ptr_array_add(out, entry);
    }

    return out;
}

/* ── The decision ────────────────────────────────────────────────── */

gchar *
clawt_venture_decision_id(const gchar *instance,
                          const gchar *confirmation_id)
{
    gchar *id;

    g_return_val_if_fail(confirmation_id != NULL, NULL);

    id = g_strdup_printf("venture-%s-%s",
                         instance != NULL ? instance : "venture",
                         confirmation_id);

    /*
     * An instance name comes from a config file and a confirmation id
     * from another program, so neither is known to be a plain word.  The
     * id is a primary key and reaches a URL in the web client, so
     * anything that is not obviously safe is folded -- the same folding
     * clawt_connector_token_path() does, for the same reason.
     */
    g_strcanon(id,
               "abcdefghijklmnopqrstuvwxyz"
               "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_",
               '_');

    return id;
}

gboolean
clawt_venture_answer_is_approval(const gchar *answer)
{
    if (answer == NULL)
        return FALSE;

    while (*answer == ' ' || *answer == '\t')
        answer++;

    /*
     * Only the word itself, and only at the start.  "approve" and
     * "approved" are the same intention; "do not approve" is not, and
     * a substring match would read it as one.
     */
    if (g_ascii_strncasecmp(answer, CLAWT_VENTURE_APPROVE,
                            strlen(CLAWT_VENTURE_APPROVE)) == 0)
        return TRUE;

    if (g_ascii_strcasecmp(answer, "yes") == 0)
        return TRUE;

    return FALSE;
}

ClawtDecision *
clawt_venture_decision_for(ClawtVentureConfirmation *confirmation,
                           const gchar              *instance,
                           const gchar              *agent_id)
{
    static const gchar *const options[] = {
        CLAWT_VENTURE_APPROVE, CLAWT_VENTURE_REJECT, NULL
    };

    g_autofree gchar *id = NULL;
    g_autoptr(GString) question = NULL;
    ClawtDecision *decision;

    g_return_val_if_fail(confirmation != NULL, NULL);

    id = clawt_venture_decision_id(instance, confirmation->id);
    question = g_string_new(NULL);

    /*
     * venture's own summary first, because it is the sentence a person
     * can answer from -- "Create expense \"Cover art\"".  Everything
     * after it is there so the card can be answered without opening
     * venture's web interface, which is the whole point of the bridge.
     */
    g_string_append_printf(
        question, "%s wants to %s",
        confirmation->origin_name != NULL ? confirmation->origin_name
                                          : "An agent",
        confirmation->summary != NULL ? confirmation->summary
                                      : "change a VENTURE record");

    if (instance != NULL)
        g_string_append_printf(question, " (via the '%s' connector)",
                               instance);

    g_string_append_c(question, '.');

    if (confirmation->record_type != NULL) {
        g_string_append_printf(question, "\n\nRecord type: %s",
                               confirmation->record_type);

        if (confirmation->record_id != 0)
            g_string_append_printf(question, " #%" G_GINT64_FORMAT,
                                   confirmation->record_id);
    }

    if (confirmation->diff != NULL)
        g_string_append_printf(question, "\n\nWhat would change:\n%s",
                               confirmation->diff);

    if (confirmation->expires_at != NULL)
        g_string_append_printf(
            question,
            "\nVENTURE drops this card unanswered at %s.",
            confirmation->expires_at);

    decision = clawt_decision_new(id, agent_id, question->str);

    clawt_decision_set_options(decision, options);

    /*
     * Reject, and honestly so: an unanswered card is dropped by venture
     * when its own TTL runs out, so this is a description of what
     * happens rather than a policy chosen here.  Nothing is written in
     * the meantime, which is why `reversible_until` stays unset -- there
     * is nothing to reverse until somebody approves, and venture's soft
     * delete is the undo after that.
     */
    clawt_decision_set_default(
        decision, CLAWT_VENTURE_REJECT,
        "Nothing has been written. VENTURE drops a change nobody answers, "
        "so leaving this is the same as rejecting it -- and the agent can "
        "stage it again once you have said what you wanted instead.");

    return decision;
}

/* ── The endpoints ───────────────────────────────────────────────── */

/*
 * Every URL is built by joining a path onto the configured instance,
 * which is the rule the connector catalogue applies to every field it
 * has.  One spelling of the join, so a bridge that polls one server
 * cannot answer to another.
 */
static gchar *
join_path(const gchar *base, const gchar *path)
{
    g_autofree gchar *trimmed = NULL;

    if (base == NULL || *base == '\0')
        return NULL;

    trimmed = g_strdup(base);
    g_strstrip(trimmed);

    while (g_str_has_suffix(trimmed, "/"))
        trimmed[strlen(trimmed) - 1] = '\0';

    if (*trimmed == '\0')
        return NULL;

    return g_strconcat(trimmed, path, NULL);
}

gchar *
clawt_venture_confirmations_url(const gchar *base)
{
    return join_path(base, "/api/v1/confirmations");
}

gchar *
clawt_venture_answer_url(const gchar *base,
                         const gchar *confirmation_id,
                         gboolean     approve)
{
    g_autofree gchar *escaped = NULL;
    g_autofree gchar *path = NULL;

    g_return_val_if_fail(confirmation_id != NULL, NULL);

    /*
     * The id came from another program and lands in a path segment.
     * Escaped rather than trusted: an id carrying a slash would reach a
     * different route entirely, and the one thing a POST here must not
     * do is arrive somewhere nobody named.
     */
    escaped = g_uri_escape_string(confirmation_id, NULL, FALSE);
    path = g_strdup_printf("/api/v1/confirmations/%s/%s", escaped,
                           approve ? CLAWT_VENTURE_APPROVE
                                   : CLAWT_VENTURE_REJECT);

    return join_path(base, path);
}
