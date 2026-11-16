/*
 * lrg-settings-pack.c - Settings: Export & import
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A fleet as a directory (since 0.42.0).  Mirrors
 * clients/gtk/gtk-prefs-pack.c: a directory, Export, Preview import --
 * what would be new, what exists, what is dangerous -- and Import with
 * resolutions and an allow-dangerous switch.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-settings-model.h"

typedef struct {
    LrgWidget *directory;
    LrgWidget *resolutions;
    LrgWidget *dangerous;
    LrgWidget *preview;
} Pack;

static Pack pack;

static void
on_done(ClawtLrgApp *app, JsonNode *reply, const GError *error,
        gpointer user_data)
{
    (void)error;

    if (reply != NULL)
        clawt_lrg_toast(app, user_data);
}

static void
on_preview(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    g_autoptr(GString) text = g_string_new(NULL);
    JsonObject *root;
    JsonArray *items;
    JsonArray *to_fill;
    guint i;

    (void)error;
    (void)user_data;

    if (reply == NULL)
        return;

    root = clawt_ipc_reply_object(reply);
    items = clawt_lrg_settings_member_array(root, "items");
    to_fill = clawt_lrg_settings_member_array(root, "to_fill");

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *item = json_array_get_object_element(items, i);

        g_string_append_printf(text, "%s %s: %s%s\n",
            clawt_ipc_payload_string(item, "kind"),
            clawt_ipc_payload_string(item, "id"),
            clawt_ipc_payload_boolean(item, "exists", FALSE)
                ? "exists" : "new",
            json_array_get_length(json_object_get_array_member(item,
                "dangerous")) > 0 ? " -- dangerous" : "");
    }
    for (i = 0; to_fill != NULL && i < json_array_get_length(to_fill); i++)
        g_string_append_printf(text, "to fill: %s\n",
                               json_array_get_string_element(to_fill, i));

    clawt_lrg_clear(pack.preview);
    clawt_lrg_take(pack.preview, clawt_lrg_paragraph(text->str, "mono"));
    clawt_lrg_app_wake(app);
}

static void
on_export_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    (void)button;

    clawt_lrg_request(app, "fleet.export",
                      clawt_ipc_payload_new("dir",
                          clawt_lrg_entry_text(pack.directory), NULL),
                      CLAWT_LRG_REQUEST_NONE, on_done, (gpointer)"Exported",
                      NULL);
}

static void
on_preview_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    (void)button;

    clawt_lrg_request(app, "fleet.import_preview",
                      clawt_ipc_payload_new("dir",
                          clawt_lrg_entry_text(pack.directory), NULL),
                      CLAWT_LRG_REQUEST_NONE, on_preview, NULL, NULL);
}

static void
on_import_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *resolutions = clawt_lrg_entry_text(pack.resolutions);

    (void)button;

    clawt_lrg_request(app, "fleet.import",
                      clawt_ipc_payload_new(
                          "dir", clawt_lrg_entry_text(pack.directory),
                          "resolutions", (resolutions != NULL &&
                                          *resolutions != '\0')
                              ? resolutions : NULL,
                          "allow_dangerous",
                          clawt_lrg_switch_active(pack.dangerous)
                              ? "true" : NULL, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_done,
                      (gpointer)"Imported: agents are held, skills disabled",
                      NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Export & import",
                                         "A fleet as a directory");
    LrgWidget *box = clawt_lrg_vbox(10.0f);
    LrgWidget *form = clawt_lrg_form_new();
    LrgWidget *buttons = clawt_lrg_hbox(8.0f);

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "Export writes the agents, teams, routines, triggers, projects, "
        "persona files and skills -- never a secret or this machine's "
        "paths. Import adds what is new and skips what exists unless told; "
        "agents arrive held, skills disabled, and nothing that turns on a "
        "dangerous setting comes in unless allowed.", "muted"));

    pack.directory = clawt_lrg_entry(NULL, "/home/you/fleet-pack");
    clawt_lrg_form_add(form, "Directory", pack.directory,
                       "An absolute path on the daemon's machine.");
    pack.resolutions = clawt_lrg_entry(NULL,
                                       "agent:scout=rename:scout2, ...");
    clawt_lrg_form_add(form, "On import", pack.resolutions,
                       "skip, replace or rename:<id>, per kind:id.");
    pack.dangerous = clawt_lrg_switch("Allow dangerous settings", FALSE);
    clawt_lrg_take(form, pack.dangerous);
    clawt_lrg_take(box, form);

    clawt_lrg_take(buttons, clawt_lrg_button("Export", NULL,
                   G_CALLBACK(on_export_clicked), app));
    clawt_lrg_take(buttons, clawt_lrg_button("Preview import", NULL,
                   G_CALLBACK(on_preview_clicked), app));
    clawt_lrg_take(buttons, clawt_lrg_button("Import", "primary",
                   G_CALLBACK(on_import_clicked), app));
    clawt_lrg_take(box, buttons);

    pack.preview = clawt_lrg_vbox(4.0f);
    clawt_lrg_add(box, pack.preview);
    g_object_unref(pack.preview);

    clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

    return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "pack", "Export & import", build, NULL
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_pack_page(void)
{
    return &page_impl;
}
