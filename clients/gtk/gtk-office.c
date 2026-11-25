/*
 * gtk-office.c - The Office page: who is where
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * One office.snapshot read, drawn as the roster every client draws: each
 * participant's name, an activity badge in clawt_office_activity_tone(),
 * and clawt_office_roster_line() ("on a call with kudu · since 2m").
 * Re-read on office.presence, the daemon's edge, never on a timer of its
 * own.  When the office is off the page says how to turn it on, from the
 * daemon's own `why`.
 *
 * The office itself is drawn above the roster by clawtilla-office,
 * embedded as a child (gtk-office-view.c); this file is the roster and
 * the settings.
 */

#include "clawt-window-private.h"

static GtkWidget *
roster_row(JsonObject *participant, gint64 now)
{
    GtkWidget *row = adw_action_row_new();
    const gchar *id = clawt_json_string(participant, "id", "?");
    const gchar *name = clawt_json_string(participant, "name", id);
    const gchar *kind = clawt_json_string(participant, "kind", "agent");
    g_autofree gchar *line = clawt_office_roster_line(participant, now);
    ClawtOfficeActivity activity = CLAWT_OFFICE_IDLE;

    clawt_office_activity_from_nick(clawt_json_string(participant, "activity", NULL),
                                    &activity);
    clawt_gtk_set_row_text(row, name, line);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row),
        clawt_gtk_badge(clawt_office_activity_label(activity),
                        clawt_gtk_tone_class(clawt_office_activity_tone(activity)),
                        "What the daemon last saw this participant doing"));
    if (g_strcmp0(kind, "agent") != 0)
        adw_action_row_add_suffix(ADW_ACTION_ROW(row),
            clawt_gtk_badge(g_strcmp0(kind, "grok_bot") == 0 ? "remote"
                            : g_strcmp0(kind, "ephemeral") == 0 ? "helper" : kind,
                            "dim-label", NULL));
    return row;
}

/* ── Settings: on or off, and which world ───────────────────────── */

static gboolean
refresh_later(gpointer user_data)
{
    clawt_gtk_refresh_office(user_data);
    return G_SOURCE_REMOVE;
}

/*
 * The refresh goes to an idle: it rebuilds the list holding the switch
 * whose own notify handler is running, and a widget torn out of its
 * parent mid-emission is the kind of crash that reports nothing useful.
 */
static void
office_set(ClawtWindow *self, JsonNode *payload)
{
    g_autoptr(JsonNode) reply = clawt_window_request(self, "office.set", payload);

    if (reply != NULL)
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, refresh_later, g_object_ref(self),
                        g_object_unref);
}

static void
on_enabled_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    gboolean on = adw_switch_row_get_active(ADW_SWITCH_ROW(object));

    office_set(user_data, clawt_build_payload("enabled", on ? "true" : "false", NULL));
}

static void
on_world_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    GtkStringList *ids = g_object_get_data(object, "clawt-world-ids");
    guint chosen = adw_combo_row_get_selected(ADW_COMBO_ROW(object));

    if (ids != NULL && chosen < g_list_model_get_n_items(G_LIST_MODEL(ids)))
        office_set(user_data, clawt_build_payload(
            "world", gtk_string_list_get_string(ids, chosen), NULL));
}

static void
on_install_custodians(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = clawt_window_request(self, "office.custodians_install",
                                                     NULL);

    (void)button;
    if (reply != NULL)
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, refresh_later, g_object_ref(self),
                        g_object_unref);
}

/*
 * Who keeps the office: the team carrying the office-custodial tag, and
 * the generator its steward uses -- or the button that makes the team.
 */
static void
fill_custodians(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = clawt_window_request(self, "office.custodians", NULL);
    JsonObject *root;
    const gchar *team;
    const gchar *generator;
    GtkWidget *row;
    g_autofree gchar *subtitle = NULL;

    if (reply == NULL)
        return;
    root = clawt_payload_of(reply);
    team = clawt_json_string(root, "team", NULL);
    generator = clawt_json_string(root, "generator", NULL);

    row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Custodial team");
    if (team != NULL) {
        subtitle = g_strdup_printf("%s keeps the office; characters are made with %s",
                                   team, generator != NULL && *generator != '\0'
                                   ? generator : "nothing yet (office.asset_generator)");
    } else {
        GtkWidget *install = gtk_button_new_with_label("Create custodial team");

        subtitle = g_strdup("Nobody keeps the office: a lead, an asset steward and a "
                            "set designer would take your requests for it");
        gtk_widget_set_valign(install, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(install, "suggested-action");
        g_signal_connect(install, "clicked", G_CALLBACK(on_install_custodians), self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), install);
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
    gtk_list_box_append(self->office_controls, row);
}

/* The switch and the world picker, filled from office.worlds. */
static void
fill_controls(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = clawt_window_request(self, "office.worlds", NULL);
    JsonObject *root;
    JsonArray *worlds;
    GtkWidget *toggle;
    GtkWidget *picker;
    g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
    GtkStringList *ids = gtk_string_list_new(NULL);
    guint current = 0;
    guint i;

    clawt_gtk_clear_list(self->office_controls);
    if (reply == NULL)
        return;
    root = clawt_payload_of(reply);

    /* the value set before the handler is connected, so building is not a change */
    toggle = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(toggle), "Keep the office");
    clawt_gtk_set_row_hint(toggle, "Who is where, from what the daemon sees; "
                                   "nothing is computed while it is off");
    adw_switch_row_set_active(ADW_SWITCH_ROW(toggle),
                              clawt_json_boolean(root, "enabled", FALSE));
    g_signal_connect(toggle, "notify::active", G_CALLBACK(on_enabled_changed), self);
    gtk_list_box_append(self->office_controls, toggle);

    worlds = json_object_has_member(root, "worlds")
        ? json_object_get_array_member(root, "worlds") : NULL;
    for (i = 0; worlds != NULL && i < json_array_get_length(worlds); i++) {
        JsonObject *w = json_array_get_object_element(worlds, i);
        const gchar *id = clawt_json_string(w, "id", "?");

        gtk_string_list_append(ids, id);
        gtk_string_list_append(names, clawt_json_string(w, "name", id));
        if (g_strcmp0(id, clawt_json_string(root, "current", NULL)) == 0)
            current = i;
    }
    picker = clawt_gtk_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(picker), "World");
    adw_combo_row_set_model(ADW_COMBO_ROW(picker), G_LIST_MODEL(names));
    adw_combo_row_set_selected(ADW_COMBO_ROW(picker), current);
    g_object_set_data_full(G_OBJECT(picker), "clawt-world-ids", ids, g_object_unref);
    g_signal_connect(picker, "notify::selected", G_CALLBACK(on_world_changed), self);
    gtk_list_box_append(self->office_controls, picker);

    fill_custodians(self);
}

static void
refresh_office_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *root;
    JsonArray *people;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    fill_controls(self);
    clawt_gtk_clear_list(self->office_list);
    reply = clawt_window_request(self, "office.snapshot", NULL);
    if (reply == NULL) {
        clawt_gtk_office_view_set_enabled(self, FALSE);
        return;
    }
    root = clawt_payload_of(reply);

    /* the drawing runs only while there is an office to draw */
    clawt_gtk_office_view_set_enabled(self, clawt_json_boolean(root, "enabled", TRUE));
    if (!clawt_json_boolean(root, "enabled", TRUE)) {
        gtk_label_set_text(GTK_LABEL(self->office_status),
                           clawt_json_string(root, "why", "The office is off."));
        return;
    }

    now = clawt_json_int(root, "now", now);
    {
        g_autofree gchar *status = g_strdup_printf(
            "World: %s.  Every pose is something the daemon saw -- a turn, a tool, a "
            "message, a decision -- and nothing else.",
            clawt_json_string(root, "world", "?"));

        gtk_label_set_text(GTK_LABEL(self->office_status), status);
    }

    people = json_object_has_member(root, "participants")
        ? json_object_get_array_member(root, "participants") : NULL;
    /* the drawing's Follow picker offers the same people as the roster */
    clawt_gtk_office_view_set_participants(self, people);
    for (i = 0; people != NULL && i < json_array_get_length(people); i++)
        gtk_list_box_append(self->office_list,
                            roster_row(json_array_get_object_element(people, i), now));
}

void
clawt_gtk_refresh_office(ClawtWindow *self)
{
    if (self->office_list == NULL ||
        !clawt_gtk_refresh_enter(self, CLAWT_REFRESH_OFFICE))
        return;

    do {
        refresh_office_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_OFFICE));
}

GtkWidget *
clawt_gtk_build_office_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *heading = gtk_label_new("Who is where");

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);

    /* on or off, and which world: the Office settings live with the office */
    self->office_controls = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->office_controls, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->office_controls), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->office_controls), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->office_controls));

    /* the drawn office goes first; it says how to build it when it is missing */
    self->office_view_slot = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(box), self->office_view_slot);
    clawt_gtk_office_view_attach(self);

    gtk_widget_add_css_class(heading, "title-4");
    gtk_label_set_xalign(GTK_LABEL(heading), 0.0f);
    gtk_box_append(GTK_BOX(box), heading);

    self->office_status = gtk_label_new("");
    gtk_widget_add_css_class(self->office_status, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(self->office_status), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(self->office_status), TRUE);
    gtk_box_append(GTK_BOX(box), self->office_status);

    self->office_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->office_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->office_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->office_list), GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->office_list));

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    return scroll;
}
