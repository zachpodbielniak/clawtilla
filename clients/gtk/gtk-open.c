/*
 * gtk-open.c - Opening a room from outside the window, and the exchange
 *              between two agents
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `clawtilla-gtk --open <room>` and a `clawtilla:` link in an org file
 * both arrive here, in the running window when there is one: the
 * application is single-instance, so a second invocation activates the
 * `app.open-room` action in the first and exits (main.c).
 *
 * The exchange view lives here too because it is the other way of
 * reaching a conversation that is not the one on screen: it lists one
 * peer's requests and this agent's written answers, out of the room's
 * own history, without the tool steps between them.
 */

#include "clawt-window-private.h"

/* How far back the exchange view reads a room's history. */
#define EXCHANGE_HISTORY_LIMIT "500"

/* ── --open ──────────────────────────────────────────────────────── */

/*
 * The `agents` or `rooms` array of a listing reply, or NULL.  Borrowed
 * from @reply.
 */
static JsonArray *
reply_array(
	JsonNode    *reply,
	const gchar *member
){
	JsonObject *payload;
	JsonNode *node;

	payload = (reply != NULL) ? clawt_payload_of(reply) : NULL;

	if (payload == NULL || !json_object_has_member(payload, member))
		return NULL;

	node = json_object_get_member(payload, member);

	return JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}

/*
 * Opens an already-normalised target.  The listings are asked for here,
 * when the link is followed, rather than taken from the sidebar's last
 * refresh: a link names something that may have been created a moment
 * ago, and the sidebar may not have caught up.
 */
static void
open_now(
	ClawtWindow *self,
	const gchar *target
){
	g_autoptr(JsonNode) agents = NULL;
	g_autoptr(JsonNode) rooms = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *agent = NULL;
	g_autofree gchar *peer = NULL;
	g_autofree gchar *room = NULL;

	/* Another fleet's agent: the daemon opens the conversation (0.72.0). */
	if (clawt_fed_ref_parse(target, NULL, NULL)) {
		g_autoptr(JsonBuilder) b = json_builder_new();
		g_autoptr(JsonNode) opened = NULL;
		const gchar *fed_room;

		json_builder_begin_object(b);
		json_builder_set_member_name(b, "ref");
		json_builder_add_string_value(b, target);
		json_builder_end_object(b);
		opened = clawt_window_request(self, "federation.open", json_builder_get_root(b));
		fed_room = opened != NULL
			? clawt_json_string(clawt_payload_of(opened), "room", NULL) : NULL;
		if (fed_room != NULL)
			clawt_gtk_select_room(self, fed_room);
		return;
	}

	agents = clawt_window_request(self, "agent.list", NULL);
	rooms = clawt_window_request(self, "room.list", NULL);

	/* a failed listing has already said why, as a toast */
	if (agents == NULL || rooms == NULL)
		return;

	if (!clawt_open_target_resolve(target, reply_array(agents, "agents"),
				       reply_array(rooms, "rooms"), &agent,
				       &peer, &room, &error)) {
		g_debug("--open %s: %s", target, error->message);
		clawt_window_toast(self, error->message);
		return;
	}

	g_debug("--open %s: agent %s, peer %s, room %s", target,
		agent != NULL ? agent : "-", peer != NULL ? peer : "-",
		room != NULL ? room : "-");

	if (room != NULL) {
		clawt_gtk_select_room(self, room);
	} else {
		GAction *action;

		clawt_gtk_select_agent(self, agent);

		/*
		 * Through the switcher's own action, so the menu, the
		 * read-only composer and the transcript all agree on which
		 * conversation is open -- the same path a click takes.
		 */
		action = g_action_map_lookup_action(G_ACTION_MAP(self),
						    "conversation");

		if (action != NULL)
			g_action_activate(action, g_variant_new_string(
						  peer != NULL ? peer : ""));
	}

	clawt_gtk_show_page(self, CLAWT_PAGE_CHAT);
	clawt_gtk_refresh_agents(self);
	gtk_window_present(GTK_WINDOW(self));
}

void
clawt_window_open_target(ClawtWindow *self, const gchar *text)
{
	g_autoptr(GError) error = NULL;
	g_autofree gchar *target = NULL;

	g_return_if_fail(CLAWT_IS_WINDOW(self));

	target = clawt_open_target_normalize(text, &error);

	if (target == NULL) {
		g_autofree gchar *message = g_strdup_printf(
			"Cannot open \"%s\": %s", text != NULL ? text : "",
			error->message);

		/*
		 * The text is shown back only after normalisation refused
		 * it, and a toast's label is plain text, so a hostile link
		 * cannot reach a markup parser through here.
		 */
		clawt_window_toast(self, message);
		return;
	}

	/*
	 * Not connected yet: held, and opened by reload_after_connect().
	 * Asking now would toast a refusal for a daemon that is merely
	 * starting, and the link would be lost.  A later --open replaces
	 * an earlier one still waiting, which is what clicking twice means.
	 */
	target = clawt_open_target_hold(&self->pending_open,
					g_steal_pointer(&target),
					self->client != NULL &&
					clawt_client_is_connected(self->client));

	if (target == NULL) {
		gtk_window_present(GTK_WINDOW(self));
		return;
	}

	open_now(self, target);
}

void
clawt_gtk_open_pending(
	ClawtWindow *self
){
	g_autofree gchar *target = NULL;

	if (self->pending_open == NULL)
		return;

	target = g_steal_pointer(&self->pending_open);
	open_now(self, target);
}

/* ── The exchange view ───────────────────────────────────────────── */

/* One entry: who and when, then what they wrote, rendered from markdown. */
static GtkWidget *
exchange_row_new(
	ClawtWindow *self,
	JsonObject  *message,
	gboolean     from_peer
){
	GtkWidget *row;
	GtkWidget *header;
	GtkWidget *who;
	GtkWidget *at;
	GtkWidget *text;
	const gchar *sender;
	gint64 ts;
	g_autofree gchar *stamp = NULL;

	(void)self;

	sender = clawt_json_string(message, "sender", "?");
	ts = clawt_json_int(message, "ts", 0);

	if (ts > 0) {
		g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(ts);

		stamp = clawt_chat_time_label(when);
	}

	row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	who = gtk_label_new(sender);
	at = gtk_label_new(stamp);
	text = gtk_label_new(NULL);

	gtk_widget_add_css_class(who, "heading");
	gtk_widget_add_css_class(at, "caption");
	gtk_widget_add_css_class(at, "dim-label");
	gtk_box_append(GTK_BOX(header), who);
	gtk_box_append(GTK_BOX(header), at);

	/* rendered as the transcript renders it: markdown, never markup */
	clawt_gtk_set_label_markdown(GTK_LABEL(text),
				     clawt_json_string(message, "body", ""));
	gtk_label_set_wrap(GTK_LABEL(text), TRUE);
	gtk_label_set_selectable(GTK_LABEL(text), TRUE);
	gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
	gtk_widget_add_css_class(text, "body");

	/*
	 * The peer's requests on a card, this agent's answers bare: the
	 * one difference that says who is speaking down a long column.
	 */
	if (from_peer)
		gtk_widget_add_css_class(row, "card");

	gtk_widget_set_margin_top(text, 2);
	gtk_box_append(GTK_BOX(row), header);
	gtk_box_append(GTK_BOX(row), text);

	if (from_peer) {
		gtk_widget_set_margin_start(header, 12);
		gtk_widget_set_margin_top(header, 10);
		gtk_widget_set_margin_start(text, 12);
		gtk_widget_set_margin_end(text, 12);
		gtk_widget_set_margin_bottom(text, 10);
	}

	return row;
}

void
clawt_gtk_show_exchange(
	ClawtWindow *self,
	const gchar *agent_id,
	const gchar *peer_id
){
	g_autoptr(JsonNode) reply = NULL;
	g_autoptr(JsonArray) exchange = NULL;
	g_autofree gchar *title = NULL;
	JsonArray *messages;
	AdwDialog *dialog;
	GtkWidget *toolbar;
	GtkWidget *scroll;
	GtkWidget *box;
	guint i;

	if (agent_id == NULL || peer_id == NULL)
		return;

	/*
	 * The direct room between the two, resolved by the daemon from a
	 * member and a viewer -- the same request the conversation switcher
	 * makes, with a longer reach back.
	 */
	reply = clawt_window_request(
		self, "room.history",
		clawt_build_payload("room", peer_id, "as", agent_id,
				    "limit", EXCHANGE_HISTORY_LIMIT, NULL));

	if (reply == NULL)
		return;

	messages = reply_array(reply, "messages");
	exchange = clawt_chat_exchange(messages, agent_id, peer_id);
	title = clawt_chat_exchange_title(agent_id, peer_id);

	dialog = adw_dialog_new();
	toolbar = adw_toolbar_view_new();
	scroll = gtk_scrolled_window_new();
	box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);

	adw_dialog_set_title(dialog, title);
	adw_dialog_set_content_width(dialog, 560);
	adw_dialog_set_content_height(dialog, 640);

	for (i = 0; i < json_array_get_length(exchange); i++) {
		JsonObject *message = json_array_get_object_element(exchange, i);

		gtk_box_append(GTK_BOX(box), exchange_row_new(
			self, message,
			g_strcmp0(clawt_json_string(message, "sender", NULL),
				  peer_id) == 0));
	}

	if (json_array_get_length(exchange) == 0) {
		GtkWidget *empty = adw_status_page_new();

		adw_status_page_set_title(ADW_STATUS_PAGE(empty),
					  "Nothing between them yet");
		adw_status_page_set_description(ADW_STATUS_PAGE(empty),
			"The requests and answers are no longer in this "
			"conversation's recent history.");
		gtk_box_append(GTK_BOX(box), empty);
	}

	gtk_widget_set_margin_top(box, 12);
	gtk_widget_set_margin_bottom(box, 12);
	gtk_widget_set_margin_start(box, 12);
	gtk_widget_set_margin_end(box, 12);

	/*
	 * NEVER horizontally: a wrapping label's natural width is its
	 * unwrapped string, and AUTOMATIC would give it that and scroll.
	 */
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
				       GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_widget_set_vexpand(scroll, TRUE);

	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
				     adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
	adw_dialog_set_child(dialog, toolbar);
	adw_dialog_present(dialog, GTK_WIDGET(self));
}
