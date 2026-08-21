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
gboolean clawt_agent_config_has_key(ClawtAgentConfig *self,
                                    const gchar      *key);

gboolean clawt_agent_config_set_string(ClawtAgentConfig *self,
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
 * clawt_agent_config_get_mounts:
 * @self: a #ClawtAgentConfig
 *
 * Returns: (transfer full) (element-type ClawtMount): the mounts, in order
 */
GPtrArray *clawt_agent_config_get_mounts(ClawtAgentConfig *self);

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

G_END_DECLS
