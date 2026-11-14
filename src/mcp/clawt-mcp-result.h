/*
 * clawt-mcp-result.h - A tool result bigger than the budget
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A connector's tool can answer with megabytes: a repository listing, a
 * log, a search across a whole tracker.  Passed through, that is read by
 * the model on every later call of the turn, and one careless listing
 * spends the context the rest of the work needed.  So the relay holds a
 * budget per connector.  A result past it is trimmed -- structurally
 * where it is JSON, on a character boundary where it is prose -- and the
 * cut is announced at both ends, because a model reads a quiet note at
 * the bottom of a long result as the end of the data.  The whole result
 * is kept under the agent's own state directory, and the agent reads the
 * rest of it with clawtilla_mcp_result_query instead of calling the tool
 * again.
 *
 * Three rules shape everything here:
 *
 * - *A limit applied after the read is not a limit.*  The relay used to
 *   read a whole line before looking at it, so the size of one message
 *   was the server's choice.  #ClawtMcpReader holds at most
 *   %CLAWT_MCP_RESULT_HOLD_MAX bytes of one message in memory and writes
 *   the rest to disk as it arrives.
 * - *A cut never lands inside a character.*  Not inside a UTF-8 sequence,
 *   not inside a JSON `\uXXXX` escape, and not between the two halves of
 *   an escaped surrogate pair -- half an emoji is an invalid string some
 *   providers refuse outright.  clawt_mcp_result_safe_cut() is the one
 *   place that decides it.
 * - *The held result is the agent's own.*  It is written 0600 under
 *   `<state_dir>/agents/<id>/mcp-results/`, and the query tool resolves a
 *   result id inside the calling agent's directory and nowhere else, so
 *   one agent cannot read what another's tool returned.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <json-glib/json-glib.h>

#include "config/clawt-config.h"

G_BEGIN_DECLS

/**
 * CLAWT_MCP_RESULT_HOLD_MAX:
 *
 * The most of one message the relay holds in memory, in bytes.  Anything
 * longer is written to the held result as it arrives and answered from
 * its first bytes, never parsed.  A ceiling on memory, not a budget: it
 * applies whatever max_result_bytes says, including 0.
 */
#define CLAWT_MCP_RESULT_HOLD_MAX (64 * 1024 * 1024)

/**
 * CLAWT_MCP_RESULT_MIN_BUDGET:
 *
 * The smallest budget honoured.  Below it the two notes would be most of
 * what the agent reads.
 */
#define CLAWT_MCP_RESULT_MIN_BUDGET 1024

/**
 * CLAWT_MCP_RESULT_KEEP_SECONDS:
 *
 * How long a held result is kept: a day, which outlasts any turn that
 * could still be reading it.
 */
#define CLAWT_MCP_RESULT_KEEP_SECONDS (24 * 60 * 60)

/**
 * CLAWT_MCP_RESULT_KEEP_COUNT:
 *
 * How many held results an agent keeps at most; the oldest go first.
 */
#define CLAWT_MCP_RESULT_KEEP_COUNT 64

/**
 * CLAWT_MCP_RESULT_QUERY_TOOL:
 *
 * The tool that reads a held result, named once so the notes in a trimmed
 * result and the tool table cannot drift apart.
 */
#define CLAWT_MCP_RESULT_QUERY_TOOL "clawtilla_mcp_result_query"

/**
 * clawt_mcp_result_budget_resolve:
 * @connector_value: the connector's own `max_result_bytes`; -1 (or any
 *   negative value) when it does not set one
 * @fleet_value: `connectors.max_result_bytes`
 *
 * The budget a connector's results are held to.
 *
 * The connector's value wins when it sets one; 0 from either place means
 * no budget, and anything else below %CLAWT_MCP_RESULT_MIN_BUDGET is read
 * as that minimum.  The one resolver, used by the relay and by every
 * client that shows the effective value.
 *
 * Returns: the budget in bytes, or 0 for none
 */
gint64 clawt_mcp_result_budget_resolve(gint64 connector_value,
                                       gint64 fleet_value);

/**
 * clawt_mcp_result_default_budget:
 *
 * `connectors.max_result_bytes`'s schema default, for a client showing a
 * field before the daemon has said what the fleet's value is -- read from
 * the schema, so no client keeps a copy of the number.
 *
 * Returns: the default budget in bytes
 */
gint64 clawt_mcp_result_default_budget(void);

/**
 * clawt_mcp_result_budget_label:
 * @budget: a resolved budget, from clawt_mcp_result_budget_resolve()
 *
 * The budget as a person reads it: "64 KB", "1.5 MB" or "no budget".
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_mcp_result_budget_label(gint64 budget);

/**
 * clawt_mcp_result_safe_cut:
 * @text: (array length=length): the bytes being cut
 * @length: how many bytes @text holds
 * @max: the most that may be kept
 *
 * Where to cut @text so that no more than @max bytes are kept and the cut
 * lands on a boundary.
 *
 * Moves back past a UTF-8 continuation byte, out of a JSON `\uXXXX`
 * escape or a trailing unescaped backslash, and in front of an escaped
 * high surrogate whose low half would be cut off.  A run of backslashes
 * is counted, so `\\u` is a backslash and a `u`, not an escape.
 *
 * Returns: the number of bytes to keep, at most @max
 */
gsize clawt_mcp_result_safe_cut(const gchar *text,
                                gsize        length,
                                gsize        max);

/**
 * clawt_mcp_result_new_id:
 *
 * A fresh held-result id: `r` and sixteen hex digits from the kernel.
 * The permission is the agent's directory, not the secrecy of the id;
 * the randomness is so an id cannot be guessed into another's anyway.
 *
 * Returns: (transfer full): the id
 */
gchar *clawt_mcp_result_new_id(void);

/**
 * clawt_mcp_result_id_is_valid:
 * @id: (nullable): a held-result id from an agent
 *
 * Whether @id has the shape clawt_mcp_result_new_id() makes.  An id is
 * part of a path, so nothing else is let near the filesystem.
 *
 * Returns: %TRUE if @id is well formed
 */
gboolean clawt_mcp_result_id_is_valid(const gchar *id);

/**
 * clawt_mcp_result_spill_dir:
 * @config: the fleet configuration
 * @agent_id: the agent the results belong to
 *
 * Where an agent's held results live: `mcp-results` under its state
 * directory.
 *
 * Returns: (transfer full): the directory
 */
gchar *clawt_mcp_result_spill_dir(ClawtConfig *config,
                                  const gchar *agent_id);

/**
 * clawt_mcp_result_path:
 * @dir: an agent's held-result directory
 * @id: a held-result id
 *
 * The file holding result @id in @dir.
 *
 * Returns: (transfer full) (nullable): the path, or %NULL when @id is
 *   not a valid id
 */
gchar *clawt_mcp_result_path(const gchar *dir,
                             const gchar *id);

/**
 * clawt_mcp_result_write_meta:
 * @dir: an agent's held-result directory
 * @id: the held result
 * @connector: (nullable): the connector it came from
 * @tool: (nullable): the tool that returned it
 * @bytes: how big it was
 * @budget: the budget it was trimmed to
 * @error: (out) (optional): return location for a #GError
 *
 * Writes what the query tool needs to say about a held result, beside it,
 * at 0600.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_mcp_result_write_meta(const gchar  *dir,
                                     const gchar  *id,
                                     const gchar  *connector,
                                     const gchar  *tool,
                                     gsize         bytes,
                                     gsize         budget,
                                     GError      **error);

/**
 * clawt_mcp_result_spill:
 * @dir: an agent's held-result directory, made 0700 if missing
 * @id: the held-result id to write
 * @data: (array length=length): the whole message, as it arrived
 * @length: its size
 * @connector: (nullable): the connector it came from
 * @tool: (nullable): the tool that returned it
 * @budget: the budget it was trimmed to
 * @error: (out) (optional): return location for a #GError
 *
 * Keeps a whole result for the query tool: the message at 0600, its
 * description beside it, and then the directory swept with
 * clawt_mcp_result_sweep().
 *
 * Returns: %TRUE on success
 */
gboolean clawt_mcp_result_spill(const gchar  *dir,
                                const gchar  *id,
                                const gchar  *data,
                                gsize         length,
                                const gchar  *connector,
                                const gchar  *tool,
                                gsize         budget,
                                GError      **error);

/**
 * clawt_mcp_result_sweep:
 * @dir: an agent's held-result directory
 * @now: the time, in seconds since the epoch
 * @keep_count: how many results to keep at most
 * @keep_seconds: how old a result may be
 *
 * Removes held results older than @keep_seconds, then the oldest beyond
 * @keep_count.  Only files with a held-result id for a name are touched.
 *
 * Returns: how many results were removed
 */
guint clawt_mcp_result_sweep(const gchar *dir,
                             gint64       now,
                             guint        keep_count,
                             gint64       keep_seconds);

/**
 * clawt_mcp_result_trim:
 * @result: a `tools/call` result object
 * @budget: the budget, in bytes, above 0
 * @original_bytes: how big the whole message was
 * @connector: (nullable): the connector, for the notes
 * @tool: (nullable): the tool, for the notes
 * @result_id: (nullable): where the whole result is held, or %NULL when
 *   it could not be kept
 * @out_kept: (out) (optional): how many bytes of content were kept
 *
 * Trims a tool result to @budget.
 *
 * Each text block gets an equal share; a text that is itself JSON is cut
 * structurally (arrays to the records that fit, objects member by
 * member) and stays valid JSON; prose is cut with
 * clawt_mcp_result_safe_cut().  `structuredContent` is trimmed the same
 * way, so the provider is not handed a second full copy.  A block that
 * cannot be cut -- an image, a blob -- is kept if it fits and otherwise
 * replaced by a sentence saying where it is in the held result.  A
 * `[clawtilla]` note goes first and last; the server's own text is
 * escaped with clawt_preamble_escape_all() so it cannot forge one.  The
 * notes come on top of @budget.
 *
 * Returns: (transfer full) (nullable): the trimmed result, or %NULL when
 *   @result is within @budget already
 */
JsonObject *clawt_mcp_result_trim(JsonObject  *result,
                                  gsize        budget,
                                  gsize        original_bytes,
                                  const gchar *connector,
                                  const gchar *tool,
                                  const gchar *result_id,
                                  gsize       *out_kept);

/**
 * clawt_mcp_result_oversized_reply:
 * @id_json: (nullable): the request's id, as JSON text
 * @total: how big the message was
 * @head: (array length=head_length): its first bytes, raw
 * @head_length: how many bytes @head holds
 * @budget: the budget to show the head within
 * @connector: (nullable): the connector
 * @tool: (nullable): the tool
 * @result_id: (nullable): where the whole message is held
 *
 * The reply for a message too big to hold in memory: never parsed, so
 * the agent is shown the start of its raw JSON, cut safely, between the
 * same two notes -- and told the whole of it can be searched.
 *
 * Returns: (transfer full): one JSON-RPC line
 */
gchar *clawt_mcp_result_oversized_reply(const gchar *id_json,
                                        gsize        total,
                                        const gchar *head,
                                        gsize        head_length,
                                        gsize        budget,
                                        const gchar *connector,
                                        const gchar *tool,
                                        const gchar *result_id);

/**
 * clawt_mcp_result_notice:
 * @connector: (nullable): the connector
 * @tool: (nullable): the tool
 * @original_bytes: how big the result was
 * @kept_bytes: how much of it the agent was shown
 * @result_id: (nullable): where the rest is held
 *
 * The line every client shows in the transcript, where the turn's tool
 * calls are, when a result was trimmed -- so somebody reading the
 * conversation knows the agent saw part of it.
 *
 * Returns: (transfer full): the notice
 */
gchar *clawt_mcp_result_notice(const gchar *connector,
                               const gchar *tool,
                               gsize        original_bytes,
                               gsize        kept_bytes,
                               const gchar *result_id);

/**
 * CLAWT_MCP_RESULT_NOTICE_PREFIX:
 *
 * How every notice from clawt_mcp_result_notice() begins.
 */
#define CLAWT_MCP_RESULT_NOTICE_PREFIX "Result trimmed:"

/**
 * clawt_mcp_result_is_notice:
 * @text: (nullable): a status step's text
 *
 * Whether a status step is a trimmed-result notice, so each client can
 * draw it as a warning beside the tool calls rather than as ordinary
 * backend chatter.
 *
 * Returns: %TRUE for a notice from clawt_mcp_result_notice()
 */
gboolean clawt_mcp_result_is_notice(const gchar *text);

/**
 * clawt_mcp_result_query:
 * @dir: the calling agent's held-result directory
 * @result_id: the result to read
 * @path: (nullable): a jq-like path into it -- `.content[0].text`,
 *   `.items[20:40]`, `["odd key"]`; a string that is JSON is descended
 *   into
 * @search: (nullable): text to look for, ignoring ASCII case
 * @offset: where to start: a byte in a string, an item in an array
 * @limit: how much: bytes of a string, items of an array; 0 for the
 *   default
 * @is_error: (out): whether the answer is a refusal
 *
 * Reads part of a held result.  A result id is looked up in @dir and
 * nowhere else -- that is the permission -- and an id that is not there
 * is answered exactly as one that never existed.  The answer is capped
 * at the budget the result was held to, announced at both ends when it
 * is cut, and the result's own text is escaped.
 *
 * Returns: (transfer full): the answer
 */
gchar *clawt_mcp_result_query(const gchar *dir,
                              const gchar *result_id,
                              const gchar *path,
                              const gchar *search,
                              gint64       offset,
                              gint64       limit,
                              gboolean    *is_error);

/**
 * ClawtMcpOversized: (skip)
 * @total: how many bytes the message was
 * @id_json: (nullable): its top-level `id`, as JSON text, when one was
 *   seen
 * @head: its first bytes
 * @head_length: how many of them
 * @result_id: (nullable): where the whole message was written, or %NULL
 *   when there was nowhere to write it
 *
 * A message longer than %CLAWT_MCP_RESULT_HOLD_MAX, described from what
 * was seen of it on the way past.
 */
typedef struct {
	gsize        total;
	const gchar *id_json;
	const gchar *head;
	gsize        head_length;
	const gchar *result_id;
} ClawtMcpOversized;

/**
 * ClawtMcpReaderFunc: (skip)
 * @message: (nullable): one whole message, or %NULL when it was too big
 * @length: its length
 * @oversized: (nullable): what was seen of a message too big to hold
 * @user_data: data for the callback
 *
 * One message from a #ClawtMcpReader: in memory, or described.
 */
typedef void (*ClawtMcpReaderFunc)(const gchar             *message,
                                   gsize                    length,
                                   const ClawtMcpOversized *oversized,
                                   gpointer                 user_data);

/**
 * ClawtMcpFraming: (skip)
 * @CLAWT_MCP_FRAMING_LINES: one message per line (stdio)
 * @CLAWT_MCP_FRAMING_SSE: a server-sent event stream; each `data:` line
 *   is a message and every other line is dropped
 * @CLAWT_MCP_FRAMING_WHOLE: the whole input is one message (a JSON HTTP
 *   body)
 *
 * How a #ClawtMcpReader finds where a message ends.
 */
typedef enum {
	CLAWT_MCP_FRAMING_LINES,
	CLAWT_MCP_FRAMING_SSE,
	CLAWT_MCP_FRAMING_WHOLE
} ClawtMcpFraming;

/**
 * ClawtMcpReader: (skip)
 *
 * Splits a byte stream into messages while holding at most a ceiling of
 * any one of them in memory.
 */
typedef struct _ClawtMcpReader ClawtMcpReader;

/**
 * clawt_mcp_reader_new: (skip)
 * @framing: where messages end
 * @ceiling: the most of one message held in memory
 * @spill_dir: (nullable): where a longer message is written as it
 *   arrives; %NULL drops the rest after counting it
 * @func: called once per message
 * @user_data: data for @func
 *
 * Returns: (transfer full): the reader
 */
ClawtMcpReader *clawt_mcp_reader_new(ClawtMcpFraming     framing,
                                     gsize               ceiling,
                                     const gchar        *spill_dir,
                                     ClawtMcpReaderFunc  func,
                                     gpointer            user_data);

/**
 * clawt_mcp_reader_feed: (skip)
 * @self: a reader
 * @data: (array length=length): the next bytes
 * @length: how many
 *
 * Takes the next bytes, calling the callback for every message they end.
 */
void clawt_mcp_reader_feed(ClawtMcpReader *self,
                           const gchar    *data,
                           gsize           length);

/**
 * clawt_mcp_reader_finish: (skip)
 * @self: a reader
 *
 * The input ended: a message still open is delivered as it stands.
 */
void clawt_mcp_reader_finish(ClawtMcpReader *self);

/**
 * clawt_mcp_reader_free: (skip)
 * @self: (nullable): a reader
 */
void clawt_mcp_reader_free(ClawtMcpReader *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMcpReader, clawt_mcp_reader_free)

/**
 * ClawtMcpResultsTrimmedFunc: (skip)
 * @connector: (nullable): the connector
 * @tool: (nullable): the tool
 * @original_bytes: how big the result was
 * @kept_bytes: how much the agent was shown
 * @result_id: (nullable): where the rest is held
 * @user_data: data for the callback
 *
 * Told each time a result is trimmed, so the daemon can put a notice in
 * the transcript.
 */
typedef void (*ClawtMcpResultsTrimmedFunc)(const gchar *connector,
                                           const gchar *tool,
                                           gsize        original_bytes,
                                           gsize        kept_bytes,
                                           const gchar *result_id,
                                           gpointer     user_data);

/**
 * ClawtMcpResults: (skip)
 *
 * The relay's side of the budget: which requests were tool calls, and
 * what to do with each answer.
 */
typedef struct _ClawtMcpResults ClawtMcpResults;

/**
 * clawt_mcp_results_new: (skip)
 * @connector: (nullable): the connector, for notes and notices
 * @budget: the resolved budget; 0 for none
 * @spill_dir: (nullable): the agent's held-result directory; %NULL when
 *   there is no agent to keep results for
 *
 * Returns: (transfer full): the state
 */
ClawtMcpResults *clawt_mcp_results_new(const gchar *connector,
                                       gsize        budget,
                                       const gchar *spill_dir);

/**
 * clawt_mcp_results_free: (skip)
 * @self: (nullable): the state
 */
void clawt_mcp_results_free(ClawtMcpResults *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtMcpResults, clawt_mcp_results_free)

/**
 * clawt_mcp_results_get_budget: (skip)
 * @self: the state
 *
 * Returns: the budget, 0 for none
 */
gsize clawt_mcp_results_get_budget(ClawtMcpResults *self);

/**
 * clawt_mcp_results_get_spill_dir: (skip)
 * @self: the state
 *
 * Returns: (transfer none) (nullable): where results are held
 */
const gchar *clawt_mcp_results_get_spill_dir(ClawtMcpResults *self);

/**
 * clawt_mcp_results_set_trimmed_func: (skip)
 * @self: the state
 * @func: (nullable): told of each trim
 * @user_data: data for @func
 */
void clawt_mcp_results_set_trimmed_func(ClawtMcpResults            *self,
                                        ClawtMcpResultsTrimmedFunc  func,
                                        gpointer                    user_data);

/**
 * clawt_mcp_results_note_outbound: (skip)
 * @self: the state
 * @line: a message on its way to the server, as it will be sent
 *
 * Remembers a `tools/call`'s id and the tool it names, so the answer can
 * be told apart from every other reply.
 */
void clawt_mcp_results_note_outbound(ClawtMcpResults *self,
                                     const gchar     *line);

/**
 * clawt_mcp_results_inbound: (skip)
 * @self: the state
 * @line: a message from the server
 *
 * Trims a tool call's answer that is over the budget, keeping the whole
 * message first.
 *
 * Returns: (transfer full): what to pass on -- @line unchanged when
 *   there was nothing to trim
 */
gchar *clawt_mcp_results_inbound(ClawtMcpResults *self,
                                 const gchar     *line);

/**
 * clawt_mcp_results_oversized: (skip)
 * @self: the state
 * @oversized: what was seen of a message too big to hold
 *
 * The reply to pass on for a message the reader could not hold.
 *
 * Returns: (transfer full) (nullable): one JSON-RPC line, or %NULL when
 *   the message had no id to answer
 */
gchar *clawt_mcp_results_oversized(ClawtMcpResults         *self,
                                   const ClawtMcpOversized *oversized);

G_END_DECLS
