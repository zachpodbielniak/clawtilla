/*
 * test-interrupt.c - Ending a turn without ending the agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * An agent's turn is carried out by an AI CLI its libreclaw spawned, and
 * that CLI spawns whatever the model asked for.  None of it is
 * clawtilla's own child, so none of it can be reached through
 * #GSubprocess -- the daemon walks /proc instead, and everything here is
 * about that walk being right.
 *
 * The test that matters most is the last one: a real three-deep process
 * tree under a real #ClawtProcessRuntime, interrupted, with the agent
 * asserted still alive afterwards.  Killing the whole tree including the
 * agent would pass every other test in this file and be exactly the
 * button people already had.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include <errno.h>
#include <signal.h>
#include <string.h>

#include "clawt-test-util.h"

/* ── Reading the process tree ────────────────────────────────────── */

static gboolean
array_has(GArray *pids, GPid wanted)
{
    guint i;

    for (i = 0; i < pids->len; i++) {
        if (g_array_index(pids, GPid, i) == wanted)
            return TRUE;
    }

    return FALSE;
}

static gint
index_of(GArray *pids, GPid wanted)
{
    guint i;

    for (i = 0; i < pids->len; i++) {
        if (g_array_index(pids, GPid, i) == wanted)
            return (gint)i;
    }

    return -1;
}

static GPid
pid_of(GSubprocess *process)
{
    const gchar *identifier = g_subprocess_get_identifier(process);

    return (identifier != NULL)
           ? (GPid)g_ascii_strtoll(identifier, NULL, 10) : 0;
}

/* Whether the kernel still has this process, zombie or not. */
static gboolean
still_there(GPid pid)
{
    return pid > 0 && kill(pid, 0) == 0;
}

static gboolean
gone(GPid pid)
{
    guint waited;

    /*
     * Polled rather than asserted immediately.  A signal is delivered
     * asynchronously, so a kill that worked perfectly is not observable
     * on the very next instruction -- a test that checked once would
     * fail on a loaded machine and pass everywhere else, which is worse
     * than no test.
     */
    for (waited = 0; waited < 3000; waited++) {
        if (!still_there(pid))
            return TRUE;

        g_usleep(1000);
    }

    return !still_there(pid);
}

/*
 * A shell holding a `sleep`, so there is a real parent and a real child.
 */
static GSubprocess *
spawn_a_little_tree(void)
{
    g_autoptr(GError) error = NULL;
    GSubprocess *process;

    process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                               G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                               &error, "/bin/sh", "-c",
                               "sleep 30 & wait", NULL);

    g_assert_no_error(error);
    g_assert_nonnull(process);

    return process;
}

/*
 * A process with nothing under it has no descendants, and the answer is
 * an empty array rather than NULL.
 *
 * A NULL return would make every caller check for it before iterating,
 * and the one that forgot would crash on the ordinary case: an agent
 * sitting between turns.
 */
static void
test_a_leaf_has_no_descendants(void)
{
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GArray) found = NULL;

    process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE, &error,
                               "/bin/sleep", "30", NULL);
    g_assert_no_error(error);

    found = clawt_process_descendants(pid_of(process));

    g_assert_nonnull(found);
    g_assert_cmpuint(found->len, ==, 0);

    g_subprocess_force_exit(process);
    g_subprocess_wait(process, NULL, NULL);
}

/*
 * Everything below the root, and the root itself never in it.
 *
 * The root is the agent.  Including it would make interrupting identical
 * to stopping, which is the button people already had and the reason
 * this one exists.
 */
static void
test_the_root_is_never_a_descendant_of_itself(void)
{
    g_autoptr(GSubprocess) process = spawn_a_little_tree();
    GPid root = pid_of(process);
    g_autoptr(GArray) found = NULL;
    guint waited;

    /* The shell needs a moment to fork its child. */
    for (waited = 0; waited < 2000; waited++) {
        g_autoptr(GArray) probe = clawt_process_descendants(root);

        if (probe->len > 0)
            break;

        g_usleep(1000);
    }

    found = clawt_process_descendants(root);

    g_assert_cmpuint(found->len, >, 0);
    g_assert_false(array_has(found, root));

    g_subprocess_force_exit(process);
    g_subprocess_wait(process, NULL, NULL);
}

/*
 * Deepest first.
 *
 * Killing a parent before its children hands those children to init,
 * where they are no longer reachable from the root at all -- so an
 * agent's stop would leave the compiler it had launched running for
 * ever, and nothing would ever find it again.
 */
static void
test_descendants_come_deepest_first(void)
{
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GArray) found = NULL;
    GPid root;
    guint waited;

    /*
     * Three levels, which is two descendants: sh -> sh -> sleep.  The
     * middle one is what makes the ordering assertion below mean
     * anything -- with a single level every order is deepest-first.
     */
    process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                               G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                               &error, "/bin/sh", "-c",
                               "sh -c 'sleep 30 & wait' & wait", NULL);
    g_assert_no_error(error);

    root = pid_of(process);

    for (waited = 0; waited < 3000; waited++) {
        g_autoptr(GArray) probe = clawt_process_descendants(root);

        if (probe->len >= 2)
            break;

        g_usleep(1000);
    }

    found = clawt_process_descendants(root);
    g_assert_cmpuint(found->len, >=, 2);

    /*
     * Asserted as a relationship rather than as a fixed order: which
     * pid is which is up to the kernel, but every one of them must come
     * before its own parent.
     */
    {
        guint i;

        for (i = 0; i < found->len; i++) {
            GPid pid = g_array_index(found, GPid, i);
            GPid parent = clawt_process_parent_of(pid);

            if (parent == root || !array_has(found, parent))
                continue;

            g_assert_cmpint(index_of(found, pid), <,
                            index_of(found, parent));
        }
    }

    g_subprocess_force_exit(process);
    g_subprocess_wait(process, NULL, NULL);
}

/*
 * The ancestry walk, both ways, and the answers that must not be yes.
 *
 * This is what is asked again immediately before each signal, so a
 * false positive here is a kill sent to somebody else's process.
 */
static void
test_ancestry_is_checked_both_ways(void)
{
    g_autoptr(GSubprocess) process = spawn_a_little_tree();
    GPid root = pid_of(process);
    GPid child = 0;
    guint waited;

    for (waited = 0; waited < 2000 && child == 0; waited++) {
        g_autoptr(GArray) probe = clawt_process_descendants(root);

        if (probe->len > 0)
            child = g_array_index(probe, GPid, 0);
        else
            g_usleep(1000);
    }

    g_assert_cmpint(child, >, 0);
    g_assert_true(clawt_process_is_descendant_of(child, root));

    /* Not the other way round, and not of itself. */
    g_assert_false(clawt_process_is_descendant_of(root, child));
    g_assert_false(clawt_process_is_descendant_of(root, root));

    /*
     * And this test's own process is not below the tree it made, which
     * is the assertion that would catch a walk that answered TRUE
     * whenever it ran out of parents to look at.
     */
    g_assert_false(clawt_process_is_descendant_of(getpid(), root));

    /* init and nonsense: never, and never a crash. */
    g_assert_false(clawt_process_is_descendant_of(1, root));
    g_assert_false(clawt_process_is_descendant_of(0, root));
    g_assert_false(clawt_process_is_descendant_of(child, 0));
    g_assert_false(clawt_process_is_descendant_of(-1, -1));

    g_subprocess_force_exit(process);
    g_subprocess_wait(process, NULL, NULL);
}

/*
 * Asking about init returns nothing rather than every process on the
 * machine.
 *
 * A root of 1 would otherwise walk the whole system, and the caller
 * above it is a function whose next step is to send signals.
 */
static void
test_init_and_nonsense_have_no_descendants(void)
{
    g_autoptr(GArray) from_init = clawt_process_descendants(1);
    g_autoptr(GArray) from_zero = clawt_process_descendants(0);
    g_autoptr(GArray) from_negative = clawt_process_descendants(-1);

    g_assert_cmpuint(from_init->len, ==, 0);
    g_assert_cmpuint(from_zero->len, ==, 0);
    g_assert_cmpuint(from_negative->len, ==, 0);

    g_assert_cmpint(clawt_process_parent_of(0), ==, 0);
    g_assert_cmpint(clawt_process_parent_of(-1), ==, 0);
}

/* ── The runtime ─────────────────────────────────────────────────── */

typedef struct {
    gchar       *dir;
    ClawtConfig *config;
} Fixture;

static void
fixture_setup(Fixture *fixture, const gchar *yaml)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *full = NULL;

    fixture->dir = g_dir_make_tmp("clawt-interrupt-XXXXXX", NULL);

    /*
     * workspace_root as well as state_dir and socket: it falls back to
     * ~/.clawtilla/agents, so a fixture that leaves it out scaffolds
     * into the developer's real fleet.
     */
    full = g_strdup_printf("daemon:\n"
                           "  state_dir: \"%s\"\n"
                           "  socket: \"%s/daemon.sock\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n%s",
                           fixture->dir, fixture->dir, fixture->dir, yaml);

    fixture->config = clawt_config_load_from_string(full, &error);
    g_assert_no_error(error);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->config);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

/*
 * An embedded agent refuses, and names the type that refused.
 *
 * A missing vfunc that answered TRUE would report a turn stopped while
 * the model carried on working, and the operator -- having pressed the
 * button and been told it worked -- would read the next message to
 * arrive as the agent ignoring them.
 */
static void
test_an_embedded_runtime_refuses_and_says_which(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtEmbeddedRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent_config;
    guint killed = 99;

    fixture_setup(&fixture, "agents:\n  - id: inside\n");
    agent_config = clawt_config_get_agent(fixture.config, "inside");

    runtime = clawt_embedded_runtime_new(agent_config, "/dev/null", NULL);

    g_assert_false(clawt_agent_runtime_interrupt(
                       CLAWT_AGENT_RUNTIME(runtime), &killed, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);

    /* The type is in the message, so the reader knows which agent it is. */
    g_assert_nonnull(strstr(error->message, "ClawtEmbeddedRuntime"));

    /* And the count is cleared rather than left holding the caller's. */
    g_assert_cmpuint(killed, ==, 0);

    /*
     * It also declares no `interrupt`, so a client never offers the
     * button in the first place -- the refusal is for the one that does
     * not ask.
     */
    g_assert_cmpuint(clawt_agent_runtime_get_caps(CLAWT_AGENT_RUNTIME(runtime))
                     & CLAWT_AGENT_CAPS_INTERRUPT, ==, 0);

    fixture_teardown(&fixture);
}

/* An agent that was never started has nothing in flight. */
static void
test_a_stopped_runtime_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    ClawtAgentConfig *agent_config;

    fixture_setup(&fixture, "agents:\n  - id: idle\n");
    agent_config = clawt_config_get_agent(fixture.config, "idle");

    runtime = clawt_process_runtime_new(agent_config, "/dev/null");

    g_assert_false(clawt_agent_runtime_interrupt(
                       CLAWT_AGENT_RUNTIME(runtime), NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE);

    /*
     * And it declares the capability regardless, because the capability
     * is about the runtime rather than about this moment: a client that
     * hid the button on a stopped agent and showed it on a running one
     * is doing the right thing with `busy`, not with `caps`.
     */
    g_assert_cmpuint(clawt_agent_runtime_get_caps(CLAWT_AGENT_RUNTIME(runtime))
                     & CLAWT_AGENT_CAPS_INTERRUPT, !=, 0);

    fixture_teardown(&fixture);
}

static gchar *
fixture_binary(const gchar *name)
{
    return g_build_filename(CLAWT_TEST_FIXTURES, name, NULL);
}

/*
 * The whole point, end to end.
 *
 * A real runtime, a real three-deep tree under it, interrupted -- and
 * the agent still alive afterwards. Every other test here would pass
 * against an implementation that killed the agent too, which is
 * `agent.stop` and is the button this one exists not to be.
 */
static void
test_interrupt_kills_the_tree_and_keeps_the_agent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *busy = fixture_binary("busy-libreclaw");
    g_autofree gchar *pidfile = NULL;
    g_autoptr(GHashTable) environment = NULL;
    ClawtAgentConfig *agent_config;
    GPid agent_pid;
    GPid grandchild = 0;
    guint killed = 0;
    guint waited;

    if (!g_file_test(busy, G_FILE_TEST_IS_EXECUTABLE)) {
        g_test_skip("the busy libreclaw fixture is not executable");
        return;
    }

    fixture_setup(&fixture, "agents:\n  - id: worker\n");
    agent_config = clawt_config_get_agent(fixture.config, "worker");
    pidfile = g_build_filename(fixture.dir, "grandchild.pid", NULL);

    runtime = clawt_process_runtime_new(agent_config, "/dev/null");
    clawt_process_runtime_set_binary(runtime, busy);
    clawt_agent_runtime_set_restart_policy(CLAWT_AGENT_RUNTIME(runtime),
                                           CLAWT_RESTART_NEVER, 1, 0);

    /*
     * Through the runtime's own environment rather than g_setenv(): the
     * child's environment is built from an allowlist, so a variable set
     * in this process never reaches it.
     */
    environment = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        g_free);
    g_hash_table_insert(environment, g_strdup("BUSY_LIBRECLAW_PIDFILE"),
                        g_strdup(pidfile));
    clawt_process_runtime_set_environment(runtime, environment);

    g_assert_true(clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime),
                                            &error));
    g_assert_no_error(error);

    agent_pid = clawt_agent_runtime_get_pid(CLAWT_AGENT_RUNTIME(runtime));
    g_assert_cmpint(agent_pid, >, 0);

    /* Wait for the tree to be genuinely three deep. */
    for (waited = 0; waited < 5000 && grandchild == 0; waited++) {
        g_autofree gchar *contents = NULL;

        g_main_context_iteration(NULL, FALSE);

        if (g_file_get_contents(pidfile, &contents, NULL, NULL))
            grandchild = (GPid)g_ascii_strtoll(contents, NULL, 10);

        if (grandchild == 0)
            g_usleep(1000);
    }

    g_assert_cmpint(grandchild, >, 0);
    g_assert_true(still_there(grandchild));
    g_assert_true(clawt_process_is_descendant_of(grandchild, agent_pid));

    g_assert_true(clawt_agent_runtime_interrupt(
                      CLAWT_AGENT_RUNTIME(runtime), &killed, &error));
    g_assert_no_error(error);

    /* It reported what it did, and it did it. */
    g_assert_cmpuint(killed, >, 0);
    g_assert_true(gone(grandchild));

    /*
     * And the agent is still up.  This is the assertion the whole
     * feature is: an interrupt that took the agent with it would be
     * agent.stop wearing a different name, and the operator would lose
     * the session, the mailbox and the link along with the runaway turn.
     */
    g_assert_true(still_there(agent_pid));
    g_assert_true(clawt_agent_runtime_is_alive(CLAWT_AGENT_RUNTIME(runtime)));

    /*
     * A second interrupt is not an error.  The tree is gone, so nothing
     * is signalled and the count says so -- pressing stop twice is a
     * person confirming what they wanted, not a mistake to report.
     */
    killed = 99;
    g_assert_true(clawt_agent_runtime_interrupt(
                      CLAWT_AGENT_RUNTIME(runtime), &killed, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(killed, ==, 0);

    clawt_agent_runtime_stop(CLAWT_AGENT_RUNTIME(runtime));
    fixture_teardown(&fixture);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/interrupt/a-leaf-has-none",
                    test_a_leaf_has_no_descendants);
    g_test_add_func("/interrupt/root-is-not-its-own-descendant",
                    test_the_root_is_never_a_descendant_of_itself);
    g_test_add_func("/interrupt/deepest-first",
                    test_descendants_come_deepest_first);
    g_test_add_func("/interrupt/ancestry-both-ways",
                    test_ancestry_is_checked_both_ways);
    g_test_add_func("/interrupt/init-and-nonsense",
                    test_init_and_nonsense_have_no_descendants);
    g_test_add_func("/interrupt/embedded-refuses-and-says-which",
                    test_an_embedded_runtime_refuses_and_says_which);
    g_test_add_func("/interrupt/a-stopped-runtime-is-refused",
                    test_a_stopped_runtime_is_refused);
    g_test_add_func("/interrupt/kills-the-tree-and-keeps-the-agent",
                    test_interrupt_kills_the_tree_and_keeps_the_agent);

    return g_test_run();
}
