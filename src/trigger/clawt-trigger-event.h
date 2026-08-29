/*
 * clawt-trigger-event.h - What every forge's delivery is flattened into
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Four forges describe the same push four ways, and a fifth caller
 * describes it however it likes.  One normalised record is what lets the
 * instruction template, the repository and branch filters, and the
 * receipt in the store all be written once instead of per provider.
 *
 * Every field is optional.  A generic caller may send nothing but a
 * body, and a template that names {{repo}} for such a delivery gets an
 * empty string rather than the literal placeholder -- an agent told to
 * look at a repository called "{{repo}}" will go and look for one.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-enums.h"
#include "clawt-types.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_TRIGGER_EVENT (clawt_trigger_event_get_type())

GType clawt_trigger_event_get_type(void) G_GNUC_CONST;

/**
 * clawt_trigger_event_new:
 * @provider: who sent it
 * @name: the event name as the sender spells it, e.g. "push"
 * @delivery_id: (nullable): the sender's own id for this delivery
 *
 * Returns: (transfer full): a new #ClawtTriggerEvent
 */
ClawtTriggerEvent *clawt_trigger_event_new(ClawtTriggerProvider  provider,
                                           const gchar          *name,
                                           const gchar          *delivery_id);

ClawtTriggerEvent *clawt_trigger_event_copy(ClawtTriggerEvent *self);
void               clawt_trigger_event_free(ClawtTriggerEvent *self);

ClawtTriggerProvider clawt_trigger_event_get_provider(ClawtTriggerEvent *self);

const gchar *clawt_trigger_event_get_name(ClawtTriggerEvent *self);
const gchar *clawt_trigger_event_get_delivery_id(ClawtTriggerEvent *self);
const gchar *clawt_trigger_event_get_repo(ClawtTriggerEvent *self);
const gchar *clawt_trigger_event_get_ref(ClawtTriggerEvent *self);

/**
 * clawt_trigger_event_get_branch:
 * @self: a #ClawtTriggerEvent
 *
 * The branch, with `refs/heads/` taken off.
 *
 * Separate from the ref because `branch: master` is what somebody writes
 * in a filter and `refs/heads/master` is what a forge sends, and the two
 * comparing unequal is a filter that silently matches nothing.
 *
 * Returns: (nullable): the branch
 */
const gchar *clawt_trigger_event_get_branch(ClawtTriggerEvent *self);

const gchar *clawt_trigger_event_get_actor(ClawtTriggerEvent *self);
const gchar *clawt_trigger_event_get_title(ClawtTriggerEvent *self);
const gchar *clawt_trigger_event_get_url(ClawtTriggerEvent *self);
const gchar *clawt_trigger_event_get_number(ClawtTriggerEvent *self);

/**
 * clawt_trigger_event_get_payload:
 * @self: a #ClawtTriggerEvent
 *
 * The body as it arrived, untouched.
 *
 * Kept verbatim rather than re-serialised: this is what the agent is
 * shown, and a body that has been through a parser and back is no longer
 * the thing that was signed.
 *
 * Returns: (nullable): the body
 */
const gchar *clawt_trigger_event_get_payload(ClawtTriggerEvent *self);

void clawt_trigger_event_set_repo(ClawtTriggerEvent *self,
                                  const gchar       *repo);
void clawt_trigger_event_set_ref(ClawtTriggerEvent *self, const gchar *ref);
void clawt_trigger_event_set_actor(ClawtTriggerEvent *self,
                                   const gchar       *actor);
void clawt_trigger_event_set_title(ClawtTriggerEvent *self,
                                   const gchar       *title);
void clawt_trigger_event_set_url(ClawtTriggerEvent *self, const gchar *url);
void clawt_trigger_event_set_number(ClawtTriggerEvent *self,
                                    const gchar       *number);
void clawt_trigger_event_set_payload(ClawtTriggerEvent *self,
                                     const gchar       *payload);

/**
 * clawt_trigger_event_set_payload_bytes:
 * @self: a #ClawtTriggerEvent
 * @body: (nullable) (array length=body_length): the raw body
 * @body_length: how many bytes of @body
 *
 * The body as bytes, because an HTTP body is not a C string.
 *
 * Nothing guarantees a delivery is NUL-terminated, and nothing
 * guarantees it holds no NUL either, so the length is carried rather
 * than recovered with strlen(). A body with an embedded NUL is truncated
 * at it *here*, once and deliberately, rather than at whichever of the
 * store, the prompt and the client reads it first.
 */
void clawt_trigger_event_set_payload_bytes(ClawtTriggerEvent *self,
                                           const guchar      *body,
                                           gsize              body_length);

/**
 * clawt_trigger_event_set_identity:
 * @self: a #ClawtTriggerEvent
 * @name: (nullable): the event name
 * @delivery_id: (nullable): the sender's id for this delivery
 *
 * Corrects the two fields a shared normaliser could only guess at.
 *
 * Forgejo emits Gitea- and GitHub-shaped headers as well as its own, so
 * the payload can be read by the shape GitHub defined while the event
 * name and delivery id have to come from whichever header the sender
 * actually set. Passing %NULL leaves a field as it was.
 */
void clawt_trigger_event_set_identity(ClawtTriggerEvent *self,
                                      const gchar       *name,
                                      const gchar       *delivery_id);

/**
 * clawt_trigger_event_placeholder:
 * @self: a #ClawtTriggerEvent
 * @name: a placeholder name without the braces, e.g. "repo"
 *
 * The value a `{{name}}` in an instruction template stands for.
 *
 * One function so the template expander, the docs table and the client
 * hints cannot disagree about which names exist.
 *
 * Returns: (nullable): the value, or %NULL if @name is not a placeholder
 *   this build understands
 */
const gchar *clawt_trigger_event_placeholder(ClawtTriggerEvent *self,
                                             const gchar       *name);

/**
 * clawt_trigger_event_placeholder_count:
 *
 * Returns: how many placeholders a template may use
 */
guint clawt_trigger_event_placeholder_count(void);

/**
 * clawt_trigger_event_placeholder_nth:
 * @n: an index below clawt_trigger_event_placeholder_count()
 *
 * Returns: (transfer none): the name, without braces
 */
const gchar *clawt_trigger_event_placeholder_nth(guint n);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTriggerEvent, clawt_trigger_event_free)

G_END_DECLS
