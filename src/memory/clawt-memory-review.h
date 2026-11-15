/*
 * clawt-memory-review.h - A small model proposes, a person disposes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent's memories pile up: two that say the same thing, one that
 * stopped being true, one that deserves a pin.  With
 * `memories.review.model` set, the daemon periodically shows a small
 * model one agent's own memories and asks what should be merged,
 * forgotten, pinned or unpinned.  Nothing it says is applied.  Each
 * suggestion is kept as a #ClawtMemoryProposal and listed in every
 * client's Memory review group, and only a person accepting one changes
 * the store -- through the store's own journaled functions, so an
 * accepted proposal is undone like any other change.
 *
 * This file is the part that can be tested without a daemon: the
 * prompt (memories fenced as data and escaped), the reading of the
 * reply (anything that is not exactly the shape asked for proposes
 * nothing), the asynchronous call with its hard limit, keeping the
 * proposals on disk, and applying one.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <gio/gio.h>
#include <ai-glib.h>

#include "memory/clawt-memory.h"
#include "memory/clawt-memory-store.h"

G_BEGIN_DECLS

/**
 * CLAWT_MEMORY_REVIEW_TIMEOUT_MS:
 *
 * How long one pass's model has before the pass proposes nothing.
 */
#define CLAWT_MEMORY_REVIEW_TIMEOUT_MS (60000)

/**
 * CLAWT_MEMORY_REVIEW_MAX_PROPOSALS:
 *
 * The most proposals one pass keeps; the rest of a longer reply is
 * dropped.  A list nobody will read to the end is not a review.
 */
#define CLAWT_MEMORY_REVIEW_MAX_PROPOSALS (20)

/**
 * CLAWT_MEMORY_REVIEW_MAX_CONTENT:
 *
 * The longest merged text accepted from the model, in characters.
 */
#define CLAWT_MEMORY_REVIEW_MAX_CONTENT (4000)

/**
 * ClawtMemoryProposalKind:
 * @CLAWT_MEMORY_PROPOSAL_MERGE: replace two or more memories with one
 *   new memory carrying the model's merged text
 * @CLAWT_MEMORY_PROPOSAL_FORGET: archive one memory
 * @CLAWT_MEMORY_PROPOSAL_PIN: pin one memory
 * @CLAWT_MEMORY_PROPOSAL_UNPIN: unpin one memory
 *
 * What a review suggests doing to an agent's memories.
 */
typedef enum {
	CLAWT_MEMORY_PROPOSAL_MERGE = 0,
	CLAWT_MEMORY_PROPOSAL_FORGET,
	CLAWT_MEMORY_PROPOSAL_PIN,
	CLAWT_MEMORY_PROPOSAL_UNPIN
} ClawtMemoryProposalKind;

GType
clawt_memory_proposal_kind_get_type(
	void
) G_GNUC_CONST;
#define CLAWT_TYPE_MEMORY_PROPOSAL_KIND (clawt_memory_proposal_kind_get_type())

/**
 * clawt_memory_proposal_kind_label:
 * @kind: a kind
 *
 * What the three clients call a kind of proposal: "Merge", "Forget",
 * "Pin", "Unpin".
 *
 * Returns: (transfer none): the label
 */
const gchar *
clawt_memory_proposal_kind_label(
	ClawtMemoryProposalKind kind
);

/**
 * ClawtMemoryProposal:
 * @id: the proposal's own id, `p` and sixteen hex digits
 * @kind: what it suggests
 * @memory_ids: (array zero-terminated=1): the memories it is about; two
 *   or more for a merge, exactly one otherwise
 * @content: (nullable): the merged text, for a merge only
 * @reason: (nullable): why, in the model's words, clipped to one line
 * @created_at: unix seconds the pass that made it finished
 *
 * One suggestion from a review, waiting for a person.
 */
typedef struct {
	gchar                    *id;
	ClawtMemoryProposalKind   kind;
	GStrv                     memory_ids;
	gchar                    *content;
	gchar                    *reason;
	gint64                    created_at;
} ClawtMemoryProposal;

GType
clawt_memory_proposal_get_type(
	void
) G_GNUC_CONST;
#define CLAWT_TYPE_MEMORY_PROPOSAL (clawt_memory_proposal_get_type())

/**
 * clawt_memory_proposal_copy:
 * @self: a proposal
 *
 * Returns: (transfer full): a deep copy
 */
ClawtMemoryProposal *
clawt_memory_proposal_copy(
	const ClawtMemoryProposal *self
);

/**
 * clawt_memory_proposal_free:
 * @self: (nullable): a proposal
 */
void
clawt_memory_proposal_free(
	ClawtMemoryProposal *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMemoryProposal, clawt_memory_proposal_free)

/**
 * clawt_memory_proposal_id_is_valid:
 * @id: (nullable): a proposal id from a client
 *
 * Returns: %TRUE when @id has the shape a review gives its proposals
 */
gboolean
clawt_memory_proposal_id_is_valid(
	const gchar *id
);

/**
 * clawt_memory_proposal_describe:
 * @proposal: a proposal
 *
 * The one line the three clients draw for a proposal: "Forget m01...",
 * "Merge 3 memories into one".  Model text inside it is clipped and on
 * one line.
 *
 * Returns: (transfer full): the line
 */
gchar *
clawt_memory_proposal_describe(
	const ClawtMemoryProposal *proposal
);

/**
 * clawt_memory_review_system_prompt:
 *
 * The instruction the model is given: the four kinds, the reply shape,
 * and that everything inside a fence is data.
 *
 * Returns: (transfer none): the system prompt
 */
const gchar *
clawt_memory_review_system_prompt(
	void
);

/**
 * clawt_memory_review_prompt:
 * @memories: (element-type ClawtMemory): the memories to review
 *
 * The user message: each memory's id, flags and text, the text escaped
 * with clawt_preamble_escape_all() and inside a fence longer than any
 * run of backticks in it, so no memory can close its fence and speak to
 * the model as an instruction.
 *
 * Returns: (transfer full): the prompt
 */
gchar *
clawt_memory_review_prompt(
	GPtrArray *memories
);

/**
 * clawt_memory_review_parse:
 * @reply: (nullable): what the model wrote
 * @memories: (element-type ClawtMemory): the memories it was shown
 * @now: unix seconds, stamped on each proposal
 * @error: (out) (optional): why the reply was not read
 *
 * Reads the reply: a JSON object with a `proposals` array, optionally
 * inside one code fence.  Each entry needs a known `kind` and `ids` that
 * all name memories it was shown; a merge needs two or more ids and a
 * `content`; a pin of a pinned memory or an unpin of an unpinned one is
 * dropped as saying nothing.  An entry that breaks a rule is skipped;
 * a reply that is not that shape at all is an error -- "it did not say"
 * is not "it proposed nothing".
 *
 * Returns: (transfer full) (element-type ClawtMemoryProposal) (nullable):
 *   the proposals, possibly empty, or %NULL with @error set
 */
GPtrArray *
clawt_memory_review_parse(
	const gchar  *reply,
	GPtrArray    *memories,
	gint64        now,
	GError      **error
);

/**
 * clawt_memory_review_run_async: (skip)
 * @provider: the model to ask
 * @memories: (element-type ClawtMemory): what to review; copied
 * @timeout_ms: the hard limit, or 0 for %CLAWT_MEMORY_REVIEW_TIMEOUT_MS
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the proposals
 * @user_data: data for @callback
 *
 * Asks @provider once, without blocking.  The task and its timer attach
 * to the thread-default context at the time of the call, which the
 * daemon pushes.  At @timeout_ms the request is cancelled and the pass
 * fails with %CLAWT_ERROR_TIMEOUT; a reply after that is discarded.
 *
 * Skipped for introspection: ai-glib ships no GIR.
 */
void
clawt_memory_review_run_async(
	AiProvider          *provider,
	GPtrArray           *memories,
	guint                timeout_ms,
	GCancellable        *cancellable,
	GAsyncReadyCallback  callback,
	gpointer             user_data
);

/**
 * clawt_memory_review_run_finish: (skip)
 * @result: the #GAsyncResult
 * @error: (out) (optional): why the pass proposed nothing
 *
 * Returns: (transfer full) (element-type ClawtMemoryProposal) (nullable):
 *   the proposals, or %NULL with @error set on a failure, a timeout or a
 *   reply that could not be read
 */
GPtrArray *
clawt_memory_review_run_finish(
	GAsyncResult  *result,
	GError       **error
);

/**
 * clawt_memory_proposals_load:
 * @dir: `<state_dir>/memory-review`
 * @agent_id: whose
 * @last_run_at: (out) (optional): unix seconds of the last pass, 0 never
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (element-type ClawtMemoryProposal) (nullable):
 *   the open proposals, empty when there is no file, %NULL on an error
 */
GPtrArray *
clawt_memory_proposals_load(
	const gchar  *dir,
	const gchar  *agent_id,
	gint64       *last_run_at,
	GError      **error
);

/**
 * clawt_memory_proposals_save:
 * @dir: `<state_dir>/memory-review`, made 0700 if missing
 * @agent_id: whose
 * @proposals: (element-type ClawtMemoryProposal): the open proposals
 * @last_run_at: unix seconds of the last pass
 * @error: (out) (optional): return location for a #GError
 *
 * Writes `<dir>/<agent>.json` at 0600: memory text is in it.
 *
 * Returns: %TRUE on success
 */
gboolean
clawt_memory_proposals_save(
	const gchar  *dir,
	const gchar  *agent_id,
	GPtrArray    *proposals,
	gint64        last_run_at,
	GError      **error
);

/**
 * clawt_memory_proposal_apply:
 * @proposal: what was accepted
 * @store: the agent's own memory store
 * @actor: (nullable): who accepted it, for the journal
 * @error: (out) (optional): return location for a #GError
 *
 * Applies one accepted proposal through the journaled store: a forget is
 * clawt_memory_store_forget_as(), a pin or unpin
 * clawt_memory_store_pin_as(), and a merge is an add of the merged text
 * (source `review:<proposal id>`, the first memory's category, the
 * highest importance, pinned when any was) followed by a forget of each
 * memory it replaces -- each its own journal row, so each can be undone.
 *
 * Every memory it names must still be there and not forgotten, or
 * nothing is changed: a proposal about a memory that has moved on since
 * is refused rather than applied to whatever is there now.
 *
 * Returns: %TRUE when applied
 */
gboolean
clawt_memory_proposal_apply(
	const ClawtMemoryProposal  *proposal,
	ClawtMemoryStore           *store,
	const gchar                *actor,
	GError                    **error
);

G_END_DECLS
