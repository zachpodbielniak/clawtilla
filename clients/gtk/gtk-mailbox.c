/*
 * gtk-mailbox.c - The mailbox page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * What is queued for an agent that is not running.  Empty while it is,
 * because delivery acknowledges at the socket -- which the page says
 * rather than leaving the emptiness to be read as an answer.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Mailbox ─────────────────────────────────────────────────────── */

static void
on_mailbox_item_action(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *item_id = g_object_get_data(G_OBJECT(button), "item-id");
    const gchar *kind = g_object_get_data(G_OBJECT(button), "kind");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(
        self, kind,
        clawt_build_payload("agent", self->selected_agent, "item", item_id,
                            NULL));

    if (reply != NULL)
        clawt_gtk_refresh_selected(self);
}

/*
 * Every expired item, across every mailbox.
 *
 * Fleet-wide rather than per-agent because that is what the daemon
 * offers -- a sweep is a sweep -- so the button says so.
 */
static void
on_mailbox_purge(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    /*
     * Per-agent: the daemon resolves a mailbox from the payload and
     * refuses without one. A fleet-wide sweep is not a thing it offers.
     */
    reply = clawt_window_request(
        self, "mailbox.purge",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    {
        g_autofree gchar *said = g_strdup_printf(
            "Purged %" G_GINT64_FORMAT " expired item(s).",
            json_object_get_int_member(clawt_payload_of(reply), "purged"));

        clawt_window_toast(self, said);
    }

    clawt_gtk_refresh_selected(self);
}

/*
 * Adds one mailbox row.
 *
 * Shared by the waiting list and the dead-letter list, which differ only
 * in whether requeueing is offered -- an item that has not run out of
 * attempts has nothing to be put back into.
 */
static void
add_mailbox_row(ClawtWindow *self, GtkListBox *list, JsonObject *item,
                gboolean dead)
{
    GtkWidget *row = adw_action_row_new();
    GtkWidget *ack = gtk_button_new_with_label("Ack");
    g_autofree gchar *title = NULL;

    title = g_strdup_printf("from %s", clawt_json_string(item, "from", "?"));
    clawt_gtk_set_row_text(row, title, clawt_json_string(item, "body", ""));

    if (clawt_json_string(item, "last_error", NULL) != NULL) {
        GtkWidget *warn = clawt_gtk_badge("failed", "error",
                                          clawt_json_string(item, "last_error", ""));

        adw_action_row_add_prefix(ADW_ACTION_ROW(row), warn);
    }

    if (dead) {
        GtkWidget *requeue = gtk_button_new_with_label("Requeue");

        g_object_set_data_full(G_OBJECT(requeue), "item-id",
                               g_strdup(clawt_json_string(item, "id", "")),
                               g_free);
        g_object_set_data(G_OBJECT(requeue), "kind", "mailbox.requeue");
        g_signal_connect(requeue, "clicked",
                         G_CALLBACK(on_mailbox_item_action), self);
        gtk_widget_set_valign(requeue, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), requeue);
    }

    g_object_set_data_full(G_OBJECT(ack), "item-id",
                           g_strdup(clawt_json_string(item, "id", "")),
                           g_free);
    g_object_set_data(G_OBJECT(ack), "kind", "mailbox.ack");
    g_signal_connect(ack, "clicked", G_CALLBACK(on_mailbox_item_action),
                     self);
    gtk_widget_set_valign(ack, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), ack);

    gtk_list_box_append(list, row);
}

static void
refresh_mailbox_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *items;
    guint i;

    clawt_gtk_clear_list(self->mailbox_list);

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "mailbox.list",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    items = json_object_get_array_member(clawt_payload_of(reply), "items");

    {
        gint64 depth = json_object_get_int_member(clawt_payload_of(reply),
                                                  "depth");
        g_autofree gchar *summary = g_strdup_printf(
            "%" G_GINT64_FORMAT " message(s) waiting", depth);

        gtk_label_set_text(self->mailbox_summary, summary);
    }

    for (i = 0; i < json_array_get_length(items); i++)
        add_mailbox_row(self, self->mailbox_list,
                        json_array_get_object_element(items, i), FALSE);

    /*
     * Dead letters in the same list, after the waiting ones.  Nothing is
     * dropped silently, so an item that ran out of attempts has to be
     * somewhere a person can see it and put it back.
     */
    {
        g_autoptr(JsonNode) dead = clawt_window_request(
            self, "mailbox.dead",
            clawt_build_payload("agent", self->selected_agent, NULL));

        if (dead != NULL) {
            JsonArray *letters = json_object_get_array_member(
                clawt_payload_of(dead), "items");

            for (i = 0; i < json_array_get_length(letters); i++)
                add_mailbox_row(self, self->mailbox_list,
                                json_array_get_object_element(letters, i),
                                TRUE);
        }
    }
}

void
clawt_gtk_refresh_mailbox(ClawtWindow *self)
{
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_MAILBOX))
        return;

    do {
        refresh_mailbox_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_MAILBOX));
}

GtkWidget *
clawt_gtk_build_mailbox_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->mailbox_summary = GTK_LABEL(gtk_label_new("No agent selected."));
    gtk_widget_set_margin_top(GTK_WIDGET(self->mailbox_summary), 12);

    {
        GtkWidget *purge = gtk_button_new_with_label("Purge expired items");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        gtk_widget_set_tooltip_text(
            purge, "Removes every item past its time-to-live from this "
                   "agent's mailbox.");
        g_signal_connect(purge, "clicked", G_CALLBACK(on_mailbox_purge),
                         self);
        gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(row, 6);
        gtk_box_append(GTK_BOX(row), purge);
        gtk_box_append(GTK_BOX(box), row);
    }

    self->mailbox_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->mailbox_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->mailbox_list), "boxed-list");
    gtk_widget_set_margin_start(GTK_WIDGET(self->mailbox_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->mailbox_list), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->mailbox_list), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->mailbox_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->mailbox_summary));
    gtk_box_append(GTK_BOX(box), scroll);

    return box;
}
