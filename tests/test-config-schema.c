/*
 * test-config-schema.c - The schema, and the files generated from it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The schema is the single source of truth for configuration.  These tests
 * are what make that claim enforceable rather than aspirational: if the
 * checked-in YAML or the docs table drifts from the table, this fails.
 */

#include <clawtilla.h>

#include <string.h>

static gchar *
read_repo_file(const gchar *relative)
{
    g_autofree gchar *path = g_build_filename(CLAWT_TEST_SRCDIR, relative, NULL);
    gchar *contents = NULL;

    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return NULL;

    return contents;
}

/*
 * The generated files must match what the schema renders right now.
 *
 * This is the whole point of generating them.  A CLAUDE.md rule asking
 * somebody to remember `make config-files` is a rule that gets forgotten on
 * a busy afternoon; this fails the build instead.
 */
static void
test_generated_files_match_schema(void)
{
    g_autofree gchar *on_disk = NULL;
    g_autofree gchar *rendered = NULL;

    on_disk = read_repo_file("data/example-config.yaml");
    g_assert_nonnull(on_disk);
    rendered = clawt_config_schema_render_example();
    g_assert_cmpstr(on_disk, ==, rendered);

    g_free(on_disk);
    g_free(rendered);

    on_disk = read_repo_file("data/default-config.yaml");
    g_assert_nonnull(on_disk);
    rendered = clawt_config_schema_render_default();
    g_assert_cmpstr(on_disk, ==, rendered);
}

static void
test_generated_docs_match_schema(void)
{
    g_autofree gchar *on_disk = read_repo_file("docs/configuration-options.org");
    g_autofree gchar *rendered = clawt_config_schema_render_org();

    g_assert_nonnull(on_disk);
    g_assert_cmpstr(on_disk, ==, rendered);
}

/*
 * The renderers walk the flat table in order and track depth, which only
 * works if a key's parent appears before it.  A child whose parent is
 * missing would indent under nothing and produce a file that is not YAML.
 */
static void
test_every_key_has_a_parent(void)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries;
    gsize i;
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const gchar *key = entries[i].key;
        g_autofree gchar *parent = g_strdup(key);
        gchar *last_dot = strrchr(parent, '.');

        if (last_dot != NULL) {
            *last_dot = '\0';
            if (!g_hash_table_contains(seen, parent))
                g_error("schema key '%s' appears before its parent '%s'",
                        key, parent);
        }

        g_hash_table_add(seen, (gpointer)key);
    }
}

/* Duplicate keys would make lookup order decide which default wins. */
static void
test_no_duplicate_keys(void)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries;
    gsize i;
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        if (g_hash_table_contains(seen, entries[i].key))
            g_error("schema key '%s' is declared twice", entries[i].key);

        g_hash_table_add(seen, (gpointer)entries[i].key);
    }
}

/*
 * Undocumented options are options nobody can decide about, and the
 * generated files would have a bare key with no explanation above it.
 */
static void
test_every_option_is_documented(void)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries;
    gsize i;

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        if (entries[i].doc == NULL || entries[i].doc[0] == '\0')
            g_error("schema key '%s' has no documentation", entries[i].key);

        if (entries[i].since == NULL)
            g_error("schema key '%s' has no Since:", entries[i].key);
    }
}

/*
 * An enum default that is not one of its own values would make the
 * documented default unreachable.
 */
static void
test_enum_defaults_are_valid(void)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries;
    gsize i;

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        gint value = 0;

        if (entries[i].type != CLAWT_SCHEMA_ENUM)
            continue;

        if (entries[i].enum_type == NULL)
            g_error("enum option '%s' declares no enum type", entries[i].key);

        if (entries[i].default_value == NULL)
            continue;

        if (!clawt_enum_from_nick(entries[i].enum_type(),
                                  entries[i].default_value, &value))
            g_error("enum option '%s' defaults to '%s', which is not one of "
                    "its values", entries[i].key, entries[i].default_value);
    }
}

/* The generated starter config has to be loadable, or the first run fails. */
static void
test_default_config_loads(void)
{
    g_autofree gchar *text = clawt_config_schema_render_default();
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;

    config = clawt_config_load_from_string(text, &error);
    g_assert_no_error(error);
    g_assert_nonnull(config);

    g_assert_true(clawt_config_validate(config, &error));
    g_assert_no_error(error);

    /* And it must not be full of "unknown key" warnings about itself. */
    g_assert_cmpuint(clawt_config_get_warnings(config)->len, ==, 0);
}

static void
test_example_config_loads(void)
{
    g_autofree gchar *text = clawt_config_schema_render_example();
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;

    config = clawt_config_load_from_string(text, &error);
    g_assert_no_error(error);
    g_assert_nonnull(config);
}

/* A dangerous option must say so where somebody will actually read it. */
static void
test_dangerous_options_are_flagged_in_output(void)
{
    g_autofree gchar *example = clawt_config_schema_render_example();

    g_assert_nonnull(strstr(example, "confirm_host_control"));
    g_assert_nonnull(strstr(example, "DANGEROUS"));
    g_assert_nonnull(strstr(example, "docs/security.org"));
}

/*
 * A generated file must not name the same top-level key twice.
 *
 * YAML silently takes the last one, so a duplicate does not fail -- it
 * discards whatever the user wrote under the first.  The generator did
 * exactly this with `agents:` once, and nothing noticed until a config
 * that looked right behaved as though it were empty.
 */
static void
assert_no_duplicate_top_level_keys(const gchar *what, const gchar *contents)
{
    g_auto(GStrv) lines = NULL;
    g_autoptr(GHashTable) seen = NULL;
    gsize i;

    g_assert_nonnull(contents);

    seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    lines = g_strsplit(contents, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        const gchar *colon;
        g_autofree gchar *key = NULL;

        /* Only unindented, uncommented keys are top level. */
        if (lines[i][0] == '\0' || g_ascii_isspace(lines[i][0]) ||
            lines[i][0] == '#')
            continue;

        colon = strchr(lines[i], ':');

        if (colon == NULL)
            continue;

        key = g_strndup(lines[i], (gsize)(colon - lines[i]));

        if (g_hash_table_contains(seen, key))
            g_error("%s names '%s' twice; YAML keeps only the last, so "
                    "everything under the first is silently lost",
                    what, key);

        g_hash_table_add(seen, g_steal_pointer(&key));
    }
}

static void
test_default_config_has_no_duplicate_keys(void)
{
    g_autofree gchar *contents = read_repo_file("data/default-config.yaml");

    assert_no_duplicate_top_level_keys("data/default-config.yaml", contents);
}

static void
test_example_config_has_no_duplicate_keys(void)
{
    g_autofree gchar *contents = read_repo_file("data/example-config.yaml");

    assert_no_duplicate_top_level_keys("data/example-config.yaml", contents);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/schema/generated-files-match",
                    test_generated_files_match_schema);
    g_test_add_func("/schema/generated-docs-match",
                    test_generated_docs_match_schema);
    g_test_add_func("/schema/every-key-has-a-parent",
                    test_every_key_has_a_parent);
    g_test_add_func("/schema/no-duplicate-keys", test_no_duplicate_keys);
    g_test_add_func("/schema/every-option-documented",
                    test_every_option_is_documented);
    g_test_add_func("/schema/enum-defaults-valid",
                    test_enum_defaults_are_valid);
    g_test_add_func("/schema/default-config-no-duplicates",
                    test_default_config_has_no_duplicate_keys);
    g_test_add_func("/schema/example-config-no-duplicates",
                    test_example_config_has_no_duplicate_keys);
    g_test_add_func("/schema/default-config-loads", test_default_config_loads);
    g_test_add_func("/schema/example-config-loads", test_example_config_loads);
    g_test_add_func("/schema/dangerous-flagged",
                    test_dangerous_options_are_flagged_in_output);

    return g_test_run();
}
