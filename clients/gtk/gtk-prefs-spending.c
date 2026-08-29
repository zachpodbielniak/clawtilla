/*
 * gtk-prefs-spending.c - Settings: spending
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What the fleet has spent, over a period somebody picks.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Spending ────────────────────────────────────────────────────── */

/*
 * The windows offered, and what each means in seconds.
 *
 * "All time" is first and is the default, because the first question is
 * how much this fleet has cost at all -- and because a narrower window
 * that happened to be empty would look exactly like a feature that does
 * not work.
 */
typedef struct {
    const gchar *label;
    gint         days;      /* 0 = everything, -1 = since local midnight */
} SpendingPeriod;

static const SpendingPeriod spending_periods[] = {
    { "All time", 0 },
    { "Today", -1 },
    { "Last 7 days", 7 },
    { "Last 30 days", 30 }
};

static gint64
spending_since_for(guint index)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    if (index >= G_N_ELEMENTS(spending_periods))
        return 0;

    if (spending_periods[index].days == 0)
        return 0;

    if (spending_periods[index].days < 0) {
        g_autoptr(GDateTime) now_dt = g_date_time_new_now_local();
        g_autoptr(GDateTime) midnight = g_date_time_new_local(
            g_date_time_get_year(now_dt), g_date_time_get_month(now_dt),
            g_date_time_get_day_of_month(now_dt), 0, 0, 0.0);

        return g_date_time_to_unix(midnight);
    }

    return now - (gint64)spending_periods[index].days * 86400;
}

static void
on_spending_period_changed(GObject *object, GParamSpec *pspec,
                           gpointer user_data)
{
    ClawtWindow *self = user_data;
    guint index = adw_combo_row_get_selected(ADW_COMBO_ROW(object));

    (void)pspec;

    self->settings_spending_since = spending_since_for(index);
    clawt_gtk_refresh_settings_spending(self);
}

/*
 * One row per agent, cheapest to read at a glance: the cost is the
 * title's suffix, the turns are the subtitle.
 */
void
clawt_gtk_refresh_settings_spending(ClawtWindow *self)
{
    if (self->settings_spending == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_SPENDING))
        return;

    do {
        g_autoptr(JsonNode) reply = NULL;
        JsonNode *payload;
        JsonArray *agents;
        JsonObject *total;
        JsonObject *root;
        g_autoptr(JsonBuilder) builder = json_builder_new();
        guint i;

        clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_spending));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "since");
        json_builder_add_int_value(builder, self->settings_spending_since);
        json_builder_end_object(builder);
        payload = json_builder_get_root(builder);

        reply = clawt_window_request(self, "usage.summary", payload);

        if (reply == NULL)
            continue;

        root = clawt_payload_of(reply);
        agents = json_object_get_array_member(root, "agents");
        total = json_object_get_object_member(root, "total");

        for (i = 0; i < json_array_get_length(agents); i++) {
            JsonObject *a = json_array_get_object_element(agents, i);
            gint64 turns = json_object_get_int_member_with_default(a, "turns",
                                                                   0);
            g_autofree gchar *cost = clawt_usage_format_cost(
                json_object_get_int_member_with_default(a, "cost_micros", 0));
            g_autofree gchar *subtitle = NULL;
            GtkWidget *row = adw_action_row_new();
            GtkWidget *value = gtk_label_new(cost);

            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(row),
                clawt_json_string(a, "name", clawt_json_string(a, "id", "?")));

            subtitle = (turns == 0)
                ? g_strdup("nothing recorded in this period")
                : g_strdup_printf("%" G_GINT64_FORMAT " turn%s, "
                                  "%" G_GINT64_FORMAT " tokens out",
                                  turns, turns == 1 ? "" : "s",
                                  json_object_get_int_member_with_default(
                                      a, "output_tokens", 0));

            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            gtk_widget_add_css_class(value, "numeric");
            gtk_widget_add_css_class(value, "dim-label");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), value);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_spending), row);
        }

        {
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *cost = clawt_usage_format_cost(
                json_object_get_int_member_with_default(total, "cost_micros",
                                                        0));
            GtkWidget *value = gtk_label_new(cost);
            g_autofree gchar *subtitle = g_strdup_printf(
                "%" G_GINT64_FORMAT " turns across the fleet",
                json_object_get_int_member_with_default(total, "turns", 0));

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Total");
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            gtk_widget_add_css_class(value, "numeric");
            gtk_widget_add_css_class(value, "heading");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), value);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_spending), row);
        }
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_SPENDING));
}

GtkWidget *
clawt_gtk_build_spending_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkStringList *periods = gtk_string_list_new(NULL);
    guint i;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Spending");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "emblem-documents-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "What the fleet has cost");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The figure each provider reported for each turn, summed per "
        "agent. Token counts cover new input and output only -- cached "
        "context is billed but is not reported as tokens, so the cost "
        "is larger than the tokens beside it suggest.");

    for (i = 0; i < G_N_ELEMENTS(spending_periods); i++)
        gtk_string_list_append(periods, spending_periods[i].label);

    self->settings_spending_period = adw_combo_row_new();
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(self->settings_spending_period), "Period");
    adw_combo_row_set_model(ADW_COMBO_ROW(self->settings_spending_period),
                            G_LIST_MODEL(periods));
    g_signal_connect(self->settings_spending_period, "notify::selected",
                     G_CALLBACK(on_spending_period_changed), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_spending_period);

    self->settings_spending = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_spending),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_spending, "boxed-list");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_spending);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}
