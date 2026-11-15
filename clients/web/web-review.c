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

/* ── The review's proposals ──────────────────────────────────────── */

/*
 * memories.review.model: a small model suggests merges, forgets and pins
 * over this agent's own memories, and nothing is changed until a person
 * accepts one.  The model choice is libclawt's list, the same one the
 * acknowledgement check offers, so the three clients offer the same.
 */
void
clawt_web_add_memory_proposals_card(
	ClawtWebApp *app,
	HtmxElement *parent,
	const gchar *agent_id
){
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(HtmxDiv) card = clawt_web_card(
		"Suggested by the review",
		"Merges, forgets and pins a small model suggests for this agent's "
		"own memories. Nothing changes until you accept one; an accepted "
		"one appears in the Memory review with an Undo.");
	HtmxElement *body = clawt_web_card_body(card);
	g_autofree gchar *escaped = g_uri_escape_string(agent_id, NULL, FALSE);
	g_autofree gchar *answer = g_strdup_printf("/a/%s/memory/proposal",
		escaped);
	g_autofree gchar *review = g_strdup_printf("/a/%s/memory/review",
		escaped);
	g_autofree gchar *setting = g_strdup_printf("/a/%s/memory/review-model",
		escaped);
	JsonObject *root;
	JsonArray *proposals;
	gint64 now = g_get_real_time();
	guint i;

	clawt_web_payload_set(payload, "agent", agent_id);
	reply = clawt_web_app_call(app, "memory.proposals",
		clawt_web_payload_take(g_steal_pointer(&payload)));
	root = clawt_web_root(reply);

	if (reply == NULL || root == NULL) {
		g_autofree gchar *why = g_strdup(clawt_web_app_last_error(app));

		clawt_web_add(body, clawt_web_notice(why, "info"));
		htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
		return;
	}

	/* The setting: libclawt's choices, keeping a hand-written model. */
	{
		g_autoptr(HtmxForm) form = clawt_web_form(setting);
		g_autoptr(HtmxButton) button = clawt_web_button("Save", "primary");
		g_autoptr(GPtrArray) values = g_ptr_array_new();
		g_autoptr(GPtrArray) labels = g_ptr_array_new();
		const gchar *current = clawt_web_member(root, "model", "");
		gboolean listed = (*current == '\0');

		for (i = 0; i < clawt_ack_check_model_count(); i++) {
			g_ptr_array_add(values,
				(gpointer)clawt_ack_check_model_nth_value(i));
			g_ptr_array_add(labels,
				(gpointer)clawt_ack_check_model_nth_label(i));

			if (g_strcmp0(clawt_ack_check_model_nth_value(i), current) == 0)
				listed = TRUE;
		}

		if (!listed) {
			g_ptr_array_add(values, (gpointer)current);
			g_ptr_array_add(labels, (gpointer)clawt_web_member(root,
				"model_label", current));
		}

		g_ptr_array_add(values, NULL);
		g_ptr_array_add(labels, NULL);

		clawt_web_add(form, clawt_web_select_field(
			"Reviewed by (the whole fleet)", "model",
			(const gchar *const *)values->pdata,
			(const gchar *const *)labels->pdata, current));
		htmx_element_set_attribute(HTMX_ELEMENT(button), "type", "submit");
		clawt_web_add(form, g_steal_pointer(&button));
		clawt_web_add(body, g_steal_pointer(&form));
	}

	/* When it last ran, or why it proposed nothing; and Review now. */
	{
		const gchar *error = clawt_web_member(root, "last_error", NULL);
		gint64 last = clawt_web_member_int(root, "last_run_at", 0);
		g_autofree gchar *ago = (last > 0)
			? clawt_time_ago_label(last * G_USEC_PER_SEC, now) : NULL;
		g_autofree gchar *line = clawt_web_member_bool(root, "running", FALSE)
			? g_strdup("A pass is running now.")
			: (error != NULL)
				? g_strdup_printf("The last pass proposed nothing: %s", error)
				: (ago != NULL) ? g_strdup_printf("Last reviewed %s.", ago)
				: g_strdup("Not reviewed yet.");

		clawt_web_add(body, clawt_web_text(line, "small muted"));

		if (clawt_web_member_bool(root, "enabled", FALSE) &&
			!clawt_web_member_bool(root, "running", FALSE)) {
			g_autoptr(HtmxForm) form = clawt_web_form(review);
			g_autoptr(HtmxButton) button = clawt_web_button("Review now",
				"default");

			htmx_element_set_attribute(HTMX_ELEMENT(button), "type",
				"submit");
			htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(button));
			htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(form));
		}
	}

	proposals = clawt_web_member_array(root, "proposals");

	if (proposals == NULL || json_array_get_length(proposals) == 0)
		clawt_web_add(body, clawt_web_empty("Nothing suggested",
			"When the review suggests a change it waits here for you."));

	for (i = 0; proposals != NULL && i < json_array_get_length(proposals);
		i++) {
		JsonObject *proposal = json_array_get_object_element(proposals, i);
		JsonArray *about = clawt_web_member_array(proposal, "memories");
		const gchar *reason = clawt_web_member(proposal, "reason", NULL);
		const gchar *content = clawt_web_member(proposal, "content", NULL);
		g_autoptr(HtmxDiv) row = htmx_div_new();
		g_autoptr(HtmxForm) form = clawt_web_form(answer);
		g_autoptr(HtmxButton) accept = clawt_web_button("Accept", "primary");
		g_autoptr(HtmxButton) reject = clawt_web_button("Reject", "default");
		guint j;

		htmx_element_add_class(HTMX_ELEMENT(row), "list-item");
		htmx_element_add_class(HTMX_ELEMENT(row), "memory-proposal");
		clawt_web_add(row, clawt_web_text(
			clawt_web_member(proposal, "line", "A change"),
			"list-item-title"));

		/* The model's words, marked as its own; the memories as they are. */
		if (reason != NULL) {
			g_autofree gchar *why = g_strdup_printf("Why: %s", reason);

			clawt_web_add(row, clawt_web_text(why, "small muted"));
		}

		if (content != NULL) {
			g_autofree gchar *into = g_strdup_printf("Into: %s", content);

			clawt_web_add(row, clawt_web_text(into, "small"));
		}

		for (j = 0; about != NULL && j < json_array_get_length(about); j++) {
			JsonObject *memory = json_array_get_object_element(about, j);
			g_autofree gchar *text = g_strdup_printf("%s: %s",
				clawt_web_member(memory, "id", "?"),
				clawt_web_member(memory, "excerpt", "(gone since)"));

			clawt_web_add(row, clawt_web_text(text, "small muted"));
		}

		clawt_web_add(form, clawt_web_hidden("proposal",
			clawt_web_member(proposal, "id", "")));
		htmx_element_set_attribute(HTMX_ELEMENT(accept), "type", "submit");
		htmx_element_set_attribute(HTMX_ELEMENT(accept), "name", "answer");
		htmx_element_set_attribute(HTMX_ELEMENT(accept), "value", "accept");
		htmx_element_set_attribute(HTMX_ELEMENT(reject), "type", "submit");
		htmx_element_set_attribute(HTMX_ELEMENT(reject), "name", "answer");
		htmx_element_set_attribute(HTMX_ELEMENT(reject), "value", "reject");
		htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(accept));
		htmx_node_add_child(HTMX_NODE(form), HTMX_NODE(reject));
		htmx_node_add_child(HTMX_NODE(row), HTMX_NODE(form));
		htmx_node_add_child(HTMX_NODE(body), HTMX_NODE(row));
	}

	htmx_node_add_child(HTMX_NODE(parent), HTMX_NODE(card));
}

/* Accepts or rejects one proposal, as the operator. */
static HtmxResponse *
on_memory_proposal(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ClawtWebApp *app = user_data;
	g_autofree gchar *agent_id = clawt_web_param(params, "id");
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	gboolean accept = g_strcmp0(clawt_web_form_value(request, "answer"),
		"accept") == 0;

	clawt_web_payload_set(payload, "agent", agent_id);
	clawt_web_payload_set(payload, "id",
		clawt_web_form_value(request, "proposal"));

	reply = clawt_web_app_call(app, accept ? "memory.proposal_accept"
		: "memory.proposal_reject",
		clawt_web_payload_take(g_steal_pointer(&payload)));

	if (reply == NULL)
		return clawt_web_error_page(app, request, agent_id, CLAWT_PAGE_AGENT,
			clawt_web_app_last_error(app));

	return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_AGENT,
		accept ? "Accepted. It is in the Memory review with an Undo."
		       : "Rejected. Nothing was changed.");
}

/* A review pass now. */
static HtmxResponse *
on_memory_review_now(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ClawtWebApp *app = user_data;
	g_autofree gchar *agent_id = clawt_web_param(params, "id");
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;

	clawt_web_payload_set(payload, "agent", agent_id);
	reply = clawt_web_app_call(app, "memory.review",
		clawt_web_payload_take(g_steal_pointer(&payload)));

	if (reply == NULL)
		return clawt_web_error_page(app, request, agent_id, CLAWT_PAGE_AGENT,
			clawt_web_app_last_error(app));

	return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_AGENT,
		"Reviewing. Its suggestions appear when the model answers.");
}

/* The fleet's review model, saved from an agent's page. */
static HtmxResponse *
on_memory_review_model(
	HtmxRequest *request,
	GHashTable  *params,
	gpointer     user_data
){
	ClawtWebApp *app = user_data;
	g_autofree gchar *agent_id = clawt_web_param(params, "id");
	g_autoptr(ClawtWebPayload) payload = clawt_web_payload_new();
	g_autoptr(JsonNode) reply = NULL;
	const gchar *model = clawt_web_form_value(request, "model");

	clawt_web_payload_set(payload, "model", model != NULL ? model : "");
	reply = clawt_web_app_call(app, "memory.review_set",
		clawt_web_payload_take(g_steal_pointer(&payload)));

	if (reply == NULL)
		return clawt_web_error_page(app, request, agent_id, CLAWT_PAGE_AGENT,
			clawt_web_app_last_error(app));

	return clawt_web_after_action(app, request, agent_id, CLAWT_PAGE_AGENT,
		"Saved for the whole fleet.");
}

void
clawt_web_register_review(
	HtmxRouter  *router,
	ClawtWebApp *app
){
	htmx_router_post(router, "/a/:id/memory/undo", on_memory_undo, app);
	htmx_router_post(router, "/a/:id/memory/proposal", on_memory_proposal,
		app);
	htmx_router_post(router, "/a/:id/memory/review", on_memory_review_now,
		app);
	htmx_router_post(router, "/a/:id/memory/review-model",
		on_memory_review_model, app);
}
