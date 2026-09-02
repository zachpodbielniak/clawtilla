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
 * A well-formed id can still be a name the routing keys on.
 *
 * An agent called "clawtilla" would sign every message as the system --
 * passing the loop guard unmeasured and closing each exchange it lands
 * in -- and one called "user" would read as the operator to
 * is_operator_room().  These are reserved at the door, where the other
 * shape rules already live.
 */
static void
test_reserved_sender_names_are_not_agent_ids(void)
{
    static const gchar *reserved[] = {
        "user", "clawtilla", "routine", "trigger", NULL
    };
    g_autoptr(ClawtConfig) config = clawt_config_new();
    gsize i;

    for (i = 0; reserved[i] != NULL; i++) {
        g_autoptr(GError) error = NULL;

        g_assert_true(clawt_agent_id_is_reserved(reserved[i]));
        g_assert_null(clawt_config_add_agent(config, reserved[i], &error));
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
        g_assert_nonnull(strstr(error->message, "reserved"));
    }

    g_assert_false(clawt_agent_id_is_reserved("oryx"));
    g_assert_false(clawt_agent_id_is_reserved(NULL));
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
 * Both of these are parsed, stored and read by nothing: the MEMORY.md
 * line budget never reaches libreclaw, and the container's network
 * reaches the container object and stops. The failure is not that they
 * are unimplemented, it is that setting one looks exactly like setting
 * one that works.
 *
 * `rooms.max_hops` used to be the third and `routines.jitter_seconds`
 * the fourth; both are enforced now, so both have moved to the cases
 * below. Clearing the flag when an option is implemented is the half of
 * the job that gets forgotten, which is exactly why the warning is
 * asserted on from both directions.
 */
static void
test_an_unimplemented_option_says_so(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;

    config = clawt_config_load_from_string(
        "agents:\n"
        "  - id: chief\n"
        "    memory:\n"
        "      max_lines: 40\n"
        "    computer:\n"
        "      type: container\n"
        "      container:\n"
        "        network: host\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    /* Named with the element it was set on, not just the key. */
    g_assert_true(warned_about(config, "agents.memory.max_lines"));
    g_assert_true(warned_about(config, "chief"));
    g_assert_true(warned_about(config, "agents.computer.container.network"));

    /* And it is still a working config, not a refusal. */
    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(config, "chief")));
}

/*
 * And the jitter, which is now read by the runner, says nothing.
 *
 * The other direction of the same rule. `routines.jitter_seconds` was
 * accepted, saved and read by nothing for two releases, and the symptom
 * was that it looked exactly like a very small random offset -- setting
 * it to 300 and watching a routine fire at 09:00 is what a working
 * jitter would also look like on any given morning. Now that
 * clawt_routine_runner_tick() holds a due routine back, the loader must
 * stop telling people it does nothing.
 */
static void
test_an_implemented_jitter_is_quiet(void)
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
        "  - id: chief\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    g_assert_false(warned_about(config, "routines.jitter_seconds"));
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
 * The routing mode reaches the agent's own config.yaml, and libreclaw
 * understands every value clawtilla can render.
 *
 * libreclaw has read session.routing_mode for as long as it has had a
 * router, and clawtilla had never written it -- so an agent's session
 * partitioning was unconfigurable from the fleet, and the chief of
 * staff this surfaced on was three context windows that had never met.
 * The schema's value list is lc_routing_mode_get_type() itself, so the
 * two ends cannot disagree about what exists; what can still break is
 * the *spelling* -- clawtilla renders enum nicknames and libreclaw's
 * parser historically accepted an underscore form -- so the walk below
 * renders every nickname and hands the result to libreclaw's own
 * parser.  A value that renders but does not parse is a mode a fleet
 * can name and silently not get: libreclaw warns and reverts to
 * sender_room, which is precisely the partitioning the operator was
 * trying to leave.
 */
static void
test_routing_mode_survives_the_render_parse_round_trip(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-cfg-XXXXXX", NULL);
    GEnumClass *cls = g_type_class_ref(LC_TYPE_ROUTING_MODE);
    guint i;

    /* Unset, the default is stated rather than omitted. */
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
            "agents:\n  - id: plain\n", &error);
        GPtrArray *agents;
        g_autofree gchar *rendered = NULL;

        g_assert_no_error(error);
        agents = clawt_config_get_agents(config);

        rendered = clawt_config_render_agent(
            config, g_ptr_array_index(agents, 0),
            "/tmp/s.sock", "/tmp/state", NULL);

        g_assert_nonnull(strstr(rendered,
                                "routing_mode: \"sender-room\""));
    }

    for (i = 0; i < cls->n_values; i++) {
        const GEnumValue *value = &cls->values[i];
        g_autoptr(GError) error = NULL;
        g_autoptr(ClawtConfig) config = NULL;
        g_autoptr(LcConfig) parsed = lc_config_new();
        GPtrArray *agents;
        g_autofree gchar *yaml = NULL;
        g_autofree gchar *rendered = NULL;
        g_autofree gchar *needle = NULL;
        g_autofree gchar *path = NULL;

        yaml = g_strdup_printf("agents:\n"
                               "  - id: modal\n"
                               "    session:\n"
                               "      routing_mode: %s\n",
                               value->value_nick);

        config = clawt_config_load_from_string(yaml, &error);
        g_assert_no_error(error);
        agents = clawt_config_get_agents(config);

        rendered = clawt_config_render_agent(
            config, g_ptr_array_index(agents, 0),
            "/tmp/s.sock", "/tmp/state", NULL);

        needle = g_strdup_printf("routing_mode: \"%s\"",
                                 value->value_nick);
        g_assert_nonnull(strstr(rendered, needle));

        /* And libreclaw's own parser reads the mode back out. */
        path = g_strdup_printf("%s/rendered-%s.yaml", dir,
                               value->value_nick);
        g_assert_true(g_file_set_contents(path, rendered, -1, &error));
        g_assert_no_error(error);

        g_assert_true(lc_config_load_from_path(parsed, path, &error));
        g_assert_no_error(error);

        g_assert_cmpint(lc_config_get_routing_mode(parsed), ==,
                        value->value);
    }

    g_type_class_unref(cls);
    clawt_test_remove_tree(dir);
}

/*
 * An orchestrator's routing defaults to agent mode.
 *
 * The role is the reason the mode exists: a chief of staff or a team
 * lead carries one operator's intent across conversations, and the
 * partitioned default is the wrong shape for that job.  The role moves
 * the *default* -- an explicit value always wins, in either direction,
 * and toggling the role writes nothing -- and the fleet-level spelling
 * of the chief role counts, because naming the chief in
 * orchestration.chief_of_staff is documented as equivalent to marking
 * the agent.
 */
static void
test_an_orchestrators_routing_defaults_to_agent(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "orchestration:\n"
        "  chief_of_staff: named\n"
        "agents:\n"
        "  - id: chief\n"
        "    chief_of_staff: true\n"
        "  - id: lead\n"
        "    team_role: lead\n"
        "  - id: worker\n"
        "  - id: contrarian\n"
        "    chief_of_staff: true\n"
        "    session:\n"
        "      routing_mode: sender-room\n"
        "  - id: named\n", &error);
    GPtrArray *agents;
    g_autofree gchar *rendered = NULL;

    g_assert_no_error(error);
    agents = clawt_config_get_agents(config);
    g_assert_cmpuint(agents->len, ==, 5);

    g_assert_cmpstr(
        clawt_agent_config_get_string(g_ptr_array_index(agents, 0),
                                      "session.routing_mode"),
        ==, "agent");
    g_assert_cmpstr(
        clawt_agent_config_get_string(g_ptr_array_index(agents, 1),
                                      "session.routing_mode"),
        ==, "agent");
    g_assert_cmpstr(
        clawt_agent_config_get_string(g_ptr_array_index(agents, 2),
                                      "session.routing_mode"),
        ==, "sender-room");
    g_assert_cmpstr(
        clawt_agent_config_get_string(g_ptr_array_index(agents, 3),
                                      "session.routing_mode"),
        ==, "sender-room");
    g_assert_cmpstr(
        clawt_agent_config_get_string(g_ptr_array_index(agents, 4),
                                      "session.routing_mode"),
        ==, "agent");

    /* And the default reaches the rendered config.yaml, not only the
     * getter -- the render is what libreclaw actually reads. */
    rendered = clawt_config_render_agent(
        config, g_ptr_array_index(agents, 0),
        "/tmp/s.sock", "/tmp/state", NULL);
    g_assert_nonnull(strstr(rendered, "routing_mode: \"agent\""));
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
 * An agent that declares no mounts still gets the fleet's.
 *
 * Pinned from both spellings: no `mounts` key at all, and an explicit
 * empty list.  Removing an agent's only mount used to leave `mounts:
 * []` behind, and an empty list looks like an override -- somebody read
 * it as one, concluded the fleet default had been suppressed, and went
 * looking for a bug in the merge.  There is none: both spellings reach
 * mounts_from_node() as a zero-length list and the factory merges the
 * defaults either way.  This says so, so that stays true.
 */
static void
test_an_empty_mount_list_still_inherits_the_defaults(void)
{
    /*
     * A VM rather than a container, only because the container branch
     * of the factory refuses without podomation's module and `make
     * test` must not need one.  Both take mounts through the same
     * apply_mounts().
     */
    static const gchar *const yamls[] = {
        "defaults:\n"
        "  mounts:\n"
        "    - source: \"/srv/shared\"\n"
        "      target: \"/data/shared\"\n"
        "agents:\n"
        "  - id: boxed\n"
        "    computer: {type: vm}\n",

        "defaults:\n"
        "  mounts:\n"
        "    - source: \"/srv/shared\"\n"
        "      target: \"/data/shared\"\n"
        "agents:\n"
        "  - id: boxed\n"
        "    computer:\n"
        "      type: vm\n"
        "      mounts: []\n"
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(yamls); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(ClawtConfig) config =
            clawt_config_load_from_string(yamls[i], &error);
        g_autoptr(GPtrArray) defaults = NULL;
        g_autoptr(ClawtComputer) computer = NULL;
        GPtrArray *mounts;
        ClawtAgentConfig *agent;
        guint m;
        gboolean found = FALSE;

        g_assert_no_error(error);

        agent = clawt_config_get_agent(config, "boxed");
        g_assert_nonnull(agent);

        /* Its own list is empty under both spellings. */
        {
            g_autoptr(GPtrArray) own = clawt_agent_config_get_mounts(agent);

            g_assert_cmpuint(own->len, ==, 0);
        }

        defaults = clawt_config_get_default_mounts(config);
        g_assert_cmpuint(defaults->len, ==, 1);

        computer = clawt_computer_factory_create(agent, defaults, NULL,
                                                  &error);
        g_assert_no_error(error);

        mounts = clawt_computer_get_mounts(computer);

        for (m = 0; mounts != NULL && m < mounts->len; m++) {
            if (g_strcmp0(clawt_mount_get_target(
                              g_ptr_array_index(mounts, m)),
                          "/data/shared") == 0)
                found = TRUE;
        }

        g_assert_true(found);
    }
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

/*
 * Whether a schema key lives inside a list of mappings.
 *
 * The same derivation clawt-schema-render.c makes, and for the same
 * reason: an `agents.*` or `integrations.*` key belongs to a block and
 * is read by that block's own getter, not by the fleet one.  Derived
 * from CLAWT_SCHEMA_LIST_OF rather than from a list of section names,
 * so a seventh list added later is classified without editing this.
 */
static gboolean
key_is_inside_a_list_of(const gchar *key)
{
    const gchar *dot = key;

    while ((dot = strchr(dot, '.')) != NULL) {
        g_autofree gchar *prefix = g_strndup(key, (gsize)(dot - key));
        const ClawtSchemaEntry *parent = clawt_config_schema_lookup(prefix);

        if (parent != NULL && parent->type == CLAWT_SCHEMA_LIST_OF)
            return TRUE;

        dot++;
    }

    return FALSE;
}

/*
 * Every fleet-level list default is reachable through the fleet getter.
 *
 * clawt_config_get_string_list() was the one getter with no schema
 * fallback, so a STRING_LIST default was rendered into
 * data/default-config.yaml, written into docs/configuration-options.org
 * and handed to nobody.  The one key that has a default,
 * orchestration.repeat_thresholds, is read at every daemon start by
 * clawt_daemon_turn_configure(), which passed the NULL straight to
 * clawt_repeat_watch_set_thresholds() -- and that empties the array the
 * watch had filled with the same "5,10,20" in its own init.  Repeat
 * detection was therefore off on every fleet that had not written the
 * key out by hand, which is every fleet using the generated config,
 * since the key ships commented.
 *
 * Walked rather than named, and the expected value comes from the
 * schema, so this covers the next list default rather than this one.
 */
static void
test_every_fleet_list_default_is_reachable(void)
{
    g_autoptr(ClawtConfig) config = clawt_config_new();
    const ClawtSchemaEntry *entries;
    gsize n_entries = 0;
    gsize i;
    guint checked = 0;

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        g_auto(GStrv) got = NULL;
        g_auto(GStrv) want = NULL;
        guint n;

        if (entries[i].type != CLAWT_SCHEMA_STRING_LIST)
            continue;

        if (entries[i].default_value == NULL)
            continue;

        if (key_is_inside_a_list_of(entries[i].key))
            continue;

        got = clawt_config_get_string_list(config, entries[i].key);
        want = g_strsplit(entries[i].default_value, ",", -1);

        if (got == NULL)
            g_error("schema key '%s' declares the default '%s' and "
                    "clawt_config_get_string_list() answers NULL",
                    entries[i].key, entries[i].default_value);

        g_assert_cmpuint(g_strv_length(got), ==, g_strv_length(want));

        for (n = 0; want[n] != NULL; n++)
            g_assert_cmpstr(got[n], ==, want[n]);

        checked++;
    }

    /*
     * And the walk actually reached something.  A filter that excluded
     * every entry would leave this green with nothing tested, which is
     * the shape of failure this test exists to catch.
     */
    g_assert_cmpuint(checked, >, 0);
}

/*
 * A value written in the file still wins over the schema default.
 *
 * The fallback above is a fallback: a fleet that sets the key gets what
 * it asked for rather than the schema's answer laid over the top.
 */
static void
test_a_written_list_wins_over_the_schema_default(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_auto(GStrv) written = NULL;
    g_autoptr(GError) error = NULL;

    config = clawt_config_load_from_string(
        "orchestration:\n"
        "  repeat_thresholds: [7, 9]\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    written = clawt_config_get_string_list(config,
                                           "orchestration.repeat_thresholds");

    g_assert_nonnull(written);
    g_assert_cmpuint(g_strv_length(written), ==, 2);
    g_assert_cmpstr(written[0], ==, "7");
    g_assert_cmpstr(written[1], ==, "9");
}

/*
 * defaults.autostart reaches an agent that says nothing about it.
 *
 * The whole point of the `defaults:` section is that an agent which is
 * silent follows the fleet, and the schema says as much on
 * agents.runtime.autostart -- "Defaults to defaults.autostart."  There
 * was no relation behind that sentence, so the getter had nothing to
 * fall back through and answered FALSE for every agent however the
 * fleet was configured.  The daemon's autostart scheduling and the
 * `autostart` field in the agent listing both ask this getter, so a
 * fleet that asked for its agents to come up with the daemon got a
 * fleet that did not, and a client that showed the setting showed
 * `false` next to the `true` in the file.
 */
static void
test_defaults_autostart_reaches_a_silent_agent(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *silent;
    ClawtAgentConfig *explicit_off;

    config = clawt_config_load_from_string(
        "defaults:\n"
        "  autostart: true\n"
        "agents:\n"
        "  - id: follower\n"
        "  - id: refuser\n"
        "    runtime:\n"
        "      autostart: false\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    silent = clawt_config_get_agent(config, "follower");
    g_assert_nonnull(silent);
    g_assert_true(clawt_agent_config_get_boolean(silent,
                                                 "runtime.autostart"));

    /* And an agent that said so still overrides the fleet. */
    explicit_off = clawt_config_get_agent(config, "refuser");
    g_assert_nonnull(explicit_off);
    g_assert_false(clawt_agent_config_get_boolean(explicit_off,
                                                  "runtime.autostart"));
}

/*
 * A mount survives being written and read back, `create` and `required`
 * included.
 *
 * add_mount_to_node() wrote source, target, mode, type, relabel, size,
 * scope, agents and teams -- and not the two booleans, which
 * mounts_from_node() reads and clawt_daemon_mount_from_payload() takes
 * from the wire.  So `agent mount add --required=false --create` was
 * accepted, validated, reported as added and saved without them: the
 * next reload gave `required: true` and `create: false`, which are the
 * opposite of both.  For `required` that is the difference between a
 * laptop whose agent starts when a share is missing and one that
 * refuses to, which is exactly the case the option's own documentation
 * recommends it for.
 *
 * Round-tripped through the writer and the parser rather than asserting
 * on the YAML, so the test is about the two agreeing rather than about
 * a spelling.
 */
static void
test_a_mount_round_trips_every_field(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtConfig) reloaded = NULL;
    g_autoptr(ClawtMount) mount = NULL;
    g_autoptr(GPtrArray) written = NULL;
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent;
    ClawtMount *back;

    config = clawt_config_load_from_string("agents:\n  - id: worker\n",
                                           &error);
    g_assert_no_error(error);

    agent = clawt_config_get_agent(config, "worker");
    g_assert_nonnull(agent);

    mount = clawt_mount_new("/srv/share", "/work/share");
    clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
    clawt_mount_set_create(mount, TRUE);
    clawt_mount_set_required(mount, FALSE);

    g_assert_true(clawt_agent_config_add_mount(agent, mount));

    yaml = clawt_config_to_string(config);
    g_assert_nonnull(yaml);

    reloaded = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);
    g_assert_nonnull(reloaded);

    written = clawt_agent_config_get_mounts(
        clawt_config_get_agent(reloaded, "worker"));

    g_assert_cmpuint(written->len, ==, 1);
    back = g_ptr_array_index(written, 0);

    g_assert_cmpstr(clawt_mount_get_target(back), ==, "/work/share");
    g_assert_cmpint(clawt_mount_get_mode(back), ==, CLAWT_MOUNT_MODE_RW);
    g_assert_true(clawt_mount_get_create(back));
    g_assert_false(clawt_mount_get_required(back));
}

/*
 * A mount that names no mode gets the mode the schema documents.
 *
 * The parser never consults a schema default for a mount -- the fields
 * come from clawt_mount_new() -- so the table's word and the code's
 * behaviour are two separate claims that nothing compared.  They had
 * come apart: `defaults.mounts.mode` said `rw` while every mount
 * without a mode was made read-only, which put "rw" into
 * data/example-config.yaml and docs/configuration-options.org as the
 * default for a list that reaches the whole fleet.  Wrong in the
 * unsafe direction, and the fix is the table rather than the parser:
 * a fleet-wide mount silently becoming writable is the failure the
 * agent-level row's own documentation is about.
 *
 * Both lists are checked from their own schema rows, so the two cannot
 * drift from the code or from each other unnoticed.
 */
static void
test_a_mount_without_a_mode_matches_the_schema(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GPtrArray) fleet = NULL;
    g_autoptr(GPtrArray) own = NULL;
    g_autoptr(GError) error = NULL;
    const ClawtSchemaEntry *entry;
    gint documented = 0;

    config = clawt_config_load_from_string(
        "defaults:\n"
        "  mounts:\n"
        "    - source: \"/tmp\"\n"
        "      target: \"/work/fleet\"\n"
        "agents:\n"
        "  - id: worker\n"
        "    computer:\n"
        "      mounts:\n"
        "        - source: \"/tmp\"\n"
        "          target: \"/work/own\"\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    entry = clawt_config_schema_lookup("defaults.mounts.mode");
    g_assert_nonnull(entry);
    g_assert_true(clawt_enum_from_nick(clawt_mount_mode_get_type(),
                                       entry->default_value, &documented));

    fleet = clawt_config_get_default_mounts(config);
    g_assert_cmpuint(fleet->len, ==, 1);
    g_assert_cmpint(clawt_mount_get_mode(g_ptr_array_index(fleet, 0)), ==,
                    documented);

    entry = clawt_config_schema_lookup("agents.computer.mounts.mode");
    g_assert_nonnull(entry);
    g_assert_true(clawt_enum_from_nick(clawt_mount_mode_get_type(),
                                       entry->default_value, &documented));

    own = clawt_agent_config_get_mounts(
        clawt_config_get_agent(config, "worker"));
    g_assert_cmpuint(own->len, ==, 1);
    g_assert_cmpint(clawt_mount_get_mode(g_ptr_array_index(own, 0)), ==,
                    documented);
}

/*
 * A misspelt enum value falls back to the documented default, and says
 * so, rather than becoming zero.
 *
 * clawt_config_get_enum() returned 0 when the written nick did not
 * parse.  Zero is not "unset" for any of these types -- it is
 * `never` for a restart policy, `ro` for a mount mode, `none` for a
 * computer type -- so a typo silently selected a real, different,
 * documented behaviour.  `restart: sometimes` meant an agent that
 * would never be restarted, chosen by a fleet that had asked for the
 * opposite, with nothing in the log and nothing in `agent show` to
 * say which of the two had happened.
 *
 * The default is what the operator can look up, so that is what it
 * runs on, and the load-time walk that already reports unknown keys
 * reports the unknown value too.
 */
static void
test_a_misspelt_enum_falls_back_and_is_reported(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(GError) error = NULL;
    const ClawtSchemaEntry *entry;
    GPtrArray *warnings;
    gint documented = 0;
    gboolean mentioned = FALSE;
    guint i;

    config = clawt_config_load_from_string(
        "defaults:\n"
        "  restart: sometimes\n"
        "agents:\n"
        "  - id: worker\n", &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    entry = clawt_config_schema_lookup("defaults.restart");
    g_assert_nonnull(entry);
    g_assert_true(clawt_enum_from_nick(entry->enum_type(),
                                       entry->default_value, &documented));

    g_assert_cmpint(clawt_config_get_enum(config, "defaults.restart"), ==,
                    documented);

    /*
     * And it is not a silent substitution: a value nobody can look up
     * has to be reported the way an unknown key is.
     */
    warnings = clawt_config_get_warnings(config);
    g_assert_nonnull(warnings);

    for (i = 0; i < warnings->len; i++) {
        const gchar *said = g_ptr_array_index(warnings, i);

        if (strstr(said, "sometimes") != NULL &&
            strstr(said, "defaults.restart") != NULL)
            mentioned = TRUE;
    }

    g_assert_true(mentioned);
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
    g_test_add_func("/config/mounts/empty-list-still-inherits",
                    test_an_empty_mount_list_still_inherits_the_defaults);
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
    g_test_add_func("/config/agents/reserved-sender-names",
                    test_reserved_sender_names_are_not_agent_ids);
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
    g_test_add_func("/config/implemented-jitter-is-quiet",
                    test_an_implemented_jitter_is_quiet);
    g_test_add_func("/config/implemented-room-limit-is-quiet",
                    test_an_implemented_room_limit_is_quiet);
    g_test_add_func("/config/ordinary-config-is-quiet",
                    test_an_ordinary_config_is_quiet);
    g_test_add_func("/config/scalar-written-as-a-list-warns",
                    test_a_scalar_written_as_a_list_warns);
    g_test_add_func("/config/agents/mounts", test_mounts_are_parsed);
    g_test_add_func("/config/agents/mount-round-trips-every-field",
                    test_a_mount_round_trips_every_field);
    g_test_add_func("/config/agents/mount-without-a-mode-matches-the-schema",
                    test_a_mount_without_a_mode_matches_the_schema);
    g_test_add_func("/config/agents/overlapping-mounts",
                    test_overlapping_mount_targets_are_a_shadow);
    g_test_add_func("/config/agents/workspace-default",
                    test_agent_workspace_defaults_under_root);

    g_test_add_func("/config/list-defaults/every-fleet-list-default-is-reachable",
                    test_every_fleet_list_default_is_reachable);
    g_test_add_func("/config/list-defaults/a-written-list-wins",
                    test_a_written_list_wins_over_the_schema_default);
    g_test_add_func("/config/defaults/autostart-reaches-a-silent-agent",
                    test_defaults_autostart_reaches_a_silent_agent);
    g_test_add_func("/config/enums/a-misspelt-value-falls-back-and-is-reported",
                    test_a_misspelt_enum_falls_back_and_is_reported);

    g_test_add_func("/config/file/save-atomic",
                    test_save_writes_atomically_and_keeps_a_backup);
    g_test_add_func("/config/file/missing-loads-defaults",
                    test_missing_file_loads_as_defaults);
    g_test_add_func("/config/validate-rejects", test_validate_rejects_impossible_values);
    g_test_add_func(
        "/config/routing-mode-survives-the-render-parse-round-trip",
        test_routing_mode_survives_the_render_parse_round_trip);
    g_test_add_func(
        "/config/an-orchestrators-routing-defaults-to-agent",
        test_an_orchestrators_routing_defaults_to_agent);

    return g_test_run();
}
