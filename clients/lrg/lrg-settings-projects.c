/*
 * lrg-settings-projects.c - Settings: Projects
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The projects the fleet is pointed at (since 0.40.0).  Mirrors
 * clients/gtk/gtk-prefs-projects.c: the list with each month's spend
 * against its budget, a dialog per project, and adding one.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

typedef struct {
    LrgWidget *list;
    guint      generation;
} Projects;

static Projects projects;

static void refresh(ClawtLrgApp *app);

/* The fields a project's dialog edits. */
static const struct {
    const gchar *key;
    const gchar *label;
    const gchar *hint;
} fields[] = {
    { "name",                "Name",            NULL },
    { "goal",                "Goal",            "Every assignment in it starts with this." },
    { "directory",           "Directory",       "Absolute host path of its repository." },
    { "review_by",           "Reviewed by",     "lead, delegator, operator or an agent." },
    { "agents",              "Agents",          "Comma-separated; empty for every agent." },
    { "monthly_budget_usd",  "Monthly budget (USD)", "0 for none." },
    { "budget_warn_percent", "Warn at percent", NULL },
};

#define N_FIELDS G_N_ELEMENTS(fields)

/* ── One project ──────────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp *app;
    gchar       *project_id;
    LrgWidget   *entries[N_FIELDS];  /* unowned: the dialog's */
    LrgWidget   *hard_stop;
    LrgDialog   *dialog;             /* weak */
    guint        pending;
    gboolean     failed;
} ProjectEditor;

static void
project_editor_clear(gpointer data)
{
    ProjectEditor *editor = data;

    g_clear_weak_pointer(&editor->dialog);
    g_free(editor->project_id);
}

/* Held by the dialog and by each request in flight. */
static void
project_editor_release(gpointer data)
{
    g_rc_box_release_full(data, project_editor_clear);
}

/* A save is every field landing; "Saved" only after the last. */
static void
on_project_set(ClawtLrgApp *app, JsonNode *reply, const GError *error,
               gpointer user_data)
{
    ProjectEditor *editor = user_data;

    (void)error;

    if (reply == NULL)
        editor->failed = TRUE;

    if (--editor->pending > 0)
        return;

    if (!editor->failed) {
        clawt_lrg_toast(app, "Saved.");

        if (editor->dialog != NULL)
            lrg_dialog_close(editor->dialog);
    }

    refresh(app);
}

static void
send_field(ProjectEditor *editor, const gchar *key, const gchar *value)
{
    clawt_lrg_request(editor->app, "project.set",
                      clawt_ipc_payload_new("project", editor->project_id,
                                            "key", key,
                                            "value", (value != NULL &&
                                                      *value != '\0')
                                                ? value : NULL,
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_project_set,
                      g_rc_box_acquire(editor), project_editor_release);
}

static gboolean
on_project_save(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                gpointer user_data)
{
    ProjectEditor *editor = user_data;
    guint i;

    (void)app;
    (void)dialog;
    (void)response;

    if (editor->pending > 0)
        return FALSE;

    editor->pending = N_FIELDS + 1;
    editor->failed = FALSE;

    for (i = 0; i < N_FIELDS; i++)
        send_field(editor, fields[i].key,
                   clawt_lrg_entry_text(editor->entries[i]));

    send_field(editor, "budget_hard_stop",
               clawt_lrg_switch_active(editor->hard_stop) ? "true" : "false");

    /* kept open until every field answers */
    return FALSE;
}

static void
on_project_removed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                   gpointer user_data)
{
    (void)error;
    (void)user_data;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Project removed. Its tasks keep its name.");
    refresh(app);
}

static void
on_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "project.remove",
                      clawt_ipc_payload_new("project", user_data, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_project_removed, NULL, NULL);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    ProjectEditor *editor = user_data;
    g_autofree gchar *title = g_strdup_printf("Remove the project %s?",
                                              editor->project_id);

    (void)button;

    if (editor->dialog != NULL)
        lrg_dialog_close(editor->dialog);

    clawt_lrg_confirm(editor->app, title,
                      "Its tasks keep its name in their history; new work "
                      "can no longer be put in it, and its budget stops "
                      "being enforced.",
                      "Remove it", TRUE, on_remove_confirmed,
                      g_strdup(editor->project_id), g_free);
}

/* A field's current value as text: a list joined, a number printed. */
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
on_edit_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    JsonObject *project = g_object_get_data(G_OBJECT(button), "clawt-project");
    ProjectEditor *editor;
    LrgWidget *form = clawt_lrg_form_new();
    LrgWidget *remove;
    LrgDialog *dialog;
    guint i;

    if (project == NULL)
        return;

    editor = g_rc_box_new0(ProjectEditor);
    editor->app = app;
    editor->project_id = g_strdup(clawt_ipc_payload_string(project, "id"));

    for (i = 0; i < N_FIELDS; i++) {
        g_autofree gchar *value = field_text(project, fields[i].key);

        editor->entries[i] = clawt_lrg_entry(value, NULL);
        clawt_lrg_form_add(form, fields[i].label, editor->entries[i],
                           fields[i].hint);
    }

    editor->hard_stop = clawt_lrg_switch(
        "Refuse new work once the budget is spent",
        json_object_get_boolean_member_with_default(project,
                                                    "budget_hard_stop",
                                                    FALSE));
    clawt_lrg_take(form, editor->hard_stop);

    remove = clawt_lrg_button("Remove this project", "danger",
                              G_CALLBACK(on_remove_clicked), editor);
    lrg_widget_set_halign(remove, LRG_ALIGN_START);
    clawt_lrg_take(form, remove);
    lrg_widget_set_min_size(form, 560.0f * (gfloat)app->scale, 0.0f);

    dialog = clawt_lrg_dialog(app, editor->project_id, NULL, form,
                              "Save changes", FALSE, on_project_save, editor,
                              project_editor_release);
    g_set_weak_pointer(&editor->dialog, dialog);
    g_object_unref(dialog);
    g_object_unref(form);
}

/* ── Adding one ───────────────────────────────────────────────────── */

static void
on_project_created(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                   gpointer user_data)
{
    LrgDialog **dialog = user_data;

    (void)error;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Project created. Give it a goal and a directory "
                         "so work finds its way to it.");

    if (*dialog != NULL)
        lrg_dialog_close(*dialog);

    refresh(app);
}

static void
weak_dialog_free(gpointer data)
{
    LrgDialog **dialog = data;

    g_clear_weak_pointer(dialog);
    g_free(dialog);
}

static gboolean
on_add_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                gpointer user_data)
{
    LrgWidget *entry = user_data;
    g_autofree gchar *project_id = g_strdup(clawt_lrg_entry_text(entry));
    LrgDialog **weak;

    (void)response;

    g_strstrip(project_id);

    if (*project_id == '\0') {
        clawt_lrg_toast(app, "A project needs an id.");
        return FALSE;
    }

    weak = g_new0(LrgDialog *, 1);
    g_set_weak_pointer(weak, dialog);
    clawt_lrg_request(app, "project.create",
                      clawt_ipc_payload_new("project", project_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_project_created, weak,
                      weak_dialog_free);

    return FALSE;
}

static void
on_add_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    LrgWidget *entry = clawt_lrg_entry(NULL, "website");

    (void)button;

    lrg_widget_set_min_size(entry, 360.0f * (gfloat)app->scale, 0.0f);
    g_object_unref(clawt_lrg_dialog(
        app, "New project",
        "An id, in lowercase with hyphens. Tasks name their project by it "
        "and it cannot be changed afterwards.",
        entry, "Create", FALSE, on_add_response, entry, NULL));
    g_object_unref(entry);
}

/* ── The list ─────────────────────────────────────────────────────── */

static void
on_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonArray *list;
    guint i;

    (void)error;

    if (GPOINTER_TO_UINT(user_data) != projects.generation || reply == NULL)
        return;

    clawt_lrg_clear(projects.list);
    list = clawt_lrg_settings_member_array(clawt_ipc_reply_object(reply),
                                           "projects");

    if (list == NULL || json_array_get_length(list) == 0)
        clawt_lrg_take(projects.list, clawt_lrg_row(
            "No projects yet",
            "A fleet works without them. A project tells each piece of "
            "work what it is for, and keeps a monthly budget on it."));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *project = json_array_get_object_element(list, i);
        const gchar *id = clawt_ipc_payload_string(project, "id");
        gdouble budget = json_object_get_double_member_with_default(
            project, "monthly_budget_usd", 0);
        gdouble spent = json_object_get_double_member_with_default(
            project, "month_spend_usd", 0);
        gboolean stopped = json_object_get_boolean_member_with_default(
            project, "stopped", FALSE);
        g_autofree gchar *subtitle = (budget > 0)
            ? g_strdup_printf("$%.2f of $%.2f this month -- %s", spent,
                              budget, clawt_ipc_payload_string(project, "goal")
                                  != NULL
                                  ? clawt_ipc_payload_string(project, "goal")
                                  : "no goal written")
            : g_strdup_printf("$%.2f this month, no budget -- %s", spent,
                              clawt_ipc_payload_string(project, "goal") != NULL
                                  ? clawt_ipc_payload_string(project, "goal")
                                  : "no goal written");
        LrgWidget *row;
        LrgWidget *edit;

        if (id == NULL)
            continue;

        row = clawt_lrg_row(clawt_ipc_payload_string(project, "name") != NULL
                                ? clawt_ipc_payload_string(project, "name")
                                : id, subtitle);
        edit = clawt_lrg_button("Edit…", "flat", G_CALLBACK(on_edit_clicked),
                                app);
        g_object_set_data_full(G_OBJECT(edit), "clawt-project",
                               json_object_ref(project),
                               (GDestroyNotify)json_object_unref);
        clawt_lrg_row_add_suffix(row, edit);

        if (stopped)
            clawt_lrg_row_add_prefix(row, clawt_lrg_badge("stopped", "warn"));

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(projects.list, row);
    }

    clawt_lrg_app_wake(app);
}

static void
refresh(ClawtLrgApp *app)
{
    if (projects.list == NULL)
        return;

    projects.generation++;
    clawt_lrg_request(app, "project.list", NULL, CLAWT_LRG_QUIET, on_list,
                      GUINT_TO_POINTER(projects.generation), NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Projects", "What the work is for");
    LrgWidget *box = clawt_lrg_vbox(10.0f);

    clawt_lrg_take(clawt_lrg_page_toolbar(page),
                   clawt_lrg_button("Add a project", "primary",
                                    G_CALLBACK(on_add_clicked), app));

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "A task belongs to the project its delegation names, its parent's, "
        "or the one whose directory its repository is. Its assignee is told "
        "the goal, and the project's month of spend is kept against its "
        "budget.", "muted"));

    projects.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, projects.list);
    g_object_unref(projects.list);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "projects", "Projects", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_projects_page(void)
{
    return &page_impl;
}
