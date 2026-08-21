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
