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

static void
refresh_tasks_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *tasks;
    guint i;

    clawt_gtk_clear_list(self->task_list);

    reply = clawt_window_request(self, "task.list", NULL);

    if (reply == NULL)
        return;

    tasks = json_object_get_array_member(clawt_payload_of(reply), "tasks");

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

    self->task_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->task_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->task_list), "boxed-list");
    gtk_widget_set_margin_top(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->task_list), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->task_list));

    return scroll;
}
