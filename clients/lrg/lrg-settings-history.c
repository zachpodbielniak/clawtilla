/*
 * lrg-settings-history.c - Settings: History
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every kept version of the config, the persona files and the skills
 * (since 0.41.0).  Mirrors clients/gtk/gtk-prefs-history.c: the newest
 * first, each opening to its diff against now with Restore.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

typedef struct {
    LrgWidget *list;
    guint      generation;
} History;

static History history;

static void refresh(ClawtLrgApp *app);

static void
on_restored(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    (void)error;
    (void)user_data;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Restored; it is the newest version now.");
    refresh(app);
}

static gboolean
on_restore_response(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                    gpointer user_data)
{
    (void)dialog;
    (void)response;

    clawt_lrg_request(app, "revision.restore",
                      clawt_ipc_payload_new("id", user_data, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_restored, NULL, NULL);
    return TRUE;
}

/* The diff arrives: shown, with Restore as the dialog's primary. */
static void
on_diff(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    const gchar *id = user_data;
    const gchar *diff;
    g_autofree gchar *title = NULL;
    LrgWidget *text;

    (void)error;

    if (reply == NULL)
        return;

    diff = clawt_ipc_payload_string(json_node_get_object(reply), "diff");
    title = g_strdup_printf("Revision %s against now", id);
    text = clawt_lrg_paragraph((diff != NULL && *diff != '\0')
                                   ? diff
                                   : "This version is how it reads now.",
                               "mono");
    lrg_widget_set_min_size(text, 720.0f * (gfloat)app->scale, 0.0f);

    g_object_unref(clawt_lrg_dialog(app, title,
        "Restoring puts this version back; it becomes the newest version, "
        "so nothing is lost.", text, "Restore this version", FALSE,
        on_restore_response, g_strdup(id), g_free));
    g_object_unref(text);
}

static void
on_open_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *id = g_object_get_data(G_OBJECT(button), "clawt-revision");

    clawt_lrg_request(app, "revision.diff",
                      clawt_ipc_payload_new("from", id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_diff, g_strdup(id), g_free);
}

static void
on_list(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    JsonArray *list;
    guint i;

    (void)error;

    if (GPOINTER_TO_UINT(user_data) != history.generation || reply == NULL)
        return;

    clawt_lrg_clear(history.list);
    list = clawt_lrg_settings_member_array(clawt_ipc_reply_object(reply),
                                           "revisions");

    if (list == NULL || json_array_get_length(list) == 0)
        clawt_lrg_take(history.list, clawt_lrg_row(
            "No versions kept yet",
            "The config, each persona file and each skill are kept every "
            "time they change."));

    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *revision = json_array_get_object_element(list, i);
        g_autoptr(GDateTime) at = g_date_time_new_from_unix_local(
            clawt_ipc_payload_int(revision, "at", 0));
        g_autofree gchar *when = g_date_time_format(at, "%F %R");
        g_autofree gchar *title = g_strdup_printf("%s %s",
            clawt_ipc_payload_string(revision, "kind"),
            clawt_ipc_payload_string(revision, "subject"));
        g_autofree gchar *subtitle = g_strdup_printf("%s, by %s%s%s", when,
            clawt_ipc_payload_string(revision, "author") != NULL
                ? clawt_ipc_payload_string(revision, "author") : "clawtilla",
            clawt_ipc_payload_string(revision, "note") != NULL ? " -- " : "",
            clawt_ipc_payload_string(revision, "note") != NULL
                ? clawt_ipc_payload_string(revision, "note") : "");
        LrgWidget *row = clawt_lrg_row(title, subtitle);
        LrgWidget *open = clawt_lrg_button("Open…", "flat",
                                           G_CALLBACK(on_open_clicked), app);

        g_object_set_data_full(G_OBJECT(open), "clawt-revision",
            g_strdup_printf("%" G_GINT64_FORMAT,
                            clawt_ipc_payload_int(revision, "id", 0)),
            g_free);
        clawt_lrg_row_add_suffix(row, open);
        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(history.list, row);
    }

    clawt_lrg_app_wake(app);
}

static void
refresh(ClawtLrgApp *app)
{
    if (history.list == NULL)
        return;

    history.generation++;
    clawt_lrg_request(app, "revision.list",
                      clawt_ipc_payload_new("limit", "100", NULL),
                      CLAWT_LRG_QUIET, on_list,
                      GUINT_TO_POINTER(history.generation), NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("History", "Every version kept");
    LrgWidget *box = clawt_lrg_vbox(10.0f);

    (void)app;

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "The config, each agent's persona files and every skill, as they "
        "were each time they changed. Open one to see what has changed "
        "since, and to put it back.", "muted"));

    history.list = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(box, history.list);
    g_object_unref(history.list);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "history", "History", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_history_page(void)
{
    return &page_impl;
}
