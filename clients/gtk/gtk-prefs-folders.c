/*
 * gtk-prefs-folders.c - Settings: shared folders
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The folders mounted into agents fleet-wide, and who each one reaches.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Shared folders, fleet-wide ───────────────────────────────────── */

/*
 * Who a shared folder reaches, in a phrase.
 *
 * On every row rather than only the scoped ones: a list where most rows
 * say nothing and one says "team backend" invites reading the silent
 * ones as unknown rather than as everybody.
 */
static gchar *
folder_scope_text(JsonObject *mount)
{
    const gchar *scope = clawt_json_string(mount, "scope", "all");
    g_autoptr(GString) out = NULL;
    static const gchar *const keys[] = { "agents", "teams", NULL };
    gsize k;

    if (g_strcmp0(scope, "all") == 0)
        return g_strdup("every agent");

    if (g_strcmp0(scope, "none") == 0)
        return g_strdup("nobody");

    out = g_string_new(NULL);

    for (k = 0; keys[k] != NULL; k++) {
        JsonArray *items = json_object_has_member(mount, keys[k])
            ? json_object_get_array_member(mount, keys[k]) : NULL;
        guint i;

        for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
            if (out->len > 0)
                g_string_append(out, ", ");

            g_string_append_printf(out, "%s %s",
                                   g_strcmp0(keys[k], "teams") == 0
                                       ? "team" : "agent",
                                   json_array_get_string_element(items, i));
        }
    }

    /* A `selected` scope naming nothing reaches nobody, and says so. */
    return out->len > 0 ? g_strdup(out->str) : g_strdup("nobody (no list)");
}

/*
 * The names somebody typed, sent as one list.
 *
 * Sorted by the *daemon*, not here.  This used to ask `team.list` and
 * file each name itself -- and `team.list` reports the teams somebody
 * declared, while an agent can perfectly well be on a team nobody
 * declared. Every such name went to `agents:`, where it matched
 * nothing: the folder reached nobody, the agents meant to have it
 * started perfectly, and the field promising "teams or agents" was the
 * thing that had caused it.
 *
 * The daemon answers the same question clawt_mount_covers() does, which
 * is the only version that cannot disagree with what the mount then
 * does.
 */
static void
add_who_list(JsonBuilder *builder, const gchar *raw)
{
    g_auto(GStrv) items = NULL;
    gsize i;
    gboolean any = FALSE;

    if (raw == NULL || *raw == '\0')
        return;

    items = g_strsplit(raw, ",", -1);

    for (i = 0; items[i] != NULL; i++) {
        g_strstrip(items[i]);

        if (items[i][0] == '\0')
            continue;

        if (!any) {
            json_builder_set_member_name(builder, "who");
            json_builder_begin_array(builder);
            any = TRUE;
        }

        json_builder_add_string_value(builder, items[i]);
    }

    if (any)
        json_builder_end_array(builder);
}

static void
on_shared_folder_add_response(AdwAlertDialog *dialog, const gchar *response,
                              gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *source = g_object_get_data(G_OBJECT(dialog), "clawt-source");
    GtkWidget *target = g_object_get_data(G_OBJECT(dialog), "clawt-target");
    GtkWidget *writable = g_object_get_data(G_OBJECT(dialog), "clawt-rw");
    const gchar *source_text;
    const gchar *target_text;
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "add") != 0)
        return;

    source_text = gtk_editable_get_text(GTK_EDITABLE(source));
    target_text = gtk_editable_get_text(GTK_EDITABLE(target));

    if (source_text == NULL || *source_text == '\0') {
        clawt_window_toast(self, "A shared folder needs a path to share.");
        return;
    }

    /*
     * An empty target means the same path inside, which is what people
     * mean nine times out of ten -- an agent told about ~/source finds
     * it at a path that reads the same in both places, so a note about
     * a file is a note either of you can follow.
     */
    if (target_text == NULL || *target_text == '\0')
        target_text = source_text;

    /*
     * Teams and agents are told apart by asking the daemon which teams
     * exist, rather than by two entry boxes: a name is one or the
     * other, and making somebody classify it is asking them to know
     * something the fleet already knows.
     */
    {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        const gchar *who_text =
            gtk_editable_get_text(GTK_EDITABLE(
                g_object_get_data(G_OBJECT(dialog), "clawt-who")));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "source");
        json_builder_add_string_value(builder, source_text);
        json_builder_set_member_name(builder, "target");
        json_builder_add_string_value(builder, target_text);
        json_builder_set_member_name(builder, "mode");
        json_builder_add_string_value(
            builder, gtk_switch_get_active(GTK_SWITCH(writable)) ? "rw"
                                                                 : "ro");
        add_who_list(builder, who_text);
        json_builder_end_object(builder);

        reply = clawt_window_request(self, "defaults.mount.add",
                                     json_builder_get_root(builder));
    }

    if (reply == NULL)
        return;

    clawt_window_toast(self,
                       "Shared with every agent. Restart one for it to "
                       "reach its computer.");
    clawt_gtk_refresh_settings_folders(self);
}

static void
on_shared_folder_add_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *dialog;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *source = gtk_entry_new();
    GtkWidget *target = gtk_entry_new();
    GtkWidget *who = gtk_entry_new();
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *label = gtk_label_new("Writable");
    GtkWidget *writable = gtk_switch_new();

    (void)button;

    dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "Share a folder with every agent",
        "Container, distrobox and VM agents get it. A host agent does "
        "not \xe2\x80\x94 there a mount is the confinement allowlist "
        "rather than a shared folder, and widening that is not something "
        "a default should do quietly.\n"
        "\n"
        "Naming a team covers everyone on it, including whoever joins "
        "later."));

    gtk_entry_set_placeholder_text(GTK_ENTRY(source), "~/source");
    gtk_entry_set_placeholder_text(GTK_ENTRY(target),
                                   "path inside \xe2\x80\x94 empty means "
                                   "the same one");
    gtk_entry_set_placeholder_text(
        GTK_ENTRY(who),
        "teams or agents, comma separated \xe2\x80\x94 empty means all");

    gtk_switch_set_active(GTK_SWITCH(writable), TRUE);
    gtk_widget_set_valign(writable, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), writable);

    gtk_box_append(GTK_BOX(box), source);
    gtk_box_append(GTK_BOX(box), target);
    gtk_box_append(GTK_BOX(box), who);
    gtk_box_append(GTK_BOX(box), row);
    adw_alert_dialog_set_extra_child(dialog, box);

    adw_alert_dialog_add_response(dialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(dialog, "add", "Share");
    adw_alert_dialog_set_response_appearance(dialog, "add",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(dialog, "add");
    adw_alert_dialog_set_close_response(dialog, "cancel");

    g_object_set_data(G_OBJECT(dialog), "clawt-source", source);
    g_object_set_data(G_OBJECT(dialog), "clawt-target", target);
    g_object_set_data(G_OBJECT(dialog), "clawt-rw", writable);
    g_object_set_data(G_OBJECT(dialog), "clawt-who", who);
    g_signal_connect(dialog, "response",
                     G_CALLBACK(on_shared_folder_add_response), self);

    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

static void
on_shared_folder_remove(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *target = g_object_get_data(G_OBJECT(button), "clawt-target");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(self, "defaults.mount.remove",
                                 clawt_build_payload("target", target, NULL));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_settings_folders(self);
}

/*
 * The daemon's fleet-level warnings, drawn as rows under whatever list
 * they are about.
 *
 * One builder rather than one per settings page: teams, integrations and
 * shared folders all report the same kind of thing -- a mistake only the
 * whole fleet can see -- and three copies of this loop would be three
 * chances for one page to stop drawing them without anybody noticing.
 */
void
clawt_gtk_append_warning_rows(GtkListBox *list, JsonNode *reply)
{
    JsonArray *warnings = (reply != NULL)
        ? json_object_get_array_member(clawt_payload_of(reply), "warnings")
        : NULL;
    guint i;

    for (i = 0; warnings != NULL && i < json_array_get_length(warnings); i++) {
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "Worth fixing");
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(row),
            json_array_get_string_element(warnings, i));
        adw_action_row_add_prefix(
            ADW_ACTION_ROW(row),
            gtk_image_new_from_icon_name("dialog-warning-symbolic"));
        gtk_list_box_append(list, row);
    }
}

void
clawt_gtk_refresh_settings_folders(ClawtWindow *self)
{
    if (self->settings_folders == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_SHARED_FOLDERS))
        return;

    do {
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *mounts;
        guint i;

        clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_folders));

        reply = clawt_window_request(self, "defaults.mount.list", NULL);

        if (reply == NULL)
            continue;

        mounts = json_object_get_array_member(clawt_payload_of(reply),
                                              "mounts");

        if (mounts == NULL || json_array_get_length(mounts) == 0) {
            GtkWidget *row = adw_action_row_new();

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          "Nothing shared yet");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(row),
                "A directory added here reaches every agent that has a "
                "computer, including ones you create later.");
            gtk_list_box_append(GTK_LIST_BOX(self->settings_folders), row);
            continue;
        }

        for (i = 0; i < json_array_get_length(mounts); i++) {
            JsonObject *mount = json_array_get_object_element(mounts, i);
            const gchar *source = clawt_json_string(mount, "source", "");
            const gchar *target = clawt_json_string(mount, "target", "");
            const gchar *mode = clawt_json_string(mount, "mode", "rw");
            GtkWidget *row = adw_action_row_new();
            GtkWidget *remove_button;
            g_autofree gchar *subtitle = NULL;

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), source);

            /*
             * Both paths, always. An agent's tools run on the host and
             * its shell runs inside the computer, so a folder has two
             * names and telling somebody only one is how they go
             * looking for a file at a path that does not exist on the
             * machine they are on.
             */
            {
                g_autofree gchar *who = folder_scope_text(mount);

                subtitle = g_strdup_printf("%s inside, %s \xc2\xb7 %s",
                                           target,
                                           g_strcmp0(mode, "ro") == 0
                                               ? "read-only" : "writable",
                                           who);
            }
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            remove_button =
                gtk_button_new_from_icon_name("user-trash-symbolic");
            gtk_widget_add_css_class(remove_button, "flat");
            gtk_widget_set_valign(remove_button, GTK_ALIGN_CENTER);
            gtk_widget_set_tooltip_text(remove_button,
                                        "Stop sharing this folder");
            g_object_set_data_full(G_OBJECT(remove_button), "clawt-target",
                                   g_strdup(target), g_free);
            g_signal_connect(remove_button, "clicked",
                             G_CALLBACK(on_shared_folder_remove), self);
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove_button);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_folders), row);
        }

        /*
         * A folder scoped to an agent or a team that is not there.  The
         * agents it was meant for start perfectly and simply do not have
         * the directory, so this list is the only place somebody would
         * ever find out.
         */
        clawt_gtk_append_warning_rows(GTK_LIST_BOX(self->settings_folders), reply);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_SHARED_FOLDERS));
}

GtkWidget *
clawt_gtk_build_shared_folders_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *add;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page),
                                   "Shared folders");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "folder-remote-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Shared with every agent");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "A directory here is mounted into every container, distrobox and "
        "VM agent, including the ones you make later \xe2\x80\x94 your "
        "projects tree, say, so you are not wiring it up per agent. An "
        "agent that declares its own folder at the same path wins there, "
        "and one can decline the lot with computer.default_mounts: false.");

    add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(add, "flat");
    gtk_widget_set_tooltip_text(add, "Share a folder");
    g_signal_connect(add, "clicked",
                     G_CALLBACK(on_shared_folder_add_clicked), self);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group),
                                            add);

    self->settings_folders = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_folders),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_folders, "boxed-list");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_folders);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}
