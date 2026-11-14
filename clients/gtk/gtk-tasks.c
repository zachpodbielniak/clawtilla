/*
 * gtk-tasks.c - The tasks page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What has been delegated, to whom, and whether it is still going.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Tasks ───────────────────────────────────────────────────────── */

/*
 * Cancels a task that is still going.
 *
 * Offered only while it is, because cancelling a finished one is not a
 * refusal the daemon needs to explain -- it is a button that should not
 * have been there.
 */
static void
on_task_cancel(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *task_id = g_object_get_data(G_OBJECT(button), "task-id");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(self, "task.cancel",
                                 clawt_build_payload("task", task_id, NULL));

    if (reply != NULL)
        clawt_gtk_refresh_tasks(self);
}

/*
 * Sends a failed or stalled run round again -- a task re-queued for its
 * assignee, or a routine run again.  The button carries whichever id its
 * row had: a task row the task's, an incident row the incident's, so a
 * routine that never made a task can still be retried.
 */
static void
on_task_retry(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *key = g_object_get_data(G_OBJECT(button), "retry-key");
    const gchar *id = g_object_get_data(G_OBJECT(button), "retry-id");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(self, "task.retry",
                                 clawt_build_payload(key, id, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self,
                       clawt_json_boolean(clawt_payload_of(reply), "rerun",
                                          FALSE)
                       ? "Running the routine again"
                       : "Queued for its assignee again");
    clawt_gtk_refresh_tasks(self);
}

static GtkWidget *
retry_button(ClawtWindow *self, const gchar *key, const gchar *id)
{
    GtkWidget *retry = gtk_button_new_with_label("Retry");

    g_object_set_data(G_OBJECT(retry), "retry-key", (gpointer)key);
    g_object_set_data_full(G_OBJECT(retry), "retry-id", g_strdup(id), g_free);
    g_signal_connect(retry, "clicked", G_CALLBACK(on_task_retry), self);
    gtk_widget_set_valign(retry, GTK_ALIGN_CENTER);

    return retry;
}

/* Saves one incident setting; the refresh that follows redraws both. */
static void
configure_incidents(ClawtWindow *self, const gchar *member, JsonNode *value)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, member);
    json_builder_add_value(builder, value);
    json_builder_end_object(builder);

    reply = clawt_window_request(self, "incident.configure",
                                 json_builder_get_root(builder));

    if (reply != NULL)
        clawt_window_toast(self, "Saved");
}

static void
on_incidents_enabled(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    JsonNode *value = json_node_new(JSON_NODE_VALUE);

    (void)pspec;
    json_node_set_boolean(value,
                          adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
    configure_incidents(user_data, "enabled", value);
}

static void
on_incidents_to(GObject *row, GParamSpec *pspec, gpointer user_data)
{
    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
    JsonNode *value = json_node_new(JSON_NODE_VALUE);

    (void)pspec;

    if (selected >= clawt_incident_recipient_count()) {
        json_node_unref(value);
        return;
    }

    json_node_set_string(value, clawt_incident_recipient_nick(
                                    clawt_incident_recipient_nth(selected)));
    configure_incidents(user_data, "to", value);
}

/*
 * What became of an incident's report, in one line: who has it, who it
 * is waiting for, or why nobody was given it.
 */
static gchar *
incident_status(JsonObject *incident)
{
    const gchar *recipient = clawt_json_string(incident, "recipient", NULL);
    const gchar *note = clawt_json_string(incident, "note", NULL);
    gint64 occurrences = clawt_json_int(incident, "occurrences", 1);
    const gchar *retried_by = clawt_json_string(incident, "retried_by", NULL);
    g_autoptr(GString) out = g_string_new(NULL);

    if (note != NULL)
        g_string_append_printf(out, "Recorded for you: %s", note);
    else if (clawt_json_boolean(incident, "queued", FALSE) && recipient != NULL)
        g_string_append_printf(out, "Waiting for %s to finish a turn",
                               recipient);
    else if (recipient != NULL)
        g_string_append_printf(out, "Reported to %s", recipient);
    else
        g_string_append(out, "Not reported yet");

    if (occurrences > 1)
        g_string_append_printf(out, " \302\267 broke %" G_GINT64_FORMAT
                               " times", occurrences);

    if (retried_by != NULL)
        g_string_append_printf(out, " \302\267 last retried by %s",
                               g_strcmp0(retried_by, "user") == 0
                               ? "you" : retried_by);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * The "Problems to look at" group: who hears about broken runs, and
 * each open incident.  The settings sit with the incidents rather than
 * on a preferences page, because the question "who was told about
 * this" is asked while looking at one.
 */
static void
refresh_incidents_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonArray *incidents;
    GtkWidget *enabled;
    GtkWidget *to;
    g_autoptr(GtkStringList) labels = gtk_string_list_new(NULL);
    const gchar *current;
    guint i;

    clawt_gtk_clear_list(self->incident_list);

    reply = clawt_window_request(self, "incident.list", NULL);

    if (reply == NULL)
        return;

    root = clawt_payload_of(reply);
    current = clawt_json_string(root, "to", "lead");

    /*
     * Built with their values before the handlers are connected, so
     * drawing the current setting does not save it straight back.
     */
    enabled = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(enabled),
                                  "Report broken runs");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(enabled),
                                "Failed, stalled and unstartable runs, and "
                                "routines that did not run");
    adw_switch_row_set_active(ADW_SWITCH_ROW(enabled),
                              clawt_json_boolean(root, "enabled", TRUE));
    g_signal_connect(enabled, "notify::active",
                     G_CALLBACK(on_incidents_enabled), self);
    gtk_list_box_append(self->incident_list, enabled);

    /* Walked from the library, never named here. */
    to = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(to),
                                  "Who hears about them");

    for (i = 0; i < clawt_incident_recipient_count(); i++)
        gtk_string_list_append(labels, clawt_incident_recipient_label(
                                           clawt_incident_recipient_nth(i)));

    adw_combo_row_set_model(ADW_COMBO_ROW(to), G_LIST_MODEL(labels));

    for (i = 0; i < clawt_incident_recipient_count(); i++) {
        if (g_strcmp0(clawt_incident_recipient_nick(
                          clawt_incident_recipient_nth(i)), current) == 0)
            adw_combo_row_set_selected(ADW_COMBO_ROW(to), i);
    }

    g_signal_connect(to, "notify::selected", G_CALLBACK(on_incidents_to),
                     self);
    gtk_list_box_append(self->incident_list, to);

    incidents = json_object_get_array_member(root, "incidents");

    if (incidents == NULL || json_array_get_length(incidents) == 0) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "Nothing has broken",
                               "A run that fails, stalls or cannot start is "
                               "reported to its team's lead and listed here. "
                               "Each team's reports are in its Problems to "
                               "look at room.");
        gtk_list_box_append(self->incident_list, row);
        return;
    }

    for (i = 0; i < json_array_get_length(incidents); i++) {
        JsonObject *incident = json_array_get_object_element(incidents, i);
        GtkWidget *row = adw_action_row_new();
        g_autofree gchar *title = NULL;
        g_autofree gchar *status = incident_status(incident);
        g_autofree gchar *subtitle = NULL;
        const gchar *reason = clawt_json_string(incident, "reason", NULL);

        title = g_strdup_printf("%s's run %s",
                                clawt_json_string(incident, "agent", "?"),
                                clawt_json_string(incident, "kind_label",
                                                  "broke"));
        subtitle = (reason != NULL)
                   ? g_strdup_printf("%s\n%s", reason, status)
                   : g_strdup(status);

        clawt_gtk_set_row_text(row, title, subtitle);
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 3);

        adw_action_row_add_suffix(
            ADW_ACTION_ROW(row),
            clawt_gtk_badge(clawt_json_string(incident, "kind", "?"),
                            clawt_gtk_tone_class("bad"),
                            clawt_json_string(incident, "room", "")));

        if (clawt_json_boolean(incident, "retryable", FALSE))
            adw_action_row_add_suffix(
                ADW_ACTION_ROW(row),
                retry_button(self, "incident",
                             clawt_json_string(incident, "id", "")));

        gtk_list_box_append(self->incident_list, row);
    }
}

static void
refresh_tasks_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *tasks;
    guint i;

    refresh_incidents_once(self);

    clawt_gtk_clear_list(self->task_list);

    reply = clawt_window_request(self, "task.list", NULL);

    if (reply == NULL) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "Tasks are unavailable",
                              "Check the daemon connection, then reopen Tasks to retry.");
        gtk_list_box_append(self->task_list, row);
        return;
    }

    tasks = json_object_get_array_member(clawt_payload_of(reply), "tasks");

    if (json_array_get_length(tasks) == 0) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "No tasks yet",
                              "Ask an agent to delegate work. Its progress and result will appear here.");
        gtk_list_box_append(self->task_list, row);
    }

    for (i = 0; i < json_array_get_length(tasks); i++) {
        JsonObject *task = json_array_get_object_element(tasks, i);
        GtkWidget *row = adw_action_row_new();
        g_autofree gchar *title = NULL;

        title = g_strdup_printf("%s → %s",
                                clawt_json_string(task, "origin", "?"),
                                clawt_json_string(task, "assignee", "?"));

        clawt_gtk_set_row_text(row, title, clawt_json_string(task, "prompt", ""));

        {
            const gchar *state = clawt_json_string(task, "state", "");
            gint value = 0;
            const gchar *tone = "neutral";

            /*
             * Coloured by the same rule the web client uses.  Every task
             * badge here was "dim-label" whatever the state, so a failed
             * task and a pending one were the same grey -- the web client
             * at least tried, and got it wrong; this one did not try.
             *
             * A state this build does not know stays neutral rather than
             * refusing: a client one version behind should still draw the
             * row.
             */
            if (clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value))
                tone = clawt_task_state_tone((ClawtTaskState)value);

            adw_action_row_add_suffix(
                ADW_ACTION_ROW(row),
                clawt_gtk_badge(clawt_json_string(task, "state", "?"),
                                clawt_gtk_tone_class(tone),
                                clawt_json_string(task, "reason", "")));

            /*
             * The subtitle carries the freshest thing anybody knows: the
             * assignee's own progress note while the work runs, and the
             * caveat once it has ended without anybody saying so.  A row
             * that reads `completed` and nothing else cannot distinguish
             * a reported result from a turn that simply stopped.
             */
            if (clawt_json_string(task, "progress_note", NULL) != NULL)
                adw_action_row_set_subtitle(
                    ADW_ACTION_ROW(row),
                    clawt_json_string(task, "progress_note", ""));
            else if (clawt_json_boolean(task, "result_inferred", FALSE))
                adw_action_row_set_subtitle(
                    ADW_ACTION_ROW(row),
                    "nobody reported this finished -- it is the last thing "
                    "the assignee wrote");

            if (g_strcmp0(state, "running") == 0 ||
                g_strcmp0(state, "pending") == 0) {
                GtkWidget *cancel = gtk_button_new_with_label("Cancel");

                g_object_set_data_full(
                    G_OBJECT(cancel), "task-id",
                    g_strdup(clawt_json_string(task, "id", "")), g_free);
                g_signal_connect(cancel, "clicked",
                                 G_CALLBACK(on_task_cancel), self);
                gtk_widget_set_valign(cancel, GTK_ALIGN_CENTER);
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), cancel);
            }

            /*
             * Retry where the daemon would accept it -- the library's
             * rule, so the button and the refusal cannot disagree.
             */
            if (clawt_enum_from_nick(CLAWT_TYPE_TASK_STATE, state, &value) &&
                clawt_task_state_is_retryable((ClawtTaskState)value))
                adw_action_row_add_suffix(
                    ADW_ACTION_ROW(row),
                    retry_button(self, "task",
                                 clawt_json_string(task, "id", "")));
        }

        gtk_list_box_append(self->task_list, row);
    }
}

void
clawt_gtk_refresh_tasks(ClawtWindow *self)
{
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_TASKS))
        return;

    do {
        refresh_tasks_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_TASKS));
}

GtkWidget *
clawt_gtk_build_task_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *column = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *heading = gtk_label_new("Problems to look at");

    gtk_widget_add_css_class(heading, "heading");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_widget_set_margin_top(heading, 12);
    gtk_widget_set_margin_start(heading, 12);
    gtk_box_append(GTK_BOX(column), heading);

    self->incident_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->incident_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->incident_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->incident_list), GTK_ALIGN_START);
    gtk_widget_set_margin_start(GTK_WIDGET(self->incident_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->incident_list), 12);
    gtk_box_append(GTK_BOX(column), GTK_WIDGET(self->incident_list));

    self->task_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->task_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->task_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->task_list), GTK_ALIGN_START);
    gtk_widget_set_margin_top(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->task_list), 12);

    gtk_box_append(GTK_BOX(column), GTK_WIDGET(self->task_list));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), column);
    /* Constrain long prompts to the viewport so they wrap instead of
     * making their action buttons disappear beyond the right edge. */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);

    return scroll;
}
