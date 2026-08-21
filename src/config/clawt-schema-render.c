/*
 * clawt-schema-render.c - Generating config files and docs from the schema
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Turns the flat table in clawt-config-schema.c into the three artefacts
 * that would otherwise be maintained by hand, and drift:
 *
 *   data/example-config.yaml       every option, default and comment
 *   data/default-config.yaml       the starter config
 *   docs/configuration-options.org the reference table
 */

#include "clawtilla.h"
#include "config/clawt-config-schema.h"

#include <string.h>

/*
 * The table is flat and dotted; YAML is nested.  Rather than build a tree,
 * the renderers walk the table in order and track the current path, which
 * works because the table is written parent-before-child and is required to
 * stay that way.  A child whose parent is missing would indent under nothing,
 * so that ordering is checked by tests/test-config-schema.c rather than left
 * as folklore.
 */
static guint
key_depth(const gchar *key)
{
    guint depth = 0;
    const gchar *p;

    for (p = key; *p != '\0'; p++)
    {
        if (*p == '.')
            depth++;
    }

    return depth;
}

static const gchar *
key_leaf(const gchar *key)
{
    const gchar *dot = strrchr(key, '.');

    return (dot != NULL) ? dot + 1 : key;
}

static void
append_indent(GString *out, guint depth)
{
    guint i;

    for (i = 0; i < depth * 2; i++)
        g_string_append_c(out, ' ');
}

/*
 * Emits a documentation block as YAML comments at the right indent.
 *
 * @prefix is "" for a live option and "# " for one written out commented,
 * so a commented option's documentation still reads as documentation rather
 * than becoming part of the commented-out value.
 */
static void
append_doc(GString     *out,
           const gchar *doc,
           guint        depth,
           const gchar *prefix)
{
    g_auto(GStrv) lines = NULL;
    guint i;

    if (doc == NULL)
        return;

    lines = g_strsplit(doc, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
    {
        append_indent(out, depth);
        g_string_append(out, prefix);

        if (lines[i][0] == '\0')
            g_string_append(out, "#\n");
        else
            g_string_append_printf(out, "# %s\n", lines[i]);
    }
}

/*
 * Lists the nicknames of an enum, so the generated file says what the legal
 * values actually are instead of leaving the reader to find the header.
 */
static gchar *
enum_values_text(const ClawtSchemaEntry *entry)
{
    g_autoptr(GEnumClass) klass = NULL;
    GString *out;
    guint i;

    if (entry->enum_type == NULL)
        return NULL;

    klass = g_type_class_ref(entry->enum_type());
    out = g_string_new(NULL);

    for (i = 0; i < klass->n_values; i++)
    {
        if (i > 0)
            g_string_append(out, ", ");
        g_string_append(out, klass->values[i].value_nick);
    }

    return g_string_free(out, FALSE);
}

/*
 * Renders one option's value.  Defaults are stored spelled exactly as they
 * should appear in YAML, so this is mostly quoting decisions: a path or a
 * string that could be read as something else gets quotes, a number or a
 * boolean does not.
 */
static void
append_value(GString *out, const ClawtSchemaEntry *entry)
{
    const gchar *value = entry->default_value;

    switch (entry->type) {
    case CLAWT_SCHEMA_SECTION:
        g_string_append_c(out, '\n');
        return;

    case CLAWT_SCHEMA_STRING_LIST:
        g_string_append(out, value != NULL ? value : " []\n");
        if (value == NULL)
            return;
        g_string_append_c(out, '\n');
        return;

    case CLAWT_SCHEMA_MAPPING:
        g_string_append(out, " {}\n");
        return;

    case CLAWT_SCHEMA_LIST_OF:
        g_string_append(out, " []\n");
        return;

    case CLAWT_SCHEMA_BOOLEAN:
    case CLAWT_SCHEMA_INT:
    case CLAWT_SCHEMA_DOUBLE:
        g_string_append_printf(out, " %s\n", value != NULL ? value : "null");
        return;

    case CLAWT_SCHEMA_SECRET:
        g_string_append(out, " null\n");
        return;

    case CLAWT_SCHEMA_PATH:
    case CLAWT_SCHEMA_STRING:
    case CLAWT_SCHEMA_ENUM:
    default:
        if (value == NULL)
            g_string_append(out, " null\n");
        else
            g_string_append_printf(out, " \"%s\"\n", value);
        return;
    }
}

/*
 * The header every generated file carries.  Says where to make changes,
 * because the obvious thing to do with a config file is edit it, and
 * editing these two in particular is wasted work.
 */
static void
append_generated_header(GString *out, const gchar *what)
{
    g_string_append_printf(out,
        "# clawtilla %s\n"
        "#\n"
        "# GENERATED FILE -- do not edit.\n"
        "#\n"
        "# Every option clawtilla understands is declared once, in\n"
        "# src/config/clawt-config-schema.c.  This file, its sibling, and the\n"
        "# reference table in docs/ are all produced from that table by\n"
        "# `make config-files`.  Editing here is lost on the next run, and\n"
        "# tests/test-config-schema.c fails when these files have drifted.\n"
        "#\n", what);
}

/*
 * Indentation for the example file, which unlike the starter config shows
 * the inside of list-of sections.
 *
 * A list-of renders as one sample element, so everything below it sits a
 * level deeper than its dotted depth suggests and the first child carries
 * the "- ".  Without this the children indent as if they were mapping
 * members of a scalar, and the file is not valid YAML at all -- which is
 * exactly how this was found.
 */
typedef struct {
    gboolean is_list[16];   /* whether depth N opened a list-of */
    gboolean pending_dash;  /* next entry starts a sample element */
    guint    dash_depth;
} ListState;

static guint
extra_indent_for(const ListState *state, guint depth)
{
    guint extra = 0;
    guint d;

    for (d = 0; d < depth && d < G_N_ELEMENTS(state->is_list); d++)
    {
        if (state->is_list[d])
            extra++;
    }

    return extra;
}

gchar *
clawt_config_schema_render_example(void)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries;
    gsize i;
    GString *out = g_string_new(NULL);
    ListState state;

    memset(&state, 0, sizeof(state));

    entries = clawt_config_schema_get(&n_entries);

    append_generated_header(out, "configuration reference");
    g_string_append(out,
        "# This file lists EVERY option with its default value and what it is\n"
        "# for.  It is documentation you can copy from, not a config to run:\n"
        "# see data/default-config.yaml, or `clawtilla --generate-config`, for\n"
        "# a starter file.\n"
        "#\n"
        "# Default location: ~/.clawtilla/config.yaml\n"
        "\n");

    for (i = 0; i < n_entries; i++)
    {
        const ClawtSchemaEntry *entry = &entries[i];
        guint depth = key_depth(entry->key);
        guint indent = depth + extra_indent_for(&state, depth);
        g_autofree gchar *enum_values = enum_values_text(entry);
        gboolean dash_here;

        /* Leaving a list-of clears its marker and everything under it. */
        {
            guint d;

            for (d = depth; d < G_N_ELEMENTS(state.is_list); d++)
                state.is_list[d] = FALSE;
        }

        dash_here = state.pending_dash && depth == state.dash_depth;
        state.pending_dash = FALSE;

        g_string_append_c(out, '\n');
        append_doc(out, entry->doc, indent, "");

        if (enum_values != NULL)
        {
            append_indent(out, indent);
            g_string_append_printf(out, "# Values: %s\n", enum_values);
        }

        if ((entry->flags & CLAWT_SCHEMA_FLAG_REQUIRED) != 0)
        {
            append_indent(out, indent);
            g_string_append(out, "# Required.\n");
        }

        if ((entry->flags & CLAWT_SCHEMA_FLAG_DANGEROUS) != 0)
        {
            append_indent(out, indent);
            g_string_append(out,
                "# DANGEROUS: read docs/security.org before enabling this.\n");
        }

        if (dash_here)
        {
            append_indent(out, indent - 1);
            g_string_append(out, "- ");
        }
        else
        {
            append_indent(out, indent);
        }

        g_string_append(out, key_leaf(entry->key));
        g_string_append_c(out, ':');

        if (entry->type == CLAWT_SCHEMA_LIST_OF)
        {
            /*
             * Open it as a block rather than emitting "[]": the whole point
             * of the example file is to show what goes inside.
             */
            g_string_append_c(out, '\n');
            if (depth < G_N_ELEMENTS(state.is_list))
                state.is_list[depth] = TRUE;
            state.pending_dash = TRUE;
            state.dash_depth = depth + 1;
        }
        else
        {
            append_value(out, entry);
        }
    }

    return g_string_free(out, FALSE);
}

gchar *
clawt_config_schema_render_default(void)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries;
    gsize i;
    GString *out = g_string_new(NULL);

    entries = clawt_config_schema_get(&n_entries);

    append_generated_header(out, "default configuration");
    g_string_append(out,
        "# A starter config.  Copy to ~/.clawtilla/config.yaml and edit.\n"
        "#\n"
        "# Only the options worth deciding immediately are live here; the rest\n"
        "# are commented out at their defaults, so this reads as a set of\n"
        "# choices rather than a wall of settings.  data/example-config.yaml\n"
        "# documents all of them.\n"
        "\n");

    for (i = 0; i < n_entries; i++)
    {
        const ClawtSchemaEntry *entry = &entries[i];
        guint depth = key_depth(entry->key);
        gboolean commented;

        /*
         * Per-agent options are documented under agents.* but have no place
         * in a starter file with no agents in it: emitting them at top level
         * would produce a config that does not validate.
         */
        if (g_str_has_prefix(entry->key, "agents.") ||
            g_str_has_prefix(entry->key, "rooms."))
            continue;

        /*
         * `agents` itself is emitted once at the end, with the worked
         * example beside it.  Emitting it here too produced a starter
         * config with the key twice -- and YAML takes the last one, so
         * anything the user wrote in the first was silently discarded.
         */
        if (g_strcmp0(entry->key, "agents") == 0)
            continue;

        commented = ((entry->flags & CLAWT_SCHEMA_FLAG_COMMENTED) != 0) ||
                    ((entry->flags & CLAWT_SCHEMA_FLAG_DANGEROUS) != 0);

        g_string_append_c(out, '\n');
        append_doc(out, entry->doc, depth, commented ? "# " : "");

        if ((entry->flags & CLAWT_SCHEMA_FLAG_DANGEROUS) != 0)
        {
            append_indent(out, depth);
            g_string_append(out,
                "# # DANGEROUS: read docs/security.org before enabling this.\n");
        }

        append_indent(out, depth);
        if (commented)
            g_string_append(out, "# ");
        g_string_append(out, key_leaf(entry->key));
        g_string_append_c(out, ':');
        append_value(out, entry);
    }

    g_string_append(out,
        "\n"
        "# The fleet.  Empty to start with; `clawtilla agent create` or\n"
        "# `clawtilla agent new --ai` fills this in.\n"
        "#\n"
        "# A minimal agent looks like:\n"
        "#\n"
        "#   agents:\n"
        "#     - id: chief-of-staff\n"
        "#       name: \"Chief of Staff\"\n"
        "#       chief_of_staff: true\n"
        "#       model:\n"
        "#         model: opus\n"
        "#       computer:\n"
        "#         type: container\n"
        "#         mounts:\n"
        "#           - source: \"~/src\"\n"
        "#             target: \"/work/src\"\n"
        "#             mode: rw\n"
        "#             relabel: shared\n"
        "agents: []\n");

    return g_string_free(out, FALSE);
}

gchar *
clawt_config_schema_render_org(void)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries;
    gsize i;
    GString *out = g_string_new(NULL);
    g_autofree gchar *current_section = NULL;

    entries = clawt_config_schema_get(&n_entries);

    g_string_append(out,
        "#+title: Configuration Options\n"
        "#+description: Every clawtilla configuration option\n"
        "\n"
        "* Configuration Options\n"
        "\n"
        "GENERATED FILE -- do not edit.  Produced from\n"
        "=src/config/clawt-config-schema.c= by =make config-files=.\n"
        "\n"
        "See [[file:configuration.org][configuration.org]] for worked examples and the reasoning\n"
        "behind these; this file is the exhaustive list.\n");

    for (i = 0; i < n_entries; i++)
    {
        const ClawtSchemaEntry *entry = &entries[i];
        g_autofree gchar *enum_values = NULL;
        g_auto(GStrv) doc_lines = NULL;
        guint line;

        if (entry->type == CLAWT_SCHEMA_SECTION ||
            entry->type == CLAWT_SCHEMA_LIST_OF)
        {
            g_string_append_printf(out, "\n** =%s=\n\n", entry->key);

            if (entry->doc != NULL)
                g_string_append_printf(out, "%s\n", entry->doc);

            g_free(current_section);
            current_section = g_strdup(entry->key);
            continue;
        }

        g_string_append_printf(out, "\n*** =%s=\n\n", entry->key);

        g_string_append_printf(out, "- Type :: %s\n",
                               clawt_config_schema_type_name(entry->type));

        enum_values = enum_values_text(entry);
        if (enum_values != NULL)
            g_string_append_printf(out, "- Values :: %s\n", enum_values);

        if (entry->default_value != NULL)
            g_string_append_printf(out, "- Default :: =%s=\n",
                                   entry->default_value);
        else
            g_string_append(out, "- Default :: unset\n");

        if ((entry->flags & CLAWT_SCHEMA_FLAG_REQUIRED) != 0)
            g_string_append(out, "- Required :: yes\n");

        if ((entry->flags & CLAWT_SCHEMA_FLAG_DANGEROUS) != 0)
            g_string_append(out,
                "- Warning :: hands over real authority; see [[file:security.org][security.org]]\n");

        if (entry->since != NULL)
            g_string_append_printf(out, "- Since :: %s\n", entry->since);

        if (entry->doc != NULL)
        {
            g_string_append_c(out, '\n');
            doc_lines = g_strsplit(entry->doc, "\n", -1);
            for (line = 0; doc_lines[line] != NULL; line++)
                g_string_append_printf(out, "%s\n", doc_lines[line]);
        }
    }

    return g_string_free(out, FALSE);
}
