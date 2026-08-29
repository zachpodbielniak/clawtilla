/*
 * clawt-config.h - clawtilla configuration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawtilla.yaml is the single source of truth for the fleet.  Each agent's
 * libreclaw config.yaml is rendered from what is here, so editing an agent
 * means editing this file -- from the CLI, from the GTK client, or by hand.
 *
 * Access is by dotted path rather than through a property per option.  With
 * a hundred-odd options that would be a hundred-odd properties to declare,
 * parse, default and keep in step with the schema, and the schema already
 * knows every one of their types and defaults.  Going through the schema
 * means an option added to the table works here immediately, and a value
 * absent from the file falls back to the documented default in one place
 * rather than at each call site.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

#include "clawt-types.h"
#include "config/clawt-secret-ref.h"

G_BEGIN_DECLS

#define CLAWT_TYPE_CONFIG (clawt_config_get_type())

G_DECLARE_FINAL_TYPE(ClawtConfig, clawt_config, CLAWT, CONFIG, GObject)

#define CLAWT_TYPE_AGENT_CONFIG (clawt_agent_config_get_type())

GType clawt_agent_config_get_type(void) G_GNUC_CONST;

/* ── Loading and saving ──────────────────────────────────────────── */

/**
 * clawt_config_new:
 *
 * Creates an empty configuration holding schema defaults only.
 *
 * Returns: (transfer full): a new #ClawtConfig
 */
ClawtConfig *clawt_config_new(void);

/**
 * clawt_config_load:
 * @path: (nullable): the file to read, or %NULL for ~/.clawtilla/config.yaml
 * @error: (out) (optional): return location for a #GError
 *
 * Loads a configuration, capturing the author's comments so a later save
 * can put them back.
 *
 * A missing file is not an error: an empty configuration on schema defaults
 * is a valid thing to start a daemon with, and requiring the file to exist
 * first would make the first run a chicken-and-egg problem.
 *
 * Returns: (transfer full) (nullable): the configuration, or %NULL on a
 *   parse error
 */
ClawtConfig *clawt_config_load(const gchar  *path,
                               GError      **error);

/**
 * clawt_config_load_from_string:
 * @yaml: configuration text
 * @error: (out) (optional): return location for a #GError
 *
 * Loads a configuration from memory.  Chiefly for tests.
 *
 * Returns: (transfer full) (nullable): the configuration, or %NULL
 */
ClawtConfig *clawt_config_load_from_string(const gchar  *yaml,
                                           GError      **error);

/**
 * clawt_config_save:
 * @self: a #ClawtConfig
 * @error: (out) (optional): return location for a #GError
 *
 * Writes the configuration back to the file it was loaded from.
 *
 * Written atomically, keeping one ".bak".  Both the schema's documentation
 * and any comments the author added survive: the first because they are
 * re-attached from the schema, the second because they were captured on
 * load.  A save that silently deleted somebody's notes would be worse than
 * refusing to save at all.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_config_save(ClawtConfig  *self,
                           GError      **error);

/**
 * clawt_config_to_string:
 * @self: a #ClawtConfig
 *
 * Renders the configuration as it would be saved.
 *
 * Returns: (transfer full): the YAML text
 */
gchar *clawt_config_to_string(ClawtConfig *self);

/**
 * clawt_config_get_path:
 * @self: a #ClawtConfig
 *
 * Returns: (transfer none) (nullable): the file this was loaded from
 */
const gchar *clawt_config_get_path(ClawtConfig *self);

/**
 * clawt_config_validate:
 * @self: a #ClawtConfig
 * @error: (out) (optional): return location for the first problem
 *
 * Checks the configuration against the schema.
 *
 * Reports unknown keys, values of the wrong type and missing required
 * fields.  Problems inside an agent block are not errors here -- that agent
 * becomes a shadow instead, so one bad agent cannot stop the daemon
 * starting the others.
 *
 * Returns: %TRUE if the configuration is usable
 */
gboolean clawt_config_validate(ClawtConfig  *self,
                               GError      **error);

/**
 * clawt_config_get_warnings:
 * @self: a #ClawtConfig
 *
 * Problems that did not prevent loading: unknown keys, and agents that
 * became shadows.
 *
 * Returns: (transfer none) (element-type utf8): the warnings
 */
GPtrArray *clawt_config_get_warnings(ClawtConfig *self);

/* ── Reading values ──────────────────────────────────────────────── */

/**
 * clawt_config_get_string:
 * @self: a #ClawtConfig
 * @key: a dotted path such as "daemon.socket"
 *
 * Reads a string, falling back to the schema default.
 *
 * Returns: (transfer none) (nullable): the value
 */
const gchar *clawt_config_get_string(ClawtConfig *self, const gchar *key);

/**
 * clawt_config_get_path_value:
 * @self: a #ClawtConfig
 * @key: a dotted path
 *
 * Reads a path, expanding "~" and the XDG variables.
 *
 * Returns: (transfer full) (nullable): the expanded path
 */
gchar *clawt_config_get_path_value(ClawtConfig *self, const gchar *key);

gboolean clawt_config_get_boolean(ClawtConfig *self, const gchar *key);
gint64   clawt_config_get_int(ClawtConfig *self, const gchar *key);
gdouble  clawt_config_get_double(ClawtConfig *self, const gchar *key);

/**
 * clawt_config_get_enum:
 * @self: a #ClawtConfig
 * @key: a dotted path
 *
 * Reads an enum by nickname, falling back to the schema default.
 *
 * Returns: the value, or the enum's first member if the key is unknown
 */
gint clawt_config_get_enum(ClawtConfig *self, const gchar *key);

/**
 * clawt_config_get_string_list:
 * @self: a #ClawtConfig
 * @key: a dotted path
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the strings
 */
GStrv clawt_config_get_string_list(ClawtConfig *self, const gchar *key);

/**
 * clawt_config_has_key:
 * @self: a #ClawtConfig
 * @key: a dotted path
 *
 * Whether the file actually sets @key, as opposed to it having a default.
 *
 * Returns: %TRUE if present in the file
 */
gboolean clawt_config_has_key(ClawtConfig *self, const gchar *key);

/* ── Writing values ──────────────────────────────────────────────── */

gboolean clawt_config_set_string(ClawtConfig *self, const gchar *key,
                                 const gchar *value);
gboolean clawt_config_set_boolean(ClawtConfig *self, const gchar *key,
                                  gboolean value);
gboolean clawt_config_set_int(ClawtConfig *self, const gchar *key,
                              gint64 value);

/**
 * clawt_config_unset:
 * @self: a #ClawtConfig
 * @key: a dotted path
 *
 * Removes @key, so it falls back to its default again.
 *
 * Returns: %TRUE if it was present
 */
gboolean clawt_config_unset(ClawtConfig *self, const gchar *key);

/* ── Agents ──────────────────────────────────────────────────────── */

/**
 * clawt_config_get_agents:
 * @self: a #ClawtConfig
 *
 * Returns: (transfer none) (element-type ClawtAgentConfig): every agent,
 *   in file order
 */
GPtrArray *clawt_config_get_agents(ClawtConfig *self);

/**
 * clawt_config_get_agent:
 * @self: a #ClawtConfig
 * @id: an agent id
 *
 * Returns: (transfer none) (nullable): the agent, or %NULL
 */
ClawtAgentConfig *clawt_config_get_agent(ClawtConfig *self, const gchar *id);

/**
 * clawt_config_add_agent:
 * @self: a #ClawtConfig
 * @id: the new agent's id
 * @error: (out) (optional): return location for a #GError
 *
 * Adds an agent with only its id set.
 *
 * Returns: (transfer none) (nullable): the new agent, or %NULL if @id is
 *   invalid or taken
 */
ClawtAgentConfig *clawt_config_add_agent(ClawtConfig  *self,
                                         const gchar  *id,
                                         GError      **error);

/**
 * clawt_config_remove_agent:
 * @self: a #ClawtConfig
 * @id: an agent id
 *
 * Returns: %TRUE if the agent existed
 */
gboolean clawt_config_remove_agent(ClawtConfig *self, const gchar *id);

/**
 * clawt_config_adopt_libreclaw:
 * @agent: the agent being imported
 * @config_path: a standalone libreclaw config.yaml, which may not exist
 *
 * Takes what a standalone libreclaw instance said about itself.
 *
 * A libreclaw instance run by hand keeps its provider, model and
 * identity files in its own config.yaml. An import that ignored them
 * would quietly move the agent onto the fleet defaults -- the persona
 * would arrive and the way it thinks would not.
 *
 * Only keys the agent has not already been given are taken, and only
 * ones clawtilla owns: the rest of that file is regenerated on every
 * start and anything else in it would be overwritten anyway.
 *
 * Returns: how many settings were adopted
 */
guint clawt_config_adopt_libreclaw(ClawtAgentConfig *agent,
                                   const gchar      *config_path);

/* ── One agent's configuration ───────────────────────────────────── */

/**
 * ClawtRoomSpec:
 * @id: the room id
 * @name: (nullable): display name
 * @members: (array zero-terminated=1) (nullable): agent ids
 * @require_mention: whether members only respond when named
 * @max_hops: this room's hop limit, or 0 to use the global one
 *
 * One entry from the config's `rooms:` list.
 */
typedef struct {
    gchar    *id;
    gchar    *name;
    GStrv     members;
    gboolean  require_mention;
    guint     max_hops;
} ClawtRoomSpec;

void clawt_room_spec_free(ClawtRoomSpec *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtRoomSpec, clawt_room_spec_free)

/**
 * ClawtTeamSpec:
 * @id: the team id, and what `agents.team` names
 * @name: (nullable): display name; the id when unset
 * @description: (nullable): what the team is for, written for whoever
 *   is deciding where a piece of work goes
 * @color: (nullable): accent colour, as a hex string
 * @order: where it sits in a list, lowest first
 *
 * One entry from the config's `teams:` list.
 */
typedef struct {
    gchar *id;
    gchar *name;
    gchar *description;
    gchar *color;
    gint   order;
} ClawtTeamSpec;

void clawt_team_spec_free(ClawtTeamSpec *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTeamSpec, clawt_team_spec_free)

/**
 * clawt_config_get_teams:
 * @self: a #ClawtConfig
 *
 * The teams declared in the config, in the order they should be shown.
 *
 * Returns: (transfer full) (element-type ClawtTeamSpec): the teams
 */
GPtrArray *clawt_config_get_teams(ClawtConfig *self);

/**
 * clawt_config_get_team: (skip)
 * @self: a #ClawtConfig
 * @team_id: which team
 *
 * Returns: (transfer full) (nullable): the team, or %NULL
 */
ClawtTeamSpec *clawt_config_get_team(ClawtConfig *self,
                                     const gchar *team_id);

/**
 * clawt_config_add_team:
 * @self: a #ClawtConfig
 * @team_id: the id for the new team
 * @error: (out) (optional): return location for a #GError
 *
 * Adds a team. Refuses an id that is already taken, and one that is not
 * a usable identifier -- a team is addressed by this from agents, tools
 * and the command line.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_config_add_team(ClawtConfig  *self,
                               const gchar  *team_id,
                               GError      **error);

/**
 * clawt_config_set_team_string:
 * @self: a #ClawtConfig
 * @team_id: which team
 * @key: the field, as it appears in the schema under `teams.`
 * @value: (nullable): the new value
 *
 * Returns: %TRUE when the team exists
 */
gboolean clawt_config_set_team_string(ClawtConfig *self,
                                      const gchar *team_id,
                                      const gchar *key,
                                      const gchar *value);

/**
 * clawt_config_remove_team:
 * @self: a #ClawtConfig
 * @team_id: which team
 *
 * Removes the team. Agents naming it are left alone and become
 * teamless, which is a state they are allowed to be in -- rewriting
 * every one of them from here would be a second thing to get wrong.
 *
 * Returns: %TRUE when a team was removed
 */
gboolean clawt_config_remove_team(ClawtConfig *self, const gchar *team_id);

/**
 * clawt_config_get_rooms:
 * @self: a #ClawtConfig
 *
 * The standing rooms declared in the config.
 *
 * Returns: (transfer full) (element-type ClawtRoomSpec): the rooms
 */
GPtrArray *clawt_config_get_rooms(ClawtConfig *self);

/**
 * clawt_agent_config_get_id:
 * @self: a #ClawtAgentConfig
 *
 * Returns: (transfer none): the agent's id
 */
const gchar *clawt_agent_config_get_id(ClawtAgentConfig *self);

/**
 * clawt_agent_config_get_config:
 * @self: a #ClawtAgentConfig
 *
 * The fleet configuration this agent was read out of.
 *
 * For the few decisions that are the fleet's rather than the agent's.
 * clawt_agent_config_get_string() and its kin resolve an agent-relative
 * key to a fleet one through the schema, which is the right route for
 * anything an agent may override -- but a `daemon.*` key is deliberately
 * not overridable, so there is no agent-relative spelling of it to
 * resolve, and asking for one returns the schema default rather than
 * what the file says.  A caller that needs the fleet's own answer has to
 * ask the fleet.
 *
 * Unowned: a #ClawtConfig outlives the agents it holds.
 *
 * Returns: (transfer none) (nullable): the fleet configuration
 */
ClawtConfig *clawt_agent_config_get_config(ClawtAgentConfig *self);

/**
 * clawt_agent_config_get_string:
 * @self: a #ClawtAgentConfig
 * @key: a path relative to the agent, such as "model.model"
 *
 * Reads a value, falling back first to the matching `defaults.*` key and
 * then to the schema default.
 *
 * The two-step fallback is what makes `defaults:` meaningful: an agent that
 * says nothing about its model should follow the fleet's default, not the
 * schema's.
 *
 * Returns: (transfer none) (nullable): the value
 */
const gchar *clawt_agent_config_get_string(ClawtAgentConfig *self,
                                           const gchar      *key);

gchar   *clawt_agent_config_get_path_value(ClawtAgentConfig *self,
                                           const gchar      *key);
gboolean clawt_agent_config_get_boolean(ClawtAgentConfig *self,
                                        const gchar      *key);
gint64   clawt_agent_config_get_int(ClawtAgentConfig *self,
                                    const gchar      *key);
gint     clawt_agent_config_get_enum(ClawtAgentConfig *self,
                                     const gchar      *key);

/**
 * clawt_agent_config_get_string_list:
 * @self: a #ClawtAgentConfig
 * @key: a path relative to the agent
 *
 * Returns: (transfer full) (nullable) (array zero-terminated=1): the strings
 */
GStrv    clawt_agent_config_get_string_list(ClawtAgentConfig *self,
                                            const gchar      *key);
/**
 * clawt_agent_config_validate_computer:
 * @self: a #ClawtAgentConfig
 * @error: (out) (optional): return location for a #GError
 *
 * Whether this agent's computer could actually work.
 *
 * Only one rule so far, and it earns its place: a VM with no disk image
 * defines, starts and boots nothing.  Provisioning refuses it too, but
 * that is a daemon restart away from the mistake -- far enough that the
 * symptom people report is "the VM was never created", with no visible
 * connection to a field left empty.
 *
 * Called from every path that creates an agent, which is why it is here
 * rather than in any one of them: the daemon's agent.create and the AI
 * designer's commit are separate code, and a rule living in one is a rule
 * the other does not have.
 *
 * Returns: %TRUE if the computer is usable as configured
 */
gboolean clawt_agent_config_validate_computer(ClawtAgentConfig  *self,
                                              GError           **error);

gboolean clawt_agent_config_has_key(ClawtAgentConfig *self,
                                    const gchar      *key);

gboolean clawt_agent_config_set_string(ClawtAgentConfig *self,
                                       const gchar      *key,
                                       const gchar      *value);

/**
 * clawt_agent_config_set_string_list:
 * @self: an agent's configuration
 * @key: a dotted path, relative to the agent
 * @values: (array zero-terminated=1) (nullable): the entries, or %NULL to clear
 *
 * Writes @key as a YAML sequence.
 *
 * clawt_agent_config_set_string() writes a scalar at a dotted path, and
 * the reader refuses anything that is not a sequence -- so setting a
 * list through it was accepted, saved, and then read back as the schema
 * default, with nothing anywhere reporting a problem.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_agent_config_set_string_list(ClawtAgentConfig   *self,
                                            const gchar        *key,
                                            const gchar *const *values);
/**
 * clawt_agent_config_set_from_string:
 * @self: an agent's configuration
 * @key: dotted path below the agent, e.g. `computer.host.allow_paths`
 * @value: (nullable): the value as text; comma-separated for a list key
 *
 * Writes @value as whatever the schema says @key is: a sequence for a
 * %CLAWT_SCHEMA_STRING_LIST key, a scalar for anything else.
 *
 * This is what a caller holding a setting as text wants.
 * clawt_agent_config_set_string() writes a scalar unconditionally, and a
 * list written as a scalar is read back as the schema default -- so the
 * value is accepted, saved, and then quietly not used.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_agent_config_set_from_string(ClawtAgentConfig *self,
                                            const gchar      *key,
                                            const gchar      *value);
gboolean clawt_agent_config_set_boolean(ClawtAgentConfig *self,
                                        const gchar      *key,
                                        gboolean          value);
gboolean clawt_agent_config_set_int(ClawtAgentConfig *self,
                                    const gchar      *key,
                                    gint64            value);

/**
 * clawt_agent_config_get_workspace:
 * @self: a #ClawtAgentConfig
 *
 * The agent's workspace, defaulting to defaults.workspace_root/<id>.
 *
 * Returns: (transfer full): the expanded path
 */
gchar *clawt_agent_config_get_workspace(ClawtAgentConfig *self);

/**
 * clawt_config_get_default_mounts:
 * @self: a #ClawtConfig
 *
 * The fleet's shared folders: `defaults.mounts`.
 *
 * Applied to every agent whose computer takes mounts -- container,
 * distrobox and VM -- so a directory somebody shares with all their
 * agents is written once rather than copied into every agent block and
 * then forgotten on the next one they create.
 *
 * Read through the same parser as the per-agent list, so the two cannot
 * disagree about what an entry means.
 *
 * Returns: (transfer full) (element-type ClawtMount): the mounts, empty
 *   when none are configured
 */
GPtrArray *clawt_config_get_default_mounts(ClawtConfig *self);

/**
 * clawt_config_add_default_mount:
 * @self: a #ClawtConfig
 * @mount: (transfer none): the mount to add
 *
 * Returns: %TRUE if it was written
 */
gboolean clawt_config_add_default_mount(ClawtConfig *self, ClawtMount *mount);

/**
 * clawt_config_remove_default_mount:
 * @self: a #ClawtConfig
 * @target: the path inside the computer
 *
 * Keyed on the target rather than the source, because the target is
 * what has to be unique.
 *
 * Returns: %TRUE if one was removed
 */
gboolean clawt_config_remove_default_mount(ClawtConfig *self,
                                           const gchar *target);

/**
 * clawt_agent_config_get_mounts:
 * @self: a #ClawtAgentConfig
 *
 * Returns: (transfer full) (element-type ClawtMount): the mounts, in order
 */
GPtrArray *clawt_agent_config_get_mounts(ClawtAgentConfig *self);

/**
 * clawt_agent_config_add_mount:
 * @self: an agent's configuration
 * @mount: (transfer none): the mount to add
 *
 * Appends one entry to `computer.mounts`.
 *
 * Mounts are the only list an agent's configuration holds, and
 * clawt_agent_config_set_string() cannot express one -- it writes a
 * scalar at a dotted path. Without this the list could be read and never
 * written, so declaring a shared folder meant editing the YAML by hand.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_agent_config_add_mount(ClawtAgentConfig *self,
                                      ClawtMount       *mount);

/**
 * clawt_agent_config_remove_mount:
 * @self: an agent's configuration
 * @target: the path inside the computer
 *
 * Removes the mount with that target.
 *
 * Keyed on the target rather than the source because the target is what
 * has to be unique: two sources cannot occupy one path inside the
 * computer, and validation already refuses that.
 *
 * Returns: %TRUE if one was removed
 */
gboolean clawt_agent_config_remove_mount(ClawtAgentConfig *self,
                                         const gchar      *target);

/**
 * clawt_agent_config_get_env:
 * @self: a #ClawtAgentConfig
 *
 * Returns: (transfer full) (element-type utf8 utf8): environment variables
 */
GHashTable *clawt_agent_config_get_env(ClawtAgentConfig *self);

/**
 * clawt_agent_config_get_credentials:
 * @self: a #ClawtAgentConfig
 *
 * The agent's secret references, unresolved.
 *
 * Returns: (transfer full) (element-type utf8 ClawtSecretRef): the references
 */
GHashTable *clawt_agent_config_get_credentials(ClawtAgentConfig *self);

/**
 * clawt_agent_config_resolve_credentials:
 * @self: a #ClawtAgentConfig
 * @secrets_dir: (nullable): directory a bare file reference resolves against
 * @timeout_seconds: how long a command backend may take
 * @error: (out) (optional): return location for a #GError
 *
 * Resolves the agent's credentials into environment variables, named
 * after each key in upper case.
 *
 * This is how a provider actually receives its key: `anthropic_api_key`
 * becomes ANTHROPIC_API_KEY in the child's environment.  The values are
 * also written to files by clawt_config_write_agent_files(), for anything
 * that wants a path instead.
 *
 * Returns: (transfer full) (element-type utf8 utf8) (nullable): the
 *   variables, or %NULL if a reference could not be resolved
 */
GHashTable *clawt_agent_config_resolve_credentials(
    ClawtAgentConfig  *self,
    const gchar       *secrets_dir,
    guint              timeout_seconds,
    GError           **error);

/**
 * clawt_agent_config_get_secret:
 * @self: a #ClawtAgentConfig
 * @key: a dotted path within the agent, e.g. `integrations.matrix.access_token`
 *
 * Reads one secret reference from anywhere in the agent's block, for the
 * places a secret sits beside ordinary settings rather than under
 * `credentials:`.
 *
 * Returns: (transfer full) (nullable): the reference, or %NULL if unset
 */
ClawtSecretRef *clawt_agent_config_get_secret(ClawtAgentConfig *self,
                                              const gchar      *key);

/**
 * clawt_agent_config_get_raw_yaml:
 * @self: a #ClawtAgentConfig
 * @key: a dotted path within the agent
 *
 * Serialises a whole subtree back to YAML, unchanged.
 *
 * This is how `libreclaw:` passthrough works: clawtilla does not model
 * libreclaw's every option, so the subtree is copied across verbatim
 * rather than being flattened through a schema that would silently drop
 * whatever it has not heard of.
 *
 * Returns: (transfer full) (nullable): the YAML, or %NULL if absent
 */
gchar *clawt_agent_config_get_raw_yaml(ClawtAgentConfig *self,
                                       const gchar      *key);

/**
 * clawt_agent_config_is_shadow:
 * @self: a #ClawtAgentConfig
 *
 * Whether this agent's configuration could not be understood.
 *
 * A shadow is listed and explains itself but refuses to run.  It is what
 * lets a config written by a newer clawtilla load in an older one: an
 * unknown computer type disables one agent rather than the daemon.
 *
 * Returns: %TRUE if the agent is a shadow
 */
/**
 * clawt_agent_config_revalidate:
 * @self: a #ClawtAgentConfig
 *
 * Retakes the shadow decision after a setting has changed.
 *
 * The decision is otherwise made once, when the config is loaded -- so
 * `agent set` on the very key an agent was shadowed for wrote the value,
 * reported success, and left the agent disabled with the old reason until
 * somebody restarted the daemon.  On a remote daemon that was not a
 * remedy anyone could reach.
 *
 * Returns: %TRUE if the agent is usable, %FALSE if it is still a shadow
 */
gboolean clawt_agent_config_revalidate(ClawtAgentConfig *self);

gboolean clawt_agent_config_is_shadow(ClawtAgentConfig *self);

/**
 * clawt_agent_config_get_shadow_reason:
 * @self: a #ClawtAgentConfig
 *
 * Returns: (transfer none) (nullable): why the agent is a shadow
 */
const gchar *clawt_agent_config_get_shadow_reason(ClawtAgentConfig *self);

ClawtAgentConfig *clawt_agent_config_ref(ClawtAgentConfig *self);
void              clawt_agent_config_unref(ClawtAgentConfig *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtAgentConfig, clawt_agent_config_unref)

/* ── Integration instances ───────────────────────────────────────── */

/**
 * ClawtIntegrationConfig:
 *
 * One entry from the top-level `integrations:` list.
 *
 * Shaped exactly like #ClawtAgentConfig and for the same reason: a thin
 * handle over the instance's YAML mapping, with typed getters that fall
 * back to the schema, rather than a struct of parsed fields that has to be
 * kept in step with the file.
 *
 * Every getter takes an agent id.  An instance may be handed to more than
 * one agent, and the things that must differ between them -- a Matrix user
 * id, a mailbox, a webhook port -- live under `per_agent`, so "the value of
 * this key" is only ever a question with an answer once you say who is
 * asking.  Passing %NULL asks for the instance's own value, which is what
 * a settings dialog wants and what an agent almost never does.
 */

#define CLAWT_TYPE_INTEGRATION_CONFIG (clawt_integration_config_get_type())

GType clawt_integration_config_get_type(void) G_GNUC_CONST;

ClawtIntegrationConfig *clawt_integration_config_ref(ClawtIntegrationConfig *self);
void                    clawt_integration_config_unref(ClawtIntegrationConfig *self);

/**
 * clawt_integration_config_get_name:
 * @self: a #ClawtIntegrationConfig
 *
 * Returns: (transfer none): the instance's name
 */
const gchar *clawt_integration_config_get_name(ClawtIntegrationConfig *self);

/**
 * clawt_integration_config_get_type_id:
 * @self: a #ClawtIntegrationConfig
 *
 * The `type:` field -- "matrix", "mcp" and so on.
 *
 * Named `type_id` rather than `type` because #GType already owns that
 * spelling on a boxed type.
 *
 * Returns: (transfer none) (nullable): the type id
 */
const gchar *clawt_integration_config_get_type_id(ClawtIntegrationConfig *self);

/**
 * clawt_integration_config_get_enabled:
 * @self: a #ClawtIntegrationConfig
 *
 * Returns: %TRUE if the instance is switched on
 */
gboolean clawt_integration_config_get_enabled(ClawtIntegrationConfig *self);

/**
 * clawt_integration_config_get_scope:
 * @self: a #ClawtIntegrationConfig
 *
 * Returns: which agents it reaches
 */
ClawtScope
clawt_integration_config_get_scope(ClawtIntegrationConfig *self);

/**
 * clawt_integration_config_get_agents:
 * @self: a #ClawtIntegrationConfig
 *
 * The ids named in `agents:`, whatever the scope.
 *
 * Returned even when the scope is `all` or `none`, so that switching scope
 * back to `selected` in a dialog does not lose the selection -- a list
 * that empties itself when you look away is not a list anybody trusts.
 *
 * Returns: (transfer full) (array zero-terminated=1): the ids
 */
GStrv clawt_integration_config_get_agents(ClawtIntegrationConfig *self);

/**
 * clawt_integration_config_covers:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: an agent id
 *
 * Whether this instance is handed to @agent_id.
 *
 * Returns: %TRUE if it is enabled and in scope for that agent
 */
gboolean clawt_integration_config_covers(ClawtIntegrationConfig *self,
                                         const gchar            *agent_id);

/**
 * clawt_integration_config_covers_on_team:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: the agent
 * @team: (nullable): the team that agent is on
 *
 * The same question, told which team the agent is on so a
 * `teams:` entry can answer it.
 *
 * Separate from clawt_integration_config_covers() because most callers
 * have an agent id and no team to hand, and one that passed NULL
 * silently would make every `teams:` entry match nothing -- a scope
 * that is configured, reported as configured, and reaches nobody.
 *
 * Returns: %TRUE if the integration applies to that agent
 */
gboolean clawt_integration_config_covers_on_team(
    ClawtIntegrationConfig *self,
    const gchar            *agent_id,
    const gchar            *team);

/**
 * clawt_integration_config_is_shadow:
 * @self: a #ClawtIntegrationConfig
 *
 * Whether this instance could not be understood.
 *
 * The same treatment a shadow agent gets: an unknown type or a missing
 * name disables one integration and explains itself, rather than stopping
 * the daemon.
 *
 * Returns: %TRUE if it refuses to be used
 */
gboolean clawt_integration_config_is_shadow(ClawtIntegrationConfig *self);

/**
 * clawt_integration_config_get_shadow_reason:
 * @self: a #ClawtIntegrationConfig
 *
 * Returns: (transfer none) (nullable): why it is a shadow
 */
const gchar *
clawt_integration_config_get_shadow_reason(ClawtIntegrationConfig *self);

/**
 * clawt_integration_config_get_string:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: (nullable): whose value to read, or %NULL for the instance's own
 * @key: a key relative to the instance, such as "homeserver"
 *
 * Reads a value, preferring @agent_id's override under `per_agent`, then
 * the instance's own, then the schema default.
 *
 * Returns: (transfer none) (nullable): the value
 */
const gchar *clawt_integration_config_get_string(ClawtIntegrationConfig *self,
                                                 const gchar            *agent_id,
                                                 const gchar            *key);

gboolean clawt_integration_config_get_boolean(ClawtIntegrationConfig *self,
                                              const gchar            *agent_id,
                                              const gchar            *key);

gint64   clawt_integration_config_get_int(ClawtIntegrationConfig *self,
                                          const gchar            *agent_id,
                                          const gchar            *key);

/**
 * clawt_integration_config_get_string_list:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: (nullable): whose value to read
 * @key: a key relative to the instance
 *
 * Returns: (transfer full) (array zero-terminated=1): the values, never %NULL
 */
GStrv    clawt_integration_config_get_string_list(ClawtIntegrationConfig *self,
                                                  const gchar            *agent_id,
                                                  const gchar            *key);

/**
 * clawt_integration_config_get_mapping:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: (nullable): whose value to read
 * @key: a key relative to the instance
 *
 * A free-form mapping such as `env`.
 *
 * Merged rather than replaced when an agent overrides it: an override that
 * silently dropped the keys it did not mention would mean repeating the
 * whole block to change one variable.
 *
 * Returns: (transfer full) (element-type utf8 utf8): the entries
 */
GHashTable *clawt_integration_config_get_mapping(ClawtIntegrationConfig *self,
                                                 const gchar            *agent_id,
                                                 const gchar            *key);

/**
 * clawt_integration_config_get_secret:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: (nullable): whose value to read
 * @key: a key relative to the instance
 *
 * Returns: (transfer full) (nullable): the reference, or %NULL if unset
 */
ClawtSecretRef *clawt_integration_config_get_secret(ClawtIntegrationConfig *self,
                                                    const gchar            *agent_id,
                                                    const gchar            *key);

gboolean clawt_integration_config_has_key(ClawtIntegrationConfig *self,
                                          const gchar            *agent_id,
                                          const gchar            *key);

/**
 * clawt_integration_config_set_string:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: (nullable): whose value to write, or %NULL for the instance's own
 * @key: a key relative to the instance
 * @value: (nullable): the value, or %NULL to unset it
 *
 * Returns: %TRUE if the file changed
 */
gboolean clawt_integration_config_set_string(ClawtIntegrationConfig *self,
                                             const gchar            *agent_id,
                                             const gchar            *key,
                                             const gchar            *value);

gboolean clawt_integration_config_set_boolean(ClawtIntegrationConfig *self,
                                              const gchar            *agent_id,
                                              const gchar            *key,
                                              gboolean                value);

gboolean clawt_integration_config_set_int(ClawtIntegrationConfig *self,
                                          const gchar            *agent_id,
                                          const gchar            *key,
                                          gint64                  value);

/**
 * clawt_integration_config_set_string_list:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: (nullable): whose value to write
 * @key: a key relative to the instance
 * @values: (nullable) (array zero-terminated=1): the values, or %NULL to unset
 *
 * Returns: %TRUE if the file changed
 */
gboolean clawt_integration_config_set_string_list(ClawtIntegrationConfig *self,
                                                  const gchar            *agent_id,
                                                  const gchar            *key,
                                                  const gchar *const     *values);

/**
 * clawt_integration_config_set_secret:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: (nullable): whose value to write
 * @key: a key relative to the instance
 * @backend: which secret backend
 * @locator: (nullable): the file, variable or command, or %NULL to unset
 *
 * Writes a secret *reference*.  There is no way to write a secret's value
 * here, which is the point of the type.
 *
 * Returns: %TRUE if the file changed
 */
gboolean clawt_integration_config_set_secret(ClawtIntegrationConfig *self,
                                             const gchar            *agent_id,
                                             const gchar            *key,
                                             ClawtSecretBackend      backend,
                                             const gchar            *locator);

/**
 * clawt_integration_config_set_scope:
 * @self: a #ClawtIntegrationConfig
 * @scope: who should get it
 * @agents: (nullable) (array zero-terminated=1): ids, for %CLAWT_SCOPE_SELECTED
 *
 * Returns: %TRUE if the file changed
 */
gboolean clawt_integration_config_set_scope(ClawtIntegrationConfig *self,
                                            ClawtScope   scope,
                                            const gchar *const     *agents);

/**
 * clawt_integration_config_set_enabled:
 * @self: a #ClawtIntegrationConfig
 * @enabled: whether it is live
 *
 * Returns: %TRUE if the file changed
 */
gboolean clawt_integration_config_set_enabled(ClawtIntegrationConfig *self,
                                              gboolean                enabled);

/**
 * clawt_integration_config_resolve_env:
 * @self: a #ClawtIntegrationConfig
 * @agent_id: (nullable): whose values to read
 * @key: the mapping key, in practice "env"
 * @secrets_dir: (nullable): where a `{file: ...}` reference is relative to
 * @error: (out) (optional): return location for a #GError
 *
 * A mapping whose values may be literals or secret references, with the
 * references fetched.
 *
 * Separate from clawt_integration_config_get_mapping() because it can
 * fail and because it holds real secrets: the result goes straight into
 * a 0600 file and nowhere else.
 *
 * Returns: (transfer full) (nullable) (element-type utf8 utf8): the
 *   resolved entries, or %NULL on the first that could not be fetched
 */
GHashTable *
clawt_integration_config_resolve_env(ClawtIntegrationConfig  *self,
                                     const gchar             *agent_id,
                                     const gchar             *key,
                                     const gchar             *secrets_dir,
                                     GError                 **error);

/**
 * clawt_config_get_integrations:
 * @self: a #ClawtConfig
 *
 * Every instance in the file, including shadows.
 *
 * Returns: (transfer none) (element-type ClawtIntegrationConfig): the instances
 */
GPtrArray *clawt_config_get_integrations(ClawtConfig *self);

/**
 * clawt_config_get_integration:
 * @self: a #ClawtConfig
 * @name: an instance name
 *
 * Returns: (transfer none) (nullable): the instance, or %NULL
 */
ClawtIntegrationConfig *clawt_config_get_integration(ClawtConfig *self,
                                                     const gchar *name);

/**
 * clawt_config_add_integration:
 * @self: a #ClawtConfig
 * @name: a name unique in the file
 * @type_id: which kind: "matrix", "email", "webhook", "local", "cmacs", "mcp"
 * @error: (out) (optional): return location for a #GError
 *
 * Adds an instance and returns it, ready to have its keys set.
 *
 * As with an agent, this changes the in-memory config only:
 * clawt_config_save() writes it, and the daemon has to reload before
 * anything is handed to an agent.
 *
 * Returns: (transfer none) (nullable): the new instance, or %NULL
 */
ClawtIntegrationConfig *clawt_config_add_integration(ClawtConfig  *self,
                                                     const gchar  *name,
                                                     const gchar  *type_id,
                                                     GError      **error);

/**
 * clawt_config_remove_integration:
 * @self: a #ClawtConfig
 * @name: an instance name
 *
 * Returns: %TRUE if it was there
 */
gboolean clawt_config_remove_integration(ClawtConfig *self,
                                         const gchar *name);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtIntegrationConfig,
                              clawt_integration_config_unref)

/* ── Routines ────────────────────────────────────────────────────── */

/**
 * ClawtRoutine:
 *
 * One entry from the top-level `routines:` list.
 *
 * The same handle shape as #ClawtIntegrationConfig, and for the same
 * reason: typed getters over the entry's own YAML, falling back to the
 * schema, rather than a parsed struct that has to be kept in step with
 * the file.
 */

#define CLAWT_TYPE_ROUTINE (clawt_routine_get_type())

GType clawt_routine_get_type(void) G_GNUC_CONST;

ClawtRoutine *clawt_routine_ref(ClawtRoutine *self);
void          clawt_routine_unref(ClawtRoutine *self);

const gchar *clawt_routine_get_id(ClawtRoutine *self);

const gchar *clawt_routine_get_string(ClawtRoutine *self, const gchar *key);
gboolean     clawt_routine_get_boolean(ClawtRoutine *self, const gchar *key);
gint64       clawt_routine_get_int(ClawtRoutine *self, const gchar *key);
gboolean     clawt_routine_has_key(ClawtRoutine *self, const gchar *key);

gboolean clawt_routine_set_string(ClawtRoutine *self, const gchar *key,
                                  const gchar *value);
gboolean clawt_routine_set_boolean(ClawtRoutine *self, const gchar *key,
                                   gboolean value);
gboolean clawt_routine_set_int(ClawtRoutine *self, const gchar *key,
                               gint64 value);

/**
 * clawt_routine_get_cron:
 * @self: a #ClawtRoutine
 * @error: (out) (optional): return location for a #GError
 *
 * The cron expression this routine's schedule means.
 *
 * %NULL with no error set for a manual routine: it has no next time,
 * which is an answer rather than a failure to compute one.
 *
 * Returns: (transfer full) (nullable): the expression
 */
gchar *clawt_routine_get_cron(ClawtRoutine *self, GError **error);

/**
 * clawt_config_get_routines:
 * @self: a #ClawtConfig
 *
 * Returns: (transfer none) (element-type ClawtRoutine): the routines
 */
GPtrArray *clawt_config_get_routines(ClawtConfig *self);

/**
 * clawt_config_get_routine:
 * @self: a #ClawtConfig
 * @id: a routine id
 *
 * Returns: (transfer none) (nullable): the routine
 */
ClawtRoutine *clawt_config_get_routine(ClawtConfig *self, const gchar *id);

/**
 * clawt_config_add_routine:
 * @self: a #ClawtConfig
 * @id: an id unique in the file
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer none) (nullable): the new routine
 */
ClawtRoutine *clawt_config_add_routine(ClawtConfig  *self,
                                       const gchar  *id,
                                       GError      **error);

gboolean clawt_config_remove_routine(ClawtConfig *self, const gchar *id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtRoutine, clawt_routine_unref)

/* ── Triggers ────────────────────────────────────────────────────── */

/**
 * ClawtTrigger:
 *
 * One entry from the top-level `triggers:` list.
 *
 * The same handle shape as #ClawtRoutine -- typed getters over the
 * entry's own YAML, falling back to the schema -- with the two getters a
 * routine has no use for: `events` is a list and `secret` is a
 * reference, and writing either as a scalar would be accepted, saved,
 * and read back as the default.
 *
 * What a trigger has *done* is deliberately not here. The endpoint it
 * answers on, whether it has been verified, and every delivery receipt
 * live in #ClawtTriggerStore, because run state in a config file is a
 * file that rewrites itself.
 */

#define CLAWT_TYPE_TRIGGER (clawt_trigger_get_type())

GType clawt_trigger_get_type(void) G_GNUC_CONST;

ClawtTrigger *clawt_trigger_ref(ClawtTrigger *self);
void          clawt_trigger_unref(ClawtTrigger *self);

const gchar *clawt_trigger_get_id(ClawtTrigger *self);

const gchar *clawt_trigger_get_string(ClawtTrigger *self, const gchar *key);
gboolean     clawt_trigger_get_boolean(ClawtTrigger *self, const gchar *key);
gint64       clawt_trigger_get_int(ClawtTrigger *self, const gchar *key);
gboolean     clawt_trigger_has_key(ClawtTrigger *self, const gchar *key);

/**
 * clawt_trigger_get_string_list:
 * @self: a #ClawtTrigger
 * @key: a key below `triggers.`
 *
 * Returns: (transfer full): the list, never %NULL and possibly empty
 */
GStrv clawt_trigger_get_string_list(ClawtTrigger *self, const gchar *key);

/**
 * clawt_trigger_get_secret:
 * @self: a #ClawtTrigger
 * @key: a key below `triggers.`
 *
 * The reference, never the secret. A reference that will not parse is a
 * warning and %NULL, so the trigger goes on authenticating nothing --
 * falling back to "no secret needed" would turn a typo into a public
 * endpoint.
 *
 * Returns: (transfer full) (nullable): the reference
 */
ClawtSecretRef *clawt_trigger_get_secret(ClawtTrigger *self,
                                         const gchar  *key);

/**
 * clawt_trigger_get_provider:
 * @self: a #ClawtTrigger
 *
 * Who this trigger expects to be called by.
 *
 * An unreadable value reads as %CLAWT_TRIGGER_PROVIDER_GENERIC, which is
 * the strictest of the five: it requires a bearer token and understands
 * no forge's headers, so a misspelled provider accepts less rather than
 * more.
 *
 * Returns: the provider
 */
ClawtTriggerProvider clawt_trigger_get_provider(ClawtTrigger *self);

gboolean clawt_trigger_set_string(ClawtTrigger *self, const gchar *key,
                                  const gchar *value);
gboolean clawt_trigger_set_boolean(ClawtTrigger *self, const gchar *key,
                                   gboolean value);
gboolean clawt_trigger_set_int(ClawtTrigger *self, const gchar *key,
                               gint64 value);

/**
 * clawt_trigger_set_string_list:
 * @self: a #ClawtTrigger
 * @key: a key below `triggers.`
 * @values: (nullable) (array zero-terminated=1): the list, or %NULL to unset
 *
 * Returns: %TRUE if it was written
 */
gboolean clawt_trigger_set_string_list(ClawtTrigger       *self,
                                       const gchar        *key,
                                       const gchar *const *values);

/**
 * clawt_trigger_set_secret:
 * @self: a #ClawtTrigger
 * @key: a key below `triggers.`
 * @backend: how it will be resolved
 * @locator: (nullable): the path, variable or command, or %NULL to unset
 *
 * Writes a reference. There is no spelling that writes a secret's value
 * into the config, for the reason #ClawtSecretRef exists.
 *
 * Returns: %TRUE if it was written
 */
gboolean clawt_trigger_set_secret(ClawtTrigger       *self,
                                  const gchar        *key,
                                  ClawtSecretBackend  backend,
                                  const gchar        *locator);

/**
 * clawt_config_get_triggers:
 * @self: a #ClawtConfig
 *
 * Returns: (transfer none) (element-type ClawtTrigger): the triggers
 */
GPtrArray *clawt_config_get_triggers(ClawtConfig *self);

/**
 * clawt_config_get_trigger:
 * @self: a #ClawtConfig
 * @id: a trigger id
 *
 * Returns: (transfer none) (nullable): the trigger
 */
ClawtTrigger *clawt_config_get_trigger(ClawtConfig *self, const gchar *id);

/**
 * clawt_config_add_trigger:
 * @self: a #ClawtConfig
 * @id: an id unique in the file
 * @error: (out) (optional): return location for a #GError
 *
 * Returns: (transfer none) (nullable): the new trigger
 */
ClawtTrigger *clawt_config_add_trigger(ClawtConfig  *self,
                                       const gchar  *id,
                                       GError      **error);

gboolean clawt_config_remove_trigger(ClawtConfig *self, const gchar *id);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtTrigger, clawt_trigger_unref)

G_END_DECLS
