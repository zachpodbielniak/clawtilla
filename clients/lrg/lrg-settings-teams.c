/*
 * lrg-settings-teams.c - Settings: Teams
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The teams the fleet declares, which is what the sidebar groups by.
 * Mirrors clients/gtk/gtk-prefs-teams.c: the list, a dialog per team
 * (its name, what it handles, removing it) and adding one.
 *
 * Every change re-reads the fleet as well as this list: the sidebar's
 * team headings come from the same declarations, and a team renamed
 * here and still drawn under its old name there reads as the rename
 * having failed.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

typedef struct {
    LrgWidget *list;
    guint      generation;
} Teams;

static Teams teams;

static void refresh(ClawtLrgApp *app);

/* Re-reads this page and the fleet, after anything here changed. */
static void
changed(ClawtLrgApp *app)
{
    refresh(app);
    clawt_lrg_refresh_fleet(app);
}

/* ── One team ─────────────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp *app;
    gchar       *team_id;
    LrgWidget   *name_entry;     /* unowned: the dialog's */
    LrgWidget   *desc_entry;
    LrgDialog   *dialog;         /* weak */
    guint        pending;
    gboolean     failed;
} TeamEditor;

static void
team_editor_clear(gpointer data)
{
    TeamEditor *editor = data;

    g_clear_weak_pointer(&editor->dialog);
    g_free(editor->team_id);
}

/*
 * Reference counted: the dialog holds one and every request in flight
 * holds one, because a dialog cancelled while its save is on the wire
 * is finalised before the reply arrives to read the editor.
 */
static void
team_editor_release(gpointer data)
{
    g_rc_box_release_full(data, team_editor_clear);
}

/*
 * Both halves of a save have to land before it is a save: the name and
 * the description are two `team.set` frames, and saying "Saved" after
 * the first would be saying it about half.
 */
static void
on_team_set(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    TeamEditor *editor = user_data;

    if (reply == NULL)
        editor->failed = TRUE;

    if (--editor->pending > 0)
        return;

    if (!editor->failed) {
        clawt_lrg_toast(app, "Saved.");

        if (editor->dialog != NULL)
            lrg_dialog_close(editor->dialog);
    }

    changed(app);
}

static gboolean
on_team_save(ClawtLrgApp *app, LrgDialog *dialog, gint response,
             gpointer user_data)
{
    TeamEditor *editor = user_data;
    const gchar *name = clawt_lrg_entry_text(editor->name_entry);
    const gchar *description = clawt_lrg_entry_text(editor->desc_entry);

    /* a save already in flight is not started twice */
    if (editor->pending > 0)
        return FALSE;

    editor->pending = 2;
    editor->failed = FALSE;

    clawt_lrg_request(app, "team.set",
                      clawt_ipc_payload_new("team", editor->team_id,
                                            "key", "name",
                                            "value", (*name != '\0')
                                                ? name : NULL,
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_team_set,
                      g_rc_box_acquire(editor), team_editor_release);
    clawt_lrg_request(app, "team.set",
                      clawt_ipc_payload_new("team", editor->team_id,
                                            "key", "description",
                                            "value", (*description != '\0')
                                                ? description : NULL,
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_team_set,
                      g_rc_box_acquire(editor), team_editor_release);

    /* kept open until both answer, so a refusal leaves the text there */
    return FALSE;
}

typedef struct {
    gchar     *team_id;
    LrgDialog *editor;   /* weak */
} TeamRemoval;

static void
team_removal_free(gpointer data)
{
    TeamRemoval *removal = data;

    g_clear_weak_pointer(&removal->editor);
    g_free(removal->team_id);
    g_free(removal);
}

static void
on_team_removed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                gpointer user_data)
{
    TeamRemoval *removal = user_data;
    g_autofree gchar *text = NULL;

    if (reply == NULL)
        return;

    /*
     * Said rather than left to be discovered: the agents still run, on
     * a team nobody declares, which is where hand-offs stop working.
     */
    text = clawt_lrg_settings_team_removed_text(clawt_ipc_payload_int(
        clawt_ipc_reply_object(reply), "orphaned", 0));
    clawt_lrg_toast(app, text);

    if (removal->editor != NULL)
        lrg_dialog_close(removal->editor);

    changed(app);
}

static void
on_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    TeamRemoval *removal = user_data;
    TeamRemoval *request = g_new0(TeamRemoval, 1);

    request->team_id = g_strdup(removal->team_id);
    g_set_weak_pointer(&request->editor, removal->editor);

    clawt_lrg_request(app, "team.remove",
                      clawt_ipc_payload_new("team", removal->team_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_team_removed, request,
                      team_removal_free);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    TeamEditor *editor = user_data;
    TeamRemoval *removal = g_new0(TeamRemoval, 1);
    g_autofree gchar *title =
        g_strdup_printf("Remove the %s team?", editor->team_id);

    removal->team_id = g_strdup(editor->team_id);
    g_set_weak_pointer(&removal->editor, editor->dialog);

    clawt_lrg_confirm(editor->app, title,
                      "The agents on it keep running and keep their "
                      "settings. They simply stop being on a team, so its "
                      "lead can no longer assign to them.",
                      "Remove it", TRUE, on_remove_confirmed, removal,
                      team_removal_free);
}

static void
open_team(ClawtLrgApp *app, const gchar *team_id, const gchar *name,
          const gchar *description)
{
    TeamEditor *editor = g_rc_box_new0(TeamEditor);
    LrgWidget *form = clawt_lrg_form_new();
    LrgWidget *remove;
    LrgDialog *dialog;

    editor->app = app;
    editor->team_id = g_strdup(team_id);
    editor->name_entry = clawt_lrg_entry(name, team_id);
    editor->desc_entry = clawt_lrg_entry(description,
                                         "What this team handles");

    clawt_lrg_form_add(form, "Name", editor->name_entry, NULL);

    /*
     * Said here: the description is not a label.  The chief of staff
     * reads it to decide what belongs to this team, so a blank one
     * means work never gets sent here.
     */
    clawt_lrg_form_add(form, "What it handles", editor->desc_entry,
                       "The chief of staff reads the description to decide "
                       "which team a piece of work belongs to. Say what this "
                       "team handles and what it does not -- the names on it "
                       "do not say that.");

    remove = clawt_lrg_button("Remove this team", "danger",
                              G_CALLBACK(on_remove_clicked), editor);
    lrg_widget_set_halign(remove, LRG_ALIGN_START);
    clawt_lrg_take(form, remove);
    lrg_widget_set_min_size(form, 520.0f * (gfloat)app->scale, 0.0f);

    dialog = clawt_lrg_dialog(app, team_id, NULL, form, "Save changes",
                              FALSE, on_team_save, editor,
                              team_editor_release);
    g_set_weak_pointer(&editor->dialog, dialog);

    /*
     * The canvas holds the presented dialog; ours goes, so closing it
     * finalises it and releases the editor with it.
     */
    g_object_unref(dialog);
    g_object_unref(form);
}

static void
on_edit_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    GObject *object = G_OBJECT(button);

    open_team(app, g_object_get_data(object, "clawt-team"),
              g_object_get_data(object, "clawt-team-name"),
              g_object_get_data(object, "clawt-team-desc"));
}

/* ── Adding one ───────────────────────────────────────────────────── */

static void
on_team_created(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                gpointer user_data)
{
    LrgDialog **dialog = user_data;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Team created. Give it a description so the "
                         "chief of staff knows what belongs here.");

    if (*dialog != NULL)
        lrg_dialog_close(*dialog);

    changed(app);
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
    g_autofree gchar *team_id = g_strdup(clawt_lrg_entry_text(entry));
    LrgDialog **weak = g_new0(LrgDialog *, 1);

    g_strstrip(team_id);

    if (*team_id == '\0') {
        clawt_lrg_toast(app, "A team needs an id.");
        g_free(weak);
        return FALSE;
    }

    g_set_weak_pointer(weak, dialog);
    clawt_lrg_request(app, "team.create",
                      clawt_ipc_payload_new("id", team_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_team_created, weak,
                      weak_dialog_free);

    /* closed by the reply, so a refused id is still there to fix */
    return FALSE;
}

static void
on_add_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    LrgWidget *entry = clawt_lrg_entry(NULL, "research");

    lrg_widget_set_min_size(entry, 360.0f * (gfloat)app->scale, 0.0f);
    g_object_unref(clawt_lrg_dialog(
        app, "New team",
        "An id, in lowercase with hyphens. Agents name their team by it "
        "and it cannot be changed afterwards.",
        entry, "Create", FALSE, on_add_response, entry, NULL));
    g_object_unref(entry);
}

/* ── The list ─────────────────────────────────────────────────────── */

/*
 * Two leads on one team, or an agent on a team nobody declared: shown
 * under the list rather than only in the daemon's log, because it is
 * the failure where work quietly goes nowhere.
 */
static void
append_warning_rows(ClawtLrgApp *app, LrgWidget *list, JsonNode *reply)
{
    JsonArray *warnings = clawt_lrg_settings_member_array(
        clawt_ipc_reply_object(reply), "warnings");
    guint i;

    for (i = 0; warnings != NULL && i < json_array_get_length(warnings);
         i++) {
        LrgWidget *row = clawt_lrg_row(
            "Worth fixing", json_array_get_string_element(warnings, i));

        clawt_lrg_row_add_prefix(row, clawt_lrg_badge("!", "warn"));
        clawt_lrg_take(list, row);
    }
}

static void
on_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonArray *list;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != teams.generation || reply == NULL)
        return;

    clawt_lrg_clear(teams.list);
    list = clawt_lrg_settings_member_array(clawt_ipc_reply_object(reply),
                                           "teams");

    if (list == NULL || json_array_get_length(list) == 0)
        clawt_lrg_take(teams.list, clawt_lrg_row(
            "No teams yet",
            "A fleet works without them. They start earning their keep "
            "once there are more agents than you can hold in your head."));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *team = json_array_get_object_element(list, i);
        const gchar *id = clawt_ipc_payload_string(team, "id");
        const gchar *name = clawt_ipc_payload_string(team, "name");
        const gchar *description =
            clawt_ipc_payload_string(team, "description");
        g_autofree gchar *subtitle = clawt_lrg_settings_team_subtitle(team);
        LrgWidget *row;
        LrgWidget *edit;

        if (id == NULL)
            continue;

        if (name == NULL || *name == '\0')
            name = id;

        row = clawt_lrg_row(name, subtitle);
        edit = clawt_lrg_button("Edit…", "flat",
                                G_CALLBACK(on_edit_clicked), app);
        g_object_set_data_full(G_OBJECT(edit), "clawt-team", g_strdup(id),
                               g_free);
        g_object_set_data_full(G_OBJECT(edit), "clawt-team-name",
                               g_strdup(name), g_free);
        g_object_set_data_full(G_OBJECT(edit), "clawt-team-desc",
                               g_strdup((description != NULL)
                                            ? description : ""),
                               g_free);
        clawt_lrg_row_add_suffix(row, edit);

        if (clawt_ipc_payload_int(team, "running", 0) > 0)
            clawt_lrg_row_add_prefix(row, clawt_lrg_badge("live", "good"));

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(teams.list, row);
    }

    append_warning_rows(app, teams.list, reply);
    clawt_lrg_app_wake(app);
}

static void
refresh(ClawtLrgApp *app)
{
    if (teams.list == NULL)
        return;

    teams.generation++;
    clawt_lrg_request(app, "team.list", NULL, CLAWT_LRG_QUIET, on_list,
                      GUINT_TO_POINTER(teams.generation), NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Teams",
                                         "How the fleet is divided");
    LrgWidget *box = clawt_lrg_vbox(10.0f);

    clawt_lrg_take(clawt_lrg_page_toolbar(page),
                   clawt_lrg_button("Add a team", "primary",
                                    G_CALLBACK(on_add_clicked), app));

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "A team has a lead who assigns work inside it, and members who "
        "talk to anyone and assign to nobody. The chief of staff sits "
        "above all of them and hands work to the leads -- it picks which "
        "team by reading these descriptions, so they are worth writing "
        "properly.", "muted"));

    teams.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, teams.list);
    g_object_unref(teams.list);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "teams", "Teams", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_teams_page(void)
{
    return &page_impl;
}
