/*
 * clawt-trigger.h - What a trigger accepts, and what it asks for
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * #ClawtTrigger itself -- the handle over one `triggers:` entry -- is
 * declared in <config/clawt-config.h> beside #ClawtRoutine, because it
 * is the same kind of thing and shares the machinery.  Here is what a
 * trigger *does*: which deliveries it accepts, what endpoint it answers
 * on, and what the agent is asked when one arrives.
 *
 * A routine is a clock; a trigger is an event.  Both end in the same
 * queued run against the same agent, so the prompt this builds is handed
 * to #ClawtRoutineRunFunc -- the callback the routine runner already
 * uses -- rather than to an execution path of its own.  Two paths would
 * differ exactly once, and the difference would be found by an operator
 * rather than by a test.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

#include "clawt-types.h"
#include "config/clawt-config.h"
#include "trigger/clawt-trigger-event.h"
#include "task/clawt-routine-runner.h"

G_BEGIN_DECLS

/**
 * CLAWT_TRIGGER_MAX_PENDING_MIN:
 *
 * The smallest `triggers.max_pending` a trigger may have.  Zero would be
 * a trigger that can never run, which is what `enabled: false` is for.
 */
#define CLAWT_TRIGGER_MAX_PENDING_MIN (1)

/**
 * CLAWT_TRIGGER_MAX_PENDING_MAX:
 *
 * The largest `triggers.max_pending` a trigger may have.  Fifty
 * unfinished runs from one webhook is already a fleet's worth of agents
 * reacting to one sender; past that the cap has stopped being one.
 */
#define CLAWT_TRIGGER_MAX_PENDING_MAX (50)

/**
 * CLAWT_TRIGGER_MATCH_MAX:
 *
 * How many `{path, value}` pairs one `triggers.match` may hold.
 */
#define CLAWT_TRIGGER_MATCH_MAX (10)

/**
 * ClawtTriggerMatch:
 * @path: dotted path into the delivery's JSON body
 * @value: text the field must contain, compared ignoring case
 *
 * One pair from `triggers.match`.
 */
struct _ClawtTriggerMatch {
	gchar *path;
	gchar *value;
};

#define CLAWT_TYPE_TRIGGER_MATCH (clawt_trigger_match_get_type())

GType clawt_trigger_match_get_type(void) G_GNUC_CONST;

/**
 * clawt_trigger_match_new:
 * @path: the dotted path
 * @value: the text it must contain
 *
 * Returns: (transfer full): a new pair
 */
ClawtTriggerMatch *
clawt_trigger_match_new(
	const gchar *path,
	const gchar *value
);

/**
 * clawt_trigger_match_copy:
 * @self: a #ClawtTriggerMatch
 *
 * Returns: (transfer full): a copy
 */
ClawtTriggerMatch *
clawt_trigger_match_copy(
	const ClawtTriggerMatch *self
);

/**
 * clawt_trigger_match_free:
 * @self: (nullable): a #ClawtTriggerMatch
 *
 * Frees the pair.
 */
void
clawt_trigger_match_free(
	ClawtTriggerMatch *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTriggerMatch, clawt_trigger_match_free)

/**
 * clawt_trigger_match_check:
 * @path: (nullable): a dotted path
 * @value: (nullable): the text it must contain
 * @error: (out) (optional): what is wrong with it
 *
 * Whether a pair can be used.
 *
 * A path is one or more non-empty parts joined by dots, with no space,
 * control character or `=` anywhere -- `=` because the command line and
 * the web form both spell a pair as `path=value`, and a path holding one
 * would split in the wrong place.  The value must not be empty: an empty
 * needle is contained in everything, so it would be a filter that never
 * filters while looking like one that does.
 *
 * Returns: %TRUE if the pair is usable
 */
gboolean
clawt_trigger_match_check(
	const gchar  *path,
	const gchar  *value,
	GError      **error
);

/**
 * clawt_trigger_match_lookup:
 * @root: a parsed JSON body
 * @path: a dotted path, already checked
 *
 * What @path holds in @root, as text.
 *
 * A part that is all digits indexes an array.  Any other part reaching
 * an array is applied to every element, so `labels.name` is the name of
 * each label, one per line -- which is what "the pull request has the
 * deploy label" needs and what no single index could say.  A string is
 * itself, a number or boolean its JSON spelling, and an object or array
 * its compact JSON text, so a value can be looked for inside a subtree.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL when nothing
 *   is at @path
 */
gchar *
clawt_trigger_match_lookup(
	JsonNode    *root,
	const gchar *path
);

/**
 * clawt_trigger_match_contains:
 * @haystack: (nullable): what the path held
 * @needle: (nullable): what it must contain
 *
 * Containment, ignoring case by Unicode case folding, so `Release`
 * finds `RELEASE` and `straße` finds `STRASSE`.
 *
 * Returns: %TRUE if @needle occurs in @haystack
 */
gboolean
clawt_trigger_match_contains(
	const gchar *haystack,
	const gchar *needle
);

/**
 * clawt_trigger_match_evaluate:
 * @matches: (element-type ClawtTriggerMatch): the filter
 * @payload: (nullable): the delivery's body
 * @out_reason: (out) (optional) (transfer full): which pair failed, and how
 *
 * Whether every pair matches.  An empty filter matches everything; a
 * non-empty one against a body that is not JSON matches nothing, because
 * "could not look" is not "found it".
 *
 * Returns: %TRUE if all pairs match
 */
gboolean
clawt_trigger_match_evaluate(
	GPtrArray    *matches,
	const gchar  *payload,
	gchar       **out_reason
);

/**
 * clawt_trigger_match_from_json:
 * @node: (nullable): what a client sent for `match`
 * @error: (out) (optional): why it cannot be used
 *
 * Reads a filter off an IPC payload, in every spelling a client sends.
 *
 * An array of `{"path", "value"}` objects is what the graphical clients
 * build; an array of `"path=value"` strings, or one string holding them
 * a line each, is what a command line and an HTML form produce.  JSON
 * null, an empty string and an empty array all clear the filter.  More
 * than %CLAWT_TRIGGER_MATCH_MAX pairs, or any pair
 * clawt_trigger_match_check() refuses, is an error: refused where it
 * arrives, rather than saved and refused at the next load.
 *
 * Returns: (transfer full) (element-type ClawtTriggerMatch) (nullable):
 *   the pairs, possibly empty, or %NULL with @error set
 */
GPtrArray *
clawt_trigger_match_from_json(
	JsonNode  *node,
	GError   **error
);

/**
 * clawt_trigger_match_explain:
 * @matches: (element-type ClawtTriggerMatch): the filter
 * @payload: (nullable): a delivery's body
 *
 * Each pair against @payload, for an editor that wants to show which
 * rows would pass before anything is saved.  One object per pair, with
 * `path`, `value`, `found`, `matched` and -- when something was found --
 * `actual`, cut to a readable length.
 *
 * Returns: (transfer full): a JSON array
 */
JsonNode *
clawt_trigger_match_explain(
	GPtrArray   *matches,
	const gchar *payload
);

/**
 * clawt_trigger_match_paths:
 * @payload: (nullable): a delivery's body
 * @limit: the most paths to list
 *
 * Every scalar in @payload with the path that reaches it, in document
 * order, so an editor can offer a path to pick rather than one to guess.
 * One object per field, with `path` and `value` (cut short).
 *
 * Returns: (transfer full): a JSON array, empty when @payload is not JSON
 */
JsonNode *
clawt_trigger_match_paths(
	const gchar *payload,
	guint        limit
);

/**
 * clawt_trigger_get_max_pending:
 * @self: trigger configuration
 *
 * How many runs this trigger may have unfinished at once.  A value
 * outside %CLAWT_TRIGGER_MAX_PENDING_MIN..%CLAWT_TRIGGER_MAX_PENDING_MAX
 * reads as the schema's default; clawt_config_load() warns about it.
 *
 * Returns: the cap
 */
guint
clawt_trigger_get_max_pending(
	ClawtTrigger *self
);

/**
 * clawt_trigger_check_settings:
 * @self: (nullable): the trigger as saved, for anything not given
 * @mode: the mode it would have
 * @room: (nullable): the room it would report to
 * @error: (out) (optional): why the combination cannot work
 *
 * The rule between `mode` and `room`, stated once: a post has to land
 * somewhere.  Asked by the loader of what is saved and by `trigger.add`
 * and `trigger.update` of what is about to be, so a client is refused
 * before the file is written rather than after.
 *
 * Returns: %TRUE if the combination can work
 */
gboolean
clawt_trigger_check_settings(
	ClawtTrigger      *self,
	ClawtTriggerMode   mode,
	const gchar       *room,
	GError           **error
);

/**
 * clawt_trigger_validate:
 * @self: trigger configuration
 * @error: (out) (optional): the first thing that stops it working
 *
 * Whether this trigger can act on a delivery as written: its `match`
 * reads, and a `post` names a room.  A trigger that fails this refuses
 * every delivery, recorded as failed with this message, rather than
 * acting on a filter or a destination it does not have.
 *
 * Returns: %TRUE if it is usable
 */
gboolean
clawt_trigger_validate(
	ClawtTrigger  *self,
	GError       **error
);

/**
 * clawt_trigger_warnings:
 * @self: trigger configuration
 *
 * Everything worth telling somebody about this trigger at load: what
 * clawt_trigger_validate() refuses, a `mode` the enum does not have, and
 * a `max_pending` outside its range -- the last two read as their
 * defaults and still work, which is exactly why they need saying.
 *
 * Returns: (transfer full) (array zero-terminated=1): the warnings,
 *   possibly none
 */
GStrv
clawt_trigger_warnings(
	ClawtTrigger *self
);

/**
 * clawt_trigger_render_post:
 * @self: a #ClawtTrigger
 * @event: the delivery
 *
 * What a `mode: post` delivery writes into the room.
 *
 * The expanded instructions as the headline, a line saying where it came
 * from and that nothing ran, and the body inside the same untrusted
 * fence a run's prompt uses -- pretty-printed when it is JSON and cut at
 * 8 KiB, because a room is read by people and a megabyte of push payload
 * is not reading.  Every byte of a post is rendered here and nowhere
 * else, so a rule about what may appear in delivered text is applied in
 * one place.
 *
 * Returns: (transfer full): the message body
 */
gchar *
clawt_trigger_render_post(
	ClawtTrigger      *self,
	ClawtTriggerEvent *event
);

/**
 * clawt_trigger_get_recipients:
 * @self: trigger configuration
 *
 * The primary agent followed by additional agents, with blank entries and
 * duplicate names removed. An absent primary agent produces an empty list.
 * Returns: (transfer full) (array zero-terminated=1): ordered recipient ids
 */
gchar **clawt_trigger_get_recipients(ClawtTrigger *self);

/**
 * clawt_trigger_dispatch:
 * @self: trigger configuration
 * @store: durable trigger store
 * @event: normalized event
 * @key: (nullable): parent deduplication key; NULL generates a new one
 * @run: (scope call) (closure user_data): callback that queues one recipient
 * @user_data: callback context
 * @error: (out) (optional): reservation or persistence error
 *
 * Reserves the whole batch atomically before invoking any callbacks, checks
 * the trigger's `max_pending` cap, then records each result independently. Recipient errors
 * appear in the result array; they do not prevent other recipients running.
 * A repeated key never invokes the callback again, including after failures.
 * Callers enforce authentication, filters and enablement before dispatch.
 * Returns: (transfer full) (nullable): JSON array of agent/task/error results
 */
JsonNode *clawt_trigger_dispatch(ClawtTrigger *self, ClawtTriggerStore *store,
    ClawtTriggerEvent *event, const gchar *key, ClawtRoutineRunFunc run,
    gpointer user_data, GError **error);

/**
 * clawt_trigger_endpoint_new:
 * @error: (out) (optional): return location for a #GError
 *
 * A fresh, unguessable endpoint id.
 *
 * The endpoint is the address a trigger is called on and is deliberately
 * not the trigger's id: renaming a trigger would otherwise publish where
 * it lives, and an id somebody chose is a word an attacker can guess.
 *
 * 32 bytes from the kernel's pool, through clawt_generate_token(). Never
 * g_random_*, which is a Mersenne Twister -- a few endpoint ids would
 * give away every other one, and an endpoint id is half of what stops a
 * stranger reaching the fleet.
 *
 * Returns: (transfer full) (nullable): the endpoint id
 */
gchar *clawt_trigger_endpoint_new(GError **error);

/**
 * clawt_trigger_accepts_event:
 * @self: a #ClawtTrigger
 * @event_name: (nullable): the name the sender used
 *
 * Whether `events:` lets this event through.
 *
 * An empty list means every event, which is what somebody who has not
 * thought about it wants. A name outside the list is *accepted and
 * recorded as ignored* by the caller rather than refused: a sender that
 * gets an error for a delivery you simply did not ask for will keep
 * retrying it for ever.
 *
 * The comparison is case-insensitive and treats `-`, `_` and a space as
 * the same character, because GitHub says `pull_request`, GitLab says
 * `Merge Request Hook` and a person writes whichever they last read.
 *
 * Returns: %TRUE if the event is one this trigger acts on
 */
gboolean clawt_trigger_accepts_event(ClawtTrigger *self,
                                     const gchar  *event_name);

/**
 * clawt_trigger_accepts_delivery:
 * @self: a #ClawtTrigger
 * @event: the normalised delivery
 * @out_reason: (out) (optional) (transfer full): why it was not accepted
 *
 * Whether the repository, branch, `ignore_actors` and `match` filters
 * let this delivery through.
 *
 * A filter set against a delivery that names no repository at all is a
 * miss, not a match: `repo: mine/thing` must not be satisfied by a
 * payload this build could not read.  For the same reason a `match`
 * that cannot be read is a miss for every delivery rather than no
 * filter at all.
 *
 * Returns: %TRUE if the delivery should start a run
 */
gboolean clawt_trigger_accepts_delivery(ClawtTrigger       *self,
                                        ClawtTriggerEvent  *event,
                                        gchar             **out_reason);

/**
 * clawt_trigger_expand_template:
 * @template_text: (nullable): the instruction template
 * @event: the delivery to expand it against
 *
 * Replaces `{{event}}`, `{{repo}}`, `{{ref}}`, `{{actor}}`, `{{title}}`,
 * `{{url}}`, `{{number}}` and `{{kind}}` with what the delivery said.
 *
 * Expanded by hand, character by character. This string came out of a
 * configuration file, so it must never reach printf(): a stray percent
 * sign in an instruction is a percent sign, and handing it to a format
 * function is the difference between an odd prompt and reading the
 * daemon's stack.
 *
 * A placeholder this build knows but the delivery did not fill expands
 * to nothing. One it does not know is left exactly as written, so a
 * misspelling is visible in the prompt rather than silently blank --
 * an agent told to look at the repository "{{repoo}}" will say so.
 *
 * Returns: (transfer full): the expanded text, never %NULL
 */
gchar *clawt_trigger_expand_template(const gchar       *template_text,
                                     ClawtTriggerEvent *event);

/**
 * clawt_trigger_build_prompt:
 * @self: a #ClawtTrigger
 * @event: the delivery
 *
 * The whole of what the agent is asked.
 *
 * Three parts, in order: the expanded instructions, a preamble saying
 * where this came from and that nobody is waiting, and the payload
 * inside a fence marked as untrusted.
 *
 * The preamble is not decoration. A webhook body is somebody else's
 * text, arriving at an agent that has tools and a computer, and the
 * agent has no way to tell from the prompt alone that the JSON below the
 * instructions is data rather than more instructions. So it is told: in
 * the same turn, before the payload, naming what not to do with it --
 * including not recording its claims as memories, which is how an
 * injected instruction would outlive the turn that carried it.
 *
 * Returns: (transfer full): the prompt
 */
gchar *clawt_trigger_build_prompt(ClawtTrigger      *self,
                                  ClawtTriggerEvent *event);

/**
 * clawt_trigger_secret_path:
 * @secrets_dir: the configured `secrets.dir`
 * @trigger_id: which trigger
 *
 * Where a generated trigger secret is kept.
 *
 * The id reaches this from a config file, so anything that is not
 * plainly a filename is folded to an underscore -- a slash would put the
 * secret somewhere other than the secrets directory, possibly on top of
 * something else.
 *
 * Returns: (transfer full): the path
 */
gchar *clawt_trigger_secret_path(const gchar *secrets_dir,
                                 const gchar *trigger_id);

G_END_DECLS
