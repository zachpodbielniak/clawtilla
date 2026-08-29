/*
 * clawt-summariser.h - Turning finished work into memories
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "config/clawt-config.h"
#include "memory/clawt-memory-store.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_SUMMARISER (clawt_summariser_get_type())

G_DECLARE_FINAL_TYPE(ClawtSummariser, clawt_summariser, CLAWT, SUMMARISER,
                     GObject)

/**
 * clawt_summariser_new:
 * @config: (transfer none): the fleet configuration
 *
 * Distils a finished piece of work into memories.
 *
 * The model is given exactly one tool, `remember`, and no others: its
 * output is a set of memories or nothing.  The executor is built with
 * ai_tool_executor_new_empty(), never ai_tool_executor_new(), which
 * grants `bash`, `read`, `write` and `edit` and cannot take them back --
 * a summariser is a thing that runs unattended after work finishes, and
 * it must not be able to do any.
 *
 * Returns: (transfer full): a new #ClawtSummariser
 */
ClawtSummariser *clawt_summariser_new(ClawtConfig *config);

/**
 * clawt_summariser_set_provider: (skip)
 * @self: a #ClawtSummariser
 * @provider: (transfer none): the AI provider to summarise with
 *
 * Supplying the provider rather than building one inside is what lets a
 * test drive this with AiMockProvider through the same code path.
 *
 * Skipped for introspection: ai-glib ships no GIR, so #AiProvider has no
 * name a binding could resolve.
 */
void clawt_summariser_set_provider(ClawtSummariser *self,
                                   AiProvider      *provider);

/**
 * clawt_summariser_use_configured_provider:
 * @self: a #ClawtSummariser
 * @error: (out) (optional): return location for a #GError
 *
 * Builds the provider named by `ai_assist.provider` and `ai_assist.model`.
 *
 * The same provider the agent designer uses, because summarising is the
 * same shape of work: a short tool-driven exchange with a model that is
 * not one of the fleet's agents.  A CLI backend cannot do it -- ai-glib's
 * CLI clients drop the tool list.
 *
 * Returns: %TRUE if a provider was built
 */
gboolean clawt_summariser_use_configured_provider(ClawtSummariser  *self,
                                                  GError          **error);

/**
 * clawt_summariser_set_budget_bytes:
 * @self: a #ClawtSummariser
 * @bytes: how much transcript one summary may read, 0 for the default
 *
 * Caps what one summary costs.
 *
 * A transcript has no upper bound and a summary is a model call nobody
 * asked for, so the input is cut to the most recent @bytes rather than
 * sent whole.  The cut is made on a character boundary -- see
 * clawt_utf8_truncate() -- because slicing a multi-byte sequence in half
 * hands the model a replacement character in the middle of a word and
 * makes it look like the transcript was corrupt.
 */
void clawt_summariser_set_budget_bytes(ClawtSummariser *self, guint bytes);

/**
 * clawt_summariser_get_budget_bytes:
 * @self: a #ClawtSummariser
 *
 * Returns: how much transcript one summary may read
 */
guint clawt_summariser_get_budget_bytes(ClawtSummariser *self);

/**
 * clawt_summariser_summarise:
 * @self: a #ClawtSummariser
 * @into: (transfer none): where the memories are written
 * @source: what this work was -- `task:<id>`, `routine:<id>`
 * @transcript: the conversation to distil
 * @from_at: unix seconds of the first message covered, 0 if unknown
 * @to_at: unix seconds of the last, 0 if unknown
 * @cancellable: (nullable): a #GCancellable
 * @error: (out) (optional): return location for a #GError
 *
 * Reads @transcript and writes what is worth keeping into @into.
 *
 * Every memory it writes carries @source and the range it covers, so a
 * memory that turns out to be wrong can be traced to the conversation
 * that produced it.  A model that calls nothing writes nothing and is
 * not an error: there is often nothing in a piece of work worth
 * remembering, and inventing a memory to have produced one is worse than
 * producing none.
 *
 * Returns: how many memories were written
 */
guint clawt_summariser_summarise(ClawtSummariser   *self,
                                 ClawtMemoryStore  *into,
                                 const gchar       *source,
                                 const gchar       *transcript,
                                 gint64             from_at,
                                 gint64             to_at,
                                 GCancellable      *cancellable,
                                 GError           **error);

/**
 * clawt_summariser_set_main_context:
 * @self: a #ClawtSummariser
 * @context: (nullable): the context an answer must arrive on
 *
 * Names the context clawt_summariser_summarise_async() calls back on.
 *
 * Not optional for an embedded daemon: it runs its own loop on its own
 * context, and a #GTask that took the global default would call back
 * onto a loop nobody iterates -- the model call would run, cost money,
 * and its memories would never be written.
 */
void clawt_summariser_set_main_context(ClawtSummariser *self,
                                       GMainContext    *context);

/**
 * clawt_summariser_summarise_async:
 * @self: a #ClawtSummariser
 * @into: (transfer none): where the memories are written
 * @source: what this work was -- `task:<id>`, `routine:<id>`
 * @transcript: the conversation to distil
 * @from_at: unix seconds of the first message covered, 0 if unknown
 * @to_at: unix seconds of the last, 0 if unknown
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called when it finishes
 * @user_data: data for @callback
 *
 * Summarises without blocking the caller's loop.
 *
 * The model call is an HTTP round trip, and the thing that triggers a
 * summary is a task finishing -- which is a signal handler on the
 * daemon's main context.  Run synchronously there it would stop the
 * whole fleet answering for the length of a completion.
 *
 * Only the model call leaves the thread.  The memories are written in
 * clawt_summariser_summarise_finish(), on the context this was called
 * from, because that is the thread every other write to the store
 * happens on.
 *
 * One summary at a time.  A second call while one is running fails,
 * saying so, rather than queueing: the per-summary state is on the
 * instance, and two overlapping runs would file each other's memories
 * under each other's transcripts.  Skipping a summary costs a fleet one
 * summary; interleaving two costs it the provenance of both.
 */
void clawt_summariser_summarise_async(ClawtSummariser     *self,
                                      ClawtMemoryStore    *into,
                                      const gchar         *source,
                                      const gchar         *transcript,
                                      gint64               from_at,
                                      gint64               to_at,
                                      GCancellable        *cancellable,
                                      GAsyncReadyCallback  callback,
                                      gpointer             user_data);

/**
 * clawt_summariser_summarise_finish:
 * @self: a #ClawtSummariser
 * @result: the #GAsyncResult
 * @error: (out) (optional): return location for a #GError
 *
 * Writes what the model produced and says how much of it landed.
 *
 * Returns: how many memories were written
 */
guint clawt_summariser_summarise_finish(ClawtSummariser  *self,
                                        GAsyncResult     *result,
                                        GError          **error);

/**
 * clawt_summariser_nudge_text:
 * @nudge_turns: the cadence from `memories.nudge_turns`, 0 for off
 *
 * The reminder to record what was learned, as it appears in an agent's
 * per-turn prompt suffix.
 *
 * It rides `prompt_suffix` rather than being a mechanism of its own, so
 * it is visible in the same place as every other standing instruction --
 * `clawtilla config render` shows it, and an operator wondering where a
 * line in the prompt came from finds it beside the computer directive
 * rather than nowhere.
 *
 * The suffix reaches every turn, so the cadence is what the reminder
 * *asks for* rather than something clawtilla counts: a nudge that fired
 * from the daemon would need a per-turn channel to the agent, and the
 * only one there is is the delivery preamble, which not every turn has.
 *
 * Returns: (transfer full) (nullable): the reminder, or %NULL when
 *   @nudge_turns is 0
 */
gchar *clawt_summariser_nudge_text(guint nudge_turns);

G_END_DECLS
