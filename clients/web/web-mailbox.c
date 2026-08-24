/*
 * web-mailbox.c - The queue, and what fell out of it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "web-pages.h"

static const gchar *
priority_tone(const gchar *priority)
{
    if (g_strcmp0(priority, "urgent") == 0)
        return "bad";
    if (g_strcmp0(priority, "high") == 0)
        return "warn";
    if (g_strcmp0(priority, "low") == 0)
        return "neutral";

    return "info";
}

static HtmxElement *
item_row(JsonObject *item, const gchar *agent_id, gboolean dead)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxDiv) head = htmx_div_new();
    const gchar *id = clawt_web_member(item, "id", "?");
    const gchar *from = clawt_web_member(item, "from", "?");
    const gchar *state = clawt_web_member(item, "state", "pending");
    const gchar *priority = clawt_web_member(item, "priority", "normal");
    const gchar *last_error = clawt_web_member(item, "last_error", NULL);
    gint64 attempts = clawt_web_member_int(item, "attempts", 0);
    g_autofree gchar *when =
        clawt_web_relative_time(clawt_web_member_int(item, "created_at", 0));
    g_autofree gchar *body = clawt_web_one_line(
        clawt_web_member(item, "body", ""), 220);
    g_autofree gchar *escaped_agent = g_uri_escape_string(agent_id, NULL,
                                                          FALSE);
    g_autofree gchar *escaped_id = g_uri_escape_string(id, NULL, FALSE);

    htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
    htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

    {
        g_autoptr(HtmxSpan) who = htmx_span_new();
        g_autofree gchar *text = g_strdup_printf("from %s", from);

        htmx_element_add_class(HTMX_ELEMENT(who), "list-item-title");
        htmx_node_set_text_content(HTMX_NODE(who), text);
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(who));
    }

    clawt_web_add(head, clawt_web_badge(priority, priority_tone(priority)));
    clawt_web_add(head, clawt_web_badge(state, dead ? "bad" : "neutral"));

    if (attempts > 0) {
        g_autofree gchar *text =
            g_strdup_printf("%" G_GINT64_FORMAT " attempts", attempts);

        clawt_web_add(head, clawt_web_badge(text, "warn"));
    }

    if (*when != '\0') {
        g_autoptr(HtmxSpan) stamp = htmx_span_new();

        htmx_element_add_class(HTMX_ELEMENT(stamp), "muted small");
        htmx_node_set_text_content(HTMX_NODE(stamp), when);
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(stamp));
    }

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

    clawt_web_add(row, clawt_web_text(body, "list-item-sub"));

    /*
     * Why it failed, verbatim. A dead letter with no reason on it sends
     * somebody to the daemon log to find the one line that would have
     * fitted here.
     */
    if (last_error != NULL)
        clawt_web_add(row, clawt_web_notice(last_error, "bad"));

    {
        g_autoptr(HtmxDiv) actions = htmx_div_new();
        g_autofree gchar *ack = g_strdup_printf("/a/%s/mailbox/%s/ack",
                                                escaped_agent, escaped_id);
        g_autofree gchar *requeue = g_strdup_printf(
            "/a/%s/mailbox/%s/requeue", escaped_agent, escaped_id);

        htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");

        if (dead)
            clawt_web_add(actions, clawt_web_post_button(
                "Requeue", requeue, "default", NULL));

        clawt_web_add(actions, clawt_web_post_button(
            "Acknowledge", ack, "default",
            "Acknowledge this item? It will not be delivered."));

        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));
    }

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

static void
add_list(ClawtWebApp *app, HtmxElement *parent, const gchar *agent_id,
         const gchar *kind, const gchar *title, const gchar *summary,
         gboolean dead)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(HtmxDiv) card = clawt_web_card(title, summary);
    HtmxElement *body = clawt_web_card_body(card);
    JsonArray *items;
    guint i;

    clawt_web_payload_set(payload, "agent", agent_id);

    reply = clawt_web_app_call(app, kind,
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    items = clawt_web_member_array(clawt_web_root(reply), "items");

    if (items == NULL || json_array_get_length(items) == 0) {
        clawt_web_add(body, clawt_web_empty(
            dead ? "No dead letters" : "The queue is empty",
            dead
            ? "Nothing has run out of attempts."
            /*
             * Said outright, because an empty queue for a *running*
             * agent means nothing at all: delivery acknowledges an item
             * the moment it reaches the socket and hands it over as an
             * ordinary turn, so the mailbox only ever holds what queued
             * while the agent was stopped.
             */
            : "A running agent's mailbox is almost always empty -- "
              "delivery hands each item straight over as a turn. What "
              "waits here is what arrived while the agent was stopped."));
    }

    {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");

        for (i = 0; items != NULL && i < json_array_get_length(items); i++)
            clawt_web_add(list, item_row(
                json_array_get_object_element(items, i), agent_id, dead));

        if (items != NULL && json_array_get_length(items) > 0)
            htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(list));
    }

    htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

HtmxElement *
clawt_web_mailbox_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();

    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    if (agent_id == NULL) {
        clawt_web_add(pad, clawt_web_empty("No agent selected", NULL));
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

        return g_steal_pointer(&view);
    }

    clawt_web_add(pad, clawt_web_section_title("Mailbox"));
    clawt_web_add(pad, clawt_web_text(
        "A durable queue. Messaging a stopped agent puts the message here, "
        "and starting it delivers the backlog in order.", "lede"));

    add_list(app, HTMX_ELEMENT(pad), agent_id, "mailbox.list",
             "Waiting", NULL, FALSE);
    add_list(app, HTMX_ELEMENT(pad), agent_id, "mailbox.dead",
             "Dead letters",
             "Items that ran out of attempts. Nothing is dropped silently.",
             TRUE);

    {
        g_autoptr(HtmxDiv) card = clawt_web_card(
            "Expired items",
            "Anything past its time-to-live, across the whole fleet.");
        HtmxElement *body = clawt_web_card_body(card);
        g_autoptr(HtmxDiv) row = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        clawt_web_add(row, clawt_web_post_button(
            "Purge expired", "/mailbox/purge", "default",
            "Remove every expired item from every mailbox?"));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(card));
    }

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Routes ──────────────────────────────────────────────────────── */

typedef struct {
    ClawtWebApp *app;
    const gchar *kind;
    const gchar *done;
} ItemAction;

static HtmxResponse *
on_item_action(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ItemAction *action = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "id");
    g_autofree gchar *item_id = clawt_web_param(params, "item");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    clawt_web_payload_set(payload, "item", item_id);

    reply = clawt_web_app_call(action->app, action->kind,
                               clawt_web_payload_take(g_steal_pointer(&payload)));

    if (reply == NULL)
        return clawt_web_error_page(action->app, request, agent_id,
                                    CLAWT_WEB_VIEW_MAILBOX,
                                    clawt_web_app_last_error(action->app));

    return clawt_web_after_action(action->app, request, agent_id,
                                  CLAWT_WEB_VIEW_MAILBOX, action->done);
}

static HtmxResponse *
on_purge(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "mailbox.purge", NULL);
    const gchar *from = htmx_request_get_query_param(request, "agent");
    g_autofree gchar *said = NULL;

    (void)params;

    if (reply == NULL)
        return clawt_web_error_page(app, request, from,
                                    CLAWT_WEB_VIEW_MAILBOX,
                                    clawt_web_app_last_error(app));

    said = g_strdup_printf("Purged %" G_GINT64_FORMAT " expired item(s).",
                           clawt_web_member_int(clawt_web_root(reply),
                                                "purged", 0));

    if (from == NULL) {
        g_autofree gchar *first = clawt_web_first_agent(app);

        return clawt_web_after_action(app, request, first,
                                      CLAWT_WEB_VIEW_MAILBOX, said);
    }

    return clawt_web_after_action(app, request, from,
                                  CLAWT_WEB_VIEW_MAILBOX, said);
}

static ItemAction *
item_action_new(ClawtWebApp *app, const gchar *kind, const gchar *done)
{
    ItemAction *action = g_new0(ItemAction, 1);

    action->app = app;
    action->kind = kind;
    action->done = done;

    return action;
}

void
clawt_web_register_mailbox(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_post(router, "/a/:id/mailbox/:item/ack", on_item_action,
                     item_action_new(app, "mailbox.ack", "Acknowledged."));
    htmx_router_post(router, "/a/:id/mailbox/:item/requeue", on_item_action,
                     item_action_new(app, "mailbox.requeue",
                                     "Back in the queue, attempts reset."));
    htmx_router_post(router, "/mailbox/purge", on_purge, app);
}
