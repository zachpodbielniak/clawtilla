/*
 * lrg-settings-spending.c - Settings: Spending
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What the fleet has spent, over a period somebody picks.  Mirrors
 * clients/gtk/gtk-prefs-spending.c: a period, one row per agent with
 * its cost, and the total.  The periods and what each means in seconds
 * are the model's (clawt_lrg_settings_period_*), so the arithmetic is
 * tested rather than trusted.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

typedef struct {
    LrgWidget *list;
    LrgWidget *period;
    guint      generation;
} Spending;

static Spending spending;

static void refresh(ClawtLrgApp *app);

static void
on_period_changed(LrgComboBox *combo, gpointer user_data)
{
    refresh(user_data);
}

/* A row with the cost on the right, where a column of them lines up. */
static LrgWidget *
cost_row(const gchar *title, const gchar *subtitle, const gchar *cost,
         gboolean total)
{
    LrgWidget *row = clawt_lrg_row(title, subtitle);
    LrgWidget *value = clawt_lrg_label(cost, total ? "heading" : "muted");

    clawt_lrg_row_add_suffix(row, value);
    return row;
}

static void
on_summary(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *agents;
    JsonObject *total = NULL;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != spending.generation || root == NULL)
        return;

    clawt_lrg_clear(spending.list);
    agents = clawt_lrg_settings_member_array(root, "agents");

    if (json_object_has_member(root, "total") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(root, "total")))
        total = json_object_get_object_member(root, "total");

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *name = clawt_ipc_payload_string(agent, "name");
        g_autofree gchar *subtitle = clawt_lrg_settings_spend_subtitle(agent);
        g_autofree gchar *cost = clawt_lrg_settings_spend_cost(agent);
        LrgWidget *row;

        if (name == NULL || *name == '\0')
            name = clawt_ipc_payload_string(agent, "id");

        row = cost_row((name != NULL) ? name : "?", subtitle, cost, FALSE);
        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(spending.list, row);
    }

    {
        g_autofree gchar *subtitle =
            clawt_lrg_settings_spend_total_subtitle(total);
        g_autofree gchar *cost = clawt_lrg_settings_spend_cost(total);
        LrgWidget *row = cost_row("Total", subtitle, cost, TRUE);

        lrg_widget_add_class(row, "clawt-card-selected");
        clawt_lrg_take(spending.list, row);
    }

    clawt_lrg_app_wake(app);
}

static void
refresh(ClawtLrgApp *app)
{
    const gchar *chosen;
    guint index = 0;
    g_autoptr(GDateTime) now = g_date_time_new_now_local();

    if (spending.list == NULL)
        return;

    chosen = clawt_lrg_combo_id(spending.period);

    if (chosen != NULL)
        index = (guint)g_ascii_strtoull(chosen, NULL, 10);

    spending.generation++;
    clawt_lrg_request(app, "usage.summary",
                      clawt_lrg_settings_usage_payload(
                          clawt_lrg_settings_period_since(index, now)),
                      CLAWT_LRG_QUIET, on_summary,
                      GUINT_TO_POINTER(spending.generation), NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Spending",
                                         "What the fleet has cost");
    LrgWidget *box = clawt_lrg_vbox(10.0f);
    LrgWidget *form = clawt_lrg_form_new();
    guint n = clawt_lrg_settings_period_count();
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    guint i;

    for (i = 0; i < n; i++) {
        g_ptr_array_add(ids, g_strdup_printf("%u", i));
        g_ptr_array_add(labels, (gpointer)clawt_lrg_settings_period_label(i));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "The figure each provider reported for each turn, summed per "
        "agent. Token counts cover new input and output only -- cached "
        "context is billed but is not reported as tokens, so the cost is "
        "larger than the tokens beside it suggest.", "muted"));

    /* connected after the initial selection, so building is not a change */
    spending.period = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                      (const gchar *const *)labels->pdata,
                                      "0");
    g_signal_connect(spending.period, "changed",
                     G_CALLBACK(on_period_changed), app);
    clawt_lrg_form_add(form, "Period", spending.period, NULL);
    clawt_lrg_take(box, form);

    spending.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, spending.list);
    g_object_unref(spending.list);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "spending", "Spending", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_spending_page(void)
{
    return &page_impl;
}
