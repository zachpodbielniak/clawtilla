/*
 * lrg-settings-orchestration.c - Settings: orchestration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The acknowledgement check (orchestration.ack_check_model), chosen
 * from libclawt's walk of the model catalogue so this page offers the
 * GTK and web pages' list; the message budget per task tree
 * (orchestration.tree_message_budget); and how long one thread of a
 * round may run in each group room (rooms.max_chain).  The combo and
 * the room rows are rebuilt on every read, with
 * the daemon's value selected: a combo built once at page creation
 * would show Off until the reply landed, and a person changing it in
 * that window would be answering the wrong question.
 */

#include "clawt-lrg.h"

typedef struct {
	LrgWidget   *slot;        /* holds the combo, rebuilt per read */
	LrgWidget   *budget;      /* the tree budget's entry */
	LrgWidget   *rounds;      /* one row per group room, rebuilt per read */
	guint        generation;
} Orchestration;

/* What a Save button in the rounds card is about. */
#define ROUND_ROOM_KEY  "clawt-round-room"
#define ROUND_ENTRY_KEY "clawt-round-entry"

static Orchestration orchestration;

static void refresh(ClawtLrgApp *app);

static void
on_saved(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	const gchar *value = user_data;

	(void)reply;

	if (error == NULL)
		clawt_lrg_toast(app, (value == NULL || *value == '\0')
			? "Acknowledgement check off."
			: "Acknowledgement check saved.");

	/* Read back, so a refusal leaves the combo showing what is set. */
	refresh(app);
}

static void
on_model_changed(
	LrgComboBox *combo,
	gpointer     user_data
){
	ClawtLrgApp *app = user_data;
	const gchar *value = clawt_lrg_combo_id(LRG_WIDGET(combo));

	if (value == NULL)
		return;

	clawt_lrg_request(app, "orchestration.set",
			  clawt_ipc_payload_new("ack_check_model", value, NULL),
			  CLAWT_LRG_REQUEST_NONE, on_saved, g_strdup(value),
			  g_free);
}

/*
 * A whole number from an entry, or -1 when it is not one: refused here,
 * naming the field, rather than sent for the daemon to refuse.
 */
static gint64
entry_number(
	LrgWidget *entry
){
	const gchar *text = clawt_lrg_entry_text(entry);
	g_autofree gchar *trimmed = NULL;
	gint64 value = 0;

	if (text == NULL)
		return -1;

	trimmed = g_strstrip(g_strdup(text));

	if (*trimmed == '\0' ||
	    !g_ascii_string_to_signed(trimmed, 10, 0, G_MAXINT32, &value, NULL))
		return -1;

	return value;
}

/* A payload carrying one integer, as the daemon reads it. */
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

/* Toasts what was saved -- @user_data is the sentence -- and reads back. */
static void
on_number_saved(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	(void)reply;

	if (error == NULL)
		clawt_lrg_toast(app, (const gchar *)user_data);

	refresh(app);
}

static void
on_budget_save(
	LrgButton *button,
	gpointer   user_data
){
	ClawtLrgApp *app = user_data;
	gint64 value = entry_number(orchestration.budget);
	gchar *said;

	(void)button;

	if (value < 0 || value > 10000) {
		clawt_lrg_toast(app, "The tree budget is a whole number from 0 "
		                     "(no budget) to 10000.");
		return;
	}

	said = (value == 0)
		? g_strdup("Task trees have no message budget now.")
		: g_strdup_printf("Each task tree may send %" G_GINT64_FORMAT
		                  " requests between settlements.", value);

	clawt_lrg_request(app, "orchestration.set",
			  int_payload(NULL, "tree_message_budget", value),
			  CLAWT_LRG_REQUEST_NONE, on_number_saved, said, g_free);
}

static void
on_chain_save(
	LrgButton *button,
	gpointer   user_data
){
	ClawtLrgApp *app = user_data;
	const gchar *room = g_object_get_data(G_OBJECT(button), ROUND_ROOM_KEY);
	LrgWidget *entry = g_object_get_data(G_OBJECT(button), ROUND_ENTRY_KEY);
	gint64 value = (entry != NULL) ? entry_number(entry) : -1;
	gchar *said;

	if (room == NULL || value < 0 || value > 100) {
		clawt_lrg_toast(app, "Replies per thread is a whole number from 0 "
		                     "(no cap) to 100.");
		return;
	}

	said = (value == 0)
		? g_strdup_printf("Threads in %s are no longer capped; each member "
		                  "still answers once per thread.", room)
		: g_strdup_printf("A thread in %s now stops after %" G_GINT64_FORMAT
		                  " repl%s.", room, value, value == 1 ? "y" : "ies");

	clawt_lrg_request(app, "room.set", int_payload(room, "max_chain", value),
			  CLAWT_LRG_REQUEST_NONE, on_number_saved, said, g_free);
}

/*
 * One row per group room, rebuilt from the reply: rooms come and go
 * between visits, and a row for a room that no longer exists would save
 * into nothing.
 */
static void
rebuild_rounds(
	ClawtLrgApp *app,
	JsonObject  *root
){
	JsonArray *rooms = (root != NULL && json_object_has_member(root, "rooms"))
		? json_object_get_array_member(root, "rooms") : NULL;
	guint i;

	clawt_lrg_clear(orchestration.rounds);

	if (rooms == NULL || json_array_get_length(rooms) == 0) {
		clawt_lrg_take(orchestration.rounds, clawt_lrg_paragraph(
			"No group rooms yet. A room with three or more members "
			"appears here.", "muted"));
		return;
	}

	for (i = 0; i < json_array_get_length(rooms); i++) {
		JsonObject *room = json_array_get_object_element(rooms, i);
		const gchar *id = clawt_ipc_payload_string(room, "id");
		const gchar *name = clawt_ipc_payload_string(room, "name");
		gint64 round = clawt_ipc_payload_int(room, "round", 0);
		g_autofree gchar *chain = g_strdup_printf("%" G_GINT64_FORMAT,
			clawt_ipc_payload_int(room, "max_chain", 0));
		g_autofree gchar *title = NULL;
		LrgWidget *line;
		LrgWidget *entry;
		LrgWidget *save;

		if (id == NULL)
			continue;

		line = clawt_lrg_hbox(8.0f);
		entry = clawt_lrg_entry(chain, "3");
		save = clawt_lrg_button("Save", "primary",
		                        G_CALLBACK(on_chain_save), app);

		title = (round > 0)
			? g_strdup_printf("%s \xe2\x80\x94 replies per thread (round %"
			                  G_GINT64_FORMAT " now)",
			                  name != NULL ? name : id, round)
			: g_strdup_printf("%s \xe2\x80\x94 replies per thread",
			                  name != NULL ? name : id);

		g_object_set_data_full(G_OBJECT(save), ROUND_ROOM_KEY,
		                       g_strdup(id), g_free);
		g_object_set_data(G_OBJECT(save), ROUND_ENTRY_KEY, entry);

		clawt_lrg_take(orchestration.rounds, clawt_lrg_label(title, NULL));
		clawt_lrg_take(line, entry);
		clawt_lrg_take(line, save);
		clawt_lrg_take(orchestration.rounds, line);
	}
}

static void
on_read(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	JsonObject *root = clawt_ipc_reply_object(reply);
	const gchar *current = NULL;
	g_autoptr(GPtrArray) ids = g_ptr_array_new();
	g_autoptr(GPtrArray) labels = g_ptr_array_new();
	LrgWidget *combo;
	guint i;

	(void)error;

	if (GPOINTER_TO_UINT(user_data) != orchestration.generation ||
	    orchestration.slot == NULL)
		return;

	if (root != NULL)
		current = clawt_ipc_payload_string(root, "ack_check_model");

	for (i = 0; i < clawt_ack_check_model_count(); i++) {
		g_ptr_array_add(ids, (gpointer)clawt_ack_check_model_nth_value(i));
		g_ptr_array_add(labels, (gpointer)clawt_ack_check_model_nth_label(i));
	}
	g_ptr_array_add(ids, NULL);
	g_ptr_array_add(labels, NULL);

	/*
	 * A model written into the file by hand is kept as a choice by
	 * clawt_lrg_combo(), so it is not shown as Off -- which saving the
	 * page would then make true.
	 */
	combo = clawt_lrg_combo((const gchar *const *)ids->pdata,
				(const gchar *const *)labels->pdata,
				current != NULL ? current : "");

	/* connected after the selection, so building is not a change */
	g_signal_connect(combo, "changed", G_CALLBACK(on_model_changed), app);

	clawt_lrg_clear(orchestration.slot);
	clawt_lrg_take(orchestration.slot, combo);

	/* The tree budget's current value, and the per-room thread caps. */
	if (orchestration.budget != NULL && root != NULL) {
		g_autofree gchar *budget = g_strdup_printf("%" G_GINT64_FORMAT,
			clawt_ipc_payload_int(root, "tree_message_budget", 0));
		lrg_text_input_set_text(LRG_TEXT_INPUT(orchestration.budget),
		                        budget);
	}

	if (orchestration.rounds != NULL)
		rebuild_rounds(app, root);

	clawt_lrg_app_wake(app);
}

static void
refresh(
	ClawtLrgApp *app
){
	if (orchestration.slot == NULL)
		return;

	orchestration.generation++;
	clawt_lrg_request(app, "orchestration.get", NULL, CLAWT_LRG_QUIET,
			  on_read, GUINT_TO_POINTER(orchestration.generation),
			  NULL);
}

static LrgWidget *
build(
	ClawtLrgApp *app
){
	LrgWidget *page = clawt_lrg_page_new("Orchestration",
					     "How agents talk to each other");
	LrgWidget *box = clawt_lrg_vbox(10.0f);
	LrgWidget *form = clawt_lrg_form_new();

	(void)app;

	clawt_lrg_take(box, clawt_lrg_paragraph(
		"When one agent answers another with only an acknowledgement -- "
		"\"thanks, noted\" -- waking the agent that asked costs it a whole "
		"turn. With a model chosen here, a short answer from a turn that "
		"did nothing else is put to that model first. Only a plain ACK "
		"skips the wake; anything else, a failure, or no verdict within 8 "
		"seconds wakes the sender as before. A skipped answer stays in the "
		"room marked \"acknowledged (no wake)\", and the sender is told "
		"about it on its next turn. Off by default.", "muted"));

	orchestration.slot = clawt_lrg_vbox(0.0f);
	clawt_lrg_form_add(form, "Acknowledgement check", orchestration.slot,
			   "A small, fast model reached over HTTP; its key is "
			   "configured as for AI assist.");
	clawt_lrg_take(box, form);

	clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(box));

	/*
	 * The tree budget, said in what it does to a fleet, with the two
	 * exemptions that make it safe: answers, and a delegator's owed
	 * follow-up.
	 */
	{
		LrgWidget *tree = clawt_lrg_vbox(10.0f);
		LrgWidget *tree_form = clawt_lrg_form_new();
		LrgWidget *line = clawt_lrg_hbox(8.0f);

		clawt_lrg_take(tree, clawt_lrg_section("Message budget per task tree"));
		clawt_lrg_take(tree, clawt_lrg_paragraph(
			"A task tree is a delegated task and everything handed on from "
			"it. The hop limit stops a chain going too deep; this stops one "
			"going too wide. It counts the messages agents in one tree send "
			"each other -- one per call, however many teammates it reaches "
			"-- and starts again whenever any task in the tree finishes. At "
			"the limit those agents can still answer and finish their own "
			"work, but are not offered the tools to message each other, and "
			"you get one alert naming the tree. Answers are never counted. "
			"0 turns it off.", "muted"));

		orchestration.budget = clawt_lrg_entry(NULL, "60");
		clawt_lrg_take(line, orchestration.budget);
		clawt_lrg_take(line, clawt_lrg_button("Save", "primary",
		                                      G_CALLBACK(on_budget_save),
		                                      app));
		clawt_lrg_form_add(tree_form, "Requests between settlements", line,
		                   NULL);
		clawt_lrg_take(tree, tree_form);
		clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(tree));
	}

	/* Rounds, per group room: rows filled in by every read. */
	{
		LrgWidget *rounds = clawt_lrg_vbox(10.0f);

		clawt_lrg_take(rounds, clawt_lrg_section("Rounds in group rooms"));
		clawt_lrg_take(rounds, clawt_lrg_paragraph(
			"A round starts at each message from you, a routine or a "
			"trigger. Every member that answers starts a thread, and an "
			"@mention in a reply carries it on. Within one thread a member "
			"answers once -- naming somebody who already has is kept in the "
			"room, marked as not delivered, with the reason -- and a thread "
			"stops after this many replies. The hop count starts again each "
			"round, so a standup no longer reaches the hop limit by itself. "
			"0 removes the cap.", "muted"));

		orchestration.rounds = clawt_lrg_vbox(6.0f);
		clawt_lrg_take(rounds, orchestration.rounds);
		clawt_lrg_take(clawt_lrg_page_body(page), clawt_lrg_card(rounds));
	}

	return page;
}

static const ClawtLrgSettingsPage page_impl = {
    "orchestration", "Orchestration", build, refresh
};

const ClawtLrgSettingsPage *
clawt_lrg_settings_orchestration_page(void)
{
    return &page_impl;
}
