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

#include "clawt-types.h"
#include "config/clawt-config.h"
#include "trigger/clawt-trigger-event.h"

G_BEGIN_DECLS

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
 * Whether the repository and branch filters let this delivery through.
 *
 * A filter set against a delivery that names no repository at all is a
 * miss, not a match: `repo: mine/thing` must not be satisfied by a
 * payload this build could not read.
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
 * `{{url}}` and `{{number}}` with what the delivery said.
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
