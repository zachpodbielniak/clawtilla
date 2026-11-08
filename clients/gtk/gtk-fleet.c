/*
 * gtk-fleet.c - The Fleet page: what each agent costs, and which sit idle
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One usage.summary read (sorted by cost, fourteen days of daily
 * figures): the period's total and the day's budget, a strip of the last
 * fourteen days, and a row per agent -- what it spent and its share, how
 * long since its last turn, whether it is idle -- with Stop on an agent
 * that is running and doing nothing, and "Stop the idle ones" for all of
 * them.  The text is libclawt's (clawt_usage_fleet_*), shared with the
 * web and libregnum clients.
 */

#include "clawt-window-private.h"

/* Two weeks of shape, without the strip outgrowing a line. */
#define STRIP_DAYS (14)

static void
fleet_stop_agent(ClawtWindow *self, const gchar *agent_id)
{
    g_autoptr(JsonNode) reply = clawt_window_request(
        self, "agent.stop", clawt_build_payload("agent", agent_id, NULL));

    (void)reply;
}

static void
on_fleet_stop(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "agent-id");

    if (id == NULL)
        return;
    fleet_stop_agent(self, id);
    clawt_gtk_refresh_fleet(self);
}

static void
on_stop_idle_confirmed(AdwAlertDialog *dialog, gchar *response, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GPtrArray *ids = g_object_get_data(G_OBJECT(dialog), "agent-ids");
    guint i;

    if (g_strcmp0(response, "stop") != 0 || ids == NULL)
        return;

    /* the ids the button was drawn for, not a fresh guess */
    for (i = 0; i < ids->len; i++)
        fleet_stop_agent(self, g_ptr_array_index(ids, i));
    clawt_gtk_refresh_fleet(self);
}

static void
on_stop_idle(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GPtrArray *ids = g_object_get_data(G_OBJECT(button), "agent-ids");
    g_autofree gchar *heading = NULL;
    AdwAlertDialog *confirm;
    GPtrArray *copy;
    guint i;

    if (ids == NULL || ids->len == 0)
        return;

    heading = g_strdup_printf("Stop %u idle agent%s?", ids->len,
                              ids->len == 1 ? "" : "s");
    confirm = ADW_ALERT_DIALOG(adw_alert_dialog_new(heading,
        "Each one's process and computer stop. A stopped agent does nothing "
        "until somebody starts it; its mailbox keeps whatever is sent to it "
        "meanwhile."));
    adw_alert_dialog_add_response(confirm, "cancel", "Leave them");
    adw_alert_dialog_add_response(confirm, "stop", "Stop them");
    adw_alert_dialog_set_response_appearance(confirm, "stop", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(confirm, "cancel");
    adw_alert_dialog_set_close_response(confirm, "cancel");

    copy = g_ptr_array_new_with_free_func(g_free);
    for (i = 0; i < ids->len; i++)
        g_ptr_array_add(copy, g_strdup(g_ptr_array_index(ids, i)));
    g_object_set_data_full(G_OBJECT(confirm), "agent-ids", copy,
                           (GDestroyNotify)g_ptr_array_unref);
    g_signal_connect(confirm, "response", G_CALLBACK(on_stop_idle_confirmed), self);
    adw_dialog_present(ADW_DIALOG(confirm), GTK_WIDGET(self));
}

static GtkWidget *
agent_row(ClawtWindow *self, JsonObject *agent, gint64 total, gint64 now,
          GPtrArray *stoppable)
{
    GtkWidget *row = adw_action_row_new();
    const gchar *id = clawt_json_string(agent, "id", NULL);
    const gchar *name = clawt_json_string(agent, "name", id);
    const gchar *idle = clawt_json_string(agent, "idle", NULL);
    const gchar *tone = clawt_json_string(agent, "idle_tone", "neutral");
    const gchar *state = clawt_json_string(agent, "state", NULL);
    gint64 cost = clawt_json_int(agent, "cost_micros", 0);
    g_autofree gchar *subtitle = clawt_usage_fleet_row_subtitle(agent, now);
    g_autofree gchar *money = clawt_usage_format_cost(cost);
    g_autofree gchar *share = clawt_usage_share_label(cost, total);
    g_autofree gchar *cost_text = g_strdup_printf("%s \xc2\xb7 %s", money, share);
    GtkWidget *value;

    clawt_gtk_set_row_text(row, name != NULL ? name : "?", subtitle);

    if (idle != NULL)
        adw_action_row_add_prefix(ADW_ACTION_ROW(row),
            clawt_gtk_badge(idle, clawt_gtk_tone_class(tone),
                            "How long since this agent last took a turn"));

    /* the state in words when it is not running: the badge is recency */
    if (state != NULL && g_strcmp0(state, "running") != 0)
        adw_action_row_add_suffix(ADW_ACTION_ROW(row),
            clawt_gtk_badge(state, "dim-label", NULL));

    value = gtk_label_new(cost_text);
    gtk_widget_add_css_class(value, "heading");
    gtk_widget_set_valign(value, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), value);

    if (id != NULL && clawt_usage_fleet_row_stoppable(agent)) {
        GtkWidget *stop = gtk_button_new_with_label("Stop");

        gtk_widget_set_valign(stop, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(stop, "flat");
        g_object_set_data_full(G_OBJECT(stop), "agent-id", g_strdup(id), g_free);
        g_signal_connect(stop, "clicked", G_CALLBACK(on_fleet_stop), self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), stop);
        g_ptr_array_add(stoppable, g_strdup(id));
    }

    return row;
}

static void
refresh_fleet_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) stoppable = g_ptr_array_new_with_free_func(g_free);
    JsonObject *root;
    JsonArray *agents;
    guint period = adw_combo_row_get_selected(ADW_COMBO_ROW(self->fleet_period));
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 total;
    guint i;

    clawt_gtk_clear_list(self->fleet_list);

    reply = clawt_window_request(self, "usage.summary",
        clawt_usage_fleet_payload(clawt_usage_period_since(period, now), STRIP_DAYS));
    if (reply == NULL) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "The fleet's figures are unavailable",
                               "Check the daemon connection, then reopen Fleet to retry.");
        gtk_list_box_append(self->fleet_list, row);
        return;
    }

    root = clawt_payload_of(reply);
    now = clawt_json_int(root, "now", now);
    total = clawt_json_int(json_object_get_object_member(root, "total"),
                           "cost_micros", 0);

    {
        g_autofree gchar *summary = clawt_usage_fleet_summary(root);
        g_autofree gchar *budget = clawt_usage_fleet_budget_line(root);
        g_autofree gchar *strip = clawt_usage_fleet_strip(root);

        gtk_label_set_text(GTK_LABEL(self->fleet_summary), summary);
        gtk_label_set_text(GTK_LABEL(self->fleet_strip), strip);
        gtk_label_set_text(GTK_LABEL(self->fleet_budget), budget != NULL ? budget : "");
        gtk_widget_set_visible(self->fleet_budget, budget != NULL);
    }

    agents = json_object_get_array_member(root, "agents");
    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++)
        gtk_list_box_append(self->fleet_list,
            agent_row(self, json_array_get_object_element(agents, i), total, now,
                      stoppable));

    {
        g_autofree gchar *label = g_strdup_printf("Stop the idle ones (%u)",
                                                  stoppable->len);

        gtk_button_set_label(GTK_BUTTON(self->fleet_stop_idle), label);
        gtk_widget_set_sensitive(self->fleet_stop_idle, stoppable->len > 0);
        g_object_set_data_full(G_OBJECT(self->fleet_stop_idle), "agent-ids",
                               g_ptr_array_ref(stoppable),
                               (GDestroyNotify)g_ptr_array_unref);
    }
}

void
clawt_gtk_refresh_fleet(ClawtWindow *self)
{
    if (self->fleet_list == NULL ||
        !clawt_gtk_refresh_enter(self, CLAWT_REFRESH_FLEET))
        return;

    do {
        refresh_fleet_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_FLEET));
}

static void
on_period_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    clawt_gtk_refresh_fleet(user_data);
}

GtkWidget *
clawt_gtk_build_fleet_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *period_list = gtk_list_box_new();
    GtkWidget *caption;
    g_autoptr(GtkStringList) periods = gtk_string_list_new(NULL);
    guint i;

    for (i = 0; i < clawt_usage_period_count(); i++)
        gtk_string_list_append(periods, clawt_usage_period_label(i));

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    self->fleet_period = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->fleet_period), "Period");
    adw_combo_row_set_model(ADW_COMBO_ROW(self->fleet_period), G_LIST_MODEL(periods));
    adw_combo_row_set_selected(ADW_COMBO_ROW(self->fleet_period),
                               CLAWT_USAGE_PERIOD_DEFAULT);
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(period_list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(period_list, "boxed-list");
    gtk_list_box_append(GTK_LIST_BOX(period_list), self->fleet_period);
    gtk_box_append(GTK_BOX(box), period_list);

    self->fleet_summary = gtk_label_new("");
    gtk_widget_add_css_class(self->fleet_summary, "title-2");
    gtk_label_set_xalign(GTK_LABEL(self->fleet_summary), 0.0f);
    self->fleet_budget = gtk_label_new("");
    gtk_widget_add_css_class(self->fleet_budget, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(self->fleet_budget), 0.0f);
    caption = gtk_label_new("The last 14 days");
    gtk_widget_add_css_class(caption, "caption");
    gtk_label_set_xalign(GTK_LABEL(caption), 0.0f);
    /* a gap is a day nothing ran */
    self->fleet_strip = gtk_label_new("");
    gtk_widget_add_css_class(self->fleet_strip, "title-3");
    gtk_label_set_xalign(GTK_LABEL(self->fleet_strip), 0.0f);
    gtk_box_append(GTK_BOX(header), self->fleet_summary);
    gtk_box_append(GTK_BOX(header), self->fleet_budget);
    gtk_box_append(GTK_BOX(header), caption);
    gtk_box_append(GTK_BOX(header), self->fleet_strip);
    gtk_box_append(GTK_BOX(box), header);

    self->fleet_stop_idle = gtk_button_new_with_label("Stop the idle ones");
    gtk_widget_add_css_class(self->fleet_stop_idle, "destructive-action");
    gtk_widget_set_halign(self->fleet_stop_idle, GTK_ALIGN_START);
    g_signal_connect(self->fleet_stop_idle, "clicked", G_CALLBACK(on_stop_idle), self);
    gtk_box_append(GTK_BOX(box), self->fleet_stop_idle);

    self->fleet_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->fleet_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->fleet_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->fleet_list), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->fleet_list));

    /* connected after the initial selection, so building is not a change */
    g_signal_connect(self->fleet_period, "notify::selected",
                     G_CALLBACK(on_period_changed), self);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    /* a wrapping label's natural width is unwrapped: never scroll sideways */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    return scroll;
}
