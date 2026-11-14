/*
 * clawt-skill-proposal.h - A skill an agent wrote, waiting for a person
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent that explains a technique back to its operator has written a
 * skill and handed it to nobody.  `clawtilla_skill_propose` gives that
 * text somewhere to go: a draft, through the same write path a taught
 * skill takes, with a decision in the operator's inbox to approve it,
 * reject it or edit it first.
 *
 * Nothing here trusts the agent.  The draft lands disabled; an update is
 * staged beside the library and never written over the live skill until
 * a person approves it; approval is refused when the bytes on disk are
 * no longer the bytes that were reviewed; and every piece of text the
 * agent wrote is escaped and fenced before anybody is shown it.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "skill/clawt-skill-library.h"

G_BEGIN_DECLS

/**
 * CLAWT_SKILL_PROPOSAL_ID_PREFIX:
 *
 * What every proposal id -- and so the id of the decision raised for it
 * -- begins with.  A client recognises a skill proposal in the decision
 * inbox by this, through clawt_skill_proposal_id_is_valid(), rather than
 * by a second copy of the rule.
 */
#define CLAWT_SKILL_PROPOSAL_ID_PREFIX "skill-proposal-"

/**
 * CLAWT_SKILL_PROPOSAL_MAX_BODY:
 *
 * The largest body, in bytes, an agent may propose.
 *
 * A skill is read into a model's context whenever it is used, and a
 * person has to read all of it before approving it.  Sixty-four
 * kilobytes is far past any procedure worth writing down, and it keeps
 * one proposal from being a document nobody can review.
 */
#define CLAWT_SKILL_PROPOSAL_MAX_BODY (65536)

/**
 * CLAWT_SKILL_PROPOSAL_MAX_REASON:
 *
 * The longest reason, in bytes, a proposal may carry.
 */
#define CLAWT_SKILL_PROPOSAL_MAX_REASON (2000)

/**
 * ClawtSkillProposalState:
 * @CLAWT_SKILL_PROPOSAL_PENDING: waiting on a person
 * @CLAWT_SKILL_PROPOSAL_APPROVED: a person approved the agent's text
 * @CLAWT_SKILL_PROPOSAL_EDITED: a person changed the text and then
 *   approved their version
 * @CLAWT_SKILL_PROPOSAL_REJECTED: discarded; the library is as it was
 *
 * Approved and edited are different states because they say different
 * things to the agent: an edited skill is not the one it wrote, and an
 * agent told only "approved" would go on believing its own wording.
 */
typedef enum {
	CLAWT_SKILL_PROPOSAL_PENDING = 0,
	CLAWT_SKILL_PROPOSAL_APPROVED,
	CLAWT_SKILL_PROPOSAL_EDITED,
	CLAWT_SKILL_PROPOSAL_REJECTED
} ClawtSkillProposalState;

/**
 * clawt_skill_proposal_state_to_string:
 * @state: a #ClawtSkillProposalState
 *
 * Returns: (transfer none): `pending`, `approved`, `edited` or `rejected`
 */
const gchar *
clawt_skill_proposal_state_to_string(
	ClawtSkillProposalState state
);

#define CLAWT_TYPE_SKILL_PROPOSAL (clawt_skill_proposal_get_type())

typedef struct _ClawtSkillProposal ClawtSkillProposal;

GType clawt_skill_proposal_get_type(void) G_GNUC_CONST;

/**
 * clawt_skill_proposal_copy:
 * @self: a #ClawtSkillProposal
 *
 * Returns: (transfer full): a copy
 */
ClawtSkillProposal *
clawt_skill_proposal_copy(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_free:
 * @self: (nullable): a #ClawtSkillProposal
 */
void
clawt_skill_proposal_free(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_get_id:
 * @self: a #ClawtSkillProposal
 *
 * Returns: (transfer none): the id, which is also the decision's id
 */
const gchar *
clawt_skill_proposal_get_id(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_get_agent:
 * @self: a #ClawtSkillProposal
 *
 * Returns: (transfer none): the agent that proposed it
 */
const gchar *
clawt_skill_proposal_get_agent(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_get_name:
 * @self: a #ClawtSkillProposal
 *
 * Returns: (transfer none): the skill's name, already through the
 *   traversal gate
 */
const gchar *
clawt_skill_proposal_get_name(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_is_update:
 * @self: a #ClawtSkillProposal
 *
 * Returns: %TRUE for a change to a skill that exists, %FALSE for a new one
 */
gboolean
clawt_skill_proposal_is_update(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_get_reason:
 * @self: a #ClawtSkillProposal
 *
 * Returns: (transfer none) (nullable): why, in the agent's own words --
 *   raw, so escape it before showing it to anybody
 */
const gchar *
clawt_skill_proposal_get_reason(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_get_created_at:
 * @self: a #ClawtSkillProposal
 *
 * Returns: when it was proposed, in microseconds since the epoch -- the
 *   unit clawt_time_ago_label() takes
 */
gint64
clawt_skill_proposal_get_created_at(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_get_settled_at:
 * @self: a #ClawtSkillProposal
 *
 * Returns: when a person settled it, in microseconds, or 0 while pending
 */
gint64
clawt_skill_proposal_get_settled_at(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_get_state:
 * @self: a #ClawtSkillProposal
 *
 * Returns: where it stands
 */
ClawtSkillProposalState
clawt_skill_proposal_get_state(
	ClawtSkillProposal *self
);

/**
 * clawt_skill_proposal_id_is_valid:
 * @id: (nullable): a candidate id
 *
 * Whether @id is a proposal id: %CLAWT_SKILL_PROPOSAL_ID_PREFIX and then
 * a lowercase UUID, nothing else.
 *
 * **This is the traversal gate for the proposal store**, the way
 * clawt_skill_name_is_valid() is for the library: an id is joined onto
 * a directory, and it arrives from a client.  It is also how a client
 * tells a skill proposal from any other decision.
 *
 * Returns: %TRUE if @id names a proposal
 */
gboolean
clawt_skill_proposal_id_is_valid(
	const gchar *id
);

/**
 * clawt_skill_proposal_fence:
 * @text: (nullable): untrusted text
 * @info: (nullable): the fence's info string, such as `markdown` or `diff`
 *
 * @text through clawt_preamble_escape(), inside a code fence no line of
 * it can close.
 *
 * The fence is one backtick longer than the longest run of backticks in
 * the text, and never shorter than three.  A fixed three-backtick fence
 * is closed by the first line of the proposal that holds three backticks,
 * and everything after that line reads as the daemon's own words again --
 * which is precisely the text an agent would put there.
 *
 * Returns: (transfer full): the fenced text
 */
gchar *
clawt_skill_proposal_fence(
	const gchar *text,
	const gchar *info
);

/**
 * clawt_skill_proposal_diff_text:
 * @before: (nullable): the old text
 * @after: (nullable): the new text
 * @before_label: (nullable): the name of the old side, for the header
 * @after_label: (nullable): the name of the new side
 *
 * A unified diff of two texts, line by line, with three lines of context.
 *
 * Exact for anything a skill is: the common head and tail are trimmed
 * first and the middle is a longest-common-subsequence match.  A middle
 * too large to match within a fixed budget is shown as removed and
 * re-added whole -- still correct, and never a hang on a hostile input.
 *
 * Returns: (transfer full): the diff, empty when the texts are equal
 */
gchar *
clawt_skill_proposal_diff_text(
	const gchar *before,
	const gchar *after,
	const gchar *before_label,
	const gchar *after_label
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtSkillProposal, clawt_skill_proposal_free)

#define CLAWT_TYPE_SKILL_PROPOSALS (clawt_skill_proposals_get_type())

G_DECLARE_FINAL_TYPE(ClawtSkillProposals, clawt_skill_proposals,
                     CLAWT, SKILL_PROPOSALS, GObject)

/**
 * clawt_skill_proposals_new:
 * @directory: where proposals are kept, under the daemon's state
 *
 * A store over @directory, read now.  Settled proposals older than a week
 * are forgotten as the store is read; pending ones are kept however old
 * they are, because a draft nobody has looked at is still waiting.
 *
 * Under the daemon's state and never under `skills.dir`: that directory
 * is the person's library, and an update staged in it would be a skill
 * the next scan picked up.
 *
 * Returns: (transfer full): the store
 */
ClawtSkillProposals *
clawt_skill_proposals_new(
	const gchar *directory
);

/**
 * clawt_skill_proposals_get_directory:
 * @self: a #ClawtSkillProposals
 *
 * Returns: (transfer none): where the proposals are kept
 */
const gchar *
clawt_skill_proposals_get_directory(
	ClawtSkillProposals *self
);

/**
 * clawt_skill_proposals_propose:
 * @self: a #ClawtSkillProposals
 * @library: the fleet's skills
 * @agent: who is proposing it
 * @name: the skill's name; for an update, the skill that exists
 * @description: when an agent should reach for it
 * @body: the markdown
 * @is_update: %TRUE to change the skill called @name, %FALSE for a new one
 * @reason: (nullable): why, in the agent's words; required for an update
 * @max_per_day: how many proposals @agent may make in any 24 hours; 0
 *   refuses them all
 * @error: (out) (optional): return location for a #GError
 *
 * Validates a proposal and writes its draft.
 *
 * A new skill is written through clawt_skill_library_create_taught() --
 * teach's own commit, so the same name rules, description bound,
 * traversal gate and scan -- and lands **disabled**, with its origin
 * naming the proposal.  An update is validated by the same parse and
 * staged under @self; the live skill is not touched until
 * clawt_skill_proposals_approve().
 *
 * Refused: a name that is not a skill name; an empty description or
 * body; a body over %CLAWT_SKILL_PROPOSAL_MAX_BODY or a reason over
 * %CLAWT_SKILL_PROPOSAL_MAX_REASON; text that is not UTF-8; an update to
 * a skill that does not exist, is not in the library's own directory, or
 * already says exactly this; a second pending proposal for the same
 * skill; and a proposal past @max_per_day.
 *
 * Returns: (transfer full) (nullable): the proposal, or %NULL with @error
 */
ClawtSkillProposal *
clawt_skill_proposals_propose(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	const gchar          *agent,
	const gchar          *name,
	const gchar          *description,
	const gchar          *body,
	gboolean              is_update,
	const gchar          *reason,
	guint                 max_per_day,
	GError              **error
);

/**
 * clawt_skill_proposals_get:
 * @self: a #ClawtSkillProposals
 * @id: a proposal id
 *
 * Returns: (transfer full) (nullable): the proposal, or %NULL
 */
ClawtSkillProposal *
clawt_skill_proposals_get(
	ClawtSkillProposals *self,
	const gchar         *id
);

/**
 * clawt_skill_proposals_list:
 * @self: a #ClawtSkillProposals
 * @pending_only: %TRUE for the ones still waiting on somebody
 *
 * Oldest first, since a queue is read from the end that has waited
 * longest.
 *
 * Returns: (transfer full) (element-type ClawtSkillProposal): the proposals
 */
GPtrArray *
clawt_skill_proposals_list(
	ClawtSkillProposals *self,
	gboolean             pending_only
);

/**
 * clawt_skill_proposals_count_recent:
 * @self: a #ClawtSkillProposals
 * @agent: an agent id
 * @now: the current time, in microseconds
 *
 * How many proposals @agent made in the 24 hours before @now, settled or
 * not.  A rejected proposal still spent the operator's attention, so it
 * still counts.
 *
 * Returns: the count
 */
guint
clawt_skill_proposals_count_recent(
	ClawtSkillProposals *self,
	const gchar         *agent,
	gint64               now
);

/**
 * clawt_skill_proposals_get_draft:
 * @self: a #ClawtSkillProposals
 * @id: a proposal id
 *
 * The `SKILL.md` the agent proposed, exactly as it was when proposed --
 * raw, so escape it before showing it.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL
 */
gchar *
clawt_skill_proposals_get_draft(
	ClawtSkillProposals *self,
	const gchar         *id
);

/**
 * clawt_skill_proposals_get_diff:
 * @self: a #ClawtSkillProposals
 * @library: the fleet's skills
 * @id: a proposal id
 *
 * For an update, the change against the live skill as it is *now* -- so
 * a skill edited since the proposal shows what approving would really
 * do.  For a new skill, %NULL.  Raw: escape it before showing it.
 *
 * Returns: (transfer full) (nullable): the diff
 */
gchar *
clawt_skill_proposals_get_diff(
	ClawtSkillProposals *self,
	ClawtSkillLibrary   *library,
	const gchar         *id
);

/**
 * clawt_skill_proposals_is_stale:
 * @self: a #ClawtSkillProposals
 * @library: the fleet's skills
 * @id: a proposal id
 *
 * Whether approving would be refused because the file on disk has moved
 * since the proposal: the live skill changed under an update, or the
 * draft of a new one was rewritten after it was reviewed.
 *
 * Returns: %TRUE when approval would be refused for that reason
 */
gboolean
clawt_skill_proposals_is_stale(
	ClawtSkillProposals *self,
	ClawtSkillLibrary   *library,
	const gchar         *id
);

/**
 * clawt_skill_proposals_describe:
 * @self: a #ClawtSkillProposals
 * @library: the fleet's skills
 * @id: a proposal id
 *
 * The decision's question: one headline line naming the agent and the
 * skill, then the agent's reason and the proposed text -- the whole
 * `SKILL.md` for a new skill, a diff for an update -- each escaped and
 * fenced, then what the scan noticed and what each answer does.
 *
 * The headline is the only line built from nothing the agent chose
 * freely (an agent id and a validated skill name), so a client may show
 * it as a title.
 *
 * Returns: (transfer full) (nullable): the question, or %NULL
 */
gchar *
clawt_skill_proposals_describe(
	ClawtSkillProposals *self,
	ClawtSkillLibrary   *library,
	const gchar         *id
);

/**
 * clawt_skill_proposals_approve:
 * @self: a #ClawtSkillProposals
 * @library: the fleet's skills
 * @id: a proposal id
 * @error: (out) (optional): return location for a #GError
 *
 * Enables a new skill, or writes an update over the live one.
 *
 * Refused, and left pending, when the bytes on disk are not the bytes
 * that were reviewed: a draft rewritten since it was proposed, or a live
 * skill that changed under the update.  Approving text somebody has not
 * seen is the one thing this must never do, so the refusal says to read
 * it again -- edit shows what is there now.
 *
 * Returns: (transfer full) (nullable): the settled proposal
 */
ClawtSkillProposal *
clawt_skill_proposals_approve(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	const gchar          *id,
	GError              **error
);

/**
 * clawt_skill_proposals_edit:
 * @self: a #ClawtSkillProposals
 * @library: the fleet's skills
 * @id: a proposal id
 * @description: the description as the person wants it
 * @body: the body as the person wants it
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the person's version and approves it: a new skill is rewritten
 * and enabled, an update replaces the live skill with the person's text.
 * The person wrote what is written, which is the review, so the stale
 * check approve() makes does not apply to the draft -- but an update is
 * still refused when the live skill changed since it was proposed, since
 * the person edited against a skill that is no longer there.
 *
 * Returns: (transfer full) (nullable): the settled proposal
 */
ClawtSkillProposal *
clawt_skill_proposals_edit(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	const gchar          *id,
	const gchar          *description,
	const gchar          *body,
	GError              **error
);

/**
 * clawt_skill_proposals_reject:
 * @self: a #ClawtSkillProposals
 * @library: (nullable): the fleet's skills
 * @id: a proposal id
 * @error: (out) (optional): return location for a #GError
 *
 * Discards the draft.  A new skill's draft is removed from the library
 * -- unless somebody has enabled it from the Skills page in the
 * meantime, which is a person keeping it, and is left alone.  A staged
 * update is simply forgotten; the live skill was never touched.
 *
 * Returns: (transfer full) (nullable): the settled proposal
 */
ClawtSkillProposal *
clawt_skill_proposals_reject(
	ClawtSkillProposals  *self,
	ClawtSkillLibrary    *library,
	const gchar          *id,
	GError              **error
);

G_END_DECLS
