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

/*
 * `confine: none` needs the fleet's permission as well as the agent's.
 *
 * daemon.allow_unconfined_host existed, was documented as the second of
 * two deliberate acts, defaulted to false, and gated nothing at all --
 * so one line in one agent block was enough to run unconfined on the
 * real machine, which is the single typo the rule exists to stop.
 */
static void
test_unconfined_host_needs_the_fleet_to_agree(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) refused = NULL;
    g_autoptr(ClawtConfig) allowed = NULL;
    g_autoptr(ClawtConfig) confined = NULL;
    const gchar *agent_block =
        "agents:\n"
        "  - id: bold\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n"
        "        confine: none\n";
    g_autofree gchar *with_permission =
        g_strconcat("daemon:\n"
                    "  allow_unconfined_host: true\n", agent_block, NULL);

    refused = clawt_config_load_from_string(agent_block, &error);
    g_assert_no_error(error);

    g_assert_true(clawt_agent_config_is_shadow(
        clawt_config_get_agent(refused, "bold")));
    g_assert_nonnull(strstr(
        clawt_agent_config_get_shadow_reason(
            clawt_config_get_agent(refused, "bold")),
        "daemon.allow_unconfined_host"));

    allowed = clawt_config_load_from_string(with_permission, &error);
    g_assert_no_error(error);
    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(allowed, "bold")));

    /*
     * And the gate is about `none` alone.  Applying it to every host
     * agent would make the fleet key a second confirm_host_control, and
     * a confined host computer is the ordinary case.
     */
    confined = clawt_config_load_from_string(
        "agents:\n"
        "  - id: careful\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n"
        "        confine: bwrap\n", &error);

    g_assert_no_error(error);
    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(confined, "careful")));
}

/*
 * The refusal has to name every type this build has, and it did not: it
 * said "none, host, container and vm" for as long as distrobox has
 * existed.  Asserted against the enum rather than against a list here,
 * or this test would be the second copy that goes stale.
 */
static void
test_unknown_computer_type_names_every_type(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GEnumClass) klass = g_type_class_ref(CLAWT_TYPE_COMPUTER_TYPE);
    const gchar *reason;
    guint i;

    config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: future\n"
        "    computer:\n"
        "      type: quantum-mainframe\n", &error);

    g_assert_no_error(error);

    reason = clawt_agent_config_get_shadow_reason(
        clawt_config_get_agent(config, "future"));
    g_assert_nonnull(reason);

    for (i = 0; i < klass->n_values; i++)
        g_assert_nonnull(strstr(reason, klass->values[i].value_nick));
}

/* Does any warning mention @needle? */
static gboolean
warned_about(ClawtConfig *config, const gchar *needle)
{
    GPtrArray *warnings = clawt_config_get_warnings(config);
    guint i;

    for (i = 0; warnings != NULL && i < warnings->len; i++) {
        if (strstr(g_ptr_array_index(warnings, i), needle) != NULL)
            return TRUE;
    }

    return FALSE;
}

/*
 * An option this build does not implement says so when somebody sets it.
 *
 * Both of these are parsed, stored and read by nothing: the routine's
 * jitter never reaches the runner, and the container's network reaches
 * the container object and stops. The failure is not that they are
 * unimplemented, it is that setting one looks exactly like setting one
 * that works.
 *
 * `rooms.max_hops` used to be the third, and is now enforced -- so it
 * has moved to the case below. Clearing the flag when an option is
 * implemented is the half of the job that gets forgotten, which is
 * exactly why the warning is asserted on from both directions.
 */
static void
test_an_unimplemented_option_says_so(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;

    config = clawt_config_load_from_string(
        "routines:\n"
        "  - id: morning\n"
        "    agent: chief\n"
        "    instructions: say hello\n"
        "    jitter_seconds: 30\n"
        "agents:\n"
        "  - id: chief\n"
        "    computer:\n"
        "      type: container\n"
        "      container:\n"
        "        network: host\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    /* Named with the element it was set on, not just the key. */
    g_assert_true(warned_about(config, "routines.jitter_seconds"));
    g_assert_true(warned_about(config, "morning"));
    g_assert_true(warned_about(config, "agents.computer.container.network"));

    /* And it is still a working config, not a refusal. */
    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(config, "chief")));
}

/*
 * And an option that *is* implemented says nothing.
 *
 * `rooms.max_hops` reached a #ClawtRoom and clawt_room_get_max_hops()
 * had no caller, so every hop in every room was counted against the
 * fleet limit whatever a room declared. Now that the loop guard is
 * given it, the loader must stop telling people it does nothing --
 * a warning that outlives the limitation it describes is the same
 * documentation lie as the paragraph that outlives it, one layer down,
 * and this is the one somebody reads at the moment they set the key.
 */
static void
test_an_implemented_room_limit_is_quiet(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;

    config = clawt_config_load_from_string(
        "rooms:\n"
        "  - id: standup\n"
        "    max_hops: 3\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    g_assert_false(warned_about(config, "rooms.max_hops"));
}

/*
 * A config that sets nothing inert must say nothing, or the warning is
 * noise people learn to scroll past.
 */
static void
test_an_ordinary_config_is_quiet(void)
{
    g_autoptr(ClawtConfig) config = load_sample();

    g_assert_false(warned_about(config, "nothing in this build reads"));
    g_assert_false(warned_about(config, "reads back as unset"));
}

/*
 * A comma-separated option written as a YAML list.
 *
 * yaml_node_get_string() answers NULL for a sequence, so every getter
 * falls through to the default and `readers: [chief]` is a permission
 * somebody granted, saved, and silently denied -- the worst outcome
 * available, because there is nothing at all to see.
 */
static void
test_a_scalar_written_as_a_list_warns(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) listed = NULL;
    g_autoptr(ClawtConfig) commas = NULL;

    listed = clawt_config_load_from_string(
        "agents:\n"
        "  - id: keeper\n"
        "    memories:\n"
        "      readers:\n"
        "        - chief\n", &error);

    g_assert_no_error(error);
    g_assert_true(warned_about(listed, "memories.readers"));
    g_assert_true(warned_about(listed, "reads back as unset"));

    /* Which is true: it does. */
    g_assert_null(clawt_agent_config_get_string(
        clawt_config_get_agent(listed, "keeper"), "memories.readers"));

    /* The documented spelling is quiet, and works. */
    commas = clawt_config_load_from_string(
        "agents:\n"
        "  - id: keeper\n"
        "    memories:\n"
        "      readers: \"chief, deputy\"\n", &error);

    g_assert_no_error(error);
    g_assert_false(warned_about(commas, "memories.readers"));
    g_assert_cmpstr(clawt_agent_config_get_string(
        clawt_config_get_agent(commas, "keeper"), "memories.readers"),
        ==, "chief, deputy");
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

/*
 * Mounts can be written, not only read.
 *
 * They are the only list an agent's configuration holds, and
 * clawt_agent_config_set_string() cannot express one -- it writes a
 * scalar at a dotted path. So the list was read and applied on every
 * start and no client could add to it: sharing a folder meant editing
 * the YAML by hand.
 */
static void
test_mounts_can_be_added_and_removed(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: boxed\n"
        "    computer: {type: container}\n",
        &error);
    ClawtAgentConfig *agent;
    g_autoptr(GPtrArray) before = NULL;
    g_autoptr(GPtrArray) after = NULL;
    g_autoptr(GPtrArray) gone = NULL;

    g_assert_no_error(error);
    agent = g_ptr_array_index(clawt_config_get_agents(config), 0);

    before = clawt_agent_config_get_mounts(agent);
    g_assert_cmpuint(before->len, ==, 0);

    {
        g_autoptr(ClawtMount) mount = clawt_mount_new("/srv/data", "/work");

        clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
        g_assert_true(clawt_agent_config_add_mount(agent, mount));
    }

    after = clawt_agent_config_get_mounts(agent);
    g_assert_cmpuint(after->len, ==, 1);
    g_assert_cmpstr(clawt_mount_get_target(g_ptr_array_index(after, 0)),
                    ==, "/work");
    g_assert_cmpint(clawt_mount_get_mode(g_ptr_array_index(after, 0)),
                    ==, CLAWT_MOUNT_MODE_RW);

    g_assert_false(clawt_agent_config_remove_mount(agent, "/nowhere"));
    g_assert_true(clawt_agent_config_remove_mount(agent, "/work"));

    gone = clawt_agent_config_get_mounts(agent);
    g_assert_cmpuint(gone->len, ==, 0);
}

/*
 * A mount with no relabel is shared, not none.
 *
 * A schema default only applies to a scalar at a dotted path; nothing
 * applies one to a member of a list. So an entry written without
 * `relabel` came back as none -- and on an SELinux system an unlabelled
 * bind mount is visible inside the container with every access denied,
 * which reads like a permissions bug rather than a labelling one. Every
 * shared folder anyone declared failed.
 */
static void
test_a_mount_without_relabel_is_shared(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: boxed\n"
        "    computer:\n"
        "      type: container\n"
        "      mounts:\n"
        "        - source: /srv/a\n"
        "          target: /a\n"
        "        - source: /srv/b\n"
        "          target: /b\n"
        "          relabel: none\n",
        &error);
    g_autoptr(GPtrArray) mounts = NULL;

    g_assert_no_error(error);
    mounts = clawt_agent_config_get_mounts(
        g_ptr_array_index(clawt_config_get_agents(config), 0));

    g_assert_cmpuint(mounts->len, ==, 2);
    g_assert_cmpint(clawt_mount_get_relabel(g_ptr_array_index(mounts, 0)),
                    ==, CLAWT_RELABEL_SHARED);

    /* An explicit none is still none: this is a default, not a policy. */
    g_assert_cmpint(clawt_mount_get_relabel(g_ptr_array_index(mounts, 1)),
                    ==, CLAWT_RELABEL_NONE);
}


/* ── teams ───────────────────────────────────────────────────────── */

/*
 * Teams are optional, and a fleet that declares none must behave exactly
 * as it did before there were any -- which is the whole reason an empty
 * list is a normal answer rather than a missing section.
 */
static void
test_a_fleet_with_no_teams_has_none(void)
{
    g_autoptr(ClawtConfig) config =
        clawt_config_load_from_string("agents:\n  - id: alpha\n", NULL);
    g_autoptr(GPtrArray) teams = clawt_config_get_teams(config);

    g_assert_nonnull(teams);
    g_assert_cmpuint(teams->len, ==, 0);
}

static void
test_teams_round_trip(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GPtrArray) teams = NULL;
    g_autoptr(ClawtTeamSpec) one = NULL;
    g_autoptr(GError) error = NULL;

    config = clawt_config_load_from_string("agents:\n  - id: alpha\n", NULL);

    g_assert_true(clawt_config_add_team(config, "research", &error));
    g_assert_no_error(error);

    g_assert_true(clawt_config_set_team_string(config, "research", "name",
                                               "Research"));
    g_assert_true(clawt_config_set_team_string(
        config, "research", "description",
        "Reads things and answers questions about them."));

    teams = clawt_config_get_teams(config);
    g_assert_cmpuint(teams->len, ==, 1);

    one = clawt_config_get_team(config, "research");
    g_assert_nonnull(one);
    g_assert_cmpstr(one->id, ==, "research");
    g_assert_cmpstr(one->name, ==, "Research");
    g_assert_nonnull(strstr(one->description, "answers questions"));
}

/*
 * A duplicate id is refused rather than added, because everything else
 * -- an agent's team, a delegation, the sidebar -- addresses a team by
 * that id, and two teams answering to one name is not a state anything
 * downstream can be right about.
 */
static void
test_a_duplicate_team_is_refused(void)
{
    g_autoptr(ClawtConfig) config =
        clawt_config_load_from_string("agents:\n  - id: alpha\n", NULL);
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_config_add_team(config, "research", NULL));
    g_assert_false(clawt_config_add_team(config, "research", &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);
}

/* ...and so is an id nothing could address it by. */
static void
test_a_team_id_must_be_usable(void)
{
    g_autoptr(ClawtConfig) config =
        clawt_config_load_from_string("agents:\n  - id: alpha\n", NULL);
    g_autoptr(GError) error = NULL;

    g_assert_false(clawt_config_add_team(config, "Not A Team", &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
}

/*
 * The id is not settable through the field setter. Everything refers to
 * a team by it, so changing it in place would leave every agent naming a
 * team that no longer exists.
 */
static void
test_a_team_id_cannot_be_edited_in_place(void)
{
    g_autoptr(ClawtConfig) config =
        clawt_config_load_from_string("agents:\n  - id: alpha\n", NULL);

    g_assert_true(clawt_config_add_team(config, "research", NULL));
    g_assert_false(clawt_config_set_team_string(config, "research", "id",
                                                "other"));
}

/*
 * Removing a team leaves its agents alone. Being teamless is a state an
 * agent is allowed to be in, and rewriting every one of them from here
 * would be a second thing to get wrong.
 */
static void
test_removing_a_team_leaves_its_agents(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    ClawtAgentConfig *agent;

    config = clawt_config_load_from_string(
        "teams:\n  - id: research\n"
        "agents:\n  - id: alpha\n    team: research\n", NULL);

    g_assert_true(clawt_config_remove_team(config, "research"));
    g_assert_false(clawt_config_remove_team(config, "research"));

    agent = clawt_config_get_agent(config, "alpha");
    g_assert_nonnull(agent);
    g_assert_cmpstr(clawt_agent_config_get_string(agent, "team"), ==,
                    "research");
}

/*
 * Order decides the list, and ties keep the order the file has -- so a
 * fleet nobody has arranged does not reshuffle its own sidebar between
 * listings.
 */
static void
test_teams_come_back_in_order(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GPtrArray) teams = NULL;

    config = clawt_config_load_from_string(
        "teams:\n"
        "  - id: zulu\n"
        "  - id: alpha\n    order: -10\n"
        "  - id: mike\n", NULL);

    teams = clawt_config_get_teams(config);
    g_assert_cmpuint(teams->len, ==, 3);

    g_assert_cmpstr(((ClawtTeamSpec *)g_ptr_array_index(teams, 0))->id, ==,
                    "alpha");
    g_assert_cmpstr(((ClawtTeamSpec *)g_ptr_array_index(teams, 1))->id, ==,
                    "zulu");
    g_assert_cmpstr(((ClawtTeamSpec *)g_ptr_array_index(teams, 2))->id, ==,
                    "mike");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/config/reads-configured", test_reads_configured_values);
    g_test_add_func("/config/teams/none-by-default",
                    test_a_fleet_with_no_teams_has_none);
    g_test_add_func("/config/teams/round-trip", test_teams_round_trip);
    g_test_add_func("/config/teams/duplicate-refused",
                    test_a_duplicate_team_is_refused);
    g_test_add_func("/config/teams/id-must-be-usable",
                    test_a_team_id_must_be_usable);
    g_test_add_func("/config/teams/id-not-editable",
                    test_a_team_id_cannot_be_edited_in_place);
    g_test_add_func("/config/teams/remove-leaves-agents",
                    test_removing_a_team_leaves_its_agents);
    g_test_add_func("/config/teams/ordered", test_teams_come_back_in_order);
    g_test_add_func("/config/mounts-writable",
                    test_mounts_can_be_added_and_removed);
    g_test_add_func("/config/mount-relabel-default",
                    test_a_mount_without_relabel_is_shared);
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
    g_test_add_func("/config/agents/unconfined-host-needs-the-fleet",
                    test_unconfined_host_needs_the_fleet_to_agree);
    g_test_add_func("/config/agents/unknown-computer-type-names-every-type",
                    test_unknown_computer_type_names_every_type);
    g_test_add_func("/config/unimplemented-option-says-so",
                    test_an_unimplemented_option_says_so);
    g_test_add_func("/config/implemented-room-limit-is-quiet",
                    test_an_implemented_room_limit_is_quiet);
    g_test_add_func("/config/ordinary-config-is-quiet",
                    test_an_ordinary_config_is_quiet);
    g_test_add_func("/config/scalar-written-as-a-list-warns",
                    test_a_scalar_written_as_a_list_warns);
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
