/*
 * daemon-skill-proposal.c - Skills agents propose, and the decision each raises
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `clawtilla_skill_propose` writes a draft through the store in
 * src/skill/clawt-skill-proposal.c and files a decision whose id is the
 * proposal's own.  There are four ways that decision can be settled and
 * every one of them comes through clawt_daemon_skill_proposal_settle():
 *
 *   - `skill.proposal_resolve`, which is what the clients' Approve,
 *     Reject and Edit send, Edit carrying the person's text;
 *   - `decision.answer`, for the CLI and for anything that only knows
 *     decisions -- approve and reject, never edit, which needs text;
 *   - `decision.dismiss` and `decision.delete`, which reject: a draft
 *     whose question nobody will answer is a draft nobody approved.
 *
 * Nothing here leaves the machine or waits on a subprocess, so no verb
 * defers.
 */

#include "clawtilla.h"

#include <string.h>

#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

/* What an answer means, read once and the same way for every route. */
typedef enum {
	PROPOSAL_ACTION_APPROVE = 0,
	PROPOSAL_ACTION_REJECT,
	PROPOSAL_ACTION_EDIT
} ProposalAction;

/*
 * The answer a person gave, as an action.
 *
 * Anything that is not plainly an approval or an edit is a rejection,
 * with the person's words passed on to the agent.  The safe direction:
 * "looks good, but call it deploy" is not an approval of what the agent
 * wrote, and reading it as one would enable text nobody agreed to.
 */
static ProposalAction
action_from_answer(
	const gchar *answer
){
	if (answer == NULL)
		return PROPOSAL_ACTION_REJECT;

	if (g_ascii_strcasecmp(answer, "approve") == 0 ||
	    g_ascii_strcasecmp(answer, "approved") == 0)
		return PROPOSAL_ACTION_APPROVE;

	if (g_ascii_strcasecmp(answer, "edit") == 0 ||
	    g_ascii_strcasecmp(answer, "edited") == 0)
		return PROPOSAL_ACTION_EDIT;

	return PROPOSAL_ACTION_REJECT;
}

/* Whether @answer is one of the three words, rather than free text. */
static gboolean
answer_is_a_word(
	const gchar *answer
){
	static const gchar * const words[] = {
		"approve", "approved", "reject", "rejected", "edit", "edited", NULL
	};
	guint i;

	for (i = 0; answer != NULL && words[i] != NULL; i++)
		if (g_ascii_strcasecmp(answer, words[i]) == 0)
			return TRUE;

	return FALSE;
}

void
clawt_daemon_skill_proposals_open(
	ClawtDaemon *self
){
	g_autofree gchar *directory = NULL;

	g_clear_object(&self->skill_proposals);

	if (self->state_dir == NULL)
		return;

	/* Under the daemon's state: skills.dir is the person's library. */
	directory = g_build_filename(self->state_dir, "skill-proposals", NULL);
	self->skill_proposals = clawt_skill_proposals_new(directory);
}

/*
 * Tells the agent how its proposal went.
 *
 * A message from the operator, because the operator settled it -- and
 * one message per proposal, which costs the agent a turn exactly as an
 * answered decision does.  An agent that is never told goes on assuming
 * its draft is the skill, or proposes it again.
 */
static void
tell_the_agent(
	ClawtDaemon        *self,
	ClawtSkillProposal *proposal,
	const gchar        *note
){
	g_autofree gchar *body = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *agent = clawt_skill_proposal_get_agent(proposal);
	const gchar *name = clawt_skill_proposal_get_name(proposal);
	const gchar *outcome = NULL;

	if (self->router == NULL || agent == NULL || *agent == '\0')
		return;

	switch (clawt_skill_proposal_get_state(proposal)) {
	case CLAWT_SKILL_PROPOSAL_APPROVED:
		outcome = clawt_skill_proposal_is_update(proposal)
			? "was approved, and the skill now says what you proposed"
			: "was approved and is enabled; an agent has it once it is assigned";
		break;
	case CLAWT_SKILL_PROPOSAL_EDITED:
		outcome = "was edited by the operator and then approved -- read it "
		          "with clawtilla_skill_read if you have it, since it is not "
		          "exactly what you wrote";
		break;
	case CLAWT_SKILL_PROPOSAL_REJECTED:
		outcome = clawt_skill_proposal_is_update(proposal)
			? "was rejected; the skill is unchanged"
			: "was rejected, and the draft is gone";
		break;
	case CLAWT_SKILL_PROPOSAL_PENDING:
		return;
	}

	body = g_strdup_printf("[clawtilla] Your skill proposal for '%s' %s.%s%s%s",
	                       name, outcome,
	                       (note != NULL && *note != '\0') ? "\n\nThe operator said: " : "",
	                       (note != NULL && *note != '\0') ? note : "",
	                       clawt_skill_proposal_get_state(proposal) ==
	                           CLAWT_SKILL_PROPOSAL_REJECTED
	                           ? "\n\nDo not propose it again unchanged." : "");

	if (clawt_mailbox_router_send_to(self->router, "user", agent, body, NULL, 0,
	                                 &error) < 0)
		g_warning("skill proposals: could not tell %s about '%s': %s", agent, name,
		          error != NULL ? error->message : "unknown");
}

/* The decision-side bookkeeping, once a proposal has been settled. */
static void
settle_decision(
	ClawtDaemon        *self,
	ClawtSkillProposal *proposal,
	gboolean            answer_it
){
	g_autoptr(ClawtDecision) decision = NULL;
	const gchar *id = clawt_skill_proposal_get_id(proposal);

	if (self->decisions == NULL)
		return;

	decision = clawt_decision_store_get(self->decisions, id);
	if (decision == NULL || clawt_decision_get_state(decision) != CLAWT_DECISION_OPEN)
		return;

	if (answer_it) {
		g_autoptr(ClawtDecision) settled = NULL;
		g_autoptr(GError) error = NULL;

		settled = clawt_decision_store_answer(
			self->decisions, id,
			clawt_skill_proposal_state_to_string(clawt_skill_proposal_get_state(proposal)),
			&error);

		if (settled == NULL) {
			g_warning("skill proposals: %s was settled and its decision could not be "
			          "answered: %s", id, error->message);
			return;
		}

		clawt_daemon_publish_decision_settled(self, settled, "answered");
	}
}

ClawtSkillProposal *
clawt_daemon_skill_proposal_settle(
	ClawtDaemon  *self,
	const gchar  *id,
	const gchar  *action,
	const gchar  *note,
	const gchar  *description,
	const gchar  *body,
	gboolean      answer_decision,
	GError      **error
){
	g_autoptr(ClawtSkillProposal) settled = NULL;
	ProposalAction chosen;

	g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

	if (self->skill_proposals == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		                    "this daemon keeps no skill proposals");
		return NULL;
	}

	if (!clawt_skill_proposal_id_is_valid(id)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "'%s' is not a skill proposal", id != NULL ? id : "");
		return NULL;
	}

	chosen = action_from_answer(action);

	switch (chosen) {
	case PROPOSAL_ACTION_APPROVE:
		settled = clawt_skill_proposals_approve(self->skill_proposals, self->skills,
		                                        id, error);
		break;
	case PROPOSAL_ACTION_EDIT:
		/*
		 * Refused rather than read as approve: "edit" with no text is a
		 * person who wanted to change something, and enabling the agent's
		 * text unchanged would be the opposite of what they asked.
		 */
		if (description == NULL || body == NULL) {
			g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			                    "editing a proposal needs the new description and "
			                    "body; use the Edit button, or skill.proposal_resolve "
			                    "with action edit");
			return NULL;
		}
		settled = clawt_skill_proposals_edit(self->skill_proposals, self->skills, id,
		                                     description, body, error);
		break;
	case PROPOSAL_ACTION_REJECT:
		settled = clawt_skill_proposals_reject(self->skill_proposals, self->skills,
		                                       id, error);
		break;
	}

	if (settled == NULL)
		return NULL;

	settle_decision(self, settled, answer_decision);

	/* The Skills page lists both the library and the pending proposals. */
	if (self->bus != NULL)
		clawt_event_bus_emit(self->bus, "skill.changed",
		                     clawt_skill_proposal_get_name(settled));

	tell_the_agent(self, settled, note);

	return g_steal_pointer(&settled);
}

gboolean
clawt_daemon_skill_proposal_check_answer(
	ClawtDaemon  *self,
	const gchar  *id,
	const gchar  *answer,
	GError      **error
){
	(void)self;

	if (!clawt_skill_proposal_id_is_valid(id))
		return TRUE;

	/*
	 * Refused before the decision is touched: the daemon can already
	 * prove it cannot carry out "edit" without the text, and settling
	 * the decision first would leave the inbox saying answered over a
	 * draft nothing happened to.
	 */
	if (action_from_answer(answer) == PROPOSAL_ACTION_EDIT) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "a skill proposal is edited with its new text: use the "
		                    "Edit button in a client, or skill.proposal_resolve "
		                    "with action edit; answer approve or reject here");
		return FALSE;
	}

	return TRUE;
}

ClawtSkillProposal *
clawt_daemon_skill_proposal_answer(
	ClawtDaemon  *self,
	const gchar  *id,
	const gchar  *answer,
	GError      **error
){
	/* Free text is a rejection, and the words go to the agent with it. */
	return clawt_daemon_skill_proposal_settle(self, id, answer,
	                                          answer_is_a_word(answer) ? NULL : answer,
	                                          NULL, NULL, FALSE, error);
}

void
clawt_daemon_skill_proposal_withdraw(
	ClawtDaemon   *self,
	ClawtDecision *decision
){
	g_autoptr(ClawtSkillProposal) settled = NULL;
	g_autoptr(GError) error = NULL;
	const gchar *id;

	if (decision == NULL || self->skill_proposals == NULL)
		return;

	id = clawt_decision_get_id(decision);
	if (!clawt_skill_proposal_id_is_valid(id))
		return;

	/*
	 * A decision nobody will answer is a draft nobody approved.  Left
	 * pending, the draft would sit disabled in the library with no
	 * question in anybody's inbox asking about it, and the next proposal
	 * for that skill would be refused for a review that is never coming.
	 */
	settled = clawt_daemon_skill_proposal_settle(self, id, "reject",
	                                             "the decision was dismissed without "
	                                             "an answer", NULL, NULL, FALSE, &error);

	if (settled == NULL && error != NULL &&
	    !g_error_matches(error, CLAWT_ERROR, CLAWT_ERROR_FAILED))
		g_warning("skill proposals: could not withdraw %s: %s", id, error->message);
}

/*
 * The hook `clawtilla_skill_propose` calls: the draft, then the decision.
 *
 * No turn hold.  An agent that proposes a skill is not waiting on the
 * answer -- the reply says so -- and holding its budget open for a review
 * that may take a day would make every proposing turn look stalled.
 */
gchar *
clawt_daemon_skill_propose_for_tools(
	const gchar  *agent_id,
	const gchar  *name,
	const gchar  *description,
	const gchar  *body,
	gboolean      is_update,
	const gchar  *reason,
	gpointer      user_data,
	GError      **error
){
	static const gchar * const options[] = { "approve", "reject", "edit", NULL };
	ClawtDaemon *self = user_data;
	g_autoptr(ClawtSkillProposal) proposal = NULL;
	g_autoptr(ClawtDecision) decision = NULL;
	g_autoptr(ClawtSkillProposal) withdrawn = NULL;
	g_autofree gchar *question = NULL;
	g_autofree gchar *stored = NULL;
	gint64 limit;
	guint used;

	if (self->skill_proposals == NULL || self->decisions == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
		                    "this daemon keeps no decision inbox to ask in");
		return NULL;
	}

	limit = clawt_config_get_int(self->config, "skills.max_proposals_per_day");

	proposal = clawt_skill_proposals_propose(self->skill_proposals, self->skills,
	                                         agent_id, name, description, body,
	                                         is_update, reason,
	                                         limit > 0 ? (guint)limit : 0, error);
	if (proposal == NULL)
		return NULL;

	question = clawt_skill_proposals_describe(self->skill_proposals, self->skills,
	                                          clawt_skill_proposal_get_id(proposal));

	decision = clawt_decision_new(clawt_skill_proposal_get_id(proposal), agent_id,
	                              question);
	clawt_decision_set_options(decision, options);
	clawt_decision_set_default(decision, "reject",
	                           "nothing changes until somebody approves it");

	stored = clawt_decision_store_post(self->decisions, decision, error);

	/*
	 * A proposal with no question about it is a draft nobody will ever
	 * be asked to review, so it is taken back rather than left behind.
	 */
	if (stored == NULL) {
		withdrawn = clawt_skill_proposals_reject(self->skill_proposals, self->skills,
		                                         clawt_skill_proposal_get_id(proposal),
		                                         NULL);
		return NULL;
	}

	if (self->bus != NULL) {
		clawt_event_bus_emit(self->bus, "decision.asked", agent_id);
		clawt_event_bus_emit(self->bus, "skill.changed",
		                     clawt_skill_proposal_get_name(proposal));
	}

	used = clawt_skill_proposals_count_recent(self->skill_proposals, agent_id,
	                                          g_get_real_time());

	return g_strdup_printf(
		"Proposed as %s. %s It reaches no prompt until the operator approves "
		"it, and they may edit or reject it. Do not wait for the answer and do "
		"not write the skill's file yourself; you will get a message when it is "
		"settled. You have %u of %" G_GINT64_FORMAT " proposals left in the "
		"next 24 hours.",
		stored,
		clawt_skill_proposal_is_update(proposal)
			? "The change is staged; the skill itself is unchanged."
			: "The skill is in the library as a disabled draft.",
		used < (guint)limit ? (guint)limit - used : 0, limit);
}

/* ── The client surface ──────────────────────────────────────────── */

/*
 * One proposal, as a client sees it.
 *
 * Every piece of text the agent wrote goes out escaped -- the reason,
 * the description, the body, the whole draft and the diff -- because a
 * client shows each of them to the operator, and a forged `[clawtilla]`
 * line in a reason reads as the daemon talking in the middle of the
 * card.  The raw text never crosses: the editor is prefilled with the
 * escaped form, so a forged marker cannot even be approved back in
 * without a person seeing it rewritten.
 */
static void
add_proposal_object(
	ClawtDaemon        *self,
	JsonBuilder        *builder,
	ClawtSkillProposal *proposal
){
	const gchar *id = clawt_skill_proposal_get_id(proposal);
	g_autofree gchar *draft = NULL;
	g_autofree gchar *diff = NULL;
	g_autofree gchar *reason = NULL;
	g_autofree gchar *escaped_draft = NULL;
	g_autofree gchar *escaped_diff = NULL;
	g_autofree gchar *description = NULL;
	g_autofree gchar *body = NULL;
	g_autoptr(ClawtSkill) parsed = NULL;
	GPtrArray *warnings = NULL;
	guint i;

	draft = clawt_skill_proposals_get_draft(self->skill_proposals, id);
	diff = clawt_skill_proposals_get_diff(self->skill_proposals, self->skills, id);
	parsed = (draft != NULL)
		? clawt_skill_parse(draft, -1, clawt_skill_proposal_get_name(proposal), NULL)
		: NULL;

	reason = clawt_preamble_escape(clawt_skill_proposal_get_reason(proposal));
	escaped_draft = clawt_preamble_escape(draft);
	escaped_diff = clawt_preamble_escape(diff);

	if (parsed != NULL) {
		description = clawt_preamble_escape(clawt_skill_get_description(parsed));
		body = clawt_preamble_escape(clawt_skill_get_body(parsed));
		warnings = clawt_skill_get_warnings(parsed);
	}

	json_builder_begin_object(builder);
	clawt_daemon_add_string_member(builder, "id", id);
	/* the same id: a client finds the decision for a proposal by it */
	clawt_daemon_add_string_member(builder, "decision", id);
	clawt_daemon_add_string_member(builder, "agent",
	                               clawt_skill_proposal_get_agent(proposal));
	clawt_daemon_add_string_member(builder, "name",
	                               clawt_skill_proposal_get_name(proposal));
	clawt_daemon_add_string_member(builder, "kind",
	                               clawt_skill_proposal_is_update(proposal)
	                                   ? "update" : "create");
	clawt_daemon_add_string_member(
		builder, "state",
		clawt_skill_proposal_state_to_string(clawt_skill_proposal_get_state(proposal)));
	clawt_daemon_add_string_member(builder, "reason", reason);
	clawt_daemon_add_string_member(builder, "description", description);
	clawt_daemon_add_string_member(builder, "body", body);
	clawt_daemon_add_string_member(builder, "text", escaped_draft);
	clawt_daemon_add_string_member(builder, "diff", escaped_diff);

	json_builder_set_member_name(builder, "created_at");
	json_builder_add_int_value(builder, clawt_skill_proposal_get_created_at(proposal));

	json_builder_set_member_name(builder, "settled_at");
	json_builder_add_int_value(builder, clawt_skill_proposal_get_settled_at(proposal));

	json_builder_set_member_name(builder, "stale");
	json_builder_add_boolean_value(
		builder, self->skills != NULL &&
		         clawt_skill_proposals_is_stale(self->skill_proposals, self->skills, id));

	json_builder_set_member_name(builder, "warnings");
	json_builder_begin_array(builder);
	for (i = 0; warnings != NULL && i < warnings->len; i++)
		json_builder_add_string_value(builder, g_ptr_array_index(warnings, i));
	json_builder_end_array(builder);

	json_builder_end_object(builder);
}

JsonNode *
clawt_daemon_handle_skill_proposal(
	ClawtDaemon  *self,
	const gchar  *kind,
	JsonNode     *request,
	JsonObject   *payload,
	gboolean     *handled
){
	g_autoptr(JsonBuilder) builder = json_builder_new();
	g_autoptr(GError) error = NULL;

	*handled = TRUE;

	if (g_strcmp0(kind, "skill.proposals") == 0) {
		gboolean pending = clawt_ipc_payload_boolean(payload, "pending", TRUE);
		g_autoptr(GPtrArray) proposals = NULL;
		guint i;

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "proposals");
		json_builder_begin_array(builder);

		if (self->skill_proposals != NULL) {
			proposals = clawt_skill_proposals_list(self->skill_proposals, pending);

			for (i = 0; i < proposals->len; i++)
				add_proposal_object(self, builder, g_ptr_array_index(proposals, i));
		}

		json_builder_end_array(builder);

		/* An empty list says which of its causes it has. */
		json_builder_set_member_name(builder, "max_per_day");
		json_builder_add_int_value(
			builder, self->config != NULL
			         ? clawt_config_get_int(self->config, "skills.max_proposals_per_day")
			         : 0);
		json_builder_set_member_name(builder, "skills_enabled");
		json_builder_add_boolean_value(builder, self->skills != NULL);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	if (g_strcmp0(kind, "skill.proposal_resolve") == 0) {
		const gchar *id = clawt_ipc_payload_string(payload, "proposal");
		const gchar *action = clawt_ipc_payload_string(payload, "action");
		g_autoptr(ClawtSkillProposal) settled = NULL;

		if (action == NULL || (g_strcmp0(action, "approve") != 0 &&
		                       g_strcmp0(action, "reject") != 0 &&
		                       g_strcmp0(action, "edit") != 0))
			return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
			                           "action is approve, reject or edit");

		settled = clawt_daemon_skill_proposal_settle(
			self, id, action,
			clawt_ipc_payload_string(payload, "note"),
			clawt_ipc_payload_string(payload, "description"),
			clawt_ipc_payload_string(payload, "body"),
			TRUE, &error);

		if (settled == NULL)
			return clawt_ipc_error_new(request, error->code, error->message);

		json_builder_begin_object(builder);
		json_builder_set_member_name(builder, "proposal");
		add_proposal_object(self, builder, settled);
		json_builder_end_object(builder);

		return clawt_ipc_response_new(request, json_builder_get_root(builder));
	}

	*handled = FALSE;

	return NULL;
}
