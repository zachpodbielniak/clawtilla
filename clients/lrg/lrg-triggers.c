/*
 * lrg-triggers.c - The Triggers page (mirrors clients/gtk/gtk-triggers.c)
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Work an agent does because something happened elsewhere, and the
 * editor that describes one.  It sits beside Routines because a routine
 * is a clock and a trigger is an event, and the two end in the same
 * queued run against the same agent.
 *
 * Under the triggers sit the recent deliveries, because that is how
 * "nothing happened" is answerable at all: the endpoint was wrong, the
 * secret was wrong, the event was filtered out, or the run failed --
 * and without the receipts those four are indistinguishable from each
 * other and from a forge that never called.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-work-model.h"

typedef struct {
    ClawtLrgApp *app;
    LrgWidget   *page;
    LrgWidget   *notice;
    LrgWidget   *list;
    LrgWidget   *deliveries;
    guint        seq;
    guint        delivery_seq;
    JsonArray   *listening;     /* where the receiver is, from the listing */
} Triggers;

static Triggers triggers;

static const gchar *const handled_leaves[] = {
    "description", "instructions", "agent", "agents", "room", "provider",
    "events", "repo", "branch", "header", "directory", "worktree",
    "isolate", "enabled", "mode", "max_pending", "match", NULL
};

static void refresh_all(ClawtLrgApp *app, ClawtLrgRequestFlags flags);

/* ── The secret, once ─────────────────────────────────────────────── */

/*
 * Shows a secret exactly once, and says that it is the only time.
 *
 * A dialog rather than a toast: a toast answers a question somebody is
 * holding right now and then goes away, and this is a value they have
 * to copy into another program's form before it does.  The secret is
 * the dialog's whole text, so its Copy button copies exactly the value
 * the forge wants; the address is in the sentence above it and on the
 * trigger's card, which has its own copy button.
 */
static void
show_secret_once(ClawtLrgApp *app, JsonNode *reply, const gchar *lead)
{
    JsonObject *object = clawt_ipc_reply_object(reply);
    g_autofree gchar *body = NULL;
    const gchar *secret;

    if (object == NULL)
        return;

    body = clawt_lrg_trigger_secret_body(object, lead, triggers.listening);
    secret = clawt_ipc_payload_string(object, "secret");

    if (body == NULL || secret == NULL)
        return;

    clawt_lrg_show_text(app, "Copy this now", body, secret);
}

/* ── The editor ───────────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp *app;
    LrgDialog   *dialog;    /* weak */
    LrgWidget   *content;   /* strong */
    gboolean     creating;
    gboolean     busy;
    gchar       *id;

    LrgWidget   *id_entry;
    LrgWidget   *description;
    LrgWidget   *instructions;
    LrgWidget   *provider;
    LrgWidget   *events;
    LrgWidget   *repo;
    LrgWidget   *branch;
    LrgWidget   *header;
    LrgWidget   *agent;
    LrgWidget   *agents;
    LrgWidget   *room;
    LrgWidget   *directory;
    LrgWidget   *worktree;
    LrgWidget   *isolate;
    LrgWidget   *enabled;
    LrgWidget   *mode;
    LrgWidget   *pending;
    LrgWidget   *match_box;     /* the condition lines, one hbox each */
    LrgWidget   *match_add;
    GPtrArray   *match_rows;    /* MatchLine*, in the order shown */
    gboolean     match_unreadable;

    GPtrArray   *extra_fields;
    GPtrArray   *extra_controls;
} TriggerEditor;

/* One "path contains value" line in the editor. */
typedef struct {
	LrgWidget *line;
	LrgWidget *path;
	LrgWidget *value;
} MatchLine;

static void
trigger_editor_clear(gpointer data)
{
    TriggerEditor *editor = data;

    if (editor->dialog != NULL)
        g_object_remove_weak_pointer(G_OBJECT(editor->dialog),
                                     (gpointer *)&editor->dialog);

    g_clear_object(&editor->content);
    g_clear_pointer(&editor->match_rows, g_ptr_array_unref);
    g_clear_pointer(&editor->extra_fields, g_ptr_array_unref);
    g_clear_pointer(&editor->extra_controls, g_ptr_array_unref);
    g_free(editor->id);
}

static gpointer
editor_ref(TriggerEditor *editor)
{
    return g_rc_box_acquire(editor);
}

static void
editor_unref(gpointer data)
{
    g_rc_box_release_full(data, trigger_editor_clear);
}

static void
close_editor(TriggerEditor *editor)
{
    if (editor->dialog != NULL && lrg_dialog_get_presented(editor->dialog))
        lrg_dialog_close(editor->dialog);
}

static void
on_saved(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    TriggerEditor *editor = user_data;

    editor->busy = FALSE;

    if (reply == NULL)
        return;

    if (editor->creating)
        show_secret_once(app, reply,
                         "Put these into the webhook on the forge. The "
                         "trigger stays switched off until you have seen "
                         "what it sends.");
    else
        clawt_lrg_toast(app, "Trigger saved");

    refresh_all(app, CLAWT_LRG_QUIET);
    close_editor(editor);
}

/* ── The payload filter ───────────────────────────────────────────── */

static void
on_match_line_removed(LrgButton *button, gpointer user_data)
{
	TriggerEditor *editor = user_data;
	MatchLine *match = g_object_get_data(G_OBJECT(button), "match-line");

	if (match == NULL)
		return;

	lrg_container_remove_child(LRG_CONTAINER(editor->match_box), match->line);
	g_ptr_array_remove(editor->match_rows, match);
	lrg_widget_set_enabled(editor->match_add,
	                       editor->match_rows->len < CLAWT_TRIGGER_MATCH_MAX);
}

/*
 * A condition line, filled in when it came from the saved filter or from
 * a field picked out of the last delivery.  The add button goes disabled
 * at ten, so the editor cannot build what the daemon would refuse.
 */
static void
match_line_add(TriggerEditor *editor, const gchar *path, const gchar *value)
{
	MatchLine *match;
	LrgWidget *remove;

	if (editor->match_rows->len >= CLAWT_TRIGGER_MATCH_MAX) {
		clawt_lrg_toast(editor->app,
		                "Ten conditions is the most a trigger can hold.");
		return;
	}

	match = g_new0(MatchLine, 1);
	match->line = clawt_lrg_hbox(8.0f);
	match->path = clawt_lrg_entry(path, "pull_request.title");
	match->value = clawt_lrg_entry(value, "release");
	remove = clawt_lrg_button("Remove", "flat",
	                          G_CALLBACK(on_match_line_removed), editor);
	g_object_set_data(G_OBJECT(remove), "match-line", match);

	lrg_widget_set_min_size(match->path, 220.0f, 0.0f);
	lrg_widget_set_min_size(match->value, 160.0f, 0.0f);
	clawt_lrg_take(match->line, match->path);
	clawt_lrg_take(match->line, clawt_lrg_label("contains", "muted"));
	clawt_lrg_take(match->line, match->value);
	clawt_lrg_take(match->line, remove);
	clawt_lrg_take(editor->match_box, match->line);

	g_ptr_array_add(editor->match_rows, match);
	lrg_widget_set_enabled(editor->match_add,
	                       editor->match_rows->len < CLAWT_TRIGGER_MATCH_MAX);
}

static void
on_match_add_clicked(LrgButton *button, gpointer user_data)
{
	match_line_add(user_data, NULL, NULL);
}

/* The lines as they stand, through the tested model. */
static JsonNode *
match_lines_node(TriggerEditor *editor)
{
	g_autofree const gchar **paths = NULL;
	g_autofree const gchar **values = NULL;
	guint i;

	paths = g_new0(const gchar *, editor->match_rows->len + 1);
	values = g_new0(const gchar *, editor->match_rows->len + 1);

	for (i = 0; i < editor->match_rows->len; i++) {
		MatchLine *match = g_ptr_array_index(editor->match_rows, i);

		paths[i] = clawt_lrg_entry_text(match->path);
		values[i] = clawt_lrg_entry_text(match->value);
	}

	return clawt_lrg_match_node(paths, values);
}

static void
on_field_picked(LrgButton *button, gpointer user_data)
{
	TriggerEditor *editor = user_data;

	match_line_add(editor, g_object_get_data(G_OBJECT(button), "path"),
	               g_object_get_data(G_OBJECT(button), "value"));
	clawt_lrg_toast(editor->app, "Added as a condition");
}

static gboolean
on_check_closed(ClawtLrgApp *app, LrgDialog *dialog, gint response,
                gpointer user_data)
{
	return TRUE;
}

/*
 * The verdict for the lines being edited against the newest body, and
 * every field in that body with a button that makes it a condition --
 * so a path is picked from what the forge actually sends.
 */
static void
on_match_checked(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                 gpointer user_data)
{
	TriggerEditor *editor = user_data;
	JsonObject *object = clawt_ipc_reply_object(reply);
	g_autofree gchar *report = NULL;
	LrgScrollBox *scroll;
	LrgWidget *content;
	JsonArray *paths = NULL;
	guint i;

	if (object == NULL)
		return;

	report = clawt_lrg_match_report(object);

	if (clawt_ipc_payload_string(object, "payload") == NULL) {
		clawt_lrg_toast(app, report);
		return;
	}

	scroll = lrg_scroll_box_new();
	content = clawt_lrg_vbox(6.0f);
	clawt_lrg_take(content, clawt_lrg_paragraph(report, NULL));
	clawt_lrg_take(content, clawt_lrg_section("Fields in it"));
	clawt_lrg_take(content, clawt_lrg_paragraph(
		"Use makes one a condition. A name on a list, such as labels.name, "
		"looks in every item of it.", "muted"));

	if (json_object_has_member(object, "paths") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "paths")))
		paths = json_object_get_array_member(object, "paths");

	for (i = 0; paths != NULL && i < json_array_get_length(paths); i++) {
		JsonObject *field = json_array_get_object_element(paths, i);
		const gchar *path = clawt_ipc_payload_string(field, "path");
		const gchar *value = clawt_ipc_payload_string(field, "value");
		LrgWidget *row = clawt_lrg_row(path, value);
		LrgWidget *use = clawt_lrg_button("Use", "flat",
		                                  G_CALLBACK(on_field_picked), editor);

		g_object_set_data_full(G_OBJECT(use), "path", g_strdup(path), g_free);
		g_object_set_data_full(G_OBJECT(use), "value", g_strdup(value),
		                       g_free);
		clawt_lrg_row_add_suffix(row, use);
		clawt_lrg_take(content, row);
	}

	lrg_scroll_box_set_hscroll(scroll, FALSE);
	lrg_scroll_box_set_vscroll(scroll, TRUE);
	lrg_scroll_box_set_child(scroll, content);
	lrg_widget_set_min_size(LRG_WIDGET(scroll), 560.0f, 480.0f);
	g_object_unref(content);

	/* The dialog holds the editor its Use buttons add to. */
	g_object_unref(clawt_lrg_dialog(app, "Against the last delivery", NULL,
	                                LRG_WIDGET(scroll), "Done", FALSE,
	                                on_check_closed, editor_ref(editor),
	                                editor_unref));
	g_object_unref(scroll);
}

static void
on_match_check_clicked(LrgButton *button, gpointer user_data)
{
	TriggerEditor *editor = user_data;
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, editor->id);
	json_builder_set_member_name(builder, "match");
	json_builder_add_value(builder, match_lines_node(editor));
	json_builder_end_object(builder);

	clawt_lrg_request(editor->app, "trigger.match_test",
	                  json_builder_get_root(builder), CLAWT_LRG_REQUEST_NONE,
	                  on_match_checked, editor_ref(editor), editor_unref);
}

static JsonObject *
collect_extras(TriggerEditor *editor)
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
    TriggerEditor *editor = user_data;
    ClawtLrgTriggerForm form;
    g_autofree gchar *instructions = clawt_lrg_editor_text(
        editor->instructions);
    g_autoptr(JsonObject) extras = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) match = NULL;
    JsonNode *payload;

    if (editor->busy)
        return FALSE;

    extras = collect_extras(editor);

    if (extras == NULL)
        return FALSE;

    form.creating = editor->creating;
    form.id = editor->creating ? clawt_lrg_entry_text(editor->id_entry)
                               : editor->id;
    form.description = clawt_lrg_entry_text(editor->description);
    form.instructions = instructions;
    form.agent = clawt_lrg_combo_id(editor->agent);
    form.agents = clawt_lrg_entry_text(editor->agents);
    form.room = clawt_lrg_entry_text(editor->room);
    form.provider = clawt_lrg_combo_id(editor->provider);
    form.events = clawt_lrg_entry_text(editor->events);
    form.repo = clawt_lrg_entry_text(editor->repo);
    form.branch = clawt_lrg_entry_text(editor->branch);
    form.header = clawt_lrg_entry_text(editor->header);
    form.directory = clawt_lrg_entry_text(editor->directory);
    form.worktree = clawt_lrg_switch_active(editor->worktree);
    form.isolate = clawt_lrg_switch_active(editor->isolate);
    form.enabled = clawt_lrg_switch_active(editor->enabled);
    form.mode = clawt_lrg_combo_id(editor->mode);
    form.max_pending = (gint64)lrg_spin_box_get_value(
        LRG_SPIN_BOX(editor->pending));
    match = match_lines_node(editor);

    /*
     * No lines over a filter the daemon could not read is this editor
     * drawing nothing, not somebody clearing it: leave it alone.
     */
    form.match = (editor->match_unreadable &&
                  json_array_get_length(json_node_get_array(match)) == 0)
        ? NULL : match;
    form.extras = extras;

    payload = clawt_lrg_trigger_payload(&form, &error);

    if (payload == NULL) {
        clawt_lrg_toast(app, error->message);
        return FALSE;
    }

    editor->busy = TRUE;
    clawt_lrg_request(app, editor->creating ? "trigger.add"
                                            : "trigger.update",
                      payload, CLAWT_LRG_REQUEST_NONE, on_saved,
                      editor_ref(editor), editor_unref);
    return FALSE;
}

static void
on_removed(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    if (reply == NULL)
        return;

    clawt_lrg_toast(app, "Trigger removed");
    refresh_all(app, CLAWT_LRG_QUIET);
    close_editor(user_data);
}

static void
on_remove_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    TriggerEditor *editor = user_data;

    clawt_lrg_request(app, "trigger.remove",
                      clawt_ipc_payload_new("id", editor->id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_removed, editor_ref(editor),
                      editor_unref);
}

static void
on_remove_clicked(LrgButton *button, gpointer user_data)
{
    TriggerEditor *editor = user_data;
    g_autofree gchar *body = g_strdup_printf(
        "\"%s\" stops answering at once: its address will refuse every "
        "delivery from now on.", editor->id);

    clawt_lrg_confirm(editor->app, "Remove this trigger?", body, "Remove",
                      TRUE, on_remove_confirmed, editor_ref(editor),
                      editor_unref);
}

static void
on_rotated(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    if (reply == NULL)
        return;

    show_secret_once(app, reply,
                     "The old secret and the old address stopped working "
                     "just now. The webhook needs both of these before it "
                     "will be accepted again.");
    refresh_all(app, CLAWT_LRG_QUIET);
    close_editor(user_data);
}

static void
on_rotate_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    TriggerEditor *editor = user_data;

    clawt_lrg_request(app, "trigger.rotate",
                      clawt_ipc_payload_new("id", editor->id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_rotated, editor_ref(editor),
                      editor_unref);
}

/*
 * Rotating is asked about first: the old secret and address stop
 * working the moment it happens, so a forge still configured with them
 * starts failing before anybody has pasted the new ones in.
 */
static void
on_rotate_clicked(LrgButton *button, gpointer user_data)
{
    TriggerEditor *editor = user_data;

    clawt_lrg_confirm(editor->app, "Rotate the secret?",
                      "A new secret and a new address are made, and the old "
                      "ones stop working at once.", "Rotate", TRUE,
                      on_rotate_confirmed, editor_ref(editor), editor_unref);
}

/*
 * Shows the delivery that was held back, so somebody can read it before
 * an agent does.  This is the whole of the verification handshake from
 * a person's side: the first authenticated delivery is captured rather
 * than run, and this is where they find out whether the thing calling
 * the endpoint is the thing they registered.
 */
static void
on_trigger_capture(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                   gpointer user_data)
{
    JsonObject *object = clawt_ipc_reply_object(reply);
    const gchar *payload;

    if (object == NULL)
        return;

    payload = clawt_ipc_payload_string(object, "payload");

    if (payload == NULL) {
        clawt_lrg_toast(app, "Nothing has arrived yet. Send it a test "
                             "delivery from the forge.");
        return;
    }

    clawt_lrg_show_text(
        app, "The first delivery",
        clawt_ipc_payload_boolean(object, "pending_verification", FALSE)
            ? "Held back, not run. If this is what you expected, switch the "
              "trigger on."
            : "What the first delivery carried.",
        payload);
}

static void
on_capture_clicked(LrgButton *button, gpointer user_data)
{
    TriggerEditor *editor = user_data;

    clawt_lrg_request(editor->app, "trigger.capture",
                      clawt_ipc_payload_new("id", editor->id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_trigger_capture, NULL, NULL);
}

/*
 * What the agent would be asked, without asking it: the template
 * expanded against a made-up delivery (trigger.test), or against the
 * latest real one with the reason it would or would not run
 * (trigger.replay) -- so somebody sees the placeholders filled in and
 * the untrusted-payload fence before anything real is acted on.
 */
static void
on_report(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    JsonObject *object = clawt_ipc_reply_object(reply);

    if (object == NULL)
        return;

    clawt_lrg_show_text(app, "What the agent would be asked",
                        GPOINTER_TO_INT(user_data)
                            ? "The latest delivery, against the trigger as "
                              "it is now. Nothing was run."
                            : "A made-up delivery, expanded. Nothing was "
                              "run.",
                        clawt_lrg_trigger_report(object));
}

static void
on_preview_clicked(LrgButton *button, gpointer user_data)
{
    TriggerEditor *editor = user_data;

    clawt_lrg_request(editor->app, "trigger.test",
                      clawt_ipc_payload_new("id", editor->id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_report,
                      GINT_TO_POINTER(FALSE), NULL);
}

static void
on_replay_clicked(LrgButton *button, gpointer user_data)
{
    TriggerEditor *editor = user_data;

    clawt_lrg_request(editor->app, "trigger.replay",
                      clawt_ipc_payload_new("id", editor->id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_report,
                      GINT_TO_POINTER(TRUE), NULL);
}

static LrgWidget *
switch_field(LrgWidget *form, const gchar *label, gboolean active,
             const gchar *hint)
{
    LrgWidget *toggle = clawt_lrg_switch(label, active);

    clawt_lrg_form_add(form, NULL, toggle, hint);
    return toggle;
}

static LrgWidget *
text_field(LrgWidget *form, JsonObject *existing, const gchar *leaf,
           const gchar *label, const gchar *placeholder, const gchar *hint)
{
    g_autofree gchar *text = clawt_lrg_work_field_text(existing, leaf,
                                                       CLAWT_LRG_FIELD_TEXT);
    LrgWidget *entry = clawt_lrg_entry(text, placeholder);

    clawt_lrg_form_add(form, label, entry, hint);
    return entry;
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
    TriggerEditor *editor = g_rc_box_new0(TriggerEditor);
    LrgScrollBox *scroll = lrg_scroll_box_new();
    LrgWidget *content = clawt_lrg_vbox(12.0f);
    LrgWidget *form;
    LrgDialog *dialog;
    g_auto(GStrv) agent_ids = NULL;
    g_auto(GStrv) agent_labels = NULL;
    g_auto(GStrv) provider_ids = NULL;
    g_auto(GStrv) provider_labels = NULL;
    const gchar *provider = "generic";
    const gchar *agent;
    guint i;

    editor->app = app;
    editor->creating = existing == NULL;
    editor->id = g_strdup((existing != NULL)
                          ? clawt_ipc_payload_string(existing, "id") : "");
    agent = (existing != NULL) ? clawt_ipc_payload_string(existing, "agent")
                               : app->agent_id;

    if (existing != NULL &&
        clawt_ipc_payload_string(existing, "provider") != NULL)
        provider = clawt_ipc_payload_string(existing, "provider");

    /* ── What it does ── */
    add_group(content, "What it does",
              "Placeholders in the instructions -- {{event}}, {{repo}}, "
              "{{ref}}, {{actor}}, {{title}}, {{url}}, {{number}} -- are "
              "filled in from the delivery. The body itself is added below "
              "them, marked as somebody else's text.");
    form = clawt_lrg_form_new();

    editor->id_entry = clawt_lrg_entry(editor->id, "push-review");
    lrg_widget_set_enabled(editor->id_entry, editor->creating);
    clawt_lrg_form_add(form, "Name", editor->id_entry,
                       editor->creating
                       ? "Not the address it is called on: that is a "
                         "separate, unguessable endpoint."
                       : NULL);
    editor->description = text_field(form, existing, "description",
                                     "Description", "One line, for the list",
                                     NULL);

    {
        g_autofree gchar *text = clawt_lrg_work_field_text(
            existing, "instructions", CLAWT_LRG_FIELD_TEXT);

        editor->instructions = clawt_lrg_editor(text, NULL);
        lrg_widget_set_min_size(editor->instructions, 0.0f, 160.0f);
        clawt_lrg_form_add(form, "Instructions", editor->instructions, NULL);
    }

    clawt_lrg_take(content, form);

    /* ── What wakes it ── */
    add_group(content, "What wakes it", NULL);
    form = clawt_lrg_form_new();

    /*
     * Walked from the library's enumeration rather than written out: a
     * provider added to clawt-enums.c reaches this editor the moment it
     * exists, and none the daemon does not understand can be offered.
     */
    clawt_lrg_provider_choices(&provider_ids, &provider_labels);
    editor->provider = clawt_lrg_combo((const gchar *const *)provider_ids,
                                       (const gchar *const *)provider_labels,
                                       provider);
    clawt_lrg_form_add(form, "Sent by", editor->provider,
                       "Each forge authenticates differently, so this is not "
                       "cosmetic. Naming it also stops a Forgejo delivery "
                       "being read as the GitHub it also pretends to be.");

    {
        g_autofree gchar *events = clawt_lrg_work_joined(existing, "events");

        editor->events = clawt_lrg_entry(events, "push, pull_request");
        clawt_lrg_form_add(form, "Events", editor->events,
                           "Comma separated. Empty means every event it is "
                           "sent.");
    }

    editor->repo = text_field(form, existing, "repo", "Repository",
                              "zach/clawtilla", "Empty means any.");
    editor->branch = text_field(form, existing, "branch", "Branch", "master",
                                "Empty means any.");
    editor->header = text_field(form, existing, "header", "Event header",
                                NULL,
                                "Only for a generic sender: which header "
                                "carries the event name.");
    clawt_lrg_take(content, form);

    /* ── Only when the body contains ── */
    add_group(content, "Only when the body contains",
              "Each condition names a field by its dotted path and text it "
              "must contain, ignoring case; all of them must hold. A "
              "delivery that misses is recorded as ignored, naming the "
              "condition, and starts nothing. None means every delivery.");

    if (existing != NULL &&
        clawt_ipc_payload_string(existing, "match_error") != NULL) {
        /*
         * Said rather than drawn as no lines, and flagged so on_save()
         * sends no `match` unless lines were added: an empty editor saved
         * over it would otherwise delete what was written.
         */
        LrgWidget *row = clawt_lrg_row(
            "The filter as written cannot be read, so the trigger refuses "
            "every delivery; add lines to replace it",
            clawt_ipc_payload_string(existing, "match_error"));

        editor->match_unreadable = TRUE;

        clawt_lrg_row_add_suffix(row, clawt_lrg_badge("refuses all", "bad"));
        clawt_lrg_take(content, clawt_lrg_card(row));
    }

    editor->match_rows = g_ptr_array_new_with_free_func(g_free);
    editor->match_box = clawt_lrg_vbox(6.0f);
    clawt_lrg_add(content, editor->match_box);
    g_object_unref(editor->match_box);

    {
        LrgWidget *actions = clawt_lrg_hbox(8.0f);

        editor->match_add = clawt_lrg_button(
            "Add a condition", NULL, G_CALLBACK(on_match_add_clicked), editor);
        clawt_lrg_add(actions, editor->match_add);
        g_object_unref(editor->match_add);

        if (!editor->creating)
            clawt_lrg_take(actions, clawt_lrg_button(
                                        "Check against the last delivery",
                                        NULL,
                                        G_CALLBACK(on_match_check_clicked),
                                        editor));

        clawt_lrg_take(content, actions);
    }

    if (existing != NULL && json_object_has_member(existing, "match") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(existing, "match"))) {
        JsonArray *pairs = json_object_get_array_member(existing, "match");
        guint m;

        for (m = 0; m < json_array_get_length(pairs); m++) {
            JsonObject *pair = json_array_get_object_element(pairs, m);

            match_line_add(editor, clawt_ipc_payload_string(pair, "path"),
                           clawt_ipc_payload_string(pair, "value"));
        }
    }

    /* ── Who runs it ── */
    add_group(content, "Who runs it", NULL);
    form = clawt_lrg_form_new();

    clawt_lrg_work_agent_choices(app->agents, &agent_ids, &agent_labels);
    editor->agent = clawt_lrg_combo((const gchar *const *)agent_ids,
                                    (const gchar *const *)agent_labels,
                                    agent);
    clawt_lrg_form_add(form, "Agent", editor->agent, NULL);

    {
        g_autofree gchar *others = clawt_lrg_work_joined(existing, "agents");

        editor->agents = clawt_lrg_entry(others, "oryx, ibex");
        clawt_lrg_form_add(form, "Additional agents", editor->agents,
                           "Comma separated agent IDs. The primary runs "
                           "first; duplicates are removed. Each takes a "
                           "slot of Runs at once.");
    }

    editor->room = text_field(form, existing, "room", "Room", NULL,
                              "Where the run reports, or where a post "
                              "lands -- a post needs one. Empty means the "
                              "agent's own conversation.");

    /* What a delivery turns into, walked from the library. */
    {
        g_auto(GStrv) mode_ids = NULL;
        g_auto(GStrv) mode_labels = NULL;
        g_autofree gchar *hints = clawt_lrg_trigger_mode_hints();
        const gchar *mode = (existing != NULL &&
                             clawt_ipc_payload_string(existing, "mode") != NULL)
            ? clawt_ipc_payload_string(existing, "mode") : "run";

        clawt_lrg_trigger_mode_choices(&mode_ids, &mode_labels);
        editor->mode = clawt_lrg_combo((const gchar *const *)mode_ids,
                                       (const gchar *const *)mode_labels,
                                       mode);
        clawt_lrg_form_add(form, "When it fires", editor->mode, hints);
    }

    {
        const ClawtSchemaEntry *entry =
            clawt_config_schema_lookup("triggers.max_pending");
        gint64 fallback = (entry != NULL && entry->default_value != NULL)
            ? g_ascii_strtoll(entry->default_value, NULL, 10)
            : CLAWT_TRIGGER_MAX_PENDING_MIN;

        editor->pending = LRG_WIDGET(lrg_spin_box_new_with_range(
            CLAWT_TRIGGER_MAX_PENDING_MIN, CLAWT_TRIGGER_MAX_PENDING_MAX,
            1.0));
        lrg_spin_box_set_digits(LRG_SPIN_BOX(editor->pending), 0);
        lrg_spin_box_set_value(
            LRG_SPIN_BOX(editor->pending),
            (gdouble)(existing != NULL
                          ? clawt_ipc_payload_int(existing, "max_pending",
                                                  fallback)
                          : fallback));
        clawt_lrg_form_add(form, "Runs at once", editor->pending,
                           "How many runs may be unfinished together. A "
                           "delivery past this is answered \"try later\" "
                           "and recorded as dropped. A post holds no slot.");
    }
    editor->directory = text_field(form, existing, "directory", "Folder",
                                   NULL,
                                   "On the agent's computer. Empty means its "
                                   "workspace.");
    editor->worktree = switch_field(
        form, "Use a git worktree",
        clawt_lrg_work_boolean(existing, "triggers.", "worktree"),
        "Worth turning on when somebody else's push is what starts this: it "
        "keeps the run off whatever you had checked out.");
    editor->isolate = switch_field(
        form, "Give it a conversation of its own",
        clawt_lrg_work_boolean(existing, "triggers.", "isolate"),
        "Keeps the runs out of the chat you type in.");

    /*
     * A new trigger always starts off, whatever the switch says -- the
     * first delivery is held for somebody to read -- so the switch is
     * shown off and disabled rather than offered and then contradicted.
     */
    editor->enabled = switch_field(
        form, "Enabled",
        existing != NULL &&
            clawt_lrg_work_boolean(existing, "triggers.", "enabled"),
        editor->creating
            ? "A new trigger always starts off: the first delivery is held "
              "for you to read before an agent acts on it."
            : "While it is off, its address answers as though it does not "
              "exist.");
    lrg_widget_set_enabled(editor->enabled, !editor->creating);
    clawt_lrg_take(content, form);

    /* ── Anything else the schema has ── */
    editor->extra_fields = clawt_lrg_work_extra_fields("triggers.",
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
                    clawt_lrg_work_boolean(existing, "triggers.",
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

    /*
     * ── Inspecting and maintaining an existing one ──
     *
     * The inspection actions get a row of their own, apart from Rotate
     * and Remove, so three labels cannot push the dialog wider than a
     * small window and a destructive button never sits beside a
     * harmless one.
     */
    if (!editor->creating) {
        LrgWidget *inspect = clawt_lrg_hbox(8.0f);
        LrgWidget *maintain = clawt_lrg_hbox(8.0f);

        add_group(content, "Check it", NULL);
        clawt_lrg_take(inspect, clawt_lrg_button(
                                    "Preview prompt", NULL,
                                    G_CALLBACK(on_preview_clicked), editor));
        clawt_lrg_take(inspect, clawt_lrg_button(
                                    "Explain latest delivery", NULL,
                                    G_CALLBACK(on_replay_clicked), editor));
        clawt_lrg_take(inspect, clawt_lrg_button(
                                    "First delivery", NULL,
                                    G_CALLBACK(on_capture_clicked), editor));
        clawt_lrg_take(content, inspect);

        clawt_lrg_take(maintain, clawt_lrg_button(
                                     "Rotate secret", NULL,
                                     G_CALLBACK(on_rotate_clicked), editor));
        clawt_lrg_take(maintain, clawt_lrg_button(
                                     "Remove", "danger",
                                     G_CALLBACK(on_remove_clicked), editor));
        clawt_lrg_take(content, maintain);
    }

    lrg_container_set_padding(LRG_CONTAINER(content), 4.0f);
    lrg_scroll_box_set_hscroll(scroll, FALSE);
    lrg_scroll_box_set_vscroll(scroll, TRUE);
    lrg_scroll_box_set_child(scroll, content);
    lrg_widget_set_min_size(LRG_WIDGET(scroll), 600.0f, 560.0f);
    editor->content = LRG_WIDGET(scroll);

    dialog = clawt_lrg_dialog(app,
                              editor->creating ? "New trigger" : editor->id,
                              NULL, LRG_WIDGET(scroll),
                              editor->creating ? "Create" : "Save", FALSE,
                              on_save, editor, editor_unref);
    editor->dialog = dialog;
    g_object_add_weak_pointer(G_OBJECT(dialog), (gpointer *)&editor->dialog);
    g_object_unref(dialog);

    g_object_unref(content);
}

/* ── The page ─────────────────────────────────────────────────────── */

static void
on_new_clicked(LrgButton *button, gpointer user_data)
{
    open_editor(user_data, NULL);
}

static void
on_edit_clicked(LrgButton *button, gpointer user_data)
{
    JsonObject *trigger = g_object_get_data(G_OBJECT(button), "trigger");

    if (trigger != NULL)
        open_editor(triggers.app, trigger);
}

static void
on_copy_address(LrgButton *button, gpointer user_data)
{
    const gchar *address = g_object_get_data(G_OBJECT(button), "address");

    clawt_lrg_copy(triggers.app, address);
    clawt_lrg_toast(triggers.app, "Address copied");
}

static LrgWidget *
trigger_card(JsonObject *trigger)
{
    const gchar *tone = NULL;
    const gchar *state = clawt_lrg_trigger_state(trigger, &tone);
    const gchar *description = clawt_ipc_payload_string(trigger,
                                                        "description");
    const gchar *id = clawt_ipc_payload_string(trigger, "id");
    g_autofree gchar *subtitle = clawt_lrg_trigger_subtitle(trigger);
    g_autofree gchar *address = NULL;
    LrgWidget *row;
    LrgWidget *edit;

    row = clawt_lrg_row((description != NULL && *description != '\0')
                            ? description
                            : (id != NULL) ? id : "?",
                        subtitle);
    clawt_lrg_row_add_suffix(row, clawt_lrg_badge(state, tone));

    address = clawt_lrg_trigger_address(
        clawt_ipc_payload_string(trigger, "endpoint"),
        (json_object_has_member(trigger, "listening") &&
         JSON_NODE_HOLDS_ARRAY(json_object_get_member(trigger, "listening")))
            ? json_object_get_array_member(trigger, "listening") : NULL);

    if (address != NULL) {
        LrgWidget *copy = clawt_lrg_button("Copy address", "flat",
                                           G_CALLBACK(on_copy_address), NULL);

        g_object_set_data_full(G_OBJECT(copy), "address",
                               g_steal_pointer(&address), g_free);
        clawt_lrg_row_add_suffix(row, copy);
    }

    edit = clawt_lrg_button("Edit…", NULL, G_CALLBACK(on_edit_clicked), NULL);
    g_object_set_data_full(G_OBJECT(edit), "trigger",
                           json_object_ref(trigger),
                           (GDestroyNotify)json_object_unref);
    clawt_lrg_row_add_suffix(row, edit);

    return clawt_lrg_card(row);
}

static void
on_triggers(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *items = NULL;
    gboolean receiving;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != triggers.seq || triggers.list == NULL)
        return;

    clawt_lrg_clear(triggers.list);

    if (root == NULL) {
        clawt_lrg_take(triggers.list,
                       clawt_lrg_empty_state(
                           "Triggers are unavailable",
                           "Check the daemon connection, then reopen "
                           "Triggers to retry."));
        return;
    }

    /*
     * Whether anything can arrive at all, said above the list rather
     * than left to be inferred from an empty delivery history.
     */
    receiving = clawt_ipc_payload_boolean(root, "receiving", FALSE);
    lrg_widget_set_visible(triggers.notice, !receiving);

    if (json_object_has_member(root, "triggers") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "triggers")))
        items = json_object_get_array_member(root, "triggers");

    g_clear_pointer(&triggers.listening, json_array_unref);

    if (items == NULL || json_array_get_length(items) == 0) {
        clawt_lrg_take(triggers.list,
                       clawt_lrg_empty_state(
                           "No triggers yet",
                           "A trigger wakes an agent when a forge calls -- "
                           "a push, a pull request. New trigger makes one "
                           "and its secret address."));
        return;
    }

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *trigger = json_array_get_object_element(items, i);
        LrgWidget *card;

        if (trigger == NULL)
            continue;

        /* every trigger carries the same receiver addresses; keep one */
        if (triggers.listening == NULL &&
            json_object_has_member(trigger, "listening") &&
            JSON_NODE_HOLDS_ARRAY(json_object_get_member(trigger,
                                                         "listening")))
            triggers.listening = json_array_ref(
                json_object_get_array_member(trigger, "listening"));

        card = trigger_card(trigger);
        clawt_lrg_appear(app, card, i);
        clawt_lrg_take(triggers.list, card);
    }
}

static void
on_deliveries(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *items = NULL;
    guint i;

    if (GPOINTER_TO_UINT(user_data) != triggers.delivery_seq ||
        triggers.deliveries == NULL)
        return;

    clawt_lrg_clear(triggers.deliveries);

    if (root == NULL)
        return;

    if (json_object_has_member(root, "deliveries") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(root, "deliveries")))
        items = json_object_get_array_member(root, "deliveries");

    /*
     * An empty list has four causes and the daemon's `note` says which
     * one this is, rather than leaving a reader to guess between "the
     * forge never called" and "the receiver is not running".
     */
    if (items == NULL || json_array_get_length(items) == 0) {
        clawt_lrg_take(triggers.deliveries,
                       clawt_lrg_card(clawt_lrg_row(
                           "Nothing yet",
                           clawt_ipc_payload_string(root, "note"))));
        return;
    }

    for (i = 0; i < json_array_get_length(items); i++) {
        JsonObject *delivery = json_array_get_object_element(items, i);
        g_autofree gchar *title = NULL;
        g_autofree gchar *subtitle = NULL;
        LrgWidget *row;

        if (delivery == NULL)
            continue;

        title = clawt_lrg_delivery_title(delivery);
        subtitle = clawt_lrg_delivery_subtitle(delivery);
        row = clawt_lrg_row(title, subtitle);
        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(triggers.deliveries, row);
    }
}

static void
refresh_all(ClawtLrgApp *app, ClawtLrgRequestFlags flags)
{
    if (triggers.list == NULL)
        return;

    triggers.seq++;
    triggers.delivery_seq++;
    clawt_lrg_request(app, "trigger.list", NULL, flags, on_triggers,
                      GUINT_TO_POINTER(triggers.seq), NULL);

    /*
     * Quiet always: a daemon with no trigger store answers NOT_SUPPORTED,
     * which is a condition the list above already explains, not news.
     */
    clawt_lrg_request(app, "trigger.deliveries", NULL, CLAWT_LRG_QUIET,
                      on_deliveries, GUINT_TO_POINTER(triggers.delivery_seq),
                      NULL);
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new(
        "Triggers", "Work an agent does because something happened elsewhere");
    LrgWidget *body = clawt_lrg_page_body(page);

    triggers.app = app;
    triggers.page = page;

    clawt_lrg_take(clawt_lrg_page_toolbar(page),
                   clawt_lrg_button("New trigger", "primary",
                                    G_CALLBACK(on_new_clicked), app));

    clawt_lrg_take(body, clawt_lrg_paragraph(
                             "A routine is a clock; a trigger is an event. "
                             "The receiver is off until "
                             "daemon.webhook_enabled is set, and it only "
                             "ever serves /health and the secret /hooks "
                             "paths.", "muted"));

    triggers.notice = clawt_lrg_card(clawt_lrg_paragraph(
        "The receiver is not running, so no trigger can be called. Set "
        "daemon.webhook_enabled and restart the daemon.", NULL));
    lrg_widget_add_class(triggers.notice, "clawt-banner");
    lrg_widget_set_visible(triggers.notice, FALSE);
    clawt_lrg_add(body, triggers.notice);
    g_object_unref(triggers.notice);

    triggers.list = clawt_lrg_vbox(10.0f);
    clawt_lrg_add(body, triggers.list);
    g_object_unref(triggers.list);

    clawt_lrg_take(body, clawt_lrg_section("Recent deliveries"));
    triggers.deliveries = clawt_lrg_vbox(4.0f);
    clawt_lrg_add(body, triggers.deliveries);
    g_object_unref(triggers.deliveries);

    return page;
}

static void
refresh(ClawtLrgApp *app)
{
    refresh_all(app, CLAWT_LRG_REQUEST_NONE);
}

static void
event(ClawtLrgApp *app, ClawtEvent *ev)
{
    const gchar *kind = clawt_event_get_kind(ev);

    /*
     * One prefix covers trigger.fired, .changed, .refused and .verified
     * -- and the last two are the ones somebody is waiting on while they
     * point a forge at a new endpoint.
     */
    if (kind != NULL && g_str_has_prefix(kind, "trigger.") &&
        app->page == CLAWT_PAGE_TRIGGERS)
        refresh_all(app, CLAWT_LRG_QUIET);
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_TRIGGERS, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_triggers_page(void)
{
    return &page_impl;
}
