/*
 * gtk-routines.c - The routines page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Work an agent does on a schedule, and the editor that describes one.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Routines ────────────────────────────────────────────────────── */

/*
 * One routine being written.
 *
 * The schedule is a segmented control over the presets with a cron field
 * behind Custom, because those are two different questions: most people
 * want "weekdays at nine" and should never meet an expression, and the
 * ones who want every six hours should not have to bend it into a preset
 * that cannot hold it.
 *
 * (An expression with a step in it cannot be written in a C comment
 * without ending it, which is its own small argument for the field.)
 */
typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    gchar       *id;
    gboolean     creating;

    GtkWidget   *id_row;
    GtkWidget   *description_row;
    GtkTextView *instructions;
    GtkWidget   *agent_row;
    GStrv        agent_ids;
    GtkWidget   *directory_row;
    GtkWidget   *worktree_row;
    GtkWidget   *enabled_row;
    GtkWidget   *catch_up_row;
    GtkWidget   *jitter_row;

    GtkWidget   *schedule_row;
    GtkWidget   *at_row;
    GtkWidget   *weekday_row;
    GtkWidget   *cron_row;
    GtkWidget   *preview;
} RoutineDialog;

static void
routine_dialog_free(gpointer data)
{
    RoutineDialog *dialog = data;

    g_free(dialog->id);
    g_strfreev(dialog->agent_ids);
    g_free(dialog);
}

static const gchar *const schedule_ids[] = {
    "manual", "hourly", "daily", "weekdays", "weekly", "custom"
};

static const gchar *const weekday_ids[] = {
    "sunday", "monday", "tuesday", "wednesday", "thursday", "friday",
    "saturday"
};

/*
 * Shows only the fields the chosen schedule uses, and says when it will
 * next run.
 *
 * The preview is the part that earns its place: an expression is not
 * something a person can check by reading it, and "next: Mon 25 Aug
 * 09:00" is.
 */
static void
update_schedule_rows(RoutineDialog *dialog)
{
    guint selected = adw_combo_row_get_selected(
        ADW_COMBO_ROW(dialog->schedule_row));
    const gchar *preset = schedule_ids[MIN(selected,
                                           G_N_ELEMENTS(schedule_ids) - 1)];
    gboolean timed = g_strcmp0(preset, "daily") == 0 ||
                     g_strcmp0(preset, "weekdays") == 0 ||
                     g_strcmp0(preset, "weekly") == 0 ||
                     g_strcmp0(preset, "hourly") == 0;
    gboolean custom = g_strcmp0(preset, "custom") == 0;

    gtk_widget_set_visible(dialog->at_row, timed);
    gtk_widget_set_visible(dialog->weekday_row,
                           g_strcmp0(preset, "weekly") == 0);
    gtk_widget_set_visible(dialog->cron_row, custom);

    if (g_strcmp0(preset, "manual") == 0) {
        gtk_label_set_text(GTK_LABEL(dialog->preview),
                           "Only when you ask.");
        return;
    }

    gtk_label_set_text(GTK_LABEL(dialog->preview),
                       custom ? "minute hour day-of-month month day-of-week"
                              : "");
}

static void
on_schedule_changed(GObject *row, GParamSpec *spec, gpointer user_data)
{
    (void)row;
    (void)spec;

    update_schedule_rows(user_data);
}

static gchar *
instructions_text(RoutineDialog *dialog)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(dialog->instructions);
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);

    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void
on_routine_saved(GtkButton *button, gpointer user_data)
{
    RoutineDialog *dialog = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *instructions = instructions_text(dialog);
    const gchar *id = dialog->creating
        ? gtk_editable_get_text(GTK_EDITABLE(dialog->id_row)) : dialog->id;
    guint schedule = adw_combo_row_get_selected(
        ADW_COMBO_ROW(dialog->schedule_row));
    guint weekday = adw_combo_row_get_selected(
        ADW_COMBO_ROW(dialog->weekday_row));
    guint agent = adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->agent_row));

    (void)button;

    if (id == NULL || *id == '\0') {
        clawt_window_toast(dialog->window, "It needs a name.");
        return;
    }

    if (*instructions == '\0') {
        /*
         * Refused rather than saved empty: a routine with no
         * instructions fires on schedule and asks an agent for nothing,
         * which costs a turn and produces a puzzled reply.
         */
        clawt_window_toast(dialog->window,
                           "It needs instructions -- that is the whole of "
                           "what the agent is asked.");
        return;
    }

    if (dialog->agent_ids == NULL || dialog->agent_ids[0] == NULL) {
        clawt_window_toast(dialog->window,
                           "There is no agent to run it.");
        return;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, id);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(
        builder, dialog->agent_ids[MIN(agent,
                                       g_strv_length(dialog->agent_ids) - 1)]);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->description_row)));
    json_builder_set_member_name(builder, "instructions");
    json_builder_add_string_value(builder, instructions);
    json_builder_set_member_name(builder, "schedule");
    json_builder_add_string_value(
        builder, schedule_ids[MIN(schedule,
                                  G_N_ELEMENTS(schedule_ids) - 1)]);
    json_builder_set_member_name(builder, "at");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->at_row)));
    json_builder_set_member_name(builder, "weekday");
    json_builder_add_string_value(
        builder, weekday_ids[MIN(weekday, G_N_ELEMENTS(weekday_ids) - 1)]);
    json_builder_set_member_name(builder, "cron");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->cron_row)));
    json_builder_set_member_name(builder, "directory");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->directory_row)));
    json_builder_set_member_name(builder, "worktree");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(
                     ADW_SWITCH_ROW(dialog->worktree_row)));
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(
                     ADW_SWITCH_ROW(dialog->enabled_row)));
    json_builder_set_member_name(builder, "catch_up");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(
                     ADW_SWITCH_ROW(dialog->catch_up_row)));
    json_builder_set_member_name(builder, "jitter_seconds");
    json_builder_add_int_value(
        builder, g_ascii_strtoll(
                     gtk_editable_get_text(GTK_EDITABLE(dialog->jitter_row)),
                     NULL, 10));
    json_builder_end_object(builder);

    reply = clawt_window_request(dialog->window,
                                 dialog->creating ? "routine.add"
                                                  : "routine.update",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_routines(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_routine_removed(GtkButton *button, gpointer user_data)
{
    RoutineDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    reply = clawt_window_request(dialog->window, "routine.remove",
                                 clawt_build_payload("id", dialog->id, NULL));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_routines(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_routine_run(GtkButton *button, gpointer user_data)
{
    RoutineDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    reply = clawt_window_request(dialog->window, "routine.run",
                                 clawt_build_payload("id", dialog->id, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window, "Started. It is in Tasks.");
    clawt_gtk_refresh_routines(dialog->window);
}

static void
open_routine_editor(ClawtWindow *self, JsonObject *existing)
{
    RoutineDialog *dialog = g_new0(RoutineDialog, 1);
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *what = adw_preferences_group_new();
    GtkWidget *where = adw_preferences_group_new();
    GtkWidget *when = adw_preferences_group_new();
    GtkWidget *actions = adw_preferences_group_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkStringList *agent_labels = gtk_string_list_new(NULL);
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(JsonNode) agents = NULL;
    GtkWidget *save;
    static const gchar *const schedules[] = {
        "Manual", "Hourly", "Daily", "Weekdays", "Weekly", "Custom", NULL
    };
    static const gchar *const weekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
        "Saturday", NULL
    };
    guint i;

    dialog->window = self;
    dialog->dialog = window;
    dialog->creating = existing == NULL;
    dialog->id = g_strdup(existing != NULL
                          ? clawt_json_string(existing, "id", "") : "");

    adw_dialog_set_title(window, dialog->creating ? "New routine"
                                                  : dialog->id);
    adw_dialog_set_content_width(window, 620);
    adw_dialog_set_content_height(window, 760);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(what),
                                    "What it does");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(what),
        "Routines only run while the daemon is running. A machine that "
        "was asleep at nine o'clock has missed nine o'clock.");

    dialog->id_row = clawt_gtk_add_entry(what, "Name", dialog->id);
    gtk_widget_set_sensitive(dialog->id_row, dialog->creating);

    dialog->description_row = clawt_gtk_add_entry(
        what, "Description",
        existing != NULL ? clawt_json_string(existing, "description", "")
                         : "");

    /*
     * A text view rather than an entry: the instructions are the whole
     * of the prompt, and a single line teaches people to write a
     * reminder where an instruction is needed.
     */
    dialog->instructions = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(dialog->instructions, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(dialog->instructions, 8);
    gtk_text_view_set_bottom_margin(dialog->instructions, 8);
    gtk_text_view_set_left_margin(dialog->instructions, 8);
    gtk_text_view_set_right_margin(dialog->instructions, 8);
    gtk_widget_set_size_request(GTK_WIDGET(dialog->instructions), -1, 160);

    if (existing != NULL)
        gtk_text_buffer_set_text(
            gtk_text_view_get_buffer(dialog->instructions),
            clawt_json_string(existing, "instructions", ""), -1);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(dialog->instructions));
    gtk_widget_add_css_class(scroll, "card");
    gtk_widget_set_margin_top(scroll, 6);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(what), scroll);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(where),
                                    "Who and where");

    agents = clawt_window_request(self, "agent.list", NULL);

    if (agents != NULL) {
        JsonArray *array = json_object_get_array_member(
            clawt_payload_of(agents), "agents");

        for (i = 0; i < json_array_get_length(array); i++) {
            JsonObject *agent = json_array_get_object_element(array, i);
            const gchar *id = clawt_json_string(agent, "id", "");

            gtk_string_list_append(agent_labels,
                                   clawt_json_string(agent, "name", id));
            g_ptr_array_add(ids, g_strdup(id));
        }
    }

    g_ptr_array_add(ids, NULL);
    dialog->agent_ids = (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);

    dialog->agent_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->agent_row),
                                  "Agent");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->agent_row),
                            G_LIST_MODEL(agent_labels));

    if (existing != NULL) {
        const gchar *agent = clawt_json_string(existing, "agent", "");

        for (i = 0; dialog->agent_ids[i] != NULL; i++) {
            if (g_strcmp0(dialog->agent_ids[i], agent) == 0)
                adw_combo_row_set_selected(ADW_COMBO_ROW(dialog->agent_row),
                                           i);
        }
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(where),
                              dialog->agent_row);

    dialog->directory_row = clawt_gtk_add_entry(
        where, "Folder",
        existing != NULL ? clawt_json_string(existing, "directory", "") : "");
    clawt_gtk_set_row_hint(
        dialog->directory_row,
        "On the agent's computer. Empty means its workspace.");

    dialog->worktree_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->worktree_row),
                                  "Use a git worktree");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->worktree_row),
        "For a routine that changes files: keeps a scheduled run off "
        "whatever you had checked out at the time.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->worktree_row),
        existing != NULL &&
        json_object_get_boolean_member(existing, "worktree"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(where),
                              dialog->worktree_row);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(when), "When");

    dialog->schedule_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->schedule_row),
                                  "Schedule");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->schedule_row),
                            G_LIST_MODEL(gtk_string_list_new(schedules)));

    if (existing != NULL) {
        const gchar *schedule = clawt_json_string(existing, "schedule",
                                                  "daily");

        for (i = 0; i < G_N_ELEMENTS(schedule_ids); i++) {
            if (g_strcmp0(schedule_ids[i], schedule) == 0)
                adw_combo_row_set_selected(
                    ADW_COMBO_ROW(dialog->schedule_row), i);
        }
    } else {
        adw_combo_row_set_selected(ADW_COMBO_ROW(dialog->schedule_row), 2);
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when),
                              dialog->schedule_row);

    dialog->at_row = clawt_gtk_add_entry(
        when, "At",
        existing != NULL ? clawt_json_string(existing, "at", "09:00")
                         : "09:00");
    clawt_gtk_set_row_hint(dialog->at_row,
                           "Local time, such as 09:00");

    dialog->weekday_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->weekday_row),
                                  "Day");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->weekday_row),
                            G_LIST_MODEL(gtk_string_list_new(weekdays)));

    if (existing != NULL) {
        const gchar *day = clawt_json_string(existing, "weekday", "monday");

        for (i = 0; i < G_N_ELEMENTS(weekday_ids); i++) {
            if (g_strcmp0(weekday_ids[i], day) == 0)
                adw_combo_row_set_selected(
                    ADW_COMBO_ROW(dialog->weekday_row), i);
        }
    } else {
        adw_combo_row_set_selected(ADW_COMBO_ROW(dialog->weekday_row), 1);
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when),
                              dialog->weekday_row);

    dialog->cron_row = clawt_gtk_add_entry(
        when, "Cron expression",
        existing != NULL ? clawt_json_string(existing, "cron", "") : "");
    clawt_gtk_set_row_hint(
        dialog->cron_row,
        "Note: with both day fields set, it is day-of-month OR "
        "day-of-week.");

    dialog->preview = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(dialog->preview), TRUE);
    gtk_label_set_xalign(GTK_LABEL(dialog->preview), 0.0f);
    gtk_widget_add_css_class(dialog->preview, "dim-label");
    gtk_widget_set_margin_top(dialog->preview, 6);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when), dialog->preview);

    dialog->enabled_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->enabled_row),
                                  "Enabled");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->enabled_row),
        "A disabled routine can still be run by hand, which is how you "
        "try one before trusting it.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->enabled_row),
        existing == NULL ||
        json_object_get_boolean_member(existing, "enabled"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when),
                              dialog->enabled_row);

    dialog->catch_up_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->catch_up_row),
                                  "Run a missed one at startup");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->catch_up_row),
        "Once, however many were missed.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->catch_up_row),
        existing != NULL &&
        json_object_get_boolean_member(existing, "catch_up"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when),
                              dialog->catch_up_row);

    /*
     * Offered here because the option now does something.
     *
     * This dialog names its eleven keys by hand rather than walking the
     * schema, which the web client does -- so implementing an option
     * reaches that client for free and this one not at all, and the
     * symptom is a setting somebody can only reach from a browser.
     */
    dialog->jitter_row = clawt_gtk_add_entry(
        when, "Jitter (seconds)",
        existing != NULL ? clawt_json_string(existing, "jitter_seconds", "0")
                         : "0");
    clawt_gtk_set_row_hint(
        dialog->jitter_row,
        "Hold each scheduled run back by up to this long, chosen at "
        "random. Only worth setting when several routines share one "
        "rate-limited service; `run now` and a catch-up start at once "
        "either way.");

    g_signal_connect(dialog->schedule_row, "notify::selected",
                     G_CALLBACK(on_schedule_changed), dialog);

    save = gtk_button_new_with_label(dialog->creating ? "Create" : "Save");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_hexpand(save, TRUE);
    g_signal_connect(save, "clicked", G_CALLBACK(on_routine_saved), dialog);
    gtk_box_append(GTK_BOX(buttons), save);

    if (!dialog->creating) {
        GtkWidget *run = gtk_button_new_with_label("Run now");
        GtkWidget *remove = gtk_button_new_with_label("Remove");

        gtk_widget_set_hexpand(run, TRUE);
        g_signal_connect(run, "clicked", G_CALLBACK(on_routine_run), dialog);
        gtk_box_append(GTK_BOX(buttons), run);

        gtk_widget_add_css_class(remove, "destructive-action");
        gtk_widget_set_hexpand(remove, TRUE);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_routine_removed),
                         dialog);
        gtk_box_append(GTK_BOX(buttons), remove);
    }

    gtk_widget_set_margin_top(buttons, 12);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), buttons);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(what));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(where));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(when));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(actions));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(window, toolbar);

    g_object_set_data_full(G_OBJECT(window), "dialog", dialog,
                           routine_dialog_free);

    update_schedule_rows(dialog);
    adw_dialog_present(window, GTK_WIDGET(self));
}

static void
on_add_routine(GtkButton *button, gpointer user_data)
{
    (void)button;

    open_routine_editor(user_data, NULL);
}

static void
on_routine_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    JsonObject *routine;

    (void)box;

    if (row == NULL)
        return;

    routine = g_object_get_data(G_OBJECT(row), "routine");

    if (routine == NULL)
        return;

    open_routine_editor(self, routine);
}

void
clawt_gtk_refresh_routines(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *routines;
    guint i;

    if (self->routine_list == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_ROUTINES))
        return;

    do {
        clawt_gtk_clear_list(self->routine_list);

        reply = clawt_window_request(self, "routine.list", NULL);

        if (reply == NULL)
            continue;

        routines = json_object_get_array_member(clawt_payload_of(reply),
                                                "routines");

        for (i = 0; i < json_array_get_length(routines); i++) {
            JsonObject *routine = json_array_get_object_element(routines, i);
            const gchar *next = clawt_json_string(routine, "next_run", NULL);
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *subtitle = NULL;
            g_autofree gchar *when = NULL;

            if (next != NULL) {
                g_autoptr(GDateTime) parsed =
                    g_date_time_new_from_iso8601(next, NULL);

                when = (parsed != NULL)
                    ? g_date_time_format(parsed, "next %a %d %b at %H:%M")
                    : g_strdup(next);
            } else if (json_object_get_boolean_member(routine, "enabled")) {
                when = g_strdup("only when you ask");
            } else {
                when = g_strdup("off");
            }

            subtitle = g_strdup_printf(
                "%s \342\200\224 %s%s%s",
                clawt_json_string(routine, "agent", "?"), when,
                g_strcmp0(clawt_json_string(routine, "last_state", "never"),
                          "never") == 0 ? "" : ", last run ",
                g_strcmp0(clawt_json_string(routine, "last_state", "never"),
                          "never") == 0
                    ? "" : clawt_json_string(routine, "last_state", ""));

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(row),
                clawt_json_string(routine, "description",
                                  clawt_json_string(routine, "id", "?")));
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
            clawt_gtk_row_opens_something(row);

            g_object_set_data_full(G_OBJECT(row), "routine",
                                   json_object_ref(routine),
                                   (GDestroyNotify)json_object_unref);
            gtk_list_box_append(self->routine_list, row);
        }
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_ROUTINES));
}

GtkWidget *
clawt_gtk_build_routine_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add = gtk_button_new_with_label("New routine");
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    self->routine_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->routine_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->routine_list), "boxed-list");
    g_signal_connect(self->routine_list, "row-activated",
                     G_CALLBACK(on_routine_activated), self);

    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_set_halign(add, GTK_ALIGN_END);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_routine), self);
    gtk_widget_set_hexpand(bar, TRUE);
    gtk_box_append(GTK_BOX(bar), add);

    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->routine_list));

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);

    return scroll;
}
