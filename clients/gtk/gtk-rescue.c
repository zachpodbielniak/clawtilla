/*
 * gtk-rescue.c - First-reply rescue in the chat: a turn that has shown
 * nothing, and the two things that help
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A card above the composer while the selected agent has a turn the
 * daemon has offered to rescue (`turn.no_reply`): the sentence from
 * clawt_rescue_offer_text(), Stop, and a "Try <model> instead" for each
 * fallback the daemon says one turn can move to.  Withdrawn on
 * `turn.no_reply_cleared` -- the turn showed something -- and on every
 * agent switch, by asking `turn.rescue` again rather than remembering:
 * the daemon holds the offers, so a window opened late still sees them.
 *
 * A condition the conversation is in until something changes, so a card
 * that stays rather than a toast that goes.
 */

#include "clawt-window-private.h"

static void
on_rescue_action(
	GtkButton *button,
	gpointer   user_data
){
	ClawtWindow *self = user_data;
	const gchar *action = g_object_get_data(G_OBJECT(button), "clawt-action");
	const gchar *room = g_object_get_data(G_OBJECT(button), "clawt-room");
	const gchar *model = g_object_get_data(G_OBJECT(button), "clawt-model");
	g_autoptr(JsonNode) reply = NULL;

	if (self->selected_agent == NULL)
		return;

	reply = clawt_window_request(self, "turn.rescue",
		clawt_build_payload("action", action, "agent", self->selected_agent,
			"room", room, "model", model, NULL));

	if (reply != NULL)
		clawt_window_toast(self, g_strcmp0(action, "switch") == 0
			? "Stopped, and sent the same message again on the other "
			  "model, for this turn only."
			: "Stopped.");

	clawt_gtk_refresh_rescue(self);
}

static GtkWidget *
action_button(
	ClawtWindow *self,
	const gchar *label,
	const gchar *action,
	const gchar *room,
	const gchar *model,
	const gchar *tooltip
){
	GtkWidget *button = gtk_button_new_with_label(label);

	gtk_widget_set_tooltip_text(button, tooltip);
	g_object_set_data_full(G_OBJECT(button), "clawt-action", g_strdup(action),
		g_free);
	g_object_set_data_full(G_OBJECT(button), "clawt-room", g_strdup(room),
		g_free);
	g_object_set_data_full(G_OBJECT(button), "clawt-model", g_strdup(model),
		g_free);
	g_signal_connect(button, "clicked", G_CALLBACK(on_rescue_action), self);

	return button;
}

void
clawt_gtk_refresh_rescue(
	ClawtWindow *self
){
	g_autoptr(JsonNode) reply = NULL;
	JsonObject *payload;
	JsonArray *offers;
	GtkWidget *child;
	guint shown = 0;
	guint i;

	if (self->rescue_box == NULL)
		return;

	while ((child = gtk_widget_get_first_child(self->rescue_box)) != NULL)
		gtk_box_remove(GTK_BOX(self->rescue_box), child);

	if (self->selected_agent == NULL) {
		gtk_widget_set_visible(self->rescue_box, FALSE);
		return;
	}

	reply = clawt_window_request(self, "turn.rescue",
		clawt_build_payload("action", "list", "agent", self->selected_agent,
			NULL));
	payload = (reply != NULL) ? clawt_payload_of(reply) : NULL;
	offers = (payload != NULL && json_object_has_member(payload, "offers"))
		? json_object_get_array_member(payload, "offers") : NULL;

	/* The request iterates the loop; the chat may have gone. */
	if (self->rescue_box == NULL)
		return;

	for (i = 0; offers != NULL && i < json_array_get_length(offers); i++) {
		JsonObject *offer = json_array_get_object_element(offers, i);
		const gchar *room = clawt_json_string(offer, "room", NULL);
		JsonArray *choices = json_object_has_member(offer, "choices")
			? json_object_get_array_member(offer, "choices") : NULL;
		g_autofree gchar *text = clawt_rescue_offer_text(
			self->selected_agent, clawt_json_int(offer, "seconds", 0));
		GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
		GtkWidget *label = gtk_label_new(text);
		GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		guint j;

		gtk_label_set_wrap(GTK_LABEL(label), TRUE);
		gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
		gtk_box_append(GTK_BOX(card), label);

		gtk_box_append(GTK_BOX(buttons), action_button(self, "Stop", "stop",
			room, NULL, "Stop what this agent is doing now -- this turn and "
			"anything else it is running. The agent stays up."));

		for (j = 0; choices != NULL && j < json_array_get_length(choices);
			j++) {
			const gchar *model = json_array_get_string_element(choices, j);
			g_autofree gchar *switch_label = clawt_rescue_switch_label(model);
			g_autofree gchar *tooltip = g_strdup_printf("Stop this turn and "
				"send the same message again on %s, for this turn only. "
				"Anything else the agent is running stops too.", model);

			gtk_box_append(GTK_BOX(buttons), action_button(self, switch_label,
				"switch", room, model, tooltip));
		}

		gtk_box_append(GTK_BOX(card), buttons);
		gtk_widget_add_css_class(card, "card");
		gtk_widget_set_margin_bottom(card, 6);
		gtk_widget_set_margin_top(card, 6);
		gtk_box_append(GTK_BOX(self->rescue_box), card);
		shown++;
	}

	gtk_widget_set_visible(self->rescue_box, shown > 0);
}

GtkWidget *
clawt_gtk_build_rescue(
	ClawtWindow *self
){
	self->rescue_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_name(self->rescue_box, "clawt-rescue");
	gtk_widget_set_visible(self->rescue_box, FALSE);

	return self->rescue_box;
}
