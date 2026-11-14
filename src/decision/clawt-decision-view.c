/*
 * clawt-decision-view.c - What every client draws about one decision
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A decision is drawn in two places -- the inbox, and inline in the room
 * it was asked from -- by three clients.  Six renderings of one record
 * agree only if what they say is decided once, here, where it can be
 * tested without a window or a browser.
 */

#include "clawtilla.h"
#include "decision/clawt-decision-view.h"

/* The surfaces a person can settle a decision from, as `via` spells them. */
static const gchar * const KNOWN_VIA[] = {
	CLAWT_DECISION_VIA_CHAT,
	CLAWT_DECISION_VIA_INBOX,
	CLAWT_DECISION_VIA_CLI
};

gboolean
clawt_decision_via_is_known(
	const gchar *via
){
	guint i;

	if (via == NULL)
		return TRUE;

	for (i = 0; i < G_N_ELEMENTS(KNOWN_VIA); i++)
		if (g_strcmp0(via, KNOWN_VIA[i]) == 0)
			return TRUE;

	return FALSE;
}

/* A string member, or NULL for one that is missing, null or not a string. */
static const gchar *
member_string(
	JsonObject  *object,
	const gchar *name
){
	JsonNode *node;

	if (!json_object_has_member(object, name))
		return NULL;

	node = json_object_get_member(object, name);

	if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE ||
	    json_node_get_value_type(node) != G_TYPE_STRING)
		return NULL;

	return json_node_get_string(node);
}

/* An integer member, or @fallback; the wire writes these as JSON ints. */
static gint64
member_int(
	JsonObject  *object,
	const gchar *name,
	gint64       fallback
){
	JsonNode *node;

	if (!json_object_has_member(object, name))
		return fallback;

	node = json_object_get_member(object, name);

	if (node == NULL || JSON_NODE_TYPE(node) != JSON_NODE_VALUE ||
	    json_node_get_value_type(node) != G_TYPE_INT64)
		return fallback;

	return json_node_get_int(node);
}

ClawtDecision *
clawt_decision_new_from_object(
	JsonObject *object
){
	g_autoptr(ClawtDecision) decision = NULL;
	g_autoptr(GPtrArray) options = NULL;
	const gchar *id;
	const gchar *answer;
	gint64 state;

	if (object == NULL)
		return NULL;

	id = member_string(object, "id");

	if (id == NULL || *id == '\0')
		return NULL;

	decision = clawt_decision_new(id, member_string(object, "agent"),
	                              member_string(object, "question"));

	clawt_decision_set_default(decision, member_string(object, "default"),
	                           member_string(object, "default_reason"));
	clawt_decision_set_task(decision, member_string(object, "task"));
	clawt_decision_set_room(decision, member_string(object, "room"));
	clawt_decision_set_routed_to(decision, member_string(object, "routed_to"));
	clawt_decision_set_answered_by(decision,
	                               member_string(object, "answered_by"));
	clawt_decision_set_reversible_until(
		decision, member_int(object, "reversible_until", 0));
	clawt_decision_set_created_at(decision,
	                              member_int(object, "created_at", 0));

	/* The options, skipping anything that is not a non-empty string. */
	if (json_object_has_member(object, "options") &&
	    JSON_NODE_HOLDS_ARRAY(json_object_get_member(object, "options"))) {
		JsonArray *array = json_object_get_array_member(object,
		                                                "options");
		guint i;

		options = g_ptr_array_new();

		for (i = 0; i < json_array_get_length(array); i++) {
			JsonNode *node = json_array_get_element(array, i);
			const gchar *option;

			if (JSON_NODE_TYPE(node) != JSON_NODE_VALUE ||
			    json_node_get_value_type(node) != G_TYPE_STRING)
				continue;

			option = json_node_get_string(node);

			if (option != NULL && *option != '\0')
				g_ptr_array_add(options, (gpointer)option);
		}

		g_ptr_array_add(options, NULL);
		clawt_decision_set_options(
			decision, (const gchar * const *)options->pdata);
	}

	/*
	 * The answer before the state, exactly as the store reads a row:
	 * clawt_decision_answer() sets ANSWERED, and a defaulted or
	 * dismissed decision must not come back as somebody's choice.
	 */
	answer = member_string(object, "answer");

	if (answer != NULL)
		clawt_decision_answer(decision, answer,
		                      member_int(object, "answered_at", 0));

	clawt_decision_set_answered_at(decision,
	                               member_int(object, "answered_at", 0));

	state = member_int(object, "state", (gint64)CLAWT_DECISION_OPEN);

	/* An unknown number is not a state; it stays open rather than guessed. */
	if (state >= (gint64)CLAWT_DECISION_OPEN &&
	    state <= (gint64)CLAWT_DECISION_DISMISSED)
		clawt_decision_set_state(decision, (ClawtDecisionState)state);

	clawt_decision_set_settled_via(decision,
	                               member_string(object, "settled_via"));

	return (ClawtDecision *)g_steal_pointer(&decision);
}

gboolean
clawt_decision_apply_settled(
	ClawtDecision *self,
	const gchar   *how,
	const gchar   *answer,
	const gchar   *via,
	gint64         when
){
	ClawtDecisionState state;

	g_return_val_if_fail(self != NULL, FALSE);

	if (clawt_decision_get_state(self) != CLAWT_DECISION_OPEN)
		return FALSE;

	if (g_strcmp0(how, "answered") == 0)
		state = CLAWT_DECISION_ANSWERED;
	else if (g_strcmp0(how, "defaulted") == 0)
		state = CLAWT_DECISION_DEFAULTED;
	else if (g_strcmp0(how, "dismissed") == 0)
		state = CLAWT_DECISION_DISMISSED;
	else
		/*
		 * `deleted` is not a state: the record is gone, and a view
		 * of it goes with it (decision.deleted says so).  Anything
		 * else is a word from a newer daemon, and leaving the view
		 * open is the honest reading of a word nobody understood.
		 */
		return FALSE;

	if (answer != NULL && *answer != '\0')
		clawt_decision_answer(self, answer, when);

	clawt_decision_set_answered_at(self, when);
	clawt_decision_set_state(self, state);
	clawt_decision_set_settled_via(self, via);

	return TRUE;
}

gboolean
clawt_decision_apply_event(
	ClawtDecision *self,
	ClawtEvent    *event
){
	g_return_val_if_fail(self != NULL, FALSE);

	if (event == NULL ||
	    g_strcmp0(clawt_event_get_kind(event), "decision.settled") != 0 ||
	    g_strcmp0(clawt_event_get_detail(event, "decision"),
	              clawt_decision_get_id(self)) != 0)
		return FALSE;

	/* The event is stamped in microseconds; a decision in seconds. */
	return clawt_decision_apply_settled(
		self, clawt_event_get_detail(event, "how"),
		clawt_event_get_detail(event, "answer"),
		clawt_event_get_detail(event, "via"),
		clawt_event_get_timestamp(event) / G_USEC_PER_SEC);
}

const gchar *
clawt_decision_event_room(
	ClawtEvent *event
){
	const gchar *room;

	if (event == NULL)
		return NULL;

	room = clawt_event_get_detail(event, "room");

	return (room != NULL && *room != '\0') ? room : NULL;
}

/* " in the chat", " in the inbox", " from the command line", or "". */
static const gchar *
via_phrase(
	const gchar *via
){
	if (g_strcmp0(via, CLAWT_DECISION_VIA_CHAT) == 0)
		return " in the chat";
	if (g_strcmp0(via, CLAWT_DECISION_VIA_INBOX) == 0)
		return " in the inbox";
	if (g_strcmp0(via, CLAWT_DECISION_VIA_CLI) == 0)
		return " from the command line";

	return "";
}

gchar *
clawt_decision_outcome_text(
	ClawtDecision *self
){
	const gchar *answer;
	const gchar *fallback;
	const gchar *via;

	g_return_val_if_fail(self != NULL, NULL);

	answer = clawt_decision_get_answer(self);
	fallback = clawt_decision_get_default(self);
	via = clawt_decision_get_settled_via(self);

	/*
	 * Named, with no default: -- -Wswitch is what tells the next state
	 * somebody adds that it has no sentence yet.
	 */
	switch (clawt_decision_get_state(self)) {
	case CLAWT_DECISION_OPEN:
		return NULL;

	case CLAWT_DECISION_ANSWERED:
		/*
		 * "You" only when a person's surface is known: an answer
		 * with no `via` came from an older client or the venture
		 * bridge, and claiming it for the reader would be a guess.
		 */
		if (via != NULL)
			return g_strdup_printf("You answered \xe2\x80\x9c%s\xe2\x80\x9d%s.",
			                       answer != NULL ? answer : "",
			                       via_phrase(via));

		return g_strdup_printf("Answered: \xe2\x80\x9c%s\xe2\x80\x9d.",
		                       answer != NULL ? answer : "");

	case CLAWT_DECISION_DEFAULTED:
		return g_strdup_printf(
			"Nobody answered in time, so the default stands: "
			"\xe2\x80\x9c%s\xe2\x80\x9d.",
			answer != NULL ? answer
			               : (fallback != NULL ? fallback : ""));

	case CLAWT_DECISION_DISMISSED:
		if (fallback != NULL && *fallback != '\0')
			return g_strdup_printf(
				"Dismissed%s as not needing you; the default "
				"stands: \xe2\x80\x9c%s\xe2\x80\x9d.",
				via_phrase(via), fallback);

		return g_strdup_printf("Dismissed%s as not needing you.",
		                       via_phrase(via));
	}

	return NULL;
}

gchar *
clawt_decision_default_text(
	ClawtDecision *self,
	GTimeZone     *tz
){
	g_autoptr(GString) line = NULL;
	const gchar *fallback;
	const gchar *reason;
	gint64 until;

	g_return_val_if_fail(self != NULL, NULL);

	fallback = clawt_decision_get_default(self);
	reason = clawt_decision_get_default_reason(self);
	until = clawt_decision_get_reversible_until(self);

	if (fallback == NULL || *fallback == '\0')
		return NULL;

	line = g_string_new(NULL);
	g_string_append_printf(line,
	                       "If nobody answers: \xe2\x80\x9c%s\xe2\x80\x9d",
	                       fallback);

	if (reason != NULL && *reason != '\0')
		g_string_append_printf(line, " \xe2\x80\x94 %s", reason);

	g_string_append_c(line, '.');

	/*
	 * A clock time in the reader's zone, 24-hour like every chat
	 * stamp: "%a %d %b %H:%M" has no locale-dependent AM/PM to fit.
	 */
	if (until > 0) {
		g_autoptr(GTimeZone) local = NULL;
		g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(until);
		g_autoptr(GDateTime) when = NULL;
		g_autofree gchar *stamp = NULL;

		if (tz == NULL)
			local = g_time_zone_new_local();

		when = (utc != NULL)
			? g_date_time_to_timezone(utc, tz != NULL ? tz : local)
			: NULL;
		stamp = (when != NULL)
			? g_date_time_format(when, "%a %d %b %H:%M") : NULL;

		if (stamp != NULL)
			g_string_append_printf(line,
			                       " Reversible until %s.", stamp);
	}

	return g_string_free(g_steal_pointer(&line), FALSE);
}

gchar *
clawt_decision_refusal_text(
	ClawtDecision *self
){
	g_autofree gchar *outcome = NULL;
	const gchar *agent;

	g_return_val_if_fail(self != NULL, NULL);

	outcome = clawt_decision_outcome_text(self);
	agent = clawt_decision_get_agent(self);

	return g_strdup_printf(
		"Already settled: %s Your press changed nothing; if you have "
		"changed your mind, say so in a message to %s.",
		outcome != NULL ? outcome : "it is no longer open.",
		agent != NULL ? agent : "the agent");
}

gboolean
clawt_decision_precedes(
	ClawtDecision *self,
	gint64         message_ts
){
	g_return_val_if_fail(self != NULL, FALSE);

	return clawt_decision_get_created_at(self) < message_ts;
}
