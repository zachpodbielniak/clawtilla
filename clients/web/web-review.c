/*
 * web-review.c - The agent page's Memory review: what an agent changed in
 * its memories, with an Undo per change
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The web half of the inspector group in gtk-review.c and lrg-review.c.
 * Every remember, forget, pin and unpin is journaled by the store; this
 * card lists the journal newest first, with an Undo form on each row the
 * daemon says it would accept and the daemon's reason on each it would
 * not.  The labels come from libclawt so the three clients agree.
 */

#include "clawt-web.h"
#include "web-pages.h"

void
clawt_web_add_memory_review_card(
	ClawtWebApp *app,
	HtmxElement *parent,
	const gchar *agent_id
){
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(HtmxDiv) card = clawt_web_card(
		"Memory review",
		"Everything this agent remembered, forgot or pinned, newest first. "
		"Undo takes one change back: an added memory is archived, a "
		"forgotten one returns, a pin flips back. Nothing is deleted.");
	HtmxElement *body = clawt_web_card_body(card);
	g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
	g_autofree gchar *action = g_strdup_printf("/a/%s/memory/undo", escaped);
	JsonArray *entries;
	gint64 now = g_get_real_time();
	guint i;

	clawt_web_payload_set(payload, "agent", agent_id);
	reply = clawt_web_app_call(app, "memory.journal",
		clawt_web_payload_take(g_steal_pointer(&payload)));
	entries = clawt_web_member_array(clawt_web_root(reply), "entries");

	if (reply == NULL) {
		g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

		clawt_web_add(body, clawt_web_notice(why, "info"));
	} else if (entries == NULL || json_array_get_length(entries) == 0) {
		clawt_web_add(body, clawt_web_empty(
			"No changes yet",
			"Each time this agent remembers, forgets or pins something it "
			"shows here, with a way to take it back."));
	}

	for (i = 0; entries != NULL && i < json_array_get_length(entries); i++) {
		JsonObject *entry = json_array_get_object_element(entries, i);
		const gchar *kind = clawt_web_member(entry, "action", "");
		const gchar *scope = clawt_web_member(entry, "scope", "agent");
		const gchar *why = clawt_web_member(entry, "why_not", NULL);
		g_autoptr(HtmxDiv) row = htmx_div_new();
		g_autoptr(HtmxDiv) head = htmx_div_new();
		g_autoptr(HtmxSpan) label = htmx_span_new();
		g_autofree gchar *title = NULL;
		g_autofree gchar *ago = clawt_time_ago_label(
			clawt_web_member_int(entry, "at", 0) * G_USEC_PER_SEC, now);
		g_autofree gchar *line = NULL;

		title = g_strdup_printf("%s: %s",
			clawt_memory_journal_action_label(kind),
			clawt_web_member(entry, "excerpt",
				clawt_web_member(entry, "memory_id", "?")));
		line = g_strdup_printf("%s \302\267 %s",
			clawt_web_member(entry, "line", ""), ago);

		htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
		htmx_element_add_class(HTMX_ELEMENT(head), "list-item-head");
		htmx_element_add_class(HTMX_ELEMENT(label), "list-item-title");
		htmx_node_set_text_content(HTMX_NODE(label), title);
		htmx_node_add_child(HTMX_NODE(head), HTMX_NODE(label));

		/*
		 * A shared database says so, since "the team believes this" and
		 * "this agent decided it" are different claims about one row.
		 */
		if (g_strcmp0(scope, "agent") != 0)
			clawt_web_add(head, clawt_web_badge(scope, "info"));

		htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(head));
		clawt_web_add(row, clawt_web_text(line, "small muted"));

		if (clawt_web_member_bool(entry, "undoable", FALSE)) {
			g_autoptr(HtmxForm) form = clawt_web_form(action);
			g_autoptr(HtmxButton) undo = clawt_web_button(
				clawt_memory_journal_undo_label(kind), "default");
			g_autofree gchar *id = g_strdup_printf("%" G_GINT64_FORMAT,
				clawt_web_member_int(entry, "id", 0));

			clawt_web_add(form, clawt_web_hidden("entry", id));
			clawt_web_add(form, clawt_web_hidden("scope", scope));
			htmx_element_set_attribute(HTMX_ELEMENT(undo), "type", "submit");
			htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(undo));
			htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(form));
		} else if (why != NULL && g_strcmp0(kind, "undo") != 0 &&
			clawt_web_member_int(entry, "undone_at", 0) == 0) {
			g_autofree gchar *note = g_strdup_printf("Cannot be undone: %s",
				why);

			clawt_web_add(row, clawt_web_text(note, "small muted"));
		}

		htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
	}

	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* Undoes one journal row, as the operator. */
static HtmxResponse *
on_memory_undo(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ClawtWebApp *app = user_data;
	g_autofree gchar *agent_id = clawt_web_param(params, "id");
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	const gchar *entry = clawt_web_form_value(request, "entry");

	clawt_web_payload_set(payload, "agent", agent_id);
	clawt_web_payload_set(payload, "scope",
		clawt_web_form_value(request, "scope"));
	clawt_web_payload_set_int(payload, "id",
		entry != NULL ? g_ascii_strtoll(entry, NULL, 10) : 0);

	reply = clawt_web_app_call(app, "memory.undo",
		clawt_web_payload_take(g_steal_pointer(&payload)));

	if (reply == NULL)
		return clawt_web_error_page(app, request, agent_id, CLAWT_PAGE_AGENT,
			clawt_web_app_last_error(app));

	return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_AGENT,
		"Undone. The agent reads its memories as they are now from its "
		"next search.");
}

void
clawt_web_register_review(
	HtmxRouter  *router,
	ClawtWebApp *app
){
	htmx_router_post(router, "/a/:id/memory/undo", on_memory_undo, app);
}
