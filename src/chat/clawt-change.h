/*
 * clawt-change.h - A change to an agent's configuration, said in its chat
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_CHANGE_H
#define CLAWT_CHANGE_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/**
 * ClawtChangeKind:
 * @CLAWT_CHANGE_SETTING: one of the agent's own settings
 * @CLAWT_CHANGE_ROUTINE: a routine that runs as the agent
 * @CLAWT_CHANGE_TRIGGER: a trigger that delivers to the agent
 * @CLAWT_CHANGE_MOUNT: a path mounted into the agent's computer
 * @CLAWT_CHANGE_INTEGRATION: an integration bound to the agent
 * @CLAWT_CHANGE_SKILL: the skills assigned to the agent
 * @CLAWT_CHANGE_PERSONA: one of the agent's persona files
 * @CLAWT_CHANGE_TEAM: the agent's team or lead
 * @CLAWT_CHANGE_CREATED: the agent itself was created
 * @CLAWT_CHANGE_REMOVED: the agent itself was removed
 *
 * What a change was to.  Each has an icon and a label from
 * clawt_change_kind_icon() and clawt_change_kind_label(), which every
 * client draws from, so the row looks the same in all of them.
 */
typedef enum {
	CLAWT_CHANGE_SETTING = 0,
	CLAWT_CHANGE_ROUTINE,
	CLAWT_CHANGE_TRIGGER,
	CLAWT_CHANGE_MOUNT,
	CLAWT_CHANGE_INTEGRATION,
	CLAWT_CHANGE_SKILL,
	CLAWT_CHANGE_PERSONA,
	CLAWT_CHANGE_TEAM,
	CLAWT_CHANGE_CREATED,
	CLAWT_CHANGE_REMOVED
} ClawtChangeKind;

GType clawt_change_kind_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_CHANGE_KIND (clawt_change_kind_get_type())

/**
 * ClawtConfigNotes:
 * @CLAWT_CONFIG_NOTES_AGENTS: note changes an agent or another fleet made
 * @CLAWT_CONFIG_NOTES_ALL: note the operator's changes too
 * @CLAWT_CONFIG_NOTES_OFF: note nothing
 *
 * `orchestration.config_notes`: which changes to an agent's
 * configuration are noted in its chat.
 */
typedef enum {
	CLAWT_CONFIG_NOTES_AGENTS = 0,
	CLAWT_CONFIG_NOTES_ALL,
	CLAWT_CONFIG_NOTES_OFF
} ClawtConfigNotes;

GType clawt_config_notes_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_CONFIG_NOTES (clawt_config_notes_get_type())

/**
 * CLAWT_CHANGE_NOTE_LINES:
 *
 * The most changes one note lists line by line; the rest are counted.
 * One save can change a dozen keys, and a note longer than the screen
 * buries the conversation it sits in.
 */
#define CLAWT_CHANGE_NOTE_LINES (5)

/**
 * CLAWT_CHANGE_VALUE_CHARS:
 *
 * The longest a value is shown in a note's detail, in characters.
 */
#define CLAWT_CHANGE_VALUE_CHARS (80)

/**
 * ClawtChange:
 * @kind: what was changed
 * @agent: the agent the change happened to
 * @verb: what happened: added, removed, updated, enabled, disabled, set,
 *   cleared, created
 * @label: (nullable): which one: a routine's description, a key's path
 * @detail: (nullable): the old and new value, or the fields that changed;
 *   a credential's value is never in it
 *
 * One change to one agent's configuration, as found by comparing the
 * configuration before and after a save.
 */
typedef struct {
	ClawtChangeKind  kind;
	gchar           *agent;
	gchar           *verb;
	gchar           *label;
	gchar           *detail;
} ClawtChange;

GType clawt_change_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_CHANGE (clawt_change_get_type())

/**
 * clawt_change_new:
 * @kind: what was changed
 * @agent: the agent it happened to
 * @verb: what happened
 * @label: (nullable): which one
 * @detail: (nullable): old and new
 *
 * Returns: (transfer full): a new #ClawtChange
 */
ClawtChange *
clawt_change_new(
	ClawtChangeKind  kind,
	const gchar     *agent,
	const gchar     *verb,
	const gchar     *label,
	const gchar     *detail
);

/**
 * clawt_change_copy:
 * @self: a #ClawtChange
 *
 * Returns: (transfer full): a copy of @self
 */
ClawtChange *
clawt_change_copy(
	const ClawtChange *self
);

/**
 * clawt_change_free:
 * @self: (nullable): a #ClawtChange
 *
 * Frees @self.
 */
void
clawt_change_free(
	ClawtChange *self
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtChange, clawt_change_free)

/**
 * clawt_change_kind_icon:
 * @kind: a #ClawtChangeKind
 *
 * Returns: (transfer none): the one-character icon a note starts with
 */
const gchar *
clawt_change_kind_icon(
	ClawtChangeKind kind
);

/**
 * clawt_change_kind_label:
 * @kind: a #ClawtChangeKind
 *
 * Returns: (transfer none): the noun a note uses: "routine", "setting"
 */
const gchar *
clawt_change_kind_label(
	ClawtChangeKind kind
);

/**
 * clawt_change_kind_from_nick:
 * @nick: (nullable): a kind's nickname, as the `change` message member
 *   carries it
 * @out_kind: (out): where the kind is written
 *
 * Returns: %TRUE when @nick names a kind
 */
gboolean
clawt_change_kind_from_nick(
	const gchar     *nick,
	ClawtChangeKind *out_kind
);

/**
 * clawt_change_key_is_credential:
 * @path: the key's full schema path (`agents.env.API_KEY`)
 * @key: the last segment of @path
 *
 * Whether a key holds a credential: by its schema type, or -- for keys
 * the schema types as plain strings -- by its name.  One rule, shared by
 * the fleet pack's export and the change notes, so the two cannot
 * disagree about what is a secret.
 *
 * Returns: %TRUE when the value must never be shown
 */
gboolean
clawt_change_key_is_credential(
	const gchar *path,
	const gchar *key
);

/**
 * clawt_change_config_to_json:
 * @yaml_text: (nullable): a clawtilla.yaml's text
 *
 * Parses a configuration into JSON for clawt_change_diff().  A text that
 * does not parse is an empty object, so a broken file compares as
 * having no agents rather than failing the save that wrote it.
 *
 * Returns: (transfer full): the configuration as JSON
 */
JsonNode *
clawt_change_config_to_json(
	const gchar *yaml_text
);

/**
 * clawt_change_diff:
 * @before: a configuration, from clawt_change_config_to_json()
 * @after: the same configuration after a save
 *
 * Every change to an agent between @before and @after: its own
 * settings (walked whole, so a key added to the schema later is seen
 * without a list here), routines and triggers matched by id and told to
 * the agent they run as or deliver to, and agents created or removed.
 * Fleet-wide settings are not about one agent and are not reported.
 *
 * Returns: (transfer container) (element-type ClawtChange): the
 *   changes, grouped by agent in the order they were found
 */
GPtrArray *
clawt_change_diff(
	JsonNode *before,
	JsonNode *after
);

/**
 * clawt_change_note_body:
 * @changes: (element-type ClawtChange): changes to one agent
 * @actor: (nullable): who made them: an agent id, `user`, `system` or
 *   `fleet:ALIAS:AGENT`
 * @via: (nullable): `cli` when an agent made them through the CLI
 *
 * The text a change note carries: a line per change, up to
 * %CLAWT_CHANGE_NOTE_LINES, each starting with its kind's icon, then who
 * made them.  This is what the CLI and an older client show; a current
 * client draws the same text as a compact row.
 *
 * Returns: (transfer full): the note's body
 */
gchar *
clawt_change_note_body(
	GPtrArray   *changes,
	const gchar *actor,
	const gchar *via
);

/**
 * clawt_change_should_note:
 * @mode: (nullable): `orchestration.config_notes`: `agents`, `all` or
 *   `off`
 * @actor: (nullable): who made the change, or %NULL when nobody said
 *   (a hand edit of the file)
 * @subject: the agent the change happened to
 *
 * Whether a change is noted in the agent's chat.  Under `agents` (the
 * default) only a change an agent or another fleet made is: the operator
 * already knows what they changed, and a hand edit has nobody to name.
 *
 * Returns: %TRUE to note it
 */
gboolean
clawt_change_should_note(
	const gchar *mode,
	const gchar *actor,
	const gchar *subject
);

/**
 * clawt_change_actor_push:
 * @actor: who is making the changes about to be saved
 * @via: (nullable): `cli` when the operator's credential carried an
 *   agent's hint rather than the agent's own call
 *
 * Says who is acting until the matching clawt_change_actor_pop().  Each
 * door that can lead to a save pushes here -- a tool call as its agent,
 * an IPC request as `user` -- and the innermost wins, so a tool call
 * arriving over IPC is the agent's.  Main context only: saves happen
 * there, and so do both doors.
 */
void
clawt_change_actor_push(
	const gchar *actor,
	const gchar *via
);

/**
 * clawt_change_actor_pop:
 *
 * Ends the innermost clawt_change_actor_push().
 */
void
clawt_change_actor_pop(void);

/**
 * clawt_change_actor_get:
 *
 * Returns: (transfer none) (nullable): who is acting now, or %NULL when
 *   no door said
 */
const gchar *
clawt_change_actor_get(void);

/**
 * clawt_change_actor_get_via:
 *
 * Returns: (transfer none) (nullable): how the current actor acted
 */
const gchar *
clawt_change_actor_get_via(void);

G_END_DECLS

#endif /* CLAWT_CHANGE_H */
