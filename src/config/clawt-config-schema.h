/*
 * clawt-config-schema.h - The single source of truth for configuration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every configuration option clawtilla understands is declared once, in the
 * table in clawt-config-schema.c.  Five things are generated from it:
 *
 *   - data/example-config.yaml   every option, its default, its comment
 *   - data/default-config.yaml   what `clawtilla --generate-config` prints
 *   - docs/configuration-options.org   the reference table
 *   - validation                 unknown and malformed keys, with line numbers
 *   - the canonical comments re-attached when the config is written back
 *
 * The alternative -- a hand-written YAML file, a hand-written docs table and
 * a hand-written validator, all describing the same options -- is three
 * things to keep in step and no way to notice when they drift.  Here a test
 * fails instead.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * ClawtSchemaType:
 * @CLAWT_SCHEMA_STRING: a string
 * @CLAWT_SCHEMA_BOOLEAN: true or false
 * @CLAWT_SCHEMA_INT: a whole number
 * @CLAWT_SCHEMA_DOUBLE: a fractional number
 * @CLAWT_SCHEMA_ENUM: one of a fixed set of nicknames
 * @CLAWT_SCHEMA_STRING_LIST: a sequence of strings
 * @CLAWT_SCHEMA_PATH: a filesystem path; "~" is expanded
 * @CLAWT_SCHEMA_SECRET: a secret reference, never a literal value
 * @CLAWT_SCHEMA_MAPPING: a free-form mapping whose keys we do not constrain
 * @CLAWT_SCHEMA_SECTION: a container for other options, not a value itself
 * @CLAWT_SCHEMA_LIST_OF: a sequence of mappings described by a sub-schema
 *
 * The kind of value an option holds.
 */
typedef enum {
    CLAWT_SCHEMA_SECTION = 0,
    CLAWT_SCHEMA_STRING,
    CLAWT_SCHEMA_BOOLEAN,
    CLAWT_SCHEMA_INT,
    CLAWT_SCHEMA_DOUBLE,
    CLAWT_SCHEMA_ENUM,
    CLAWT_SCHEMA_STRING_LIST,
    CLAWT_SCHEMA_PATH,
    CLAWT_SCHEMA_SECRET,
    CLAWT_SCHEMA_MAPPING,
    CLAWT_SCHEMA_LIST_OF
} ClawtSchemaType;

/**
 * ClawtSchemaFlags:
 * @CLAWT_SCHEMA_FLAG_NONE: nothing special
 * @CLAWT_SCHEMA_FLAG_REQUIRED: the config is invalid without it
 * @CLAWT_SCHEMA_FLAG_COMMENTED: written out commented in the starter config
 * @CLAWT_SCHEMA_FLAG_DANGEROUS: enabling it hands over real authority
 * @CLAWT_SCHEMA_FLAG_PER_AGENT: also valid inside an agent block
 *
 * @CLAWT_SCHEMA_FLAG_COMMENTED is what keeps `--generate-config` usable:
 * a starter file that set every option would be a wall of defaults, so
 * only the ones worth deciding immediately are left live.
 *
 * @CLAWT_SCHEMA_FLAG_DANGEROUS drives the extra warning text in the
 * generated config and the docs.  Handing an agent an unconfined host
 * should read like a decision, not a setting.
 */
typedef enum {
    CLAWT_SCHEMA_FLAG_NONE      = 0,
    CLAWT_SCHEMA_FLAG_REQUIRED  = 1 << 0,
    CLAWT_SCHEMA_FLAG_COMMENTED = 1 << 1,
    CLAWT_SCHEMA_FLAG_DANGEROUS = 1 << 2,
    CLAWT_SCHEMA_FLAG_PER_AGENT = 1 << 3
} ClawtSchemaFlags;

/**
 * ClawtSchemaEntry:
 * @key: dotted path, e.g. "daemon.socket"; the last component is the YAML key
 * @type: what kind of value it holds
 * @flags: see #ClawtSchemaFlags
 * @default_value: (nullable): the default, spelled exactly as it appears in YAML
 * @enum_type: (nullable): for %CLAWT_SCHEMA_ENUM, a getter for the #GType
 * @doc: the documentation comment, as one string with embedded newlines
 * @since: (nullable): version the option appeared in
 *
 * One configuration option.
 */
typedef struct {
    const gchar      *key;
    ClawtSchemaType   type;
    ClawtSchemaFlags  flags;
    const gchar      *default_value;
    GType           (*enum_type)(void);
    const gchar      *doc;
    const gchar      *since;
} ClawtSchemaEntry;

/**
 * ClawtSchemaAgentKey:
 * @agent_key: what the option is called inside an `agents:` block
 * @fleet_key: the fleet-level key it falls back to when the agent is silent
 *
 * One option that can be set in two places.
 *
 * Two shapes reach this, and they are the same relationship seen from
 * opposite ends. A fleet-policy key carrying %CLAWT_SCHEMA_FLAG_PER_AGENT
 * may also be written inside an agent, under a shorter name --
 * `orchestration.mailbox.max_depth` is `mailbox.max_depth` there. And an
 * `agents.*` key may take its default from the `defaults:` section --
 * `model.provider` from `defaults.provider`. Neither spelling is
 * derivable from the other: `computer.type` inherits `defaults.computer`
 * and `memories.enabled` keeps its whole name.
 *
 * So it is stated, once, here. It used to be stated twice and privately
 * -- in clawt-agent-manager.c for the mailbox keys and in clawt-config.c
 * for the rest -- which is how the daemon came to have no way to report
 * the nine PER_AGENT options to a client, and so no client could offer
 * them.
 */
typedef struct {
    const gchar *agent_key;
    const gchar *fleet_key;
} ClawtSchemaAgentKey;

/**
 * clawt_config_schema_agent_keys:
 * @n_entries: (out): number of entries
 *
 * Every option settable both on an agent and on the fleet.
 *
 * Returns: (transfer none) (array length=n_entries): the relation
 */
const ClawtSchemaAgentKey *
clawt_config_schema_agent_keys(gsize *n_entries);

/**
 * clawt_config_schema_agent_key_for:
 * @fleet_key: a fleet-level key
 *
 * What @fleet_key is called inside an `agents:` block.
 *
 * Returns: (transfer none) (nullable): the agent-relative name, or %NULL
 *   if @fleet_key cannot be set per agent
 */
const gchar *
clawt_config_schema_agent_key_for(const gchar *fleet_key);

/**
 * clawt_config_schema_fleet_key_for:
 * @agent_key: an agent-relative key
 *
 * The fleet-level key @agent_key falls back to.
 *
 * Returns: (transfer none) (nullable): the fleet key, or %NULL if this
 *   option has no fleet-wide setting
 */
const gchar *
clawt_config_schema_fleet_key_for(const gchar *agent_key);

/**
 * clawt_config_schema_agent_name:
 * @entry: a schema entry
 *
 * What @entry is called inside an `agents:` block, if anything.
 *
 * Two shapes are settable on an agent and they are spelled differently:
 * an `agents.*` row is itself the option, while a fleet key flagged
 * %CLAWT_SCHEMA_FLAG_PER_AGENT has a shorter name in the relation. Every
 * caller that builds an editor, or reports what an agent has, needs the
 * same answer -- so it is given once here rather than branched on in
 * each of them, which is how the daemon and the web client came to
 * disagree about whether nine options existed.
 *
 * Sections, mappings and lists-of return %NULL: they are structure
 * rather than settings.
 *
 * Returns: (transfer none) (nullable): the agent-relative name
 */
const gchar *
clawt_config_schema_agent_name(const ClawtSchemaEntry *entry);

/**
 * clawt_config_schema_get:
 * @n_entries: (out): number of entries
 *
 * Gets the configuration schema.
 *
 * Returns: (transfer none) (array length=n_entries): the schema table
 */
const ClawtSchemaEntry *
clawt_config_schema_get(gsize *n_entries);

/**
 * clawt_config_schema_lookup:
 * @key: a dotted key path
 *
 * Finds the schema entry for @key.
 *
 * Agent keys are looked up under their canonical "agents.*" path, so
 * "agents.model.provider" and a `provider:` inside any agent block resolve
 * to the same entry.
 *
 * Returns: (transfer none) (nullable): the entry, or %NULL if unknown
 */
const ClawtSchemaEntry *
clawt_config_schema_lookup(const gchar *key);

/**
 * clawt_config_schema_type_name:
 * @type: a #ClawtSchemaType
 *
 * Returns: (transfer none): the name used for @type in documentation
 */
const gchar *
clawt_config_schema_type_name(ClawtSchemaType type);

/**
 * clawt_config_schema_render_example:
 *
 * Renders data/example-config.yaml: every option, with its default and its
 * documentation.
 *
 * Returns: (transfer full): the YAML text
 */
gchar *
clawt_config_schema_render_example(void);

/**
 * clawt_config_schema_render_default:
 *
 * Renders data/default-config.yaml, the starter config that
 * `clawtilla --generate-config` prints.  Options flagged
 * %CLAWT_SCHEMA_FLAG_COMMENTED are written out commented.
 *
 * Returns: (transfer full): the YAML text
 */
gchar *
clawt_config_schema_render_default(void);

/**
 * clawt_config_schema_render_org:
 *
 * Renders the option-reference tables for docs/configuration-options.org.
 *
 * Returns: (transfer full): the org-mode text
 */
gchar *
clawt_config_schema_render_org(void);

/**
 * clawt_config_schema_comment_for:
 * @key: a dotted key path
 *
 * The canonical documentation comment for @key, split into lines, ready to
 * hand to yaml_node_set_leading_comments().  This is what puts the schema's
 * documentation back into a config file that clawtilla has rewritten.
 *
 * Returns: (transfer full) (nullable) (element-type utf8): comment lines,
 *   or %NULL if @key is unknown or undocumented
 */
GPtrArray *
clawt_config_schema_comment_for(const gchar *key);

G_END_DECLS
