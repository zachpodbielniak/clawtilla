/*
 * lrg-rescue.c - First-reply rescue in the chat: a turn that has shown
 * nothing, and the two things that help
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The libregnum half of gtk-rescue.c and web-rescue.c: a card above the
 * composer while the selected agent has a turn the daemon offered to
 * rescue, with Stop and a "Try <model> instead" per fallback one turn can
 * move to.  The offers are the daemon's (`turn.rescue`), re-read on
 * `turn.no_reply` and `turn.no_reply_cleared` and on every agent switch;
 * the sentence and the labels are libclawt's.  Requests go through
 * clawt_lrg_request(), never from inside a frame, and a reply for an
 * agent somebody has since moved away from is dropped by the guard.
 */

#include "clawt-lrg.h"

static struct {
	LrgWidget *box;   /* the cards; hidden when there are none */
} rescue;

static void
on_rescued(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	const gchar *action = user_data;

	if (reply != NULL)
		clawt_lrg_toast(app, g_strcmp0(action, "switch") == 0
			? "Stopped, and sent the same message again on the other "
			  "model, for this turn only."
			: "Stopped.");

	clawt_lrg_rescue_refresh(app);
}

static void
on_action_clicked(
	LrgButton *button,
	gpointer   user_data
){
	ClawtLrgApp *app = user_data;
	const gchar *action = g_object_get_data(G_OBJECT(button), "clawt-action");
	const gchar *room = g_object_get_data(G_OBJECT(button), "clawt-room");
	const gchar *model = g_object_get_data(G_OBJECT(button), "clawt-model");

	if (app->agent_id == NULL)
		return;

	clawt_lrg_request(app, "turn.rescue",
	                  clawt_ipc_payload_new("action", action,
	                                        "agent", app->agent_id,
	                                        "room", room,
	                                        "model", model, NULL),
	                  CLAWT_LRG_REQUEST_NONE, on_rescued,
	                  (gpointer)(g_strcmp0(action, "switch") == 0
	                             ? "switch" : "stop"), NULL);
}

static LrgWidget *
action_button(
	ClawtLrgApp *app,
	const gchar *label,
	const gchar *style,
	const gchar *action,
	const gchar *room,
	const gchar *model
){
	LrgWidget *button = clawt_lrg_button(label, style,
	                                     G_CALLBACK(on_action_clicked), app);

	g_object_set_data_full(G_OBJECT(button), "clawt-action", g_strdup(action),
	                       g_free);
	g_object_set_data_full(G_OBJECT(button), "clawt-room", g_strdup(room),
	                       g_free);
	g_object_set_data_full(G_OBJECT(button), "clawt-model", g_strdup(model),
	                       g_free);

	return button;
}

static void
on_offers(
	ClawtLrgApp  *app,
	JsonNode     *reply,
	const GError *error,
	gpointer      user_data
){
	JsonObject *root = clawt_ipc_reply_object(reply);
	JsonArray *offers = (root != NULL && json_object_has_member(root, "offers"))
		? json_object_get_array_member(root, "offers") : NULL;
	guint i;

	if (rescue.box == NULL)
		return;

	clawt_lrg_clear(rescue.box);

	for (i = 0; offers != NULL && i < json_array_get_length(offers); i++) {
		JsonObject *offer = json_array_get_object_element(offers, i);
		const gchar *room = clawt_ipc_payload_string(offer, "room");
		JsonArray *choices = json_object_has_member(offer, "choices")
			? json_object_get_array_member(offer, "choices") : NULL;
		g_autofree gchar *text = clawt_rescue_offer_text(app->agent_id,
			clawt_ipc_payload_int(offer, "seconds", 0));
		LrgWidget *content = clawt_lrg_vbox(6.0f);
		LrgWidget *buttons = clawt_lrg_hbox(6.0f);
		guint j;

		clawt_lrg_take(content, clawt_lrg_paragraph(text, NULL));
		clawt_lrg_take(buttons, action_button(app, "Stop", "danger", "stop",
		                                      room, NULL));

		for (j = 0; choices != NULL && j < json_array_get_length(choices);
		     j++) {
			const gchar *model = json_array_get_string_element(choices, j);
			g_autofree gchar *label = clawt_rescue_switch_label(model);

			clawt_lrg_take(buttons, action_button(app, label, "flat",
			                                      "switch", room, model));
		}

		clawt_lrg_take(content, buttons);
		clawt_lrg_take(content, clawt_lrg_paragraph(
			"Either stops everything this agent is running now; trying "
			"another model sends the same message again on it, for this "
			"turn only.", "muted"));
		clawt_lrg_take(rescue.box, clawt_lrg_card(content));
	}

	lrg_widget_set_visible(rescue.box,
	                       offers != NULL && json_array_get_length(offers) > 0);
	clawt_lrg_app_wake(app);
}

void
clawt_lrg_rescue_refresh(
	ClawtLrgApp *app
){
	if (rescue.box == NULL)
		return;

	if (app->agent_id == NULL) {
		clawt_lrg_clear(rescue.box);
		lrg_widget_set_visible(rescue.box, FALSE);
		return;
	}

	clawt_lrg_request(app, "turn.rescue",
	                  clawt_ipc_payload_new("action", "list",
	                                        "agent", app->agent_id, NULL),
	                  CLAWT_LRG_GUARDED | CLAWT_LRG_QUIET, on_offers, NULL,
	                  NULL);
}

LrgWidget *
clawt_lrg_rescue_build(
	ClawtLrgApp *app
){
	(void)app;

	rescue.box = clawt_lrg_vbox(6.0f);
	lrg_widget_set_visible(rescue.box, FALSE);

	return rescue.box;
}
