/*
 * test-routine-read-only.c - Read-only and proactive routines
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A read-only run is a room: every turn there is offered only the read
 * tools and refused anything else, by the tools and by policy.check alike.
 * It runs only where the CLI's own tools can be refused at all -- refused
 * at run time and warned about at load otherwise -- and it brings the
 * Claude hook with it even to an agent that has no action rules. A
 * proactive routine with no instructions is told what to look for.
 *
 * The daemon half includes core/clawt-daemon-private.h, as test-policy
 * does.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    gchar        *dir;
    GMainContext *context;
    ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_setup(DaemonFixture *fixture, const gchar *extra)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-ro-XXXXXX", NULL);
    path = g_build_filename(fixture->dir, "config.yaml", NULL);
    yaml = g_strdup_printf("daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
                           "  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n%s",
                           fixture->dir, fixture->dir, fixture->dir, fixture->dir, extra);
    g_assert_true(g_file_set_contents(path, yaml, -1, &error));
    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(path, fixture->context);
    g_assert_true(clawt_daemon_start(fixture->daemon, NULL));
}

static void
daemon_teardown(DaemonFixture *fixture)
{
    clawt_daemon_stop(fixture->daemon);
    g_clear_object(&fixture->daemon);
    while (g_main_context_iteration(fixture->context, FALSE))
        ;
    g_clear_pointer(&fixture->context, g_main_context_unref);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

static JsonNode *
request(DaemonFixture *fixture, const gchar *kind, const gchar *payload_json)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");
    g_autoptr(JsonParser) parser = json_parser_new();

    g_assert_true(json_parser_load_from_data(parser, payload_json, -1, NULL));
    clawt_ipc_frame_set_payload(frame, json_node_copy(json_parser_get_root(parser)));
    return clawt_daemon_handle_request(fixture->daemon, frame);
}

static const gchar AGENTS[] =
    "agents:\n"
    "  - id: watcher\n"
    "    model:\n      provider: claude-code\n"
    "    runtime:\n      autostart: false\n"
    "  - id: codexer\n"
    "    model:\n      provider: codex-cli\n"
    "    runtime:\n      autostart: false\n"
    "routines:\n"
    "  - id: scan\n    agent: watcher\n    proactive: true\n    schedule: daily\n    at: \"03:00\"\n"
    "  - id: codex-scan\n    agent: codexer\n    read_only: true\n    instructions: look\n"
    "    schedule: daily\n    at: \"03:00\"\n";

/* The run gets its own room, marked read-only, and is told so. */
static void
test_run_marks_its_room(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) tasks = NULL;
    ClawtTask *task;
    g_autoptr(JsonNode) listing = NULL;
    g_autofree gchar *text = NULL;

    daemon_setup(&fixture, AGENTS);
    reply = request(&fixture, "routine.run", "{\"id\":\"scan\"}");
    g_assert_nonnull(reply);

    tasks = clawt_task_manager_list(fixture.daemon->tasks, "watcher", TRUE);
    g_assert_cmpuint(tasks->len, ==, 1);
    task = g_ptr_array_index(tasks, 0);

    /* Proactive with no instructions: told what to look for, and not to act. */
    g_assert_nonnull(strstr(clawt_task_get_prompt(task), "Do not act on any of it"));
    g_assert_nonnull(strstr(clawt_task_get_prompt(task), "This run is read-only"));

    /* The room the run went to is read-only, and lists only the read tools. */
    {
        g_autoptr(GPtrArray) rooms = clawt_room_manager_list(fixture.daemon->rooms);
        const gchar *room_id = NULL;
        guint i;

        for (i = 0; i < rooms->len; i++) {
            const gchar *id = clawt_room_get_id(g_ptr_array_index(rooms, i));

            if (g_str_has_prefix(id, "routine:"))
                room_id = id;
        }
        g_assert_nonnull(room_id);
        g_assert_true(clawt_mcp_tools_room_is_read_only(fixture.daemon->mcp_tools, room_id));

        listing = clawt_mcp_tools_list_in_room(fixture.daemon->mcp_tools, "watcher", room_id);
        text = json_to_string(listing, FALSE);
        g_assert_nonnull(strstr(text, "\"clawtilla_ask_decision\""));
        g_assert_null(strstr(text, "\"clawtilla_message_agent\""));
        g_assert_null(strstr(text, "\"clawtilla_memory_add\""));
        g_clear_pointer(&text, g_free);
        g_clear_pointer(&listing, json_node_unref);

        /* The same agent's other conversations keep every tool. */
        listing = clawt_mcp_tools_list_in_room(fixture.daemon->mcp_tools, "watcher", "elsewhere");
        text = json_to_string(listing, FALSE);
        g_assert_nonnull(strstr(text, "\"clawtilla_memory_add\""));

        /* And the hook's check says no to the CLI's own Write there. */
        {
            g_autofree gchar *payload = g_strdup_printf(
                "{\"agent\":\"watcher\",\"tool\":\"Write\",\"room\":\"%s\"}", room_id);
            g_autoptr(JsonNode) check = request(&fixture, "policy.check", payload);
            JsonObject *answer = clawt_ipc_frame_get_payload(check);

            g_assert_false(json_object_get_boolean_member(answer, "allowed"));
            g_assert_nonnull(strstr(json_object_get_string_member(answer, "reason"),
                                    "read-only routine"));
        }
    }

    daemon_teardown(&fixture);
}

/* On a CLI whose own tools cannot be refused it does not run, and says why. */
static void
test_refused_where_it_cannot_hold(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    GPtrArray *warnings;
    gboolean warned = FALSE;
    guint i;

    daemon_setup(&fixture, AGENTS);
    reply = request(&fixture, "routine.run", "{\"id\":\"codex-scan\"}");
    g_assert_true(clawt_ipc_frame_is_error(reply));

    warnings = clawt_config_get_warnings(fixture.daemon->config);
    for (i = 0; i < warnings->len; i++)
        if (strstr(g_ptr_array_index(warnings, i), "routine 'codex-scan' is read-only"))
            warned = TRUE;
    g_assert_true(warned);

    daemon_teardown(&fixture);
}

/* The hook comes with a read-only routine even to an agent with no rules. */
static void
test_hook_follows_read_only_routine(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = clawt_config_load_from_string(
        "agents:\n  - id: watcher\n  - id: plain\n"
        "routines:\n  - id: scan\n    agent: watcher\n    read_only: true\n", &error);

    g_assert_no_error(error);
    g_assert_true(clawt_policy_agent_needs_hook(clawt_config_get_agent(config, "watcher")));
    g_assert_false(clawt_policy_agent_needs_hook(clawt_config_get_agent(config, "plain")));
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /*
     * The fixture's fleet holds a routine the daemon warns about at load,
     * on purpose; the test asserts the warning is there. Criticals stay
     * fatal.
     */
    g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);

    g_test_add_func("/routine-read-only/run-marks-its-room", test_run_marks_its_room);
    g_test_add_func("/routine-read-only/refused-where-it-cannot-hold",
                    test_refused_where_it_cannot_hold);
    g_test_add_func("/routine-read-only/hook-follows-routine", test_hook_follows_read_only_routine);

    return g_test_run();
}
