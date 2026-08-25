/*
 * web-alerts.c - What has happened, for a reader who was looking elsewhere
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The GTK client puts this in a collapsible panel on the right.  Here it
 * is a page, which is the same capability in the idiom this client has:
 * a panel that pushes the content aside needs a third column and a
 * breakpoint, and a page needs neither and works with JavaScript off.
 *
 * The split is the same and it is the whole design: of the daemon's
 * events, two are notifications -- a download that failed, a message the
 * loop guard refused -- and the rest is a routine stream that is worth
 * having and must not shout.  So: one list, a filter that widens it, and
 * three tiers of which only two are coloured.
 */

#include "clawt-web.h"
#include "web-pages.h"
#include "web-ui.h"

#include <string.h>

/* "4m ago", because a wall-clock time in a list of things that just
 * happened is a number you have to subtract. */
static gchar *
alert_when(gint64 ts)
{
    gint64 seconds = (g_get_real_time() - ts) / G_USEC_PER_SEC;

    if (seconds < 60)
        return g_strdup("just now");

    if (seconds < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m ago", seconds / 60);

    if (seconds < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h ago", seconds / 3600);

    return g_strdup_printf("%" G_GINT64_FORMAT "d ago", seconds / 86400);
}

static HtmxElement *
alert_row(ClawtWebAlert *alert)
{
    g_autoptr(HtmxElement) row = HTMX_ELEMENT(htmx_article_new());
    g_autoptr(HtmxDiv) text = htmx_div_new();
    g_autoptr(HtmxDiv) meta = htmx_div_new();
    g_autofree gchar *when = alert_when(alert->ts);
    g_autofree gchar *line = NULL;
    g_autofree gchar *dismiss = g_strdup_printf("/alerts/dismiss?id=%u",
                                                alert->id);

    htmx_element_add_class(row, "alert-row");

    switch (alert->tier) {
    case CLAWT_WEB_ALERT_ERROR:
        htmx_element_add_class(row, "alert-error");
        break;
    case CLAWT_WEB_ALERT_NOTICE:
        htmx_element_add_class(row, "alert-notice");
        break;
    default:
        /*
         * No card and no disc.  A routine entry is one line of quiet
         * caption text, because severity is carried by weight and
         * container as much as by hue -- which also means it survives a
         * colourblind reader and a Catppuccin palette.
         */
        htmx_element_add_class(row, "alert-routine");
        break;
    }

    if (!alert->read && alert->tier != CLAWT_WEB_ALERT_ROUTINE)
        htmx_element_add_class(row, "alert-unread");

    htmx_element_add_class(HTMX_ELEMENT(text), "alert-text");
    htmx_node_set_text_content(HTMX_NODE(text), alert->text);
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(text));

    htmx_element_add_class(HTMX_ELEMENT(meta), "alert-meta");

    /*
     * Relative time and the source, so a row says where to go and look
     * without a second click.  The agent is a link that narrows the page
     * to it -- one control for scope, not a second dropdown.
     */
    if (alert->agent != NULL && *alert->agent != '\0') {
        g_autofree gchar *escaped = g_uri_escape_string(alert->agent, NULL,
                                                        FALSE);
        g_autofree gchar *url = g_strdup_printf("/alerts?agent=%s", escaped);
        g_autoptr(HtmxA) who = htmx_a_new_with_href(url);

        htmx_element_add_class(HTMX_ELEMENT(who), "alert-agent");
        htmx_node_set_text_content(HTMX_NODE(who), alert->agent);
        htmx_node_add_child(HTMX_NODE(meta), HTMX_NODE(who));
    }

    line = g_strdup_printf("%s · %s", when, alert->source);

    {
        g_autoptr(HtmxSpan) span = htmx_span_new();

        htmx_element_add_class(HTMX_ELEMENT(span), "muted");
        htmx_node_set_text_content(HTMX_NODE(span), line);
        htmx_node_add_child(HTMX_NODE(meta), HTMX_NODE(span));
    }

    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(meta));

    /*
     * Always present rather than revealed on hover: a control that is
     * invisible until you know it exists is a control nobody finds --
     * and this page has to work without JavaScript, where there is no
     * hover to speak of on a touch screen either.
     */
    clawt_web_add(row, clawt_web_post_button("Dismiss", dismiss, "default",
                                             NULL));

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

static HtmxElement *
alerts_body(ClawtWebApp *app, gboolean show_all, const gchar *agent)
{
    g_autoptr(HtmxDiv) view = htmx_div_new();
    g_autoptr(HtmxDiv) head = htmx_div_new();
    GPtrArray *alerts = clawt_web_app_alerts(app);
    guint shown = 0;
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(view), "view-pad");

    clawt_web_add(view, clawt_web_section_title("Alerts"));

    {
        g_autoptr(HtmxP) lede = htmx_p_new();

        htmx_element_add_class(HTMX_ELEMENT(lede), "muted");
        htmx_node_set_text_content(HTMX_NODE(lede),
            "What happened while you were looking somewhere else. An alert "
            "is an event that demanded attention; a routine entry is one "
            "that did not. This is the session's; the daemon keeps the "
            "durable copy, which the history below reads.");
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(lede));
    }

    htmx_element_add_class(HTMX_ELEMENT(head), "btn-row");

    /*
     * Two options, not five.  A page that opens onto every routine event
     * is noise; one that opens onto only errors hides what you came for.
     */
    {
        g_autoptr(HtmxA) only = htmx_a_new_with_href("/alerts");
        g_autoptr(HtmxA) every = htmx_a_new_with_href("/alerts?all=1");

        htmx_element_add_class(HTMX_ELEMENT(only), "tab");
        htmx_element_add_class(HTMX_ELEMENT(every), "tab");
        htmx_node_set_text_content(HTMX_NODE(only), "Alerts");
        htmx_node_set_text_content(HTMX_NODE(every), "Everything");

        if (!show_all)
            htmx_element_set_attribute(HTMX_ELEMENT(only), "aria-current",
                                       "page");
        else
            htmx_element_set_attribute(HTMX_ELEMENT(every), "aria-current",
                                       "page");

        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(only));
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(every));
    }

    clawt_web_add(head, clawt_web_post_button("Clear all", "/alerts/clear",
                                              "default", NULL));
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(head));

    if (agent != NULL) {
        g_autoptr(HtmxA) chip = htmx_a_new_with_href(show_all
                                                     ? "/alerts?all=1"
                                                     : "/alerts");
        g_autofree gchar *label = g_strdup_printf("Only %s  ×", agent);

        htmx_element_add_class(HTMX_ELEMENT(chip), "tab");
        htmx_node_set_text_content(HTMX_NODE(chip), label);
        htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(chip));
    }

    for (i = 0; alerts != NULL && i < alerts->len; i++) {
        ClawtWebAlert *alert = g_ptr_array_index(alerts, i);

        if (!show_all && alert->tier == CLAWT_WEB_ALERT_ROUTINE)
            continue;

        if (agent != NULL && g_strcmp0(agent, alert->agent) != 0)
            continue;

        clawt_web_add(view, alert_row(alert));
        shown++;
    }

    if (shown == 0)
        clawt_web_add(view, clawt_web_empty(
            show_all ? "Nothing has happened."
                     : "Nothing has needed you.",
            show_all ? NULL
                     : "Widen it to Everything to see the whole stream."));

    /*
     * And the durable copy.  ClawtEventLog has been recording every
     * published event since the daemon was written and was read back by
     * nobody -- which is why diagnosing a message loop meant running
     * sqlite3 and grep on the host.
     */
    {
        g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *events;

        if (agent != NULL)
            clawt_web_payload_set(payload, "subject", agent);

        clawt_web_payload_set_int(payload, "limit", 50);

        reply = clawt_web_app_call(app, "event.list",
                                   clawt_web_payload_take(
                                       g_steal_pointer(&payload)));
        events = clawt_web_member_array(clawt_web_root(reply), "events");

        if (events != NULL && json_array_get_length(events) > 0) {
            g_autoptr(HtmxElement) details = HTMX_ELEMENT(htmx_details_new());
            g_autoptr(HtmxElement) summary = HTMX_ELEMENT(htmx_summary_new());
            g_autoptr(HtmxDiv) inner = htmx_div_new();
            guint n = json_array_get_length(events);

            htmx_node_set_text_content(HTMX_NODE(summary),
                                       "Earlier, from the daemon's log");
            htmx_node_add_child(HTMX_NODE(details), HTMX_NODE(summary));
            htmx_element_add_class(HTMX_ELEMENT(inner), "details-body");

            /* Newest first, like the live list above it. */
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

static HtmxResponse *
on_alerts(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *all = htmx_request_get_query_param(request, "all");
    const gchar *agent = htmx_request_get_query_param(request, "agent");
    g_autofree gchar *html = NULL;
    g_autoptr(HtmxElement) body = NULL;

    (void)params;

    /*
     * Opening the page marks everything read, exactly as opening the GTK
     * panel does.  The badge is "how much have you not seen", and you
     * have now seen it.
     */
    body = alerts_body(app, all != NULL && *all != '\0',
                       (agent != NULL && *agent != '\0') ? agent : NULL);
    clawt_web_app_alerts_mark_read(app);

    html = clawt_web_page(app, NULL, CLAWT_WEB_VIEW_CHAT, body, request);

    return clawt_web_html_response(html);
}

static HtmxResponse *
on_dismiss(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    const gchar *id = htmx_request_get_query_param(request, "id");

    (void)params;

    if (id != NULL)
        clawt_web_app_alert_dismiss(app, (guint)g_ascii_strtoull(id, NULL,
                                                                 10));

    return clawt_web_redirect(request, "/alerts");
}

static HtmxResponse *
on_clear(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;

    (void)params;

    clawt_web_app_alerts_clear(app);

    return clawt_web_redirect(request, "/alerts");
}

void
clawt_web_register_alerts(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/alerts", on_alerts, app);
    htmx_router_post(router, "/alerts/dismiss", on_dismiss, app);
    htmx_router_post(router, "/alerts/clear", on_clear, app);
}
