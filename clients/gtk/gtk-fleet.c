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
 *
 * Above the spend, the plan allowances (usage.providers): one card per
 * login with a meter per window -- what is *left*, coloured by
 * clawt_usage_window_level(), with when it resets.  Spend answers "what
 * did it cost", the allowance answers "how long can it keep going", and
 * on a subscription the second is the one that stops the fleet.
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
    GtkWidget *fleet_fallback;

    clawt_gtk_set_row_text(row, name != NULL ? name : "?", subtitle);

    if (idle != NULL)
        adw_action_row_add_prefix(ADW_ACTION_ROW(row),
            clawt_gtk_badge(idle, clawt_gtk_tone_class(tone),
                            "How long since this agent last took a turn"));

    /*
     * On one of its fallbacks: what it costs is now another backend's
     * bill, which is the question this page is read to answer.
     * usage.summary carries `failover` as agent.list does.
     */
    fleet_fallback = clawt_gtk_failover_badge(agent, FALSE);
    if (fleet_fallback != NULL)
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), fleet_fallback);

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

/* One allowance window: label, a meter of what is left, the figure, the reset. */
static GtkWidget *
limit_window_row(JsonObject *window, gint64 now)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *label = gtk_label_new(clawt_json_string(window, "label", "?"));
    GtkWidget *meter = gtk_progress_bar_new();
    GtkWidget *figures = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_autofree gchar *value = clawt_usage_window_value_label(window);
    g_autofree gchar *reset = clawt_usage_window_reset_label(window, now);
    GtkWidget *value_label = gtk_label_new(value);
    gdouble left = 0.0;
    gboolean balance = g_strcmp0(clawt_json_string(window, "kind", "meter"), "balance") == 0;
    ClawtAllowanceLevel level = clawt_usage_window_level(window);

    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_width_chars(GTK_LABEL(label), 16);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), label);

    /* filled for what is left, so a full bar is good news */
    gtk_widget_set_hexpand(meter, TRUE);
    gtk_widget_set_valign(meter, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(meter, "clawt-meter");
    gtk_widget_add_css_class(meter,
        clawt_gtk_tone_class(clawt_allowance_level_tone(level)));
    if (!balance && clawt_usage_window_remaining(window, &left))
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(meter), left / 100.0);
    else
        /* no direction, or a balance: an empty bar would read as "used up" */
        gtk_widget_set_opacity(meter, 0.0);
    gtk_box_append(GTK_BOX(box), meter);

    gtk_widget_add_css_class(value_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(value_label), 1.0f);
    gtk_box_append(GTK_BOX(figures), value_label);
    if (reset != NULL) {
        GtkWidget *reset_label = gtk_label_new(reset);

        gtk_widget_add_css_class(reset_label, "caption");
        gtk_widget_add_css_class(reset_label, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(reset_label), 1.0f);
        gtk_box_append(GTK_BOX(figures), reset_label);
    }
    gtk_widget_set_size_request(figures, 170, -1);
    gtk_widget_set_valign(figures, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(box), figures);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    return row;
}

/* One login: a card titled with the CLI and account, its windows inside. */
static GtkWidget *
limit_login_card(JsonObject *login, gint64 now)
{
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    g_autofree gchar *title = clawt_usage_login_title(login);
    g_autofree gchar *status = clawt_usage_login_status_label(login, now);
    GtkWidget *title_label = gtk_label_new(title);
    GtkWidget *status_label = gtk_label_new(status);
    JsonArray *windows = json_object_has_member(login, "windows")
        ? json_object_get_array_member(login, "windows") : NULL;
    ClawtAllowanceLevel level = clawt_usage_login_level(login);
    guint i;

    gtk_widget_add_css_class(title_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(title_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(title_label), TRUE);
    gtk_widget_add_css_class(status_label, "dim-label");
    gtk_widget_add_css_class(status_label, "caption");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(status_label), TRUE);
    gtk_box_append(GTK_BOX(titles), title_label);
    gtk_box_append(GTK_BOX(titles), status_label);
    gtk_widget_set_hexpand(titles, TRUE);
    gtk_box_append(GTK_BOX(head), titles);

    if (level == CLAWT_ALLOWANCE_LOW || level == CLAWT_ALLOWANCE_CRITICAL)
        gtk_box_append(GTK_BOX(head),
            clawt_gtk_badge(level == CLAWT_ALLOWANCE_CRITICAL ? "nearly out" : "running low",
                            clawt_gtk_tone_class(clawt_allowance_level_tone(level)),
                            "The most used of this login's windows"));
    gtk_box_append(GTK_BOX(card), head);

    if (windows != NULL && json_array_get_length(windows) > 0) {
        GtkWidget *list = gtk_list_box_new();

        gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
        gtk_widget_add_css_class(list, "boxed-list");
        for (i = 0; i < json_array_get_length(windows); i++)
            gtk_list_box_append(GTK_LIST_BOX(list),
                limit_window_row(json_array_get_object_element(windows, i), now));
        gtk_box_append(GTK_BOX(card), list);
    }
    return card;
}

static void
refresh_limits_once(ClawtWindow *self, gboolean ask)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonArray *logins;
    GtkWidget *child;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    while ((child = gtk_widget_get_first_child(self->fleet_limits)) != NULL)
        gtk_box_remove(GTK_BOX(self->fleet_limits), child);

    reply = clawt_window_request(self, "usage.providers",
        clawt_build_payload("refresh", ask ? "true" : "false", NULL));
    if (reply == NULL)
        return;

    root = clawt_payload_of(reply);
    now = clawt_json_int(root, "now", now);
    logins = json_object_has_member(root, "logins")
        ? json_object_get_array_member(root, "logins") : NULL;

    if (logins == NULL || json_array_get_length(logins) == 0) {
        GtkWidget *none = gtk_label_new("No agent runs a CLI provider, so there is no "
                                        "plan allowance to show.");

        gtk_widget_add_css_class(none, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(none), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(none), TRUE);
        gtk_box_append(GTK_BOX(self->fleet_limits), none);
        return;
    }

    for (i = 0; i < json_array_get_length(logins); i++)
        gtk_box_append(GTK_BOX(self->fleet_limits),
            limit_login_card(json_array_get_object_element(logins, i), now));
}

void
clawt_gtk_refresh_limits(ClawtWindow *self, gboolean ask)
{
    if (self->fleet_limits == NULL)
        return;
    refresh_limits_once(self, ask);
}

static void
on_limits_refresh(GtkButton *button, gpointer user_data)
{
    clawt_gtk_refresh_limits(user_data, TRUE);
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

    /*
     * The daemon's fleet warnings ride on team.list, as on the settings
     * Teams page.  Cleared first so they do not pile up; the list hides
     * itself when empty so no empty box is drawn.  A failed team.list
     * only leaves the warnings off -- the figures below are the page.
     */
    {
        g_autoptr(JsonNode) teams = NULL;

        clawt_gtk_clear_list(self->fleet_warnings);
        teams = clawt_window_request(self, "team.list", NULL);
        if (teams != NULL)
            clawt_gtk_append_warning_rows(self->fleet_warnings, teams);
        gtk_widget_set_visible(GTK_WIDGET(self->fleet_warnings),
            gtk_widget_get_first_child(GTK_WIDGET(self->fleet_warnings)) != NULL);
    }

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

    /* the plan allowances, before the spend: they say how long it can go on */
    {
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *heading = gtk_label_new("Plan allowances");
        GtkWidget *refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");

        gtk_widget_add_css_class(heading, "title-4");
        gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
        gtk_widget_set_hexpand(heading, TRUE);
        gtk_widget_add_css_class(refresh, "flat");
        gtk_widget_set_tooltip_text(refresh, "Read every login again");
        g_signal_connect(refresh, "clicked", G_CALLBACK(on_limits_refresh), self);
        gtk_box_append(GTK_BOX(row), heading);
        gtk_box_append(GTK_BOX(row), refresh);
        gtk_box_append(GTK_BOX(box), row);

        self->fleet_limits = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
        gtk_box_append(GTK_BOX(box), self->fleet_limits);
    }

    self->fleet_period = clawt_gtk_combo_row_new();
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

    /* the daemon's warnings, above the list; shown only when there are some */
    self->fleet_warnings = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->fleet_warnings, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->fleet_warnings), "boxed-list");
    gtk_widget_set_visible(GTK_WIDGET(self->fleet_warnings), FALSE);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->fleet_warnings));

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
