/*
 * gtk-review.c - The inspector's Memory review: what an agent changed in
 * its memories, with an Undo per change
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent's memories are read back later as its own conclusions, and
 * clawtilla_memory_add, _forget and _pin act at once.  This group lists
 * the journal the store keeps of every one of those, newest first, and
 * offers an Undo on each row the store says it would accept -- with the
 * reason, from the daemon, on each row it would not.
 *
 * The words for an action and for its reversal come from libclawt
 * (clawt_memory_journal_action_label(), _undo_label()), so the web
 * client and clawtilla-lrg say the same thing about the same row.
 *
 * Refilled on its own by `memory.changed`, like the "Waiting for" list:
 * an event never rebuilds the form somebody may be editing above it.
 */

#include "clawt-window-private.h"

static void refresh_memory_proposals(ClawtWindow *self);

static void
on_memory_undo(
	GtkButton *button,
	gpointer   user_data
){
	ClawtWindow *self = user_data;
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(JsonNode) reply = NULL;
	const gchar *scope = g_object_get_data(G_OBJECT(button), "clawt-scope");
	gint64 id = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(button),
		"clawt-entry"));

	if (self->selected_agent == NULL)
		return;

	json_builder_begin_object(builder);
	json_builder_set_member_name(builder, "agent");
	json_builder_add_string_value(builder, self->selected_agent);
	json_builder_set_member_name(builder, "scope");
	json_builder_add_string_value(builder, scope != NULL ? scope : "agent");
	json_builder_set_member_name(builder, "id");
	json_builder_add_int_value(builder, id);
	json_builder_end_object(builder);

	reply = clawt_window_request(self, "memory.undo",
		json_builder_get_root(builder));

	if (reply != NULL)
		clawt_window_toast(self, "Undone. The agent reads its memories "
			"as they are now from its next search.");

	clawt_gtk_refresh_memory_review(self);
}

void
clawt_gtk_refresh_memory_review(
	ClawtWindow *self
){
	g_autoptr(JsonNode) reply = NULL;
	JsonObject *root;
	JsonArray *entries;
	gint64 now = g_get_real_time();
	guint i;

	refresh_memory_proposals(self);

	if (self->inspector_memory_review == NULL || self->selected_agent == NULL)
		return;

	reply = clawt_window_request(self, "memory.journal",
		clawt_build_payload("agent", self->selected_agent, NULL));

	/* The request iterates the loop; the inspector may have gone. */
	if (self->inspector_memory_review == NULL)
		return;

	gtk_list_box_remove_all(self->inspector_memory_review);

	root = clawt_ipc_reply_object(reply);
	entries = (root != NULL && json_object_has_member(root, "entries"))
		? json_object_get_array_member(root, "entries") : NULL;

	if (entries == NULL || json_array_get_length(entries) == 0) {
		GtkWidget *row = adw_action_row_new();

		clawt_gtk_set_row_text(row, "No changes yet",
			reply == NULL
				? "This agent has no memory store, or the daemon could "
				  "not read its journal."
				: "Each time this agent remembers, forgets or pins "
				  "something it shows here, with a way to take it back.");
		gtk_list_box_append(self->inspector_memory_review, row);
		return;
	}

	for (i = 0; i < json_array_get_length(entries); i++) {
		JsonObject *entry = json_array_get_object_element(entries, i);
		const gchar *action = clawt_json_string(entry, "action", "");
		const gchar *scope = clawt_json_string(entry, "scope", "agent");
		const gchar *why = clawt_json_string(entry, "why_not", NULL);
		g_autofree gchar *ago = clawt_time_ago_label(
			clawt_json_int(entry, "at", 0) * G_USEC_PER_SEC, now);
		g_autofree gchar *title = NULL;
		g_autofree gchar *subtitle = NULL;
		GtkWidget *row = adw_action_row_new();

		title = g_strdup_printf("%s: %s",
			clawt_memory_journal_action_label(action),
			clawt_json_string(entry, "excerpt",
				clawt_json_string(entry, "memory_id", "?")));

		/*
		 * The line says who and when; a shared database says so, since
		 * "the team believes this" and "this agent decided it" are
		 * different claims about one row.
		 */
		subtitle = g_strdup_printf("%s \302\267 %s%s%s%s",
			clawt_json_string(entry, "line", ""), ago,
			g_strcmp0(scope, "agent") != 0 ? " \302\267 in the " : "",
			g_strcmp0(scope, "agent") != 0 ? scope : "",
			g_strcmp0(scope, "agent") != 0 ? "'s memories" : "");

		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

		if (clawt_json_boolean(entry, "undoable", FALSE)) {
			GtkWidget *undo = gtk_button_new_with_label(
				clawt_memory_journal_undo_label(action));

			gtk_widget_set_valign(undo, GTK_ALIGN_CENTER);
			gtk_widget_add_css_class(undo, "flat");
			gtk_widget_set_tooltip_text(undo, "Undo this change");
			g_object_set_data_full(G_OBJECT(undo), "clawt-scope",
				g_strdup(scope), g_free);
			g_object_set_data(G_OBJECT(undo), "clawt-entry",
				GSIZE_TO_POINTER((gsize)clawt_json_int(entry, "id", 0)));
			g_signal_connect(undo, "clicked", G_CALLBACK(on_memory_undo),
				self);
			adw_action_row_add_suffix(ADW_ACTION_ROW(row), undo);
		} else if (why != NULL && g_strcmp0(action, "undo") != 0 &&
			clawt_json_int(entry, "undone_at", 0) == 0) {
			/* Why there is no button, where the button would be. */
			GtkWidget *note = gtk_image_new_from_icon_name(
				"dialog-information-symbolic");

			gtk_widget_set_tooltip_text(note, why);
			gtk_widget_add_css_class(note, "dim-label");
			adw_action_row_add_suffix(ADW_ACTION_ROW(row), note);
		}

		gtk_list_box_append(self->inspector_memory_review, row);
	}
}

void
clawt_gtk_build_memory_review(
	ClawtWindow *self
){
	GtkWidget *group = adw_preferences_group_new();
	GtkWidget *list = gtk_list_box_new();

	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
		"Memory review");
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
		"Everything this agent remembered, forgot or pinned, newest first. "
		"Undo takes one change back: an added memory is archived, a "
		"forgotten one returns, a pin flips back. Nothing is deleted.");

	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), list);

	if (self->inspector_memory_review != NULL)
		g_object_remove_weak_pointer(G_OBJECT(self->inspector_memory_review),
			(gpointer *)&self->inspector_memory_review);

	self->inspector_memory_review = GTK_LIST_BOX(list);
	g_object_add_weak_pointer(G_OBJECT(list),
		(gpointer *)&self->inspector_memory_review);

	gtk_box_append(self->inspector, group);
	clawt_gtk_refresh_memory_review(self);
}

/* ── The review's proposals ──────────────────────────────────────── */

/*
 * memories.review.model: a small model periodically suggests merges,
 * forgets and pins over this agent's memories.  Nothing it suggests is
 * applied until somebody presses Accept here, and then it goes through
 * the journaled store, so it lands in the list above with an Undo.
 */

#define REVIEW_VALUES_KEY "clawt-review-values"

static void
on_proposal_answer(
	GtkButton *button,
	gpointer   user_data
){
	ClawtWindow *self = user_data;
	g_autoptr(JsonNode) reply = NULL;
	const gchar *id = g_object_get_data(G_OBJECT(button), "clawt-proposal");
	gboolean accept = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button),
		"clawt-accept"));

	if (self->selected_agent == NULL || id == NULL)
		return;

	reply = clawt_window_request(self, accept ? "memory.proposal_accept"
		: "memory.proposal_reject",
		clawt_build_payload("agent", self->selected_agent, "id", id, NULL));

	if (reply != NULL)
		clawt_window_toast(self, accept
			? "Accepted. It is in the Memory review with an Undo."
			: "Rejected. Nothing was changed.");

	clawt_gtk_refresh_memory_review(self);
}

static void
on_review_now(
	GtkButton *button,
	gpointer   user_data
){
	ClawtWindow *self = user_data;
	g_autoptr(JsonNode) reply = NULL;

	(void)button;

	if (self->selected_agent == NULL)
		return;

	reply = clawt_window_request(self, "memory.review",
		clawt_build_payload("agent", self->selected_agent, NULL));

	if (reply != NULL)
		clawt_window_toast(self, "Reviewing. Its suggestions appear here "
			"when the model answers.");
}

static void
on_review_model_chosen(
	AdwComboRow *row,
	GParamSpec  *pspec,
	gpointer     user_data
){
	ClawtWindow *self = user_data;
	GPtrArray *values = g_object_get_data(G_OBJECT(row), REVIEW_VALUES_KEY);
	guint selected = adw_combo_row_get_selected(row);
	g_autoptr(JsonNode) reply = NULL;

	(void)pspec;

	if (self->inspector_review_loading || values == NULL ||
		selected >= values->len)
		return;

	reply = clawt_window_request(self, "memory.review_set",
		clawt_build_payload("model", g_ptr_array_index(values, selected),
			NULL));

	if (reply != NULL)
		clawt_window_toast(self, "Saved for the whole fleet.");
}

/* The model choice, from libclawt's list, keeping a hand-written value. */
static GtkWidget *
review_model_row(
	ClawtWindow *self,
	JsonObject  *root
){
	GtkWidget *row = clawt_gtk_combo_row_new();
	g_autoptr(GtkStringList) labels = gtk_string_list_new(NULL);
	GPtrArray *values = g_ptr_array_new_with_free_func(g_free);
	const gchar *current = clawt_json_string(root, "model", "");
	guint selected = 0;
	guint i;

	for (i = 0; i < clawt_ack_check_model_count(); i++) {
		const gchar *value = clawt_ack_check_model_nth_value(i);

		gtk_string_list_append(labels, clawt_ack_check_model_nth_label(i));
		g_ptr_array_add(values, g_strdup(value));

		if (g_strcmp0(value, current) == 0)
			selected = i;
	}

	if (current != NULL && *current != '\0' && selected == 0) {
		gtk_string_list_append(labels, clawt_json_string(root, "model_label",
			current));
		g_ptr_array_add(values, g_strdup(current));
		selected = values->len - 1;
	}

	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Reviewed by");
	clawt_gtk_set_row_hint(row, "A small model, for the whole fleet. Off "
		"spends nothing; on, each agent's memories are read about once an "
		"interval and nothing is changed until you accept a suggestion.");

	self->inspector_review_loading = TRUE;
	g_object_set_data_full(G_OBJECT(row), REVIEW_VALUES_KEY, values,
		(GDestroyNotify)g_ptr_array_unref);
	adw_combo_row_set_model(ADW_COMBO_ROW(row), G_LIST_MODEL(labels));
	adw_combo_row_set_selected(ADW_COMBO_ROW(row), selected);
	self->inspector_review_loading = FALSE;

	g_signal_connect(row, "notify::selected",
		G_CALLBACK(on_review_model_chosen), self);

	return row;
}

static void
refresh_memory_proposals(
	ClawtWindow *self
){
	g_autoptr(JsonNode) reply = NULL;
	JsonObject *root;
	JsonArray *proposals;
	gint64 now = g_get_real_time();
	guint i;

	if (self->inspector_memory_proposals == NULL ||
		self->selected_agent == NULL)
		return;

	reply = clawt_window_request(self, "memory.proposals",
		clawt_build_payload("agent", self->selected_agent, NULL));

	/* The request iterates the loop; the inspector may have gone. */
	if (self->inspector_memory_proposals == NULL)
		return;

	gtk_list_box_remove_all(self->inspector_memory_proposals);

	root = clawt_ipc_reply_object(reply);
	if (root == NULL) {
		GtkWidget *row = adw_action_row_new();

		clawt_gtk_set_row_text(row, "No review",
			"This agent has no memory store, or the daemon could not say.");
		gtk_list_box_append(self->inspector_memory_proposals, row);
		return;
	}

	gtk_list_box_append(self->inspector_memory_proposals,
		review_model_row(self, root));

	{
		GtkWidget *row = adw_action_row_new();
		GtkWidget *now_button = gtk_button_new_with_label("Review now");
		const gchar *error = clawt_json_string(root, "last_error", NULL);
		gint64 last = clawt_json_int(root, "last_run_at", 0);
		g_autofree gchar *ago = (last > 0)
			? clawt_time_ago_label(last * G_USEC_PER_SEC, now) : NULL;
		g_autofree gchar *subtitle = NULL;

		subtitle = clawt_json_boolean(root, "running", FALSE)
			? g_strdup("A pass is running now.")
			: (error != NULL)
				? g_strdup_printf("The last pass proposed nothing: %s", error)
				: (ago != NULL)
					? g_strdup_printf("Last reviewed %s.", ago)
					: g_strdup("Not reviewed yet.");

		clawt_gtk_set_row_text(row, "Review", subtitle);
		gtk_widget_set_valign(now_button, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(now_button, "flat");
		gtk_widget_set_sensitive(now_button,
			clawt_json_boolean(root, "enabled", FALSE) &&
			!clawt_json_boolean(root, "running", FALSE));
		g_signal_connect(now_button, "clicked", G_CALLBACK(on_review_now),
			self);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), now_button);
		gtk_list_box_append(self->inspector_memory_proposals, row);
	}

	proposals = json_object_has_member(root, "proposals")
		? json_object_get_array_member(root, "proposals") : NULL;

	for (i = 0; proposals != NULL && i < json_array_get_length(proposals);
		i++) {
		JsonObject *proposal = json_array_get_object_element(proposals, i);
		JsonArray *about = json_object_has_member(proposal, "memories")
			? json_object_get_array_member(proposal, "memories") : NULL;
		g_autoptr(GString) subtitle = g_string_new(NULL);
		const gchar *reason = clawt_json_string(proposal, "reason", NULL);
		const gchar *content = clawt_json_string(proposal, "content", NULL);
		const gchar *id = clawt_json_string(proposal, "id", NULL);
		GtkWidget *row = adw_action_row_new();
		GtkWidget *accept = gtk_button_new_with_label("Accept");
		GtkWidget *reject = gtk_button_new_with_label("Reject");
		guint j;

		/*
		 * The memories it is about, as they are now, and the model's
		 * words marked as its own -- a person deciding about "forget"
		 * has to read what would be forgotten.
		 */
		if (reason != NULL)
			g_string_append_printf(subtitle, "Why: %s", reason);

		if (content != NULL)
			g_string_append_printf(subtitle, "%sInto: %s",
				subtitle->len > 0 ? "\n" : "", content);

		for (j = 0; about != NULL && j < json_array_get_length(about); j++) {
			JsonObject *memory = json_array_get_object_element(about, j);

			g_string_append_printf(subtitle, "%s%s: %s",
				subtitle->len > 0 ? "\n" : "",
				clawt_json_string(memory, "id", "?"),
				clawt_json_string(memory, "excerpt", "(gone since)"));
		}

		adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
		adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
			clawt_json_string(proposal, "line", "A change"));
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle->str);

		gtk_widget_set_valign(accept, GTK_ALIGN_CENTER);
		gtk_widget_set_valign(reject, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(accept, "suggested-action");
		gtk_widget_add_css_class(reject, "flat");
		g_object_set_data_full(G_OBJECT(accept), "clawt-proposal",
			g_strdup(id), g_free);
		g_object_set_data_full(G_OBJECT(reject), "clawt-proposal",
			g_strdup(id), g_free);
		g_object_set_data(G_OBJECT(accept), "clawt-accept",
			GINT_TO_POINTER(TRUE));
		g_signal_connect(accept, "clicked", G_CALLBACK(on_proposal_answer),
			self);
		g_signal_connect(reject, "clicked", G_CALLBACK(on_proposal_answer),
			self);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), accept);
		adw_action_row_add_suffix(ADW_ACTION_ROW(row), reject);

		gtk_list_box_append(self->inspector_memory_proposals, row);
	}
}

void
clawt_gtk_build_memory_proposals(
	ClawtWindow *self
){
	GtkWidget *group = adw_preferences_group_new();
	GtkWidget *list = gtk_list_box_new();

	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
		"Suggested by the review");
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
		"Merges, forgets and pins a small model suggests for this agent's "
		"own memories. Nothing changes until you accept one; an accepted "
		"one appears in the Memory review above with an Undo.");

	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list, "boxed-list");
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), list);

	if (self->inspector_memory_proposals != NULL)
		g_object_remove_weak_pointer(
			G_OBJECT(self->inspector_memory_proposals),
			(gpointer *)&self->inspector_memory_proposals);

	self->inspector_memory_proposals = GTK_LIST_BOX(list);
	g_object_add_weak_pointer(G_OBJECT(list),
		(gpointer *)&self->inspector_memory_proposals);

	gtk_box_append(self->inspector, group);
	refresh_memory_proposals(self);
}
