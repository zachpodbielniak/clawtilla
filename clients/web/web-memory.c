/*
 * web-memory.c - Fleet-wide recall, and the model of the operator
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two things that are not about one agent, which is why they are not on
 * the agent page.
 *
 * Recall searches what was actually said, across every room and every
 * session -- a different question from "what does this agent remember",
 * answered out of a different database.  The operator profile is what
 * the fleet believes about the person running it: editable here, with
 * the half agents wrote shown separately and dated, because a model of
 * somebody they cannot read is not something to build.
 */

#include "clawt-web.h"
#include "web-pages.h"
#include "web-ui.h"

#include <string.h>

/* ── Recall ──────────────────────────────────────────────────────── */

static HtmxElement *
recall_row(JsonObject *hit)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxDiv) head = htmx_div_new();
    g_autoptr(GDateTime) when = NULL;
    const gchar *from = clawt_web_member(hit, "from_name", NULL);
    gint64 at = clawt_web_member_int(hit, "at", 0);

    if (from == NULL || *from == '\0')
        from = clawt_web_member(hit, "from", "?");

    htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
    htmx_element_add_class(HTMX_ELEMENT(row), "recall-hit");
    htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

    clawt_web_add(head, clawt_web_badge(from, "neutral"));
    clawt_web_add(head, clawt_web_badge(clawt_web_member(hit, "room", "?"),
                                        "info"));

    if (at > 0)
        when = g_date_time_new_from_unix_local(at);

    /*
     * The clock and the date, not a relative time.  A recall result is a
     * record being matched against something else -- an event line, a
     * task, a journal entry -- and "2m ago" rendered on the server is
     * wrong the moment it is sent.  Relative times belong to the lists
     * that are about recency.
     */
    if (when != NULL) {
        g_autofree gchar *day = g_date_time_format(when, "%Y-%m-%d");
        g_autofree gchar *stamp = clawt_chat_time_label(when);
        g_autofree gchar *label = g_strdup_printf("%s %s", day, stamp);

        clawt_web_add(head, clawt_web_badge(label, "neutral"));
    }

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));
    clawt_web_add(row, clawt_web_text(clawt_web_member(hit, "body", ""),
                                      "list-item-sub"));

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

static void
add_recall_card(ClawtWebApp *app, HtmxElement *parent, const gchar *query)
{
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "Recall",
        "Everything the fleet has said, across every room and every "
        "session. This is the transcript itself, not what an agent chose "
        "to remember.");
    HtmxElement *body = clawt_web_card_body(card);
    g_autoptr(HtmxForm) form = clawt_web_form("/memory");
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *hits = NULL;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(form), "recall-form");
    clawt_web_add(form, clawt_web_field("Search", "q", query,
                                        "what was said"));

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) go = clawt_web_button("Recall", "default");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(go), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(go));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));

    if (query == NULL || *query == '\0') {
        clawt_web_add(body, clawt_web_empty(
            "Nothing searched yet",
            "A query is matched as a phrase, so quotes, brackets and NOT "
            "are looked for rather than interpreted."));

        htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
        return;
    }

    {
        ClawtWebPayload *payload = clawt_web_payload_new();

        clawt_web_payload_set(payload, "query", query);
        reply = clawt_web_app_call(app, "memory.recall",
                                   clawt_web_payload_take(payload));
    }

    if (reply == NULL) {
        /*
         * Copied at the point of failure: rendering makes calls of its
         * own and each frees the app's last error, so a message read
         * afterwards is whatever now lives at that address.
         */
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        clawt_web_add(body, clawt_web_notice(
            why != NULL ? why : "the daemon did not answer", "bad"));

        htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
        return;
    }

    hits = clawt_web_member_array(clawt_web_root(reply), "hits");

    if (hits == NULL || json_array_get_length(hits) == 0) {
        /*
         * Why it is empty.  An index with nothing in it and a query that
         * matched nothing look identical, and they need different next
         * steps -- which is the same failure the FTS5 quoting exists to
         * prevent one layer down.
         */
        gint64 indexed = clawt_web_member_int(clawt_web_root(reply),
                                              "indexed", 0);

        clawt_web_add(body, clawt_web_empty(
            indexed > 0 ? "Nothing matched" : "Nothing has been said yet",
            indexed > 0
            ? "The whole query is matched as one phrase."
            : "The index fills as the fleet talks."));
    }

    if (!clawt_web_member_bool(clawt_web_root(reply), "full_text", TRUE))
        clawt_web_add(body, clawt_web_notice(
            "This sqlite has no FTS5, so recall is a substring match "
            "rather than a ranked search.", "warn"));

    for (i = 0; hits != NULL && i < json_array_get_length(hits); i++)
        clawt_web_add(body,
                      recall_row(json_array_get_object_element(hits, i)));

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* ── The operator profile ────────────────────────────────────────── */

static void
add_operator_card(ClawtWebApp *app, HtmxElement *parent)
{
    g_autoptr(HtmxDiv) card = clawt_web_card(
        "About you",
        "What every agent is told about the person it works for. The "
        "first half is yours to write; the second is what agents "
        "recorded, and you can delete any of it.");
    HtmxElement *body = clawt_web_card_body(card);
    g_autoptr(HtmxForm) form = clawt_web_form("/memory/operator");
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *payload;
    JsonArray *learned;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(form), "operator-profile");

    reply = clawt_web_app_call(app, "operator.get", NULL);

    if (reply == NULL) {
        g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

        clawt_web_add(body, clawt_web_notice(
            why != NULL ? why : "the daemon did not answer", "bad"));

        htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
        return;
    }

    payload = clawt_web_root(reply);

    /*
     * A notice rather than a hidden form: somebody typing a profile that
     * no agent will ever be told is worse off than somebody looking at
     * a page that says why.
     */
    if (!clawt_web_member_bool(payload, "enabled", FALSE))
        clawt_web_add(body, clawt_web_notice(
            "memories.operator_profile is off, so no agent is told any of "
            "this.", "warn"));

    clawt_web_add(form, clawt_web_textarea_field(
        "Profile", "text", clawt_web_member(payload, "text", ""), 10));

    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) save = clawt_web_button("Save", "default");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));

    clawt_web_add(body, clawt_web_text(
        clawt_web_member(payload, "path", ""), "small muted mono"));

    learned = clawt_web_member_array(payload, "learned");

    if (learned == NULL || json_array_get_length(learned) == 0) {
        clawt_web_add(body, clawt_web_empty(
            "Nothing recorded yet",
            "Agents add to this by writing fleet-scope memories in the "
            "'operator' category."));

        htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
        return;
    }

    for (i = 0; i < json_array_get_length(learned); i++) {
        JsonObject *memory = json_array_get_object_element(learned, i);
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxDiv) head = htmx_div_new();
        g_autoptr(GDateTime) when = NULL;
        gint64 at = clawt_web_member_int(memory, "created_at", 0);

        htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
        htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

        clawt_web_add(head, clawt_web_badge(
            clawt_web_member(memory, "source", "unattributed"), "neutral"));

        /*
         * Dated, because an inference the fleet drew six months ago and
         * one it drew this morning are not the same claim about a
         * person -- and this is the one list where that matters most.
         */
        if (at > 0)
            when = g_date_time_new_from_unix_local(at);

        if (when != NULL) {
            g_autofree gchar *day = g_date_time_format(when, "%Y-%m-%d");

            clawt_web_add(head, clawt_web_badge(day, "info"));
        }

        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));
        clawt_web_add(row, clawt_web_text(
            clawt_web_member(memory, "content", ""), "list-item-sub"));

        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* ── Routes ──────────────────────────────────────────────────────── */

static HtmxResponse *
on_memory(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *query = htmx_request_get_query_param(request, "q");
    g_autoptr(HtmxDiv) view = htmx_div_new();
    g_autofree gchar *html = NULL;

    (void)params;

    add_recall_card(app, HTMX_ELEMENT(view), query);
    add_operator_card(app, HTMX_ELEMENT(view));

    html = clawt_web_page(app, NULL, CLAWT_WEB_VIEW_CHAT,
                          HTMX_ELEMENT(view), request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_memory_search(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    const gchar *query = clawt_web_form_value(request, "q");
    g_autofree gchar *target = NULL;
    g_autofree gchar *escaped = NULL;

    (void)params;
    (void)user_data;

    if (query == NULL || *query == '\0')
        return clawt_web_redirect(request, "/memory");

    /*
     * Escaped, and into the query string rather than the path: a recall
     * query is prose and will contain spaces, slashes and quotes.
     */
    escaped = g_uri_escape_string(query, NULL, FALSE);
    target = g_strdup_printf("/memory?q=%s", escaped);

    return clawt_web_redirect(request, target);
}

static HtmxResponse *
on_operator_set(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *text = clawt_web_form_value(request, "text");

    (void)params;

    if (text != NULL) {
        ClawtWebPayload *payload = clawt_web_payload_new();
        g_autoptr(JsonNode) reply = NULL;

        clawt_web_payload_set(payload, "text", text);
        reply = clawt_web_app_call(app, "operator.set",
                                   clawt_web_payload_take(payload));
    }

    return clawt_web_redirect(request, "/memory");
}

void
clawt_web_register_memory(HtmxRouter *router, ClawtWebApp *app)
{
    /*
     * Registered before `/a/:id/:view`, like everything else that is not
     * under an agent.  That route matches every path below an agent and
     * renders the chat page with a 200, so anything registered after it
     * comes back as HTML nobody asked for.
     */
    htmx_router_get(router, "/memory", on_memory, app);
    htmx_router_post(router, "/memory", on_memory_search, app);
    htmx_router_post(router, "/memory/operator", on_operator_set, app);
}
