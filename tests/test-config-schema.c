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
#include <glib/gstdio.h>

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


/* ── Options settable in two places ──────────────────────────────── */

/*
 * Every fleet key flagged PER_AGENT has an agent-relative name.
 *
 * The flag says an agent may override the option; without a name to
 * write it under, the flag is a promise nothing keeps. It was exactly
 * that for `orchestration.mailbox.overflow`, which was flagged for the
 * whole life of the file while the code read the fleet value and ignored
 * the agent's.
 */
static void
test_per_agent_keys_have_an_agent_name(void)
{
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        if (!(schema[i].flags & CLAWT_SCHEMA_FLAG_PER_AGENT))
            continue;

        if (clawt_config_schema_agent_key_for(schema[i].key) == NULL)
            g_test_fail_printf(
                "%s is PER_AGENT but has no agent-relative name; add it to "
                "agent_keys[] in clawt-config-schema.c", schema[i].key);
    }
}

/*
 * Every fleet key named in the relation exists in the schema.
 *
 * A relation entry pointing at a key nobody declares is an inheritance
 * that silently resolves to nothing.
 */
static void
test_relation_names_real_fleet_keys(void)
{
    const ClawtSchemaAgentKey *keys;
    gsize n_entries = 0;
    gsize i;

    keys = clawt_config_schema_agent_keys(&n_entries);
    g_assert_cmpuint(n_entries, >, 0);

    for (i = 0; i < n_entries; i++) {
        g_assert_nonnull(keys[i].agent_key);
        g_assert_nonnull(keys[i].fleet_key);

        if (clawt_config_schema_lookup(keys[i].fleet_key) == NULL)
            g_test_fail_printf("%s -> %s: no such fleet key",
                               keys[i].agent_key, keys[i].fleet_key);
    }
}

/*
 * Every agent-relative name in the relation describes a real option.
 *
 * Either the schema has an `agents.<name>` row of its own, or the fleet
 * key does -- one of the two has to, or nothing knows the option's type.
 */
static void
test_relation_names_describable_options(void)
{
    const ClawtSchemaAgentKey *keys;
    gsize n_entries = 0;
    gsize i;

    keys = clawt_config_schema_agent_keys(&n_entries);

    for (i = 0; i < n_entries; i++) {
        g_autofree gchar *agent_path =
            g_strdup_printf("agents.%s", keys[i].agent_key);

        if (clawt_config_schema_lookup(agent_path) == NULL &&
            clawt_config_schema_lookup(keys[i].fleet_key) == NULL)
            g_test_fail_printf("%s describes nothing", keys[i].agent_key);
    }
}

/*
 * No two options claim the same agent-relative name.
 *
 * A collision does not fail anywhere: whichever is looked up second
 * silently answers for the first. It nearly happened when the agent
 * spelling was being *derived* rather than stated, because dropping the
 * leading section turned both `memories.enabled` and `agents.enabled`
 * into `enabled`.
 */
static void
test_agent_names_are_unique(void)
{
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    const ClawtSchemaAgentKey *keys;
    gsize n_entries = 0;
    gsize i;

    keys = clawt_config_schema_agent_keys(&n_entries);

    for (i = 0; i < n_entries; i++) {
        g_autofree gchar *agent_path =
            g_strdup_printf("agents.%s", keys[i].agent_key);

        if (g_hash_table_contains(seen, keys[i].agent_key))
            g_test_fail_printf("two options are called %s inside an agent",
                               keys[i].agent_key);

        /*
         * An override must not land on the name of a different option.
         *
         * The two shapes look alike and are not: `computer.container.image`
         * is an `agents.*` option that *inherits* `defaults.container_image`,
         * which is fine and expected. A PER_AGENT fleet key whose agent
         * name is already an `agents.*` row is the other thing -- one
         * name meaning two options, where whichever resolves second
         * silently answers for the first.
         */
        {
            const ClawtSchemaEntry *fleet =
                clawt_config_schema_lookup(keys[i].fleet_key);

            if (fleet != NULL &&
                (fleet->flags & CLAWT_SCHEMA_FLAG_PER_AGENT) &&
                clawt_config_schema_lookup(agent_path) != NULL)
                g_test_fail_printf(
                    "%s names both an agents.* option and an override of %s",
                    keys[i].agent_key, keys[i].fleet_key);
        }

        g_hash_table_add(seen, (gpointer)keys[i].agent_key);
    }
}

/*
 * The relation is symmetric.
 *
 * Two accessors read one table, and a caller that used the wrong
 * direction would get NULL and quietly fall through to a default.
 */
static void
test_the_relation_reads_both_ways(void)
{
    const ClawtSchemaAgentKey *keys;
    gsize n_entries = 0;
    gsize i;

    keys = clawt_config_schema_agent_keys(&n_entries);

    for (i = 0; i < n_entries; i++) {
        g_assert_cmpstr(clawt_config_schema_agent_key_for(keys[i].fleet_key),
                        ==, keys[i].agent_key);
        g_assert_cmpstr(clawt_config_schema_fleet_key_for(keys[i].agent_key),
                        ==, keys[i].fleet_key);
    }

    g_assert_null(clawt_config_schema_agent_key_for("nothing.like.this"));
    g_assert_null(clawt_config_schema_fleet_key_for("nothing.like.this"));
}

/*
 * Writes a config naming every relation option twice -- once fleet-wide
 * and once inside an agent -- so the resolution order can be asserted on
 * a real ClawtConfig rather than reasoned about.
 */
static gchar *
write_relation_config(const gchar *dir, gboolean with_agent_values)
{
    g_autoptr(GString) yaml = g_string_new(NULL);
    g_autofree gchar *path = g_build_filename(dir, "clawtilla.yaml", NULL);

    g_string_append_printf(yaml,
        "daemon:\n"
        "  socket: '%s/d.sock'\n"
        "  state_dir: '%s/state'\n"
        "  tailscale: false\n"
        "defaults:\n"
        "  workspace_root: '%s/agents'\n"
        "  provider: fleet-provider\n"
        "  model: fleet-model\n"
        "  computer: host\n"
        "  restart: always\n"
        "  container_image: fleet-image\n"
        "memories:\n"
        "  enabled: true\n"
        "  max_results: 77\n"
        "  readers: [fleet-reader]\n"
        "orchestration:\n"
        "  mailbox:\n"
        "    max_depth: 111\n"
        "    overflow: drop-oldest\n"
        "    max_attempts: 112\n"
        "    lease_seconds: 113\n"
        "    default_ttl_seconds: 114\n"
        "    backoff_seconds: 115\n"
        "agents:\n"
        "  - id: solo\n"
        "    name: 'Solo'\n",
        dir, dir, dir);

    if (with_agent_values) {
        g_string_append(yaml,
            "    mailbox:\n"
            "      max_depth: 221\n"
            "      overflow: block-sender\n"
            "      max_attempts: 222\n"
            "      lease_seconds: 223\n"
            "      default_ttl_seconds: 224\n"
            "      backoff_seconds: 225\n"
            "    memories:\n"
            "      enabled: false\n"
            "      max_results: 88\n"
            "      readers: [agent-reader]\n"
            "    model:\n"
            "      provider: agent-provider\n"
            "      model: agent-model\n"
            "    computer:\n"
            "      type: container\n"
            "      container:\n"
            "        image: agent-image\n"
            "    runtime:\n"
            "      restart: never\n");
    }

    if (!g_file_set_contents(path, yaml->str, -1, NULL))
        return NULL;

    return g_steal_pointer(&path);
}

/*
 * An agent's own value wins.
 *
 * This is the round trip the relation exists for: every name in it is
 * written into an agent block and read back through the ordinary getter.
 * A name that is wrong -- or a getter that resolves it under the wrong
 * schema path -- comes back as the fleet value or the schema default
 * instead, which is exactly how nine options came to be settable and
 * unread.
 */
static void
test_an_agents_own_value_wins(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-relation-XXXXXX", NULL);
    g_autofree gchar *path = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;

    g_assert_nonnull(dir);
    path = write_relation_config(dir, TRUE);
    g_assert_nonnull(path);

    config = clawt_config_load(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(config);

    agent = clawt_config_get_agent(config, "solo");
    g_assert_nonnull(agent);

    g_assert_cmpint(clawt_agent_config_get_int(agent, "mailbox.max_depth"),
                    ==, 221);
    g_assert_cmpint(clawt_agent_config_get_int(agent, "mailbox.max_attempts"),
                    ==, 222);
    g_assert_cmpint(clawt_agent_config_get_int(agent, "mailbox.lease_seconds"),
                    ==, 223);
    g_assert_cmpint(
        clawt_agent_config_get_int(agent, "mailbox.default_ttl_seconds"),
        ==, 224);
    g_assert_cmpint(
        clawt_agent_config_get_int(agent, "mailbox.backoff_seconds"), ==, 225);
    g_assert_cmpint(clawt_agent_config_get_int(agent, "memories.max_results"),
                    ==, 88);
    g_assert_false(clawt_agent_config_get_boolean(agent, "memories.enabled"));

    g_assert_cmpstr(clawt_agent_config_get_string(agent, "model.provider"),
                    ==, "agent-provider");
    g_assert_cmpstr(clawt_agent_config_get_string(agent, "model.model"),
                    ==, "agent-model");
    g_assert_cmpstr(clawt_agent_config_get_string(agent, "computer.type"),
                    ==, "container");
    g_assert_cmpstr(clawt_agent_config_get_string(agent, "runtime.restart"),
                    ==, "never");
    g_assert_cmpstr(
        clawt_agent_config_get_string(agent, "computer.container.image"),
        ==, "agent-image");

    /*
     * The enum goes through a different resolver -- it needs the schema
     * entry for its GType, and there is no `agents.mailbox.overflow` row
     * to find it under. Looking only there returned 0, which for this
     * enum is `reject`: an agent asking for block-sender got the
     * opposite of what it asked for, silently.
     */
    g_assert_cmpint(clawt_agent_config_get_enum(agent, "mailbox.overflow"),
                    ==, CLAWT_OVERFLOW_BLOCK_SENDER);

    {
        g_auto(GStrv) readers =
            clawt_agent_config_get_string_list(agent, "memories.readers");

        g_assert_nonnull(readers);
        g_assert_cmpstr(readers[0], ==, "agent-reader");
    }

    g_unlink(path);
    g_rmdir(dir);
}

/*
 * With the agent silent, the fleet's value is what it gets.
 *
 * Not the schema default -- that is the step after. `mailbox.max_depth`
 * had no fleet fallback at all before this, because the only code that
 * knew it existed was reading it a different way.
 */
static void
test_the_fleet_value_comes_next(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-relation-XXXXXX", NULL);
    g_autofree gchar *path = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;

    g_assert_nonnull(dir);
    path = write_relation_config(dir, FALSE);
    g_assert_nonnull(path);

    config = clawt_config_load(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(config);

    agent = clawt_config_get_agent(config, "solo");
    g_assert_nonnull(agent);

    g_assert_cmpint(clawt_agent_config_get_int(agent, "mailbox.max_depth"),
                    ==, 111);
    g_assert_cmpint(clawt_agent_config_get_int(agent, "mailbox.max_attempts"),
                    ==, 112);
    g_assert_cmpint(clawt_agent_config_get_int(agent, "mailbox.lease_seconds"),
                    ==, 113);
    g_assert_cmpint(
        clawt_agent_config_get_int(agent, "mailbox.default_ttl_seconds"),
        ==, 114);
    g_assert_cmpint(
        clawt_agent_config_get_int(agent, "mailbox.backoff_seconds"), ==, 115);
    g_assert_cmpint(clawt_agent_config_get_int(agent, "memories.max_results"),
                    ==, 77);
    g_assert_cmpint(clawt_agent_config_get_enum(agent, "mailbox.overflow"),
                    ==, CLAWT_OVERFLOW_DROP_OLDEST);

    g_assert_cmpstr(clawt_agent_config_get_string(agent, "model.provider"),
                    ==, "fleet-provider");
    g_assert_cmpstr(clawt_agent_config_get_string(agent, "computer.type"),
                    ==, "host");
    g_assert_cmpstr(clawt_agent_config_get_string(agent, "runtime.restart"),
                    ==, "always");

    {
        g_auto(GStrv) readers =
            clawt_agent_config_get_string_list(agent, "memories.readers");

        g_assert_nonnull(readers);
        g_assert_cmpstr(readers[0], ==, "fleet-reader");
    }

    g_unlink(path);
    g_rmdir(dir);
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
    g_test_add_func("/schema/per-agent-keys-have-an-agent-name",
                    test_per_agent_keys_have_an_agent_name);
    g_test_add_func("/schema/relation-names-real-fleet-keys",
                    test_relation_names_real_fleet_keys);
    g_test_add_func("/schema/relation-names-describable-options",
                    test_relation_names_describable_options);
    g_test_add_func("/schema/agent-names-are-unique",
                    test_agent_names_are_unique);
    g_test_add_func("/schema/the-relation-reads-both-ways",
                    test_the_relation_reads_both_ways);
    g_test_add_func("/schema/an-agents-own-value-wins",
                    test_an_agents_own_value_wins);
    g_test_add_func("/schema/the-fleet-value-comes-next",
                    test_the_fleet_value_comes_next);

    g_test_add_func("/schema/dangerous-flagged",
                    test_dangerous_options_are_flagged_in_output);

    return g_test_run();
}
