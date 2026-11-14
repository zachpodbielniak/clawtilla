/*
 * web-rescue.c - First-reply rescue in the chat: a turn that has shown
 * nothing, and the two things that help
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The web half of gtk-rescue.c.  Drawn inside the live-steps region of
 * the operator's conversation with the agent, which the page re-fetches
 * on the `fleet` umbrella event -- so `turn.no_reply` and its
 * `_cleared` both redraw it.  The offers come from `turn.rescue`; the
 * sentence and the button labels from libclawt, so the three clients say
 * the same thing.  Stop and each switch are forms posting to
 * /a/:id/rescue, registered before the /a/:id/:view catch-all.
 */

#include "clawt-web.h"
#include "web-pages.h"

static HtmxForm *
action_form(
	const gchar *action_url,
	const gchar *label,
	const gchar *action,
	const gchar *room,
	const gchar *model,
	const gchar *tone
){
	HtmxForm *form = clawt_web_form(action_url);
	g_autoptr(HtmxButton) button = clawt_web_button(label, tone);

	clawt_web_add(form, clawt_web_hidden("action", action));

	if (room != NULL)
		clawt_web_add(form, clawt_web_hidden("room", room));

	if (model != NULL)
		clawt_web_add(form, clawt_web_hidden("model", model));

	htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "submit");
	htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(button));

	return form;
}

void
clawt_web_add_rescue_card(
	ClawtWebApp *app,
	HtmxElement *parent,
	const gchar *agent_id
){
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
	g_autofree gchar *url = g_strdup_printf("/a/%s/rescue", escaped);
	JsonArray *offers;
	guint i;

	clawt_web_payload_set(payload, "action", "list");
	clawt_web_payload_set(payload, "agent", agent_id);
	reply = clawt_web_app_call(app, "turn.rescue",
		clawt_web_payload_take(g_steal_pointer(&payload)));
	offers = clawt_web_member_array(clawt_web_root(reply), "offers");

	for (i = 0; offers != NULL && i < json_array_get_length(offers); i++) {
		JsonObject *offer = json_array_get_object_element(offers, i);
		const gchar *room = clawt_web_member(offer, "room", NULL);
		JsonArray *choices = clawt_web_member_array(offer, "choices");
		g_autofree gchar *text = clawt_rescue_offer_text(agent_id,
			clawt_web_member_int(offer, "seconds", 0));
		g_autoptr(HtmxDiv) card = htmx_div_new();
		g_autoptr(HtmxDiv) row = htmx_div_new();
		guint j;

		htmx_element_add_class(HTMX_ELEMENT(card), "rescue-card");
		clawt_web_add(card, clawt_web_text(text, "small"));
		htmx_element_add_class(HTMX_ELEMENT(row), "row-actions");

		clawt_web_add(row, action_form(url, "Stop", "stop", room, NULL,
			"danger"));

		for (j = 0; choices != NULL && j < json_array_get_length(choices);
			j++) {
			const gchar *model = json_array_get_string_element(choices, j);
			g_autofree gchar *label = clawt_rescue_switch_label(model);

			clawt_web_add(row, action_form(url, label, "switch", room, model,
				"default"));
		}

		htmx_node_add_child(HTMX_NODE(card), HTMX_NODE(row));
		clawt_web_add(card, clawt_web_text(
			"Either stops everything this agent is running now; trying "
			"another model sends the same message again on it, for this "
			"turn only.", "small muted"));
		htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
	}
}

static HtmxResponse *
on_rescue(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ClawtWebApp *app = user_data;
	g_autofree gchar *agent_id = clawt_web_param(params, "id");
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	const gchar *action = clawt_web_form_value(request, "action");

	clawt_web_payload_set(payload, "action", action);
	clawt_web_payload_set(payload, "agent", agent_id);
	clawt_web_payload_set(payload, "room",
		clawt_web_form_value(request, "room"));
	clawt_web_payload_set(payload, "model",
		clawt_web_form_value(request, "model"));

	reply = clawt_web_app_call(app, "turn.rescue",
		clawt_web_payload_take(g_steal_pointer(&payload)));

	if (reply == NULL)
		return clawt_web_error_page(app, request, agent_id, CLAWT_PAGE_CHAT,
			clawt_web_app_last_error(app));

	return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_CHAT,
		g_strcmp0(action, "switch") == 0
			? "Stopped, and sent the same message again on the other model, "
			  "for this turn only."
			: "Stopped.");
}

void
clawt_web_register_rescue(
	HtmxRouter  *router,
	ClawtWebApp *app
){
	htmx_router_post(router, "/a/:id/rescue", on_rescue, app);
}
