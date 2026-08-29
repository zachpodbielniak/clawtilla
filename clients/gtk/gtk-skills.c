/*
 * gtk-skills.c - The Skills page, and `/name` in the composer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * There is deliberately **no popover** here.  The composer already has
 * a `GtkRevealer` above the entry for its built-in `/` commands, and
 * the comment on it in clawt-window-private.h records why: a popover
 * parented to the entry becomes one of that widget's children, and the
 * window then never maps at all -- no window, no log line, nothing to
 * attach a debugger to.  A skill command is the same kind of thing as a
 * built-in one, so it goes in the same list rather than in a second
 * control that would have to be positioned, dismissed and keyboard-
 * driven all over again.
 *
 * The expansion is asked of the daemon rather than done here, so this
 * client and the web one send byte-identical text for the same
 * `/name args`.
 */

#include "clawt-window-private.h"

#include <string.h>

/* ── The library ─────────────────────────────────────────────────── */

typedef struct {
    ClawtWindow *window;
    gchar       *name;
    gboolean     enable;
} SkillAction;

/*
 * The GClosureNotify shape, not a plain GDestroyNotify.
 *
 * g_signal_connect_data() passes the closure as a second argument, so a
 * one-argument free function has to be cast -- and the cast is what
 * -Wcast-function-type is warning about, correctly: on some ABIs it is
 * undefined behaviour rather than a formality.
 */
static void
skill_action_free(gpointer data, GClosure *closure)
{
    SkillAction *action = data;

    (void)closure;

    g_free(action->name);
    g_free(action);
}

static void
on_enable_clicked(GtkButton *button, gpointer user_data)
{
    SkillAction *action = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, action->name);
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, action->enable);
    json_builder_end_object(builder);

    reply = clawt_window_request(action->window, "skill.enable",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_skills(action->window);
}

static void
on_remove_confirmed(AdwAlertDialog *dialog, const gchar *response,
                    gpointer user_data)
{
    SkillAction *action = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    (void)dialog;

    if (g_strcmp0(response, "remove") != 0)
        return;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, action->name);
    json_builder_end_object(builder);

    reply = clawt_window_request(action->window, "skill.remove",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_skills(action->window);
}

static void
on_remove_clicked(GtkButton *button, gpointer user_data)
{
    SkillAction *action = user_data;
    SkillAction *owned;
    AdwDialog *dialog;
    g_autofree gchar *body = NULL;

    body = g_strdup_printf(
        "Delete the skill \"%s\" and take its links out of every agent's "
        "workspace? The directory and everything in it goes.", action->name);

    dialog = adw_alert_dialog_new("Remove this skill?", body);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                   "cancel", "Cancel",
                                   "remove", "Remove", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                             "remove",
                                             ADW_RESPONSE_DESTRUCTIVE);

    /*
     * The dialog gets its own copy rather than borrowing the button's.
     *
     * The button belongs to a list this page rebuilds on every
     * `skill.changed` event, and events are delivered from an idle --
     * so a rebuild can happen while the dialog is open, taking the
     * button's closure data with it and leaving the response handler
     * reading freed memory.
     */
    owned = g_new0(SkillAction, 1);
    owned->window = action->window;
    owned->name = g_strdup(action->name);

    g_signal_connect_data(dialog, "response",
                          G_CALLBACK(on_remove_confirmed), owned,
                          skill_action_free, 0);
    adw_dialog_present(dialog, GTK_WIDGET(button));
}

/*
 * One skill, as a group of rows.
 *
 * The warnings go at the top, above the description and above the
 * buttons.  Placement is the whole point of them: somebody on this page
 * is deciding whether to enable something, and a warning below the
 * button is a warning that gets enabled around.
 */
static GtkWidget *
skill_group(ClawtWindow *self, JsonObject *skill)
{
    GtkWidget *group = adw_preferences_group_new();
    const gchar *name = clawt_json_string(skill, "name", "?");
    gboolean enabled = json_object_has_member(skill, "enabled") &&
                       json_object_get_boolean_member(skill, "enabled");
    JsonArray *warnings = json_object_has_member(skill, "warnings")
                          ? json_object_get_array_member(skill, "warnings")
                          : NULL;
    JsonArray *skipped = json_object_has_member(skill, "skipped")
                         ? json_object_get_array_member(skill, "skipped")
                         : NULL;
    GtkWidget *buttons;
    GtkWidget *toggle;
    GtkWidget *remove;
    SkillAction *action;
    guint i;

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), name);
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        clawt_json_string(skill, "description", NULL));

    if (!enabled) {
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "Not enabled");
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(row),
            "Nothing in this skill reaches an agent yet. Read it first: "
            "what a skill says goes into a model's context with your "
            "agent's own authority.");
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    for (i = 0; warnings != NULL && i < json_array_get_length(warnings);
         i++) {
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Warning");
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(row),
            json_array_get_string_element(warnings, i));
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    if (skipped != NULL && json_array_get_length(skipped) > 0) {
        GtkWidget *row = adw_action_row_new();
        g_autoptr(GString) list = g_string_new(NULL);

        for (i = 0; i < json_array_get_length(skipped); i++) {
            if (i > 0)
                g_string_append(list, ", ");

            g_string_append(list, json_array_get_string_element(skipped, i));
        }

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "Not copied");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), list->str);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    {
        GtkWidget *row = adw_action_row_new();
        g_autofree gchar *detail = NULL;

        detail = g_strdup_printf(
            "%s%s%s", clawt_json_string(skill, "source", "user"),
            clawt_json_string(skill, "origin", NULL) != NULL ? ", from " : "",
            clawt_json_string(skill, "origin", ""));

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Provenance");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), detail);
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    {
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Directory");
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(row), clawt_json_string(skill, "directory", ""));
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    gtk_widget_set_margin_top(buttons, 6);

    toggle = gtk_button_new_with_label(enabled ? "Disable" : "Enable");

    if (!enabled)
        gtk_widget_add_css_class(toggle, "suggested-action");

    action = g_new0(SkillAction, 1);
    action->window = self;
    action->name = g_strdup(name);
    action->enable = !enabled;
    g_signal_connect_data(toggle, "clicked", G_CALLBACK(on_enable_clicked),
                          action, skill_action_free, 0);
    gtk_box_append(GTK_BOX(buttons), toggle);

    remove = gtk_button_new_with_label("Remove");
    gtk_widget_add_css_class(remove, "destructive-action");

    action = g_new0(SkillAction, 1);
    action->window = self;
    action->name = g_strdup(name);
    g_signal_connect_data(remove, "clicked", G_CALLBACK(on_remove_clicked),
                          action, skill_action_free, 0);
    gtk_box_append(GTK_BOX(buttons), remove);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), buttons);

    return group;
}

/* ── Teaching a task ─────────────────────────────────────────────── */

/*
 * One button on a recording.
 *
 * The id travels, never the widget: the list is rebuilt whenever a
 * teach.changed event arrives, which can happen between a click being
 * queued and the handler running.
 */
typedef struct {
    ClawtWindow *window;
    gchar       *id;
    gchar       *kind;
} TeachAction;

static void
teach_action_free(gpointer data, GClosure *closure)
{
    TeachAction *action = data;

    (void)closure;

    g_free(action->id);
    g_free(action->kind);
    g_free(action);
}

static void
on_teach_action(GtkButton *button, gpointer user_data)
{
    TeachAction *action = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, action->id);
    json_builder_end_object(builder);

    reply = clawt_window_request(action->window, action->kind,
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    if (g_strcmp0(action->kind, "teach.synthesize") == 0)
        clawt_window_toast(action->window,
                           "Drafted. Read it, then commit it.");
    else if (g_strcmp0(action->kind, "teach.commit") == 0)
        clawt_window_toast(action->window,
                           "Written, and disabled. Read it before you "
                           "enable it.");

    clawt_gtk_refresh_skills(action->window);
}

/*
 * The steps, fetched only when somebody asks for them.
 *
 * `teach.list` deliberately answers without them -- a demonstration can
 * be twenty thousand steps and a listing that carried every one of them
 * would be a listing nobody could load.
 */
static void
on_teach_show(GtkButton *button, gpointer user_data)
{
    TeachAction *action = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GString) text = g_string_new(NULL);
    JsonObject *trace;
    JsonArray *steps;
    AdwDialog *dialog;
    GtkWidget *scroll;
    GtkWidget *label;
    guint i;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, action->id);
    json_builder_end_object(builder);

    reply = clawt_window_request(action->window, "teach.show",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    trace = clawt_payload_of(reply);
    steps = json_object_get_array_member(trace, "steps");

    for (i = 0; steps != NULL && i < json_array_get_length(steps); i++) {
        JsonObject *step = json_array_get_object_element(steps, i);

        g_string_append_printf(text, "%u. [%s] %s\n", i + 1,
                               clawt_json_string(step, "kind", "?"),
                               clawt_json_string(step, "label", ""));

        if (clawt_json_string(step, "detail", NULL) != NULL)
            g_string_append_printf(text, "    %s\n",
                                   clawt_json_string(step, "detail", ""));
    }

    if (text->len == 0)
        g_string_append(text, "Nothing was captured.");

    dialog = adw_alert_dialog_new("What was recorded", NULL);
    label = gtk_label_new(text->str);
    scroll = gtk_scrolled_window_new();

    gtk_label_set_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(label), TRUE);
    gtk_widget_set_size_request(scroll, 480, 400);

    /*
     * NEVER horizontally, for the reason every other list here uses it:
     * a wrapping label reports its unwrapped width as natural, and an
     * AUTOMATIC policy hands it exactly that.
     */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), label);

    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), scroll);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                   "close", "Close", NULL);
    adw_dialog_present(dialog, GTK_WIDGET(button));
}

static void
add_teach_button(ClawtWindow *self, GtkWidget *box, const gchar *label,
                 const gchar *kind, const gchar *id, GCallback callback,
                 const gchar *css)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    TeachAction *action = g_new0(TeachAction, 1);

    action->window = self;
    action->id = g_strdup(id);
    action->kind = g_strdup(kind);

    if (css != NULL)
        gtk_widget_add_css_class(button, css);

    g_signal_connect_data(button, "clicked", callback, action,
                          teach_action_free, 0);
    gtk_box_append(GTK_BOX(box), button);
}

static void
on_teach_start_response(AdwAlertDialog *dialog, const gchar *response,
                        gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *agent = g_object_get_data(G_OBJECT(dialog), "agent");
    GtkWidget *kind = g_object_get_data(G_OBJECT(dialog), "kind");
    GtkWidget *goal = g_object_get_data(G_OBJECT(dialog), "goal");
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    guint selected;

    if (g_strcmp0(response, "record") != 0)
        return;

    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(kind));

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder,
                                  gtk_editable_get_text(GTK_EDITABLE(agent)));
    json_builder_set_member_name(builder, "source");
    json_builder_add_string_value(builder,
                                  clawt_teach_source_nth_nick(selected));

    if (*gtk_editable_get_text(GTK_EDITABLE(goal)) != '\0') {
        json_builder_set_member_name(builder, "goal");
        json_builder_add_string_value(
            builder, gtk_editable_get_text(GTK_EDITABLE(goal)));
    }

    json_builder_end_object(builder);

    reply = clawt_window_request(self, "teach.start",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_skills(self);
}

static void
on_teach_start_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwDialog *dialog;
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *agent = adw_entry_row_new();
    GtkWidget *kind = adw_combo_row_new();
    GtkWidget *goal = adw_entry_row_new();
    GtkStringList *kinds = gtk_string_list_new(NULL);
    guint i;

    dialog = adw_alert_dialog_new(
        "Record a task",
        "Watching the agent captures the calls it makes. Demonstrating "
        "captures every key you press, in any window -- read the trace "
        "before you turn it into a skill.");

    /*
     * Walked from the library rather than listed here.  A client with a
     * list of its own is a client that can disagree with the daemon
     * about what exists, and every hand-written copy in this tree has.
     */
    for (i = 0; i < clawt_teach_source_count(); i++)
        gtk_string_list_append(kinds, clawt_teach_source_nth_label(i));

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(agent), "Agent");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(kind), "What to watch");
    adw_combo_row_set_model(ADW_COMBO_ROW(kind), G_LIST_MODEL(kinds));
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(goal),
                                  "What you are teaching");

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), agent);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), kind);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), goal);

    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), group);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                   "cancel", "Cancel",
                                   "record", "Record", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                             "record",
                                             ADW_RESPONSE_SUGGESTED);

    g_object_set_data(G_OBJECT(dialog), "agent", agent);
    g_object_set_data(G_OBJECT(dialog), "kind", kind);
    g_object_set_data(G_OBJECT(dialog), "goal", goal);
    g_signal_connect(dialog, "response",
                     G_CALLBACK(on_teach_start_response), self);
    adw_dialog_present(dialog, GTK_WIDGET(button));
}

/*
 * The recordings, above the skills.
 *
 * Above because a running recording is a thing happening to somebody's
 * screen right now, and the list of skills is a thing that will still
 * be there later.
 */
static GtkWidget *
teach_group(ClawtWindow *self)
{
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *record = gtk_button_new_with_label("Record a task");
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *recordings = NULL;
    guint i;

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Teach a task");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Record a task being done, then have a model write the procedure "
        "up as a skill. The draft lands disabled, with the same checks an "
        "imported skill gets.");

    reply = clawt_window_request(self, "teach.list", NULL);

    if (reply != NULL)
        recordings = json_object_get_array_member(clawt_payload_of(reply),
                                                  "recordings");

    for (i = 0; recordings != NULL && i < json_array_get_length(recordings);
         i++) {
        JsonObject *trace = json_array_get_object_element(recordings, i);
        const gchar *id = clawt_json_string(trace, "id", "?");
        gboolean active = json_object_get_boolean_member(trace, "active");
        JsonArray *caveats = json_object_get_array_member(trace, "caveats");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        g_autofree gchar *subtitle = NULL;
        guint c;

        subtitle = g_strdup_printf(
            "%s, agent %s, %" G_GINT64_FORMAT " step(s)%s",
            clawt_json_string(trace, "source", "?"),
            clawt_json_string(trace, "agent", "-"),
            json_object_get_int_member(trace, "step_count"),
            active ? " -- recording now" : "");

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      clawt_json_string(trace, "goal", id));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
        adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(row), 0);

        gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);

        if (active)
            add_teach_button(self, buttons, "Stop", "teach.stop", id,
                             G_CALLBACK(on_teach_action), "destructive-action");
        else {
            add_teach_button(self, buttons, "Steps", "teach.show", id,
                             G_CALLBACK(on_teach_show), NULL);
            add_teach_button(self, buttons, "Draft a skill",
                             "teach.synthesize", id,
                             G_CALLBACK(on_teach_action), "suggested-action");
            add_teach_button(self, buttons, "Commit", "teach.commit", id,
                             G_CALLBACK(on_teach_action), NULL);
            add_teach_button(self, buttons, "Remove", "teach.remove", id,
                             G_CALLBACK(on_teach_action), NULL);
        }

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), buttons);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);

        /*
         * The caveat under every recording, not in a document.  This is
         * where somebody decides whether the trace is safe to keep, and
         * a limitation they have to go and look up is one they will not.
         */
        for (c = 0; caveats != NULL && c < json_array_get_length(caveats);
             c++) {
            GtkWidget *note = adw_action_row_new();

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(note),
                                               FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(note),
                                          "Before you share this");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(note),
                json_array_get_string_element(caveats, c));
            adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(note), 0);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), note);
        }
    }

    gtk_widget_set_halign(record, GTK_ALIGN_END);
    gtk_widget_set_margin_top(record, 6);
    g_signal_connect(record, "clicked",
                     G_CALLBACK(on_teach_start_clicked), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), record);

    return group;
}

void
clawt_gtk_refresh_skills(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *payload;
    JsonArray *skills;
    JsonArray *problems;
    GtkWidget *child;
    guint i;

    if (self->skill_box == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_SKILLS))
        return;

    do {
        while ((child = gtk_widget_get_first_child(self->skill_box)) != NULL)
            gtk_box_remove(GTK_BOX(self->skill_box), child);

        gtk_box_append(GTK_BOX(self->skill_box), teach_group(self));

        reply = clawt_window_request(self, "skill.list", NULL);

        if (reply == NULL)
            continue;

        payload = clawt_payload_of(reply);
        skills = json_object_get_array_member(payload, "skills");
        problems = json_object_get_array_member(payload, "problems");

        for (i = 0; problems != NULL && i < json_array_get_length(problems);
             i++) {
            GtkWidget *banner =
                adw_banner_new(json_array_get_string_element(problems, i));

            adw_banner_set_revealed(ADW_BANNER(banner), TRUE);
            gtk_box_append(GTK_BOX(self->skill_box), banner);
        }

        if (skills == NULL || json_array_get_length(skills) == 0) {
            GtkWidget *status = adw_status_page_new();
            gboolean on = json_object_has_member(payload, "enabled") &&
                          json_object_get_boolean_member(payload, "enabled");

            /*
             * Two causes, and they send somebody to different places.
             * Anywhere an empty result could read as an answer, say why
             * it is empty.
             */
            adw_status_page_set_title(
                ADW_STATUS_PAGE(status),
                on ? "No skills yet" : "Skills are turned off");
            adw_status_page_set_description(
                ADW_STATUS_PAGE(status),
                on ? clawt_json_string(payload, "directory",
                                       "Nothing in the skills directory.")
                   : "Set skills.enabled to scan and link them.");
            gtk_widget_set_vexpand(status, TRUE);
            gtk_box_append(GTK_BOX(self->skill_box), status);
            continue;
        }

        for (i = 0; i < json_array_get_length(skills); i++)
            gtk_box_append(GTK_BOX(self->skill_box),
                           skill_group(self, json_array_get_object_element(
                                                 skills, i)));
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_SKILLS));
}

static void
on_import_response(AdwAlertDialog *dialog, const gchar *response,
                   gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *source = g_object_get_data(G_OBJECT(dialog), "source");
    GtkWidget *origin = g_object_get_data(G_OBJECT(dialog), "origin");
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "import") != 0)
        return;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "source");
    json_builder_add_string_value(builder,
                                  gtk_editable_get_text(
                                      GTK_EDITABLE(source)));

    if (*gtk_editable_get_text(GTK_EDITABLE(origin)) != '\0') {
        json_builder_set_member_name(builder, "origin");
        json_builder_add_string_value(
            builder, gtk_editable_get_text(GTK_EDITABLE(origin)));
    }

    json_builder_end_object(builder);

    reply = clawt_window_request(self, "skill.import",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Imported, disabled. Read it, then enable it.");
    clawt_gtk_refresh_skills(self);
}

static void
on_import_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwDialog *dialog;
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *source = adw_entry_row_new();
    GtkWidget *origin = adw_entry_row_new();

    dialog = adw_alert_dialog_new(
        "Import a skill",
        "A directory holding a SKILL.md, on the machine the daemon runs "
        "on. It arrives disabled, and any script beside it is left where "
        "it is -- markdown only, on purpose.");

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(source), "Directory");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(origin),
                                  "Where it came from");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), source);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), origin);

    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), group);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                   "cancel", "Cancel",
                                   "import", "Import", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                             "import",
                                             ADW_RESPONSE_SUGGESTED);

    g_object_set_data(G_OBJECT(dialog), "source", source);
    g_object_set_data(G_OBJECT(dialog), "origin", origin);
    g_signal_connect(dialog, "response", G_CALLBACK(on_import_response),
                     self);
    adw_dialog_present(dialog, GTK_WIDGET(button));
}

static void
on_new_response(AdwAlertDialog *dialog, const gchar *response,
                gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *name = g_object_get_data(G_OBJECT(dialog), "name");
    GtkWidget *description = g_object_get_data(G_OBJECT(dialog),
                                               "description");
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "create") != 0)
        return;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder,
                                  gtk_editable_get_text(GTK_EDITABLE(name)));
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(description)));
    json_builder_end_object(builder);

    reply = clawt_window_request(self, "skill.create",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_skills(self);
}

static void
on_new_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwDialog *dialog;
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *name = adw_entry_row_new();
    GtkWidget *description = adw_entry_row_new();

    dialog = adw_alert_dialog_new(
        "Write a skill",
        "Lowercase letters, digits and single hyphens. The description is "
        "the only part an agent reads before deciding to open it, so write "
        "it as \"use this when ...\".");

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(name), "Name");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(description),
                                  "Description");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), name);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), description);

    adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), group);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog),
                                   "cancel", "Cancel",
                                   "create", "Create", NULL);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog),
                                             "create",
                                             ADW_RESPONSE_SUGGESTED);

    g_object_set_data(G_OBJECT(dialog), "name", name);
    g_object_set_data(G_OBJECT(dialog), "description", description);
    g_signal_connect(dialog, "response", G_CALLBACK(on_new_response), self);
    adw_dialog_present(dialog, GTK_WIDGET(button));
}

static void
on_reload_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    reply = clawt_window_request(self, "skill.reload", NULL);

    if (reply == NULL)
        return;

    clawt_gtk_refresh_skills(self);
}

GtkWidget *
clawt_gtk_build_skill_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    GtkWidget *import = gtk_button_new_with_label("Import");
    GtkWidget *create = gtk_button_new_with_label("New skill");
    GtkWidget *reload = gtk_button_new_with_label("Rescan");

    self->skill_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_halign(bar, GTK_ALIGN_END);
    gtk_widget_set_hexpand(bar, TRUE);

    g_signal_connect(reload, "clicked", G_CALLBACK(on_reload_clicked), self);
    g_signal_connect(import, "clicked", G_CALLBACK(on_import_clicked), self);
    g_signal_connect(create, "clicked", G_CALLBACK(on_new_clicked), self);

    gtk_box_append(GTK_BOX(bar), reload);
    gtk_box_append(GTK_BOX(bar), import);
    gtk_box_append(GTK_BOX(bar), create);

    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), self->skill_box);

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    /*
     * Horizontally NEVER, so a long warning wraps instead of making the
     * page as wide as itself.  A wrapping GtkLabel still reports the
     * unwrapped string as its natural width, and an AUTOMATIC policy
     * gives its child exactly that -- so the whole page scrolls
     * sideways, nothing is ellipsised, and nothing is logged.
     */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);

    return scroll;
}

/* ── `/name` in the composer ─────────────────────────────────────── */

/*
 * The agent's own commands, cached for as long as the line stays a
 * command.
 *
 * Asked once rather than per keystroke: the answer cannot change
 * between two characters, and a round trip per key would make typing
 * feel like the daemon was struggling. Dropped by
 * clawt_gtk_skill_commands_forget() whenever the line stops being a
 * command or the selection changes, so a skill enabled while somebody
 * has the window open is picked up the next time they type a slash.
 */
JsonNode *
clawt_gtk_skill_commands(ClawtWindow *self)
{
    g_autoptr(JsonBuilder) builder = NULL;

    if (self->slash_commands != NULL)
        return self->slash_commands;

    if (self->selected_agent == NULL)
        return NULL;

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, self->selected_agent);
    json_builder_end_object(builder);

    self->slash_commands = clawt_window_request(self, "skill.commands",
                                                json_builder_get_root(
                                                    builder));

    return self->slash_commands;
}

void
clawt_gtk_skill_commands_forget(ClawtWindow *self)
{
    g_clear_pointer(&self->slash_commands, json_node_unref);
}

gboolean
clawt_gtk_skill_expand(ClawtWindow *self, const gchar *line, gchar **out)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *name = NULL;
    const gchar *space;
    const gchar *arguments = NULL;

    if (out != NULL)
        *out = NULL;

    if (line == NULL || line[0] != '/' || self->selected_agent == NULL)
        return FALSE;

    space = strchr(line, ' ');

    if (space != NULL) {
        name = g_strndup(line + 1, (gsize)(space - line - 1));
        arguments = space + 1;
    } else {
        name = g_strdup(line + 1);
    }

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, self->selected_agent);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);

    if (arguments != NULL && *arguments != '\0') {
        json_builder_set_member_name(builder, "arguments");
        json_builder_add_string_value(builder, arguments);
    }

    json_builder_end_object(builder);

    /*
     * Expanded by the daemon, so this client and the web one send
     * byte-identical text for the same `/name args`. Two substitutions
     * would agree until somebody typed a `$`.
     */
    reply = clawt_window_request(self, "skill.expand",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return FALSE;

    if (out != NULL)
        *out = g_strdup(clawt_json_string(clawt_payload_of(reply), "prompt",
                                          NULL));

    return TRUE;
}
