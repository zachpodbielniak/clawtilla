/*
 * web-activity.c - The activity page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every turn running across the fleet, from activity.list, as the GTK
 * client's page draws it: who, where, for how long, on whose behalf, and
 * the last few steps. Stop interrupts the whole agent and asks first,
 * with the library's wording; Steer is offered only where msg.send folds
 * the text into the running turn, which is the agent's conversation with
 * the operator.
 *
 * The panel polls itself while the tab is visible. Steps are drawn as
 * plain lines rather than in a <details>, because an outerHTML swap
 * every few seconds would close anything somebody had opened.
 *
 * The same swap took the Steer field with it: text typed into it was
 * gone at the next poll, so a correction took longer than four seconds
 * to write could not be sent at all -- found by trying to. Polling
 * pauses while the focus is inside the panel, which covers typing and
 * the click on Steer. Not hx-preserve as well: it kept the field across
 * the page the POST answers with, sent text and all, which invites
 * sending it twice.
 */

#include "web-pages.h"

#include <string.h>

/* One running turn. */
static HtmxElement *
turn_element(JsonObject *turn, gint64 now)
{
    g_autoptr(HtmxDiv) row = htmx_div_new();
    g_autoptr(HtmxDiv) head = htmx_div_new();
    const gchar *agent_id = clawt_web_member(turn, "agent", "?");
    const gchar *room = clawt_web_member(turn, "room_name",
                                         clawt_web_member(turn, "room", ""));
    const gchar *origin = clawt_web_member(turn, "origin", NULL);
    const gchar *task = clawt_web_member(turn, "task", NULL);
    g_autofree gchar *elapsed = clawt_activity_elapsed_label(
        clawt_web_member_int(turn, "started", 0), now);
    g_autofree gchar *title = g_strdup_printf(
        "%s in %s", clawt_web_member(turn, "agent_name", agent_id),
        *room != '\0' ? room : "an unnamed room");
    g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
    g_autoptr(GString) sub = g_string_new(NULL);
    JsonArray *steps = clawt_web_member_array(turn, "steps");
    guint i;

    htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
    htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");

    {
        g_autoptr(HtmxSpan) label = htmx_span_new();

        htmx_element_add_class(HTMX_ELEMENT(label), "list-item-title");
        htmx_node_set_text_content(HTMX_NODE(label), title);
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));
    }
    clawt_web_add(head, clawt_web_badge(elapsed, "info"));
    htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));

    g_string_append_printf(sub, "%" G_GINT64_FORMAT " step(s)",
                           clawt_web_member_int(turn, "step_count", 0));
    if (origin != NULL && g_strcmp0(origin, "user") != 0)
        g_string_append_printf(sub, " · for %s", origin);
    if (task != NULL)
        g_string_append_printf(sub, " · task %s", task);
    clawt_web_add(row, clawt_web_text(sub->str, "list-item-sub"));

    /* The most recent steps, through the same summary the chat draws. */
    for (i = 0; steps != NULL && i < json_array_get_length(steps); i++) {
        g_autoptr(ClawtTurnStep) step = clawt_turn_step_new_from_object(
            json_array_get_object_element(steps, i), agent_id);
        g_autofree gchar *line = NULL;
        g_autofree gchar *css = NULL;

        if (step == NULL)
            continue;
        line = clawt_turn_step_summary(step);
        css = g_strdup_printf("turn-run-cmd %s", clawt_turn_step_tone(step));
        clawt_web_add(row, clawt_web_text(line, css));
    }

    if (clawt_web_member_bool(turn, "steerable", FALSE)) {
        g_autofree gchar *action = g_strdup_printf("/activity/%s/steer", escaped);
        g_autofree gchar *input_id = g_strdup_printf("activity-steer-%s", agent_id);
        g_autoptr(HtmxForm) form = clawt_web_form(action);
        g_autoptr(HtmxButton) send = clawt_web_button("Steer", "primary");
        g_autoptr(HtmxDiv) field = htmx_div_new();
        g_autoptr(HtmxLabel) tag = htmx_label_new_with_text("Steer this turn");
        g_autoptr(HtmxInput) input = htmx_input_new_text("body");

        htmx_element_add_class(HTMX_ELEMENT(field), "field");
        htmx_input_set_placeholder(input, "It reads this at its next step");
        htmx_element_set_id(HTMX_ELEMENT(input), input_id);
        htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(tag));
        htmx_node_add_child(HTMX_NODE(field), HTMX_NODE(input));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(field));
        htmx_element_set_attribute(HTMX_ELEMENT(send), "type", "submit");
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(send));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(form));
    } else {
        clawt_web_add(row, clawt_web_notice(
            "This turn is in another room, and steering reaches only its "
            "conversation with you. Message it there, or stop it.", "info"));
    }

    {
        g_autoptr(HtmxDiv) actions = htmx_div_new();
        g_autofree gchar *action = g_strdup_printf("/activity/%s/stop", escaped);
        g_autofree gchar *confirm = clawt_activity_stop_prompt(
            agent_id, (guint)clawt_web_member_int(turn, "running_turns", 1));

        htmx_element_add_class(HTMX_ELEMENT(actions), "btn-row");
        clawt_web_add(actions, clawt_web_post_button("Stop agent", action, "danger",
                                                     confirm));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(actions));
    }

    return HTMX_ELEMENT(g_steal_pointer(&row));
}

/* The part that polls itself: everything below the title. */
static HtmxElement *
activity_panel(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) panel = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "activity.list", NULL);
    JsonArray *turns = clawt_web_member_array(clawt_web_root(reply), "turns");
    /* The daemon's clock, so an age is not skewed by this machine's. */
    gint64 now = clawt_web_member_int(clawt_web_root(reply), "now", g_get_real_time());
    guint i;

    htmx_element_set_id(HTMX_ELEMENT(panel), "activity-panel");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-get", "/f/activity");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-trigger",
                               "every 4s [!document.hidden && "
                               "!document.activeElement.closest('#activity-panel')]");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-swap", "outerHTML");

    if (reply == NULL) {
        clawt_web_add(panel, clawt_web_empty(
            "Activity is unavailable",
            "Check the daemon connection, then reopen Activity to retry."));
    } else if (turns == NULL || json_array_get_length(turns) == 0) {
        clawt_web_add(panel, clawt_web_empty(
            "Nothing is running",
            "Every agent is idle. A turn appears here the moment one starts."));
    } else {
        g_autoptr(HtmxDiv) list = htmx_div_new();

        htmx_element_add_class(HTMX_ELEMENT(list), "list");
        for (i = 0; i < json_array_get_length(turns); i++)
            htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(
                turn_element(json_array_get_object_element(turns, i), now)));
        htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(list));
    }

    return HTMX_ELEMENT(g_steal_pointer(&panel));
}

HtmxElement *
clawt_web_activity_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();

    (void)agent_id;
    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");

    clawt_web_add(pad, clawt_web_section_title("Activity"));
    clawt_web_add(pad, clawt_web_text(
        "Every turn running across the fleet. Stop interrupts the whole "
        "agent, not one turn.", "lede"));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(activity_panel(app)));
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));

    return g_steal_pointer(&view);
}

/* ── Routes ──────────────────────────────────────────────────────── */

static HtmxResponse *
on_activity_fragment(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autoptr(HtmxElement) panel = activity_panel(user_data);

    (void)request;
    (void)params;
    return clawt_web_fragment_response(panel);
}

static HtmxResponse *
on_activity_stop(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "agent");
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *done = NULL;

    clawt_web_payload_set(payload, "agent", agent_id);
    reply = clawt_web_app_call(app, "agent.interrupt",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id, CLAWT_PAGE_ACTIVITY,
                                    clawt_web_app_last_error(app));

    done = clawt_web_member_int(clawt_web_root(reply), "killed", 0) > 0
        ? g_strdup_printf("Stopped %s.", agent_id)
        : g_strdup_printf("%s had nothing running by the time it was asked.", agent_id);

    return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_ACTIVITY, done);
}

static HtmxResponse *
on_activity_steer(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autofree gchar *agent_id = clawt_web_param(params, "agent");
    g_autofree gchar *body = g_strdup(clawt_web_form_value(request, "body"));
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;

    if (body == NULL || *g_strstrip(body) == '\0')
        return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_ACTIVITY,
                                      "Nothing to send.");

    clawt_web_payload_set(payload, "target", agent_id);
    clawt_web_payload_set(payload, "body", body);
    reply = clawt_web_app_call(app, "msg.send",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    if (reply == NULL)
        return clawt_web_error_page(app, request, agent_id, CLAWT_PAGE_ACTIVITY,
                                    clawt_web_app_last_error(app));

    /* msg.send decides; a turn that finished meanwhile got a new message. */
    return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_ACTIVITY,
        clawt_web_member_bool(clawt_web_root(reply), "steered", FALSE)
            ? "Steered: it reads this at its next step."
            : "It had finished, so this went as a new message.");
}

void
clawt_web_register_activity(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/f/activity", on_activity_fragment, app);
    htmx_router_post(router, "/activity/:agent/stop", on_activity_stop, app);
    htmx_router_post(router, "/activity/:agent/steer", on_activity_steer, app);
}
