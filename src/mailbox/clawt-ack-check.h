/*
 * clawt-ack-check.h - Whether an answer only acknowledged
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_ACK_CHECK_H
#define CLAWT_ACK_CHECK_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <gio/gio.h>
#include <ai-glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_ACK_CHECK_MAX_CHARS:
 *
 * The longest answer that is even considered, in characters.  Anything
 * longer is assumed to carry substance and wakes its addressee as
 * always; asking a model about it would spend more than it could save.
 */
#define CLAWT_ACK_CHECK_MAX_CHARS (600)

/**
 * CLAWT_ACK_CHECK_TIMEOUT_MS:
 *
 * How long the model has to answer before the answer is treated as
 * "it did not say", which wakes the addressee.
 */
#define CLAWT_ACK_CHECK_TIMEOUT_MS (8000)

/**
 * ClawtAckVerdict:
 * @CLAWT_ACK_VERDICT_UNSAID: the model did not say: it failed, timed out
 *   or answered something that is neither word.  Wakes the addressee.
 * @CLAWT_ACK_VERDICT_ACK: the model said the answer only acknowledges.
 *   The one verdict that skips a wake.
 * @CLAWT_ACK_VERDICT_NEEDS: the model said the addressee needs to read
 *   it.  Wakes the addressee.
 *
 * What the acknowledgement check concluded.  Three values and not a
 * boolean, because "it said no" and "it did not say" are different facts
 * that happen to lead to the same action today -- and a log line that
 * reports a timeout as a NEEDS sends whoever reads it to the wrong layer.
 */
typedef enum {
	CLAWT_ACK_VERDICT_UNSAID = 0,
	CLAWT_ACK_VERDICT_ACK,
	CLAWT_ACK_VERDICT_NEEDS
} ClawtAckVerdict;

GType clawt_ack_verdict_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_ACK_VERDICT (clawt_ack_verdict_get_type())

/**
 * clawt_ack_check_gate:
 * @answer: the answering turn's closing text
 * @evidence: %TRUE when the daemon saw this turn's steps (the agent
 *   streams them), so a zero below is a count and not an absence
 * @tool_calls: tool calls the turn made, from its streamed steps and its
 *   clawtilla tool calls; commands run, files changed and anything sent
 *   outward are all tool calls
 * @condition: %TRUE when the message being answered set a completion
 *   condition (it was a task's delivery)
 * @out_reason: (out) (optional) (transfer none): why the answer was not
 *   considered, for the log
 *
 * Whether an answer may be put to the model at all.  Only a turn that
 * did nothing but write a short reply to a message that asked for
 * nothing in particular is a candidate; everything else wakes its
 * addressee as it always has, without asking anybody.
 *
 * No evidence is not zero tool calls: an agent whose steps are not
 * streamed could have done anything, and is never considered.
 *
 * Returns: %TRUE when the answer may be checked
 */
gboolean clawt_ack_check_gate(const gchar  *answer,
                              gboolean      evidence,
                              guint         tool_calls,
                              gboolean      condition,
                              const gchar **out_reason);

/**
 * clawt_ack_check_system_prompt:
 *
 * The instruction the model is given.  One question, two permitted
 * words.
 *
 * Returns: (transfer none): the system prompt
 */
const gchar *clawt_ack_check_system_prompt(void);

/**
 * clawt_ack_check_prompt:
 * @question: (nullable): the message that was answered
 * @answer: the answer
 *
 * The user message: both texts fenced as data, each fence longer than
 * any run of backticks inside it, so neither text can close its fence
 * and speak to the model as an instruction.
 *
 * Returns: (transfer full): the prompt
 */
gchar *clawt_ack_check_prompt(const gchar *question, const gchar *answer);

/**
 * clawt_ack_check_parse:
 * @reply: (nullable): what the model wrote
 *
 * Reads the model's reply.  Only a reply that is the word ACK, alone
 * (case and surrounding whitespace and a trailing full stop aside), is
 * an ACK; the word NEEDS alone is a NEEDS; anything else -- including
 * "ACK, but..." -- is %CLAWT_ACK_VERDICT_UNSAID.
 *
 * Returns: the verdict
 */
ClawtAckVerdict clawt_ack_check_parse(const gchar *reply);

/**
 * clawt_ack_check_run_async: (skip)
 * @provider: the model to ask
 * @question: (nullable): the message that was answered
 * @answer: the answer
 * @timeout_ms: the hard limit, or 0 for %CLAWT_ACK_CHECK_TIMEOUT_MS
 * @cancellable: (nullable): a #GCancellable
 * @callback: (scope async): called with the verdict
 * @user_data: data for @callback
 *
 * Asks @provider the one question, without blocking.  The timer and the
 * callback are attached to the thread-default context at the time of
 * this call, which the daemon pushes -- so an embedded daemon's check
 * answers on its own loop.  At @timeout_ms the request is cancelled and
 * the verdict is %CLAWT_ACK_VERDICT_UNSAID; a reply arriving after that
 * is discarded.
 *
 * Skipped for introspection: ai-glib ships no GIR, so #AiProvider has no
 * name a binding could resolve.
 */
void clawt_ack_check_run_async(AiProvider          *provider,
                               const gchar         *question,
                               const gchar         *answer,
                               guint                timeout_ms,
                               GCancellable        *cancellable,
                               GAsyncReadyCallback  callback,
                               gpointer             user_data);

/**
 * clawt_ack_check_run_finish: (skip)
 * @result: the #GAsyncResult
 * @error: (out) (optional): why the model did not say, when it did not
 *
 * Returns: the verdict; %CLAWT_ACK_VERDICT_UNSAID with @error set on a
 *   failure or a timeout
 */
ClawtAckVerdict clawt_ack_check_run_finish(GAsyncResult  *result,
                                           GError       **error);

/**
 * clawt_ack_check_split_model:
 * @spec: (nullable): an `orchestration.ack_check_model` value
 * @out_provider: (out) (transfer full) (optional): the provider id
 * @out_model: (out) (transfer full) (optional): the model id
 *
 * Splits `provider/model` at its first slash -- a model id may carry
 * slashes of its own, a provider id never does.
 *
 * Returns: %TRUE when @spec names a provider and a model; %FALSE for
 *   empty (the check is off) or malformed
 */
gboolean clawt_ack_check_split_model(const gchar  *spec,
                                     gchar       **out_provider,
                                     gchar       **out_model);

/**
 * clawt_ack_check_model_count:
 *
 * How many choices a client offers for `orchestration.ack_check_model`:
 * "Off" first, then every model of every provider in the catalogue that
 * ai-glib reaches over HTTP.  A command-line provider is not offered: it
 * spawns a whole CLI for each check and would spend most of the eight
 * seconds starting.  Walked by every client so none keeps a copy.
 *
 * Returns: the number of choices
 */
guint clawt_ack_check_model_count(void);

/**
 * clawt_ack_check_model_nth_value:
 * @n: a choice, below clawt_ack_check_model_count()
 *
 * Returns: (transfer none): the value to store -- "" for Off
 */
const gchar *clawt_ack_check_model_nth_value(guint n);

/**
 * clawt_ack_check_model_nth_label:
 * @n: a choice, below clawt_ack_check_model_count()
 *
 * Returns: (transfer none): how to show it
 */
const gchar *clawt_ack_check_model_nth_label(guint n);

/**
 * clawt_ack_check_model_label:
 * @value: (nullable): a stored value
 *
 * How to show a stored value, including one the catalogue does not
 * list (somebody typed it into the file): that one is shown as itself
 * rather than as Off, which would be a lie about what is configured.
 *
 * Returns: (transfer full): the label
 */
gchar *clawt_ack_check_model_label(const gchar *value);

/**
 * clawt_ack_check_note:
 * @replier: who answered
 * @room_id: where
 * @answer: what they said
 *
 * The one line folded into the asker's next turn: "while you were
 * away, oryx acknowledged ...".  The answer is quoted escaped and
 * clipped, since it is somebody else's text inside the daemon's note.
 *
 * Returns: (transfer full): the line
 */
gchar *clawt_ack_check_note(const gchar *replier,
                            const gchar *room_id,
                            const gchar *answer);

G_END_DECLS

#endif /* CLAWT_ACK_CHECK_H */
