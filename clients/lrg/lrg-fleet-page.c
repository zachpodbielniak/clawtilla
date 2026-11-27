/*
 * lrg-fleet-page.c - The Fleet page: what each agent costs, and which sit idle
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-fleet.c.  One usage.summary read (sorted by
 * cost, with fourteen days of daily figures) drawn as: the period's
 * total and the day's budget, a strip of the last fourteen days, and a
 * row per agent -- what it spent and its share, how long since its last
 * turn, whether it is idle -- with Stop on any agent that is running and
 * doing nothing, and "Stop the idle ones" for all of them at once.
 * Every line of text is libclawt's (clawt_usage_fleet_*), so the three
 * clients say the same thing.
 *
 * Above the spend, the plan allowances (usage.providers), as in GTK: a
 * card per login, a meter per window filled for what is left, coloured
 * by clawt_usage_window_level().  Read with `refresh` when the page is
 * shown or Refresh pressed; usage.providers_changed redraws without
 * asking again, or a login that keeps failing would loop.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"
#include "clawt-lrg-fleet-model.h"

/* Fourteen days: two weeks of shape without the strip outgrowing a row. */
#define STRIP_DAYS (14)


static struct {
    ClawtLrgApp *app;
    LrgWidget   *page;
    LrgWidget   *period;
    LrgWidget   *summary;
    LrgWidget   *budget;
    LrgWidget   *strip;
    LrgWidget   *list;
    LrgWidget   *stop_idle;
    LrgWidget   *limits;        /* the allowance cards */
    LrgWidget   *warnings;      /* the daemon's fleet warnings, one row each */
    guint        warnings_generation;
    guint        limits_generation;
    GPtrArray   *stoppable;     /* agent ids Stop the idle ones would stop */
    guint        generation;
    gboolean     in_flight;
    gboolean     again;
    JsonNode    *summary_reply; /* usage.summary, ref'd */
    JsonNode    *agents_reply;  /* agent.list, ref'd */
    JsonNode    *incidents_reply;
    JsonNode    *teams_reply;
    guint        side_generation;
} fleet;

static void paint_rows(ClawtLrgApp *app);
static void paint_warnings(ClawtLrgApp *app);

static void
keep_reply(JsonNode **slot, JsonNode *reply)
{
    if (*slot == reply)
        return;

    if (*slot != NULL)
        json_node_unref(*slot);

    *slot = (reply != NULL) ? json_node_ref(reply) : NULL;
}

static JsonObject *
listed_agent(const gchar *id)
{
    JsonArray *agents = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(fleet.agents_reply), "agents");
    guint i;

    if (id == NULL)
        return NULL;

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *one = json_array_get_object_element(agents, i);

        if (g_strcmp0(clawt_ipc_payload_string(one, "id"), id) == 0)
            return one;
    }

    return NULL;
}

static void request_fleet(ClawtLrgApp *app);

static gboolean
page_showing(ClawtLrgApp *app)
{
    return app->page == CLAWT_PAGE_FLEET;
}

/* ── Stop ─────────────────────────────────────────────────────────── */

static void
on_stop_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "agent-id");

    if (id != NULL)
        clawt_lrg_agent_action(user_data, id, "agent.stop");
}

static void
on_stop_idle_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    guint i;

    /* the ids the button was drawn for, not a fresh guess */
    for (i = 0; fleet.stoppable != NULL && i < fleet.stoppable->len; i++)
        clawt_lrg_agent_action(app, g_ptr_array_index(fleet.stoppable, i),
                               "agent.stop");
}

static void
on_stop_idle_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autofree gchar *title = NULL;

    if (fleet.stoppable == NULL || fleet.stoppable->len == 0)
        return;

    title = g_strdup_printf("Stop %u idle agent%s?", fleet.stoppable->len,
                            fleet.stoppable->len == 1 ? "" : "s");
    clawt_lrg_confirm(app, title,
                      "Each one's process and computer stop. A stopped agent "
                      "does nothing until somebody starts it; its mailbox "
                      "keeps whatever is sent to it meanwhile.",
                      "Stop them", TRUE, on_stop_idle_confirmed, NULL, NULL);
}

/* ── Drawing ──────────────────────────────────────────────────────── */

static LrgWidget *
agent_row(ClawtLrgApp *app, JsonObject *agent, gint64 total, gint64 now)
{
    const gchar *id = clawt_ipc_payload_string(agent, "id");
    const gchar *name = clawt_ipc_payload_string(agent, "name");
    JsonObject *machine = listed_agent(id);
    gint64 cost = clawt_ipc_payload_int(agent, "cost_micros", 0);
    g_autofree gchar *subtitle = clawt_usage_fleet_row_subtitle(agent, now);
    g_autofree gchar *detail = clawt_container_agent_detail(machine);
    g_autofree gchar *pressure = clawt_container_memory_pressure_text(id,
                                                                      machine);
    g_autofree gchar *shown = NULL;
    g_autofree gchar *money = clawt_usage_format_cost(cost);
    g_autofree gchar *share = clawt_usage_share_label(cost, total);
    g_autofree gchar *cost_text = g_strdup_printf("%s \xc2\xb7 %s", money, share);
    const gchar *idle = clawt_ipc_payload_string(agent, "idle");
    const gchar *tone = clawt_ipc_payload_string(agent, "idle_tone");
    const gchar *state = clawt_ipc_payload_string(agent, "state");
    LrgWidget *row;

    if (detail != NULL)
        shown = g_strdup_printf("%s\n%s", subtitle, detail);

    if (name == NULL || *name == '\0')
        name = id;

    row = clawt_lrg_row(name != NULL ? name : "?",
                        shown != NULL ? shown : subtitle);

    if (pressure != NULL)
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge("memory", "warn"));

    if (idle != NULL)
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge(idle, tone != NULL ? tone : "neutral"));

    /* the state in words when it is not running: the badge above is recency */
    if (state != NULL && g_strcmp0(state, "running") != 0)
        clawt_lrg_row_add_suffix(row, clawt_lrg_badge(state, "neutral"));

    /* on a fallback: the cost is another backend's bill now */
    if (agent != NULL && json_object_has_member(agent, "failover") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(agent, "failover"))) {
        g_autofree gchar *fleet_fallback = clawt_failover_badge(
            json_object_get_object_member(agent, "failover"));

        if (fleet_fallback != NULL)
            clawt_lrg_row_add_suffix(row, clawt_lrg_badge(fleet_fallback,
                                                          "warn"));
    }

    clawt_lrg_row_add_suffix(row, clawt_lrg_label(cost_text, "heading"));

    if (clawt_usage_fleet_row_stoppable(agent) && id != NULL) {
        LrgWidget *stop = clawt_lrg_button("Stop", "flat",
                                           G_CALLBACK(on_stop_clicked), app);

        g_object_set_data_full(G_OBJECT(stop), "agent-id", g_strdup(id), g_free);
        clawt_lrg_row_add_suffix(row, stop);
        g_ptr_array_add(fleet.stoppable, g_strdup(id));
    }

    return row;
}

static void
paint_rows(ClawtLrgApp *app)
{
    JsonObject *root = clawt_ipc_reply_object(fleet.summary_reply);
    JsonArray *agents = NULL;
    gint64 total = 0;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;
    g_autofree gchar *summary = NULL;
    g_autofree gchar *budget = NULL;
    g_autofree gchar *strip = NULL;
    g_autofree gchar *stop_label = NULL;

    if (root == NULL || fleet.list == NULL)
        return;

    summary = clawt_usage_fleet_summary(root);
    budget = clawt_usage_fleet_budget_line(root);
    strip = clawt_usage_fleet_strip(root);

    if (json_object_has_member(root, "now"))
        now = json_object_get_int_member(root, "now");
    if (json_object_has_member(root, "total") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "total")))
        total = clawt_ipc_payload_int(
            json_object_get_object_member(root, "total"), "cost_micros", 0);
    agents = clawt_lrg_settings_member_array(root, "agents");

    lrg_label_set_text(LRG_LABEL(fleet.summary), summary);
    lrg_label_set_text(LRG_LABEL(fleet.strip), strip);
    lrg_label_set_text(LRG_LABEL(fleet.budget), budget != NULL ? budget : "");
    lrg_widget_set_visible(fleet.budget, budget != NULL);

    g_ptr_array_set_size(fleet.stoppable, 0);
    clawt_lrg_clear(fleet.list);

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        LrgWidget *row = agent_row(app, json_array_get_object_element(agents, i),
                                   total, now);

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(fleet.list, row);
    }

    stop_label = g_strdup_printf("Stop the idle ones (%u)", fleet.stoppable->len);
    lrg_button_set_text(LRG_BUTTON(fleet.stop_idle), stop_label);
    lrg_widget_set_enabled(fleet.stop_idle, fleet.stoppable->len > 0);
    clawt_lrg_app_wake(app);
}

static void
on_summary(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);

    fleet.in_flight = FALSE;

    if (GPOINTER_TO_UINT(user_data) == fleet.generation && root != NULL &&
        fleet.list != NULL) {
        keep_reply(&fleet.summary_reply, reply);
        paint_rows(app);
    }

    /* an event that arrived while this was in flight asked for a re-read */
    if (fleet.again) {
        fleet.again = FALSE;
        request_fleet(app);
    }
}

/* ── Plan allowances ──────────────────────────────────────────────── */

static LrgWidget *
limit_window_row(JsonObject *window, gint64 now)
{
    LrgWidget *row = clawt_lrg_hbox(12.0f);
    LrgWidget *figures = clawt_lrg_vbox(2.0f);
    LrgWidget *label = clawt_lrg_label(clawt_ipc_payload_string(window, "label"), NULL);
    g_autofree gchar *value = clawt_usage_window_value_label(window);
    g_autofree gchar *reset = clawt_usage_window_reset_label(window, now);
    g_autofree gchar *meter_class = g_strdup_printf(
        "clawt-meter-%s", clawt_allowance_level_tone(clawt_usage_window_level(window)));
    gdouble left = 0.0;

    lrg_widget_set_min_size(label, 150.0f, 0.0f);
    lrg_widget_set_valign(label, LRG_ALIGN_CENTER);
    clawt_lrg_take(row, label);

    /* filled for what is left; no direction, no meter -- empty reads "used up" */
    if (clawt_usage_window_remaining(window, &left)) {
        LrgProgressBar *bar = lrg_progress_bar_new();

        lrg_progress_bar_set_max(bar, 1.0);
        lrg_progress_bar_set_value(bar, left / 100.0);
        lrg_widget_add_class(LRG_WIDGET(bar), meter_class);
        lrg_widget_set_min_size(LRG_WIDGET(bar), 120.0f, 8.0f);
        lrg_widget_set_valign(LRG_WIDGET(bar), LRG_ALIGN_CENTER);
        lrg_widget_set_fill_weight(LRG_WIDGET(bar), 1.0f);
        clawt_lrg_take(row, LRG_WIDGET(bar));
    } else {
        LrgWidget *spacer = clawt_lrg_label("", NULL);

        lrg_widget_set_fill_weight(spacer, 1.0f);
        clawt_lrg_take(row, spacer);
    }

    clawt_lrg_take(figures, clawt_lrg_label(value, "heading"));
    if (reset != NULL)
        clawt_lrg_take(figures, clawt_lrg_label(reset, "caption"));
    lrg_widget_set_min_size(figures, 170.0f, 0.0f);
    clawt_lrg_take(row, figures);
    return row;
}

static LrgWidget *
limit_login_card(JsonObject *login, gint64 now)
{
    LrgWidget *box = clawt_lrg_vbox(6.0f);
    LrgWidget *head = clawt_lrg_hbox(8.0f);
    LrgWidget *title_label;
    g_autofree gchar *title = clawt_usage_login_title(login);
    g_autofree gchar *status = clawt_usage_login_status_label(login, now);
    JsonArray *windows = clawt_lrg_settings_member_array(login, "windows");
    ClawtAllowanceLevel level = clawt_usage_login_level(login);
    guint i;

    title_label = clawt_lrg_label(title, "heading");
    lrg_widget_set_fill_weight(title_label, 1.0f);
    clawt_lrg_take(head, title_label);
    if (level == CLAWT_ALLOWANCE_LOW || level == CLAWT_ALLOWANCE_CRITICAL)
        clawt_lrg_take(head, clawt_lrg_badge(
            level == CLAWT_ALLOWANCE_CRITICAL ? "nearly out" : "running low",
            clawt_allowance_level_tone(level)));
    clawt_lrg_take(box, head);
    clawt_lrg_take(box, clawt_lrg_label(status, "muted"));

    for (i = 0; windows != NULL && i < json_array_get_length(windows); i++)
        clawt_lrg_take(box, limit_window_row(json_array_get_object_element(windows, i),
                                             now));
    return clawt_lrg_card(box);
}

static void
on_limits(ClawtLrgApp *app, JsonNode *reply, const GError *error, gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *logins;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    /* a newer read was asked for while this one was in flight */
    if (GPOINTER_TO_UINT(user_data) != fleet.limits_generation ||
        fleet.limits == NULL || root == NULL)
        return;

    now = clawt_ipc_payload_int(root, "now", now);
    logins = clawt_lrg_settings_member_array(root, "logins");
    clawt_lrg_clear(fleet.limits);
    clawt_lrg_take(fleet.limits, clawt_lrg_label("Plan allowances", "title"));

    if (logins == NULL || json_array_get_length(logins) == 0)
        clawt_lrg_take(fleet.limits, clawt_lrg_label(
            "No agent runs a CLI provider, so there is no plan allowance to show.",
            "muted"));
    for (i = 0; logins != NULL && i < json_array_get_length(logins); i++)
        clawt_lrg_take(fleet.limits,
                       limit_login_card(json_array_get_object_element(logins, i), now));
    clawt_lrg_app_wake(app);
}

static void
request_limits(ClawtLrgApp *app, gboolean ask)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    if (fleet.limits == NULL)
        return;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "refresh");
    json_builder_add_boolean_value(builder, ask);
    json_builder_end_object(builder);

    fleet.limits_generation++;
    clawt_lrg_request(app, "usage.providers", json_builder_get_root(builder),
                      CLAWT_LRG_QUIET, on_limits,
                      GUINT_TO_POINTER(fleet.limits_generation), NULL);
}

/*
 * The daemon's fleet warnings, as the web Fleet page and the GTK one
 * draw them: above the fleet list, one row each, nothing at all when
 * there are none.  They ride on team.list.  A stale reply is dropped by
 * its generation; a failed one keeps what was drawn.
 */
static void
on_warnings(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    if (GPOINTER_TO_UINT(user_data) != fleet.side_generation ||
        fleet.warnings == NULL || reply == NULL ||
        clawt_ipc_reply_object(reply) == NULL)
        return;

    (void)error;
    keep_reply(&fleet.teams_reply, reply);
    paint_warnings(app);
}

static void
on_incidents(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    if (GPOINTER_TO_UINT(user_data) != fleet.side_generation || reply == NULL)
        return;

    (void)error;
    keep_reply(&fleet.incidents_reply, reply);
    paint_warnings(app);
}

static void
on_agents(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    if (GPOINTER_TO_UINT(user_data) != fleet.side_generation || reply == NULL)
        return;

    (void)error;
    keep_reply(&fleet.agents_reply, reply);
    paint_rows(app);
}

static void
paint_warnings(ClawtLrgApp *app)
{
    g_autoptr(GPtrArray) texts = clawt_lrg_fleet_warnings(fleet.teams_reply);
    JsonArray *incidents = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(fleet.incidents_reply), "incidents");
    guint i;

    if (fleet.warnings == NULL)
        return;

    clawt_lrg_clear(fleet.warnings);

    for (i = 0; i < texts->len; i++) {
        LrgWidget *row = clawt_lrg_row("Worth fixing",
                                       g_ptr_array_index(texts, i));

        clawt_lrg_row_add_prefix(row, clawt_lrg_badge("!", "warn"));
        clawt_lrg_take(fleet.warnings, row);
    }

    for (i = 0; incidents != NULL && i < json_array_get_length(incidents);
         i++) {
        g_autofree gchar *text = clawt_container_oom_notice(
            json_array_get_object_element(incidents, i));
        LrgWidget *row;

        if (text == NULL)
            continue;

        row = clawt_lrg_row("Out of memory", text);
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge("!", "warn"));
        clawt_lrg_take(fleet.warnings, row);
    }

    clawt_lrg_app_wake(app);
}

static void
request_warnings(ClawtLrgApp *app)
{
    if (fleet.warnings == NULL)
        return;

    fleet.side_generation++;
    clawt_lrg_request(app, "team.list", NULL, CLAWT_LRG_QUIET, on_warnings,
                      GUINT_TO_POINTER(fleet.side_generation), NULL);
    clawt_lrg_request(app, "incident.list", NULL, CLAWT_LRG_QUIET, on_incidents,
                      GUINT_TO_POINTER(fleet.side_generation), NULL);
    clawt_lrg_request(app, "agent.list", NULL, CLAWT_LRG_QUIET, on_agents,
                      GUINT_TO_POINTER(fleet.side_generation), NULL);
}

static void
request_fleet(ClawtLrgApp *app)
{
    const gchar *chosen;
    guint index = CLAWT_USAGE_PERIOD_DEFAULT;

    if (fleet.list == NULL)
        return;

    /* one in flight; events meanwhile ask for one more read, not one each */
    if (fleet.in_flight) {
        fleet.again = TRUE;
        return;
    }

    chosen = clawt_lrg_combo_id(fleet.period);
    if (chosen != NULL)
        index = (guint)g_ascii_strtoull(chosen, NULL, 10);

    request_warnings(app);

    fleet.in_flight = TRUE;
    fleet.generation++;
    clawt_lrg_request(app, "usage.summary",
                      clawt_usage_fleet_payload(
                          clawt_usage_period_since(index,
                              g_get_real_time() / G_USEC_PER_SEC), STRIP_DAYS),
                      CLAWT_LRG_QUIET, on_summary,
                      GUINT_TO_POINTER(fleet.generation), NULL);
}

static void
on_period_changed(LrgComboBox *combo, gpointer user_data)
{
    request_fleet(user_data);
}

static void
on_refresh_clicked(LrgButton *button, gpointer user_data)
{
    request_fleet(user_data);
    request_limits(user_data, TRUE);
}

/* ── The page ─────────────────────────────────────────────────────── */

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *top = clawt_lrg_vbox(8.0f);
    LrgWidget *form = clawt_lrg_form_new();
    guint n = clawt_usage_period_count();
    g_autofree gchar *initial = g_strdup_printf("%u", CLAWT_USAGE_PERIOD_DEFAULT);
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    guint i;

    fleet.app = app;
    if (fleet.stoppable == NULL)
        fleet.stoppable = g_ptr_array_new_with_free_func(g_free);

    fleet.page = clawt_lrg_page_new("Fleet",
                                    "What each agent costs, and which sit idle");

    for (i = 0; i < n; i++) {
        g_ptr_array_add(ids, g_strdup_printf("%u", i));
        g_ptr_array_add(labels, (gpointer)clawt_usage_period_label(i));
    }
    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    /* connected after the initial selection, so building is not a change */
    fleet.period = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                   (const gchar *const *)labels->pdata,
                                   initial);
    g_signal_connect(fleet.period, "changed", G_CALLBACK(on_period_changed), app);
    clawt_lrg_form_add(form, "Period", fleet.period, NULL);
    clawt_lrg_take(top, form);

    fleet.summary = clawt_lrg_label("", "title");
    clawt_lrg_add(top, fleet.summary);
    g_object_unref(fleet.summary);

    fleet.budget = clawt_lrg_label("", "muted");
    clawt_lrg_add(top, fleet.budget);
    g_object_unref(fleet.budget);

    /* the last fourteen days, oldest first: a gap is a day nothing ran */
    clawt_lrg_take(top, clawt_lrg_label("The last 14 days", "caption"));
    fleet.strip = clawt_lrg_label("", "heading");
    clawt_lrg_add(top, fleet.strip);
    g_object_unref(fleet.strip);

    /* the plan allowances, before the spend: they say how long it can go on */
    fleet.limits = clawt_lrg_vbox(10.0f);
    clawt_lrg_add(clawt_lrg_page_body(fleet.page), fleet.limits);
    g_object_unref(fleet.limits);

    clawt_lrg_take(clawt_lrg_page_body(fleet.page), clawt_lrg_card(top));

    /* the daemon's warnings sit above the fleet list; empty draws nothing */
    fleet.warnings = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(clawt_lrg_page_body(fleet.page), fleet.warnings);
    g_object_unref(fleet.warnings);

    fleet.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(clawt_lrg_page_body(fleet.page), fleet.list);
    g_object_unref(fleet.list);

    fleet.stop_idle = clawt_lrg_button("Stop the idle ones", "danger",
                                       G_CALLBACK(on_stop_idle_clicked), app);
    clawt_lrg_add(clawt_lrg_page_toolbar(fleet.page), fleet.stop_idle);
    g_object_unref(fleet.stop_idle);
    clawt_lrg_take(clawt_lrg_page_toolbar(fleet.page),
                   clawt_lrg_button("Refresh", "flat",
                                    G_CALLBACK(on_refresh_clicked), app));

    return fleet.page;
}

static void
refresh(ClawtLrgApp *app)
{
    request_fleet(app);
    request_limits(app, TRUE);
}

static void
event(ClawtLrgApp *app, ClawtEvent *event)
{
    const gchar *kind = clawt_event_get_kind(event);

    if (fleet.page == NULL || !page_showing(app))
        return;

    /* a message is usually a turn's answer, and a turn is a cost */
    if (g_strcmp0(kind, "message") == 0 ||
        g_strcmp0(kind, "agent.state") == 0 ||
        g_strcmp0(kind, "agent.idle_stopped") == 0 ||
        g_strcmp0(kind, "agent.failover") == 0 ||
        g_strcmp0(kind, "spending.limit-reached") == 0)
        request_fleet(app);

    /* a login was read: redraw, ask for nothing */
    if (g_strcmp0(kind, "usage.providers_changed") == 0)
        request_limits(app, FALSE);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_FLEET, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_fleet_page(void)
{
    return &page_impl;
}
