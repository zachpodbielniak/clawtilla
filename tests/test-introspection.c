/*
 * test-introspection.c - What an agent is doing, and what is doing it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `running` is a fact about a process.  It is the same word for an agent
 * thinking for four minutes and an agent idle for four hours, and for a
 * long time it was the only word any headless surface had: the daemon
 * computed `busy` and `peer` for exactly this question, the two
 * graphical clients rendered them, and the CLI -- the one used over ssh,
 * from scripts, and by an agent introspecting its own fleet -- read
 * neither.
 *
 * The other half is which OS process is serving the agent.
 * clawt_agent_runtime_get_pid() was declared, documented and implemented
 * by both runtimes with no caller outside the test suite, so diagnosing
 * a stuck agent meant leaving clawtilla for pgrep and /proc and then
 * guessing which of N identical libreclaw children belonged to which
 * agent.
 *
 * The label is tested here rather than in either client because it is a
 * rule both clients apply, and this file is the whole of it -- no
 * window, no browser.
 */

#include <clawtilla.h>

#include "clawt-test-util.h"

/*
 * An idle agent gets no label at all.
 *
 * NULL rather than "idle" on purpose, and asserted rather than left to
 * each caller's taste: the sidebar draws a subtitle only when there is
 * something to say, the web client draws a badge only when there is one
 * to draw, and both would otherwise carry a word for nothing happening
 * on every row of a quiet fleet.  The CLI spells its own "idle" because
 * a missing line there reads as a field it does not know about.
 */
static void
test_idle_has_no_label(void)
{
    g_assert_null(clawt_agent_activity_label(FALSE, NULL));

    /* Not busy is not busy, whoever the last turn happened to be for. */
    g_assert_null(clawt_agent_activity_label(FALSE, "kudu"));
    g_assert_null(clawt_agent_activity_label(FALSE, "user"));
}

/*
 * Working, with and without somebody to be working for.
 */
static void
test_working_says_so(void)
{
    g_autofree gchar *alone = clawt_agent_activity_label(TRUE, NULL);
    g_autofree gchar *with_peer = clawt_agent_activity_label(TRUE, "kudu");

    g_assert_cmpstr(alone, ==, "working");
    g_assert_cmpstr(with_peer, ==, "working for kudu");
}

/*
 * The operator is not a peer, and arrives spelled out.
 *
 * "user" is a real sender id on the wire, not an absence -- the peer
 * field is set from whoever the turn is for, and that is who it is when
 * a person is talking to the agent.  A caller passing it through renders
 * "working for user" at the user.
 *
 * The empty string too: a client that reads a missing member with a ""
 * fallback would otherwise produce "working for ", which looks like a
 * name that failed to load.
 */
static void
test_the_operator_is_not_a_peer(void)
{
    g_autofree gchar *as_user = clawt_agent_activity_label(TRUE, "user");
    g_autofree gchar *as_empty = clawt_agent_activity_label(TRUE, "");

    g_assert_cmpstr(as_user, ==, "working");
    g_assert_cmpstr(as_empty, ==, "working");
}

/*
 * A runtime that has never started has no uptime.
 *
 * Worth its own assertion because the stamp behind it is *not* cleared
 * when a child dies -- the restart streak needs the last start's time to
 * tell a crash loop from a process that ran for a week -- so the naive
 * reading of that field reports the uptime of a corpse, climbing for
 * ever, on an agent that is not running.
 */
static void
test_a_stopped_runtime_has_no_uptime(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtAgentRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;

    config = clawt_config_load_from_string("agents:\n  - id: scribe\n",
                                           &error);
    g_assert_no_error(error);

    runtime = CLAWT_AGENT_RUNTIME(clawt_process_runtime_new(
        clawt_config_get_agent(config, "scribe"), "/dev/null"));

    g_assert_false(clawt_agent_runtime_is_alive(runtime));
    g_assert_cmpint(clawt_agent_runtime_get_uptime_seconds(runtime), ==, 0);
    g_assert_cmpint(clawt_agent_runtime_get_pid(runtime), ==, 0);
}

/*
 * And it has never replaced a child.
 *
 * Zero is reported rather than withheld: "this process has never been
 * swapped" is the answer that makes a pid worth trusting, and a runtime
 * that respawns in place is exactly how an agent came to be reported
 * stopped while it was alive and answering nothing.
 */
static void
test_a_fresh_runtime_has_no_restarts(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtAgentRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;

    config = clawt_config_load_from_string("agents:\n  - id: scribe\n",
                                           &error);
    g_assert_no_error(error);

    runtime = CLAWT_AGENT_RUNTIME(clawt_process_runtime_new(
        clawt_config_get_agent(config, "scribe"), "/dev/null"));

    g_assert_cmpuint(clawt_agent_runtime_get_restarts(runtime), ==, 0);
}

/*
 * A start that fails does not count as a restart.
 *
 * The counter exists to say "the process under this agent was replaced",
 * and a spawn that never produced a process replaced nothing.  Counting
 * it would make a misconfigured agent look like a flapping one, which
 * sends the reader to the wrong layer entirely.
 */
static void
test_a_failed_start_is_not_a_restart(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtAgentRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;

    config = clawt_config_load_from_string("agents:\n  - id: scribe\n",
                                           &error);
    g_assert_no_error(error);

    runtime = CLAWT_AGENT_RUNTIME(clawt_process_runtime_new(
        clawt_config_get_agent(config, "scribe"), "/dev/null"));

    /*
     * Named rather than left to the resolver, so this test does not
     * depend on whether a libreclaw is installed on the build machine.
     */
    clawt_process_runtime_set_binary(CLAWT_PROCESS_RUNTIME(runtime),
                                     "/nonexistent/no-such-binary");

    g_assert_false(clawt_agent_runtime_start(runtime, &error));
    g_assert_nonnull(error);
    g_assert_cmpuint(clawt_agent_runtime_get_restarts(runtime), ==, 0);

    /* And a failed start leaves no uptime behind either. */
    g_assert_cmpint(clawt_agent_runtime_get_uptime_seconds(runtime), ==, 0);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/introspection/idle-has-no-label",
                    test_idle_has_no_label);
    g_test_add_func("/introspection/working-says-so",
                    test_working_says_so);
    g_test_add_func("/introspection/the-operator-is-not-a-peer",
                    test_the_operator_is_not_a_peer);
    g_test_add_func("/introspection/stopped-runtime-has-no-uptime",
                    test_a_stopped_runtime_has_no_uptime);
    g_test_add_func("/introspection/fresh-runtime-has-no-restarts",
                    test_a_fresh_runtime_has_no_restarts);
    g_test_add_func("/introspection/failed-start-is-not-a-restart",
                    test_a_failed_start_is_not_a_restart);

    return g_test_run();
}
