/*
 * test-config.c - Loading, reading, writing and validating configuration
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The cases here are the ones that go wrong quietly: a save that eats the
 * user's comments, a typo that falls through to a default, an agent block
 * from a newer version that stops the whole daemon.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

static const gchar *sample_config =
    "# clawtilla configuration for the test fleet\n"
    "# second header line\n"
    "\n"
    "daemon:\n"
    "  socket: \"/run/test/clawtilla.sock\"   # where clients dial in\n"
    "  # I turned this up while chasing a link problem.\n"
    "  log_level: debug\n"
    "\n"
    "defaults:\n"
    "  model: opus\n"
    "  computer: container\n"
    "\n"
    "agents:\n"
    "  # the one that hands out work\n"
    "  - id: chief\n"
    "    name: \"Chief of Staff\"\n"
    "    chief_of_staff: true\n"
    "  - id: researcher\n"
    "    model:\n"
    "      model: sonnet\n"
    "    computer:\n"
    "      type: container\n"
    "      mounts:\n"
    "        - source: \"/tmp\"\n"
    "          target: \"/work/tmp\"\n"
    "          mode: rw\n"
    "          relabel: shared\n";

static ClawtConfig *
load_sample(void)
{
    g_autoptr(GError) error = NULL;
    ClawtConfig *config = clawt_config_load_from_string(sample_config, &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    return config;
}

/* Values come from the file when it sets them. */
static void
test_reads_configured_values(void)
{
    g_autoptr(ClawtConfig) config = load_sample();

    g_assert_cmpstr(clawt_config_get_string(config, "daemon.socket"),
                    ==, "/run/test/clawtilla.sock");
    g_assert_cmpint(clawt_config_get_enum(config, "daemon.log_level"),
                    ==, CLAWT_LOG_DEBUG);
}

/* And from the schema when it does not. */
static void
test_falls_back_to_schema_defaults(void)
{
    g_autoptr(ClawtConfig) config = load_sample();

    g_assert_false(clawt_config_has_key(config, "orchestration.max_hops"));
    g_assert_cmpint(clawt_config_get_int(config, "orchestration.max_hops"),
                    ==, 8);
    g_assert_cmpint(clawt_config_get_enum(config, "orchestration.mailbox.overflow"),
                    ==, CLAWT_OVERFLOW_REJECT);
}

/*
 * An agent inherits from defaults.* before the schema.  Without that step
 * the `defaults:` section would be decoration.
 */
static void
test_agent_inherits_from_defaults(void)
{
    g_autoptr(ClawtConfig) config = load_sample();
    ClawtAgentConfig *chief = clawt_config_get_agent(config, "chief");
    ClawtAgentConfig *researcher = clawt_config_get_agent(config, "researcher");

    g_assert_nonnull(chief);
    g_assert_nonnull(researcher);

    /* chief says nothing about its model, so it takes the fleet default. */
    g_assert_cmpstr(clawt_agent_config_get_string(chief, "model.model"),
                    ==, "opus");

    /* researcher overrides it. */
    g_assert_cmpstr(clawt_agent_config_get_string(researcher, "model.model"),
                    ==, "sonnet");

    /* And the computer type comes from defaults for both. */
    g_assert_cmpstr(clawt_agent_config_get_string(chief, "computer.type"),
                    ==, "container");
}

/*
 * The case this whole subsystem exists for: saving must not eat the
 * comments.  Both the ones the user wrote and the schema's own.
 */
static void
test_save_preserves_comments(void)
{
    g_autoptr(ClawtConfig) config = load_sample();
    g_autofree gchar *rendered = NULL;

    rendered = clawt_config_to_string(config);
    g_assert_nonnull(rendered);

    g_assert_nonnull(strstr(rendered,
        "# clawtilla configuration for the test fleet"));
    g_assert_nonnull(strstr(rendered, "# second header line"));
    g_assert_nonnull(strstr(rendered, "# where clients dial in"));
    g_assert_nonnull(strstr(rendered,
        "# I turned this up while chasing a link problem."));
    g_assert_nonnull(strstr(rendered, "# the one that hands out work"));
}

/* Writing the file twice must produce the same bytes. */
static void
test_save_is_idempotent(void)
{
    g_autoptr(ClawtConfig) first = load_sample();
    g_autofree gchar *once = clawt_config_to_string(first);
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) second = NULL;
    g_autofree gchar *twice = NULL;

    second = clawt_config_load_from_string(once, &error);
    g_assert_no_error(error);
    twice = clawt_config_to_string(second);

    g_assert_cmpstr(once, ==, twice);
}

/* Values written by the daemon must come back as what they were. */
static void
test_set_and_reload_round_trip(void)
{
    g_autoptr(ClawtConfig) config = load_sample();
    g_autofree gchar *rendered = NULL;
    g_autoptr(ClawtConfig) reloaded = NULL;
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_config_set_boolean(config, "daemon.tcp_enabled", TRUE));
    g_assert_true(clawt_config_set_int(config, "orchestration.max_hops", 3));
    g_assert_true(clawt_config_set_string(config, "defaults.model", "fable"));

    rendered = clawt_config_to_string(config);
    reloaded = clawt_config_load_from_string(rendered, &error);
    g_assert_no_error(error);

    g_assert_true(clawt_config_get_boolean(reloaded, "daemon.tcp_enabled"));
    g_assert_cmpint(clawt_config_get_int(reloaded, "orchestration.max_hops"),
                    ==, 3);
    g_assert_cmpstr(clawt_config_get_string(reloaded, "defaults.model"),
                    ==, "fable");
}

/* A written value carries the schema's documentation with it. */
static void
test_written_value_gets_schema_comment(void)
{
    g_autoptr(ClawtConfig) config = clawt_config_new();
    g_autofree gchar *rendered = NULL;

    clawt_config_set_boolean(config, "daemon.allow_unconfined_host", FALSE);
    rendered = clawt_config_to_string(config);

    g_assert_nonnull(strstr(rendered, "allow_unconfined_host"));
    g_assert_nonnull(strstr(rendered, "confirm_host_control"));
}

static void
test_unset_returns_to_default(void)
{
    g_autoptr(ClawtConfig) config = load_sample();

    g_assert_cmpint(clawt_config_get_enum(config, "daemon.log_level"),
                    ==, CLAWT_LOG_DEBUG);
    g_assert_true(clawt_config_unset(config, "daemon.log_level"));
    g_assert_cmpint(clawt_config_get_enum(config, "daemon.log_level"),
                    ==, CLAWT_LOG_INFO);

    g_assert_false(clawt_config_unset(config, "daemon.log_level"));
}

/* An unknown key is a warning, not a failure: a typo should be visible
 * without stopping the daemon, and a key from a newer version should load. */
static void
test_unknown_key_warns_but_loads(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    GPtrArray *warnings;
    guint i;
    gboolean found = FALSE;

    config = clawt_config_load_from_string(
        "daemon:\n  socket: \"/tmp/x\"\n  invented_option: 1\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    warnings = clawt_config_get_warnings(config);
    for (i = 0; i < warnings->len; i++) {
        if (strstr(g_ptr_array_index(warnings, i), "invented_option") != NULL)
            found = TRUE;
    }

    g_assert_true(found);
}

static void
test_malformed_yaml_is_an_error(void)
{
    static const gchar *bad[] = {
        "daemon:\n\tsocket: x\n",              /* tab indentation */
        "daemon: [unclosed\n",
        "just a scalar\n",                      /* not a mapping */
        NULL
    };
    gsize i;

    for (i = 0; bad[i] != NULL; i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(ClawtConfig) config = NULL;

        config = clawt_config_load_from_string(bad[i], &error);

        g_assert_null(config);
        g_assert_nonnull(error);
    }
}

/* An empty file is a working daemon on defaults, not a failure. */
static void
test_empty_config_is_valid(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string("", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);
    g_assert_true(clawt_config_validate(config, &error));
    g_assert_cmpstr(clawt_config_get_string(config, "defaults.model"),
                    ==, "sonnet");
}

static void
test_comment_only_config_is_valid(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config =
        clawt_config_load_from_string("# nothing but a comment\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);
}

/* ── Agents ──────────────────────────────────────────────────────── */

static void
test_agent_list_and_lookup(void)
{
    g_autoptr(ClawtConfig) config = load_sample();

    g_assert_cmpuint(clawt_config_get_agents(config)->len, ==, 2);
    g_assert_nonnull(clawt_config_get_agent(config, "chief"));
    g_assert_null(clawt_config_get_agent(config, "nobody"));
}

static void
test_add_and_remove_agent(void)
{
    g_autoptr(ClawtConfig) config = load_sample();
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *added;

    added = clawt_config_add_agent(config, "writer", &error);
    g_assert_no_error(error);
    g_assert_nonnull(added);
    g_assert_cmpstr(clawt_agent_config_get_id(added), ==, "writer");
    g_assert_cmpuint(clawt_config_get_agents(config)->len, ==, 3);

    g_assert_true(clawt_config_remove_agent(config, "writer"));
    g_assert_cmpuint(clawt_config_get_agents(config)->len, ==, 2);
    g_assert_false(clawt_config_remove_agent(config, "writer"));
}

static void
test_duplicate_agent_id_is_refused(void)
{
    g_autoptr(ClawtConfig) config = load_sample();
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_config_add_agent(config, "chief", &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);
}

/*
 * Ids become directory names, socket names and SQLite filenames, so the
 * character set is restricted at the door rather than escaped at each use.
 */
static void
test_invalid_agent_ids_are_refused(void)
{
    static const gchar *bad[] = {
        "", "../escape", "has space", "UPPER", "has/slash",
        "-leading-dash", "_leading_underscore", "has.dot", NULL
    };
    g_autoptr(ClawtConfig) config = clawt_config_new();
    gsize i;

    for (i = 0; bad[i] != NULL; i++) {
        g_autoptr(GError) error = NULL;

        g_assert_null(clawt_config_add_agent(config, bad[i], &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    }

    g_assert_false(clawt_is_valid_id(NULL));
    g_assert_true(clawt_is_valid_id("chief-of-staff_2"));
}

/*
 * Forward compatibility: a config naming something this build has never
 * heard of disables that agent, not the daemon.
 */
static void
test_unknown_computer_type_becomes_shadow(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    ClawtAgentConfig *agent;

    config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: fine\n"
        "  - id: future\n"
        "    computer:\n"
        "      type: quantum-mainframe\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    agent = clawt_config_get_agent(config, "future");
    g_assert_nonnull(agent);
    g_assert_true(clawt_agent_config_is_shadow(agent));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
                            "quantum-mainframe"));

    /* The other agent is untouched. */
    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(config, "fine")));
}

/* Two chiefs-of-staff would make fleet routing a coin toss. */
static void
test_two_chiefs_of_staff_is_a_shadow(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;

    config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: first\n"
        "    chief_of_staff: true\n"
        "  - id: second\n"
        "    chief_of_staff: true\n", &error);

    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(config, "first")));
    g_assert_true(clawt_agent_config_is_shadow(
        clawt_config_get_agent(config, "second")));
}

/*
 * A host computer without the confirmation is a shadow, not a quietly
 * downgraded agent.  Silently confining it would be the wrong kind of
 * helpful: the user asked for something the daemon will not do.
 */
static void
test_host_computer_needs_confirmation(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) unconfirmed = NULL;
    g_autoptr(ClawtConfig) confirmed = NULL;

    unconfirmed = clawt_config_load_from_string(
        "agents:\n"
        "  - id: bold\n"
        "    computer:\n"
        "      type: host\n", &error);

    g_assert_true(clawt_agent_config_is_shadow(
        clawt_config_get_agent(unconfirmed, "bold")));
    g_assert_nonnull(strstr(
        clawt_agent_config_get_shadow_reason(
            clawt_config_get_agent(unconfirmed, "bold")),
        "confirm_host_control"));

    confirmed = clawt_config_load_from_string(
        "agents:\n"
        "  - id: bold\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n", &error);

    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(confirmed, "bold")));
}

static void
test_mounts_are_parsed(void)
{
    g_autoptr(ClawtConfig) config = load_sample();
    ClawtAgentConfig *agent = clawt_config_get_agent(config, "researcher");
    g_autoptr(GPtrArray) mounts = clawt_agent_config_get_mounts(agent);
    ClawtMount *mount;

    g_assert_cmpuint(mounts->len, ==, 1);

    mount = g_ptr_array_index(mounts, 0);
    g_assert_cmpstr(clawt_mount_get_source(mount), ==, "/tmp");
    g_assert_cmpstr(clawt_mount_get_target(mount), ==, "/work/tmp");
    g_assert_cmpint(clawt_mount_get_mode(mount), ==, CLAWT_MOUNT_MODE_RW);
    g_assert_cmpint(clawt_mount_get_relabel(mount), ==, CLAWT_RELABEL_SHARED);
}

/* Two mounts at the same target would have one silently hide the other. */
static void
test_overlapping_mount_targets_are_a_shadow(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;

    config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: clashing\n"
        "    computer:\n"
        "      mounts:\n"
        "        - source: \"/tmp\"\n"
        "          target: \"/work\"\n"
        "        - source: \"/var/tmp\"\n"
        "          target: \"/work\"\n", &error);

    g_assert_true(clawt_agent_config_is_shadow(
        clawt_config_get_agent(config, "clashing")));
}

static void
test_agent_workspace_defaults_under_root(void)
{
    g_autoptr(ClawtConfig) config = load_sample();
    ClawtAgentConfig *agent = clawt_config_get_agent(config, "chief");
    g_autofree gchar *workspace = clawt_agent_config_get_workspace(agent);

    g_assert_nonnull(workspace);
    g_assert_true(g_str_has_suffix(workspace, "/chief"));
    g_assert_true(g_path_is_absolute(workspace));
}

/* ── Files on disk ───────────────────────────────────────────────── */

static void
test_save_writes_atomically_and_keeps_a_backup(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-cfg-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "config.yaml", NULL);
    g_autofree gchar *backup = g_build_filename(dir, "config.yaml.bak", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    g_autofree gchar *contents = NULL;
    GStatBuf st;

    g_assert_true(g_file_set_contents(path, sample_config, -1, &error));

    config = clawt_config_load(path, &error);
    g_assert_no_error(error);

    clawt_config_set_string(config, "defaults.model", "haiku");
    g_assert_true(clawt_config_save(config, &error));
    g_assert_no_error(error);

    g_assert_true(g_file_test(backup, G_FILE_TEST_EXISTS));

    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_nonnull(strstr(contents, "haiku"));
    g_assert_nonnull(strstr(contents, "# where clients dial in"));

    /* A config naming credential locations should not be world-readable. */
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0077, ==, 0);

    g_unlink(path);
    g_unlink(backup);
    clawt_test_remove_tree(dir);
}

/* A missing file is a fresh config, not an error: requiring the file to
 * exist first would make the very first run impossible. */
static void
test_missing_file_loads_as_defaults(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config =
        clawt_config_load("/nonexistent/clawtilla/config.yaml", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);
    g_assert_cmpstr(clawt_config_get_string(config, "defaults.model"),
                    ==, "sonnet");
}

static void
test_validate_rejects_impossible_values(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;

    config = clawt_config_load_from_string(
        "orchestration:\n  max_hops: 0\n", &error);
    g_assert_nonnull(config);

    g_assert_false(clawt_config_validate(config, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
}

/*
 * An agent with a computer gets a standing per-turn directive naming it.
 *
 * An agent runs as a libreclaw process on the host, so its own bash,
 * read and write tools touch the host filesystem and its container is
 * reachable only through clawtilla_computer_exec. Nothing told it that,
 * and an agent with a perfectly good container sat in its workspace
 * running commands on the host and reporting, correctly, that it did
 * not appear to be in one.
 *
 * The suffix rather than the system prompt because a resumed session
 * never re-receives the system prompt: this is the rule that has to
 * hold on turn 200.
 */
static void
test_computer_directive_is_injected(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: boxed\n"
        "    computer: {type: container}\n"
        "  - id: guest\n"
        "    computer: {type: vm}\n"
        "  - id: talker\n"
        "    computer: {type: none}\n"
        "  - id: mine\n"
        "    computer: {type: container}\n"
        "    prompt_suffix: \"Always answer in French.\"\n",
        &error);
    GPtrArray *agents;
    g_autofree gchar *boxed = NULL;
    g_autofree gchar *guest = NULL;
    g_autofree gchar *talker = NULL;
    g_autofree gchar *mine = NULL;

    g_assert_no_error(error);
    agents = clawt_config_get_agents(config);
    g_assert_cmpuint(agents->len, ==, 4);

    boxed = clawt_config_render_agent(config, g_ptr_array_index(agents, 0),
                                       "/tmp/s.sock", "/tmp/state", NULL);
    g_assert_nonnull(boxed);
    g_assert_nonnull(strstr(boxed, "prompt_suffix"));

    /* Named, so the agent can say which container it means. */
    g_assert_nonnull(strstr(boxed, "clawt-boxed"));
    g_assert_nonnull(strstr(boxed, "clawtilla_computer_exec"));

    guest = clawt_config_render_agent(config, g_ptr_array_index(agents, 1),
                                       "/tmp/s.sock", "/tmp/state", NULL);
    g_assert_nonnull(strstr(guest, "clawt-guest"));
    g_assert_nonnull(strstr(guest, "virtual machine"));

    /* Nothing to redirect for an agent with no computer. */
    talker = clawt_config_render_agent(config, g_ptr_array_index(agents, 2),
                                        "/tmp/s.sock", "/tmp/state", NULL);
    g_assert_null(strstr(talker, "prompt_suffix"));

    /*
     * The user's own suffix is kept alongside ours. Replacing it would
     * be a standing rule of theirs silently discarded.
     */
    mine = clawt_config_render_agent(config, g_ptr_array_index(agents, 3),
                                      "/tmp/s.sock", "/tmp/state", NULL);
    g_assert_nonnull(strstr(mine, "clawt-mine"));
    g_assert_nonnull(strstr(mine, "Always answer in French."));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/config/reads-configured", test_reads_configured_values);
    g_test_add_func("/config/computer-directive",
                    test_computer_directive_is_injected);
    g_test_add_func("/config/schema-defaults", test_falls_back_to_schema_defaults);
    g_test_add_func("/config/agent-inherits-defaults",
                    test_agent_inherits_from_defaults);
    g_test_add_func("/config/save-preserves-comments",
                    test_save_preserves_comments);
    g_test_add_func("/config/save-idempotent", test_save_is_idempotent);
    g_test_add_func("/config/set-round-trip", test_set_and_reload_round_trip);
    g_test_add_func("/config/written-value-documented",
                    test_written_value_gets_schema_comment);
    g_test_add_func("/config/unset-returns-default",
                    test_unset_returns_to_default);
    g_test_add_func("/config/unknown-key-warns", test_unknown_key_warns_but_loads);
    g_test_add_func("/config/malformed-yaml", test_malformed_yaml_is_an_error);
    g_test_add_func("/config/empty-is-valid", test_empty_config_is_valid);
    g_test_add_func("/config/comment-only-valid",
                    test_comment_only_config_is_valid);

    g_test_add_func("/config/agents/list", test_agent_list_and_lookup);
    g_test_add_func("/config/agents/add-remove", test_add_and_remove_agent);
    g_test_add_func("/config/agents/duplicate-id",
                    test_duplicate_agent_id_is_refused);
    g_test_add_func("/config/agents/invalid-ids",
                    test_invalid_agent_ids_are_refused);
    g_test_add_func("/config/agents/unknown-computer-shadow",
                    test_unknown_computer_type_becomes_shadow);
    g_test_add_func("/config/agents/two-chiefs",
                    test_two_chiefs_of_staff_is_a_shadow);
    g_test_add_func("/config/agents/host-needs-confirmation",
                    test_host_computer_needs_confirmation);
    g_test_add_func("/config/agents/mounts", test_mounts_are_parsed);
    g_test_add_func("/config/agents/overlapping-mounts",
                    test_overlapping_mount_targets_are_a_shadow);
    g_test_add_func("/config/agents/workspace-default",
                    test_agent_workspace_defaults_under_root);

    g_test_add_func("/config/file/save-atomic",
                    test_save_writes_atomically_and_keeps_a_backup);
    g_test_add_func("/config/file/missing-loads-defaults",
                    test_missing_file_loads_as_defaults);
    g_test_add_func("/config/validate-rejects", test_validate_rejects_impossible_values);

    return g_test_run();
}
