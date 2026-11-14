/*
 * gtk-prefs-orchestration.c - Settings: orchestration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * How agents talk to each other, as far as a person tunes it from here:
 * the acknowledgement check (orchestration.ack_check_model), the message
 * budget per task tree (orchestration.tree_message_budget), and how long
 * one thread of a round may run in each group room (rooms.max_chain).
 * The model choices are libclawt's walk of the model catalogue, so this
 * page, the web page and the libregnum page offer the same list.
 */

#include "clawt-window-private.h"

#include <string.h>

/* The stored value behind each row of the combo, in the same order. */
#define ACK_VALUES_KEY "clawt-ack-values"

static void
on_ack_model_changed(
	GObject    *object,
	GParamSpec *pspec,
	gpointer    user_data
){
	ClawtWindow *self = user_data;
	GPtrArray *values;
	guint index;
	g_autoptr(JsonNode) reply = NULL;

	(void)pspec;

	/* A refresh selecting the current value is not the person choosing it. */
	if (self->settings_ack_loading)
		return;

	values = g_object_get_data(object, ACK_VALUES_KEY);
	index = adw_combo_row_get_selected(ADW_COMBO_ROW(object));

	if (values == NULL || index >= values->len)
		return;

	reply = clawt_window_request(self, "orchestration.set",
		clawt_build_payload("ack_check_model",
			(const gchar *)g_ptr_array_index(values, index), NULL));

	if (reply != NULL)
		clawt_window_toast(self, index == 0
			? "Acknowledgement check off."
			: "Acknowledgement check saved.");
}

/* The room a thread-cap row edits. */
#define ROUND_ROOM_KEY "clawt-round-room"

/*
 * A whole number from an entry row, or -1 when it is not one.  Digits
 * only: "3 replies" or "-1" is refused at the page rather than sent for
 * the daemon to refuse, so the message says which field.
 */
static gint64
entry_number(
	GtkWidget *row
){
	const gchar *text = gtk_editable_get_text(GTK_EDITABLE(row));
	gint64 value = 0;

	if (text == NULL || *text == '\0' ||
	    !g_ascii_string_to_signed(text, 10, 0, G_MAXINT32, &value, NULL))
		return -1;

	return value;
}

/* A request whose payload carries one integer, as the daemon reads it. */
static JsonNode *
int_payload(
	const gchar *room,
	const gchar *key,
	gint64       value
){
	g_autoptr(JsonBuilder) builder = json_builder_new();

	json_builder_begin_object(builder);
	if (room != NULL) {
		json_builder_set_member_name(builder, "room");
		json_builder_add_string_value(builder, room);
	}
	json_builder_set_member_name(builder, key);
	json_builder_add_int_value(builder, value);
	json_builder_end_object(builder);

	return json_builder_get_root(builder);
}

static void
on_tree_budget_apply(
	AdwEntryRow *row,
	gpointer     user_data
){
	ClawtWindow *self = user_data;
	gint64 value = entry_number(GTK_WIDGET(row));
	g_autoptr(JsonNode) reply = NULL;
	g_autofree gchar *said = NULL;

	if (value < 0 || value > 10000) {
		clawt_window_toast(self, "The tree budget is a whole number from 0 "
		                         "(no budget) to 10000.");
		return;
	}

	reply = clawt_window_request(self, "orchestration.set",
		int_payload(NULL, "tree_message_budget", value));

	if (reply == NULL)
		return;

	said = (value == 0)
		? g_strdup("Task trees have no message budget now.")
		: g_strdup_printf("Each task tree may send %" G_GINT64_FORMAT
		                  " requests between settlements.", value);
	clawt_window_toast(self, said);
}

static void
on_max_chain_apply(
	AdwEntryRow *row,
	gpointer     user_data
){
	ClawtWindow *self = user_data;
	const gchar *room = g_object_get_data(G_OBJECT(row), ROUND_ROOM_KEY);
	gint64 value = entry_number(GTK_WIDGET(row));
	g_autoptr(JsonNode) reply = NULL;
	g_autofree gchar *said = NULL;

	if (value < 0 || value > 100) {
		clawt_window_toast(self, "Replies per thread is a whole number from "
		                         "0 (no cap) to 100.");
		return;
	}

	reply = clawt_window_request(self, "room.set",
		int_payload(room, "max_chain", value));

	if (reply == NULL)
		return;

	said = (value == 0)
		? g_strdup_printf("Threads in %s are no longer capped; each member "
		                  "still answers once per thread.", room)
		: g_strdup_printf("A thread in %s now stops after %" G_GINT64_FORMAT
		                  " repl%s.", room, value, value == 1 ? "y" : "ies");
	clawt_window_toast(self, said);
}

/*
 * One row per group room, rebuilt from the reply: rooms come and go
 * between visits, and a row for a room that no longer exists would save
 * into nothing.
 */
static void
rebuild_round_rows(
	ClawtWindow *self,
	JsonObject  *root
){
	JsonArray *rooms = (root != NULL && json_object_has_member(root, "rooms"))
		? json_object_get_array_member(root, "rooms") : NULL;
	guint i;

	if (self->settings_rounds_rows == NULL)
		self->settings_rounds_rows = g_ptr_array_new();

	for (i = 0; i < self->settings_rounds_rows->len; i++)
		adw_preferences_group_remove(
			ADW_PREFERENCES_GROUP(self->settings_rounds_group),
			g_ptr_array_index(self->settings_rounds_rows, i));
	g_ptr_array_set_size(self->settings_rounds_rows, 0);

	if (rooms == NULL || json_array_get_length(rooms) == 0) {
		GtkWidget *empty = adw_action_row_new();

		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(empty),
			"No group rooms yet");
		adw_action_row_set_subtitle(ADW_ACTION_ROW(empty),
			"A room with three or more members appears here.");
		adw_preferences_group_add(
			ADW_PREFERENCES_GROUP(self->settings_rounds_group), empty);
		g_ptr_array_add(self->settings_rounds_rows, empty);
		return;
	}

	for (i = 0; i < json_array_get_length(rooms); i++) {
		JsonObject *room = json_array_get_object_element(rooms, i);
		const gchar *id = clawt_json_string(room, "id", "");
		const gchar *name = clawt_json_string(room, "name", id);
		gint64 round = clawt_json_int(room, "round", 0);
		GtkWidget *entry = adw_entry_row_new();
		g_autofree gchar *title = NULL;
		g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT,
			clawt_json_int(room, "max_chain", 0));

		title = (round > 0)
			? g_strdup_printf("%s \xe2\x80\x94 replies per thread (round %"
			                  G_GINT64_FORMAT " now)", name, round)
			: g_strdup_printf("%s \xe2\x80\x94 replies per thread", name);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(entry), title);
		gtk_editable_set_text(GTK_EDITABLE(entry), value);
		adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(entry), TRUE);
		adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(entry),
		                                GTK_INPUT_PURPOSE_DIGITS);
		g_object_set_data_full(G_OBJECT(entry), ROUND_ROOM_KEY,
		                       g_strdup(id), g_free);
		g_signal_connect(entry, "apply", G_CALLBACK(on_max_chain_apply),
		                 self);
		adw_preferences_group_add(
			ADW_PREFERENCES_GROUP(self->settings_rounds_group), entry);
		g_ptr_array_add(self->settings_rounds_rows, entry);
	}
}

void
clawt_gtk_refresh_settings_orchestration(
	ClawtWindow *self
){
	if (self->settings_ack_model == NULL)
		return;

	if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_ORCHESTRATION))
		return;

	do {
		g_autoptr(JsonNode) reply = NULL;
		g_autoptr(GtkStringList) labels = gtk_string_list_new(NULL);
		GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
		const gchar *current = "";
		JsonObject *root;
		guint selected = 0;
		guint i;

		reply = clawt_window_request(self, "orchestration.get", NULL);
		root = reply != NULL ? clawt_payload_of(reply) : NULL;

		/* The settings dialog may have closed while the request waited. */
		if (self->settings_ack_model == NULL) {
			g_ptr_array_unref(values);
			continue;
		}

		if (root != NULL)
			current = clawt_json_string(root, "ack_check_model", "");

		for (i = 0; i < clawt_ack_check_model_count(); i++) {
			const gchar *value = clawt_ack_check_model_nth_value(i);

			gtk_string_list_append(labels, clawt_ack_check_model_nth_label(i));
			g_ptr_array_add(values, g_strdup(value));

			if (g_strcmp0(value, current) == 0)
				selected = i;
		}

		/*
		 * A model somebody wrote into the file by hand is kept as a
		 * choice rather than shown as Off, which would be a lie about
		 * what is configured -- and saving the page would make it true.
		 */
		if (current != NULL && *current != '\0' && selected == 0) {
			g_autofree gchar *label = clawt_ack_check_model_label(current);

			gtk_string_list_append(labels, label);
			g_ptr_array_add(values, g_strdup(current));
			selected = values->len - 1;
		}

		/* The tree budget and the per-room thread caps. */
		if (self->settings_tree_budget != NULL && root != NULL) {
			g_autofree gchar *budget = g_strdup_printf("%" G_GINT64_FORMAT,
				clawt_json_int(root, "tree_message_budget", 0));

			gtk_editable_set_text(GTK_EDITABLE(self->settings_tree_budget),
			                      budget);
		}

		if (self->settings_rounds_group != NULL)
			rebuild_round_rows(self, root);

		self->settings_ack_loading = TRUE;
		g_object_set_data_full(G_OBJECT(self->settings_ack_model),
				       ACK_VALUES_KEY, values,
				       (GDestroyNotify)g_ptr_array_unref);
		adw_combo_row_set_model(ADW_COMBO_ROW(self->settings_ack_model),
					G_LIST_MODEL(labels));
		adw_combo_row_set_selected(ADW_COMBO_ROW(self->settings_ack_model),
					   selected);
		self->settings_ack_loading = FALSE;
	} while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_ORCHESTRATION));
}

GtkWidget *
clawt_gtk_build_orchestration_page(
	ClawtWindow *self
){
	GtkWidget *page = adw_preferences_page_new();
	GtkWidget *group = adw_preferences_group_new();

	adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Orchestration");
	adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
					   "mail-send-receive-symbolic");

	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
					"Acknowledgement check");
	adw_preferences_group_set_description(
		ADW_PREFERENCES_GROUP(group),
		"When one agent answers another with only an acknowledgement -- "
		"\"thanks, noted\" -- waking the agent that asked costs it a whole "
		"turn. With a model chosen here, a short answer from a turn that "
		"did nothing else is put to that model first: only a plain ACK "
		"skips the wake, and anything else, a failure, or no verdict "
		"within 8 seconds wakes the sender as before. A skipped answer "
		"stays in the room marked \"acknowledged (no wake)\", and the "
		"sender is told about it on its next turn. Off by default.");

	self->settings_ack_model = adw_combo_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->settings_ack_model),
				      "Model");
	adw_action_row_set_subtitle(ADW_ACTION_ROW(self->settings_ack_model),
		"A small, fast model reached over HTTP; its key is configured "
		"as for AI assist");
	g_signal_connect(self->settings_ack_model, "notify::selected",
			 G_CALLBACK(on_ack_model_changed), self);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
				  self->settings_ack_model);

	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
				 ADW_PREFERENCES_GROUP(group));

	/*
	 * The tree budget.  Said in what it does to a fleet rather than in
	 * the key's own words, with the two exemptions that make it safe:
	 * answers, and the follow-up a delegator is owed.
	 */
	{
		GtkWidget *tree = adw_preferences_group_new();

		adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(tree),
						"Message budget per task tree");
		adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(tree),
			"A task tree is a delegated task and everything handed on "
			"from it. The hop limit stops a chain going too deep; this "
			"stops one going too wide. It counts the messages agents in "
			"one tree send each other -- one per call, however many "
			"teammates it reaches -- and starts again whenever any task "
			"in the tree finishes. At the limit those agents can still "
			"answer and finish their own work, but are not offered the "
			"tools to message each other, and you get one alert naming "
			"the tree. Answers are never counted. 0 turns it off.");

		self->settings_tree_budget = adw_entry_row_new();
		adw_preferences_row_set_title(
			ADW_PREFERENCES_ROW(self->settings_tree_budget),
			"Requests between settlements");
		adw_entry_row_set_show_apply_button(
			ADW_ENTRY_ROW(self->settings_tree_budget), TRUE);
		adw_entry_row_set_input_purpose(
			ADW_ENTRY_ROW(self->settings_tree_budget),
			GTK_INPUT_PURPOSE_DIGITS);
		g_signal_connect(self->settings_tree_budget, "apply",
				 G_CALLBACK(on_tree_budget_apply), self);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(tree),
					  self->settings_tree_budget);
		adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
					 ADW_PREFERENCES_GROUP(tree));
	}

	/* Rounds, per group room: rows filled in by the refresh. */
	{
		GtkWidget *rounds = adw_preferences_group_new();

		adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(rounds),
						"Rounds in group rooms");
		adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(rounds),
			"A round starts at each message from you, a routine or a "
			"trigger. Every member that answers starts a thread, and an "
			"@mention in a reply carries it on. Within one thread a member "
			"answers once -- naming somebody who already has is kept in "
			"the room, marked as not delivered, with the reason -- and a "
			"thread stops after this many replies. The hop count starts "
			"again each round, so a standup no longer reaches the hop "
			"limit by itself. 0 removes the cap.");

		self->settings_rounds_group = rounds;
		adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
					 ADW_PREFERENCES_GROUP(rounds));
	}

	return page;
}
