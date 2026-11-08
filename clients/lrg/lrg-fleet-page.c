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
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

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
    GPtrArray   *stoppable;     /* agent ids Stop the idle ones would stop */
    guint        generation;
    gboolean     in_flight;
    gboolean     again;
} fleet;

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
    gint64 cost = clawt_ipc_payload_int(agent, "cost_micros", 0);
    g_autofree gchar *subtitle = clawt_usage_fleet_row_subtitle(agent, now);
    g_autofree gchar *money = clawt_usage_format_cost(cost);
    g_autofree gchar *share = clawt_usage_share_label(cost, total);
    g_autofree gchar *cost_text = g_strdup_printf("%s \xc2\xb7 %s", money, share);
    const gchar *idle = clawt_ipc_payload_string(agent, "idle");
    const gchar *tone = clawt_ipc_payload_string(agent, "idle_tone");
    const gchar *state = clawt_ipc_payload_string(agent, "state");
    LrgWidget *row;

    if (name == NULL || *name == '\0')
        name = id;

    row = clawt_lrg_row(name != NULL ? name : "?", subtitle);

    if (idle != NULL)
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge(idle, tone != NULL ? tone : "neutral"));

    /* the state in words when it is not running: the badge above is recency */
    if (state != NULL && g_strcmp0(state, "running") != 0)
        clawt_lrg_row_add_suffix(row, clawt_lrg_badge(state, "neutral"));

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
on_summary(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *agents = NULL;
    gint64 total = 0;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    fleet.in_flight = FALSE;

    if (GPOINTER_TO_UINT(user_data) == fleet.generation && root != NULL &&
        fleet.list != NULL) {
        g_autofree gchar *summary = clawt_usage_fleet_summary(root);
        g_autofree gchar *budget = clawt_usage_fleet_budget_line(root);
        g_autofree gchar *strip = clawt_usage_fleet_strip(root);
        g_autofree gchar *stop_label = NULL;

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

    /* an event that arrived while this was in flight asked for a re-read */
    if (fleet.again) {
        fleet.again = FALSE;
        request_fleet(app);
    }
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

    clawt_lrg_take(clawt_lrg_page_body(fleet.page), clawt_lrg_card(top));

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
        g_strcmp0(kind, "spending.limit-reached") == 0)
        request_fleet(app);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_FLEET, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_fleet_page(void)
{
    return &page_impl;
}
