/*
 * gtk-inline-decisions.c - A decision drawn in the room it was asked from
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The inbox is where every open question waits; this is the same
 * question in the conversation that raised it, so somebody reading the
 * chat can answer without leaving it.  A second view of one record, not
 * a copy: pressing an option here sends the decision.answer the inbox
 * sends, and settling it anywhere -- here, the inbox, another window,
 * the CLI, the clock -- redraws both from the same `decision.settled`.
 */

#include "clawt-window-private.h"

/* The record a card was last drawn from, as object data on the card. */
#define CARD_DECISION "clawt-inline-decision"

static void card_fill(ClawtWindow *self, GtkWidget *card);

GPtrArray *
clawt_gtk_inline_decisions_fetch(
	ClawtWindow *self
){
	g_autoptr(JsonNode) reply = NULL;
	GPtrArray *out;
	JsonObject *payload;
	JsonArray *items;
	guint i;

	out = g_ptr_array_new_with_free_func(
		(GDestroyNotify)clawt_decision_free);

	if (self->selected_room == NULL)
		return out;

	reply = clawt_window_request(
		self, "decision.list",
		clawt_build_payload("room", self->selected_room, NULL));

	payload = clawt_payload_of(reply);

	if (payload == NULL || !json_object_has_member(payload, "decisions"))
		return out;

	items = json_object_get_array_member(payload, "decisions");

	for (i = 0; items != NULL && i < json_array_get_length(items); i++) {
		ClawtDecision *decision = clawt_decision_new_from_object(
			json_array_get_object_element(items, i));

		if (decision != NULL)
			g_ptr_array_add(out, decision);
	}

	return out;
}

/* The card currently drawing @id, or NULL. */
static GtkWidget *
card_for(
	ClawtWindow *self,
	const gchar *id
){
	if (self->inline_decisions == NULL || id == NULL)
		return NULL;

	return g_hash_table_lookup(self->inline_decisions, id);
}

/* Replaces the record a card draws, and redraws it. */
static void
card_take(
	ClawtWindow   *self,
	GtkWidget     *card,
	ClawtDecision *decision
){
	g_object_set_data_full(G_OBJECT(card), CARD_DECISION, decision,
	                       (GDestroyNotify)clawt_decision_free);
	card_fill(self, card);
}

/*
 * A press that did not land: refused because somebody else got there
 * first, or because the decision is gone.  The refusal is already on
 * screen (clawt_window_request() toasts it); this makes the card say
 * what is now true instead of leaving buttons the daemon will refuse
 * again.
 */
static void
card_resync(
	ClawtWindow *self,
	const gchar *id
){
	g_autoptr(GPtrArray) now = clawt_gtk_inline_decisions_fetch(self);
	GtkWidget *card;
	guint i;

	card = card_for(self, id);

	if (card == NULL)
		return;

	for (i = 0; i < now->len; i++) {
		ClawtDecision *decision = g_ptr_array_index(now, i);

		if (g_strcmp0(clawt_decision_get_id(decision), id) == 0) {
			card_take(self, card,
			          g_ptr_array_steal_index(now, i));
			return;
		}
	}

	/* Not in the room's list any more: it was deleted. */
	g_hash_table_remove(self->inline_decisions, id);
	gtk_box_remove(self->transcript, card);
}

static void
on_inline_option(
	GtkButton *button,
	gpointer   user_data
){
	ClawtWindow *self = user_data;
	GtkWidget *card;
	GtkWidget *choices;
	g_autoptr(JsonNode) reply = NULL;
	g_autofree gchar *id = NULL;
	g_autofree gchar *option = NULL;
	JsonObject *payload;

	id = g_strdup(g_object_get_data(G_OBJECT(button), "decision-id"));
	option = g_strdup(g_object_get_data(G_OBJECT(button), "option"));
	card = card_for(self, id);

	if (id == NULL || option == NULL || card == NULL)
		return;

	/*
	 * Every option goes insensitive before the request, which is the
	 * client's half of refusing a double tap: the request iterates the
	 * main context while it waits, so a second click would otherwise
	 * start a second answer inside the first.  The daemon refuses that
	 * second one anyway; this keeps it from being sent.
	 */
	choices = g_object_get_data(G_OBJECT(card), "inline-options");

	if (choices != NULL)
		gtk_widget_set_sensitive(choices, FALSE);

	reply = clawt_window_request(
		self, "decision.answer",
		clawt_build_payload("decision", id, "answer", option,
		                    "via", CLAWT_DECISION_VIA_CHAT, NULL));

	/*
	 * The card is looked up again rather than trusted: the wait can
	 * have switched rooms, emptying the transcript and the table.
	 */
	card = card_for(self, id);

	if (card == NULL)
		return;

	payload = clawt_payload_of(reply);

	if (payload != NULL && json_object_has_member(payload, "decision")) {
		ClawtDecision *settled = clawt_decision_new_from_object(
			json_object_get_object_member(payload, "decision"));

		if (settled != NULL) {
			card_take(self, card, settled);
			return;
		}
	}

	card_resync(self, id);
}

/*
 * One option as a button whose label wraps -- an option is a sentence,
 * and a label that does not wrap makes the button as wide as it.  The
 * inbox draws its options the same way, for the same reason.
 */
static GtkWidget *
option_button(
	ClawtWindow *self,
	const gchar *id,
	const gchar *option
){
	GtkWidget *text = gtk_label_new(option);
	GtkWidget *button = gtk_button_new();

	gtk_label_set_wrap(GTK_LABEL(text), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(text), 0.0);
	gtk_button_set_child(GTK_BUTTON(button), text);
	gtk_widget_set_halign(button, GTK_ALIGN_FILL);

	g_object_set_data_full(G_OBJECT(button), "decision-id", g_strdup(id),
	                       g_free);
	g_object_set_data_full(G_OBJECT(button), "option", g_strdup(option),
	                       g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(on_inline_option), self);

	return button;
}

/* A wrapping plain-text label: every string here came from an agent. */
static GtkWidget *
text_label(
	const gchar *text,
	const gchar *css_class
){
	GtkWidget *label = gtk_label_new(text);

	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);

	if (css_class != NULL)
		gtk_widget_add_css_class(label, css_class);

	return label;
}

/*
 * Draws the card from the record it holds: who asked and the question;
 * then, while open, the options as buttons with the default and its
 * deadline under them, and once settled the one sentence saying how it
 * ended and who ended it.  Rebuilt whole rather than edited, because a
 * settled card has none of an open one's controls.
 */
static void
card_fill(
	ClawtWindow *self,
	GtkWidget   *card
){
	ClawtDecision *decision = g_object_get_data(G_OBJECT(card),
	                                            CARD_DECISION);
	const gchar * const *options;
	g_autofree gchar *who = NULL;
	GtkWidget *body;
	gboolean open;
	guint i;

	clawt_gtk_clear_box(GTK_BOX(card));
	g_object_set_data(G_OBJECT(card), "inline-options", NULL);

	if (decision == NULL)
		return;

	/* The inset, inside the frame "card" draws. */
	body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_set_margin_start(body, 12);
	gtk_widget_set_margin_end(body, 12);
	gtk_widget_set_margin_top(body, 10);
	gtk_widget_set_margin_bottom(body, 10);
	gtk_box_append(GTK_BOX(card), body);
	card = body;

	open = clawt_decision_get_state(decision) == CLAWT_DECISION_OPEN;
	who = g_strdup_printf("%s asks", clawt_decision_get_agent(decision));

	gtk_box_append(GTK_BOX(card), text_label(who, "caption-heading"));
	gtk_box_append(GTK_BOX(card),
	               text_label(clawt_decision_get_question(decision), NULL));

	if (open) {
		GtkWidget *choices = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
		g_autofree gchar *fallback =
			clawt_decision_default_text(decision, NULL);

		options = clawt_decision_get_options(decision);

		for (i = 0; options != NULL && options[i] != NULL; i++)
			gtk_box_append(GTK_BOX(choices),
			               option_button(self,
			                             clawt_decision_get_id(decision),
			                             options[i]));

		/*
		 * No options is a free-text question, which the chat cannot
		 * answer with a button: the inbox has the box for it.
		 */
		if (gtk_widget_get_first_child(choices) == NULL)
			gtk_box_append(GTK_BOX(choices), text_label(
				"No options offered: answer it in Decisions.",
				"dim-label"));

		g_object_set_data(G_OBJECT(gtk_widget_get_parent(card)),
		                  "inline-options", choices);
		gtk_box_append(GTK_BOX(card), choices);

		if (fallback != NULL)
			gtk_box_append(GTK_BOX(card),
			               text_label(fallback, "dim-label"));
	} else {
		g_autofree gchar *outcome = clawt_decision_outcome_text(decision);

		gtk_box_append(GTK_BOX(card), text_label(outcome, "dim-label"));
	}
}

void
clawt_gtk_inline_decision_append(
	ClawtWindow   *self,
	ClawtDecision *decision
){
	GtkWidget *card;
	const gchar *id;

	if (self->transcript == NULL || decision == NULL)
		return;

	id = clawt_decision_get_id(decision);

	if (card_for(self, id) != NULL)
		return;

	card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_widget_add_css_class(card, "card");
	gtk_widget_add_css_class(card, "clawt-inline-decision");
	gtk_widget_set_margin_start(card, 12);
	gtk_widget_set_margin_end(card, 12);
	gtk_widget_set_margin_top(card, 6);
	gtk_widget_set_margin_bottom(card, 6);

	card_take(self, card, clawt_decision_copy(decision));

	/*
	 * Before the live step block when a turn is running, so the block
	 * stays the live edge it is drawn as -- a card after it would have
	 * the turn's later steps appear above the question it asked.
	 */
	if (self->steps_block != NULL)
		gtk_box_insert_child_after(
			self->transcript, card,
			gtk_widget_get_prev_sibling(self->steps_block));
	else
		gtk_box_append(self->transcript, card);

	g_hash_table_insert(self->inline_decisions, g_strdup(id), card);
}

void
clawt_gtk_inline_decisions_on_event(
	ClawtWindow *self,
	ClawtEvent  *event
){
	const gchar *kind = clawt_event_get_kind(event);
	const gchar *id = clawt_event_get_detail(event, "decision");
	GtkWidget *card;

	/* Only the room on screen; every other room's view is not drawn. */
	if (self->selected_room == NULL ||
	    g_strcmp0(clawt_decision_event_room(event),
	              self->selected_room) != 0)
		return;

	card = card_for(self, id);

	if (g_strcmp0(kind, "decision.settled") == 0) {
		ClawtDecision *decision = (card != NULL)
			? g_object_get_data(G_OBJECT(card), CARD_DECISION) : NULL;

		if (decision != NULL && clawt_decision_apply_event(decision, event))
			card_fill(self, card);

		return;
	}

	if (g_strcmp0(kind, "decision.deleted") == 0) {
		if (card != NULL) {
			g_hash_table_remove(self->inline_decisions, id);
			gtk_box_remove(self->transcript, card);
		}

		return;
	}

	/*
	 * Asked: the event says where and which, not what, so the room's
	 * list is read and anything not yet drawn is added at the live end.
	 */
	if (g_strcmp0(kind, "decision.asked") == 0 && card == NULL) {
		guint generation = self->history_generation;
		g_autoptr(GPtrArray) asked = clawt_gtk_inline_decisions_fetch(self);
		guint i;

		/*
		 * The fetch iterates the main context, so a room switch can
		 * land inside it; this room's decisions must not be drawn
		 * into the next room's transcript.
		 */
		if (generation != self->history_generation)
			return;

		for (i = 0; i < asked->len; i++)
			clawt_gtk_inline_decision_append(self,
			                                 g_ptr_array_index(asked, i));

		clawt_gtk_queue_scroll(self);
	}
}
