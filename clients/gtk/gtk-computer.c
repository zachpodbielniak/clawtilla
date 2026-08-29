/*
 * gtk-computer.c - The computer page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The machine an agent has, and a command run inside it.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

#include <string.h>

/* ── Computer ────────────────────────────────────────────────────── */

static void
on_exec(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    GtkTextBuffer *buffer;
    GtkTextIter end;
    const gchar *command;

    (void)widget;

    if (self->selected_agent == NULL)
        return;

    command = gtk_editable_get_text(GTK_EDITABLE(self->exec_entry));

    if (command == NULL || *command == '\0')
        return;

    buffer = gtk_text_view_get_buffer(self->exec_output);
    gtk_text_buffer_get_end_iter(buffer, &end);

    {
        g_autofree gchar *echo = g_strdup_printf("$ %s\n", command);

        gtk_text_buffer_insert(buffer, &end, echo, -1);
    }

    reply = clawt_window_request(
        self, "computer.exec",
        clawt_build_payload("agent", self->selected_agent, "command",
                            command, NULL));

    if (reply == NULL)
        return;

    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end,
                           clawt_json_string(clawt_payload_of(reply),
                                             "stdout", ""), -1);

    /*
     * stderr is shown too.  A console that swallowed it would leave a
     * failing command looking like one that produced nothing.
     */
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end,
                           clawt_json_string(clawt_payload_of(reply),
                                             "stderr", ""), -1);

    gtk_editable_set_text(GTK_EDITABLE(self->exec_entry), "");
}

void
clawt_gtk_refresh_computer(ClawtWindow *self, JsonObject *agent)
{
    const gchar *caps = clawt_json_string(agent, "caps", "");
    gboolean has_computer = strstr(caps, "computer") != NULL;

    gtk_widget_set_sensitive(GTK_WIDGET(self->exec_entry), has_computer);

    gtk_label_set_text(self->computer_state,
                       has_computer
                           ? clawt_json_string(agent, "computer", "none")
                           : "This agent has no computer.");
}

GtkWidget *
clawt_gtk_build_computer_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *run;
    GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    self->computer_state = GTK_LABEL(gtk_label_new("No agent selected."));
    gtk_widget_set_margin_top(GTK_WIDGET(self->computer_state), 12);

    self->exec_output = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(self->exec_output, FALSE);
    gtk_text_view_set_monospace(self->exec_output, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->exec_output));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_margin_start(scroll, 12);
    gtk_widget_set_margin_end(scroll, 12);

    self->exec_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->exec_entry, "Command");
    gtk_widget_set_hexpand(GTK_WIDGET(self->exec_entry), TRUE);
    g_signal_connect(self->exec_entry, "activate", G_CALLBACK(on_exec), self);

    run = gtk_button_new_with_label("Run");
    g_signal_connect(run, "clicked", G_CALLBACK(on_exec), self);

    gtk_box_append(GTK_BOX(entry_box), GTK_WIDGET(self->exec_entry));
    gtk_box_append(GTK_BOX(entry_box), run);
    gtk_widget_set_margin_start(entry_box, 12);
    gtk_widget_set_margin_end(entry_box, 12);
    gtk_widget_set_margin_bottom(entry_box, 12);

    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->computer_state));
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), entry_box);

    return box;
}
