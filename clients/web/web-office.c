/*
 * web-office.c - The Office page: who is where
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-office.c.  The roster is a panel at
 * /f/office-roster that reloads itself every few seconds while the tab is
 * visible -- office.presence is an edge, and a poll that short draws it
 * near enough to when it happened.  Each line is libclawt's
 * (clawt_office_roster_line()) and each badge clawt_office_activity_tone(),
 * so the three clients agree.  The drawn office (web-office-view.c) heads
 * the page whenever the office is on, in every build.
 */

#include "web-pages.h"

static HtmxElement *
roster_panel(ClawtWebApp *app)
{
    g_autoptr(HtmxDiv) panel = htmx_div_new();
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "office.snapshot", NULL);
    JsonObject *root = clawt_web_root(reply);
    JsonArray *people = clawt_web_member_array(root, "participants");
    gint64 now = clawt_web_member_int(root, "now", g_get_real_time() / G_USEC_PER_SEC);
    g_autoptr(HtmxDiv) list = htmx_div_new();
    guint i;

    htmx_element_set_id(HTMX_ELEMENT(panel), "office-roster");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-get", "/f/office-roster");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-trigger",
                               "every 3s [!document.hidden]");
    htmx_element_set_attribute(HTMX_ELEMENT(panel), "hx-swap", "outerHTML");

    if (reply == NULL || root == NULL) {
        clawt_web_add(panel, clawt_web_empty("The office is unavailable",
            "Check the daemon connection, then reopen Office to retry."));
        return HTMX_ELEMENT(g_steal_pointer(&panel));
    }
    if (!clawt_web_member_bool(root, "enabled", TRUE)) {
        clawt_web_add(panel, clawt_web_empty("The office is off",
            clawt_web_member(root, "why", "Set office.enabled to true.")));
        return HTMX_ELEMENT(g_steal_pointer(&panel));
    }

    {
        g_autofree gchar *status = g_strdup_printf(
            "World: %s. Every pose is something the daemon saw -- a turn, a tool, "
            "a message, a decision -- and nothing else.",
            clawt_web_member(root, "world", "?"));

        clawt_web_add(panel, clawt_web_text(status, "lede"));
    }

    htmx_element_add_class(HTMX_ELEMENT(list), "list");
    for (i = 0; people != NULL && i < json_array_get_length(people); i++) {
        JsonObject *p = json_array_get_object_element(people, i);
        const gchar *id = clawt_web_member(p, "id", "?");
        const gchar *kind = clawt_web_member(p, "kind", "agent");
        g_autofree gchar *line = clawt_office_roster_line(p, now);
        ClawtOfficeActivity activity = CLAWT_OFFICE_IDLE;
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxDiv) head = htmx_div_new();
        g_autoptr(HtmxSpan) name = htmx_span_new();

        clawt_office_activity_from_nick(clawt_web_member(p, "activity", NULL), &activity);
        htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
        htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");
        clawt_web_add(head, clawt_web_badge(clawt_office_activity_label(activity),
                                            clawt_office_activity_tone(activity)));
        htmx_element_add_class(HTMX_ELEMENT(name), "list-item-title");
        htmx_node_set_text_content(HTMX_NODE(name), clawt_web_member(p, "name", id));
        htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(name));
        if (g_strcmp0(kind, "agent") != 0)
            clawt_web_add(head, clawt_web_badge(
                g_strcmp0(kind, "grok_bot") == 0 ? "remote"
                : g_strcmp0(kind, "ephemeral") == 0 ? "helper" : kind, "neutral"));
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));
        clawt_web_add(row, clawt_web_text(line, "list-item-sub"));
        htmx_node_add_child(HTMX_NODE(list), HTMX_NODE(row));
    }
    htmx_node_add_child(HTMX_NODE(panel), HTMX_NODE(list));
    return HTMX_ELEMENT(g_steal_pointer(&panel));
}

/* On or off, and which world: office.set from a plain form. */
static HtmxElement *
settings_card(ClawtWebApp *app)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "office.worlds", NULL);
    JsonObject *root = clawt_web_root(reply);
    JsonArray *worlds = clawt_web_member_array(root, "worlds");
    g_autoptr(HtmxDiv) card = clawt_web_card("Office settings", NULL);
    HtmxElement *body = clawt_web_card_body(card);
    g_autoptr(HtmxForm) form = clawt_web_form("/office/set");
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) names = g_ptr_array_new();
    guint i;

    for (i = 0; worlds != NULL && i < json_array_get_length(worlds); i++) {
        JsonObject *w = json_array_get_object_element(worlds, i);

        g_ptr_array_add(ids, (gpointer)clawt_web_member(w, "id", "?"));
        g_ptr_array_add(names, (gpointer)clawt_web_member(w, "name",
                                                          clawt_web_member(w, "id", "?")));
    }
    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(names, NULL);

    clawt_web_add(form, clawt_web_switch_field("Keep the office", "enabled",
        "Who is where, from what the daemon sees; nothing is computed while it is off.",
        clawt_web_member_bool(root, "enabled", FALSE)));
    clawt_web_add(form, clawt_web_select_field("World", "world",
        (const gchar *const *)ids->pdata, (const gchar *const *)names->pdata,
        clawt_web_member(root, "current", NULL)));
    {
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) save = clawt_web_button("Save", "primary");

        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(save));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
    }
    htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    return HTMX_ELEMENT(g_steal_pointer(&card));
}

/*
 * Who keeps the office: the team carrying the office-custodial tag and
 * its generator, or a form whose one button makes the team.
 */
static HtmxElement *
custodians_card(ClawtWebApp *app)
{
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "office.custodians", NULL);
    JsonObject *root = clawt_web_root(reply);
    const gchar *team = clawt_web_member(root, "team", NULL);
    const gchar *generator = clawt_web_member(root, "generator", "");
    g_autoptr(HtmxDiv) card = clawt_web_card("Custodial team", NULL);
    HtmxElement *body = clawt_web_card_body(card);

    if (team != NULL) {
        g_autofree gchar *line = g_strdup_printf(
            "%s keeps the office; characters are made with %s.", team,
            *generator != '\0' ? generator : "nothing yet (office.asset_generator)");

        clawt_web_add(body, clawt_web_text(line, "muted"));
    } else {
        g_autoptr(HtmxForm) form = clawt_web_form("/office/custodians");
        g_autoptr(HtmxDiv) row = htmx_div_new();
        g_autoptr(HtmxButton) install = clawt_web_button("Create custodial team",
                                                         "primary");

        clawt_web_add(body, clawt_web_text(
            "Nobody keeps the office: a lead, an asset steward and a set designer "
            "would take your requests for it.", "muted"));
        htmx_element_add_class(HTMX_ELEMENT(row), "btn-row");
        htmx_element_set_attribute(HTMX_ELEMENT(install), "type", "submit");
        htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(install));
        htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(row));
        htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
    }
    return HTMX_ELEMENT(g_steal_pointer(&card));
}

static HtmxResponse *
on_custodians(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(JsonNode) reply = clawt_web_app_call(app, "office.custodians_install", NULL);

    (void)params;
    if (reply == NULL)
        return clawt_web_error_page(app, request, NULL, CLAWT_PAGE_OFFICE,
                                    clawt_web_app_last_error(app));
    return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_OFFICE,
                                  "The custodial team is installed.");
}

static HtmxResponse *
on_set(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    ClawtWebApp *app = user_data;
    g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *world = clawt_web_form_value(request, "world");

    (void)params;
    clawt_web_payload_set_bool(payload, "enabled", clawt_web_form_flag(request, "enabled"));
    if (world != NULL && *world != '\0')
        clawt_web_payload_set(payload, "world", world);
    reply = clawt_web_app_call(app, "office.set",
                               clawt_web_payload_take(g_steal_pointer(&payload)));
    if (reply == NULL)
        return clawt_web_error_page(app, request, NULL, CLAWT_PAGE_OFFICE,
                                    clawt_web_app_last_error(app));
    return clawt_web_after_action(app, request, NULL, CLAWT_PAGE_OFFICE, "Saved.");
}

HtmxElement *
clawt_web_office_body(ClawtWebApp *app, const gchar *agent_id)
{
    g_autoptr(HtmxElement) view = HTMX_ELEMENT(htmx_main_new());
    g_autoptr(HtmxDiv) pad = htmx_div_new();

    (void)agent_id;
    htmx_element_add_class(view, "view");
    htmx_element_add_class(HTMX_ELEMENT(pad), "view-pad");
    clawt_web_add(pad, clawt_web_section_title("Office"));
    /* the drawing first: it is what the page is for once the office is on */
    clawt_web_office_view_add(app, HTMX_ELEMENT(pad));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(settings_card(app)));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(custodians_card(app)));
    clawt_web_add(pad, clawt_web_text("Who is where", "limits-title"));
    htmx_node_add_child(HTMX_NODE(pad), HTMX_NODE(roster_panel(app)));
    htmx_node_add_child(HTMX_NODE(view), HTMX_NODE(pad));
    return g_steal_pointer(&view);
}

static HtmxResponse *
on_roster(HtmxRequest *request, GHashTable *params, gpointer user_data)
{
    g_autoptr(HtmxElement) panel = roster_panel(user_data);

    (void)request;
    (void)params;
    return clawt_web_fragment_response(panel);
}

void
clawt_web_register_office(HtmxRouter *router, ClawtWebApp *app)
{
    htmx_router_get(router, "/f/office-roster", on_roster, app);
    htmx_router_post(router, "/office/set", on_set, app);
    htmx_router_post(router, "/office/custodians", on_custodians, app);
    clawt_web_office_view_register(router, app);
}
