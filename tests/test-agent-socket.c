/*
 * test-agent-socket.c - An agent's own IPC socket, and nothing more
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * VENTURE #72: a bwrap-confined agent's box bound the operator's
 * daemon.sock read-write, so anything in it that could exec could send
 * any IPC verb as the operator -- answer its own policy ask, rewrite its
 * own rules and confinement, add a trigger, unblock anybody's task.
 *
 * These run a scratch daemon with one confined agent (`alpha`, behind a
 * stub bwrap, write=ask) and one unconfined (`beta`), both on the fake
 * libreclaw, and check four things:
 *
 *   a) every socket the box binds refuses the operator's verbs;
 *   b) the verbs clawtilla-mcp-server, the policy hook and the relays
 *      send still work there;
 *   c) alpha's socket cannot act as beta, whatever the payload says;
 *   d) the box clawt_agent_confine_build() produced binds no daemon.sock.
 *
 * (a) and (d) are read from the bwrap argv the daemon actually built, so
 * they say the same thing about the box the agent would really run in.
 * Hermetic: no real bubblewrap runs and the network is never opened.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <gio/gunixsocketaddress.h>
#include <string.h>
#include <sys/stat.h>

#include "clawt-test-util.h"

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    gchar        *bin;          /* holds the stub bwrap */
    gchar        *saved_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
} Fixture;

static void
write_config(Fixture *fixture)
{
    g_autofree gchar *binary = g_build_filename(CLAWT_TEST_FIXTURES,
                                                "fake-libreclaw", NULL);
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    /*
     * Everything pinned under the temporary directory, as in
     * test-daemon.c: the sockets, the state, the pods, the skills and
     * the workspaces, so nothing here reaches a real fleet.
     */
    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "skills:\n  dir: \"%s/skills\"\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n"
        "  libreclaw_binary: \"%s\"\n"
        "agents:\n"
        "  - id: alpha\n"
        "    enabled: true\n"
        "    computer:\n"
        "      type: none\n"
        "    runtime:\n"
        "      confine: bwrap\n"
        "    policy:\n"
        "      rules: [\"write=ask\"]\n"
        "  - id: beta\n"
        "    enabled: true\n"
        "    computer:\n"
        "      type: none\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        fixture->dir, binary);

    g_file_set_contents(fixture->config_path, yaml, -1, &error);
    g_assert_no_error(error);
}

static void
fixture_setup(Fixture *fixture)
{
    g_autofree gchar *stub = NULL;
    g_autofree gchar *path = NULL;

    fixture->dir = g_dir_make_tmp("clawt-agentsock-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);
    fixture->bin = g_build_filename(fixture->dir, "bin", NULL);
    g_assert_cmpint(g_mkdir_with_parents(fixture->bin, 0700), ==, 0);

    /*
     * bubblewrap "installed": a stub first on PATH, which exits at once
     * when the runtime spawns it.  The box is read as the argv the
     * daemon built, never run.
     */
    stub = g_build_filename(fixture->bin, "bwrap", NULL);
    g_assert_true(g_file_set_contents(stub, "#!/bin/sh\nexit 0\n", -1,
                                      NULL));
    g_assert_cmpint(g_chmod(stub, 0755), ==, 0);

    /*
     * Not the runner's own agent id: ClawtClient puts CLAWTILLA_AGENT_ID
     * in every frame as acting_agent, and a suite run from inside an
     * agent's shell would otherwise be refused as that agent on alpha's
     * socket.  The test that wants it sets it itself.
     */
    g_unsetenv("CLAWTILLA_AGENT_ID");

    fixture->saved_path = g_strdup(g_getenv("PATH"));
    path = g_strdup_printf("%s:%s", fixture->bin,
                           fixture->saved_path != NULL ? fixture->saved_path
                                                       : "/usr/bin:/bin");
    g_setenv("PATH", path, TRUE);

    write_config(fixture);

    fixture->context = g_main_context_new();
    g_main_context_push_thread_default(fixture->context);
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->daemon != NULL) {
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

    /* Closing a listener finishes on the next iteration; see test-daemon.c. */
    while (g_main_context_iteration(fixture->context, FALSE))
        ;

    g_main_context_pop_thread_default(fixture->context);
    g_clear_pointer(&fixture->context, g_main_context_unref);

    if (fixture->saved_path != NULL)
        g_setenv("PATH", fixture->saved_path, TRUE);
    else
        g_unsetenv("PATH");

    clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->config_path, g_free);
    g_clear_pointer(&fixture->bin, g_free);
    g_clear_pointer(&fixture->saved_path, g_free);
}

/* The daemon up, and both agents started through the daemon's own path. */
static void
start_fleet(Fixture *fixture)
{
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_daemon_start(fixture->daemon, &error));
    g_assert_no_error(error);

    g_assert_true(clawt_daemon_start_agent(fixture->daemon, "alpha",
                                           &error));
    g_assert_no_error(error);
    g_assert_true(clawt_daemon_start_agent(fixture->daemon, "beta",
                                           &error));
    g_assert_no_error(error);
}

static gchar *
operator_socket(Fixture *fixture)
{
    return g_build_filename(fixture->dir, "daemon.sock", NULL);
}

static gchar *
link_socket(Fixture *fixture)
{
    return g_build_filename(fixture->dir, "state", "agents.sock", NULL);
}

/* Where the daemon puts an agent's own socket: beside daemon.sock. */
static gchar *
agent_socket(Fixture *fixture, const gchar *agent_id)
{
    g_autofree gchar *name = g_strconcat(agent_id, ".sock", NULL);

    return g_build_filename(fixture->dir, "agent-sockets", name, NULL);
}

static gchar *
agent_token(Fixture *fixture, const gchar *agent_id)
{
    g_autofree gchar *path = g_build_filename(fixture->dir, "state",
                                              "agents", agent_id, "token",
                                              NULL);
    gchar *token = NULL;

    g_assert_true(g_file_get_contents(path, &token, NULL, NULL));

    return g_strstrip(token);
}

/*
 * The bwrap argv the daemon built for @agent_id's box: the very sandbox
 * clawt_daemon_start_agent() handed its runtime, prepared as it would be
 * spawned.
 */
static GStrv
box_argv(Fixture *fixture, const gchar *agent_id)
{
    ClawtAgent *agent = clawt_agent_manager_get(
        clawt_daemon_get_agents(fixture->daemon), agent_id);
    ClawtAgentRuntime *runtime;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) env = NULL;
    GStrv argv = NULL;

    g_assert_nonnull(agent);
    runtime = clawt_agent_get_runtime(agent);
    g_assert_true(CLAWT_IS_PROCESS_RUNTIME(runtime));

    g_assert_true(clawt_process_runtime_prepare(
        CLAWT_PROCESS_RUNTIME(runtime), &argv, &env, &error));
    g_assert_no_error(error);
    g_assert_nonnull(argv);

    return argv;
}

/* Whether @path is bound into the box by any flag at all. */
static gboolean
argv_binds(GStrv argv, const gchar *path)
{
    guint i;

    for (i = 0; argv[i] != NULL; i++) {
        if (g_str_has_suffix(argv[i], "bind") &&
            g_str_has_prefix(argv[i], "--") &&
            argv[i + 1] != NULL && g_strcmp0(argv[i + 1], path) == 0)
            return TRUE;
    }

    return FALSE;
}

/*
 * Every unix socket the box binds, other than the link (which speaks
 * libreclaw's own protocol and authenticates with the agent's token at
 * hello; test-link.c is its test).  These are the IPC sockets a process
 * inside the box can dial.
 */
static GPtrArray *
box_ipc_sockets(Fixture *fixture, GStrv argv)
{
    GPtrArray *found = g_ptr_array_new_with_free_func(g_free);
    g_autofree gchar *link = link_socket(fixture);
    guint i;

    for (i = 0; argv[i] != NULL; i++) {
        GStatBuf info;

        if (!g_str_has_suffix(argv[i], "bind") || argv[i + 1] == NULL)
            continue;

        if (g_strcmp0(argv[i + 1], link) == 0 ||
            g_stat(argv[i + 1], &info) != 0 || !S_ISSOCK(info.st_mode))
            continue;

        g_ptr_array_add(found, g_strdup(argv[i + 1]));
    }

    return found;
}

static ClawtClient *
connect_to(const gchar *path)
{
    g_autoptr(GError) error = NULL;
    ClawtClient *client = clawt_client_new(path);

    g_assert_true(clawt_client_connect(client, &error));
    g_assert_no_error(error);

    return client;
}

static JsonNode *
parse(const gchar *json)
{
    g_autoptr(JsonParser) parser = json_parser_new();

    g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));

    return json_node_copy(json_parser_get_root(parser));
}

/* One request; the reply, or NULL with @error set. */
static JsonNode *
ask(ClawtClient *client, const gchar *kind, const gchar *payload_json,
    GError **error)
{
    return clawt_client_request(client, kind,
                                payload_json != NULL ? parse(payload_json)
                                                     : NULL,
                                error);
}

/* The value at @key in alpha's config, as the operator socket reads it. */
static gchar *
alpha_restart_policy(Fixture *fixture)
{
    ClawtConfig *config = clawt_daemon_get_config(fixture->daemon);
    ClawtAgentConfig *alpha = clawt_config_get_agent(config, "alpha");

    return g_strdup(clawt_enum_to_nick(
        CLAWT_TYPE_RESTART_POLICY,
        clawt_agent_config_get_enum(alpha, "runtime.restart")));
}

/* ── d) the box ──────────────────────────────────────────────────── */

/*
 * The confined box binds its link and its own socket, and never the
 * operator's daemon.sock.  On 0b2bf57a it bound daemon.sock read-write
 * (clawt-daemon.c: sockets[] = { link_socket, ipc_socket }).
 */
static void
test_box_has_no_operator_socket(void)
{
    Fixture fixture = { 0 };
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *daemon_sock = NULL;
    g_autofree gchar *own = NULL;
    g_autofree gchar *link = NULL;

    fixture_setup(&fixture);
    start_fleet(&fixture);

    argv = box_argv(&fixture, "alpha");
    daemon_sock = operator_socket(&fixture);
    own = agent_socket(&fixture, "alpha");
    link = link_socket(&fixture);

    g_assert_true(g_file_test(daemon_sock, G_FILE_TEST_EXISTS));
    g_assert_false(argv_binds(argv, daemon_sock));

    g_assert_true(argv_binds(argv, link));
    g_assert_true(argv_binds(argv, own));

    /* And nobody else's: beta's socket exists, and is not in alpha's box. */
    {
        g_autofree gchar *theirs = agent_socket(&fixture, "beta");

        g_assert_true(g_file_test(theirs, G_FILE_TEST_EXISTS));
        g_assert_false(argv_binds(argv, theirs));
    }

    fixture_teardown(&fixture);
}

/* ── a) the operator's verbs, from inside the box ────────────────── */

/*
 * From every IPC socket the box can dial, the operator's verbs are
 * refused -- by name, as a permission error, and without doing anything.
 * On 0b2bf57a the box's one IPC socket was daemon.sock, where agent.set
 * succeeded and changed alpha's own runtime.restart.
 */
static void
test_box_refuses_operator_verbs(void)
{
    static const struct {
        const gchar *kind;
        const gchar *payload;
    } operator_verbs[] = {
        { "agent.set",
          "{\"agent\":\"alpha\",\"key\":\"runtime.restart\","
          "\"value\":\"always\"}" },
        { "decision.answer",
          "{\"id\":\"policy-1\",\"answer\":\"approve\"}" },
        { "trigger.add",
          "{\"id\":\"t1\",\"agent\":\"alpha\",\"forge\":\"github\","
          "\"events\":[\"push\"]}" },
        { "task.unblock", "{\"id\":\"task-1\"}" },
        { "computer.exec",
          "{\"agent\":\"alpha\",\"argv\":[\"true\"]}" },
        { "control.subscribe", NULL },
        { "device.redeem", "{\"code\":\"123456\"}" },
        { "agent.restart", "{\"agent\":\"alpha\"}" },
        { "agent.stop", "{\"agent\":\"beta\"}" },
        { "task.cancel", "{\"id\":\"task-1\"}" },
        { "routine.add",
          "{\"agent\":\"alpha\",\"id\":\"r1\",\"cron\":\"* * * * *\","
          "\"prompt\":\"x\"}" },

        /*
         * Reads, refused because a reply can carry a secret or another
         * agent's conversations (Zach's decision ba0011f1: agents may
         * read fleet state, never secrets).
         */
        { "config.show", NULL },
        { "agent.show", "{\"agent\":\"beta\"}" },
        { "connector.list", NULL },
        { "room.history", "{\"room\":\"dm:beta\"}" },
        { "room.list", NULL },
        { "event.list", NULL },
        { "mailbox.list", "{\"agent\":\"beta\"}" }
    };
    Fixture fixture = { 0 };
    g_auto(GStrv) argv = NULL;
    g_autoptr(GPtrArray) sockets = NULL;
    g_autofree gchar *before = NULL;
    g_autofree gchar *after = NULL;
    guint i;
    guint j;

    fixture_setup(&fixture);
    start_fleet(&fixture);

    argv = box_argv(&fixture, "alpha");
    sockets = box_ipc_sockets(&fixture, argv);

    /* The box has to be able to reach the daemon at all, or (b) fails. */
    g_assert_cmpuint(sockets->len, >=, 1);

    before = alpha_restart_policy(&fixture);

    for (i = 0; i < sockets->len; i++) {
        g_autoptr(ClawtClient) client =
            connect_to(g_ptr_array_index(sockets, i));

        for (j = 0; j < G_N_ELEMENTS(operator_verbs); j++) {
            g_autoptr(GError) error = NULL;
            g_autoptr(JsonNode) reply = ask(client, operator_verbs[j].kind,
                                            operator_verbs[j].payload,
                                            &error);

            if (reply != NULL || error == NULL ||
                !g_error_matches(error, CLAWT_ERROR,
                                 CLAWT_ERROR_PERMISSION_DENIED))
                g_test_message("%s on %s: %s",
                               operator_verbs[j].kind,
                               (const gchar *)g_ptr_array_index(sockets, i),
                               error != NULL ? error->message
                                             : "served");

            g_assert_null(reply);
            g_assert_error(error, CLAWT_ERROR,
                           CLAWT_ERROR_PERMISSION_DENIED);
            g_assert_nonnull(strstr(error->message,
                                    operator_verbs[j].kind));
        }
    }

    /* Nothing was done: the refused agent.set changed nothing. */
    after = alpha_restart_policy(&fixture);
    g_assert_cmpstr(before, ==, after);

    fixture_teardown(&fixture);
}

/* ── b) what the agent's own clients send ────────────────────────── */

/*
 * What clawtilla-mcp-server, the policy hook and the relays send, over
 * the agent's own socket, exactly as they send it.  The mcp-server's
 * tool.rpc with the token; the same without `agent` at all, which the
 * connection supplies; the hook's policy.check, which with write=ask is
 * deferred to a person and so is answered here only by being queued.
 */
static void
test_agent_verbs_are_served(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *own = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *with_agent = NULL;
    g_autofree gchar *without_agent = NULL;
    g_autoptr(ClawtClient) client = NULL;

    fixture_setup(&fixture);
    start_fleet(&fixture);

    own = agent_socket(&fixture, "alpha");
    token = agent_token(&fixture, "alpha");
    client = connect_to(own);

    with_agent = g_strdup_printf(
        "{\"agent\":\"alpha\",\"token\":\"%s\",\"request\":"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}}",
        token);
    without_agent = g_strdup_printf(
        "{\"token\":\"%s\",\"request\":"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/list\"}}",
        token);

    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(client, "tool.rpc", with_agent,
                                        &error);
        JsonObject *response;

        g_assert_no_error(error);
        g_assert_nonnull(reply);
        response = json_object_get_object_member(json_node_get_object(reply),
                                                 "response");
        g_assert_true(json_object_has_member(response, "result"));
    }

    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(client, "tool.rpc", without_agent,
                                        &error);

        g_assert_no_error(error);
        g_assert_nonnull(reply);
    }

    /* A read is allowed outright by the rules: answered at once. */
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(
            client, "policy.check",
            "{\"agent\":\"alpha\",\"tool\":\"Read\",\"summary\":\"Read x\"}",
            &error);

        g_assert_no_error(error);
        g_assert_nonnull(reply);
        g_assert_true(json_object_get_boolean_member(
            json_node_get_object(reply), "allowed"));
    }

    /*
     * The relays' policy.check carries no token, and the connection
     * says who it is: still answered.
     */
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(
            client, "policy.check",
            "{\"tool\":\"Grep\",\"class\":\"read\"}", &error);

        g_assert_no_error(error);
        g_assert_nonnull(reply);
    }

    /*
     * Fleet state, read as the operator would read it (Zach's decision
     * ba0011f1): the roster and the task list.
     */
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(client, "agent.list", NULL, &error);
        JsonArray *agents;

        g_assert_no_error(error);
        g_assert_nonnull(reply);
        agents = json_object_get_array_member(json_node_get_object(reply),
                                              "agents");
        g_assert_cmpuint(json_array_get_length(agents), ==, 2);
    }

    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(client, "task.list", NULL, &error);

        g_assert_no_error(error);
        g_assert_nonnull(reply);
    }

    /* A relay may only speak for connectors its agent holds. */
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(
            client, "connector.tools_seen",
            "{\"name\":\"somebody-elses\",\"tools\":3}", &error);

        g_assert_null(reply);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    }

    fixture_teardown(&fixture);
}

/*
 * The files the agent's CLI is handed point its clients at its own
 * socket: the .mcp.json the CLI starts clawtilla-mcp-server from, and
 * the hook settings that run `clawtilla policy-hook`.
 */
static void
test_rendered_clients_dial_the_agent_socket(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *mcp_path = NULL;
    g_autofree gchar *mcp = NULL;
    g_autofree gchar *hook_path = NULL;
    g_autofree gchar *hook = NULL;
    g_autofree gchar *own = NULL;
    g_autofree gchar *daemon_sock = NULL;

    fixture_setup(&fixture);
    start_fleet(&fixture);

    own = agent_socket(&fixture, "alpha");
    daemon_sock = operator_socket(&fixture);

    mcp_path = g_build_filename(fixture.dir, "agents", "alpha", ".mcp.json",
                                NULL);
    g_assert_true(g_file_get_contents(mcp_path, &mcp, NULL, NULL));
    g_assert_nonnull(strstr(mcp, own));
    g_assert_null(strstr(mcp, daemon_sock));

    hook_path = g_build_filename(fixture.dir, "state", "agents", "alpha",
                                 "claude-settings.json", NULL);
    g_assert_true(g_file_get_contents(hook_path, &hook, NULL, NULL));
    g_assert_nonnull(strstr(hook, own));
    g_assert_null(strstr(hook, daemon_sock));

    fixture_teardown(&fixture);
}

/* ── c) one agent cannot be another ──────────────────────────────── */

/*
 * alpha's socket acts as alpha only.  Naming beta in the payload is
 * refused -- with beta's own token too, so a token is not a way round it
 * -- and so is alpha's token on beta's socket, where the connection is
 * beta and the token does not match.
 */
static void
test_one_agent_cannot_act_as_another(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *alpha_sock = NULL;
    g_autofree gchar *beta_sock = NULL;
    g_autofree gchar *alpha_token = NULL;
    g_autofree gchar *beta_token = NULL;
    g_autofree gchar *as_beta = NULL;
    g_autofree gchar *alphas_token_on_beta = NULL;
    g_autoptr(ClawtClient) alpha = NULL;
    g_autoptr(ClawtClient) beta = NULL;
    static const gchar *naming_beta[] = {
        "policy.check", "computer.desktop", "computer.control",
        "connector.app", "connector.result_trimmed"
    };
    guint i;

    fixture_setup(&fixture);
    start_fleet(&fixture);

    alpha_sock = agent_socket(&fixture, "alpha");
    beta_sock = agent_socket(&fixture, "beta");
    alpha_token = agent_token(&fixture, "alpha");
    beta_token = agent_token(&fixture, "beta");
    alpha = connect_to(alpha_sock);
    beta = connect_to(beta_sock);

    as_beta = g_strdup_printf(
        "{\"agent\":\"beta\",\"token\":\"%s\",\"request\":"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}}",
        beta_token);

    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(alpha, "tool.rpc", as_beta, &error);

        g_assert_null(reply);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    }

    for (i = 0; i < G_N_ELEMENTS(naming_beta); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(
            alpha, naming_beta[i],
            "{\"agent\":\"beta\",\"tool\":\"Bash\",\"name\":\"x\"}",
            &error);

        g_assert_null(reply);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    }

    /* Not a string is not an agent id either. */
    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(
            alpha, "policy.check", "{\"agent\":[\"alpha\"],\"tool\":\"Read\"}",
            &error);

        g_assert_null(reply);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    }

    /* beta's socket with alpha's token: the connection is beta. */
    alphas_token_on_beta = g_strdup_printf(
        "{\"token\":\"%s\",\"request\":"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}}",
        alpha_token);

    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(beta, "tool.rpc",
                                        alphas_token_on_beta, &error);

        g_assert_null(reply);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    }

    fixture_teardown(&fixture);
}

/*
 * The frame's `acting_agent` (the CLI sets it from CLAWTILLA_AGENT_ID)
 * says who a save is credited to.  On alpha's socket a frame naming beta
 * is refused before anything runs -- a read and an act alike -- so it is
 * never credited to beta; naming alpha, or nobody, is served.  On the
 * head before this, the frame was served and the daemon credited beta.
 */
static void
test_acting_agent_is_the_connection(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *own = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *call = NULL;
    g_autofree gchar *saved = g_strdup(g_getenv("CLAWTILLA_AGENT_ID"));
    g_autoptr(ClawtClient) client = NULL;

    fixture_setup(&fixture);
    start_fleet(&fixture);

    own = agent_socket(&fixture, "alpha");
    token = agent_token(&fixture, "alpha");
    client = connect_to(own);
    call = g_strdup_printf(
        "{\"token\":\"%s\",\"request\":"
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}}",
        token);

    /* As a CLI run with beta's id in its environment would send it. */
    g_setenv("CLAWTILLA_AGENT_ID", "beta", TRUE);

    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(client, "agent.list", NULL, &error);

        g_assert_null(reply);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
        g_assert_nonnull(strstr(error->message, "acting_agent"));
    }

    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(client, "tool.rpc", call, &error);

        g_assert_null(reply);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    }

    /* Naming the connection's own agent is served. */
    g_setenv("CLAWTILLA_AGENT_ID", "alpha", TRUE);

    {
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(client, "tool.rpc", call, &error);

        g_assert_no_error(error);
        g_assert_nonnull(reply);
    }

    if (saved != NULL)
        g_setenv("CLAWTILLA_AGENT_ID", saved, TRUE);
    else
        g_unsetenv("CLAWTILLA_AGENT_ID");

    fixture_teardown(&fixture);
}

/* ── The socket itself ───────────────────────────────────────────── */

/*
 * 0600 in a 0700 directory, gone when the agent stops and when the
 * daemon does, and a stale one a crashed daemon left is cleared at the
 * next start.  The operator socket is unchanged throughout: agent.set
 * still works there.
 */
static void
test_socket_lifecycle(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *own = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *stale = NULL;
    g_autofree gchar *daemon_sock = NULL;
    GStatBuf info;

    fixture_setup(&fixture);

    /* A stale socket, as a daemon killed with -9 leaves one. */
    dir = g_build_filename(fixture.dir, "agent-sockets", NULL);
    stale = g_build_filename(dir, "ghost.sock", NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    {
        g_autoptr(GSocket) socket = g_socket_new(G_SOCKET_FAMILY_UNIX,
                                                 G_SOCKET_TYPE_STREAM,
                                                 G_SOCKET_PROTOCOL_DEFAULT,
                                                 NULL);
        g_autoptr(GSocketAddress) address = g_unix_socket_address_new(stale);

        g_assert_nonnull(socket);
        g_assert_true(g_socket_bind(socket, address, FALSE, NULL));
        g_socket_close(socket, NULL);
    }
    g_assert_true(g_file_test(stale, G_FILE_TEST_EXISTS));

    start_fleet(&fixture);
    g_assert_false(g_file_test(stale, G_FILE_TEST_EXISTS));

    own = agent_socket(&fixture, "alpha");
    g_assert_cmpint(g_stat(own, &info), ==, 0);
    g_assert_true(S_ISSOCK(info.st_mode));
    g_assert_cmpint(info.st_mode & 0777, ==, 0600);
    g_assert_cmpint(g_stat(dir, &info), ==, 0);
    g_assert_cmpint(info.st_mode & 0777, ==, 0700);

    /* The operator socket still serves the operator. */
    daemon_sock = operator_socket(&fixture);
    {
        g_autoptr(ClawtClient) operator = connect_to(daemon_sock);
        g_autoptr(GError) error = NULL;
        g_autoptr(JsonNode) reply = ask(
            operator, "agent.set",
            "{\"agent\":\"beta\",\"key\":\"runtime.restart\","
            "\"value\":\"always\"}", &error);

        g_assert_no_error(error);
        g_assert_nonnull(reply);
    }

    clawt_daemon_stop_agent(fixture.daemon, "alpha", FALSE);
    g_assert_false(g_file_test(own, G_FILE_TEST_EXISTS));

    /* Back with the next start, and gone with the daemon. */
    {
        g_autoptr(GError) error = NULL;

        g_assert_true(clawt_daemon_start_agent(fixture.daemon, "alpha",
                                               &error));
        g_assert_no_error(error);
    }
    g_assert_true(g_file_test(own, G_FILE_TEST_EXISTS));

    clawt_daemon_stop(fixture.daemon);
    g_assert_false(g_file_test(own, G_FILE_TEST_EXISTS));

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/agent-socket/box-has-no-operator-socket",
                    test_box_has_no_operator_socket);
    g_test_add_func("/agent-socket/box-refuses-operator-verbs",
                    test_box_refuses_operator_verbs);
    g_test_add_func("/agent-socket/agent-verbs-are-served",
                    test_agent_verbs_are_served);
    g_test_add_func("/agent-socket/rendered-clients-dial-it",
                    test_rendered_clients_dial_the_agent_socket);
    g_test_add_func("/agent-socket/one-agent-cannot-act-as-another",
                    test_one_agent_cannot_act_as_another);
    g_test_add_func("/agent-socket/acting-agent-is-the-connection",
                    test_acting_agent_is_the_connection);
    g_test_add_func("/agent-socket/lifecycle", test_socket_lifecycle);

    return g_test_run();
}
