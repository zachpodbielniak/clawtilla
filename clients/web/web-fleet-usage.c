/*
 * web-fleet-usage.c - The Fleet page: what each agent costs, and which sit idle
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-fleet.c.  The page body carries a panel that
 * reloads itself (as Activity's does), and the period lives in that
 * panel's own URL -- /f/fleet-usage?period=N -- because a page body is
 * rendered without the request.  Every line of text is libclawt's
 * (clawt_usage_fleet_*), shared with the GTK and libregnum clients.
 * Named for usage: web-fleet.c is the sidebar's fleet.
 */

#include "web-pages.h"

#include <string.h>

/* Two weeks of shape, without the strip outgrowing a line. */
#define STRIP_DAYS (14)

static guint
period_of(const gchar *text)
{
    guint64 n;

    if (text == NULL || *text == '\0')
        return CLAWT_USAGE_PERIOD_DEFAULT;
    n = g_ascii_strtoull(text, NULL, 10);
    return n < clawt_usage_period_count() ? (guint)n : CLAWT_USAGE_PERIOD_DEFAULT;
}

/* One agent: what it spent and its share, its recency, Stop when idle. */
static HtmxElement *
agent_element(JsonObject *agent, gint64 total, gint64 now)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxDiv) head = htmx_div_new();
    const gchar *id = clawt_web_member(agent, "id", "?");
    const gchar *name = clawt_web_member(agent, "name", id);
    const gchar *idle = clawt_web_member(agent, "idle", NULL);
    const gchar *tone = clawt_web_member(agent, "idle_tone", "neutral");
    const gchar *state = clawt_web_member(agent, "state", NULL);
    gint64 cost = clawt_web_member_int(agent, "cost_micros", 0);
    g_autofree gchar *subtitle = clawt_usage_fleet_row_subtitle(agent, now);
    g_autofree gchar *money = clawt_usage_format_cost(cost);
    g_autofree gchar *share = clawt_usage_share_label(cost, total);
    g_autofree gchar *cost_text = g_strdup_printf("%s \xc2\xb7 %s", money, share);

    htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
    htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

    {
        g_autoptr(HtmxSpan) label = htmx_span_new();

        htmx_element_add_class(HTMX_ELEMENT(label), "list-item-title");
        htmx_node_set_text_content(HTMX_NODE(label), name);
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
    }
    if (idle != NULL)
        clawt_web_add(head, clawt_web_badge(idle, tone));
    /* the state in words when it is not running: the badge is recency */
    if (state != NULL && g_strcmp0(state, "running") != 0)
        clawt_web_add(head, clawt_web_badge(state, "neutral"));
    clawt_web_add(head, clawt_web_badge(cost_text, "info"));
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

    clawt_web_add(row, clawt_web_text(subtitle, "list-item-sub"));

    if (clawt_usage_fleet_row_stoppable(agent)) {
        g_autoptr(HtmxDiv) actions = htmx_div_new();
        g_autofree gchar *escaped = g_uri_escape_string(id, NULL, FALSE);
        g_autofree gchar *action = g_strdup_printf("/fleet-usage/%s/stop", escaped);
        g_autofree gchar *confirm = g_strdup_printf(
            "Stop %s? Its process and computer stop until somebody starts it.", id);

        htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
        clawt_web_add(actions, clawt_web_post_button("Stop", action, "default", confirm));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));
    }

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

/* The part that reloads itself: the period tabs and everything below. */
static HtmxElement *
fleet_panel(ClawtWebApp *app, guint period)
{
    g_autoptr(HtmxDiv) panel = htmx_div_new();
    g_autofree gchar *self_url = g_strdup_printf("/f/fleet-usage?period=%u", period);
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "usage.summary",
        clawt_usage_fleet_payload(clawt_usage_period_since(period, now), STRIP_DAYS));
    JsonObject *root = clawt_web_root(reply);
    guint i;

    htmx_element_set_id(HTMX_ELEMENT(panel), "fleet-usage-panel");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-get", self_url);
    /* a turn is a cost; a minute is fine-grained enough for a bill */
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-trigger",
                               "every 60s [!document.hidden]");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-swap", "outerHTML");

    {
        g_autoptr(HtmxElement) nav = HTMX_ELEMENT(htmx_nav_new());

        htmx_element_add_class(nav, "tabs");
        for (i = 0; i < clawt_usage_period_count(); i++) {
            g_autofree gchar *url = g_strdup_printf("/f/fleet-usage?period=%u", i);
            g_autoptr(HtmxA) tab = htmx_a_new_with_href("#");

            htmx_element_add_class(HTMX_ELEMENT(tab), "tab");
            htmx_node_set_text_content(HTMX_NODE(tab), clawt_usage_period_label(i));
            htmx_element_set_attribute(HTMX_ELEMENT(tab), "hx-get", url);
            htmx_element_set_attribute(HTMX_ELEMENT(tab), "hx-target", "#fleet-usage-panel");
            htmx_element_set_attribute(HTMX_ELEMENT(tab), "hx-swap", "outerHTML");
            if (i == period)
                htmx_element_set_attribute(HTMX_ELEMENT(tab), "aria-current", "page");
            htmx_node_add_child(HTMX_NODE(nav), HTMX_NODE(tab));
        }
        htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(nav));
    }

    if (reply == NULL || root == NULL) {
        clawt_web_add(panel, clawt_web_empty(
            "The fleet's figures are unavailable",
            "Check the daemon connection, then reopen Fleet to retry."));
        return HTMX_ELEMENT(g_steal_pointer(&panel));
    }

    {
        g_autofree gchar *summary = clawt_usage_fleet_summary(root);
        g_autofree gchar *budget = clawt_usage_fleet_budget_line(root);
        g_autofree gchar *strip = clawt_usage_fleet_strip(root);
        JsonObject *total = clawt_web_member_object(root, "total");
        gint64 total_cost = clawt_web_member_int(total, "cost_micros", 0);
        JsonArray *agents = clawt_web_member_array(root, "agents");
        guint stoppable = clawt_usage_fleet_stoppable_count(root);
        g_autoptr(HtmxDiv) list = htmx_div_new();

        now = clawt_web_member_int(root, "now", now);
        clawt_web_add(panel, clawt_web_text(summary, "fleet-usage-total"));
        if (budget != NULL)
            clawt_web_add(panel, clawt_web_text(budget, "lede"));
        clawt_web_add(panel, clawt_web_text("The last 14 days", "list-item-sub"));
        /* a gap is a day nothing ran */
        clawt_web_add(panel, clawt_web_text(strip, "fleet-usage-strip"));

        if (stoppable > 0) {
            g_autoptr(HtmxDiv) actions = htmx_div_new();
            g_autofree gchar *label = g_strdup_printf("Stop the idle ones (%u)", stoppable);
            g_autofree gchar *confirm = g_strdup_printf(
                "Stop %u idle agent%s? Each one's process and computer stop until "
                "somebody starts it; its mailbox keeps whatever is sent meanwhile.",
                stoppable, stoppable == 1 ? "" : "s");

            htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
            clawt_web_add(actions, clawt_web_post_button(label, "/fleet-usage/stop-idle",
                                                         "danger", confirm));
            htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(actions));
        }

        htmx_element_add_class(HTMX_ELEMENT(list), "list");
        for (i = 0; agents != NULL && i < json_array_get_length(agents); i++)
            htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(
                agent_element(json_array_get_object_element(agents, i), total_cost, now)));
        htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(list));
    }

    return HTMX_ELEMENT(g_steal_pointer(&panel));
}

HtmxElement *
clawt_web_fleet_usage_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();

    (void)agent_id;
    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Fleet"));
    clawt_web_add(pad, clawt_web_text(
        "What each agent costs, and which sit idle. An idle agent that is "
        "running still holds its process and its computer.", "lede"));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(
        fleet_panel(app, CLAWT_USAGE_PERIOD_DEFAULT)));
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Routes ──────────────────────────────────────────────────────── */

static HtmxResponse *
on_fleet_usage_fragment(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autoptr(HtmxElement) panel = fleet_panel(user_data,
        period_of(htmx_request_get_query_param(request, "period")));

    (void)params;
    return clawt_web_fragment_response(panel);
}

static gboolean
stop_one(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    reply = clawt_web_app_call(app, "agent.stop",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    return reply != NULL;
}

static HtmxResponse *
on_fleet_usage_stop(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "agent");
    g_autofree gchar *done = NULL;

    if (!stop_one(app, agent_id))
        return clawt_web_error_page(app, request, NULL, CLAWT_PAGE_FLEET,
                                    clawt_web_app_last_error(app));

    done = g_strdup_printf("Stopped %s.", agent_id);
    return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_FLEET, done);
}

/*
 * Every running idle agent, read fresh: a page that sat open for an
 * hour must not stop an agent that has since gone back to work.
 */
static HtmxResponse *
on_fleet_usage_stop_idle(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "usage.summary",
        clawt_usage_fleet_payload(clawt_usage_period_since(0, now), 0));
    JsonArray *agents = clawt_web_member_array(clawt_web_root(reply), "agents");
    g_autofree gchar *done = NULL;
    guint stopped = 0;
    guint i;

    (void)params;
    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);

        if (clawt_usage_fleet_row_stoppable(agent) &&
            stop_one(app, clawt_web_member(agent, "id", NULL)))
            stopped++;
    }

    done = g_strdup_printf("Stopped %u idle agent%s.", stopped, stopped == 1 ? "" : "s");
    return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_FLEET, done);
}

void
clawt_web_register_fleet_usage(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/f/fleet-usage", on_fleet_usage_fragment, app);
    htmx_router_post(router, "/fleet-usage/stop-idle", on_fleet_usage_stop_idle, app);
    htmx_router_post(router, "/fleet-usage/:agent/stop", on_fleet_usage_stop, app);
}
