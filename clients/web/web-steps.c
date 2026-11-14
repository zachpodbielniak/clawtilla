/*
 * web-steps.c - The Screen view's saved steps: a desktop task replayed
 * with no model
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The web half of gtk-steps.c.  A card under the Screen card: what the
 * agent's last turn did to the screen (`steps.last`), a name field and
 * Save when the daemon says it can be replayed exactly, and every saved
 * list with Replay and Delete forms (`steps.list`), all posting to
 * /a/:id/steps/<save|replay|delete>, registered before the /a/:id/:view
 * catch-all.  The step lines are the daemon's, built by libclawt.
 */

#include "clawt-web.h"
#include "web-pages.h"

static gchar *
steps_line(
	JsonArray *steps
){
	g_autoptr(GString) line = g_string_new(NULL);
	guint n = (steps != NULL) ? json_array_get_length(steps) : 0;
	guint i;

	g_string_append_printf(line, "%u step%s", n, n == 1 ? "" : "s");

	for (i = 0; i < n && i < 4; i++)
		g_string_append_printf(line, "%s%s", i == 0 ? ": " : " \302\267 ",
			json_array_get_string_element(steps, i));

	if (n > 4)
		g_string_append(line, " \342\200\246");

	return g_string_free(g_steal_pointer(&line), FALSE);
}

void
clawt_web_add_saved_steps_card(
	ClawtWebApp *app,
	HtmxElement *parent,
	const gchar *agent_id
){
	g_autoptr(ClawtWebPayload) ask_last = clawt_web_payload_new();
	g_autoptr(ClawtWebPayload) ask_list = clawt_web_payload_new();
	g_autoptr(JsonNode) last = NULL;
	g_autoptr(JsonNode) listing = NULL;
	g_autoptr(HtmxDiv) card = clawt_web_card(
		"Saved steps",
		"What the agent pressed on this screen, kept so it can be done "
		"again with no model. Replay needs you to hold the screen and "
		"refuses a screen that has changed.");
	HtmxElement *body = clawt_web_card_body(card);
	g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
	g_autofree gchar *save_url = g_strdup_printf("/a/%s/steps/save", escaped);
	g_autofree gchar *replay_url = g_strdup_printf("/a/%s/steps/replay",
		escaped);
	g_autofree gchar *delete_url = g_strdup_printf("/a/%s/steps/delete",
		escaped);
	JsonObject *run;
	JsonArray *saved;
	guint i;

	clawt_web_payload_set(ask_last, "agent", agent_id);
	last = clawt_web_app_call(app, "steps.last",
		clawt_web_payload_take(g_steal_pointer(&ask_last)));
	clawt_web_payload_set(ask_list, "agent", agent_id);
	listing = clawt_web_app_call(app, "steps.list",
		clawt_web_payload_take(g_steal_pointer(&ask_list)));

	run = clawt_web_root(last);

	if (run == NULL || clawt_web_member_array(run, "steps") == NULL) {
		clawt_web_add(body, clawt_web_text("The agent's last turn did "
			"nothing to this screen since the daemon started.",
			"small muted"));
	} else {
		g_autofree gchar *line = steps_line(clawt_web_member_array(run,
			"steps"));
		gboolean saveable = clawt_web_member_bool(run, "saveable", FALSE);
		g_autofree gchar *said = g_strdup_printf("Its last turn: %s.%s%s",
			line, saveable ? "" : " Cannot be saved: ",
			saveable ? "" : clawt_web_member(run, "why_not", "unknown"));

		clawt_web_add(body, clawt_web_text(said, "small"));

		if (saveable) {
			g_autoptr(HtmxForm) form = clawt_web_form(save_url);
			g_autoptr(HtmxButton) save = clawt_web_button("Save", "primary");

			clawt_web_add(form, clawt_web_field("Name", "name", NULL,
				"Name these steps"));
			htmx_element_set_attribute(HTMX_ELEMENT(save), "type", "submit");
			htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(save));
			htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
		}
	}

	saved = clawt_web_member_array(clawt_web_root(listing), "saved");

	for (i = 0; saved != NULL && i < json_array_get_length(saved); i++) {
		JsonObject *entry = json_array_get_object_element(saved, i);
		const gchar *name = clawt_web_member(entry, "name", "?");
		g_autofree gchar *line = steps_line(clawt_web_member_array(entry,
			"steps"));
		g_autoptr(HtmxDiv) row = htmx_div_new();
		g_autoptr(HtmxSpan) label = htmx_span_new();
		g_autoptr(HtmxForm) replay = clawt_web_form(replay_url);
		g_autoptr(HtmxForm) forget = clawt_web_form(delete_url);
		g_autoptr(HtmxButton) replay_button = clawt_web_button("Replay",
			"default");
		g_autoptr(HtmxButton) delete_button = clawt_web_button("Delete",
			"danger");

		htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
		htmx_element_add_class(HTMX_ELEMENT(label), "list-item-title");
		htmx_node_set_text_content(HTMX_NODE(label), name);
		htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(label));
		clawt_web_add(row, clawt_web_text(line, "small muted"));

		clawt_web_add(replay, clawt_web_hidden("name", name));
		htmx_element_set_attribute(HTMX_ELEMENT(replay_button), "type",
			"submit");
		htmx_node_add_child(HTMX_NODE(replay), HTMX_NODE(replay_button));
		htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(replay));

		clawt_web_add(forget, clawt_web_hidden("name", name));
		htmx_element_set_attribute(HTMX_ELEMENT(delete_button), "type",
			"submit");
		htmx_node_add_child(HTMX_NODE(forget), HTMX_NODE(delete_button));
		htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(forget));

		htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
	}

	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

static HtmxResponse *
on_steps(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ClawtWebApp *app = user_data;
	g_autofree gchar *agent_id = clawt_web_param(params, "id");
	g_autofree gchar *verb = clawt_web_param(params, "verb");
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	const gchar *kind;
	const gchar *done;

	if (g_strcmp0(verb, "save") == 0) {
		kind = "steps.save";
		done = "Saved. Take the screen and press Replay to run them again "
			"with no model.";
	} else if (g_strcmp0(verb, "replay") == 0) {
		kind = "steps.replay";
		done = "Checking the screen against the saved picture, then "
			"replaying.";
	} else if (g_strcmp0(verb, "delete") == 0) {
		kind = "steps.delete";
		done = "Deleted.";
	} else {
		return clawt_web_error_page(app, request, agent_id,
			CLAWT_PAGE_COMPUTER, "save, replay or delete");
	}

	clawt_web_payload_set(payload, "agent", agent_id);
	clawt_web_payload_set(payload, "name",
		clawt_web_form_value(request, "name"));

	reply = clawt_web_app_call(app, kind,
		clawt_web_payload_take(g_steal_pointer(&payload)));

	if (reply == NULL)
		return clawt_web_error_page(app, request, agent_id,
			CLAWT_PAGE_COMPUTER, clawt_web_app_last_error(app));

	return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_COMPUTER,
		done);
}

void
clawt_web_register_steps(
	HtmxRouter  *router,
	ClawtWebApp *app
){
	htmx_router_post(router, "/a/:id/steps/:verb", on_steps, app);
}
