/*
 * clawt-integration-field.h - What to ask somebody adding an integration
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

G_BEGIN_DECLS

/**
 * ClawtFieldKind:
 * @CLAWT_FIELD_TEXT: one line of text
 * @CLAWT_FIELD_SECRET: a reference to a credential, never the credential
 * @CLAWT_FIELD_INT: a number
 * @CLAWT_FIELD_BOOLEAN: on or off
 * @CLAWT_FIELD_CHOICE: exactly one of @choices
 * @CLAWT_FIELD_LIST: several values, comma separated
 * @CLAWT_FIELD_FLAGS: any number of @choices, comma separated
 *
 * What kind of control a field wants.
 *
 * Deliberately about the *control* rather than about the schema type it
 * happens to have.  `backend` and `priority` are both strings in the
 * schema and one of them is a fixed set of five while the other is a
 * fixed set of four; `rooms` and `args` are both string lists and one is
 * a set of room ids while the other is a command line.  A client that
 * dispatched on the schema type would draw all four the same way.
 */
typedef enum {
    CLAWT_FIELD_TEXT = 0,
    CLAWT_FIELD_SECRET,
    CLAWT_FIELD_INT,
    CLAWT_FIELD_BOOLEAN,
    CLAWT_FIELD_CHOICE,
    CLAWT_FIELD_LIST,
    CLAWT_FIELD_FLAGS
} ClawtFieldKind;

/**
 * ClawtIntegrationField:
 * @type: the integration type this belongs to
 * @key: the config key, which is also the member name on the wire
 * @label: what to call it in front of a person
 * @hint: (nullable): one short line under it
 * @example: (nullable): a placeholder showing the shape of a real value
 * @kind: which control to draw
 * @choices: (array zero-terminated=1) (nullable): the values, for a
 *   %CLAWT_FIELD_CHOICE or %CLAWT_FIELD_FLAGS
 * @choice_labels: (array zero-terminated=1) (nullable): what to call each
 *   of @choices, in the same order
 * @when_key: (nullable): another field in the same type
 * @when_value: (nullable): the values of @when_key this field applies to,
 *   comma separated
 * @required: whether the integration cannot work without it
 *
 * One thing to ask about, and how to ask it.
 *
 * This exists because both clients were rendering configuration keys
 * straight at people.  The web editor labelled its inputs `imap_host`,
 * `smtp_port`, `access_token` and `user_id` and used the schema's whole
 * documentation paragraph as the placeholder; the GTK notify editor drew
 * every backend's fields at once, so choosing "Desktop notification"
 * still asked for a Matrix homeserver and a room.  Neither is a bug in
 * any one place -- they are what happens when a form is generated from a
 * data model rather than written for the person filling it in.
 *
 * In the library rather than in either client for the usual reason: it
 * is a rule both apply, and two copies of a form's labels is two forms
 * that will eventually disagree about what a field is called.
 *
 * @when_key is the other half of the complaint.  A field that does not
 * apply to what you have chosen is worse than a missing one: it reads as
 * something you have failed to fill in.  Two entries may share a @key
 * with different @when_value -- notify's `url` is a topic under ntfy and
 * a server under Gotify, and calling both "URL" with one hint is how you
 * get a form that is accurate and useless.
 *
 * It is *not* a second copy of the schema.  Every @key here must be a
 * real `integrations.*` schema key -- that is what the daemon accepts --
 * and tests/test-integration-field.c fails if one is not, if a type's
 * required or credential keys have no field, or if a credential key is
 * described as anything but a %CLAWT_FIELD_SECRET.
 */
typedef struct {
    const gchar        *type;
    const gchar        *key;
    const gchar        *label;
    const gchar        *hint;
    const gchar        *example;
    ClawtFieldKind      kind;
    const gchar *const *choices;
    const gchar *const *choice_labels;
    const gchar        *when_key;
    const gchar        *when_value;
    gboolean            required;
} ClawtIntegrationField;

/**
 * clawt_integration_fields:
 * @type: an integration type id
 * @n_fields: (out): how many were returned
 *
 * The fields of one type, in the order they should be asked for.
 *
 * Returns: (array length=n_fields): the fields, or an empty array for a
 *   type that has none of its own
 */
const ClawtIntegrationField *clawt_integration_fields(const gchar *type,
                                                      gsize       *n_fields);

/**
 * clawt_integration_field_applies:
 * @field: a #ClawtIntegrationField
 * @when_value: (nullable): the current value of @field's `when_key`
 *
 * Whether this field is worth showing.
 *
 * @when_value is what the caller read for @field->when_key, already
 * resolved to its effective value -- a notify instance with no `backend`
 * set is on the schema default, and passing %NULL for it would hide
 * every field of the backend it is actually using.
 *
 * Returns: %TRUE when the field applies
 */
gboolean clawt_integration_field_applies(const ClawtIntegrationField *field,
                                         const gchar                 *when_value);

/**
 * clawt_integration_field_default:
 * @field: a #ClawtIntegrationField
 *
 * The value a field has when nothing has been chosen.
 *
 * The first of @choices, for a choice; %NULL otherwise.  Wanted because
 * a client resolving `when_key` has to resolve it the same way the
 * daemon will, and "unset" is not a value either of them uses.
 *
 * Returns: (nullable) (transfer none): the default
 */
const gchar *clawt_integration_field_default(const ClawtIntegrationField *field);

/**
 * clawt_integration_needs_summary:
 * @type: an integration type id
 *
 * What somebody will have to supply, as prose: "Needs a homeserver, a
 * user ID and an access token."
 *
 * For the moment *before* a type is chosen, which is where neither
 * client said anything at all -- so picking one was a guess, and finding
 * out what it wanted meant creating it first.
 *
 * Returns: (transfer full) (nullable): the sentence, or %NULL for a type
 *   that needs nothing
 */
gchar *clawt_integration_needs_summary(const gchar *type);

/**
 * clawt_integration_type_label:
 * @type: an integration type id
 *
 * The type's name as a person would say it: "Matrix", "Email", "MCP
 * server".
 *
 * The ids are lowercase config values and both clients were showing them
 * as headings and as the entries of a picker.  `mcp` and `notify` are
 * not words.
 *
 * Returns: (transfer none): the label, or @type itself when it is not a
 *   type this build knows -- a plugin's, most likely, and its own id is
 *   a better answer than a blank
 */
const gchar *clawt_integration_type_label(const gchar *type);

G_END_DECLS
