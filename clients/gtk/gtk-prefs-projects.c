/*
 * gtk-prefs-projects.c - Settings: projects
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The projects the fleet is pointed at (`projects:`, since 0.40.0): what
 * each is for, where it lives, who reviews its work, and what it may
 * spend in a month -- with what it has spent so far.
 */

#include "clawt-window-private.h"

/* The fields a project's dialog edits, in the order they are shown. */
static const struct {
    const gchar *key;
    const gchar *title;
} project_fields[] = {
    { "name",                "Name" },
    { "goal",                "Goal (every assignment in it starts with this)" },
    { "directory",           "Directory (absolute host path)" },
    { "review_by",           "Reviewed by" },
    { "agents",              "Agents who work in it (comma-separated; empty for all)" },
    { "monthly_budget_usd",  "Monthly budget in dollars (0 for none)" },
    { "budget_warn_percent", "Warn at this percent of the budget" },
};

typedef struct {
    ClawtWindow *window;
    gchar       *project_id;
    GtkWidget   *rows[G_N_ELEMENTS(project_fields)];
    GtkWidget   *hard_stop;
} ProjectDialog;

static void
project_dialog_free(gpointer data)
{
    ProjectDialog *editor = data;

    g_free(editor->project_id);
    g_free(editor);
}

/* Each field written back; an emptied one removes the key. */
static void
on_project_saved(GtkButton *button, gpointer user_data)
{
    ProjectDialog *editor = user_data;
    guint i;

    (void)button;

    for (i = 0; i < G_N_ELEMENTS(project_fields); i++) {
        /* The reviewer is a drop-down (0.51.3); the rest are text. */
        const gchar *value = ADW_IS_COMBO_ROW(editor->rows[i])
            ? clawt_gtk_value_row_get(editor->rows[i])
            : gtk_editable_get_text(GTK_EDITABLE(editor->rows[i]));
        g_autoptr(JsonNode) reply = clawt_window_request(
            editor->window, "project.set",
            clawt_build_payload("project", editor->project_id,
                                "key", project_fields[i].key,
                                "value", value != NULL ? value : "",
                                NULL));

        if (reply == NULL)
            return;
    }

    {
        g_autoptr(JsonNode) reply = clawt_window_request(
            editor->window, "project.set",
            clawt_build_payload("project", editor->project_id,
                                "key", "budget_hard_stop", "value",
                                adw_switch_row_get_active(
                                    ADW_SWITCH_ROW(editor->hard_stop))
                                    ? "true" : "false", NULL));

        if (reply == NULL)
            return;
    }

    clawt_window_toast(editor->window, "Saved.");
    clawt_gtk_refresh_settings_projects(editor->window);
}

static void
on_project_removed(AdwAlertDialog *dialog, gchar *response, gpointer user_data)
{
    ProjectDialog *editor = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)dialog;

    if (g_strcmp0(response, "delete") != 0)
        return;

    reply = clawt_window_request(editor->window, "project.remove",
                                 clawt_build_payload("project",
                                                     editor->project_id,
                                                     NULL));
    if (reply == NULL)
        return;

    clawt_window_toast(editor->window,
                       "Project removed. Its tasks keep its name.");
    clawt_gtk_refresh_settings_projects(editor->window);
}

static void
on_project_delete_clicked(GtkButton *button, gpointer user_data)
{
    ProjectDialog *editor = user_data;
    g_autofree gchar *heading = g_strdup_printf("Remove the project %s?",
                                                editor->project_id);
    AdwAlertDialog *confirm = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        heading,
        "Its tasks keep its name in their history; new work can no longer "
        "be put in it, and its budget stops being enforced."));

    (void)button;

    adw_alert_dialog_add_response(confirm, "cancel", "Keep it");
    adw_alert_dialog_add_response(confirm, "delete", "Remove it");
    adw_alert_dialog_set_response_appearance(confirm, "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_close_response(confirm, "cancel");
    g_signal_connect(confirm, "response", G_CALLBACK(on_project_removed),
                     editor);
    adw_dialog_present(ADW_DIALOG(confirm), GTK_WIDGET(editor->window));
}

/* A project's fields as the dialog shows them: text, a list, a number. */
static gchar *
field_text(JsonObject *project, const gchar *key)
{
    JsonNode *node;

    if (!json_object_has_member(project, key))
        return g_strdup("");

    node = json_object_get_member(project, key);

    if (JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *items = json_node_get_array(node);
        g_autoptr(GString) out = g_string_new(NULL);
        guint i;

        for (i = 0; i < json_array_get_length(items); i++)
            g_string_append_printf(out, "%s%s", i > 0 ? ", " : "",
                                   json_array_get_string_element(items, i));
        return g_string_free(g_steal_pointer(&out), FALSE);
    }

    if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
        return g_strdup_printf("%g", json_node_get_double(node));
    if (json_node_get_value_type(node) == G_TYPE_INT64)
        return g_strdup_printf("%" G_GINT64_FORMAT, json_node_get_int(node));
    if (json_node_get_value_type(node) == G_TYPE_STRING)
        return g_strdup(json_node_get_string(node));

    return g_strdup("");
}

static void
on_project_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    JsonObject *project = g_object_get_data(G_OBJECT(row), "clawt-project");
    ProjectDialog *editor;
    AdwDialog *dialog;
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *save;
    GtkWidget *remove;
    guint i;

    (void)box;

    if (project == NULL)
        return;

    editor = g_new0(ProjectDialog, 1);
    editor->window = self;
    editor->project_id = g_strdup(clawt_json_string(project, "id", ""));

    dialog = ADW_DIALOG(adw_preferences_dialog_new());
    adw_dialog_set_title(dialog, editor->project_id);
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    editor->project_id);

    for (i = 0; i < G_N_ELEMENTS(project_fields); i++) {
        g_autofree gchar *value = field_text(project, project_fields[i].key);

        /*
         * Who reviews is a fixed list and the fleet's agents (0.51.3),
         * not a word to remember the spelling of.
         */
        if (g_strcmp0(project_fields[i].key, "review_by") == 0) {
            g_autoptr(JsonNode) listed = clawt_window_request(
                self, "agent.list", NULL);
            JsonObject *root = (listed != NULL) ? clawt_payload_of(listed)
                                                : NULL;
            JsonArray *agents = (root != NULL &&
                                 json_object_has_member(root, "agents"))
                ? json_object_get_array_member(root, "agents") : NULL;
            g_autoptr(GStrvBuilder) ids = g_strv_builder_new();
            g_auto(GStrv) agent_ids = NULL;
            guint a;

            for (a = 0; agents != NULL && a < json_array_get_length(agents);
                 a++)
                g_strv_builder_add(ids, clawt_json_string(
                    json_array_get_object_element(agents, a), "id", ""));
            agent_ids = g_strv_builder_end(ids);
            editor->rows[i] = clawt_gtk_review_by_row_new(
                (const gchar *const *)agent_ids, TRUE, value);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                      editor->rows[i]);
            continue;
        }

        editor->rows[i] = adw_entry_row_new();
        adw_preferences_row_set_use_markup(
            ADW_PREFERENCES_ROW(editor->rows[i]), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(editor->rows[i]),
                                      project_fields[i].title);
        gtk_editable_set_text(GTK_EDITABLE(editor->rows[i]), value);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  editor->rows[i]);
    }

    editor->hard_stop = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(editor->hard_stop),
                                  "Refuse new work once the budget is spent");
    adw_switch_row_set_active(ADW_SWITCH_ROW(editor->hard_stop),
        json_object_get_boolean_member_with_default(project,
                                                    "budget_hard_stop",
                                                    FALSE));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), editor->hard_stop);

    save = gtk_button_new_with_label("Save changes");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_margin_top(save, 12);
    g_signal_connect(save, "clicked", G_CALLBACK(on_project_saved), editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), save);

    remove = gtk_button_new_with_label("Remove this project");
    gtk_widget_add_css_class(remove, "destructive-action");
    gtk_widget_set_margin_top(remove, 6);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_project_delete_clicked),
                     editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), remove);

    g_object_set_data_full(G_OBJECT(dialog), "clawt-project-editor", editor,
                           project_dialog_free);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(page));
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

static void
on_project_add_response(AdwAlertDialog *dialog, gchar *response,
                        gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "clawt-project-id");
    g_autoptr(JsonNode) reply = NULL;
    const gchar *project_id;

    if (g_strcmp0(response, "create") != 0 || entry == NULL)
        return;

    project_id = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (*project_id == '\0') {
        clawt_window_toast(self, "A project needs an id.");
        return;
    }

    reply = clawt_window_request(self, "project.create",
                                 clawt_build_payload("project", project_id,
                                                     NULL));
    if (reply == NULL)
        return;

    clawt_window_toast(self, "Project created. Give it a goal and a "
                             "directory so work finds its way to it.");
    clawt_gtk_refresh_settings_projects(self);
}

static void
on_project_add_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "New project",
        "An id, in lowercase with hyphens. Tasks name their project by it "
        "and it cannot be changed afterwards."));
    GtkWidget *entry = gtk_entry_new();

    (void)button;

    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "website");
    adw_alert_dialog_set_extra_child(dialog, entry);
    adw_alert_dialog_add_response(dialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(dialog, "create", "Create");
    adw_alert_dialog_set_response_appearance(dialog, "create",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(dialog, "create");
    adw_alert_dialog_set_close_response(dialog, "cancel");
    g_object_set_data(G_OBJECT(dialog), "clawt-project-id", entry);
    g_signal_connect(dialog, "response", G_CALLBACK(on_project_add_response),
                     self);
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

void
clawt_gtk_refresh_settings_projects(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *projects;
    guint i;

    if (self->settings_projects == NULL)
        return;

    clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_projects));
    reply = clawt_window_request(self, "project.list", NULL);

    if (reply == NULL)
        return;

    projects = json_object_get_array_member(clawt_payload_of(reply),
                                            "projects");

    if (json_array_get_length(projects) == 0) {
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "No projects yet");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
            "A fleet works without them. A project tells each piece of "
            "work what it is for, and keeps a monthly budget on it.");
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(GTK_LIST_BOX(self->settings_projects), row);
    }

    for (i = 0; i < json_array_get_length(projects); i++) {
        JsonObject *project = json_array_get_object_element(projects, i);
        gdouble budget = json_object_get_double_member_with_default(
            project, "monthly_budget_usd", 0);
        gdouble spent = json_object_get_double_member_with_default(
            project, "month_spend_usd", 0);
        GtkWidget *row = adw_action_row_new();
        g_autofree gchar *money = (budget > 0)
            ? g_strdup_printf("$%.2f of $%.2f this month%s", spent, budget,
                              json_object_get_boolean_member_with_default(
                                  project, "stopped", FALSE)
                                  ? " -- stopped" : "")
            : g_strdup_printf("$%.2f this month, no budget", spent);
        g_autofree gchar *subtitle = g_strdup_printf("%s \342\200\224 %s",
            money, clawt_json_string(project, "goal", "no goal written"));

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
            clawt_json_string(project, "name",
                              clawt_json_string(project, "id", "?")));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
        clawt_gtk_row_opens_something(row);
        g_object_set_data_full(G_OBJECT(row), "clawt-project",
                               json_object_ref(project),
                               (GDestroyNotify)json_object_unref);
        gtk_list_box_append(GTK_LIST_BOX(self->settings_projects), row);
    }
}

GtkWidget *
clawt_gtk_build_projects_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Projects");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "folder-symbolic");
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "What the work is for");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
        "A task belongs to the project its delegation names, its parent's, "
        "or the one whose directory its repository is. Its assignee is told "
        "the goal, and the project's month of spend is kept against its "
        "budget.");

    gtk_widget_add_css_class(add, "flat");
    gtk_widget_set_tooltip_text(add, "Add a project");
    g_signal_connect(add, "clicked", G_CALLBACK(on_project_add_clicked), self);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group), add);

    self->settings_projects = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_projects),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_projects, "boxed-list");
    g_signal_connect(self->settings_projects, "row-activated",
                     G_CALLBACK(on_project_activated), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_projects);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}
