/*
 * lrg-routines.c - The Routines page (mirrors clients/gtk/gtk-routines.c)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Work an agent does on a schedule, and the editor that describes one.
 *
 * Every decision -- what a row says, which badge it carries, what a
 * schedule means, what Save sends -- is in lrg-work-model.c; this file
 * only draws it and wires the buttons.
 *
 * The schedule is a choice of presets with a cron field behind Custom,
 * because those are two different questions: most people want
 * "weekdays at nine" and should never meet an expression, and the ones
 * who want every six hours should not have to bend it into a preset
 * that cannot hold it.  Under both sits a preview of the next three
 * runs, worked out with the daemon's own cron code: an expression is
 * not something a person can check by reading it, and three dates are.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-work-model.h"

typedef struct {
    ClawtLrgApp *app;
    LrgWidget   *page;
    LrgWidget   *list;
    guint        seq;      /* the newest routine.list asked for */
} Routines;

static Routines routines;

/* the leaves the editor draws a control of its own for */
static const gchar *const handled_leaves[] = {
    "description", "instructions", "agent", "enabled", "schedule", "at",
    "weekday", "cron", "directory", "worktree", "read_only", "proactive",
    "catch_up", "jitter_seconds", "max_start_failures", NULL
};

static void refresh_list(ClawtLrgApp *app, ClawtLrgRequestFlags flags);

/* ── The editor ───────────────────────────────────────────────────── */

/*
 * One routine being written.
 *
 * Reference-counted (g_rc_box) because two things outlive each other in
 * either order: the dialog, which releases its reference when it goes,
 * and a request in flight, which releases its own when the reply lands.
 * The content is held so a reply arriving after the dialog closed reads
 * widgets that still exist; the dialog itself is a weak pointer, because
 * holding it would be a cycle through its own response closure.
 */
typedef struct {
    ClawtLrgApp *app;
    LrgDialog   *dialog;       /* weak */
    LrgWidget   *content;      /* strong */
    gboolean     creating;
    gboolean     busy;
    gchar       *id;

    LrgWidget   *id_entry;
    LrgWidget   *description;
    LrgWidget   *instructions;
    LrgWidget   *agent;
    LrgWidget   *directory;
    LrgWidget   *worktree;
    LrgWidget   *schedule;
    LrgWidget   *at_picker;    /* a picker, or ... */
    LrgWidget   *at_entry;     /* ... text, for a value no picker holds */
    LrgWidget   *weekday;
    LrgWidget   *cron;
    LrgWidget   *preview;
    LrgWidget   *enabled;
    LrgWidget   *catch_up;
    LrgWidget   *read_only;
    LrgWidget   *proactive;
    LrgWidget   *jitter;
    LrgWidget   *max_failures;

    GPtrArray   *extra_fields;   /* ClawtLrgSchemaField */
    GPtrArray   *extra_controls; /* parallel: LrgWidget (borrowed) */
} RoutineEditor;

static void
routine_editor_clear(gpointer data)
{
    RoutineEditor *editor = data;

    if (editor->dialog != NULL)
        g_object_remove_weak_pointer(G_OBJECT(editor->dialog),
                                     (gpointer *)&editor->dialog);

    g_clear_object(&editor->content);
    g_clear_pointer(&editor->extra_fields, g_ptr_array_unref);
    g_clear_pointer(&editor->extra_controls, g_ptr_array_unref);
    g_free(editor->id);
}

static gpointer
editor_ref(RoutineEditor *editor)
{
    return g_rc_box_acquire(editor);
}

static void
editor_unref(gpointer data)
{
    g_rc_box_release_full(data, routine_editor_clear);
}

/* The field vbox clawt_lrg_form_add() wrapped @control in. */
static void
show_field(LrgWidget *control, gboolean visible)
{
    LrgContainer *field;

    if (control == NULL)
        return;

    field = lrg_widget_get_parent(control);
    lrg_widget_set_visible((field != NULL) ? LRG_WIDGET(field) : control,
                           visible);
}

/* The time of day as the daemon spells it. */
static gchar *
editor_at(RoutineEditor *editor)
{
    if (editor->at_picker != NULL)
        return g_strdup_printf(
            "%02d:%02d",
            lrg_time_picker_get_hour(LRG_TIME_PICKER(editor->at_picker)),
            lrg_time_picker_get_minute(LRG_TIME_PICKER(editor->at_picker)));

    return g_strdup(clawt_lrg_entry_text(editor->at_entry));
}

/*
 * Shows only the fields the chosen schedule reads, and says when it
 * will next run.
 */
static void
update_schedule(RoutineEditor *editor)
{
    const gchar *schedule = clawt_lrg_combo_id(editor->schedule);
    ClawtLrgScheduleUses uses = clawt_lrg_schedule_uses(schedule);
    g_autofree gchar *at = editor_at(editor);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *preview = NULL;
    g_autofree gchar *escaped = NULL;
    gboolean ok = FALSE;

    show_field((editor->at_picker != NULL) ? editor->at_picker
                                           : editor->at_entry, uses.at);
    show_field(editor->weekday, uses.weekday);
    show_field(editor->cron, uses.cron);

    preview = clawt_lrg_schedule_preview(schedule, at,
                                         clawt_lrg_combo_id(editor->weekday),
                                         clawt_lrg_entry_text(editor->cron),
                                         now, &ok);

    if (uses.minute_only) {
        g_autofree gchar *with = g_strdup_printf(
            "Hourly uses only the minute of the time.  %s", preview);

        g_free(preview);
        preview = g_steal_pointer(&with);
    }

    escaped = clawt_lrg_markdown_escape(preview);
    lrg_markdown_label_set_markdown(LRG_MARKDOWN_LABEL(editor->preview),
                                    escaped);

    if (ok)
        lrg_widget_remove_class(editor->preview, "clawt-tone-bad");
    else
        lrg_widget_add_class(editor->preview, "clawt-tone-bad");

    clawt_lrg_app_wake(editor->app);
}

static void
on_schedule_input(GObject *control, gpointer user_data)
{
    update_schedule(user_data);
}

static void
on_saved(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    RoutineEditor *editor = user_data;

    editor->busy = FALSE;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, editor->creating ? "Routine created"
                                          : "Routine saved");
    refresh_list(app, CLAWT_LRG_QUIET);

    if (editor->dialog != NULL && lrg_dialog_get_presented(editor->dialog))
        lrg_dialog_close(editor->dialog);
}

/* The extra schema fields, typed, or NULL with a toast if one is not. */
static JsonObject *
collect_extras(RoutineEditor *editor)
{
    JsonObject *extras = json_object_new();
    guint i;

    for (i = 0; i < editor->extra_fields->len; i++) {
        ClawtLrgSchemaField *field = g_ptr_array_index(editor->extra_fields,
                                                       i);
        LrgWidget *control = g_ptr_array_index(editor->extra_controls, i);
        g_autoptr(GError) error = NULL;
        JsonNode *value;

        if (field->kind == CLAWT_LRG_FIELD_SWITCH)
            value = clawt_lrg_work_field_value(
                field->kind, NULL, clawt_lrg_switch_active(control), &error);
        else
            value = clawt_lrg_work_field_value(
                field->kind, clawt_lrg_entry_text(control), FALSE, &error);

        if (value == NULL) {
            g_autofree gchar *text = g_strdup_printf("%s: %s", field->leaf,
                                                     error->message);

            clawt_lrg_toast(editor->app, text);
            json_object_unref(extras);
            return NULL;
        }

        json_object_set_member(extras, field->leaf, value);
    }

    return extras;
}

static gboolean
on_save(ClawtLrgApp *app, LrgDialog *dialog, gint response,
        gpointer user_data)
{
    RoutineEditor *editor = user_data;
    ClawtLrgRoutineForm form;
    g_autofree gchar *instructions = clawt_lrg_editor_text(
        editor->instructions);
    g_autofree gchar *at = editor_at(editor);
    g_autofree gchar *jitter = g_strdup_printf(
        "%.0f", lrg_spin_box_get_value(LRG_SPIN_BOX(editor->jitter)));
    g_autoptr(JsonObject) extras = NULL;
    g_autoptr(GError) error = NULL;
    JsonNode *payload;

    /* the form stays open while the daemon answers, and on a refusal */
    if (editor->busy)
        return FALSE;

    extras = collect_extras(editor);

    if (extras == NULL)
        return FALSE;

    /* Drawn by hand above, so not among the walked extras. */
    json_object_set_int_member(
        extras, "max_start_failures",
        (gint64)MAX(lrg_spin_box_get_value(LRG_SPIN_BOX(editor->max_failures)),
                    0.0));

    form.creating = editor->creating;
    form.id = editor->creating ? clawt_lrg_entry_text(editor->id_entry)
                               : editor->id;
    form.description = clawt_lrg_entry_text(editor->description);
    form.instructions = instructions;
    form.agent = clawt_lrg_combo_id(editor->agent);
    form.directory = clawt_lrg_entry_text(editor->directory);
    form.schedule = clawt_lrg_combo_id(editor->schedule);
    form.at = at;
    form.weekday = clawt_lrg_combo_id(editor->weekday);
    form.cron = clawt_lrg_entry_text(editor->cron);
    form.worktree = clawt_lrg_switch_active(editor->worktree);
    form.enabled = clawt_lrg_switch_active(editor->enabled);
    form.catch_up = clawt_lrg_switch_active(editor->catch_up);
    form.read_only = clawt_lrg_switch_active(editor->read_only);
    form.proactive = clawt_lrg_switch_active(editor->proactive);
    form.jitter = jitter;
    form.extras = extras;

    payload = clawt_lrg_routine_payload(&form, &error);

    if (payload == NULL) {
        clawt_lrg_toast(app, error->message);
        return FALSE;
    }

    editor->busy = TRUE;
    clawt_lrg_request(app, editor->creating ? "routine.add"
                                            : "routine.update",
                      payload, CLAWT_LRG_REQUEST_NONE, on_saved,
                      editor_ref(editor), editor_unref);
    return FALSE;
}

static void
on_ran(ClawtLrgApp *app, JsonNode *reply, const GError *error,
       gpointer user_data)
{
    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Started. It is in Tasks.");
    refresh_list(app, CLAWT_LRG_QUIET);
}

/* Runs a routine now, enabled or not -- how one is tried before trusted. */
static void
run_routine(ClawtLrgApp *app, const gchar *id)
{
    clawt_lrg_request(app, "routine.run",
                      clawt_ipc_payload_new("id", id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_ran, NULL, NULL);
}

static void
on_run_clicked(LrgButton *button, gpointer user_data)
{
    RoutineEditor *editor = user_data;

    run_routine(editor->app, editor->id);
}

static void
on_removed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    RoutineEditor *editor = user_data;

    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Routine removed");
    refresh_list(app, CLAWT_LRG_QUIET);

    if (editor->dialog != NULL && lrg_dialog_get_presented(editor->dialog))
        lrg_dialog_close(editor->dialog);
}

static void
on_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    RoutineEditor *editor = user_data;

    clawt_lrg_request(app, "routine.remove",
                      clawt_ipc_payload_new("id", editor->id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_removed, editor_ref(editor),
                      editor_unref);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    RoutineEditor *editor = user_data;
    g_autofree gchar *body = g_strdup_printf(
        "\"%s\" stops running and leaves clawtilla.yaml. Tasks it already "
        "started are kept.", editor->id);

    clawt_lrg_confirm(editor->app, "Remove this routine?", body, "Remove",
                      TRUE, on_remove_confirmed, editor_ref(editor),
                      editor_unref);
}

static LrgWidget *
switch_field(LrgWidget *form, const gchar *label, gboolean active,
             const gchar *hint)
{
    LrgWidget *toggle = clawt_lrg_switch(label, active);

    clawt_lrg_form_add(form, NULL, toggle, hint);
    return toggle;
}

static void
add_group(LrgWidget *content, const gchar *title, const gchar *lede)
{
    clawt_lrg_take(content, clawt_lrg_section(title));

    if (lede != NULL)
        clawt_lrg_take(content, clawt_lrg_paragraph(lede, "muted"));
}

static void
open_editor(ClawtLrgApp *app, JsonObject *existing)
{
    RoutineEditor *editor = g_rc_box_new0(RoutineEditor);
    LrgScrollBox *scroll = lrg_scroll_box_new();
    LrgWidget *content = clawt_lrg_vbox(12.0f);
    LrgWidget *form;
    LrgDialog *dialog;
    g_auto(GStrv) agent_ids = NULL;
    g_auto(GStrv) agent_labels = NULL;
    g_auto(GStrv) schedule_ids = NULL;
    g_auto(GStrv) schedule_labels = NULL;
    g_auto(GStrv) day_ids = NULL;
    g_auto(GStrv) day_labels = NULL;
    g_autofree gchar *at = NULL;
    g_autofree gchar *jitter = NULL;
    const gchar *agent = NULL;
    const gchar *schedule = "daily";
    const gchar *weekday = "monday";
    gint hour = 9;
    gint minute = 0;
    guint i;

    editor->app = app;
    editor->creating = existing == NULL;
    editor->id = g_strdup((existing != NULL)
                          ? clawt_ipc_payload_string(existing, "id") : "");

    /*
     * A new routine's schedule fields start at the schema's defaults:
     * the daemon fills those in for a key nobody wrote, and the editor
     * showing anything else would describe a routine that is not the
     * one it creates.
     */
    if (existing != NULL) {
        agent = clawt_ipc_payload_string(existing, "agent");

        if (clawt_ipc_payload_string(existing, "schedule") != NULL)
            schedule = clawt_ipc_payload_string(existing, "schedule");

        if (clawt_ipc_payload_string(existing, "weekday") != NULL)
            weekday = clawt_ipc_payload_string(existing, "weekday");
    } else {
        agent = app->agent_id;
    }

    at = clawt_lrg_work_field_text(existing, "at", CLAWT_LRG_FIELD_TEXT);

    if (*at == '\0') {
        g_free(at);
        at = g_strdup("09:00");
    }

    /* ── What it does ── */
    add_group(content, "What it does",
              "Routines only run while the daemon is running. A machine "
              "that was asleep at nine o'clock has missed nine o'clock.");
    form = clawt_lrg_form_new();

    editor->id_entry = clawt_lrg_entry(editor->id, "morning-brief");
    lrg_widget_set_enabled(editor->id_entry, editor->creating);
    clawt_lrg_form_add(form, "Name", editor->id_entry,
                       editor->creating ? NULL
                       : "A routine's name is how it is referred to, so it "
                         "is fixed once made.");

    {
        g_autofree gchar *text = clawt_lrg_work_field_text(
            existing, "description", CLAWT_LRG_FIELD_TEXT);

        editor->description = clawt_lrg_entry(text,
                                              "One line, for the list");
        clawt_lrg_form_add(form, "Description", editor->description, NULL);
    }

    /*
     * A multi-line editor rather than an entry: the instructions are the
     * whole of the prompt, and a single line teaches people to write a
     * reminder where an instruction is needed.
     */
    {
        g_autofree gchar *text = clawt_lrg_work_field_text(
            existing, "instructions", CLAWT_LRG_FIELD_TEXT);

        editor->instructions = clawt_lrg_editor(text, NULL);
        lrg_widget_set_min_size(editor->instructions, 0.0f, 160.0f);
        clawt_lrg_form_add(form, "Instructions", editor->instructions,
                           "Sent as the whole of the prompt: the agent has "
                           "no memory of the last run and nobody is there "
                           "to answer a question about what was meant.");
    }

    clawt_lrg_take(content, form);

    /* ── Who and where ── */
    add_group(content, "Who and where", NULL);
    form = clawt_lrg_form_new();

    clawt_lrg_work_agent_choices(app->agents, &agent_ids, &agent_labels);
    editor->agent = clawt_lrg_combo((const gchar *const *)agent_ids,
                                    (const gchar *const *)agent_labels,
                                    agent);
    clawt_lrg_form_add(form, "Agent", editor->agent, NULL);

    {
        g_autofree gchar *text = clawt_lrg_work_field_text(
            existing, "directory", CLAWT_LRG_FIELD_TEXT);

        editor->directory = clawt_lrg_entry(text, "The agent's workspace");
        clawt_lrg_form_add(form, "Folder", editor->directory,
                           "On the agent's computer. Empty means its "
                           "workspace.");
    }

    editor->worktree = switch_field(
        form, "Use a git worktree",
        clawt_lrg_work_boolean(existing, "routines.", "worktree"),
        "For a routine that changes files: keeps a scheduled run off "
        "whatever you had checked out at the time.");

    clawt_lrg_take(content, form);

    /* ── When ── */
    add_group(content, "When", NULL);
    form = clawt_lrg_form_new();

    clawt_lrg_schedule_choices(&schedule_ids, &schedule_labels);
    editor->schedule = clawt_lrg_combo((const gchar *const *)schedule_ids,
                                       (const gchar *const *)schedule_labels,
                                       schedule);
    clawt_lrg_form_add(form, "Schedule", editor->schedule, NULL);

    /*
     * A picker when the stored time is one it can hold, text when it is
     * not: a picker handed "9.30" would show 00:00 and save it.
     */
    if (clawt_lrg_parse_at(at, &hour, &minute)) {
        LrgTimePicker *picker = lrg_time_picker_new();

        lrg_time_picker_set_use_24h(picker, TRUE);
        lrg_time_picker_set_time(picker, hour, minute, 0);
        editor->at_picker = LRG_WIDGET(picker);
        g_signal_connect(picker, "time-changed",
                         G_CALLBACK(on_schedule_input), editor);
        clawt_lrg_form_add(form, "At", editor->at_picker,
                           "Local time, 24-hour.");
    } else {
        editor->at_entry = clawt_lrg_entry(at, "09:00");
        g_signal_connect(editor->at_entry, "text-changed",
                         G_CALLBACK(on_schedule_input), editor);
        clawt_lrg_form_add(form, "At", editor->at_entry,
                           "Local time, such as 09:00.");
    }

    clawt_lrg_weekday_choices(&day_ids, &day_labels);
    editor->weekday = clawt_lrg_combo((const gchar *const *)day_ids,
                                      (const gchar *const *)day_labels,
                                      weekday);
    clawt_lrg_form_add(form, "Day", editor->weekday, NULL);

    {
        g_autofree gchar *text = clawt_lrg_work_field_text(
            existing, "cron", CLAWT_LRG_FIELD_TEXT);

        editor->cron = clawt_lrg_entry(text, "minute hour day-of-month month "
                                             "day-of-week");
        clawt_lrg_form_add(form, "Cron expression", editor->cron,
                           "Note: with both day fields set, it is "
                           "day-of-month OR day-of-week.");
    }

    editor->preview = clawt_lrg_paragraph("", "muted");
    clawt_lrg_take(form, editor->preview);

    editor->enabled = switch_field(
        form, "Enabled",
        clawt_lrg_work_boolean(existing, "routines.", "enabled"),
        "A disabled routine can still be run by hand, which is how you try "
        "one before trusting it.");
    editor->catch_up = switch_field(
        form, "Run a missed one at startup",
        clawt_lrg_work_boolean(existing, "routines.", "catch_up"),
        "Once, however many were missed.");

    /*
     * Read-only and proactive: the two ways a routine looks without
     * acting.  The hints say what each costs, because "read-only" on a
     * codex agent means "will not run", and the daemon's warning is
     * somewhere else on screen.
     */
    editor->read_only = switch_field(
        form, "Read-only",
        clawt_lrg_work_boolean(existing, "routines.", "read_only"),
        "Looks, changes nothing, reports as decisions. Claude Code agents "
        "only.");
    editor->proactive = switch_field(
        form, "Proactive",
        clawt_lrg_work_boolean(existing, "routines.", "proactive"),
        "Looks for what needs attention, unasked. Implies read-only.");

    jitter = clawt_lrg_work_field_text(existing, "jitter_seconds",
                                       CLAWT_LRG_FIELD_NUMBER);
    editor->jitter = LRG_WIDGET(lrg_spin_box_new_with_range(0.0, 86400.0,
                                                            1.0));
    lrg_spin_box_set_digits(LRG_SPIN_BOX(editor->jitter), 0);
    lrg_spin_box_set_value(LRG_SPIN_BOX(editor->jitter),
                           g_ascii_strtod(*jitter != '\0' ? jitter : "0",
                                          NULL));
    clawt_lrg_form_add(form, "Jitter (seconds)", editor->jitter,
                       "Hold each scheduled run back by up to this long, "
                       "chosen at random. Only worth setting when several "
                       "routines share one rate-limited service; `run now` "
                       "and a catch-up start at once either way.");

    /*
     * How many runs in a row may find the computer down before the
     * routine is turned off, with libclawt's explanation -- the same
     * words the GTK and web editors use.
     */
    {
        g_autofree gchar *limit = clawt_lrg_work_field_text(
            existing, "max_start_failures", CLAWT_LRG_FIELD_NUMBER);

        editor->max_failures = LRG_WIDGET(lrg_spin_box_new_with_range(
            0.0, 1000.0, 1.0));
        lrg_spin_box_set_digits(LRG_SPIN_BOX(editor->max_failures), 0);
        lrg_spin_box_set_value(LRG_SPIN_BOX(editor->max_failures),
                               g_ascii_strtod(*limit != '\0' ? limit : "3",
                                              NULL));
        clawt_lrg_form_add(form, "Turn off after failed starts",
                           editor->max_failures,
                           clawt_routine_start_failures_hint());
    }

    clawt_lrg_take(content, form);

    /*
     * ── Everything else the schema has ──
     *
     * Walked rather than listed: the GTK editor names its keys by hand,
     * so `isolate` -- in the schema since 0.1.0 -- was reachable only
     * from a browser.  An option added to the schema appears here the
     * day it exists.
     */
    editor->extra_fields = clawt_lrg_work_extra_fields("routines.",
                                                       handled_leaves);
    editor->extra_controls = g_ptr_array_new();

    if (editor->extra_fields->len > 0) {
        add_group(content, "More", NULL);
        form = clawt_lrg_form_new();

        for (i = 0; i < editor->extra_fields->len; i++) {
            ClawtLrgSchemaField *field =
                g_ptr_array_index(editor->extra_fields, i);
            g_autofree gchar *label = clawt_lrg_work_field_label(field->leaf);
            LrgWidget *control;

            if (field->kind == CLAWT_LRG_FIELD_SWITCH) {
                control = switch_field(
                    form, label,
                    clawt_lrg_work_boolean(existing, "routines.",
                                           field->leaf),
                    field->hint);
            } else {
                g_autofree gchar *text = clawt_lrg_work_field_text(
                    existing, field->leaf, field->kind);

                control = clawt_lrg_entry(text, NULL);
                clawt_lrg_form_add(form, label, control, field->hint);
            }

            g_ptr_array_add(editor->extra_controls, control);
        }

        clawt_lrg_take(content, form);
    }

    /* ── Actions an existing routine has ── */
    if (!editor->creating) {
        LrgWidget *actions = clawt_lrg_hbox(8.0f);

        clawt_lrg_take(actions, clawt_lrg_button("Run now", NULL,
                                                 G_CALLBACK(on_run_clicked),
                                                 editor));
        clawt_lrg_take(actions, clawt_lrg_button(
                                    "Remove", "danger",
                                    G_CALLBACK(on_remove_clicked), editor));
        clawt_lrg_take(content, actions);
    }

    g_signal_connect(editor->schedule, "changed",
                     G_CALLBACK(on_schedule_input), editor);
    g_signal_connect(editor->weekday, "changed",
                     G_CALLBACK(on_schedule_input), editor);
    g_signal_connect(editor->cron, "text-changed",
                     G_CALLBACK(on_schedule_input), editor);

    lrg_container_set_padding(LRG_CONTAINER(content), 4.0f);
    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, content);
    lrg_widget_set_min_size(LRG_WIDGET(scroll), 600.0f, 560.0f);
    editor->content = LRG_WIDGET(scroll);

    update_schedule(editor);

    dialog = clawt_lrg_dialog(app,
                              editor->creating ? "New routine" : editor->id,
                              NULL, LRG_WIDGET(scroll),
                              editor->creating ? "Create" : "Save", FALSE,
                              on_save, editor, editor_unref);
    editor->dialog = dialog;
    g_object_add_weak_pointer(G_OBJECT(dialog), (gpointer *)&editor->dialog);
    g_object_unref(dialog);

    g_object_unref(content);
}

/* ── The list ─────────────────────────────────────────────────────── */

static void
on_new_clicked(LrgButton *button, gpointer user_data)
{
    open_editor(user_data, NULL);
}

static void
on_edit_clicked(LrgButton *button, gpointer user_data)
{
    JsonObject *routine = g_object_get_data(G_OBJECT(button), "routine");

    if (routine != NULL)
        open_editor(routines.app, routine);
}

static void
on_turned_on(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	if (reply == NULL)
		return;

	clawt_lrg_toast(app, "Turned back on.");
	refresh_list(app, CLAWT_LRG_QUIET);
}

/*
 * One click to turn a routine back on after its computer would not
 * start: `enabled: true` through routine.update, which the daemon takes
 * as the edit it is, clearing the count and the note.
 */
static void
on_routine_turn_on(
	LrgButton *button,
	gpointer   user_data
){
	const gchar *id = g_object_get_data(G_OBJECT(button), "routine-id");
	JsonObject *payload;
	JsonNode *node;

	if (id == NULL)
		return;

	payload = json_object_new();
	json_object_set_string_member(payload, "id", id);
	json_object_set_boolean_member(payload, "enabled", TRUE);
	node = json_node_new(JSON_NODE_OBJECT);
	json_node_take_object(node, payload);

	clawt_lrg_request(routines.app, "routine.update", node,
	                  CLAWT_LRG_REQUEST_NONE, on_turned_on, NULL, NULL);
}

static void
on_row_run_clicked(LrgButton *button, gpointer user_data)
{
    const gchar *id = g_object_get_data(G_OBJECT(button), "routine-id");

    if (id != NULL)
        run_routine(routines.app, id);
}

static LrgWidget *
routine_card(ClawtLrgApp *app, JsonObject *routine, gint64 now_us)
{
    const gchar *agent = clawt_ipc_payload_string(routine, "agent");
    const gchar *detail = clawt_ipc_payload_string(routine, "last_detail");
    g_autofree gchar *when = clawt_lrg_routine_when(routine);
    g_autofree gchar *subtitle = g_strdup_printf(
        "%s \342\200\224 %s", (agent != NULL) ? agent : "?", when);
    const gchar *tone = NULL;
    g_autofree gchar *last = clawt_lrg_routine_last_run(routine, now_us,
                                                        &tone);
    LrgWidget *body = clawt_lrg_vbox(4.0f);
    LrgWidget *row = clawt_lrg_row(clawt_lrg_routine_title(routine),
                                   subtitle);
    LrgWidget *edit;
    LrgWidget *run;

    if (clawt_ipc_payload_string(routine, "problem") != NULL)
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge("broken", "bad"));
    else if (json_object_has_member(routine, "turned_off_after"))
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge("turned off", "bad"));
    else if (!clawt_lrg_work_boolean(routine, "routines.", "enabled"))
        clawt_lrg_row_add_prefix(row, clawt_lrg_badge("off", "neutral"));

    if (clawt_lrg_work_boolean(routine, "routines.", "proactive"))
        clawt_lrg_row_add_suffix(row, clawt_lrg_badge("proactive", "info"));
    else if (clawt_lrg_work_boolean(routine, "routines.", "read_only"))
        clawt_lrg_row_add_suffix(row, clawt_lrg_badge("read-only", "info"));

    if (last != NULL) {
        g_autofree gchar *label = g_strdup_printf("last run %s", last);

        clawt_lrg_row_add_suffix(row, clawt_lrg_badge(label, tone));
    }

    run = clawt_lrg_button("Run now", "flat", G_CALLBACK(on_row_run_clicked),
                           NULL);
    g_object_set_data_full(G_OBJECT(run), "routine-id",
                           g_strdup(clawt_ipc_payload_string(routine, "id")),
                           g_free);
    clawt_lrg_row_add_suffix(row, run);

    edit = clawt_lrg_button("Edit…", NULL, G_CALLBACK(on_edit_clicked), NULL);
    g_object_set_data_full(G_OBJECT(edit), "routine",
                           json_object_ref(routine),
                           (GDestroyNotify)json_object_unref);
    clawt_lrg_row_add_suffix(row, edit);

    clawt_lrg_take(body, row);

    /* what the last run said about itself -- a failure's reason */
    if (detail != NULL && *detail != '\0')
        clawt_lrg_take(body, clawt_lrg_paragraph(detail, "muted"));

    /*
     * Turned off because its computer would not start: why, in
     * libclawt's words, and the click that turns it back on.
     */
    if (json_object_has_member(routine, "turned_off_after")) {
        g_autofree gchar *note = clawt_routine_turned_off_note(
            (guint)clawt_ipc_payload_int(routine, "turned_off_after", 0),
            clawt_ipc_payload_string(routine, "turned_off_error"));
        LrgWidget *line = clawt_lrg_hbox(8.0f);
        LrgWidget *turn_on = clawt_lrg_button(
            "Turn back on", "primary", G_CALLBACK(on_routine_turn_on), NULL);

        g_object_set_data_full(G_OBJECT(turn_on), "routine-id",
                               g_strdup(clawt_ipc_payload_string(routine,
                                                                 "id")),
                               g_free);
        clawt_lrg_take(line, clawt_lrg_paragraph(note, NULL));
        clawt_lrg_take(line, turn_on);
        clawt_lrg_take(body, line);
    }

    return clawt_lrg_card(body);
}

static void
on_routines(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    guint seq = GPOINTER_TO_UINT(user_data);
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *items = NULL;
    gint64 now_us = g_get_real_time();
    guint i;

    /*
     * Only the newest answer draws.  Replies land on later frames and in
     * any order, so an older listing arriving last would put back a
     * routine that was just removed.
     */
    if (seq != routines.seq || routines.list == NULL)
        return;

    clawt_lrg_clear(routines.list);

    if (reply == NULL) {
        clawt_lrg_take(routines.list,
                       clawt_lrg_empty_state(
                           "Routines are unavailable",
                           "Check the daemon connection, then reopen "
                           "Routines to retry."));
        return;
    }

    if (root != NULL && json_object_has_member(root, "routines") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "routines")))
        items = json_object_get_array_member(root, "routines");

    if (items == NULL || json_array_get_length(items) == 0) {
        clawt_lrg_take(routines.list,
                       clawt_lrg_empty_state(
                           "No routines yet",
                           "A routine asks an agent to do something on a "
                           "schedule -- a morning brief, a nightly check. "
                           "New routine starts one."));
        return;
    }

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *routine = json_array_get_object_element(items, i);
        LrgWidget *card;

        if (routine == NULL)
            continue;

        card = routine_card(app, routine, now_us);
        clawt_lrg_appear(app, card, i);
        clawt_lrg_take(routines.list, card);
    }
}

static void
refresh_list(ClawtLrgApp *app, ClawtLrgRequestFlags flags)
{
    if (routines.list == NULL)
        return;

    routines.seq++;
    clawt_lrg_request(app, "routine.list", NULL, flags, on_routines,
                      GUINT_TO_POINTER(routines.seq), NULL);
}

/* ── The page ─────────────────────────────────────────────────────── */

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new(
        "Routines", "Work an agent does on a schedule");

    routines.app = app;
    routines.page = page;
    routines.list = clawt_lrg_vbox(10.0f);

    clawt_lrg_take(clawt_lrg_page_toolbar(page),
                   clawt_lrg_button("New routine", "primary",
                                    G_CALLBACK(on_new_clicked), app));
    clawt_lrg_add(clawt_lrg_page_body(page), routines.list);
    g_object_unref(routines.list);

    return page;
}

static void
refresh(ClawtLrgApp *app)
{
    refresh_list(app, CLAWT_LRG_REQUEST_NONE);
}

static void
event(ClawtLrgApp *app, ClawtEvent *ev)
{
    const gchar *kind = clawt_event_get_kind(ev);

    /*
     * `routine.ran` is published when one starts, so the list shows the
     * new "last run" without anybody reopening the page.  A hidden page
     * is refreshed by being shown, so only the visible one asks.
     */
    if (kind != NULL && g_str_has_prefix(kind, "routine.") &&
        app->page == CLAWT_PAGE_ROUTINES)
        refresh_list(app, CLAWT_LRG_QUIET);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_ROUTINES, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_routines_page(void)
{
    return &page_impl;
}
