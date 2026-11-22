/*
 * test-ephemeral.c - Team tags, and the helpers agents spawn and reap
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The library half: tag syntax, the well-known table, which team a tag
 * means, the fleet warnings, and the name generator.  The daemon half
 * drives spawning and reaping against a fixture fleet with two parents,
 * because "only the parent may reap" is a rule about two agents and a
 * test with one cannot see it broken.  Spawned helpers are made but not
 * started (ephemeral_no_start): make test runs no agent CLI.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

/* ── Tags ───────────────────────────────────────────────────────── */

static void
test_tag_syntax(void)
{
    g_assert_true(clawt_team_tag_is_valid("ephemeral"));
    g_assert_true(clawt_team_tag_is_valid("night-shift2"));
    g_assert_true(clawt_team_tag_is_valid("acme:night-shift"));

    g_assert_false(clawt_team_tag_is_valid(NULL));
    g_assert_false(clawt_team_tag_is_valid(""));
    g_assert_false(clawt_team_tag_is_valid("Ephemeral"));
    g_assert_false(clawt_team_tag_is_valid("2fast"));
    g_assert_false(clawt_team_tag_is_valid("trailing-"));
    g_assert_false(clawt_team_tag_is_valid("a:b:c"));
    g_assert_false(clawt_team_tag_is_valid(":ephemeral"));
    g_assert_false(clawt_team_tag_is_valid("has space"));
}

/* Every well-known tag is valid, documented, and found by name. */
static void
test_well_known_table(void)
{
    g_autofree gchar *doc_path = g_build_filename(CLAWT_TEST_FIXTURES, "..", "..",
                                                  "docs", "team-tags.org", NULL);
    g_autofree gchar *doc = NULL;
    guint i;

    g_assert_cmpuint(clawt_team_tag_count(), >, 0);
    g_assert_null(clawt_team_tag_nth(clawt_team_tag_count()));
    g_assert_true(g_file_get_contents(doc_path, &doc, NULL, NULL));

    for (i = 0; i < clawt_team_tag_count(); i++) {
        const ClawtTeamTagInfo *info = clawt_team_tag_nth(i);
        g_autofree gchar *row = g_strdup_printf("| =%s= |", info->name);

        g_assert_true(clawt_team_tag_is_valid(info->name));
        g_assert_true(clawt_team_tag_lookup(info->name) == info);
        /* the docs' table is the table, not a copy that drifts */
        g_assert_nonnull(strstr(doc, row));
    }
    g_assert_null(clawt_team_tag_lookup("acme:night-shift"));
}

/* The first team in the list keeps a unique tag, and the warning says so. */
static void
test_find_by_tag_and_warnings(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "teams:\n"
        "  - id: helpers\n    order: 1\n    tags: [ephemeral, acme:night]\n"
        "  - id: temps\n    order: 2\n    tags: [ephemeral]\n"
        "  - id: odd\n    order: 3\n    tags: [Not-A-Tag]\n", &error);
    g_auto(GStrv) warnings = NULL;
    g_autofree gchar *found = NULL;
    g_autofree gchar *none = NULL;
    gboolean duplicate = FALSE;
    gboolean malformed = FALSE;
    gsize i;

    g_assert_no_error(error);
    found = clawt_team_find_by_tag(config, CLAWT_TEAM_TAG_EPHEMERAL);
    g_assert_cmpstr(found, ==, "helpers");
    none = clawt_team_find_by_tag(config, "office-custodial");
    g_assert_null(none);

    g_assert_false(clawt_team_validate_fleet(config, &warnings));
    for (i = 0; warnings != NULL && warnings[i] != NULL; i++) {
        if (strstr(warnings[i], "'helpers' keeps it") != NULL &&
            strstr(warnings[i], "'temps'") != NULL)
            duplicate = TRUE;
        if (strstr(warnings[i], "'Not-A-Tag'") != NULL)
            malformed = TRUE;
    }
    g_assert_true(duplicate);
    g_assert_true(malformed);
}

/* ── Names ──────────────────────────────────────────────────────── */

static gboolean
taken_in(const gchar *name, gpointer user_data)
{
    return g_hash_table_contains(user_data, name);
}

static void
test_names(void)
{
    g_autoptr(GRand) a = g_rand_new_with_seed(42);
    g_autoptr(GRand) b = g_rand_new_with_seed(42);
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
    g_autofree gchar *first = clawt_ephemeral_name_generate(a, NULL, NULL);
    g_autofree gchar *again = clawt_ephemeral_name_generate(b, NULL, NULL);
    guint i;

    /* reproducible from a seed, and always an id */
    g_assert_cmpstr(first, ==, again);
    g_assert_cmpuint(clawt_ephemeral_name_pair_count(), >, 1000);

    for (i = 0; i < 300; i++) {
        gchar *name = clawt_ephemeral_name_generate(a, taken_in, seen);

        g_assert_true(clawt_is_valid_id(name));
        g_assert_nonnull(strchr(name, '-'));
        g_assert_false(g_hash_table_contains(seen, name));
        g_hash_table_add(seen, name);
    }
}

/* ── The daemon ─────────────────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    GMainContext *context;
    ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_start(DaemonFixture *fixture)
{
    g_autoptr(GError) error = NULL;

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path, fixture->context);
    clawt_daemon_start(fixture->daemon, &error);
    g_assert_no_error(error);
    fixture->daemon->ephemeral_no_start = TRUE;
}

static void
daemon_stop(DaemonFixture *fixture)
{
    clawt_daemon_stop(fixture->daemon);
    g_clear_object(&fixture->daemon);
    while (g_main_context_iteration(fixture->context, FALSE))
        ;
    g_clear_pointer(&fixture->context, g_main_context_unref);
}

static void
daemon_setup(DaemonFixture *fixture, gint per_parent)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-ephemeral-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml", NULL);
    yaml = g_strdup_printf(
        "daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
        "  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "orchestration:\n  ephemeral_max_per_parent: %d\n"
        "teams:\n  - id: core\n"
        "agents:\n"
        "  - id: chief\n    chief_of_staff: true\n    runtime:\n      autostart: false\n"
        "  - id: kudu\n    team: core\n    team_role: member\n"
        "    tools:\n      ephemeral: true\n    runtime:\n      autostart: false\n"
        "  - id: oryx\n    team: core\n    runtime:\n      autostart: false\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir, per_parent);
    g_assert_true(g_file_set_contents(fixture->config_path, yaml, -1, &error));
    daemon_start(fixture);
}

static void
daemon_teardown(DaemonFixture *fixture)
{
    daemon_stop(fixture);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
    g_free(fixture->config_path);
}

static JsonObject *
args_of(const gchar *json)
{
    g_autoptr(JsonParser) parser = json_parser_new();

    g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
    return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

static gchar *
spawn(DaemonFixture *fixture, const gchar *parent, const gchar *json, GError **error)
{
    g_autoptr(JsonObject) args = args_of(json);

    return clawt_daemon_ephemeral_spawn(fixture->daemon, parent, args, error);
}

static ClawtAgentConfig *
agent(DaemonFixture *fixture, const gchar *id)
{
    return clawt_config_get_agent(fixture->daemon->config, id);
}

/* The chief may by default; a member only when told; a helper never. */
static void
test_permission_defaults(void)
{
    DaemonFixture fixture = { 0 };
    ClawtMcpTools *tools;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *child = NULL;

    daemon_setup(&fixture, 5);
    tools = fixture.daemon->mcp_tools;

    g_assert_true(clawt_agent_config_get_boolean(agent(&fixture, "chief"),
                                                 "tools.ephemeral"));
    g_assert_false(clawt_agent_config_get_boolean(agent(&fixture, "oryx"),
                                                  "tools.ephemeral"));
    g_assert_true(clawt_mcp_tools_is_permitted(tools, "chief", "clawtilla_ephemeral_spawn"));
    g_assert_true(clawt_mcp_tools_is_permitted(tools, "kudu", "clawtilla_ephemeral_spawn"));
    g_assert_false(clawt_mcp_tools_is_permitted(tools, "oryx", "clawtilla_ephemeral_spawn"));
    g_assert_false(clawt_mcp_tools_is_permitted(tools, "oryx", "clawtilla_ephemeral_reap"));

    child = spawn(&fixture, "chief", "{\"description\":\"reads the logs\"}", &error);
    g_assert_no_error(error);
    g_assert_false(clawt_mcp_tools_is_permitted(tools, child, "clawtilla_ephemeral_spawn"));
    g_assert_false(clawt_agent_config_get_boolean(agent(&fixture, child),
                                                  "tools.manage_fleet"));

    /* oryx was never allowed and has none; still refused at the call */
    g_clear_pointer(&child, g_free);
    child = spawn(&fixture, "oryx", "{\"description\":\"x\"}", &error);
    g_assert_null(child);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);

    daemon_teardown(&fixture);
}

/* A spawn makes the team, records the parent, and is listed as such. */
static void
test_spawn_records_and_lists(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *child = NULL;
    g_autofree gchar *team = NULL;
    g_autofree gchar *store = NULL;
    g_autofree gchar *saved = NULL;
    g_autofree gchar *file = NULL;
    ClawtAgentConfig *config;

    daemon_setup(&fixture, 5);
    child = spawn(&fixture, "kudu",
                  "{\"description\":\"sorts the bug list\",\"name\":\"wobbly-yak\"}",
                  &error);
    g_assert_no_error(error);
    g_assert_cmpstr(child, ==, "wobbly-yak");

    config = agent(&fixture, child);
    g_assert_nonnull(config);
    g_assert_true(clawt_agent_config_is_ephemeral(config));
    g_assert_cmpstr(clawt_agent_config_get_ephemeral_string(config, "parent"), ==, "kudu");
    team = clawt_team_find_by_tag(fixture.daemon->config, CLAWT_TEAM_TAG_EPHEMERAL);
    g_assert_nonnull(team);
    g_assert_cmpstr(clawt_agent_config_get_string(config, "team"), ==, team);
    g_assert_cmpstr(clawt_agent_config_get_string(config, "description"), ==,
                    "sorts the bug list");

    /* a member parent may assign to its own helper, and only it */
    g_assert_true(clawt_team_may_assign(agent(&fixture, "kudu"), config, NULL));
    g_assert_false(clawt_team_may_assign(agent(&fixture, "oryx"), config, NULL));

    /* the store holds it; clawtilla.yaml never does */
    store = g_build_filename(fixture.dir, "state", "ephemeral.yaml", NULL);
    g_assert_true(g_file_get_contents(store, &saved, NULL, NULL));
    g_assert_nonnull(strstr(saved, "wobbly-yak"));
    g_assert_true(g_file_get_contents(fixture.config_path, &file, NULL, NULL));
    g_assert_null(strstr(file, "wobbly-yak"));
    g_assert_nonnull(strstr(file, CLAWT_TEAM_TAG_EPHEMERAL));

    /* the ephemeral team has no lead and is not warned about for it */
    {
        g_auto(GStrv) warnings = NULL;
        gsize i;

        clawt_team_validate_fleet(fixture.daemon->config, &warnings);
        for (i = 0; warnings != NULL && warnings[i] != NULL; i++)
            g_assert_null(strstr(warnings[i], team));
    }

    /* a name already in use is refused, not suffixed */
    g_clear_pointer(&child, g_free);
    child = spawn(&fixture, "chief", "{\"description\":\"x\",\"name\":\"oryx\"}", &error);
    g_assert_null(child);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);

    daemon_teardown(&fixture);
}

/* Never more computer than the parent, and the per-parent limit holds. */
static void
test_limits_and_computer(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;
    g_autofree gchar *host = NULL;

    daemon_setup(&fixture, 1);

    host = spawn(&fixture, "chief", "{\"description\":\"x\",\"computer\":\"host\"}", &error);
    g_assert_null(host);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    first = spawn(&fixture, "chief", "{\"description\":\"one\"}", &error);
    g_assert_no_error(error);
    second = spawn(&fixture, "chief", "{\"description\":\"two\"}", &error);
    g_assert_null(second);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_LOOP_LIMIT);
    /* the refusal names the helper holding the room */
    g_assert_nonnull(strstr(error->message, first));

    daemon_teardown(&fixture);
}

/* Only the parent or the operator; open work needs force. */
static void
test_reap_rules(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *child = NULL;
    g_autofree gchar *workspace = NULL;
    ClawtTask *task;

    daemon_setup(&fixture, 5);
    child = spawn(&fixture, "kudu", "{\"description\":\"x\"}", &error);
    g_assert_no_error(error);
    workspace = clawt_agent_config_get_workspace(agent(&fixture, child));
    g_assert_true(g_file_test(workspace, G_FILE_TEST_IS_DIR));

    g_assert_false(clawt_daemon_ephemeral_reap(fixture.daemon, "chief", child,
                                               FALSE, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    task = clawt_task_manager_create(fixture.daemon->tasks, "kudu", child,
                                     "do the thing", NULL, &error);
    g_assert_no_error(error);
    g_assert_false(clawt_daemon_ephemeral_reap(fixture.daemon, "kudu", child,
                                               FALSE, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE);
    g_assert_nonnull(strstr(error->message, clawt_task_get_id(task)));
    g_clear_error(&error);

    g_assert_true(clawt_daemon_ephemeral_reap(fixture.daemon, "kudu", child,
                                              TRUE, &error));
    g_assert_no_error(error);
    g_assert_null(agent(&fixture, child));
    g_assert_false(g_file_test(workspace, G_FILE_TEST_EXISTS));

    daemon_teardown(&fixture);
}

/* A reload and a restart both keep it; removing the parent hands it on. */
static void
test_persists_and_reparents(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *child = NULL;
    g_autoptr(JsonNode) frame = NULL;
    g_autoptr(JsonNode) reply = NULL;

    daemon_setup(&fixture, 5);
    child = spawn(&fixture, "kudu", "{\"description\":\"x\"}", &error);
    g_assert_no_error(error);

    g_assert_true(clawt_daemon_reload(fixture.daemon, &error));
    g_assert_no_error(error);
    g_assert_nonnull(agent(&fixture, child));
    g_assert_true(clawt_agent_config_is_ephemeral(agent(&fixture, child)));

    daemon_stop(&fixture);
    daemon_start(&fixture);
    g_assert_nonnull(agent(&fixture, child));
    g_assert_nonnull(clawt_agent_manager_get(fixture.daemon->agents, child));

    frame = clawt_ipc_request_new("agent.remove", "t1");
    {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, "kudu");
        json_builder_end_object(builder);
        clawt_ipc_frame_set_payload(frame, json_builder_get_root(builder));
    }
    reply = clawt_daemon_handle_request(fixture.daemon, frame);
    g_assert_false(clawt_ipc_frame_is_error(reply));
    g_assert_cmpstr(clawt_agent_config_get_ephemeral_string(agent(&fixture, child),
                                                            "parent"), ==, "chief");

    daemon_teardown(&fixture);
}

/*
 * Finished and idle earns one reminder after the configured hours, and
 * new work clears the record so the next finish is reminded afresh.
 */
static void
test_reminder_once(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *child = NULL;
    ClawtAgentConfig *config;
    ClawtTask *task;
    gint64 now = 1800000000;

    daemon_setup(&fixture, 5);
    child = spawn(&fixture, "kudu", "{\"description\":\"x\"}", &error);
    g_assert_no_error(error);
    config = agent(&fixture, child);

    /* no work yet is not "done" */
    clawt_daemon_ephemeral_sweep(fixture.daemon, now);
    g_assert_null(clawt_agent_config_get_ephemeral_string(config, "done_at"));

    task = clawt_task_manager_create(fixture.daemon->tasks, "kudu", child,
                                     "do it", NULL, &error);
    g_assert_no_error(error);
    g_assert_true(clawt_task_manager_start(fixture.daemon->tasks,
                                           clawt_task_get_id(task)));
    g_assert_true(clawt_task_manager_complete(fixture.daemon->tasks,
                                              clawt_task_get_id(task), "done"));

    clawt_daemon_ephemeral_sweep(fixture.daemon, now);
    g_assert_nonnull(clawt_agent_config_get_ephemeral_string(config, "done_at"));
    g_assert_null(clawt_agent_config_get_ephemeral_string(config, "reminded_at"));

    /* not yet: an hour short of the default 24 */
    clawt_daemon_ephemeral_sweep(fixture.daemon, now + 23 * 3600);
    g_assert_null(clawt_agent_config_get_ephemeral_string(config, "reminded_at"));

    clawt_daemon_ephemeral_sweep(fixture.daemon, now + 25 * 3600);
    g_assert_nonnull(clawt_agent_config_get_ephemeral_string(config, "reminded_at"));

    /* new work clears both, so the next finish is reminded about again */
    task = clawt_task_manager_create(fixture.daemon->tasks, "kudu", child,
                                     "and this", NULL, &error);
    g_assert_no_error(error);
    clawt_daemon_ephemeral_sweep(fixture.daemon, now + 26 * 3600);
    g_assert_null(clawt_agent_config_get_ephemeral_string(config, "done_at"));
    g_assert_null(clawt_agent_config_get_ephemeral_string(config, "reminded_at"));

    daemon_teardown(&fixture);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/ephemeral/tag-syntax", test_tag_syntax);
    g_test_add_func("/ephemeral/well-known-table", test_well_known_table);
    g_test_add_func("/ephemeral/find-by-tag-and-warnings", test_find_by_tag_and_warnings);
    g_test_add_func("/ephemeral/names", test_names);
    g_test_add_func("/ephemeral/permission-defaults", test_permission_defaults);
    g_test_add_func("/ephemeral/spawn-records-and-lists", test_spawn_records_and_lists);
    g_test_add_func("/ephemeral/limits-and-computer", test_limits_and_computer);
    g_test_add_func("/ephemeral/reap-rules", test_reap_rules);
    g_test_add_func("/ephemeral/persists-and-reparents", test_persists_and_reparents);
    g_test_add_func("/ephemeral/reminder-once", test_reminder_once);

    return g_test_run();
}
