/*
 * gtk-activity.c - The activity page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every turn running across the fleet, from activity.list: who, where,
 * for how long, on whose behalf, and the last few things it did. Two
 * actions, each labelled with what it actually does. Stop interrupts the
 * whole agent -- agent.interrupt has no narrower form -- so it says so
 * and asks first. Steer sends a correction into the running turn through
 * msg.send, which reaches only the agent's conversation with you, so it
 * is offered only on that row. The web client's page is built from the
 * same reply and the same library rules.
 */

#include "clawt-window-private.h"

/* ── Stop ────────────────────────────────────────────────────────── */

static void
on_stop_confirmed(AdwAlertDialog *dialog, gchar *response, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *agent_id = g_object_get_data(G_OBJECT(dialog), "agent-id");
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "stop") != 0)
        return;

    reply = clawt_window_request(self, "agent.interrupt",
                                 clawt_build_payload("agent", agent_id, NULL));
    if (reply != NULL) {
        gint64 killed = clawt_json_int(clawt_payload_of(reply), "killed", 0);
        g_autofree gchar *text = killed > 0
            ? g_strdup_printf("Stopped %s.", agent_id)
            : g_strdup_printf("%s had nothing running by the time it was asked.",
                              agent_id);

        clawt_window_toast(self, text);
    }

    clawt_gtk_refresh_activity(self);
}

static void
on_stop_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *agent_id = g_object_get_data(G_OBJECT(button), "agent-id");
    guint running = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "running"));
    g_autofree gchar *heading = clawt_activity_stop_prompt(agent_id, running);
    AdwAlertDialog *confirm;

    confirm = ADW_ALERT_DIALOG(adw_alert_dialog_new(heading,
        "Whatever it was doing is abandoned, and anything queued for it "
        "stays queued."));
    adw_alert_dialog_add_response(confirm, "cancel", "Let it run");
    adw_alert_dialog_add_response(confirm, "stop", "Stop it");
    adw_alert_dialog_set_response_appearance(confirm, "stop", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(confirm, "cancel");
    adw_alert_dialog_set_close_response(confirm, "cancel");
    g_object_set_data_full(G_OBJECT(confirm), "agent-id", g_strdup(agent_id), g_free);
    g_signal_connect(confirm, "response", G_CALLBACK(on_stop_confirmed), self);

    adw_dialog_present(ADW_DIALOG(confirm), GTK_WIDGET(self));
}

/* ── Steer ───────────────────────────────────────────────────────── */

/*
 * Sends what was typed into the running turn. msg.send decides: a busy
 * agent holds it as a steer and says `steered`; one that finished in the
 * meantime takes it as an ordinary message, which is said instead, since
 * the two read the text at different moments.
 */
static void
on_steer_activate(GtkEntry *entry, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *agent_id = g_object_get_data(G_OBJECT(entry), "agent-id");
    g_autofree gchar *body = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
    g_autoptr(JsonNode) reply = NULL;

    g_strstrip(body);
    if (*body == '\0')
        return;

    reply = clawt_window_request(self, "msg.send",
                                 clawt_build_payload("target", agent_id,
                                                     "body", body, NULL));
    if (reply == NULL)
        return;

    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    clawt_window_toast(self,
        clawt_json_boolean(clawt_payload_of(reply), "steered", FALSE)
            ? "Steered: it reads this at its next step."
            : "It had finished, so this went as a new message.");
}

/* ── The list ────────────────────────────────────────────────────── */

/*
 * Rows are rebuilt on every step, so an expander somebody opened would
 * snap shut each time the agent did anything -- which is exactly when
 * they are reading it. Which rows are open is kept here, by agent and
 * room, and reapplied.
 */
static gchar *
turn_key(JsonObject *turn)
{
    return g_strdup_printf("%s\x1f%s", clawt_json_string(turn, "agent", ""),
                           clawt_json_string(turn, "room", ""));
}

static void
on_expanded(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *key = g_object_get_data(row, "turn-key");

    (void)pspec;
    if (self->activity_expanded == NULL)
        self->activity_expanded = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                        g_free, NULL);
    if (adw_expander_row_get_expanded(ADW_EXPANDER_ROW(row)))
        g_hash_table_add(self->activity_expanded, g_strdup(key));
    else
        g_hash_table_remove(self->activity_expanded, key);
}

static GtkWidget *
turn_row(ClawtWindow *self, JsonObject *turn, gint64 now)
{
    const gchar *agent_id = clawt_json_string(turn, "agent", "?");
    const gchar *room_name = clawt_json_string(turn, "room_name",
                                               clawt_json_string(turn, "room", ""));
    const gchar *origin = clawt_json_string(turn, "origin", NULL);
    const gchar *task = clawt_json_string(turn, "task", NULL);
    g_autofree gchar *elapsed = clawt_activity_elapsed_label(
        clawt_json_int(turn, "started", 0), now);
    g_autofree gchar *title = NULL;
    g_autoptr(GString) subtitle = g_string_new(NULL);
    GtkWidget *row = adw_expander_row_new();
    JsonArray *steps = json_object_has_member(turn, "steps")
        ? json_object_get_array_member(turn, "steps") : NULL;
    GtkWidget *stop;
    guint i;

    title = g_strdup_printf("%s in %s", clawt_json_string(turn, "agent_name", agent_id),
                            *room_name != '\0' ? room_name : "an unnamed room");
    g_string_append_printf(subtitle, "running %s", elapsed);
    if (origin != NULL && g_strcmp0(origin, "user") != 0)
        g_string_append_printf(subtitle, " · for %s", origin);
    if (task != NULL)
        g_string_append_printf(subtitle, " · task %s", task);
    g_string_append_printf(subtitle, " · %" G_GINT64_FORMAT " step(s)",
                           clawt_json_int(turn, "step_count", 0));

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), subtitle->str);

    stop = gtk_button_new_with_label("Stop agent");
    gtk_widget_add_css_class(stop, "destructive-action");
    gtk_widget_set_valign(stop, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text(stop, "Interrupts every turn this agent is running");
    g_object_set_data_full(G_OBJECT(stop), "agent-id", g_strdup(agent_id), g_free);
    g_object_set_data(G_OBJECT(stop), "running",
                      GUINT_TO_POINTER((guint)clawt_json_int(turn, "running_turns", 1)));
    g_signal_connect(stop, "clicked", G_CALLBACK(on_stop_clicked), self);
    adw_expander_row_add_suffix(ADW_EXPANDER_ROW(row), stop);

    /* The most recent steps, through the same summary the chat draws. */
    for (i = 0; steps != NULL && i < json_array_get_length(steps); i++) {
        g_autoptr(ClawtTurnStep) step = clawt_turn_step_new_from_object(
            json_array_get_object_element(steps, i), agent_id);
        g_autofree gchar *summary = NULL;
        GtkWidget *line;

        if (step == NULL)
            continue;
        summary = clawt_turn_step_summary(step);
        line = adw_action_row_new();
        clawt_gtk_set_row_text(line, summary, NULL);
        gtk_widget_add_css_class(line, clawt_gtk_tone_class(clawt_turn_step_tone(step)));
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), line);
    }

    if (clawt_json_boolean(turn, "steerable", FALSE)) {
        GtkWidget *steer = adw_entry_row_new();

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(steer), "Steer this turn");
        g_object_set_data_full(G_OBJECT(steer), "agent-id", g_strdup(agent_id), g_free);
        g_signal_connect(steer, "entry-activated", G_CALLBACK(on_steer_activate), self);
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), steer);
    } else {
        GtkWidget *note = adw_action_row_new();

        clawt_gtk_set_row_text(note, "Steering reaches only its conversation with you",
                               "This turn is in another room. Message it there, or stop it.");
        adw_expander_row_add_row(ADW_EXPANDER_ROW(row), note);
    }

    {
        g_autofree gchar *key = turn_key(turn);

        if (self->activity_expanded != NULL &&
            g_hash_table_contains(self->activity_expanded, key))
            adw_expander_row_set_expanded(ADW_EXPANDER_ROW(row), TRUE);
        g_object_set_data_full(G_OBJECT(row), "turn-key", g_steal_pointer(&key), g_free);
        g_signal_connect(row, "notify::expanded", G_CALLBACK(on_expanded), self);
    }

    return row;
}

static void
refresh_activity_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *turns;
    gint64 now;
    guint i;

    clawt_gtk_clear_list(self->activity_list);

    reply = clawt_window_request(self, "activity.list", NULL);
    if (reply == NULL) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "Activity is unavailable",
                               "Check the daemon connection, then reopen Activity to retry.");
        gtk_list_box_append(self->activity_list, row);
        return;
    }

    turns = json_object_get_array_member(clawt_payload_of(reply), "turns");
    /* The daemon's clock, so an age is not skewed by this machine's. */
    now = clawt_json_int(clawt_payload_of(reply), "now", g_get_real_time());

    if (turns == NULL || json_array_get_length(turns) == 0) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "Nothing is running",
                               "Every agent is idle. A turn appears here the moment one starts.");
        gtk_list_box_append(self->activity_list, row);
        return;
    }

    for (i = 0; i < json_array_get_length(turns); i++)
        gtk_list_box_append(self->activity_list,
                            turn_row(self, json_array_get_object_element(turns, i), now));
}

void
clawt_gtk_refresh_activity(ClawtWindow *self)
{
    if (self->activity_list == NULL ||
        !clawt_gtk_refresh_enter(self, CLAWT_REFRESH_ACTIVITY))
        return;

    do {
        refresh_activity_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_ACTIVITY));
}

/*
 * Ages move whether or not anything else does: a turn sitting in one
 * long tool call produces no step for minutes, and a "running 12s" that
 * never changes reads as a stale page. Asked only while the page is up.
 */
static gboolean
on_activity_tick(gpointer user_data)
{
    ClawtWindow *self = user_data;

    if (clawt_gtk_current_page(self) == CLAWT_PAGE_ACTIVITY)
        clawt_gtk_refresh_activity(self);

    return G_SOURCE_CONTINUE;
}

GtkWidget *
clawt_gtk_build_activity_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->activity_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->activity_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->activity_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->activity_list), GTK_ALIGN_START);
    gtk_widget_set_margin_top(GTK_WIDGET(self->activity_list), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->activity_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->activity_list), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->activity_list));
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    if (self->activity_tick == 0)
        self->activity_tick = g_timeout_add_seconds(10, on_activity_tick, self);

    return scroll;
}
