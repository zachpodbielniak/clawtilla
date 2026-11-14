/*
 * clawt-grok.h - Handing work to a Grok bot, and hearing back
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Grok Bot has no API an outside program can drive.  What it has is a
 * routine with a webhook trigger: a POST to the routine's URL, carrying
 * its key as a bearer token, starts a run with the request body as the
 * run's input -- and answers at once with a run id, long before the run
 * has done anything.  So a job here is three things in sequence: the
 * POST, a run nobody can watch, and a callback the bot is asked to make
 * when it is done, to an address minted for that one job.
 *
 * This header is the part of that with no sockets in it: the job record,
 * the envelope the bot is handed, the routine instructions a person
 * pastes into Grok once, the two shapes a report can arrive in, and the
 * rules every client applies to a job's state.  The daemon side --
 * sending, listening, delivering the result back into the agent's room --
 * is in src/core/daemon-grok.c.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

/**
 * CLAWT_GROK_MAX_REPORT_BYTES:
 *
 * The most a bot's report may carry, in bytes.
 *
 * Bounded on the way in by the listener, which refuses a larger body
 * before reading it, and stated here so the envelope can tell the bot.
 */
#define CLAWT_GROK_MAX_REPORT_BYTES (256 * 1024)

/**
 * CLAWT_GROK_MAX_ENVELOPE_BYTES:
 *
 * The most Grok accepts as a webhook body, which the instructions and
 * context together must fit inside.
 */
#define CLAWT_GROK_MAX_ENVELOPE_BYTES (1024 * 1024)

/**
 * ClawtGrokJob:
 *
 * One piece of work handed to a Grok bot.  A boxed, copyable record; the
 * store owns the durable copy.
 */
typedef struct _ClawtGrokJob ClawtGrokJob;

#define CLAWT_TYPE_GROK_JOB (clawt_grok_job_get_type())

GType clawt_grok_job_get_type(void) G_GNUC_CONST;

/**
 * clawt_grok_job_new:
 * @id: the job's id
 * @bot: the bot's name, as the integration instance is called
 *
 * Returns: (transfer full): a job in %CLAWT_GROK_JOB_SENT
 */
ClawtGrokJob *clawt_grok_job_new(const gchar *id, const gchar *bot);

ClawtGrokJob *clawt_grok_job_copy(ClawtGrokJob *self);
void          clawt_grok_job_free(ClawtGrokJob *self);

const gchar      *clawt_grok_job_get_id(ClawtGrokJob *self);
const gchar      *clawt_grok_job_get_bot(ClawtGrokJob *self);
const gchar      *clawt_grok_job_get_agent_id(ClawtGrokJob *self);
const gchar      *clawt_grok_job_get_room_id(ClawtGrokJob *self);
const gchar      *clawt_grok_job_get_task_id(ClawtGrokJob *self);
const gchar      *clawt_grok_job_get_run_uuid(ClawtGrokJob *self);
const gchar      *clawt_grok_job_get_instructions(ClawtGrokJob *self);
const gchar      *clawt_grok_job_get_result(ClawtGrokJob *self);
const gchar      *clawt_grok_job_get_note(ClawtGrokJob *self);
ClawtGrokJobState clawt_grok_job_get_state(ClawtGrokJob *self);
gint              clawt_grok_job_get_depth(ClawtGrokJob *self);
gint64            clawt_grok_job_get_created_at(ClawtGrokJob *self);
gint64            clawt_grok_job_get_updated_at(ClawtGrokJob *self);
gint64            clawt_grok_job_get_expires_at(ClawtGrokJob *self);

void clawt_grok_job_set_agent_id(ClawtGrokJob *self, const gchar *agent_id);
void clawt_grok_job_set_room_id(ClawtGrokJob *self, const gchar *room_id);
void clawt_grok_job_set_task_id(ClawtGrokJob *self, const gchar *task_id);
void clawt_grok_job_set_run_uuid(ClawtGrokJob *self, const gchar *run_uuid);
void clawt_grok_job_set_instructions(ClawtGrokJob *self,
                                     const gchar  *instructions);
void clawt_grok_job_set_result(ClawtGrokJob *self, const gchar *result);
void clawt_grok_job_set_note(ClawtGrokJob *self, const gchar *note);
void clawt_grok_job_set_state(ClawtGrokJob *self, ClawtGrokJobState state);
void clawt_grok_job_set_depth(ClawtGrokJob *self, gint depth);
void clawt_grok_job_set_created_at(ClawtGrokJob *self, gint64 when);
void clawt_grok_job_set_updated_at(ClawtGrokJob *self, gint64 when);
void clawt_grok_job_set_expires_at(ClawtGrokJob *self, gint64 when);

/**
 * clawt_grok_job_to_json:
 * @self: a #ClawtGrokJob
 * @builder: where to write it, positioned to begin a value
 *
 * The job as the IPC surface and every client sees it.  Never the
 * token, which the job does not hold, nor the bot's key.
 */
void clawt_grok_job_to_json(ClawtGrokJob *self, JsonBuilder *builder);

/* ── State, as every client draws it ─────────────────────────────── */

/**
 * clawt_grok_job_state_is_settled:
 * @state: a #ClawtGrokJobState
 *
 * Whether nothing more will happen to the job.
 *
 * `needs_input` is settled: the bot has stopped and said why, and the
 * agent decides whether to send a new job with the answer.
 *
 * Returns: %TRUE for done, failed, needs_input, expired and refused
 */
gboolean clawt_grok_job_state_is_settled(ClawtGrokJobState state);

/**
 * clawt_grok_job_state_needs_attention:
 * @state: a #ClawtGrokJobState
 *
 * Whether the page's badge counts it: a job that went wrong, ran out of
 * time, or came back with a question.
 *
 * Returns: %TRUE for failed, needs_input, expired and refused
 */
gboolean clawt_grok_job_state_needs_attention(ClawtGrokJobState state);

/**
 * clawt_grok_job_state_tone:
 * @state: a #ClawtGrokJobState
 *
 * The tone every client paints the state in: `good`, `bad`, `warn`,
 * `info` or `neutral` -- the same vocabulary as clawt_task_state_tone(),
 * which every client's stylesheet already paints.
 *
 * Returns: (transfer none): the tone
 */
const gchar *clawt_grok_job_state_tone(ClawtGrokJobState state);

/* ── Which bot ───────────────────────────────────────────────────── */

/**
 * clawt_grok_resolve_bot:
 * @names: (array zero-terminated=1): the bots this agent may reach
 * @asked: what the agent called it
 * @error: (out) (optional): why nothing was chosen, naming the choices
 *
 * Which of @names "@asked" means: the exact name, then the same name in
 * another case, then a prefix only one name has -- so "Chief", "chief"
 * and "ch" all reach a bot called `chief` when nothing else starts with
 * "ch".  Anything else is refused, and the refusal lists every name, so
 * the agent's next attempt is a choice rather than another guess.
 *
 * Returns: the index into @names, or -1
 */
gint clawt_grok_resolve_bot(const gchar *const  *names,
                            const gchar         *asked,
                            GError             **error);

/* ── Tokens and addresses ────────────────────────────────────────── */

/**
 * clawt_grok_token_digest:
 * @token: a job's callback token
 *
 * What the store keeps instead of the token: a SHA-256, hex.  A copy of
 * the database is then not a way to answer somebody else's job.
 *
 * Returns: (transfer full): the digest
 */
gchar *clawt_grok_token_digest(const gchar *token);

/**
 * clawt_grok_token_matches:
 * @digest: (nullable): the stored digest
 * @presented: (nullable): what the caller sent
 *
 * Compared in constant time, digest to digest, so a caller learns
 * nothing from how long a wrong token took to refuse.
 *
 * Returns: %TRUE when @presented is the token @digest was made from
 */
gboolean clawt_grok_token_matches(const gchar *digest,
                                  const gchar *presented);

/**
 * clawt_grok_callback_url:
 * @host: the daemon's tailnet name, e.g. `box.tail1234.ts.net`
 * @port: the webhook listener's port
 * @job_id: the job
 *
 * Plain HTTP on purpose: the tailnet is WireGuard, end to end, and the
 * listener holds no certificate.
 *
 * Returns: (transfer full): where the bot reports the job
 */
gchar *clawt_grok_callback_url(const gchar *host, guint16 port,
                               const gchar *job_id);

/**
 * clawt_grok_mcp_url:
 * @host: the daemon's tailnet name
 * @port: the webhook listener's port
 *
 * Returns: (transfer full): the MCP endpoint a bot is connected to once
 */
gchar *clawt_grok_mcp_url(const gchar *host, guint16 port);

/* ── What the bot is handed ──────────────────────────────────────── */

/**
 * clawt_grok_envelope:
 * @job: the job, with its id, bot, agent and expiry set
 * @instructions: what to do, in the agent's words
 * @context: (nullable): anything else the agent attached
 * @callback_url: where to report
 * @token: the job's callback token, in the clear -- the only place it is
 * @mcp_url: (nullable): the MCP endpoint, when the bot has been connected
 *   to it
 *
 * The body POSTed to the routine, which the bot reads as its input.
 *
 * Self-describing on purpose: a routine's standing instructions are
 * pasted once and drift, so the envelope repeats how to answer -- the
 * URL, the header, the shape of the body and the tool -- every time.
 *
 * Returns: (transfer full): the JSON text
 */
gchar *clawt_grok_envelope(ClawtGrokJob *job,
                           const gchar  *instructions,
                           const gchar  *context,
                           const gchar  *callback_url,
                           const gchar  *token,
                           const gchar  *mcp_url);

/**
 * clawt_grok_message_envelope:
 * @bot: the bot's name here
 * @agent_id: the agent that replied
 * @conversation: the conversation's id, its room
 * @body: what the agent said
 * @mcp_url: (nullable): the MCP endpoint, when the bot is connected to it
 *
 * The body POSTed to a bot's routine to hand it an agent's reply in a
 * conversation the bot started: `{"clawtilla": {"kind": "message", ...}}`,
 * saying how to answer and that silence ends it.
 *
 * Returns: (transfer full): the JSON text
 */
gchar *clawt_grok_message_envelope(const gchar *bot,
                                   const gchar *agent_id,
                                   const gchar *conversation,
                                   const gchar *body,
                                   const gchar *mcp_url);

/**
 * clawt_grok_routine_prompt:
 * @bot: the bot's name here
 * @description: (nullable): what it is for
 * @mcp_url: (nullable): the MCP endpoint, when there is one
 *
 * The standing instructions a person pastes into the Grok routine once.
 *
 * Returns: (transfer full): the text
 */
gchar *clawt_grok_routine_prompt(const gchar *bot,
                                 const gchar *description,
                                 const gchar *mcp_url);

/* ── What the bot answers ────────────────────────────────────────── */

/**
 * clawt_grok_dispatch_reading:
 * @status: the HTTP status the routine's webhook answered
 * @body: (nullable) (array length=length): its body
 * @length: its length, or -1
 * @out_run_uuid: (out) (transfer full) (optional) (nullable): Grok's run
 *   id, on success
 * @out_reason: (out) (transfer full) (optional) (nullable): why it did
 *   not start, in words for the agent
 *
 * What the routine's answer to a POST means.  200 is a run that has
 * *started*; 400 is a routine switched off; 401 a key that is wrong or
 * rotated; 429 the routine's hourly cap.  Grok neither queues nor
 * retries a refused request, so none of these is worth sending again
 * unchanged.
 *
 * Returns: %CLAWT_GROK_JOB_ACCEPTED or %CLAWT_GROK_JOB_REFUSED
 */
ClawtGrokJobState clawt_grok_dispatch_reading(guint         status,
                                              const gchar  *body,
                                              gssize        length,
                                              gchar       **out_run_uuid,
                                              gchar       **out_reason);

/**
 * clawt_grok_report_parse:
 * @body: (array length=length): what the bot POSTed
 * @length: its length, or -1
 * @out_state: (out): what it says happened
 * @out_text: (out) (transfer full): the result, the question or the note
 * @error: (out) (optional): why it could not be read
 *
 * A report: `{"status": "done"|"failed"|"needs_input"|"progress",
 * "result": "..."}`.  `note` is read as `result` for a progress report,
 * because a bot that was told both will use whichever it saw last.
 *
 * Returns: %TRUE if it was a report
 */
gboolean clawt_grok_report_parse(const gchar        *body,
                                 gssize              length,
                                 ClawtGrokJobState  *out_state,
                                 gchar             **out_text,
                                 GError            **error);

/**
 * clawt_grok_report_state_from_word:
 * @word: (nullable): `done`, `failed`, `needs_input` or `progress`
 * @out_state: (out): the state
 *
 * Returns: %TRUE when @word is one of the four a bot may say
 */
gboolean clawt_grok_report_state_from_word(const gchar       *word,
                                           ClawtGrokJobState *out_state);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtGrokJob, clawt_grok_job_free)

G_END_DECLS
