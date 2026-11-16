/*
 * gtk-prefs-history.c - Settings: history
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every kept version of the config, the persona files and the skills
 * (since 0.41.0): the newest first, each opening to what changed since
 * -- its diff against how the thing reads now -- and Restore, which puts
 * it back and becomes the newest version itself.
 */

#include "clawt-window-private.h"

typedef struct {
    ClawtWindow *window;
    gchar       *id;
    AdwDialog   *dialog;
} RevisionView;

static void
revision_view_free(gpointer data)
{
    RevisionView *view = data;

    g_free(view->id);
    g_free(view);
}

static void
on_restore_clicked(GtkButton *button, gpointer user_data)
{
    RevisionView *view = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *said = NULL;

    (void)button;

    reply = clawt_window_request(view->window, "revision.restore",
                                 clawt_build_payload("id", view->id, NULL));
    if (reply == NULL)
        return;

    said = g_strdup_printf("Revision %s restored; it is the newest version "
                           "now.", view->id);
    clawt_window_toast(view->window, said);
    clawt_gtk_refresh_settings_history(view->window);
    adw_dialog_close(view->dialog);
}

static void
on_revision_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(row), "clawt-revision");
    const gchar *title = g_object_get_data(G_OBJECT(row), "clawt-title");
    g_autoptr(JsonNode) reply = NULL;
    RevisionView *view;
    GtkWidget *text;
    GtkWidget *scroll;
    GtkWidget *toolbar;
    GtkWidget *header;
    GtkWidget *restore;
    const gchar *diff;

    (void)box;

    if (id == NULL)
        return;

    reply = clawt_window_request(self, "revision.diff",
                                 clawt_build_payload("from", id, NULL));
    if (reply == NULL)
        return;

    diff = clawt_json_string(clawt_payload_of(reply), "diff", "");

    text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(text), 8);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(text), 8);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(text)),
        *diff != '\0' ? diff : "This version is how it reads now.", -1);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text);
    gtk_widget_set_vexpand(scroll, TRUE);

    view = g_new0(RevisionView, 1);
    view->window = self;
    view->id = g_strdup(id);

    restore = gtk_button_new_with_label("Restore this version");
    gtk_widget_add_css_class(restore, "suggested-action");
    g_signal_connect(restore, "clicked", G_CALLBACK(on_restore_clicked), view);

    header = adw_header_bar_new();
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), restore);

    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);

    view->dialog = adw_dialog_new();
    adw_dialog_set_title(view->dialog, title != NULL ? title : id);
    adw_dialog_set_content_width(view->dialog, 900);
    adw_dialog_set_content_height(view->dialog, 700);
    adw_dialog_set_child(view->dialog, toolbar);
    g_object_set_data_full(G_OBJECT(view->dialog), "clawt-revision-view",
                           view, revision_view_free);
    adw_dialog_present(view->dialog, GTK_WIDGET(self));
}

void
clawt_gtk_refresh_settings_history(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *revisions;
    guint i;

    if (self->settings_history == NULL)
        return;

    clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_history));
    reply = clawt_window_request(self, "revision.list",
                                 clawt_build_payload("limit", "100", NULL));
    if (reply == NULL)
        return;

    revisions = json_object_get_array_member(clawt_payload_of(reply),
                                             "revisions");

    for (i = 0; i < json_array_get_length(revisions); i++) {
        JsonObject *revision = json_array_get_object_element(revisions, i);
        GtkWidget *row = adw_action_row_new();
        g_autoptr(GDateTime) at = g_date_time_new_from_unix_local(
            json_object_get_int_member(revision, "at"));
        g_autofree gchar *when = g_date_time_format(at, "%F %R");
        g_autofree gchar *title = g_strdup_printf(
            "%s %s", clawt_json_string(revision, "kind", ""),
            clawt_json_string(revision, "subject", ""));
        g_autofree gchar *subtitle = g_strdup_printf(
            "%s, by %s%s%s", when,
            clawt_json_string(revision, "author", "clawtilla"),
            clawt_json_string(revision, "note", NULL) != NULL ? " -- " : "",
            clawt_json_string(revision, "note", ""));

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
        clawt_gtk_row_opens_something(row);
        g_object_set_data_full(G_OBJECT(row), "clawt-revision",
            g_strdup_printf("%" G_GINT64_FORMAT,
                            json_object_get_int_member(revision, "id")),
            g_free);
        g_object_set_data_full(G_OBJECT(row), "clawt-title",
                               g_steal_pointer(&title), g_free);
        gtk_list_box_append(GTK_LIST_BOX(self->settings_history), row);
    }
}

GtkWidget *
clawt_gtk_build_history_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "History");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "document-open-recent-symbolic");
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Every version kept");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
        "The config, each agent's persona files and every skill, as they "
        "were each time they changed. Open one to see what has changed "
        "since, and to put it back -- which is a save of its own, so "
        "nothing is lost by trying.");

    self->settings_history = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_history),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_history, "boxed-list");
    g_signal_connect(self->settings_history, "row-activated",
                     G_CALLBACK(on_revision_activated), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_history);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}
