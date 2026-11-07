/*
 * lrg-settings-folders.c - Settings: Shared folders
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The folders mounted into agents fleet-wide, and who each one reaches.
 * Mirrors clients/gtk/gtk-prefs-folders.c, and adds the one setting a
 * shared folder has that GTK's form does not offer: the SELinux
 * relabel, walked from clawt_relabel_*() so a value added to the
 * library is offered here without this file changing.
 *
 * The relabel matters more here than on one agent: it is work podman
 * does at every start, rootless podman refuses it on files the daemon's
 * user does not own, and a shared folder reaches every computer -- so
 * one wrong relabel stops the whole fleet's containers starting, with a
 * libpod 500 naming a syscall.  The daemon refuses that up front
 * (clawt_mount_list_check_relabel()), and this form is where somebody
 * can answer the refusal with `none`.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

typedef struct {
    LrgWidget *list;
    guint      generation;
} Folders;

static Folders folders;

static void refresh(ClawtLrgApp *app);

/* ── Removing ─────────────────────────────────────────────────────── */

static void
on_removed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    if (reply != NULL) {
        clawt_lrg_toast(app, "No longer shared. An agent keeps it until "
                             "its next start.");
        refresh(app);
    }
}

static void
on_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    clawt_lrg_request(app, "defaults.mount.remove",
                      clawt_ipc_payload_new("target", user_data, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_removed, NULL, NULL);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *target = g_object_get_data(G_OBJECT(button), "clawt-target");
    g_autofree gchar *title = g_strdup_printf("Stop sharing %s?", target);

    clawt_lrg_confirm(app, title,
                      "Every agent that has it loses it at its next start. "
                      "Nothing in the folder is touched.",
                      "Stop sharing", TRUE, on_remove_confirmed,
                      g_strdup(target), g_free);
}

/* ── Adding ───────────────────────────────────────────────────────── */

typedef struct {
    LrgWidget *source;     /* unowned: the dialog's */
    LrgWidget *target;
    LrgWidget *who;
    LrgWidget *writable;
    LrgWidget *relabel;
    LrgDialog *dialog;     /* weak */
} AddFolder;

static void
add_folder_clear(gpointer data)
{
    AddFolder *add = data;

    g_clear_weak_pointer(&add->dialog);
}

static void
add_folder_release(gpointer data)
{
    g_rc_box_release_full(data, add_folder_clear);
}

static void
on_added(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    AddFolder *add = user_data;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Shared. Restart an agent for it to reach its "
                         "computer.");

    if (add->dialog != NULL)
        lrg_dialog_close(add->dialog);

    refresh(app);
}

static gboolean
on_add_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                gpointer user_data)
{
    AddFolder *add = user_data;
    g_autoptr(GError) error = NULL;
    JsonNode *payload;

    payload = clawt_lrg_settings_folder_add_payload(
        clawt_lrg_entry_text(add->source),
        clawt_lrg_entry_text(add->target),
        clawt_lrg_switch_active(add->writable),
        clawt_lrg_entry_text(add->who),
        clawt_lrg_combo_id(add->relabel), &error);

    if (payload == NULL) {
        clawt_lrg_toast(app, error->message);
        return FALSE;
    }

    clawt_lrg_request(app, "defaults.mount.add", payload,
                      CLAWT_LRG_REQUEST_NONE, on_added,
                      g_rc_box_acquire(add), add_folder_release);

    /* closed by the reply: a refused path is still there to correct */
    return FALSE;
}

static void
on_add_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    AddFolder *add = g_rc_box_new0(AddFolder);
    LrgWidget *form = clawt_lrg_form_new();
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(GPtrArray) labels = g_ptr_array_new();
    LrgDialog *dialog;
    guint i;

    add->source = clawt_lrg_entry(NULL, "~/source");
    add->target = clawt_lrg_entry(NULL, "path inside — empty means the "
                                        "same one");
    add->who = clawt_lrg_entry(NULL, "teams or agents, comma separated — "
                                     "empty means all");
    add->writable = clawt_lrg_switch("Writable", TRUE);

    /* "" first: send nothing, and the backend decides */
    g_ptr_array_add(ids, (gpointer)"");
    g_ptr_array_add(labels,
                    (gpointer)clawt_lrg_settings_relabel_label(NULL));

    for (i = 0; i < clawt_relabel_count(); i++) {
        g_ptr_array_add(ids, (gpointer)clawt_relabel_nth_nick(i));
        g_ptr_array_add(labels, (gpointer)clawt_relabel_nth_label(i));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);
    add->relabel = clawt_lrg_combo((const gchar *const *)ids->pdata,
                                   (const gchar *const *)labels->pdata, "");

    clawt_lrg_form_add(form, "Folder on this machine", add->source, NULL);
    clawt_lrg_form_add(form, "Inside the computer", add->target, NULL);
    clawt_lrg_form_add(form, "Who gets it", add->who,
                       "Naming a team covers everyone on it, including "
                       "whoever joins later.");
    clawt_lrg_form_add(form, NULL, add->writable, NULL);
    clawt_lrg_form_add(form, "SELinux relabel", add->relabel,
                       "Rootless podman cannot relabel files it does not "
                       "own, so a system path needs none.");
    lrg_widget_set_min_size(form, 540.0f * (gfloat)app->scale, 0.0f);

    dialog = clawt_lrg_dialog(
        app, "Share a folder with every agent",
        "Container, distrobox and VM agents get it. A host agent does not "
        "— there a mount is the confinement allowlist rather than a "
        "shared folder, and widening that is not something a default "
        "should do quietly.",
        form, "Share", FALSE, on_add_response, add, add_folder_release);
    g_set_weak_pointer(&add->dialog, dialog);
    g_object_unref(dialog);
    g_object_unref(form);
}

/* ── The list ─────────────────────────────────────────────────────── */

/*
 * A folder scoped to an agent or team that is not there: the agents it
 * was meant for start perfectly and lack the directory, so this list is
 * the only place anybody would find out.
 */
static void
append_warning_rows(LrgWidget *list, JsonNode *reply)
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
    JsonArray *mounts;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != folders.generation || reply == NULL)
        return;

    clawt_lrg_clear(folders.list);
    mounts = clawt_lrg_settings_member_array(clawt_ipc_reply_object(reply),
                                             "mounts");

    if (mounts == NULL || json_array_get_length(mounts) == 0)
        clawt_lrg_take(folders.list, clawt_lrg_row(
            "Nothing shared yet",
            "A directory added here reaches every agent that has a "
            "computer, including ones you create later."));

    for (i = 0; mounts != NULL && i < json_array_get_length(mounts); i++) {
        JsonObject *mount = json_array_get_object_element(mounts, i);
        const gchar *source = clawt_ipc_payload_string(mount, "source");
        const gchar *target = clawt_ipc_payload_string(mount, "target");
        g_autofree gchar *subtitle = clawt_lrg_settings_folder_subtitle(mount);
        LrgWidget *row = clawt_lrg_row((source != NULL) ? source : "?",
                                       subtitle);
        LrgWidget *remove = clawt_lrg_button("Stop sharing", "flat",
                                             G_CALLBACK(on_remove_clicked),
                                             app);

        g_object_set_data_full(G_OBJECT(remove), "clawt-target",
                               g_strdup(target), g_free);
        clawt_lrg_row_add_suffix(row, remove);

        if (clawt_ipc_payload_boolean(mount, "required", FALSE))
            clawt_lrg_row_add_prefix(row, clawt_lrg_badge("required",
                                                          "info"));

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(folders.list, row);
    }

    append_warning_rows(folders.list, reply);
    clawt_lrg_app_wake(app);
}

static void
refresh(ClawtLrgApp *app)
{
    if (folders.list == NULL)
        return;

    folders.generation++;
    clawt_lrg_request(app, "defaults.mount.list", NULL, CLAWT_LRG_QUIET,
                      on_list, GUINT_TO_POINTER(folders.generation), NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Shared folders",
                                         "Shared with every agent");
    LrgWidget *box = clawt_lrg_vbox(10.0f);

    clawt_lrg_take(clawt_lrg_page_toolbar(page),
                   clawt_lrg_button("Share a folder", "primary",
                                    G_CALLBACK(on_add_clicked), app));

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "A directory here is mounted into every container, distrobox and "
        "VM agent, including the ones you make later — your projects "
        "tree, say, so you are not wiring it up per agent. An agent that "
        "declares its own folder at the same path wins there, and one can "
        "decline the lot with computer.default_mounts: false.", "muted"));

    folders.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, folders.list);
    g_object_unref(folders.list);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "folders", "Shared folders", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_folders_page(void)
{
    return &page_impl;
}
