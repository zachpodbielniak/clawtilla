/*
 * web-triggers.c - The triggers page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Work an agent does because something happened elsewhere.  It sits
 * beside routines because a routine is a clock and a trigger is an
 * event, and both end in the same queued run against the same agent.
 *
 * A file of its own rather than a fourth view in web-work.c: the secret
 * is shown exactly once, and a page that also has to render it needs
 * enough care that it should not be sharing a file with three other
 * views' row builders.
 */

#include "clawt-web.h"
#include "web-pages.h"
#include "web-ui.h"

#include <string.h>

/*
 * A trigger's state in one word, in priority order.
 *
 * "Why is this not firing" wants one answer, not three flags a reader
 * has to combine -- and the first thing that is true is always the
 * answer, because a trigger with no secret refuses everything whatever
 * else is set.
 */
static const gchar *
trigger_state(JsonObject *trigger, const gchar **out_tone)
{
    if (!clawt_web_member_bool(trigger, "has_secret", FALSE)) {
        *out_tone = "bad";
        return "No secret, so it refuses every delivery. Rotate it.";
    }

    if (clawt_web_member_bool(trigger, "pending_verification", TRUE)) {
        *out_tone = "warn";
        return "Waiting for its first delivery. The next one that "
               "authenticates is held here for you to read rather than "
               "run.";
    }

    if (!clawt_web_member_bool(trigger, "enabled", FALSE)) {
        *out_tone = "warn";
        return "Switched off. Its address answers as though it does not "
               "exist.";
    }

    *out_tone = "good";
    return "On.";
}

/*
 * The URL a forge should be pointed at, built from what is actually
 * listening.
 *
 * Assembled here rather than asked of the operator: the endpoint and the
 * bound addresses are two things the daemon knows and a person would
 * otherwise have to join by hand, and getting it wrong produces
 * deliveries that never arrive with nothing anywhere saying why.
 */
static void
add_endpoint_rows(HtmxElement *body, JsonObject *trigger)
{
    const gchar *endpoint = clawt_web_member(trigger, "endpoint", NULL);
    JsonArray *listening = clawt_web_member_array(trigger, "listening");
    guint i;

    if (endpoint == NULL)
        return;

    if (listening == NULL || json_array_get_length(listening) == 0) {
        g_autofree gchar *path = g_strdup_printf("/hooks/%s", endpoint);

        clawt_web_add(body, clawt_web_row("Path", path));
        clawt_web_add(body, clawt_web_notice(
            "Nothing is listening. Set daemon.webhook_enabled and restart "
            "the daemon, or no delivery can arrive.", "warn"));
        return;
    }

    for (i = 0; i < json_array_get_length(listening); i++) {
        g_autofree gchar *url = g_strdup_printf(
            "%s/hooks/%s", json_array_get_string_element(listening, i),
            endpoint);

        clawt_web_add(body, clawt_web_row(i == 0 ? "URL" : "also", url));
    }
}

static void
add_trigger_form(HtmxElement *parent, JsonObject *existing)
{
    const gchar *id = clawt_web_member(existing, "id", NULL);
    g_autofree gchar *action = NULL;
    g_autofree gchar *escaped = NULL;
    g_autoptr(HtmxForm) form = NULL;
    g_autoptr(HtmxDiv) card = NULL;
    HtmxElement *body;
    g_autofree const gchar **provider_values = NULL;
    g_autofree const gchar **provider_labels = NULL;
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;

    if (id != NULL) {
        escaped = g_uri_escape_string(id, NULL, FALSE);
        action = g_strdup_printf("/triggers/%s/save", escaped);
    } else {
        action = g_strdup("/triggers/add");
    }

    card = clawt_web_card(
        id != NULL ? id : "New trigger",
        id != NULL ? NULL
                   : "Work that happens because somebody else's server "
                     "called, rather than because you asked.");
    body = clawt_web_card_body(card);

    if (id != NULL) {
        const gchar *tone = "good";
        const gchar *state = trigger_state(existing, &tone);

        clawt_web_add(body, clawt_web_notice(state, tone));
        add_endpoint_rows(body, existing);
    }

    form = clawt_web_form(action);

    if (id == NULL)
        clawt_web_add(form, clawt_web_field("Id", "id", NULL,
                                            "ci-failed"));

    /*
     * The providers come from the library's own enumeration, never a
     * list written here. A list in this client and a list in
     * clawt-enums.c would drift, and the drift is a provider the page
     * offers that the daemon does not understand -- which reads as a
     * trigger that refuses every delivery.
     */
    provider_values = g_new0(const gchar *,
                             clawt_trigger_provider_count() + 1);
    provider_labels = g_new0(const gchar *,
                             clawt_trigger_provider_count() + 1);

    for (i = 0; i < clawt_trigger_provider_count(); i++) {
        provider_values[i] = clawt_trigger_provider_nth_nick((guint)i);
        provider_labels[i] = clawt_trigger_provider_nth_label((guint)i);
    }

    /*
     * The fields come from the schema, the same table `trigger.add`
     * walks on the other side. Two lists of a trigger's keys would
     * differ the first time one was added, and the symptom is a field
     * that is accepted, reported as saved, and never read.
     */
    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *leaf;
        const gchar *value;

        if (!g_str_has_prefix(entry->key, "triggers."))
            continue;

        leaf = entry->key + strlen("triggers.");

        if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
            continue;

        if (entry->type == CLAWT_SCHEMA_SECTION ||
            entry->type == CLAWT_SCHEMA_MAPPING ||
            entry->type == CLAWT_SCHEMA_LIST_OF)
            continue;

        /*
         * A secret is never a form field. It is generated by the daemon
         * and shown once; a field that could send one would put it in a
         * POST body, a browser's autofill and quite possibly a log.
         */
        if (entry->type == CLAWT_SCHEMA_SECRET)
            continue;

        value = clawt_web_member(existing, leaf, NULL);

        if (g_strcmp0(leaf, "provider") == 0) {
            clawt_web_add(form, clawt_web_select_field(
                "Sent by", leaf, provider_values, provider_labels,
                value != NULL ? value : "generic"));
            continue;
        }

        if (entry->type == CLAWT_SCHEMA_BOOLEAN) {
            clawt_web_add(form, clawt_web_switch_field(
                leaf, leaf, entry->doc, g_strcmp0(value, "true") == 0));
            continue;
        }

        if (entry->type == CLAWT_SCHEMA_STRING_LIST) {
            JsonArray *array = clawt_web_member_array(existing, leaf);
            g_autoptr(GString) joined = g_string_new(NULL);
            guint e;

            for (e = 0; array != NULL && e < json_array_get_length(array);
                 e++) {
                if (joined->len > 0)
                    g_string_append(joined, ", ");

                g_string_append(joined,
                                json_array_get_string_element(array, e));
            }

            clawt_web_add(form, clawt_web_field(leaf, leaf, joined->str,
                                                entry->doc));
            continue;
        }

        if (g_strcmp0(leaf, "instructions") == 0) {
            clawt_web_add(form, clawt_web_textarea_field(
                "Instructions", leaf, value, 6));
            continue;
        }

        clawt_web_add(form, clawt_web_field(leaf, leaf, value, entry->doc));
    }

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) save = clawt_web_button(
            id != NULL ? "Save" : "Add trigger", "primary");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));

        if (id != NULL) {
            g_autofree gchar *test = g_strdup_printf("/triggers/%s/test",
                                                     escaped);
            g_autofree gchar *capture =
                g_strdup_printf("/triggers/%s/capture", escaped);
            g_autofree gchar *rotate = g_strdup_printf("/triggers/%s/rotate",
                                                       escaped);
            g_autofree gchar *remove = g_strdup_printf("/triggers/%s/remove",
                                                       escaped);

            clawt_web_add(row, clawt_web_post_button("Preview prompt", test,
                                                     "default", NULL));
            clawt_web_add(row, clawt_web_post_button("First delivery",
                                                     capture, "default",
                                                     NULL));
            clawt_web_add(row, clawt_web_post_button(
                "Rotate secret", rotate, "danger",
                "Rotate? The old secret and address stop working at once, "
                "and the webhook needs both new ones."));
            clawt_web_add(row, clawt_web_post_button(
                "Remove", remove, "danger", "Remove this trigger?"));
        }

        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/*
 * The receipts, which are how "nothing happened" is answerable.
 *
 * Four things produce no run -- the endpoint was wrong, the secret was
 * wrong, the event was filtered out, or the run failed -- and without
 * this they are indistinguishable from each other and from a forge that
 * never called.
 */
static void
add_deliveries(ClawtWebApp *app, HtmxElement *pad)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "trigger.deliveries",
                                                   NULL);
    JsonArray *deliveries;
    g_autoptr(HtmxDiv) card = clawt_web_card("Recent deliveries", NULL);
    HtmxElement *body = clawt_web_card_body(card);
    guint i;

    deliveries = clawt_web_member_array(clawt_web_root(reply), "deliveries");

    if (deliveries == NULL || json_array_get_length(deliveries) == 0) {
        clawt_web_add(body, clawt_web_empty(
            "Nothing yet",
            clawt_web_member(clawt_web_root(reply), "note", NULL)));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
        return;
    }

    for (i = 0; i < json_array_get_length(deliveries); i++) {
        JsonObject *row = json_array_get_object_element(deliveries, i);
        const gchar *detail = clawt_web_member(row, "detail", NULL);
        g_autofree gchar *value = g_strdup_printf(
            "%s \342\200\224 %s%s%s",
            clawt_web_member(row, "event", "-"),
            clawt_web_member(row, "outcome", "?"),
            detail != NULL ? ": " : "", detail != NULL ? detail : "");

        clawt_web_add(body, clawt_web_row(clawt_web_member(row, "trigger",
                                                           "?"),
                                          value));
    }

    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
}

HtmxElement *
clawt_web_triggers_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "trigger.list", NULL);
    JsonObject *root = clawt_web_root(reply);
    JsonArray *triggers;
    guint i;

    /*
     * Discarded, as the routines page discards it: triggers are
     * fleet-wide, and the agent is only here so the sidebar and the
     * topbar keep their selection.
     */
    (void)agent_id;

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Triggers"));
    clawt_web_add(pad, clawt_web_text(
        "A routine is a clock; a trigger is an event. The receiver is its "
        "own server on its own port and serves nothing but /health and "
        "the secret /hooks paths, so putting it behind a tunnel exposes "
        "no other part of the machine.", "lede"));

    if (root != NULL && !clawt_web_member_bool(root, "receiving", FALSE))
        clawt_web_add(pad, clawt_web_notice(
            "The receiver is not running, so nothing can arrive. Set "
            "daemon.webhook_enabled.", "warn"));

    triggers = clawt_web_member_array(root, "triggers");

    if (triggers == NULL || json_array_get_length(triggers) == 0)
        clawt_web_add(pad, clawt_web_empty("No triggers", NULL));

    for (i = 0; triggers != NULL && i < json_array_get_length(triggers); i++)
        add_trigger_form(HTMX_ELEMENT(pad),
                         json_array_get_object_element(triggers, i));

    add_trigger_form(HTMX_ELEMENT(pad), NULL);
    add_deliveries(app, HTMX_ELEMENT(pad));

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Handlers ────────────────────────────────────────────────────── */

static HtmxResponse *
triggers_page(ClawtWebApp *app, HtmxRequest *request, const gchar *toast,
              gboolean failed)
{
    g_autofree gchar *first = clawt_web_first_agent(app);

    if (failed)
        return clawt_web_error_page(app, request, first,
                                    CLAWT_WEB_VIEW_TRIGGERS, toast);

    return clawt_web_after_action(app, request, first,
                                  CLAWT_WEB_VIEW_TRIGGERS, toast);
}

/*
 * Reads the posted form, dispatching on what the schema says each key
 * is.
 *
 * A list written as a scalar is accepted, echoed back, saved, and read
 * as the default -- and the default for `events` is empty, which means
 * *every* event. So the failure would not be an ignored filter; it would
 * be the narrowest instruction somebody can give turning into the
 * widest.
 */
static JsonNode *
trigger_payload(HtmxRequest *request, const gchar *id)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;

    clawt_web_payload_set(payload, "id", id);

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *leaf;
        const gchar *value;

        if (!g_str_has_prefix(entry->key, "triggers."))
            continue;

        leaf = entry->key + strlen("triggers.");

        if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
            continue;

        /* Never posted, never accepted. See add_trigger_form(). */
        if (entry->type == CLAWT_SCHEMA_SECRET)
            continue;

        if (entry->type == CLAWT_SCHEMA_BOOLEAN) {
            if (clawt_web_form_had(request, leaf))
                clawt_web_payload_set_bool(payload, leaf,
                                           clawt_web_form_flag(request,
                                                               leaf));
            continue;
        }

        value = clawt_web_form_value(request, leaf);

        if (value == NULL)
            continue;

        if (entry->type == CLAWT_SCHEMA_INT) {
            clawt_web_payload_set_int(payload, leaf,
                                      g_ascii_strtoll(value, NULL, 10));
            continue;
        }

        /*
         * A list goes over as the comma-separated string the field
         * holds; the daemon splits it, because it is the side that
         * knows the key is a list.
         */
        clawt_web_payload_set(payload, leaf, value);
    }

    return clawt_web_payload_take(g_steal_pointer(&payload));
}

static HtmxResponse *
on_trigger_add(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = clawt_web_form_value(request, "id");
    g_autoptr(JsonNode) reply = NULL;

    (void)params;

    if (id == NULL || *id == '\0')
        return triggers_page(app, request, "A trigger needs a name.", TRUE);

    reply = clawt_web_app_call(app, "trigger.add",
                               trigger_payload(request, id));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    /*
     * The one and only time the secret is rendered.
     *
     * It is put in the banner rather than a card, because the banner is
     * what somebody is already looking at after a submit -- and because
     * a card would be re-rendered on the next refresh, at which point
     * there would be no secret to put in it and the page would silently
     * lose the value somebody had not copied yet.
     */
    {
        JsonObject *root = clawt_web_root(reply);
        const gchar *secret = clawt_web_member(root, "secret", NULL);
        const gchar *endpoint = clawt_web_member(root, "endpoint", NULL);
        g_autofree gchar *told = NULL;

        /*
         * The sentence is printed because the daemon said the value is
         * shown once, not because this client assumes so -- the same
         * reading the GTK client takes, for the same reason.
         */
        told = g_strdup_printf(
            "Added, switched off. Path /hooks/%s, secret %s.%s The first "
            "delivery that authenticates is held for you to read.",
            endpoint != NULL ? endpoint : "?",
            secret != NULL ? secret : "?",
            clawt_web_member_bool(root, "secret_shown_once", FALSE)
                ? " This is the only time the secret is shown; if you"
                  " lose it, rotate."
                : "");

        return triggers_page(app, request, told, FALSE);
    }
}

static HtmxResponse *
on_trigger_save(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_web_app_call(app, "trigger.update",
                               trigger_payload(request, id));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    return triggers_page(app, request, "Saved.", FALSE);
}

static HtmxResponse *
on_trigger_rotate(HtmxRequest *request, GHashTable *params,
                  gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "id", id);
    reply = clawt_web_app_call(app, "trigger.rotate",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    {
        JsonObject *root = clawt_web_root(reply);
        g_autofree gchar *told = g_strdup_printf(
            "Rotated. Path /hooks/%s, secret %s. The old secret and the "
            "old address stopped working just now, so the webhook needs "
            "both of these.%s",
            clawt_web_member(root, "endpoint", "?"),
            clawt_web_member(root, "secret", "?"),
            clawt_web_member_bool(root, "secret_shown_once", FALSE)
                ? " This is the only time the secret is shown."
                : "");

        return triggers_page(app, request, told, FALSE);
    }
}

static HtmxResponse *
on_trigger_remove(HtmxRequest *request, GHashTable *params,
                  gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "id", id);
    reply = clawt_web_app_call(app, "trigger.remove",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    return triggers_page(app, request, "Removed.", FALSE);
}

/*
 * Shows what the agent would be asked, without asking it.
 *
 * The banner carries the whole expanded prompt, fence and all: seeing
 * the untrusted-payload block is most of the point, because it is the
 * thing that stops a webhook body reading as an instruction.
 */
static HtmxResponse *
on_trigger_test(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "id", id);
    reply = clawt_web_app_call(app, "trigger.test",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    return triggers_page(app, request,
                         clawt_web_member(clawt_web_root(reply), "prompt",
                                          ""),
                         FALSE);
}

static HtmxResponse *
on_trigger_capture(HtmxRequest *request, GHashTable *params,
                   gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *id = clawt_web_param(params, "trigger");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *captured;

    clawt_web_payload_set(payload, "id", id);
    reply = clawt_web_app_call(app, "trigger.capture",
                               clawt_web_payload_take(
                                   g_steal_pointer(&payload)));

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        return triggers_page(app, request, why, TRUE);
    }

    captured = clawt_web_member(clawt_web_root(reply), "payload", NULL);

    if (captured == NULL)
        return triggers_page(app, request,
                             "Nothing has arrived yet. Send it a test "
                             "delivery from the forge.", FALSE);

    return triggers_page(app, request, captured, FALSE);
}

void
clawt_web_register_triggers(HtmxRouter *router, ClawtWebApp *app)
{
    /*
     * Under /triggers, never under /a/:id -- and registered before
     * clawt_web_register_views() in any case, since "/a/:id/:view"
     * matches everything below an agent and renders the chat page with
     * a 200 for anything after it.
     */
    htmx_router_post(router, "/triggers/add", on_trigger_add, app);
    htmx_router_post(router, "/triggers/:trigger/save", on_trigger_save, app);
    htmx_router_post(router, "/triggers/:trigger/rotate", on_trigger_rotate,
                     app);
    htmx_router_post(router, "/triggers/:trigger/remove", on_trigger_remove,
                     app);
    htmx_router_post(router, "/triggers/:trigger/test", on_trigger_test, app);
    htmx_router_post(router, "/triggers/:trigger/capture", on_trigger_capture,
                     app);
}
