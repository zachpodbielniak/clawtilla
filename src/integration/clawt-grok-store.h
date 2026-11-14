/*
 * clawt-grok-store.h - The Grok jobs that are owed an answer, on disk
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A job is a question asked of somebody else's machine, answered minutes
 * or hours later into a listener that has to still know what it asked.
 * Held in memory it would be the shape AGENTS.md calls "a stored
 * question with an in-memory asker": a daemon restarted between the POST
 * and the callback would answer the bot with a 404 and the agent with
 * silence.  So every job is a row, its token a digest, and the expiry
 * sweep reads the clock off the rows rather than off timers it lost.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "integration/clawt-grok.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_GROK_STORE (clawt_grok_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtGrokStore, clawt_grok_store, CLAWT, GROK_STORE,
                     GObject)

/**
 * clawt_grok_store_new:
 * @path: the database file, created 0600 if absent
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer full) (nullable): the store
 */
ClawtGrokStore *clawt_grok_store_new(const gchar *path, GError **error);

/**
 * clawt_grok_store_add:
 * @self: a #ClawtGrokStore
 * @job: the job, with every field it will be known by
 * @token_digest: clawt_grok_token_digest() of its callback token
 * @error: (out) (optional): return location for a #GError
 *
 * Written before the POST is sent, so a bot that answers faster than the
 * POST's own reply arrives finds its job already here.
 *
 * Returns: %TRUE if it was written
 */
gboolean clawt_grok_store_add(ClawtGrokStore  *self,
                              ClawtGrokJob    *job,
                              const gchar     *token_digest,
                              GError         **error);

/**
 * clawt_grok_store_get:
 * @self: a #ClawtGrokStore
 * @id: the job
 *
 * Returns: (transfer full) (nullable): the job, or %NULL if there is none
 */
ClawtGrokJob *clawt_grok_store_get(ClawtGrokStore *self, const gchar *id);

/**
 * clawt_grok_store_check_token:
 * @self: a #ClawtGrokStore
 * @id: the job
 * @presented: (nullable): the token the caller sent
 *
 * Returns: %TRUE when @presented is this job's token
 */
gboolean clawt_grok_store_check_token(ClawtGrokStore *self,
                                      const gchar    *id,
                                      const gchar    *presented);

/**
 * clawt_grok_store_update:
 * @self: a #ClawtGrokStore
 * @job: the job, as it now is
 * @error: (out) (optional): return location for a #GError
 *
 * Writes state, run id, result, note and updated time.  The identity
 * fields -- bot, agent, room, task, token -- are fixed at add.
 *
 * Returns: %TRUE if the row was changed
 */
gboolean clawt_grok_store_update(ClawtGrokStore  *self,
                                 ClawtGrokJob    *job,
                                 GError         **error);

/**
 * clawt_grok_store_settle:
 * @self: a #ClawtGrokStore
 * @id: the job
 * @state: a settled state
 * @text: (nullable): the result, the question, or why
 * @now: microseconds
 * @error: (out) (optional): return location for a #GError
 *
 * Settles the job if, and only if, it is not settled already -- one
 * statement, so two reports racing (the HTTP route and the MCP tool, or
 * a report and the expiry sweep) cannot both win.
 *
 * Returns: (transfer full) (nullable): the job as settled, or %NULL when
 *   it was already settled or does not exist (@error says which)
 */
ClawtGrokJob *clawt_grok_store_settle(ClawtGrokStore     *self,
                                      const gchar        *id,
                                      ClawtGrokJobState   state,
                                      const gchar        *text,
                                      gint64              now,
                                      GError            **error);

/**
 * clawt_grok_store_list:
 * @self: a #ClawtGrokStore
 * @bot: (nullable): one bot's jobs, or every bot's
 * @agent_id: (nullable): one agent's jobs, or every agent's
 * @open_only: only jobs not yet settled
 * @limit: how many at most, newest first
 *
 * Returns: (transfer full) (element-type ClawtGrokJob): the jobs
 */
GPtrArray *clawt_grok_store_list(ClawtGrokStore *self,
                                 const gchar    *bot,
                                 const gchar    *agent_id,
                                 gboolean        open_only,
                                 guint           limit);

/**
 * clawt_grok_store_due:
 * @self: a #ClawtGrokStore
 * @now: microseconds
 *
 * The open jobs whose deadline has passed.
 *
 * Returns: (transfer full) (element-type ClawtGrokJob): the jobs
 */
GPtrArray *clawt_grok_store_due(ClawtGrokStore *self, gint64 now);

/**
 * clawt_grok_store_prune:
 * @self: a #ClawtGrokStore
 * @before: microseconds; settled jobs last changed before it go
 *
 * Returns: how many rows were removed
 */
guint clawt_grok_store_prune(ClawtGrokStore *self, gint64 before);

G_END_DECLS
