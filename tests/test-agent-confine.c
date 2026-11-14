/*
 * test-agent-confine.c - An agent's own CLI in a sandbox
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * `runtime.confine: bwrap` read as data: the bwrap argv and the
 * environment clawt_process_runtime_prepare() would spawn, the refusals
 * at load, and the binds the builder grants.  Hermetic -- no bubblewrap
 * is run and none needs to be installed.  Whether bwrap is "installed" is
 * decided by PATH, which each test points at a directory holding a stub
 * called bwrap or at an empty one, so the answer does not depend on the
 * machine.  The one test that runs a real box is behind
 * CLAWT_TEST_INTEGRATION=1.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "clawt-test-util.h"

typedef struct {
    gchar *base;        /* canonical, so emitted paths compare equal */
    gchar *workspace;
    gchar *state;
    gchar *bin;         /* holds a stub bwrap and a stub interpreter */
    gchar *empty;       /* a PATH with nothing on it */
    gchar *tools;       /* outside every bind */
    gchar *saved_path;
} Fixture;

static void
write_executable(const gchar *path, const gchar *text)
{
    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    g_assert_cmpint(g_chmod(path, 0755), ==, 0);
}

static void
fixture_setup(Fixture *fixture)
{
    g_autofree gchar *made = g_dir_make_tmp("clawt-confine-XXXXXX", NULL);
    g_autofree gchar *stub = NULL;

    fixture->base = clawt_canonicalize_missing(made);
    fixture->workspace = g_build_filename(fixture->base, "workspace", NULL);
    fixture->state = g_build_filename(fixture->base, "state", NULL);
    fixture->bin = g_build_filename(fixture->base, "bin", NULL);
    fixture->empty = g_build_filename(fixture->base, "empty", NULL);
    fixture->tools = g_build_filename(fixture->base, "tools", NULL);

    g_assert_cmpint(g_mkdir_with_parents(fixture->workspace, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(fixture->state, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(fixture->bin, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(fixture->empty, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(fixture->tools, 0700), ==, 0);

    stub = g_build_filename(fixture->bin, "bwrap", NULL);
    write_executable(stub, "#!/bin/sh\nexit 0\n");

    fixture->saved_path = g_strdup(g_getenv("PATH"));
}

/* bubblewrap "installed": the stub is the only bwrap on PATH. */
static void
with_bwrap(Fixture *fixture)
{
    g_setenv("PATH", fixture->bin, TRUE);
}

static void
without_bwrap(Fixture *fixture)
{
    g_setenv("PATH", fixture->empty, TRUE);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->saved_path != NULL)
        g_setenv("PATH", fixture->saved_path, TRUE);
    else
        g_unsetenv("PATH");

    clawt_test_remove_tree(fixture->base);

    g_clear_pointer(&fixture->base, g_free);
    g_clear_pointer(&fixture->workspace, g_free);
    g_clear_pointer(&fixture->state, g_free);
    g_clear_pointer(&fixture->bin, g_free);
    g_clear_pointer(&fixture->empty, g_free);
    g_clear_pointer(&fixture->tools, g_free);
    g_clear_pointer(&fixture->saved_path, g_free);
}

/*
 * A one-agent config.  @agent_extra continues the agent's block and
 * @top_extra goes before `agents:`.  The workspace and the state
 * directory are pinned to the fixture so nothing reaches the real fleet.
 */
static ClawtConfig *
load(Fixture *fixture, const gchar *top_extra, const gchar *agent_extra)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s\"\n"
        "%s"
        "agents:\n"
        "  - id: boxed\n"
        "    workspace: \"%s\"\n"
        "%s",
        fixture->state, top_extra != NULL ? top_extra : "",
        fixture->workspace, agent_extra != NULL ? agent_extra : "");
    ClawtConfig *config = clawt_config_load_from_string(yaml, &error);

    g_assert_no_error(error);
    g_assert_nonnull(config);

    return config;
}

/* The index of "flag path" in @argv, or -1. */
static gint
find_pair(GStrv argv, const gchar *flag, const gchar *path)
{
    guint i;

    for (i = 0; argv[i] != NULL; i++) {
        if (g_strcmp0(argv[i], flag) == 0 && argv[i + 1] != NULL &&
            g_strcmp0(argv[i + 1], path) == 0)
            return (gint)i;
    }

    return -1;
}

static const gchar *
env_value(GStrv env, const gchar *name)
{
    gsize length = strlen(name);
    guint i;

    for (i = 0; env[i] != NULL; i++) {
        if (strncmp(env[i], name, length) == 0 && env[i][length] == '=')
            return env[i] + length + 1;
    }

    return NULL;
}

/* ── Asking for it ───────────────────────────────────────────────── */

/*
 * Off unless asked for, so an upgrade changes nothing about agents that
 * were already running -- and the fleet default turns it on for every
 * agent that says nothing, while one that says none keeps its answer.
 */
static void
test_off_by_default_and_fleet_default(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) plain = NULL;
    g_autoptr(ClawtConfig) fleet = NULL;
    g_autoptr(ClawtConfig) opted_out = NULL;

    fixture_setup(&fixture);
    with_bwrap(&fixture);

    plain = load(&fixture, NULL, NULL);
    g_assert_false(clawt_agent_confine_wanted(
        clawt_config_get_agent(plain, "boxed")));

    fleet = load(&fixture, "defaults:\n  confine: bwrap\n", NULL);
    g_assert_true(clawt_agent_confine_wanted(
        clawt_config_get_agent(fleet, "boxed")));
    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(fleet, "boxed")));

    opted_out = load(&fixture, "defaults:\n  confine: bwrap\n",
                     "    runtime:\n      confine: none\n");
    g_assert_false(clawt_agent_confine_wanted(
        clawt_config_get_agent(opted_out, "boxed")));

    fixture_teardown(&fixture);
}

/*
 * An embedded agent is the daemon; there is nothing to wrap.  Refused at
 * load as a shadow naming both keys -- and naming defaults.confine when
 * that is where the bwrap came from, since the agent's own block then
 * says nothing that would explain it.
 */
static void
test_embedded_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) own = NULL;
    g_autoptr(ClawtConfig) inherited = NULL;
    g_autoptr(ClawtConfig) exempt = NULL;
    ClawtAgentConfig *agent;

    fixture_setup(&fixture);
    with_bwrap(&fixture);

    own = load(&fixture, NULL,
               "    runtime:\n      type: embedded\n      confine: bwrap\n");
    agent = clawt_config_get_agent(own, "boxed");
    g_assert_true(clawt_agent_config_is_shadow(agent));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
                            "embedded"));
    g_assert_null(strstr(clawt_agent_config_get_shadow_reason(agent),
                         "defaults.confine"));

    inherited = load(&fixture, "defaults:\n  confine: bwrap\n",
                     "    runtime:\n      type: embedded\n");
    agent = clawt_config_get_agent(inherited, "boxed");
    g_assert_true(clawt_agent_config_is_shadow(agent));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
                            "defaults.confine"));

    /* The remedy the message names works. */
    exempt = load(&fixture, "defaults:\n  confine: bwrap\n",
                  "    runtime:\n      type: embedded\n      confine: none\n");
    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(exempt, "boxed")));

    fixture_teardown(&fixture);
}

/*
 * No bubblewrap is a shadow with the reason, never an agent that starts
 * unconfined -- and the same config is fine once bwrap is there.
 */
static void
test_missing_bwrap_is_a_shadow(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) config = NULL;
    ClawtAgentConfig *agent;

    fixture_setup(&fixture);
    without_bwrap(&fixture);

    config = load(&fixture, NULL, "    runtime:\n      confine: bwrap\n");
    agent = clawt_config_get_agent(config, "boxed");

    g_assert_false(clawt_agent_confine_available());
    g_assert_true(clawt_agent_config_is_shadow(agent));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
                            "bubblewrap"));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
                            "not run unconfined"));

    with_bwrap(&fixture);
    g_assert_true(clawt_agent_config_revalidate(agent));

    fixture_teardown(&fixture);
}

/* An enum value the type does not have is reported, not read as 0. */
static void
test_unknown_value_is_a_shadow(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) config = NULL;
    ClawtAgentConfig *agent;

    fixture_setup(&fixture);
    with_bwrap(&fixture);

    config = load(&fixture, NULL, "    runtime:\n      confine: firejail\n");
    agent = clawt_config_get_agent(config, "boxed");

    g_assert_true(clawt_agent_config_is_shadow(agent));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
                            "firejail"));

    fixture_teardown(&fixture);
}

/* ── What goes in the box ────────────────────────────────────────── */

/*
 * The builder grants each thing the agent needs, in the mode it needs
 * it, and the predicate agrees with every one of them -- they are the one
 * object, which is the point.
 */
static void
test_builder_grants(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) mounts =
        g_ptr_array_new_with_free_func((GDestroyNotify)clawt_mount_free);
    g_auto(GStrv) wrapped = NULL;
    g_autofree gchar *login = NULL;
    g_autofree gchar *shared = NULL;
    g_autofree gchar *reference = NULL;
    g_autofree gchar *link_socket = NULL;
    g_autofree gchar *ipc_socket = NULL;
    g_autofree gchar *server = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *interpreter = NULL;
    g_autofree gchar *extra = NULL;
    g_autofree gchar *mcp = NULL;
    g_autofree gchar *mcp_json = NULL;
    g_autofree gchar *linked = NULL;
    g_autofree gchar *top = NULL;
    g_autofree gchar *agent_block = NULL;
    g_autofree gchar *secret = NULL;
    g_autofree gchar *outside = NULL;
    const gchar *sockets[3];
    const gchar *argv[] = { "libreclaw", NULL };
    ClawtMount *mount;
    gint home_at;

    fixture_setup(&fixture);
    with_bwrap(&fixture);

    login = g_build_filename(fixture.base, "claude-login", NULL);
    shared = g_build_filename(fixture.base, "shared", NULL);
    reference = g_build_filename(fixture.base, "reference", NULL);
    extra = g_build_filename(fixture.base, "extra", NULL);
    outside = g_build_filename(fixture.base, "not-granted", NULL);
    g_assert_cmpint(g_mkdir(login, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(shared, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(reference, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(extra, 0700), ==, 0);
    g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);

    /*
     * Stand-ins for the sockets: the builder asks only whether the path
     * is there, and `make test` opens no sockets.
     */
    link_socket = g_build_filename(fixture.base, "agents.sock", NULL);
    ipc_socket = g_build_filename(fixture.base, "daemon.sock", NULL);
    g_assert_true(g_file_set_contents(link_socket, "", 0, NULL));
    g_assert_true(g_file_set_contents(ipc_socket, "", 0, NULL));

    /*
     * An MCP server in a build tree, written the way npm writes a CLI --
     * a script naming its interpreter through env -- with a token file
     * argument outside every other bind.
     */
    interpreter = g_build_filename(fixture.bin, "fakenode", NULL);
    write_executable(interpreter, "#!/bin/sh\nexit 0\n");
    server = g_build_filename(fixture.tools, "fake-mcp", NULL);
    write_executable(server, "#!/usr/bin/env fakenode\n");
    token = g_build_filename(fixture.tools, "token", NULL);
    g_assert_true(g_file_set_contents(token, "t", -1, NULL));

    /*
     * And a second server reached through a symlink on PATH, the way
     * ~/.local/bin/claude is: its target is bound, and its name has to
     * be recreated or nothing inside can call it by that name.
     */
    linked = g_build_filename(fixture.bin, "linked-mcp", NULL);
    g_assert_cmpint(symlink(server, linked), ==, 0);

    mcp = g_build_filename(fixture.workspace, ".mcp.json", NULL);
    mcp_json = g_strdup_printf(
        "{\"mcpServers\":{\"thing\":{\"command\":\"%s\","
        "\"args\":[\"--token-file\",\"%s\",\"relative\"]},"
        "\"other\":{\"command\":\"linked-mcp\"}}}",
        server, token);
    g_assert_true(g_file_set_contents(mcp, mcp_json, -1, NULL));

    secret = g_build_filename(fixture.workspace, "secret.key", NULL);
    g_assert_true(g_file_set_contents(secret, "k", -1, NULL));

    top = g_strdup_printf(
        "accounts:\n"
        "  - id: work\n"
        "    provider: claude-code\n"
        "    config_dir: \"%s\"\n", login);
    agent_block = g_strdup_printf(
        "    model:\n"
        "      provider: claude-code\n"
        "      account: work\n"
        "    runtime:\n"
        "      confine: bwrap\n"
        "      confine_paths: [\"%s\"]\n"
        "    computer:\n"
        "      type: host\n"
        "      host:\n"
        "        confirm_host_control: true\n"
        "        deny_paths: [\"%s\"]\n", extra, secret);
    config = load(&fixture, top, agent_block);
    g_assert_false(clawt_agent_config_is_shadow(
        clawt_config_get_agent(config, "boxed")));

    mount = clawt_mount_new(shared, "/mnt/shared");
    clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
    g_ptr_array_add(mounts, mount);
    mount = clawt_mount_new(reference, "/mnt/reference");
    clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RO);
    g_ptr_array_add(mounts, mount);

    sockets[0] = link_socket;
    sockets[1] = ipc_socket;
    sockets[2] = NULL;

    sandbox = clawt_agent_confine_build(clawt_config_get_agent(config,
                                                               "boxed"),
                                        fixture.state, mounts, sockets,
                                        NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(sandbox);
    g_assert_cmpint(clawt_sandbox_get_mode(sandbox), ==,
                    CLAWT_CONFINE_BWRAP);

    wrapped = clawt_sandbox_wrap_argv(sandbox, argv);

    /* Writable, and the working directory. */
    g_assert_cmpint(find_pair(wrapped, "--bind", fixture.workspace), >=, 0);
    g_assert_cmpint(find_pair(wrapped, "--chdir", fixture.workspace), >=, 0);
    g_assert_cmpint(find_pair(wrapped, "--bind", fixture.state), >=, 0);

    /* The login, writable: the CLI refreshes its tokens in place. */
    g_assert_cmpint(find_pair(wrapped, "--bind", login), >=, 0);

    /* Mounts in the mode they were declared in. */
    g_assert_cmpint(find_pair(wrapped, "--bind", shared), >=, 0);
    g_assert_cmpint(find_pair(wrapped, "--ro-bind", reference), >=, 0);
    g_assert_cmpint(find_pair(wrapped, "--bind", reference), ==, -1);

    /* Both sockets, and nothing else from the directory holding them. */
    g_assert_cmpint(find_pair(wrapped, "--bind", link_socket), >=, 0);
    g_assert_cmpint(find_pair(wrapped, "--bind", ipc_socket), >=, 0);
    g_assert_cmpint(find_pair(wrapped, "--bind", fixture.base), ==, -1);

    /* The .mcp.json program, its interpreter, and its file argument. */
    g_assert_cmpint(find_pair(wrapped, "--ro-bind", server), >=, 0);
    g_assert_cmpint(find_pair(wrapped, "--ro-bind", interpreter), >=, 0);
    g_assert_cmpint(find_pair(wrapped, "--ro-bind", token), >=, 0);

    /* What the operator named, read-only. */
    g_assert_cmpint(find_pair(wrapped, "--ro-bind", extra), >=, 0);

    /* The symlinked one: "--symlink <target> <name>", after its target. */
    {
        gint target_at = find_pair(wrapped, "--ro-bind", server);
        gint link_at = find_pair(wrapped, "--symlink", server);

        g_assert_cmpint(link_at, >, target_at);
        g_assert_cmpstr(wrapped[link_at + 2], ==, linked);
    }

    /* A host computer's deny reaches this box too. */
    g_assert_cmpint(find_pair(wrapped, "--ro-bind", "/dev/null"), >=, 0);

    /* And what nobody granted is not there at all. */
    {
        guint i;

        for (i = 0; wrapped[i] != NULL; i++)
            g_assert_cmpstr(wrapped[i], !=, outside);
    }

    /* The predicate is the same object, so it agrees. */
    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, login));
    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, token));
    g_assert_true(clawt_sandbox_path_is_allowed(sandbox, shared));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, outside));
    g_assert_false(clawt_sandbox_path_is_allowed(sandbox, secret));

    /*
     * The network is the CLI's, and $HOME is private: a tmpfs before
     * every bind, so the workspace under it is not covered.
     */
    {
        guint i;

        for (i = 0; wrapped[i] != NULL; i++)
            g_assert_cmpstr(wrapped[i], !=, "--unshare-net");
    }

    home_at = find_pair(wrapped, "--tmpfs", g_get_home_dir());

    /*
     * $HOME under `make test` is a path that does not exist and need not
     * canonicalise to itself, so the tmpfs is looked for by position
     * rather than by spelling when the plain one is not there.
     */
    if (home_at < 0) {
        g_autofree gchar *canonical =
            clawt_canonicalize_missing(g_get_home_dir());

        home_at = find_pair(wrapped, "--tmpfs", canonical);
    }

    g_assert_cmpint(home_at, >=, 0);
    g_assert_cmpint(find_pair(wrapped, "--bind", fixture.workspace), >,
                    home_at);

    fixture_teardown(&fixture);
}

/*
 * Without an account the CLI's own login is bound -- the directory and
 * the file beside it, for Claude Code.
 */
static void
test_default_login_paths(void)
{
    g_auto(GStrv) claude = clawt_agent_confine_default_login_paths(NULL);
    g_auto(GStrv) codex = clawt_agent_confine_default_login_paths("codex");
    g_autofree gchar *dir = g_build_filename(g_get_home_dir(), ".claude",
                                             NULL);
    g_autofree gchar *file = g_build_filename(g_get_home_dir(),
                                              ".claude.json", NULL);
    g_autofree gchar *codex_dir = g_build_filename(g_get_home_dir(),
                                                   ".codex", NULL);

    g_assert_cmpstr(claude[0], ==, dir);
    g_assert_cmpstr(claude[1], ==, file);
    g_assert_null(claude[2]);
    g_assert_cmpstr(codex[0], ==, codex_dir);
}

/* ── What the runtime spawns ─────────────────────────────────────── */

static ClawtProcessRuntime *
confined_runtime(Fixture *fixture, ClawtConfig *config, GHashTable *env,
                 gchar **out_binary, gchar **out_config_path)
{
    ClawtAgentConfig *agent = clawt_config_get_agent(config, "boxed");
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    ClawtProcessRuntime *runtime;

    *out_binary = g_build_filename(fixture->tools, "libreclaw", NULL);
    *out_config_path = g_build_filename(fixture->state, "config.yaml",
                                        NULL);
    write_executable(*out_binary, "#!/bin/sh\nexit 0\n");
    g_assert_true(g_file_set_contents(*out_config_path, "", 0, NULL));

    runtime = clawt_process_runtime_new(agent, *out_config_path);
    clawt_process_runtime_set_binary(runtime, *out_binary);

    if (env != NULL)
        clawt_process_runtime_set_environment(runtime, env);

    sandbox = clawt_agent_confine_build(agent, fixture->state, NULL, NULL,
                                        NULL, &error);
    g_assert_no_error(error);
    clawt_process_runtime_set_sandbox(runtime, sandbox);

    return runtime;
}

/*
 * The whole command as data: bwrap, the binds, then libreclaw with its
 * config -- and libreclaw itself bound, since in a checkout it lives
 * under $HOME where nothing else would bring it in.
 */
static void
test_prepare_wraps_libreclaw(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = NULL;
    g_auto(GStrv) env = NULL;
    g_autofree gchar *binary = NULL;
    g_autofree gchar *config_path = NULL;
    g_autofree gchar *bwrap = NULL;
    guint length;
    gint separator = -1;
    guint i;

    fixture_setup(&fixture);
    with_bwrap(&fixture);

    config = load(&fixture, NULL, "    runtime:\n      confine: bwrap\n");
    runtime = confined_runtime(&fixture, config, NULL, &binary,
                               &config_path);

    g_assert_true(clawt_process_runtime_prepare(runtime, &argv, &env,
                                                &error));
    g_assert_no_error(error);

    /* The resolved bwrap, not the bare name. */
    bwrap = g_find_program_in_path("bwrap");
    g_assert_cmpstr(argv[0], ==, bwrap);

    length = g_strv_length(argv);
    g_assert_cmpstr(argv[length - 3], ==, binary);
    g_assert_cmpstr(argv[length - 2], ==, "-c");
    g_assert_cmpstr(argv[length - 1], ==, config_path);

    for (i = 0; argv[i] != NULL; i++) {
        if (g_strcmp0(argv[i], "--") == 0) {
            separator = (gint)i;
            break;
        }
    }

    g_assert_cmpint(separator, ==, (gint)length - 4);
    g_assert_cmpint(find_pair(argv, "--ro-bind", binary), >=, 0);
    g_assert_cmpint(find_pair(argv, "--ro-bind", binary), <, separator);
    g_assert_cmpint(find_pair(argv, "--die-with-parent", "--unshare-pid"),
                    >=, 0);

    /* A second prepare does not bind libreclaw twice. */
    {
        g_auto(GStrv) again = NULL;
        g_auto(GStrv) again_env = NULL;
        guint binds = 0;

        g_assert_true(clawt_process_runtime_prepare(runtime, &again,
                                                    &again_env, NULL));

        for (i = 0; again[i] != NULL; i++) {
            if (g_strcmp0(again[i], "--ro-bind") == 0 &&
                g_strcmp0(again[i + 1], binary) == 0)
                binds++;
        }

        g_assert_cmpuint(binds, ==, 1);
    }

    fixture_teardown(&fixture);
}

/*
 * The agent's env: cannot point TMPDIR, HOME or XDG_* anywhere: inside
 * the box they are the box's.  Everything else it set still arrives.
 */
static void
test_prepare_owns_the_environment(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GHashTable) env_in = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    g_auto(GStrv) argv = NULL;
    g_auto(GStrv) env = NULL;
    g_autofree gchar *binary = NULL;
    g_autofree gchar *config_path = NULL;
    guint i;

    fixture_setup(&fixture);
    with_bwrap(&fixture);

    g_hash_table_insert(env_in, g_strdup("TMPDIR"), g_strdup("/srv/leak"));
    g_hash_table_insert(env_in, g_strdup("HOME"), g_strdup("/srv/leak"));
    g_hash_table_insert(env_in, g_strdup("XDG_CACHE_HOME"),
                        g_strdup("/srv/leak/cache"));
    g_hash_table_insert(env_in, g_strdup("XDG_RUNTIME_DIR"),
                        g_strdup("/srv/leak/run"));
    g_hash_table_insert(env_in, g_strdup("AGENT_FLAVOUR"),
                        g_strdup("salty"));

    config = load(&fixture, NULL, "    runtime:\n      confine: bwrap\n");
    runtime = confined_runtime(&fixture, config, env_in, &binary,
                               &config_path);

    g_assert_true(clawt_process_runtime_prepare(runtime, &argv, &env, NULL));

    g_assert_cmpstr(env_value(env, "TMPDIR"), ==, "/tmp");
    g_assert_cmpstr(env_value(env, "XDG_CACHE_HOME"), ==,
                    g_get_user_cache_dir());
    g_assert_cmpstr(env_value(env, "XDG_RUNTIME_DIR"), ==,
                    g_get_user_runtime_dir());
    g_assert_cmpstr(env_value(env, "AGENT_FLAVOUR"), ==, "salty");

    for (i = 0; env[i] != NULL; i++)
        g_assert_null(strstr(env[i], "/srv/leak"));

    /*
     * And unconfined, the same agent keeps what it set -- the rule is
     * the sandbox's, not a change to every agent's environment.
     */
    {
        g_auto(GStrv) bare_argv = NULL;
        g_auto(GStrv) bare_env = NULL;

        clawt_process_runtime_set_sandbox(runtime, NULL);
        g_assert_true(clawt_process_runtime_prepare(runtime, &bare_argv,
                                                    &bare_env, NULL));
        g_assert_cmpstr(bare_argv[0], ==, binary);
        g_assert_cmpstr(env_value(bare_env, "TMPDIR"), ==, "/srv/leak");
    }

    fixture_teardown(&fixture);
}

/*
 * bubblewrap gone between load and start: refused, never spawned bare.
 */
static void
test_prepare_refuses_without_bwrap(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = NULL;
    g_auto(GStrv) env = NULL;
    g_autofree gchar *binary = NULL;
    g_autofree gchar *config_path = NULL;

    fixture_setup(&fixture);
    with_bwrap(&fixture);

    config = load(&fixture, NULL, "    runtime:\n      confine: bwrap\n");
    runtime = confined_runtime(&fixture, config, NULL, &binary,
                               &config_path);

    without_bwrap(&fixture);

    g_assert_false(clawt_process_runtime_prepare(runtime, &argv, &env,
                                                 &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
    g_assert_null(argv);
    g_assert_null(env);

    fixture_teardown(&fixture);
}

/*
 * A read-only bind of a file with a second name is refused before
 * anything is spawned -- the name it is bound under is not the only one
 * the agent can reach it by.
 */
static void
test_prepare_refuses_a_hardlinked_bind(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) argv = NULL;
    g_auto(GStrv) env = NULL;
    g_autofree gchar *binary = NULL;
    g_autofree gchar *config_path = NULL;
    g_autofree gchar *file = NULL;
    g_autofree gchar *alias = NULL;
    g_autofree gchar *block = NULL;

    fixture_setup(&fixture);
    with_bwrap(&fixture);

    file = g_build_filename(fixture.tools, "settings.json", NULL);
    alias = g_build_filename(fixture.workspace, "writable-alias", NULL);
    g_assert_true(g_file_set_contents(file, "{}", -1, NULL));
    g_assert_cmpint(link(file, alias), ==, 0);

    block = g_strdup_printf("    runtime:\n      confine: bwrap\n"
                            "      confine_paths: [\"%s\"]\n", file);
    config = load(&fixture, NULL, block);
    runtime = confined_runtime(&fixture, config, NULL, &binary,
                               &config_path);

    g_assert_false(clawt_process_runtime_prepare(runtime, &argv, &env,
                                                 &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
    g_assert_nonnull(strstr(error->message, file));

    fixture_teardown(&fixture);
}

/* ── clawtilla_computer_exec gets the same checks ────────────────── */

/*
 * The host computer's bwrap box applies the hard-link refusal too, before
 * anything is spawned: a denied key with a second name was readable
 * through it while the agent was told the key was off-limits.  The stub
 * bwrap is enough, since the refusal comes first.
 */
static void
test_host_exec_refuses_a_hardlinked_deny(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtExecResult) result = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *key = NULL;
    g_autofree gchar *alias = NULL;
    const gchar *argv[] = { "true", NULL };

    fixture_setup(&fixture);
    with_bwrap(&fixture);

    key = g_build_filename(fixture.workspace, "id_ed25519", NULL);
    alias = g_build_filename(fixture.tools, "copy", NULL);
    g_assert_true(g_file_set_contents(key, "k", -1, NULL));
    g_assert_cmpint(link(key, alias), ==, 0);

    sandbox = clawt_sandbox_new(CLAWT_CONFINE_BWRAP, fixture.workspace);
    clawt_sandbox_add_deny_path(sandbox, key);
    computer = clawt_host_computer_new("boxed", sandbox);

    g_assert_true(clawt_computer_start(computer, &error));
    g_assert_no_error(error);

    result = clawt_computer_exec(computer, argv, NULL, 10, NULL, &error);
    g_assert_null(result);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
    g_assert_nonnull(strstr(error->message, "hard links"));

    clawt_computer_stop(computer, NULL);
    fixture_teardown(&fixture);
}

/* ── What the clients say ────────────────────────────────────────── */

/*
 * The warning beside the setting is the same two refusals the validator
 * makes, worded once for all three clients.
 */
static void
test_notice(void)
{
    const gchar *missing = clawt_agent_confine_notice("bwrap", "process",
                                                      FALSE);
    const gchar *embedded = clawt_agent_confine_notice("bwrap", "embedded",
                                                       TRUE);

    g_assert_nonnull(missing);
    g_assert_nonnull(strstr(missing, "bubblewrap"));
    g_assert_nonnull(strstr(missing, "shadow"));
    g_assert_nonnull(embedded);
    g_assert_nonnull(strstr(embedded, "embedded"));

    g_assert_null(clawt_agent_confine_notice("bwrap", "process", TRUE));
    g_assert_null(clawt_agent_confine_notice("none", "embedded", FALSE));
    g_assert_null(clawt_agent_confine_notice(NULL, NULL, FALSE));

    g_assert_nonnull(strstr(clawt_agent_confine_label(), "bubblewrap"));
    g_assert_nonnull(strstr(clawt_agent_confine_explanation(), "shadow"));
}

/* ── A real box (CLAWT_TEST_INTEGRATION=1) ───────────────────────── */

/*
 * Starts a confined child under real bubblewrap and reads what it sees.
 *
 * The "libreclaw" here is /bin/sh: the runtime runs `<binary> -c
 * <config>`, so a script path as the config is a program that runs
 * inside the box.  It writes what it can and cannot reach into the
 * workspace, then waits for SIGTERM -- and the stop has to deliver that
 * signal to *it*, through bwrap, which forwards nothing.
 */
static void
test_integration_real_box(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtProcessRuntime) runtime = NULL;
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GHashTable) env_in = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    g_autofree gchar *script = NULL;
    g_autofree gchar *report = NULL;
    g_autofree gchar *outside = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *terminated = NULL;
    g_autofree gchar *real_path = NULL;
    g_autofree gchar *host_bwrap = NULL;
    guint waited;

    if (g_getenv("CLAWT_TEST_INTEGRATION") == NULL) {
        g_test_skip("set CLAWT_TEST_INTEGRATION=1 to run a real bwrap");
        return;
    }

    real_path = g_strdup(g_getenv("PATH"));
    host_bwrap = g_find_program_in_path("bwrap");

    if (host_bwrap == NULL) {
        g_test_skip("bubblewrap is not installed");
        return;
    }

    fixture_setup(&fixture);

    outside = g_build_filename(fixture.tools, "not-yours", NULL);
    g_assert_true(g_file_set_contents(outside, "x", -1, NULL));
    report = g_build_filename(fixture.workspace, "report", NULL);
    terminated = g_build_filename(fixture.workspace, "terminated", NULL);
    script = g_build_filename(fixture.workspace, "probe.sh", NULL);

    body = g_strdup_printf(
        "#!/bin/sh\n"
        "trap 'echo yes > \"%s\"; exit 0' TERM\n"
        "{\n"
        "  echo \"tmpdir=$TMPDIR\"\n"
        "  echo \"home=$HOME\"\n"
        "  if [ -e \"%s\" ]; then echo outside=visible; "
        "else echo outside=absent; fi\n"
        "  if echo hi > /tmp/probe; then echo tmp=writable; fi\n"
        "  if touch \"$HOME/.probe\"; then echo home=writable; fi\n"
        "} > \"%s\"\n"
        "while :; do sleep 0.1; done\n",
        terminated, outside, report);
    write_executable(script, body);

    config = load(&fixture, NULL, "    runtime:\n      confine: bwrap\n");

    runtime = clawt_process_runtime_new(
        clawt_config_get_agent(config, "boxed"), script);
    clawt_process_runtime_set_binary(runtime, "/bin/sh");

    g_hash_table_insert(env_in, g_strdup("TMPDIR"),
                        g_strdup(fixture.tools));
    g_hash_table_insert(env_in, g_strdup("PATH"), g_strdup(real_path));
    clawt_process_runtime_set_environment(runtime, env_in);

    sandbox = clawt_agent_confine_build(
        clawt_config_get_agent(config, "boxed"), fixture.state, NULL, NULL,
        NULL, &error);
    g_assert_no_error(error);
    clawt_process_runtime_set_sandbox(runtime, sandbox);

    g_assert_true(clawt_agent_runtime_start(CLAWT_AGENT_RUNTIME(runtime),
                                            &error));
    g_assert_no_error(error);

    /* Bounded, so a box that never starts fails rather than hangs. */
    for (waited = 0; waited < 100 &&
                     !g_file_get_contents(report, &text, NULL, NULL);
         waited++)
        g_usleep(100 * 1000);

    g_usleep(300 * 1000);
    g_clear_pointer(&text, g_free);
    g_assert_true(g_file_get_contents(report, &text, NULL, NULL));

    g_assert_nonnull(strstr(text, "tmpdir=/tmp\n"));
    g_assert_nonnull(strstr(text, "outside=absent"));
    g_assert_nonnull(strstr(text, "tmp=writable"));
    g_assert_nonnull(strstr(text, "home=writable"));

    /* The pid reported is the payload's, not bwrap's. */
    {
        GPid pid = clawt_agent_runtime_get_pid(CLAWT_AGENT_RUNTIME(runtime));
        g_autofree gchar *comm_path =
            g_strdup_printf("/proc/%d/comm", (gint)pid);
        g_autofree gchar *comm = NULL;

        g_assert_cmpint(pid, >, 0);
        g_assert_true(g_file_get_contents(comm_path, &comm, NULL, NULL));
        g_assert_cmpstr(g_strstrip(comm), !=, "bwrap");
    }

    clawt_agent_runtime_stop(CLAWT_AGENT_RUNTIME(runtime));

    /* SIGTERM reached the payload, which bwrap alone would not do. */
    g_assert_true(g_file_test(terminated, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/agent-confine/off-by-default",
                    test_off_by_default_and_fleet_default);
    g_test_add_func("/agent-confine/embedded-refused",
                    test_embedded_is_refused);
    g_test_add_func("/agent-confine/missing-bwrap-shadow",
                    test_missing_bwrap_is_a_shadow);
    g_test_add_func("/agent-confine/unknown-value",
                    test_unknown_value_is_a_shadow);
    g_test_add_func("/agent-confine/builder-grants", test_builder_grants);
    g_test_add_func("/agent-confine/default-logins",
                    test_default_login_paths);
    g_test_add_func("/agent-confine/prepare-wraps",
                    test_prepare_wraps_libreclaw);
    g_test_add_func("/agent-confine/prepare-env",
                    test_prepare_owns_the_environment);
    g_test_add_func("/agent-confine/prepare-no-bwrap",
                    test_prepare_refuses_without_bwrap);
    g_test_add_func("/agent-confine/prepare-hardlink",
                    test_prepare_refuses_a_hardlinked_bind);
    g_test_add_func("/agent-confine/host-exec-hardlink",
                    test_host_exec_refuses_a_hardlinked_deny);
    g_test_add_func("/agent-confine/notice", test_notice);
    g_test_add_func("/agent-confine/integration-real-box",
                    test_integration_real_box);

    return g_test_run();
}
