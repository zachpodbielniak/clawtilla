/*
 * gtk-prefs-teams.c - Settings: teams
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The teams the fleet declares, which is what the sidebar groups by.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* Edit one team: its name, what it is for, and removing it. */
typedef struct {
    ClawtWindow *window;
    gchar       *team_id;
    GtkWidget   *name_entry;
    GtkWidget   *description_entry;
} TeamDialog;

static void
team_dialog_free(gpointer data)
{
    TeamDialog *editor = data;

    g_free(editor->team_id);
    g_free(editor);
}

static void
on_team_removed(AdwAlertDialog *dialog, gchar *response, gpointer user_data)
{
    TeamDialog *editor = user_data;
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "delete") != 0)
        return;

    reply = clawt_window_request(
        editor->window, "team.remove",
        clawt_build_payload("team", editor->team_id, NULL));

    if (reply == NULL)
        return;

    {
        gint64 orphaned = clawt_json_int(clawt_payload_of(reply),
                                         "orphaned", 0);
        g_autofree gchar *message = NULL;

        /*
         * Said rather than left to be discovered. The agents are fine and
         * still running; they are simply on a team that is no longer
         * declared, which is where hand-offs quietly stop working.
         */
        message = (orphaned > 0)
            ? g_strdup_printf("Team removed. %" G_GINT64_FORMAT " agent%s "
                              "still name it -- put them on another team.",
                              orphaned, orphaned == 1 ? "" : "s")
            : g_strdup("Team removed.");

        clawt_window_toast(editor->window, message);
    }

    clawt_gtk_refresh_settings_teams(editor->window);
    clawt_gtk_refresh_agents(editor->window);
}

static void
on_team_saved(GtkButton *button, gpointer user_data)
{
    TeamDialog *editor = user_data;
    g_autoptr(JsonNode) named = NULL;
    g_autoptr(JsonNode) described = NULL;

    named = clawt_window_request(
        editor->window, "team.set",
        clawt_build_payload("team", editor->team_id, "key", "name",
                            "value", clawt_gtk_answer_of(editor->name_entry), NULL));

    described = clawt_window_request(
        editor->window, "team.set",
        clawt_build_payload("team", editor->team_id, "key", "description",
                            "value", clawt_gtk_answer_of(editor->description_entry),
                            NULL));

    if (named == NULL || described == NULL)
        return;

    clawt_window_toast(editor->window, "Saved.");
    clawt_gtk_refresh_settings_teams(editor->window);
    clawt_gtk_refresh_agents(editor->window);
}

static void
on_team_delete_clicked(GtkButton *button, gpointer user_data)
{
    TeamDialog *editor = user_data;
    AdwAlertDialog *confirm;
    g_autofree gchar *heading = NULL;

    heading = g_strdup_printf("Remove the %s team?", editor->team_id);

    confirm = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        heading,
        "The agents on it keep running and keep their settings. They "
        "simply stop being on a team, so its lead can no longer assign "
        "to them."));

    adw_alert_dialog_add_response(confirm, "cancel", "Keep it");
    adw_alert_dialog_add_response(confirm, "delete", "Remove it");
    adw_alert_dialog_set_response_appearance(confirm, "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(confirm, "cancel");
    adw_alert_dialog_set_close_response(confirm, "cancel");

    g_signal_connect(confirm, "response", G_CALLBACK(on_team_removed),
                     editor);

    adw_dialog_present(ADW_DIALOG(confirm), GTK_WIDGET(editor->window));
}

static void
on_team_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *team_id = g_object_get_data(G_OBJECT(row), "clawt-team");
    TeamDialog *editor;
    AdwDialog *dialog;
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *save;
    GtkWidget *remove;

    if (team_id == NULL)
        return;

    editor = g_new0(TeamDialog, 1);
    editor->window = self;
    editor->team_id = g_strdup(team_id);

    dialog = ADW_DIALOG(adw_preferences_dialog_new());
    adw_dialog_set_title(dialog, team_id);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), team_id);

    editor->name_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(editor->name_entry), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(editor->name_entry),
                                  "Name");
    gtk_editable_set_text(
        GTK_EDITABLE(editor->name_entry),
        (const gchar *)g_object_get_data(G_OBJECT(row), "clawt-team-name"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              editor->name_entry);

    editor->description_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(editor->description_entry), FALSE);
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(editor->description_entry), "What it handles");
    gtk_editable_set_text(
        GTK_EDITABLE(editor->description_entry),
        (const gchar *)g_object_get_data(G_OBJECT(row), "clawt-team-desc"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              editor->description_entry);

    /*
     * Said here rather than left implicit. This description is not a
     * label: the chief of staff reads it to decide what belongs to this
     * team, so a blank one means work never gets sent here.
     */
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The chief of staff reads the description to decide which team a "
        "piece of work belongs to. Say what this team handles and what it "
        "does not -- the names on it do not say that.");

    save = gtk_button_new_with_label("Save changes");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_margin_top(save, 12);
    g_signal_connect(save, "clicked", G_CALLBACK(on_team_saved), editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), save);

    remove = gtk_button_new_with_label("Remove this team");
    gtk_widget_add_css_class(remove, "destructive-action");
    gtk_widget_set_margin_top(remove, 6);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_team_delete_clicked),
                     editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), remove);

    g_object_set_data_full(G_OBJECT(dialog), "clawt-team-editor", editor,
                           team_dialog_free);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(page));
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

static void
on_team_add_response(AdwAlertDialog *dialog, gchar *response,
                     gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "clawt-team-id");
    g_autoptr(JsonNode) reply = NULL;
    const gchar *team_id;

    if (g_strcmp0(response, "create") != 0 || entry == NULL)
        return;

    team_id = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (team_id == NULL || *team_id == '\0') {
        clawt_window_toast(self, "A team needs an id.");
        return;
    }

    reply = clawt_window_request(self, "team.create",
                                 clawt_build_payload("id", team_id, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Team created. Give it a description so the "
                             "chief of staff knows what belongs here.");
    clawt_gtk_refresh_settings_teams(self);
    clawt_gtk_refresh_agents(self);
}

static void
on_team_add_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *dialog;
    GtkWidget *entry = gtk_entry_new();

    dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "New team",
        "An id, in lowercase with hyphens. Agents name their team by it "
        "and it cannot be changed afterwards."));

    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "research");
    adw_alert_dialog_set_extra_child(dialog, entry);

    adw_alert_dialog_add_response(dialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(dialog, "create", "Create");
    adw_alert_dialog_set_response_appearance(dialog, "create",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(dialog, "create");
    adw_alert_dialog_set_close_response(dialog, "cancel");

    g_object_set_data(G_OBJECT(dialog), "clawt-team-id", entry);
    g_signal_connect(dialog, "response", G_CALLBACK(on_team_add_response),
                     self);

    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

void
clawt_gtk_refresh_settings_teams(ClawtWindow *self)
{
    if (self->settings_teams == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_TEAMS))
        return;

    do {
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *teams;
        guint i;

        clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_teams));

        reply = clawt_window_request(self, "team.list", NULL);

        if (reply == NULL)
            continue;

        teams = json_object_get_array_member(clawt_payload_of(reply),
                                             "teams");

        if (json_array_get_length(teams) == 0) {
            GtkWidget *row = adw_action_row_new();

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          "No teams yet");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(row),
                "A fleet works without them. They start earning their "
                "keep once there are more agents than you can hold in "
                "your head.");
            gtk_widget_set_sensitive(row, FALSE);
            gtk_list_box_append(GTK_LIST_BOX(self->settings_teams), row);
        }

        for (i = 0; i < json_array_get_length(teams); i++) {
            JsonObject *team = json_array_get_object_element(teams, i);
            const gchar *id = clawt_json_string(team, "id", "?");
            const gchar *lead = clawt_json_string(team, "lead", NULL);
            const gchar *description =
                clawt_json_string(team, "description", NULL);
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *subtitle = NULL;

            subtitle = g_strdup_printf(
                "%s \342\200\224 %" G_GINT64_FORMAT " of %"
                G_GINT64_FORMAT " running, lead: %s",
                (description != NULL && *description != '\0')
                    ? description
                    : "no description, so nothing will be sent here",
                clawt_json_int(team, "running", 0),
                clawt_json_int(team, "total", 0),
                lead != NULL ? lead : "nobody");

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(row),
                clawt_json_string(team, "name", id));
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
            clawt_gtk_row_opens_something(row);

            g_object_set_data_full(G_OBJECT(row), "clawt-team",
                                   g_strdup(id), g_free);
            g_object_set_data_full(
                G_OBJECT(row), "clawt-team-name",
                g_strdup(clawt_json_string(team, "name", id)), g_free);
            g_object_set_data_full(
                G_OBJECT(row), "clawt-team-desc",
                g_strdup(description != NULL ? description : ""), g_free);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_teams), row);
        }

        /*
         * Two leads on one team, or an agent on a team nobody declared.
         * Shown here rather than only in the daemon's log: it is the
         * failure where work quietly goes nowhere.
         */
        clawt_gtk_append_warning_rows(GTK_LIST_BOX(self->settings_teams), reply);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_TEAMS));
}

GtkWidget *
clawt_gtk_build_teams_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *add;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Teams");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "system-users-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "How the fleet is divided");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "A team has a lead who assigns work inside it, and members who "
        "talk to anyone and assign to nobody. The chief of staff sits "
        "above all of them and hands work to the leads -- it picks which "
        "team by reading these descriptions, so they are worth writing "
        "properly.");

    add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(add, "flat");
    gtk_widget_set_tooltip_text(add, "Add a team");
    g_signal_connect(add, "clicked", G_CALLBACK(on_team_add_clicked), self);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group),
                                            add);

    self->settings_teams = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_teams),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_teams, "boxed-list");
    g_signal_connect(self->settings_teams, "row-activated",
                     G_CALLBACK(on_team_activated), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_teams);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}
