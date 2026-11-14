/*
 * gtk-triggers.c - The triggers page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Work an agent does because something happened elsewhere, and the
 * editor that describes one.  It sits beside the routines page because a
 * routine is a clock and a trigger is an event, and the two end in the
 * same queued run against the same agent.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

/* ── Triggers ────────────────────────────────────────────────────── */

typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    gchar       *id;
    gboolean     creating;

    GtkWidget   *id_row;
    GtkWidget   *description_row;
    GtkTextView *instructions;
    GtkWidget   *agent_row;
    GtkWidget   *agents_row;
    GStrv        agent_ids;
    GtkWidget   *room_row;
    GtkWidget   *provider_row;
    GtkWidget   *events_row;
    GtkWidget   *repo_row;
    GtkWidget   *branch_row;
    GtkWidget   *header_row;
    GtkWidget   *ignore_actors_row;
    GtkWidget   *replay_connector_row;
    GtkWidget   *organization_row;
    GtkWidget   *directory_row;
    GtkWidget   *worktree_row;
    GtkWidget   *isolate_row;
    GtkWidget   *enabled_row;
    GtkWidget   *mode_row;
    GtkWidget   *pending_row;
    GtkWidget   *match_group;
    GtkWidget   *match_add;
    GPtrArray   *match_rows;    /* MatchRow*, in the order they are shown */
    gboolean     match_unreadable;
    guint        shown_mode;     /* the selection the editor opened with */
    gint64       shown_pending;  /* and the cap */
} TriggerDialog;

/*
 * One "path contains value" condition in the editor.
 *
 * Kept here rather than found by walking the group, for the reason the
 * connections dialog gives: AdwPreferencesGroup wraps what it is handed
 * and offers no way back to it.
 */
typedef struct {
	GtkWidget *row;
	GtkWidget *path;
	GtkWidget *value;
} MatchRow;

static void
trigger_dialog_free(gpointer data)
{
    TriggerDialog *dialog = data;

    g_free(dialog->id);
    g_strfreev(dialog->agent_ids);
    g_clear_pointer(&dialog->match_rows, g_ptr_array_unref);
    g_free(dialog);
}

/* ── The payload filter ──────────────────────────────────────────── */

static void
on_match_row_removed(GtkButton *button, gpointer user_data)
{
	TriggerDialog *dialog = user_data;
	MatchRow *match = g_object_get_data(G_OBJECT(button), "match-row");

	if (match == NULL)
		return;

	adw_preferences_group_remove(ADW_PREFERENCES_GROUP(dialog->match_group),
	                             match->row);
	g_ptr_array_remove(dialog->match_rows, match);
	gtk_widget_set_sensitive(dialog->match_add,
	                         dialog->match_rows->len < CLAWT_TRIGGER_MATCH_MAX);
}

/*
 * Adds a condition row, pre-filled when it came from an existing filter
 * or from a field picked out of the last delivery.
 *
 * The add button goes insensitive at the limit rather than the eleventh
 * row being refused on save: the daemon would refuse it, and a control
 * that lets somebody build what will be refused is a bug report about
 * the control.
 */
static void
match_row_add(TriggerDialog *dialog, const gchar *path, const gchar *value)
{
	MatchRow *match;
	GtkWidget *box;
	GtkWidget *contains;
	GtkWidget *remove;

	if (dialog->match_rows->len >= CLAWT_TRIGGER_MATCH_MAX) {
		clawt_window_toast(dialog->window,
		                   "Ten conditions is the most a trigger can hold.");
		return;
	}

	match = g_new0(MatchRow, 1);
	match->row = adw_preferences_row_new();
	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	match->path = gtk_entry_new();
	match->value = gtk_entry_new();
	contains = gtk_label_new("contains");
	remove = gtk_button_new_from_icon_name("list-remove-symbolic");

	gtk_entry_set_placeholder_text(GTK_ENTRY(match->path),
	                               "pull_request.title");
	gtk_entry_set_placeholder_text(GTK_ENTRY(match->value), "release");
	gtk_editable_set_text(GTK_EDITABLE(match->path), path != NULL ? path : "");
	gtk_editable_set_text(GTK_EDITABLE(match->value),
	                      value != NULL ? value : "");
	gtk_widget_set_hexpand(match->path, TRUE);
	gtk_widget_set_hexpand(match->value, TRUE);
	gtk_widget_add_css_class(contains, "dim-label");
	gtk_widget_add_css_class(remove, "flat");
	gtk_widget_set_tooltip_text(remove, "Remove this condition");
	g_object_set_data(G_OBJECT(remove), "match-row", match);
	g_signal_connect(remove, "clicked", G_CALLBACK(on_match_row_removed),
	                 dialog);

	gtk_widget_set_margin_top(box, 6);
	gtk_widget_set_margin_bottom(box, 6);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 6);
	gtk_box_append(GTK_BOX(box), match->path);
	gtk_box_append(GTK_BOX(box), contains);
	gtk_box_append(GTK_BOX(box), match->value);
	gtk_box_append(GTK_BOX(box), remove);
	gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(match->row), box);
	gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(match->row), FALSE);

	adw_preferences_group_add(ADW_PREFERENCES_GROUP(dialog->match_group),
	                          match->row);
	g_ptr_array_add(dialog->match_rows, match);
	gtk_widget_set_sensitive(dialog->match_add,
	                         dialog->match_rows->len < CLAWT_TRIGGER_MATCH_MAX);
}

static void
on_match_add(GtkButton *button, gpointer user_data)
{
	(void)button;

	match_row_add(user_data, NULL, NULL);
}

/*
 * The rows as the daemon reads them: a {path, value} object each, with
 * a wholly blank row left out rather than sent as a pair that would be
 * refused for having no path.
 */
static JsonNode *
match_rows_node(TriggerDialog *dialog)
{
	g_autoptr(JsonBuilder) builder = json_builder_new();
	guint i;

	json_builder_begin_array(builder);

	for (i = 0; i < dialog->match_rows->len; i++) {
		MatchRow *match = g_ptr_array_index(dialog->match_rows, i);
		const gchar *path = gtk_editable_get_text(GTK_EDITABLE(match->path));
		const gchar *value = gtk_editable_get_text(
			GTK_EDITABLE(match->value));

		if (*path == '\0' && *value == '\0')
			continue;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "path");
		json_builder_add_string_value(builder, path);
		json_builder_set_member_name(builder, "value");
		json_builder_add_string_value(builder, value);
		json_builder_end_object(builder);
	}

	json_builder_end_array(builder);

	return json_builder_get_root(builder);
}

/* A field picked out of the last delivery becomes a condition. */
static void
on_match_field_picked(AdwActionRow *row, gpointer user_data)
{
	TriggerDialog *dialog = user_data;

	match_row_add(dialog, g_object_get_data(G_OBJECT(row), "path"),
	              g_object_get_data(G_OBJECT(row), "value"));
	clawt_window_toast(dialog->window, "Added as a condition.");
}

/*
 * The conditions being edited, against the newest body this trigger
 * received -- before anything is saved -- and every field in that body,
 * each one a click away from being a condition.
 *
 * This is what makes a path something picked rather than guessed: the
 * person sees the forge's actual shape, not the shape they remember.
 */
static void
on_match_check(GtkButton *button, gpointer user_data)
{
	TriggerDialog *dialog = user_data;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) reply = NULL;
	JsonObject *answer;
	JsonArray *results;
	JsonArray *paths;
	AdwDialog *window;
	GtkWidget *page = adw_preferences_page_new();
	GtkWidget *verdict = adw_preferences_group_new();
	GtkWidget *fields = adw_preferences_group_new();
	GtkWidget *toolbar = adw_toolbar_view_new();
	guint i;

	(void)button;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "id");
	json_builder_add_string_value(builder, dialog->id);
	json_builder_set_member_name(builder, "match");
	json_builder_add_value(builder, match_rows_node(dialog));
	json_builder_end_object(builder);

	reply = clawt_window_request(dialog->window, "trigger.match_test",
	                             json_builder_get_root(builder));

	if (reply == NULL)
		return;

	answer = clawt_payload_of(reply);

	if (!json_object_has_member(answer, "payload")) {
		clawt_window_toast(dialog->window,
		                   clawt_json_string(answer, "note",
		                                     "Nothing has arrived yet."));
		return;
	}

	window = adw_dialog_new();
	adw_dialog_set_title(window, "Against the last delivery");
	adw_dialog_set_content_width(window, 640);
	adw_dialog_set_content_height(window, 620);

	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(verdict),
	                                json_object_get_boolean_member(answer,
	                                                               "matches")
	                                    ? "It would have been let through"
	                                    : "It would have been ignored");

	if (json_object_has_member(answer, "reason"))
		adw_preferences_group_set_description(
			ADW_PREFERENCES_GROUP(verdict),
			clawt_json_string(answer, "reason", ""));

	results = json_object_get_array_member(answer, "results");

	for (i = 0; i < json_array_get_length(results); i++) {
		JsonObject *result = json_array_get_object_element(results, i);
		GtkWidget *row = adw_action_row_new();
		g_autofree gchar *title = g_strdup_printf(
			"%s contains \342\200\234%s\342\200\235",
			clawt_json_string(result, "path", "?"),
			clawt_json_string(result, "value", ""));
		g_autofree gchar *subtitle = NULL;

		if (!json_object_get_boolean_member(result, "found"))
			subtitle = g_strdup("Does not match: nothing is there");
		else
			subtitle = g_strdup_printf(
				"%s: it is \342\200\234%s\342\200\235",
				json_object_get_boolean_member(result, "matched")
					? "Matches" : "Does not match",
				clawt_json_string(result, "actual", ""));

		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(verdict), row);
	}

	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(fields),
	                                "Fields in it");
	adw_preferences_group_set_description(
		ADW_PREFERENCES_GROUP(fields),
		"Click one to make it a condition. A name on a list, such as "
		"labels.name, looks in every item of it.");

	paths = json_object_get_array_member(answer, "paths");

	for (i = 0; i < json_array_get_length(paths); i++) {
		JsonObject *field = json_array_get_object_element(paths, i);
		GtkWidget *row = adw_action_row_new();
		const gchar *path = clawt_json_string(field, "path", "");
		const gchar *value = clawt_json_string(field, "value", "");

		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), path);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value);
		gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
		g_object_set_data_full(G_OBJECT(row), "path", g_strdup(path),
		                       g_free);
		g_object_set_data_full(G_OBJECT(row), "value", g_strdup(value),
		                       g_free);
		g_signal_connect(row, "activated",
		                 G_CALLBACK(on_match_field_picked), dialog);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(fields), row);
	}

	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
	                         ADW_PREFERENCES_GROUP(verdict));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
	                         ADW_PREFERENCES_GROUP(fields));
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
	                             adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
	adw_dialog_set_child(window, toolbar);

	adw_dialog_present(window, GTK_WIDGET(dialog->dialog));
}

/*
 * The line under the mode selector is the library's sentence for the
 * selected mode, so it says what the choice costs -- and the cap, which
 * only a run consumes, is greyed out for a post.
 */
static void
on_mode_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
	TriggerDialog *dialog = user_data;
	guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(object));

	(void)pspec;

	if (selected >= clawt_trigger_mode_count())
		return;

	clawt_gtk_set_row_hint(dialog->mode_row,
	                       clawt_trigger_mode_nth_hint(selected));

	if (dialog->pending_row != NULL)
		gtk_widget_set_sensitive(
			dialog->pending_row,
			clawt_trigger_mode_nth(selected) == CLAWT_TRIGGER_MODE_RUN);
}

/*
 * Shows a secret exactly once, and says that it is the only time.
 *
 * A dialog rather than a toast: a toast is answering a question somebody
 * is holding right now and then goes away, and this is a value they have
 * to copy into another program's form before it does. The sentence about
 * rotation is not decoration -- without it somebody closes this and then
 * goes looking for the secret in the listing, where it will never be.
 */
static void
show_secret_once(ClawtWindow *self, JsonObject *reply, const gchar *lead)
{
    const gchar *secret = clawt_json_string(reply, "secret", NULL);
    const gchar *endpoint = clawt_json_string(reply, "endpoint", NULL);
    AdwDialog *window;
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *toolbar = adw_toolbar_view_new();

    if (secret == NULL)
        return;

    window = adw_dialog_new();
    adw_dialog_set_title(window, "Copy this now");
    adw_dialog_set_content_width(window, 620);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "The webhook's settings");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group), lead);

    if (endpoint != NULL) {
        GtkWidget *row = clawt_gtk_add_entry(group, "URL path", NULL);
        g_autofree gchar *path = g_strdup_printf("/hooks/%s", endpoint);

        gtk_editable_set_text(GTK_EDITABLE(row), path);
        gtk_editable_set_editable(GTK_EDITABLE(row), FALSE);
    }

    {
        GtkWidget *row = clawt_gtk_add_entry(group, "Secret", secret);

        gtk_editable_set_editable(GTK_EDITABLE(row), FALSE);
    }

    /*
     * The sentence is printed because the daemon said the value is
     * shown once, not because this client assumes so. If a future
     * daemon ever made a secret re-readable, a hardcoded "this is the
     * only time" would be the client telling somebody to do something
     * irreversible for no reason.
     */
    if (json_object_get_boolean_member(reply, "secret_shown_once")) {
        GtkWidget *note = gtk_label_new(
            "This is the only time the secret is shown. It is not in the "
            "listing, the log or an event. If you lose it, rotate the "
            "trigger -- that makes a new secret and a new address, and "
            "retires both of these.");

        gtk_label_set_wrap(GTK_LABEL(note), TRUE);
        gtk_label_set_xalign(GTK_LABEL(note), 0.0f);
        gtk_widget_add_css_class(note, "dim-label");
        gtk_widget_set_margin_top(note, 12);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), note);
    }

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(window, toolbar);

    adw_dialog_present(window, GTK_WIDGET(self));
}

static gchar *
trigger_instructions_text(TriggerDialog *dialog)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(dialog->instructions);
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);

    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void
on_trigger_saved(GtkButton *button, gpointer user_data)
{
    TriggerDialog *dialog = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *instructions = trigger_instructions_text(dialog);
    const gchar *id = dialog->creating
        ? gtk_editable_get_text(GTK_EDITABLE(dialog->id_row)) : dialog->id;
    guint agent = adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->agent_row));
    guint provider = adw_combo_row_get_selected(
        ADW_COMBO_ROW(dialog->provider_row));

    (void)button;

    if (id == NULL || *id == '\0') {
        clawt_window_toast(dialog->window, "It needs a name.");
        return;
    }

    if (*instructions == '\0') {
        /*
         * Refused rather than saved empty, as a routine's are: a trigger
         * with no instructions wakes an agent and asks it for nothing,
         * which costs a turn and produces a puzzled reply -- and here it
         * happens whenever somebody else pushes.
         */
        clawt_window_toast(dialog->window,
                           "It needs instructions -- that is the whole of "
                           "what the agent is asked.");
        return;
    }

    if (dialog->agent_ids == NULL || dialog->agent_ids[0] == NULL) {
        clawt_window_toast(dialog->window, "There is no agent to run it.");
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
    json_builder_set_member_name(builder, "room");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->room_row)));

    /*
     * The nick from the library's own enumeration, never a string
     * spelled here. A list in this file and a list in clawt-enums.c
     * would drift, and the drift is a provider a client offers that the
     * daemon does not understand -- which reads as a trigger that
     * refuses every delivery.
     */
    json_builder_set_member_name(builder, "provider");
    json_builder_add_string_value(
        builder, clawt_trigger_provider_nth_nick(
                     MIN(provider, clawt_trigger_provider_count() - 1)));

    json_builder_set_member_name(builder, "events");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->events_row)));
    json_builder_set_member_name(builder, "agents");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->agents_row)));
    json_builder_set_member_name(builder, "repo");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->repo_row)));
    json_builder_set_member_name(builder, "branch");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->branch_row)));
    json_builder_set_member_name(builder, "header");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->header_row)));
    json_builder_set_member_name(builder, "ignore_actors");
    json_builder_add_string_value(
        builder,
        gtk_editable_get_text(GTK_EDITABLE(dialog->ignore_actors_row)));
    json_builder_set_member_name(builder, "replay_connector");
    json_builder_add_string_value(
        builder,
        gtk_editable_get_text(GTK_EDITABLE(dialog->replay_connector_row)));
    json_builder_set_member_name(builder, "organization_id");
    json_builder_add_int_value(
        builder,
        g_ascii_strtoll(gtk_editable_get_text(
                            GTK_EDITABLE(dialog->organization_row)),
                        NULL, 10));
    json_builder_set_member_name(builder, "directory");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->directory_row)));
    json_builder_set_member_name(builder, "worktree");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(
                     ADW_SWITCH_ROW(dialog->worktree_row)));
    json_builder_set_member_name(builder, "isolate");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(
                     ADW_SWITCH_ROW(dialog->isolate_row)));
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(
                     ADW_SWITCH_ROW(dialog->enabled_row)));

    /*
     * Mode and cap only when somebody moved them.  The controls open on
     * what the trigger already has -- the schema default when the file
     * says nothing -- so sending them regardless would write `mode: run`
     * and `max_pending: 4` into clawtilla.yaml on every save of a trigger
     * that never chose either.  The nick comes from the library's table,
     * as the provider's does.
     */
    {
        guint mode = adw_combo_row_get_selected(
            ADW_COMBO_ROW(dialog->mode_row));
        gint64 pending = (gint64)adw_spin_row_get_value(
            ADW_SPIN_ROW(dialog->pending_row));

        if (mode != dialog->shown_mode) {
            json_builder_set_member_name(builder, "mode");
            json_builder_add_string_value(
                builder, clawt_trigger_mode_nth_nick(
                             MIN(mode, clawt_trigger_mode_count() - 1)));
        }

        if (pending != dialog->shown_pending) {
            json_builder_set_member_name(builder, "max_pending");
            json_builder_add_int_value(builder, pending);
        }
    }
    /*
     * A filter the daemon could not read is drawn as no rows, so an
     * untouched editor would send [] and delete it.  Sent only when
     * somebody wrote a row to replace it.
     */
    {
        g_autoptr(JsonNode) match = match_rows_node(dialog);

        if (!dialog->match_unreadable ||
            json_array_get_length(json_node_get_array(match)) > 0) {
            json_builder_set_member_name(builder, "match");
            json_builder_add_value(builder, g_steal_pointer(&match));
        }
    }
    json_builder_end_object(builder);

    reply = clawt_window_request(dialog->window,
                                 dialog->creating ? "trigger.add"
                                                  : "trigger.update",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    if (dialog->creating)
        show_secret_once(dialog->window, clawt_payload_of(reply),
                         "Put these into the webhook on the forge. The "
                         "trigger stays switched off until you have seen "
                         "what it sends.");

    clawt_gtk_refresh_triggers(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_trigger_removed(GtkButton *button, gpointer user_data)
{
    TriggerDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    reply = clawt_window_request(dialog->window, "trigger.remove",
                                 clawt_build_payload("id", dialog->id, NULL));

    if (reply == NULL)
        return;

    clawt_gtk_refresh_triggers(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_trigger_rotated(GtkButton *button, gpointer user_data)
{
    TriggerDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    reply = clawt_window_request(dialog->window, "trigger.rotate",
                                 clawt_build_payload("id", dialog->id, NULL));

    if (reply == NULL)
        return;

    show_secret_once(dialog->window, clawt_payload_of(reply),
                     "The old secret and the old address stopped working "
                     "just now. The webhook needs both of these before it "
                     "will be accepted again.");

    clawt_gtk_refresh_triggers(dialog->window);
    adw_dialog_close(dialog->dialog);
}

/*
 * Shows the delivery that was held back, so somebody can read it before
 * an agent does.
 *
 * This is the whole of the verification handshake from a person's side:
 * the first authenticated delivery is captured rather than run, and this
 * is where they find out whether the thing calling the endpoint is the
 * thing they registered.
 */
static void
on_trigger_capture(GtkButton *button, gpointer user_data)
{
    TriggerDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *payload;
    AdwDialog *window;
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();

    (void)button;

    reply = clawt_window_request(dialog->window, "trigger.capture",
                                 clawt_build_payload("id", dialog->id, NULL));

    if (reply == NULL)
        return;

    payload = clawt_json_string(clawt_payload_of(reply), "payload", NULL);

    if (payload == NULL) {
        clawt_window_toast(dialog->window,
                           "Nothing has arrived yet. Send it a test "
                           "delivery from the forge.");
        return;
    }

    window = adw_dialog_new();
    adw_dialog_set_title(window, "The first delivery");
    adw_dialog_set_content_width(window, 720);
    adw_dialog_set_content_height(window, 620);

    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                             payload, -1);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
    adw_dialog_set_child(window, toolbar);

    adw_dialog_present(window, GTK_WIDGET(dialog->window));
}

/*
 * Shows what the agent would be asked, without asking it.
 *
 * The template is expanded against a made-up delivery, so somebody can
 * see the placeholders filled in -- and see the untrusted-payload fence
 * -- before a forge sends anything real.
 */
static void
on_trigger_test(GtkButton *button, gpointer user_data)
{
    TriggerDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *prompt;
    AdwDialog *window;
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *view = gtk_text_view_new();

    (void)button;

    reply = clawt_window_request(dialog->window,
                                 g_object_get_data(G_OBJECT(button), "replay") != NULL
                                     ? "trigger.replay" : "trigger.test",
                                 clawt_build_payload("id", dialog->id, NULL));

    if (reply == NULL)
        return;

    prompt = clawt_json_string(clawt_payload_of(reply), "report",
        clawt_json_string(clawt_payload_of(reply), "prompt", ""));

    window = adw_dialog_new();
    adw_dialog_set_title(window, "What the agent would be asked");
    adw_dialog_set_content_width(window, 720);
    adw_dialog_set_content_height(window, 620);

    gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(view), 12);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)),
                             prompt, -1);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), view);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
    adw_dialog_set_child(window, toolbar);

    adw_dialog_present(window, GTK_WIDGET(dialog->window));
}

static void
open_trigger_editor(ClawtWindow *self, JsonObject *existing)
{
    TriggerDialog *dialog = g_new0(TriggerDialog, 1);
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *what = adw_preferences_group_new();
    GtkWidget *listen = adw_preferences_group_new();
    GtkWidget *where = adw_preferences_group_new();
    GtkWidget *actions = adw_preferences_group_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkStringList *agent_labels = gtk_string_list_new(NULL);
    GtkStringList *provider_labels = gtk_string_list_new(NULL);
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(JsonNode) agents = NULL;
    GtkWidget *save;
    guint i;

    dialog->window = self;
    dialog->dialog = window;
    dialog->creating = existing == NULL;
    dialog->id = g_strdup(existing != NULL
                          ? clawt_json_string(existing, "id", "") : "");

    adw_dialog_set_title(window, dialog->creating ? "New trigger"
                                                  : dialog->id);
    adw_dialog_set_content_width(window, 620);
    adw_dialog_set_content_height(window, 760);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(what),
                                    "What it does");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(what),
        "Placeholders in the instructions -- {{event}}, {{repo}}, "
        "{{ref}}, {{actor}}, {{title}}, {{url}}, {{number}} -- are "
        "filled in from the delivery. The body itself is added below "
        "them, marked as somebody else's text.");

    dialog->id_row = clawt_gtk_add_entry(what, "Name", dialog->id);
    gtk_widget_set_sensitive(dialog->id_row, dialog->creating);

    dialog->description_row = clawt_gtk_add_entry(
        what, "Description",
        existing != NULL ? clawt_json_string(existing, "description", "")
                         : "");

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

    /* ── What it listens for ── */

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(listen),
                                    "What wakes it");

    /*
     * Walked from the library's enumeration rather than written out.
     * A provider added to clawt-enums.c reaches both clients from the
     * moment it exists, and neither can offer one the daemon does not
     * understand.
     */
    for (i = 0; i < clawt_trigger_provider_count(); i++)
        gtk_string_list_append(provider_labels,
                               clawt_trigger_provider_nth_label(i));

    dialog->provider_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->provider_row),
                                  "Sent by");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->provider_row),
                            G_LIST_MODEL(provider_labels));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->provider_row),
        "Each forge authenticates differently, so this is not cosmetic. "
        "Naming it also stops a Forgejo delivery being read as the "
        "GitHub it also pretends to be.");

    if (existing != NULL) {
        const gchar *provider = clawt_json_string(existing, "provider",
                                                  "generic");

        for (i = 0; i < clawt_trigger_provider_count(); i++) {
            if (g_strcmp0(clawt_trigger_provider_nth_nick(i), provider) == 0)
                adw_combo_row_set_selected(
                    ADW_COMBO_ROW(dialog->provider_row), i);
        }
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(listen),
                              dialog->provider_row);

    {
        g_autofree gchar *events = NULL;

        if (existing != NULL &&
            json_object_has_member(existing, "events") &&
            JSON_NODE_HOLDS_ARRAY(json_object_get_member(existing,
                                                         "events"))) {
            JsonArray *array = json_object_get_array_member(existing,
                                                            "events");
            g_autoptr(GString) joined = g_string_new(NULL);
            guint e;

            for (e = 0; e < json_array_get_length(array); e++) {
                if (joined->len > 0)
                    g_string_append(joined, ", ");

                g_string_append(joined,
                                json_array_get_string_element(array, e));
            }

            events = g_strdup(joined->str);
        }

        dialog->events_row = clawt_gtk_add_entry(listen, "Events",
                                                 events != NULL ? events
                                                                : "");
        clawt_gtk_set_row_hint(
            dialog->events_row,
            "Comma separated, such as push, pull_request. Empty means "
            "every event it is sent.");
    }

    dialog->repo_row = clawt_gtk_add_entry(
        listen, "Repository",
        existing != NULL ? clawt_json_string(existing, "repo", "") : "");
    clawt_gtk_set_row_hint(dialog->repo_row,
                           "Such as zach/clawtilla. Empty means any.");

    dialog->branch_row = clawt_gtk_add_entry(
        listen, "Branch",
        existing != NULL ? clawt_json_string(existing, "branch", "") : "");
    clawt_gtk_set_row_hint(dialog->branch_row,
                           "Such as master. Empty means any.");

    dialog->header_row = clawt_gtk_add_entry(
        listen, "Event header",
        existing != NULL ? clawt_json_string(existing, "header", "") : "");
    clawt_gtk_set_row_hint(
        dialog->header_row,
        "Only for a generic sender: which header carries the event name.");

    {
        g_autoptr(GString) joined = g_string_new(NULL);

        if (existing != NULL &&
            json_object_has_member(existing, "ignore_actors") &&
            JSON_NODE_HOLDS_ARRAY(json_object_get_member(existing,
                                                         "ignore_actors"))) {
            JsonArray *array = json_object_get_array_member(existing,
                                                            "ignore_actors");
            guint a;

            for (a = 0; a < json_array_get_length(array); a++) {
                if (joined->len > 0)
                    g_string_append(joined, ", ");

                g_string_append(joined,
                                json_array_get_string_element(array, a));
            }
        }

        dialog->ignore_actors_row = clawt_gtk_add_entry(
            listen, "Ignore changes by", joined->str);
        clawt_gtk_set_row_hint(
            dialog->ignore_actors_row,
            "Comma separated accounts. Name the one the agent's own "
            "credential belongs to, so it is not woken by its own writes.");
    }

    /* ── Only when the body says so ── */

    dialog->match_rows = g_ptr_array_new_with_free_func(g_free);
    dialog->match_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(dialog->match_group),
                                    "Only when the body contains");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(dialog->match_group),
        "Each condition names a field by its dotted path and text it must "
        "contain, ignoring case; all of them must hold. A delivery that "
        "misses is recorded as ignored, naming the condition, and starts "
        "nothing. None means every delivery.");

    {
        GtkWidget *suffix = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        dialog->match_add = gtk_button_new_from_icon_name("list-add-symbolic");
        gtk_widget_add_css_class(dialog->match_add, "flat");
        gtk_widget_set_tooltip_text(dialog->match_add, "Add a condition");
        g_signal_connect(dialog->match_add, "clicked",
                         G_CALLBACK(on_match_add), dialog);
        gtk_box_append(GTK_BOX(suffix), dialog->match_add);

        if (existing != NULL) {
            GtkWidget *check = gtk_button_new_with_label(
                "Check against the last delivery");

            gtk_widget_add_css_class(check, "flat");
            g_signal_connect(check, "clicked", G_CALLBACK(on_match_check),
                             dialog);
            gtk_box_append(GTK_BOX(suffix), check);
        }

        adw_preferences_group_set_header_suffix(
            ADW_PREFERENCES_GROUP(dialog->match_group), suffix);
    }

    if (existing != NULL &&
        json_object_has_member(existing, "match") &&
        JSON_NODE_HOLDS_ARRAY(json_object_get_member(existing, "match"))) {
        JsonArray *array = json_object_get_array_member(existing, "match");
        guint m;

        for (m = 0; m < json_array_get_length(array); m++) {
            JsonObject *pair = json_array_get_object_element(array, m);

            match_row_add(dialog, clawt_json_string(pair, "path", ""),
                          clawt_json_string(pair, "value", ""));
        }
    }

    /*
     * A filter the daemon could not read is said, not drawn as no rows:
     * saving an empty editor over it would delete what somebody wrote.
     */
    if (existing != NULL && json_object_has_member(existing, "match_error")) {
        g_autofree gchar *said = g_strdup_printf(
            "The filter as written cannot be read, so the trigger refuses "
            "every delivery: %s. It is left as it is unless you add "
            "conditions here to replace it.",
            clawt_json_string(existing, "match_error", ""));

        dialog->match_unreadable = TRUE;
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(dialog->match_group), said);
    }

    dialog->replay_connector_row = clawt_gtk_add_entry(
        listen, "Catch up through",
        existing != NULL ? clawt_json_string(existing, "replay_connector", "")
                         : "");
    clawt_gtk_set_row_hint(
        dialog->replay_connector_row,
        "VENTURE only: a venture connector on this agent whose token, an "
        "owner's, reads the business-event feed to replay what arrived "
        "while the daemon was down. Empty means no catch-up.");

    {
        g_autofree gchar *organization = g_strdup_printf(
            "%" G_GINT64_FORMAT,
            existing != NULL
                ? clawt_json_int(existing, "organization_id", 0) : 0);

        dialog->organization_row = clawt_gtk_add_entry(
            listen, "Organization", organization);
        clawt_gtk_set_row_hint(
            dialog->organization_row,
            "VENTURE only: which organization's events to catch up on. "
            "0 means the token's own.");
    }

    /* ── Who runs it ── */

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(where),
                                    "Who runs it");

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

    {
        g_autoptr(GString) joined = g_string_new(NULL);
        if (existing != NULL && json_object_has_member(existing, "agents") &&
            JSON_NODE_HOLDS_ARRAY(json_object_get_member(existing, "agents"))) {
            JsonArray *array = json_object_get_array_member(existing, "agents");
            guint recipient;
            for (recipient = 0; recipient < json_array_get_length(array); recipient++) {
                if (joined->len > 0)
                    g_string_append(joined, ", ");
                g_string_append(joined, json_array_get_string_element(array, recipient));
            }
        }
        dialog->agents_row = clawt_gtk_add_entry(where, "Additional agents", joined->str);
        clawt_gtk_set_row_hint(dialog->agents_row,
            "Comma separated agent IDs. The primary runs first; duplicates are removed. Each one takes a slot of Runs at once.");
    }

    dialog->room_row = clawt_gtk_add_entry(
        where, "Room",
        existing != NULL ? clawt_json_string(existing, "room", "") : "");
    clawt_gtk_set_row_hint(
        dialog->room_row,
        "Where the run reports, or where a post lands -- a post needs "
        "one. Empty means the agent's own conversation.");

    /*
     * What a delivery turns into, walked from the library so the two
     * sentences are the ones every client shows.
     */
    {
        GtkStringList *mode_labels = gtk_string_list_new(NULL);
        const gchar *mode = existing != NULL
            ? clawt_json_string(existing, "mode", "run") : "run";
        guint selected = 0;

        for (i = 0; i < clawt_trigger_mode_count(); i++) {
            gtk_string_list_append(mode_labels,
                                   clawt_trigger_mode_nth_label(i));

            if (g_strcmp0(clawt_trigger_mode_nth_nick(i), mode) == 0)
                selected = i;
        }

        dialog->mode_row = adw_combo_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->mode_row),
                                      "When it fires");
        adw_combo_row_set_model(ADW_COMBO_ROW(dialog->mode_row),
                                G_LIST_MODEL(mode_labels));
        adw_combo_row_set_selected(ADW_COMBO_ROW(dialog->mode_row), selected);
        dialog->shown_mode = selected;
        clawt_gtk_set_row_hint(dialog->mode_row,
                               clawt_trigger_mode_nth_hint(selected));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(where),
                                  dialog->mode_row);
    }

    {
        const ClawtSchemaEntry *entry =
            clawt_config_schema_lookup("triggers.max_pending");
        gint64 fallback = (entry != NULL && entry->default_value != NULL)
            ? g_ascii_strtoll(entry->default_value, NULL, 10)
            : CLAWT_TRIGGER_MAX_PENDING_MIN;

        dialog->pending_row = adw_spin_row_new_with_range(
            CLAWT_TRIGGER_MAX_PENDING_MIN, CLAWT_TRIGGER_MAX_PENDING_MAX, 1);
        adw_preferences_row_set_title(
            ADW_PREFERENCES_ROW(dialog->pending_row), "Runs at once");
        adw_spin_row_set_value(
            ADW_SPIN_ROW(dialog->pending_row),
            (gdouble)(existing != NULL
                          ? clawt_json_int(existing, "max_pending", fallback)
                          : fallback));
        /* Read back, so a value the spinner clamped compares as shown. */
        dialog->shown_pending = (gint64)adw_spin_row_get_value(
            ADW_SPIN_ROW(dialog->pending_row));
        clawt_gtk_set_row_hint(
            dialog->pending_row,
            "How many runs may be unfinished together. A delivery past "
            "this is answered \"try later\" and recorded as dropped.");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(where),
                                  dialog->pending_row);
        gtk_widget_set_sensitive(
            dialog->pending_row,
            clawt_trigger_mode_nth(adw_combo_row_get_selected(
                ADW_COMBO_ROW(dialog->mode_row))) == CLAWT_TRIGGER_MODE_RUN);
    }

    g_signal_connect(dialog->mode_row, "notify::selected",
                     G_CALLBACK(on_mode_changed), dialog);

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
        "Worth turning on when somebody else's push is what starts this: "
        "it keeps the run off whatever you had checked out.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->worktree_row),
        existing != NULL &&
        json_object_get_boolean_member(existing, "worktree"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(where),
                              dialog->worktree_row);

    dialog->isolate_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->isolate_row),
                                  "Give it a conversation of its own");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->isolate_row),
        "Keeps the runs out of the chat you type in.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->isolate_row),
        existing != NULL &&
        json_object_get_boolean_member(existing, "isolate"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(where),
                              dialog->isolate_row);

    dialog->enabled_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->enabled_row),
                                  "Enabled");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->enabled_row),
        dialog->creating
            ? "A new trigger always starts off, whatever this says: the "
              "first delivery is held for you to read before an agent "
              "acts on it."
            : "While it is off, its address answers as though it does not "
              "exist.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->enabled_row),
        existing != NULL &&
        json_object_get_boolean_member(existing, "enabled"));
    gtk_widget_set_sensitive(dialog->enabled_row, !dialog->creating);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(where),
                              dialog->enabled_row);

    /* ── Actions ── */

    save = gtk_button_new_with_label(dialog->creating ? "Create" : "Save");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_hexpand(save, TRUE);
    g_signal_connect(save, "clicked", G_CALLBACK(on_trigger_saved), dialog);
    gtk_box_append(GTK_BOX(buttons), save);

    if (!dialog->creating) {
        GtkWidget *inspection = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *preview = gtk_button_new_with_label("Preview prompt");
        GtkWidget *replay = gtk_button_new_with_label("Explain latest delivery");
        GtkWidget *capture = gtk_button_new_with_label("First delivery");
        GtkWidget *rotate = gtk_button_new_with_label("Rotate secret");
        GtkWidget *remove = gtk_button_new_with_label("Remove");

        g_object_set_data(G_OBJECT(replay), "replay", GINT_TO_POINTER(1));
        g_signal_connect(replay, "clicked", G_CALLBACK(on_trigger_test), dialog);
        gtk_widget_set_hexpand(replay, TRUE);
        gtk_box_append(GTK_BOX(inspection), replay);
        gtk_widget_set_hexpand(preview, TRUE);
        g_signal_connect(preview, "clicked", G_CALLBACK(on_trigger_test),
                         dialog);
        gtk_box_append(GTK_BOX(inspection), preview);

        gtk_widget_set_hexpand(capture, TRUE);
        g_signal_connect(capture, "clicked", G_CALLBACK(on_trigger_capture),
                         dialog);
        gtk_box_append(GTK_BOX(inspection), capture);
        /* Inspection actions get their own row so six labels cannot
         * force the 620-pixel editor wider than a small laptop window. */
        gtk_widget_set_margin_top(inspection, 12);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), inspection);

        gtk_widget_set_hexpand(rotate, TRUE);
        g_signal_connect(rotate, "clicked", G_CALLBACK(on_trigger_rotated),
                         dialog);
        gtk_box_append(GTK_BOX(buttons), rotate);

        gtk_widget_add_css_class(remove, "destructive-action");
        gtk_widget_set_hexpand(remove, TRUE);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_trigger_removed),
                         dialog);
        gtk_box_append(GTK_BOX(buttons), remove);
    }

    gtk_widget_set_margin_top(buttons, 12);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), buttons);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(what));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(listen));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(dialog->match_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(where));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(actions));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(window, toolbar);

    g_object_set_data_full(G_OBJECT(window), "dialog", dialog,
                           trigger_dialog_free);

    adw_dialog_present(window, GTK_WIDGET(self));
}

static void
on_add_trigger(GtkButton *button, gpointer user_data)
{
    (void)button;

    open_trigger_editor(user_data, NULL);
}

static void
on_trigger_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    JsonObject *trigger;

    (void)box;

    if (row == NULL)
        return;

    trigger = g_object_get_data(G_OBJECT(row), "trigger");

    if (trigger == NULL)
        return;

    open_trigger_editor(self, trigger);
}

/*
 * What has been delivered, and what became of it.
 *
 * This is how "nothing happened" is answerable at all: the endpoint was
 * wrong, the secret was wrong, the event was filtered out, or the run
 * failed -- and without the receipts those four are indistinguishable
 * from each other and from a forge that never called.
 */
static void
fill_deliveries(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *deliveries;
    guint i;

    if (self->delivery_list == NULL)
        return;

    clawt_gtk_clear_list(self->delivery_list);

    reply = clawt_window_request(self, "trigger.deliveries", NULL);

    if (reply == NULL)
        return;

    deliveries = json_object_get_array_member(clawt_payload_of(reply),
                                              "deliveries");

    if (json_array_get_length(deliveries) == 0) {
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "Nothing yet");
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(row),
            clawt_json_string(clawt_payload_of(reply), "note", ""));
        gtk_list_box_append(self->delivery_list, row);
        return;
    }

    for (i = 0; i < json_array_get_length(deliveries); i++) {
        JsonObject *delivery = json_array_get_object_element(deliveries, i);
        GtkWidget *row = adw_action_row_new();
        const gchar *detail = clawt_json_string(delivery, "detail", NULL);
        gint64 at = g_ascii_strtoll(clawt_json_string(delivery, "at", "0"),
                                    NULL, 10);
        g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(at);
        g_autofree gchar *title = NULL;
        g_autofree gchar *subtitle = NULL;

        title = g_strdup_printf("%s \342\200\224 %s",
                                clawt_json_string(delivery, "trigger", "?"),
                                clawt_json_string(delivery, "outcome", "?"));

        subtitle = g_strdup_printf(
            "%s%s%s%s",
            clawt_json_string(delivery, "event", "-"),
            detail != NULL ? " \342\200\224 " : "",
            detail != NULL ? detail : "",
            when != NULL ? "" : "");

        if (when != NULL) {
            g_autofree gchar *stamp = g_date_time_format(when,
                                                         "%a %d %b %H:%M");
            g_autofree gchar *both = g_strdup_printf("%s \342\200\224 %s",
                                                     stamp, subtitle);

            g_free(g_steal_pointer(&subtitle));
            subtitle = g_steal_pointer(&both);
        }

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
        gtk_list_box_append(self->delivery_list, row);
    }
}

void
clawt_gtk_refresh_triggers(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *triggers;
    guint i;

    if (self->trigger_list == NULL)
        return;

    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_TRIGGERS))
        return;

    do {
        clawt_gtk_clear_list(self->trigger_list);

        reply = clawt_window_request(self, "trigger.list", NULL);

        if (reply == NULL)
            continue;

        triggers = json_object_get_array_member(clawt_payload_of(reply),
                                                "triggers");

        for (i = 0; i < json_array_get_length(triggers); i++) {
            JsonObject *trigger = json_array_get_object_element(triggers, i);
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *subtitle = NULL;
            const gchar *state;

            /*
             * One word, in priority order, rather than three flags a
             * reader has to combine. "Why is this not firing" wants one
             * answer, and the first thing that is true is always it.
             */
            if (!json_object_get_boolean_member(trigger, "has_secret"))
                state = "no secret -- it refuses everything";
            else if (json_object_get_boolean_member(trigger,
                                                    "pending_verification"))
                state = "waiting for its first delivery";
            else if (!json_object_get_boolean_member(trigger, "enabled"))
                state = "off";
            else
                state = "on";

            subtitle = g_strdup_printf(
                "%s \342\200\224 %s \342\200\224 %s",
                clawt_json_string(trigger, "agent", "?"),
                clawt_json_string(trigger, "provider", "generic"), state);

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(row),
                clawt_json_string(trigger, "description",
                                  clawt_json_string(trigger, "id", "?")));
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
            clawt_gtk_row_opens_something(row);

            g_object_set_data_full(G_OBJECT(row), "trigger",
                                   json_object_ref(trigger),
                                   (GDestroyNotify)json_object_unref);
            gtk_list_box_append(self->trigger_list, row);
        }

        fill_deliveries(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_TRIGGERS));
}

GtkWidget *
clawt_gtk_build_trigger_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add = gtk_button_new_with_label("New trigger");
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *heading = gtk_label_new("Recent deliveries");
    GtkWidget *lede = gtk_label_new(
        "A routine is a clock; a trigger is an event. The receiver is off "
        "until daemon.webhook_enabled is set, and it only ever serves "
        "/health and the secret /hooks paths.");

    self->trigger_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->trigger_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->trigger_list), "boxed-list");

    /*
     * The list is aligned to the top rather than filling the viewport: a
     * GtkListBox in a GtkScrolledWindow expands to it, so one short row
     * would draw a card with several hundred pixels of empty frame
     * under it.
     */
    gtk_widget_set_valign(GTK_WIDGET(self->trigger_list), GTK_ALIGN_START);
    g_signal_connect(self->trigger_list, "row-activated",
                     G_CALLBACK(on_trigger_activated), self);

    gtk_label_set_wrap(GTK_LABEL(lede), TRUE);
    gtk_label_set_xalign(GTK_LABEL(lede), 0.0f);
    gtk_widget_add_css_class(lede, "dim-label");

    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_set_halign(add, GTK_ALIGN_END);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_trigger), self);
    gtk_widget_set_hexpand(bar, TRUE);
    gtk_box_append(GTK_BOX(bar), add);

    self->delivery_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->delivery_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->delivery_list), "boxed-list");
    gtk_widget_set_valign(GTK_WIDGET(self->delivery_list), GTK_ALIGN_START);

    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_widget_add_css_class(heading, "heading");
    gtk_widget_set_margin_top(heading, 12);

    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), lede);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->trigger_list));
    gtk_box_append(GTK_BOX(box), heading);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->delivery_list));

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    /*
     * NEVER horizontally: a wrapping GtkLabel still reports its
     * unwrapped string as its natural width, and a scrolled window left
     * at AUTOMATIC gives its child exactly that -- so the lede above
     * would make the whole page scroll sideways rather than wrap.
     */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);

    return scroll;
}
