/*
 * web-alerts.c - The notifications page: what wants you, and what happened
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The GTK client puts this in a collapsible panel on the right.  Here it
 * is a page, which is the same capability in the idiom this client has:
 * a panel that pushes the content aside needs a third column and a
 * breakpoint, and a page needs neither.
 *
 * Like every client's, it is a view of the daemon's notification list
 * (`notice.list`, docs/clients.org "Notifications"): conversations with
 * something unread, decisions waiting, Grok jobs that need somebody, and
 * the alerts the daemon recorded from its own events.  The sections are
 * drawn by clawt_web_notices_sections() in web-ui.c, which is pure, so
 * tests/test-web-render.c holds them; this file is the page around them
 * and the routes its buttons post to.  Nothing is kept here: a read, a
 * dismissal or a mute is the daemon's, and shows in every client.
 */

#include "clawt-web.h"
#include "web-pages.h"
#include "web-ui.h"

#include <string.h>

/* The page, optionally narrowed to one agent. */
static HtmxElement *
alerts_body(ClawtWebApp *app, const gchar *agent)
{
    g_autoptr(HtmxDiv) view = htmx_div_new();
    g_autoptr(HtmxDiv) head = htmx_div_new();
    JsonObject *reply = clawt_web_app_notices(app);
    guint shown = 0;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(view), "view-pad");

    clawt_web_add(view, clawt_web_section_title("Notifications"));

    {
        g_autoptr(HtmxP) lede = htmx_p_new();

        htmx_element_add_class(HTMX_ELEMENT(lede), "muted");
        htmx_node_set_text_content(HTMX_NODE(lede),
            "What wants you, and what happened while you were looking "
            "somewhere else. The daemon keeps this list, so reading, "
            "dismissing or muting here does it in every client. "
            "Right-click a row for what it offers.");
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(lede));
    }

    /*
     * Two verbs for the whole list, as every client has them.  "Mark all
     * read" reads the alerts and every conversation; "Clear" dismisses
     * the alerts -- only the narrowed agent's when the page is narrowed.
     */
    htmx_element_add_class(HTMX_ELEMENT(head), "btn-row");
    clawt_web_add(head, clawt_web_post_button("Mark all read",
                                              "/alerts/read-all", "default",
                                              NULL));

    {
        g_autofree gchar *escaped = (agent != NULL)
            ? g_uri_escape_string(agent, NULL, FALSE) : NULL;
        g_autofree gchar *clear = (agent != NULL)
            ? g_strdup_printf("/alerts/clear?agent=%s", escaped)
            : g_strdup("/alerts/clear");

        clawt_web_add(head, clawt_web_post_button("Clear", clear, "default",
                                                  NULL));
    }

    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(head));

    if (agent != NULL) {
        g_autoptr(HtmxA) chip = htmx_a_new_with_href("/alerts");
        g_autofree gchar *label = g_strdup_printf("Only %s  ×", agent);

        htmx_element_add_class(HTMX_ELEMENT(chip), "tab");
        htmx_node_set_text_content(HTMX_NODE(chip), label);
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(chip));
    }

    clawt_web_add(view, clawt_web_notices_sections(reply, agent,
                                                   g_get_real_time(),
                                                   &shown));

    if (shown == 0)
        clawt_web_add(view, clawt_web_empty(
            reply != NULL ? "Nothing has needed you."
                          : "The daemon did not answer.",
            "Older events are in the daemon's log, below."));

    /*
     * And the routine stream.  ClawtEventLog has been recording every
     * published event since the daemon was written; the page reads it
     * rather than keeping a copy.
     */
    {
        g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
        g_autoptr(JsonNode) history = NULL;
        JsonArray *events;

        if (agent != NULL)
            clawt_web_payload_set(payload, "subject", agent);

        clawt_web_payload_set_int(payload, "limit", 50);

        history = clawt_web_app_call(app, "event.list",
                                     clawt_web_payload_take(
                                         g_steal_pointer(&payload)));
        events = clawt_web_member_array(clawt_web_root(history), "events");

        if (events != NULL && json_array_get_length(events) > 0) {
            g_autoptr(HtmxElement) details = HTMX_ELEMENT(htmx_details_new());
            g_autoptr(HtmxElement) summary = HTMX_ELEMENT(htmx_summary_new());
            g_autoptr(HtmxDiv) inner = htmx_div_new();
            guint n = json_array_get_length(events);

            htmx_node_set_text_content(HTMX_NODE(summary),
                                       "Earlier, from the daemon's log");
            htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(summary));
            htmx_element_add_class(HTMX_ELEMENT(inner), "details-body");

            /* Newest first, like the list above it. */
            for (i = n; i > 0; i--) {
                JsonObject *one = json_array_get_object_element(events, i - 1);
                g_autoptr(HtmxDiv) line = htmx_div_new();
                g_autofree gchar *text = g_strdup_printf(
                    "%s · %s",
                    clawt_web_member(one, "kind", "?"),
                    clawt_web_member(one, "subject", "the fleet"));

                htmx_element_add_class(HTMX_ELEMENT(line), "alert-routine");
                htmx_node_set_text_content(HTMX_NODE(line), text);
                htmx_node_add_child(HTMX_NODE(inner), HTMX_NODE(line));
            }

            htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(inner));
            htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(details));
        }
    }

    return HTMX_ELEMENT(g_steal_pointer(&view));
}

/* {"all": true}, which notice.read and room.read both take. */
static JsonNode *
all_payload(void)
{
    ClawtWebPayload *payload = clawt_web_payload_new();

    clawt_web_payload_set_bool(payload, "all", TRUE);

    return clawt_web_payload_take(payload);
}

static HtmxResponse *
on_alerts(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *agent = htmx_request_get_query_param(request, "agent");
    g_autofree gchar *html = NULL;
    g_autoptr(HtmxElement) body = NULL;
    JsonObject *reply;

    (void)params;

    /*
     * Opening the page reads every alert, exactly as opening the GTK
     * panel does -- for every client, since the read is the daemon's.
     * Drawn first, so the rows that were new still say so this once.
     */
    body = alerts_body(app, (agent != NULL && *agent != '\0') ? agent : NULL);
    reply = clawt_web_app_notices(app);

    if (reply != NULL && clawt_web_member_int(reply, "unread", 0) > 0) {
        g_autoptr(JsonNode) read = clawt_web_app_call(app, "notice.read",
                                                      all_payload());

        (void)read;
        clawt_web_app_notices_changed(app);
    }

    html = clawt_web_page(app, NULL, CLAWT_PAGE_CHAT, body, request);

    return clawt_web_html_response(html);
}

/*
 * One verb, then the page again.  The list is marked unknown so the page
 * shows the result rather than racing the `notice.changed` event; a
 * refusal is the page's error banner.
 */
static HtmxResponse *
after_verb(ClawtWebApp *app, HtmxRequest *request, const gchar *kind,
           JsonNode *payload)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, kind, payload);

    clawt_web_app_notices_changed(app);

    if (reply == NULL)
        return clawt_web_error_page(app, request, NULL, CLAWT_PAGE_CHAT,
                                    clawt_web_app_last_error(app));

    return clawt_web_redirect(request, "/alerts");
}

/* A query parameter as a one-member payload, or NULL without it. */
static JsonNode *
param_payload(HtmxRequest *request, const gchar *query, const gchar *key)
{
    const gchar *value = htmx_request_get_query_param(request, query);
    ClawtWebPayload *payload = clawt_web_payload_new();

    if (value != NULL && *value != '\0')
        clawt_web_payload_set(payload, key, value);

    return clawt_web_payload_take(payload);
}

static HtmxResponse *
on_read(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    (void)params;

    return after_verb(user_data, request, "notice.read",
                      param_payload(request, "id", "id"));
}

static HtmxResponse *
on_read_all(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(JsonNode) rooms = NULL;

    (void)params;

    /* every conversation too: "Mark all read" is the whole list */
    rooms = clawt_web_app_call(app, "room.read", all_payload());
    (void)rooms;

    return after_verb(app, request, "notice.read", all_payload());
}

static HtmxResponse *
on_dismiss(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    (void)params;

    return after_verb(user_data, request, "notice.dismiss",
                      param_payload(request, "id", "id"));
}

static HtmxResponse *
on_clear(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    (void)params;

    return after_verb(user_data, request, "notice.clear",
                      param_payload(request, "agent", "agent"));
}

static HtmxResponse *
on_chat_read(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    (void)params;

    return after_verb(user_data, request, "room.read",
                      param_payload(request, "room", "room"));
}

static HtmxResponse *
on_mute(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    (void)params;

    return after_verb(user_data, request, "notice.mute",
                      param_payload(request, "agent", "agent"));
}

void
clawt_web_register_alerts(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/alerts", on_alerts, app);
    htmx_router_post(router, "/alerts/read", on_read, app);
    htmx_router_post(router, "/alerts/read-all", on_read_all, app);
    htmx_router_post(router, "/alerts/dismiss", on_dismiss, app);
    htmx_router_post(router, "/alerts/clear", on_clear, app);
    htmx_router_post(router, "/alerts/chat-read", on_chat_read, app);
    htmx_router_post(router, "/alerts/mute", on_mute, app);
}
