/*
 * gtk-prefs-pack.c - Settings: export and import a fleet
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A fleet as a directory (since 0.42.0): written out for another machine
 * or as a template, and taken in -- previewed first, each collision
 * skipped unless the operator says replace or rename, dangerous items
 * refused unless allowed.  See clawt-fleet-pack.h.
 */

#include "clawt-window-private.h"

typedef struct {
    ClawtWindow *window;
    GtkWidget   *directory;
    GtkWidget   *resolutions;
    GtkWidget   *dangerous;
    GtkWidget   *preview;
} PackPage;

static PackPage pack;

static const gchar *
pack_directory(void)
{
    return gtk_editable_get_text(GTK_EDITABLE(pack.directory));
}

/* The preview's items and to-fill list, as plain lines. */
static gchar *
preview_text(JsonObject *preview)
{
    g_autoptr(GString) out = g_string_new(NULL);
    JsonArray *items = json_object_get_array_member(preview, "items");
    JsonArray *to_fill = json_object_get_array_member(preview, "to_fill");
    guint i;

    for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
        JsonObject *item = json_array_get_object_element(items, i);
        JsonArray *dangerous = json_object_get_array_member(item,
                                                            "dangerous");

        g_string_append_printf(out, "%s %s: %s%s\n",
            clawt_json_string(item, "kind", ""),
            clawt_json_string(item, "id", ""),
            json_object_get_boolean_member(item, "exists")
                ? "exists, skipped unless replaced or renamed" : "new",
            json_array_get_length(dangerous) > 0
                ? " -- turns on a dangerous setting" : "");
    }

    if (to_fill != NULL && json_array_get_length(to_fill) > 0)
        g_string_append(out, "\nTo fill in afterwards:\n");
    for (i = 0; to_fill != NULL && i < json_array_get_length(to_fill); i++)
        g_string_append_printf(out, "  %s\n",
                               json_array_get_string_element(to_fill, i));

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static void
on_export_clicked(GtkButton *button, gpointer user_data)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *said = NULL;

    (void)button;
    (void)user_data;

    reply = clawt_window_request(pack.window, "fleet.export",
                                 clawt_build_payload("dir", pack_directory(),
                                                     NULL));
    if (reply == NULL)
        return;

    said = g_strdup_printf("Exported: %" G_GINT64_FORMAT " agents, %"
                           G_GINT64_FORMAT " skills",
                           clawt_json_int(clawt_payload_of(reply), "agents", 0),
                           clawt_json_int(clawt_payload_of(reply), "skills",
                                          0));
    clawt_window_toast(pack.window, said);
}

static void
on_preview_clicked(GtkButton *button, gpointer user_data)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *text = NULL;

    (void)button;
    (void)user_data;

    reply = clawt_window_request(pack.window, "fleet.import_preview",
                                 clawt_build_payload("dir", pack_directory(),
                                                     NULL));
    if (reply == NULL)
        return;

    text = preview_text(clawt_payload_of(reply));
    gtk_label_set_text(GTK_LABEL(pack.preview), text);
}

static void
on_import_clicked(GtkButton *button, gpointer user_data)
{
    g_autoptr(JsonNode) reply = NULL;
    const gchar *resolutions =
        gtk_editable_get_text(GTK_EDITABLE(pack.resolutions));

    (void)button;
    (void)user_data;

    reply = clawt_window_request(pack.window, "fleet.import",
        clawt_build_payload("dir", pack_directory(),
                            "resolutions", *resolutions != '\0'
                                ? resolutions : NULL,
                            "allow_dangerous",
                            adw_switch_row_get_active(
                                ADW_SWITCH_ROW(pack.dangerous))
                                ? "true" : NULL,
                            NULL));
    if (reply == NULL)
        return;

    clawt_window_toast(pack.window, "Imported. Agents are held and skills "
                                    "disabled until you start and read them.");
    clawt_gtk_refresh_agents(pack.window);
}

GtkWidget *
clawt_gtk_build_pack_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *export_button = gtk_button_new_with_label("Export");
    GtkWidget *preview_button = gtk_button_new_with_label("Preview import");
    GtkWidget *import_button = gtk_button_new_with_label("Import");

    pack.window = self;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page),
                                   "Export & import");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "document-send-symbolic");
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "A fleet as a directory");
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
        "Export writes the agents, teams, routines, triggers, projects, "
        "persona files and skills -- never a secret or this machine's "
        "paths. Import shows what it would do first; what exists is "
        "skipped unless you say replace or rename, agents arrive held, "
        "skills disabled, and nothing that turns on a dangerous setting "
        "comes in unless you allow it.");

    pack.directory = clawt_gtk_add_entry(group,
        "Directory (absolute path on the daemon's machine)", "");
    pack.resolutions = clawt_gtk_add_entry(group,
        "On import: agent:scout=rename:scout2, team:crew=replace, ...", "");

    pack.dangerous = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(pack.dangerous),
                                  "Allow items that turn on dangerous "
                                  "settings");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), pack.dangerous);

    gtk_widget_add_css_class(import_button, "suggested-action");
    g_signal_connect(export_button, "clicked", G_CALLBACK(on_export_clicked),
                     NULL);
    g_signal_connect(preview_button, "clicked",
                     G_CALLBACK(on_preview_clicked), NULL);
    g_signal_connect(import_button, "clicked", G_CALLBACK(on_import_clicked),
                     NULL);
    gtk_box_append(GTK_BOX(buttons), export_button);
    gtk_box_append(GTK_BOX(buttons), preview_button);
    gtk_box_append(GTK_BOX(buttons), import_button);
    gtk_widget_set_margin_top(buttons, 12);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), buttons);

    pack.preview = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(pack.preview), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(pack.preview), TRUE);
    gtk_label_set_selectable(GTK_LABEL(pack.preview), TRUE);
    gtk_widget_set_margin_top(pack.preview, 12);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), pack.preview);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    return page;
}
