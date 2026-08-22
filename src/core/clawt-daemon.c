/*
 * clawt-daemon.c - The fleet, assembled
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "core/clawt-daemon.h"

#include <string.h>

/* How often expired mailbox items and abandoned leases are cleaned up. */
#define SWEEP_INTERVAL_SECONDS 60

struct _ClawtDaemon {
    GObject parent_instance;

    gchar        *config_path;
    GMainContext *main_context;
    GMainLoop    *loop;

    ClawtConfig        *config;
    ClawtAgentManager  *agents;
    ClawtRoomManager   *rooms;
    ClawtTaskManager   *tasks;
    ClawtMailboxRouter *router;
    ClawtLoopGuard     *guard;
    ClawtEventBus      *bus;
    ClawtEventLog      *log;
    ClawtExchange      *exchange;
    ClawtLinkServer    *link_server;
    ClawtIpcServer     *ipc_server;
    ClawtMcpTools      *mcp_tools;
    ClawtPodBridge     *pod_bridge;
    ClawtPluginManager *plugins;

    gchar   *libreclaw_binary;
    gchar   *state_dir;
    gchar   *link_socket;
    guint    sweep_source_id;
    gboolean running;
};

G_DEFINE_FINAL_TYPE(ClawtDaemon, clawt_daemon, G_TYPE_OBJECT)

enum {
    SIGNAL_STARTED,
    SIGNAL_STOPPED,
    SIGNAL_RELOADED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static gboolean deliver_for_tools(const gchar *from_agent,
                                  const gchar *target,
                                  const gchar *body,
                                  const gchar *task_id,
                                  gint         depth,
                                  gpointer     user_data,
                                  GError     **error);

/* ── Construction ────────────────────────────────────────────────── */

ClawtDaemon *
clawt_daemon_new(const gchar *config_path, GMainContext *main_context)
{
    ClawtDaemon *self = g_object_new(CLAWT_TYPE_DAEMON, NULL);

    self->config_path = (config_path != NULL)
                        ? clawt_expand_path(config_path)
                        : clawt_expand_path("~/.clawtilla/config.yaml");

    if (main_context != NULL)
        self->main_context = g_main_context_ref(main_context);

    return self;
}

void
clawt_daemon_set_libreclaw_binary(ClawtDaemon *self, const gchar *path)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    g_free(self->libreclaw_binary);
    self->libreclaw_binary = g_strdup(path);
}

ClawtConfig *
clawt_daemon_get_config(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->config;
}

ClawtAgentManager *
clawt_daemon_get_agents(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->agents;
}

ClawtRoomManager *
clawt_daemon_get_rooms(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->rooms;
}

ClawtTaskManager *
clawt_daemon_get_tasks(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->tasks;
}

ClawtMailboxRouter *
clawt_daemon_get_router(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->router;
}

ClawtEventBus *
clawt_daemon_get_event_bus(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->bus;
}

ClawtEventLog *
clawt_daemon_get_event_log(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->log;
}

ClawtExchange *
clawt_daemon_get_exchange(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->exchange;
}

ClawtLinkServer *
clawt_daemon_get_link_server(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->link_server;
}

ClawtIpcServer *
clawt_daemon_get_ipc_server(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->ipc_server;
}

ClawtLoopGuard *
clawt_daemon_get_loop_guard(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->guard;
}

ClawtPluginManager *
clawt_daemon_get_plugins(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->plugins;
}

ClawtMcpTools *
clawt_daemon_get_mcp_tools(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    return self->mcp_tools;
}

/* ── Link plumbing ───────────────────────────────────────────────── */

/*
 * Checks an agent's token against the file the daemon wrote for it.
 *
 * The socket's permissions already keep other users out; this is what
 * stops one of your own agents claiming to be another and reading its
 * mail.
 */
static gboolean
authenticate_agent(const gchar *agent_id, const gchar *token,
                   gpointer user_data)
{
    ClawtDaemon *self = user_data;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *expected = NULL;

    if (agent_id == NULL)
        return FALSE;

    if (clawt_agent_manager_get(self->agents, agent_id) == NULL)
        return FALSE;

    state_dir = clawt_config_agent_state_dir(self->config, agent_id);
    token_path = g_build_filename(state_dir, "token", NULL);

    if (!g_file_get_contents(token_path, &expected, NULL, NULL))
        return FALSE;

    g_strstrip(expected);

    if (token == NULL)
        return FALSE;

    /*
     * Length-independent comparison is not worth reaching for here: both
     * ends are local, and an attacker who can time this precisely can
     * already read the token file.
     */
    return g_strcmp0(token, expected) == 0;
}

static JsonNode *
on_agent_mcp_request(ClawtLink *link, JsonNode *request, gpointer user_data)
{
    ClawtDaemon *self = user_data;

    return clawt_mcp_tools_call(self->mcp_tools,
                                clawt_link_get_agent_id(link), request);
}

static void
on_link_added(ClawtLinkServer *server, const gchar *agent_id,
              gpointer user_data)
{
    ClawtDaemon *self = user_data;
    ClawtAgent *agent;
    ClawtLink *link;

    (void)server;

    agent = clawt_agent_manager_get(self->agents, agent_id);
    if (agent == NULL)
        return;

    link = clawt_link_server_get_link(self->link_server, agent_id);
    clawt_agent_set_link(agent, link);

    if (link != NULL)
        clawt_link_set_mcp_handler(link, on_agent_mcp_request, self, NULL);

    clawt_event_bus_emit(self->bus, "agent.connected", agent_id);

    /*
     * The backlog goes out the moment the agent connects.  Being able to
     * message an agent that is not running is the whole point of the
     * mailbox, and it is only true if starting one delivers what it
     * missed.
     */
    clawt_mailbox_router_drain(self->router, agent_id);
}

static void
on_link_removed(ClawtLinkServer *server, const gchar *agent_id,
                gpointer user_data)
{
    ClawtDaemon *self = user_data;
    ClawtAgent *agent;

    (void)server;

    agent = clawt_agent_manager_get(self->agents, agent_id);

    if (agent != NULL)
        clawt_agent_set_link(agent, NULL);

    clawt_event_bus_emit(self->bus, "agent.disconnected", agent_id);
}

/*
 * An agent's typing indicator becomes an agent.typing event.
 *
 * libreclaw raises it for the whole turn -- from the moment the message
 * is picked up to the moment the reply is posted -- so it is what tells
 * a client the difference between an agent that is working and one that
 * is never going to answer.  Not persisted: it describes right now, and
 * a client that connects late learns the state from the next transition.
 */
static void
on_link_typing(ClawtLinkServer *server,
               const gchar     *agent_id,
               const gchar     *room_id,
               gboolean         typing,
               gpointer         user_data)
{
    ClawtDaemon *self = user_data;
    ClawtEvent *event;

    (void)server;

    if (agent_id == NULL)
        return;

    event = clawt_event_new("agent.typing", agent_id);
    clawt_event_set_detail(event, "typing", typing ? "true" : "false");

    if (room_id != NULL)
        clawt_event_set_detail(event, "room", room_id);

    clawt_event_bus_publish(self->bus, event);
    clawt_event_free(event);
}

static void
on_link_message(ClawtLinkServer *server, const gchar *agent_id,
                const gchar *room_id, const gchar *body,
                const gchar *thread_id, gpointer user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(ClawtMessage) message = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *destination = room_id;

    (void)server;

    if (body == NULL)
        return;

    /*
     * An agent that replies without naming a room is answering whoever
     * last wrote to it, which for a delegated task is the agent that
     * delegated.  Without this fallback a reply would have nowhere to go
     * and be dropped, which reads as the agent ignoring the request.
     */
    if (destination == NULL) {
        ClawtAgent *chief = clawt_agent_manager_get_chief_of_staff(self->agents);

        destination = (chief != NULL) ? clawt_agent_get_id(chief) : NULL;
    }

    if (destination == NULL)
        return;

    message = clawt_message_new(destination, agent_id, body);
    clawt_message_set_task_id(message, thread_id);
    clawt_message_set_depth(message, 1);

    {
        ClawtEvent *event = clawt_event_new("message", destination);

        clawt_event_set_detail(event, "from", agent_id);
        clawt_event_set_detail(event, "body", body);
        clawt_event_bus_publish(self->bus, event);
        clawt_event_free(event);
    }

    /*
     * A task id in the reply means the delegated work is finished.  An
     * agent that replies without also calling clawtilla_task_complete
     * would otherwise leave the delegator waiting on a task that is
     * already done.
     */
    if (thread_id != NULL)
        clawt_task_manager_complete(self->tasks, thread_id, body);

    if (clawt_mailbox_router_send(self->router, message, &error) < 0)
        g_info("daemon: %s's message was not routed: %s", agent_id,
               error->message);
}

/*
 * How the orchestration tools actually send anything.
 *
 * Routed through the same path as every other message rather than posting
 * straight into a mailbox, so the hop limits, rate limits and cycle
 * detection apply to tool calls exactly as they do to ordinary chat.
 */
static gboolean
deliver_for_tools(const gchar *from_agent, const gchar *target,
                  const gchar *body, const gchar *task_id, gint depth,
                  gpointer user_data, GError **error)
{
    ClawtDaemon *self = user_data;

    return clawt_mailbox_router_send_to(self->router, from_agent, target,
                                        body, task_id, depth, error) >= 0;
}

/* ── Agents ──────────────────────────────────────────────────────── */

static void
apply_mounts(ClawtDaemon *self, ClawtAgent *agent, ClawtComputer *computer)
{
    ClawtAgentConfig *config = clawt_agent_get_config(agent);
    g_autoptr(GPtrArray) mounts = NULL;
    guint i;

    mounts = clawt_agent_config_get_mounts(config);

    for (i = 0; mounts != NULL && i < mounts->len; i++)
        clawt_computer_add_mount(computer, g_ptr_array_index(mounts, i));

    /*
     * Every computer gets the exchange unless the agent turned it off,
     * because the alternative is each pair of agents needing a hand-wired
     * mount before they can pass a file.
     */
    if (self->exchange != NULL &&
        clawt_agent_config_get_boolean(config, "computer.exchange")) {
        g_autoptr(GError) error = NULL;

        if (clawt_exchange_prepare(self->exchange, clawt_agent_get_id(agent),
                                   &error)) {
            g_autoptr(GPtrArray) exchange_mounts =
                clawt_exchange_get_mounts(self->exchange,
                                          clawt_agent_get_id(agent));

            for (i = 0; i < exchange_mounts->len; i++)
                clawt_computer_add_mount(
                    computer, g_ptr_array_index(exchange_mounts, i));
        } else {
            g_warning("agent %s: the exchange is unavailable: %s",
                      clawt_agent_get_id(agent), error->message);
        }
    }
}

/*
 * Renders every agent's files, whether or not it is running.
 *
 * Done up front rather than at start time only, because the link token is
 * written here and an agent started by hand -- or by systemd, or inside a
 * container someone else brought up -- has to be able to authenticate
 * without clawtilla having launched it.
 */
static void
render_all_agents(ClawtDaemon *self)
{
    GPtrArray *agents = clawt_agent_manager_list(self->agents);
    guint i;

    for (i = 0; i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        ClawtAgentConfig *config = clawt_agent_get_config(agent);
        g_autoptr(GError) error = NULL;

        if (clawt_agent_config_is_shadow(config))
            continue;

        if (!clawt_config_write_agent_files(self->config, config,
                                            self->link_socket, NULL, &error))
            g_warning("agent %s: %s", clawt_agent_get_id(agent),
                      error->message);
    }
}

gboolean
clawt_daemon_start_agent(ClawtDaemon *self, const gchar *agent_id,
                         GError **error)
{
    ClawtAgent *agent;
    ClawtAgentConfig *config;
    g_autofree gchar *config_path = NULL;
    g_autoptr(GError) local = NULL;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);
    g_return_val_if_fail(agent_id != NULL, FALSE);

    agent = clawt_agent_manager_get(self->agents, agent_id);

    if (agent == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no agent called '%s'", agent_id);
        return FALSE;
    }

    if (clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_SHADOW) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "%s cannot start: %s", agent_id,
                    clawt_agent_get_status_detail(agent));
        return FALSE;
    }

    config = clawt_agent_get_config(agent);

    /*
     * Integrations are checked before anything is started.  An agent whose
     * Matrix block is missing its homeserver starts perfectly cleanly and
     * then never receives anything, which is a much worse failure than
     * refusing here with the key named.
     */
    if (!clawt_integration_validate(config, error))
        return FALSE;

    if (!clawt_config_write_agent_files(self->config, config,
                                        self->link_socket, &config_path,
                                        error))
        return FALSE;

    /* The computer first: an agent that starts before its computer is
     * ready spends its first turns discovering it cannot reach it. */
    if (clawt_agent_get_computer(agent) == NULL) {
        g_autoptr(ClawtComputer) computer = NULL;

        computer = clawt_computer_factory_create(config, self->pod_bridge,
                                                 &local);

        if (computer == NULL) {
            clawt_agent_mark_shadow(agent, local->message);
            g_propagate_error(error, g_steal_pointer(&local));
            return FALSE;
        }

        apply_mounts(self, agent, computer);

        if (!clawt_computer_start(computer, &local)) {
            clawt_agent_mark_shadow(agent, local->message);
            g_propagate_error(error, g_steal_pointer(&local));
            return FALSE;
        }

        clawt_agent_set_computer(agent, computer);
    }

    if (clawt_agent_get_runtime(agent) == NULL) {
        ClawtRuntimeType type = (ClawtRuntimeType)
            clawt_agent_config_get_enum(config, "runtime.type");

        if (type == CLAWT_RUNTIME_EMBEDDED) {
            g_autoptr(ClawtEmbeddedRuntime) runtime =
                clawt_embedded_runtime_new(config, config_path,
                                           self->main_context);

            clawt_agent_set_runtime(agent, CLAWT_AGENT_RUNTIME(runtime));
        } else {
            g_autoptr(ClawtProcessRuntime) runtime =
                clawt_process_runtime_new(config, config_path);
            g_autoptr(GHashTable) env = NULL;

            /*
             * An explicit override wins over the config, so a test or a
             * host embedding the daemon can point at its own build
             * without editing anybody's file.
             */
            if (self->libreclaw_binary != NULL) {
                clawt_process_runtime_set_binary(runtime,
                                                 self->libreclaw_binary);
            } else {
                g_autofree gchar *configured = clawt_config_get_path_value(
                    self->config, "defaults.libreclaw_binary");

                if (configured != NULL)
                    clawt_process_runtime_set_binary(runtime, configured);
            }

            env = clawt_agent_config_get_env(config);

            /*
             * The agent's resolved credentials go in alongside its own
             * `env:` block: an API key that never reaches the child is an
             * agent that cannot talk to its provider.
             */
            {
                g_autofree gchar *secrets_dir =
                    clawt_config_get_path_value(self->config, "secrets.dir");
                g_autoptr(GHashTable) credentials = NULL;
                g_autoptr(GError) secret_error = NULL;

                credentials = clawt_agent_config_resolve_credentials(
                    config, secrets_dir,
                    (guint)clawt_config_get_int(
                        self->config, "secrets.command_timeout_seconds"),
                    &secret_error);

                if (credentials == NULL) {
                    clawt_agent_mark_shadow(agent, secret_error->message);
                    g_propagate_error(error, g_steal_pointer(&secret_error));
                    return FALSE;
                }

                {
                    GHashTableIter iter;
                    gpointer key;
                    gpointer value;

                    g_hash_table_iter_init(&iter, credentials);

                    while (g_hash_table_iter_next(&iter, &key, &value))
                        g_hash_table_insert(env, g_strdup(key),
                                            g_strdup(value));
                }
            }

            if (env != NULL)
                clawt_process_runtime_set_environment(runtime, env);

            clawt_agent_set_runtime(agent, CLAWT_AGENT_RUNTIME(runtime));
        }

        {
            ClawtAgentRuntime *runtime = clawt_agent_get_runtime(agent);

            clawt_agent_runtime_set_restart_policy(
                runtime,
                (ClawtRestartPolicy)clawt_agent_config_get_enum(
                    config, "runtime.restart"),
                (guint)clawt_agent_config_get_int(config,
                                                  "runtime.backoff_seconds"),
                (guint)clawt_agent_config_get_int(config,
                                                  "runtime.max_restarts"));
        }
    }

    if (!clawt_agent_start(agent, error))
        return FALSE;

    clawt_event_bus_emit(self->bus, "agent.started", agent_id);

    return TRUE;
}

gboolean
clawt_daemon_stop_agent(ClawtDaemon *self, const gchar *agent_id)
{
    ClawtAgent *agent;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    agent = clawt_agent_manager_get(self->agents, agent_id);

    if (agent == NULL ||
        clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_STOPPED)
        return FALSE;

    clawt_agent_stop(agent);
    clawt_event_bus_emit(self->bus, "agent.stopped", agent_id);

    /*
     * Its tasks are orphaned rather than left assigned.  A task whose
     * assignee is gone would sit in the list looking like work in
     * progress for ever.
     */
    clawt_task_manager_orphan_agent_tasks(self->tasks, agent_id);

    return TRUE;
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

/*
 * Quitting from an idle rather than inline, so the reply to
 * control.shutdown reaches the client before the socket closes.  Without
 * it a clean shutdown looks to the client exactly like a crash.
 */
static gboolean
clawt_daemon_quit_idle(gpointer user_data)
{
    clawt_daemon_quit(CLAWT_DAEMON(user_data));

    return G_SOURCE_REMOVE;
}

/*
 * Drops every component, in reverse dependency order.
 *
 * The order matters: the servers hold callbacks into the managers, and
 * the managers hold references to the config.  Freeing the config first
 * would leave a live server calling into it.
 */
static void
release_components(ClawtDaemon *self)
{
    g_clear_object(&self->plugins);
    g_clear_object(&self->mcp_tools);
    g_clear_object(&self->ipc_server);
    g_clear_object(&self->link_server);
    g_clear_object(&self->pod_bridge);
    g_clear_object(&self->router);
    g_clear_object(&self->guard);
    g_clear_object(&self->tasks);
    g_clear_object(&self->rooms);
    g_clear_object(&self->agents);
    g_clear_object(&self->exchange);
    g_clear_object(&self->log);
    g_clear_object(&self->bus);
    g_clear_object(&self->config);

    g_clear_pointer(&self->state_dir, g_free);
    g_clear_pointer(&self->link_socket, g_free);
}

static gboolean
on_sweep(gpointer user_data)
{
    ClawtDaemon *self = user_data;

    clawt_mailbox_router_sweep(self->router);
    clawt_event_log_sweep(self->log);

    return G_SOURCE_CONTINUE;
}

static JsonNode *
on_ipc_request(JsonNode *request, gpointer user_data)
{
    return clawt_daemon_handle_request(CLAWT_DAEMON(user_data), request);
}

static void
configure_limits(ClawtDaemon *self)
{
    clawt_loop_guard_set_limits(
        self->guard,
        (guint)clawt_config_get_int(self->config, "orchestration.max_hops"),
        (guint)clawt_config_get_int(self->config,
                                    "orchestration.rate_limit_per_minute"),
        (guint)clawt_config_get_int(self->config,
                                    "orchestration.cycle_window"));

    clawt_loop_guard_set_task_budget(
        self->guard,
        clawt_config_get_double(self->config, "orchestration.task_budget_usd"));

    clawt_task_manager_set_max_depth(
        self->tasks,
        (guint)clawt_config_get_int(self->config, "orchestration.max_hops"));
}

gboolean
clawt_daemon_start(ClawtDaemon *self, GError **error)
{
    g_autofree gchar *transcript_dir = NULL;
    g_autofree gchar *event_dir = NULL;
    g_autofree gchar *exchange_dir = NULL;
    GPtrArray *agents;
    guint i;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    if (self->running)
        return TRUE;

    /*
     * Anything left from a previous start is released first.
     *
     * start() used to overwrite every component pointer without freeing
     * what was there, so a stop-then-start -- or a retry after a start
     * that failed partway -- orphaned the whole previous generation.  A
     * standalone daemon exits and never notices; an embedding host that
     * restarts the fleet in-process leaks it every time.
     */
    release_components(self);

    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);

    self->config = clawt_config_load(self->config_path, error);

    if (self->config == NULL) {
        if (self->main_context != NULL)
            g_main_context_pop_thread_default(self->main_context);
        return FALSE;
    }

    /*
     * Warnings, not failures.  A config clawtilla partly disagrees with
     * should start the agents it does understand and say what it skipped:
     * refusing to start at all turns one bad line into an outage.
     */
    {
        GPtrArray *warnings = clawt_config_get_warnings(self->config);

        for (i = 0; warnings != NULL && i < warnings->len; i++)
            g_warning("config: %s", (const gchar *)
                      g_ptr_array_index(warnings, i));
    }

    self->state_dir = clawt_config_get_path_value(self->config,
                                                  "daemon.state_dir");

    if (self->state_dir == NULL)
        self->state_dir = clawt_expand_path("~/.clawtilla");

    if (!clawt_ensure_dir(self->state_dir, 0700, error)) {
        if (self->main_context != NULL)
            g_main_context_pop_thread_default(self->main_context);
        return FALSE;
    }

    self->link_socket = g_build_filename(self->state_dir, "agents.sock",
                                         NULL);

    /*
     * Now that the state directory is known, nothing may mount it.  It
     * holds every agent's token and resolved credentials; an agent that
     * could read it could read every other agent's mail and connect as
     * any of them.
     */
    {
        g_autofree gchar *secrets_dir =
            clawt_config_get_path_value(self->config, "secrets.dir");
        const gchar *forbidden[] = { self->state_dir, secrets_dir, NULL };

        clawt_mount_set_forbidden_sources(forbidden);
    }

    event_dir = g_build_filename(self->state_dir, "events", NULL);
    transcript_dir = g_build_filename(self->state_dir, "transcripts", NULL);

    self->bus = clawt_event_bus_new(1024);
    self->log = clawt_event_log_new(
        event_dir,
        (gint)clawt_config_get_int(self->config, "daemon.event_log_days"));
    clawt_event_log_attach(self->log, self->bus);

    exchange_dir = clawt_config_get_path_value(self->config,
                                               "defaults.exchange_dir");

    if (exchange_dir != NULL)
        self->exchange = clawt_exchange_new(
            exchange_dir,
            clawt_config_get_int(self->config, "defaults.exchange_max_bytes"));

    self->agents = clawt_agent_manager_new(self->config);
    clawt_agent_manager_set_state_dir(self->agents, self->state_dir);

    if (!clawt_agent_manager_load(self->agents, error)) {
        if (self->main_context != NULL)
            g_main_context_pop_thread_default(self->main_context);
        return FALSE;
    }

    self->rooms = clawt_room_manager_new(transcript_dir);
    clawt_room_manager_load(self->rooms, self->config);

    self->tasks = clawt_task_manager_new();
    self->guard = clawt_loop_guard_new();
    configure_limits(self);

    self->router = clawt_mailbox_router_new(self->agents, self->rooms,
                                            self->guard);
    clawt_mailbox_router_set_event_bus(self->router, self->bus);

    self->mcp_tools = clawt_mcp_tools_new(self->agents, self->tasks,
                                          self->guard);
    clawt_mcp_tools_set_deliver_func(self->mcp_tools, deliver_for_tools,
                                     self, NULL);
    clawt_mcp_tools_set_room_manager(self->mcp_tools, self->rooms);

    {
        /*
         * NULL means "search", which is what we want unless the config
         * names a directory.  The environment variable is read by the
         * bridge as part of that search, so it keeps working and keeps
         * its priority over the compiled-in location.
         */
        const gchar *module_dir =
            clawt_config_has_key(self->config, "daemon.pod_module_dir")
            ? clawt_config_get_string(self->config, "daemon.pod_module_dir")
            : NULL;

        self->pod_bridge = clawt_pod_bridge_new(module_dir);
    }

    /*
     * Plugins load before the listeners open, so a plugin that adds a
     * computer backend or a tool is in place before the first agent
     * connects and asks what it can do.
     */
    self->plugins = clawt_plugin_manager_new(self->config);
    clawt_plugin_manager_add_service(self->plugins, "agents",
                                     G_OBJECT(self->agents));
    clawt_plugin_manager_add_service(self->plugins, "rooms",
                                     G_OBJECT(self->rooms));
    clawt_plugin_manager_add_service(self->plugins, "tasks",
                                     G_OBJECT(self->tasks));
    clawt_plugin_manager_add_service(self->plugins, "router",
                                     G_OBJECT(self->router));
    clawt_plugin_manager_add_service(self->plugins, "events",
                                     G_OBJECT(self->bus));
    clawt_plugin_manager_add_service(self->plugins, "config",
                                     G_OBJECT(self->config));
    clawt_plugin_manager_load_all(self->plugins);
    clawt_plugin_manager_attach_bus(self->plugins, self->bus);

    {
        g_autoptr(GPtrArray) providers =
            clawt_plugin_manager_tool_providers(self->plugins);

        clawt_mcp_tools_set_tool_providers(self->mcp_tools, providers);
    }

    self->link_server = clawt_link_server_new(self->link_socket);
    clawt_link_server_set_auth_func(self->link_server, authenticate_agent,
                                    self, NULL);
    g_signal_connect(self->link_server, "link-added",
                     G_CALLBACK(on_link_added), self);
    g_signal_connect(self->link_server, "link-removed",
                     G_CALLBACK(on_link_removed), self);
    g_signal_connect(self->link_server, "message",
                     G_CALLBACK(on_link_message), self);
    g_signal_connect(self->link_server, "typing",
                     G_CALLBACK(on_link_typing), self);

    if (!clawt_link_server_start(self->link_server, error)) {
        if (self->main_context != NULL)
            g_main_context_pop_thread_default(self->main_context);
        return FALSE;
    }

    {
        g_autofree gchar *socket_path =
            clawt_config_get_path_value(self->config, "daemon.socket");

        self->ipc_server = clawt_ipc_server_new(
            socket_path != NULL ? socket_path
                                : clawt_client_default_socket_path());
    }

    clawt_ipc_server_set_handler(self->ipc_server, on_ipc_request, self,
                                 NULL);
    clawt_ipc_server_attach_bus(self->ipc_server, self->bus);

    if (clawt_config_get_boolean(self->config, "daemon.tcp_enabled")) {
        g_autofree gchar *token_file =
            clawt_config_get_path_value(self->config, "daemon.token_file");
        g_autofree gchar *token = NULL;

        if (token_file != NULL &&
            g_file_get_contents(token_file, &token, NULL, NULL))
            g_strstrip(token);

        clawt_ipc_server_set_tcp(
            self->ipc_server,
            clawt_config_get_string(self->config, "daemon.tcp_address"),
            (guint16)clawt_config_get_int(self->config, "daemon.tcp_port"),
            token);

        clawt_ipc_server_set_tls(
            self->ipc_server,
            clawt_config_get_string(self->config, "daemon.tls_cert"),
            clawt_config_get_string(self->config, "daemon.tls_key"));
    }

    if (!clawt_ipc_server_start(self->ipc_server, error)) {
        clawt_link_server_stop(self->link_server);

        if (self->main_context != NULL)
            g_main_context_pop_thread_default(self->main_context);
        return FALSE;
    }

    {
        GSource *source = g_timeout_source_new_seconds(SWEEP_INTERVAL_SECONDS);

        /*
         * Attached to the daemon's own context rather than added with
         * g_timeout_add_seconds(), which always uses the default context
         * -- so an embedded daemon's sweep would never run.
         */
        g_source_set_callback(source, on_sweep, self, NULL);
        self->sweep_source_id = g_source_attach(source, self->main_context);
        g_source_unref(source);
    }

    self->running = TRUE;

    render_all_agents(self);

    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        ClawtAgentConfig *config = clawt_agent_get_config(agent);
        g_autoptr(GError) local = NULL;

        if (clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_SHADOW)
            continue;

        if (!clawt_agent_config_get_boolean(config, "enabled"))
            continue;

        if (!clawt_agent_config_get_boolean(config, "runtime.autostart"))
            continue;

        if (!clawt_daemon_start_agent(self, clawt_agent_get_id(agent),
                                      &local))
            g_warning("agent %s did not start: %s",
                      clawt_agent_get_id(agent), local->message);
    }

    clawt_event_bus_emit(self->bus, "daemon.started", NULL);
    g_signal_emit(self, signals[SIGNAL_STARTED], 0);

    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);

    return TRUE;
}

void
clawt_daemon_stop(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (!self->running)
        return;

    self->running = FALSE;

    if (self->sweep_source_id != 0) {
        GSource *source = g_main_context_find_source_by_id(
            self->main_context, self->sweep_source_id);

        if (source != NULL)
            g_source_destroy(source);

        self->sweep_source_id = 0;
    }

    if (self->agents != NULL)
        clawt_agent_manager_stop_all(self->agents);

    if (self->rooms != NULL)
        clawt_room_manager_flush(self->rooms);

    if (self->ipc_server != NULL)
        clawt_ipc_server_stop(self->ipc_server);

    if (self->link_server != NULL)
        clawt_link_server_stop(self->link_server);

    if (self->bus != NULL)
        clawt_event_bus_emit(self->bus, "daemon.stopped", NULL);

    g_signal_emit(self, signals[SIGNAL_STOPPED], 0);
}

gint
clawt_daemon_run(ClawtDaemon *self)
{
    g_autoptr(GError) error = NULL;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), 1);

    if (!clawt_daemon_start(self, &error)) {
        g_printerr("clawtillad: %s\n", error->message);
        return 1;
    }

    self->loop = g_main_loop_new(self->main_context, FALSE);
    g_main_loop_run(self->loop);

    clawt_daemon_stop(self);

    g_clear_pointer(&self->loop, g_main_loop_unref);

    return 0;
}

void
clawt_daemon_quit(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->loop != NULL)
        g_main_loop_quit(self->loop);
}

gboolean
clawt_daemon_reload(ClawtDaemon *self, GError **error)
{
    g_autoptr(ClawtConfig) reloaded = NULL;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    /*
     * Parsed before anything is replaced.  Swapping in a config and then
     * discovering it is malformed would leave the daemon running on half
     * of each.
     */
    reloaded = clawt_config_load(self->config_path, error);

    if (reloaded == NULL)
        return FALSE;

    g_clear_object(&self->config);
    self->config = g_steal_pointer(&reloaded);

    configure_limits(self);

    /*
     * The manager holds its own reference, so it has to be pointed at the
     * new configuration and then reconciled.  Without both, a reload
     * changed the daemon's config while the fleet carried on reading the
     * old one -- an added agent never appeared, a removed one never went
     * away, and nothing a reload changed ever took effect.
     */
    clawt_agent_manager_set_config(self->agents, self->config);
    clawt_agent_manager_load(self->agents, NULL);

    clawt_room_manager_load(self->rooms, self->config);

    /*
     * Files are re-rendered for running agents too, so a restart picks up
     * the change -- but nothing is restarted here.  A reload that
     * interrupted every agent mid-turn would make editing one description
     * cost the whole fleet's work.
     */
    render_all_agents(self);

    clawt_event_bus_emit(self->bus, "daemon.reloaded", NULL);
    g_signal_emit(self, signals[SIGNAL_RELOADED], 0);

    return TRUE;
}


/* ── The client surface ──────────────────────────────────────────── */

static void
add_agent_object(JsonBuilder *builder, ClawtAgent *agent)
{
    ClawtAgentConfig *config = clawt_agent_get_config(agent);
    g_autofree gchar *caps = NULL;
    ClawtMailbox *mailbox;

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, clawt_agent_get_id(agent));

    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, clawt_agent_get_name(agent));

    if (clawt_agent_get_description(agent) != NULL) {
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder,
                                      clawt_agent_get_description(agent));
    }

    json_builder_set_member_name(builder, "state");
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                                    clawt_agent_get_state(agent)));

    if (clawt_agent_get_status_detail(agent) != NULL) {
        json_builder_set_member_name(builder, "detail");
        json_builder_add_string_value(builder,
                                      clawt_agent_get_status_detail(agent));
    }

    caps = clawt_flags_to_string(CLAWT_TYPE_AGENT_CAPS,
                                 clawt_agent_get_caps(agent));
    json_builder_set_member_name(builder, "caps");
    json_builder_add_string_value(builder, caps);

    json_builder_set_member_name(builder, "chief_of_staff");
    json_builder_add_boolean_value(builder,
                                   clawt_agent_is_chief_of_staff(agent));

    json_builder_set_member_name(builder, "connected");
    json_builder_add_boolean_value(
        builder, clawt_agent_get_link(agent) != NULL &&
                 clawt_link_is_open(clawt_agent_get_link(agent)));

    json_builder_set_member_name(builder, "computer");
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(
                     CLAWT_TYPE_COMPUTER_TYPE,
                     clawt_agent_config_get_enum(config, "computer.type")));

    json_builder_set_member_name(builder, "model");
    json_builder_add_string_value(
        builder, clawt_agent_config_get_string(config, "model.model"));

    /*
     * The settings a client offers for editing, so it can show what is
     * currently set rather than guessing at defaults.
     */
    json_builder_set_member_name(builder, "provider");
    json_builder_add_string_value(
        builder, clawt_agent_config_get_string(config, "model.provider"));

    json_builder_set_member_name(builder, "effort");
    json_builder_add_string_value(
        builder, clawt_agent_config_get_string(config, "model.effort"));

    json_builder_set_member_name(builder, "restart");
    json_builder_add_string_value(
        builder, clawt_agent_config_get_string(config, "runtime.restart"));

    json_builder_set_member_name(builder, "autostart");
    json_builder_add_boolean_value(
        builder, clawt_agent_config_get_boolean(config, "runtime.autostart"));

    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(
        builder, clawt_agent_config_get_boolean(config, "enabled"));

    mailbox = clawt_agent_get_mailbox(agent);
    json_builder_set_member_name(builder, "mailbox_depth");
    json_builder_add_int_value(builder, mailbox != NULL
                                        ? clawt_mailbox_depth(mailbox) : 0);

    /*
     * Credentials are named, never valued.  A client that displays an
     * agent should be able to say "a Matrix token is configured" without
     * the token itself crossing the socket or landing in a screenshot.
     */
    {
        g_autoptr(GHashTable) credentials =
            clawt_agent_config_get_credentials(config);
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        json_builder_set_member_name(builder, "credentials");
        json_builder_begin_object(builder);

        if (credentials != NULL) {
            g_hash_table_iter_init(&iter, credentials);

            while (g_hash_table_iter_next(&iter, &key, &value)) {
                g_autofree gchar *described =
                    clawt_secret_ref_describe(value);

                json_builder_set_member_name(builder, key);
                json_builder_add_string_value(builder, described);
            }
        }

        json_builder_end_object(builder);
    }

    json_builder_end_object(builder);
}

static void
add_task_object(JsonBuilder *builder, ClawtTask *task)
{
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, clawt_task_get_id(task));

    json_builder_set_member_name(builder, "origin");
    json_builder_add_string_value(builder, clawt_task_get_origin(task));

    json_builder_set_member_name(builder, "assignee");
    json_builder_add_string_value(builder, clawt_task_get_assignee(task));

    json_builder_set_member_name(builder, "state");
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE,
                                    clawt_task_get_state(task)));

    json_builder_set_member_name(builder, "prompt");
    json_builder_add_string_value(builder, clawt_task_get_prompt(task));

    if (clawt_task_get_result(task) != NULL) {
        json_builder_set_member_name(builder, "result");
        json_builder_add_string_value(builder, clawt_task_get_result(task));
    }

    if (clawt_task_get_reason(task) != NULL) {
        json_builder_set_member_name(builder, "reason");
        json_builder_add_string_value(builder, clawt_task_get_reason(task));
    }

    json_builder_set_member_name(builder, "depth");
    json_builder_add_int_value(builder, clawt_task_get_depth(task));

    json_builder_end_object(builder);
}

static void
add_mailbox_item(JsonBuilder *builder, ClawtMailboxItem *item)
{
    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, clawt_mailbox_item_get_id(item));

    json_builder_set_member_name(builder, "from");
    json_builder_add_string_value(builder, clawt_mailbox_item_get_from(item));

    json_builder_set_member_name(builder, "to");
    json_builder_add_string_value(builder, clawt_mailbox_item_get_to(item));

    json_builder_set_member_name(builder, "body");
    json_builder_add_string_value(builder, clawt_mailbox_item_get_body(item));

    if (clawt_mailbox_item_get_room(item) != NULL) {
        json_builder_set_member_name(builder, "room");
        json_builder_add_string_value(builder,
                                      clawt_mailbox_item_get_room(item));
    }

    json_builder_set_member_name(builder, "priority");
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(CLAWT_TYPE_PRIORITY,
                                    clawt_mailbox_item_get_priority(item)));

    json_builder_set_member_name(builder, "state");
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(CLAWT_TYPE_MAILBOX_STATE,
                                    clawt_mailbox_item_get_state(item)));

    json_builder_set_member_name(builder, "attempts");
    json_builder_add_int_value(builder,
                               clawt_mailbox_item_get_attempts(item));

    json_builder_set_member_name(builder, "created_at");
    json_builder_add_int_value(builder,
                               clawt_mailbox_item_get_created_at(item));

    if (clawt_mailbox_item_get_last_error(item) != NULL) {
        json_builder_set_member_name(builder, "last_error");
        json_builder_add_string_value(
            builder, clawt_mailbox_item_get_last_error(item));
    }

    json_builder_end_object(builder);
}

static ClawtMailbox *
mailbox_for(ClawtDaemon *self, JsonObject *payload, GError **error)
{
    const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
    ClawtAgent *agent;

    if (agent_id == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "which agent's mailbox?");
        return NULL;
    }

    agent = clawt_agent_manager_get(self->agents, agent_id);

    if (agent == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no agent called '%s'", agent_id);
        return NULL;
    }

    if (clawt_agent_get_mailbox(agent) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "%s has no mailbox", agent_id);
        return NULL;
    }

    return clawt_agent_get_mailbox(agent);
}

JsonNode *
clawt_daemon_handle_request(ClawtDaemon *self, JsonNode *request)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GError) error = NULL;
    JsonObject *payload;
    const gchar *kind;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    g_return_val_if_fail(request != NULL, NULL);

    kind = clawt_ipc_frame_get_kind(request);
    payload = clawt_ipc_frame_get_payload(request);
    builder = json_builder_new();

    if (!self->running)
        return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                   "the daemon is not running");

    /* ── control ── */

    if (g_strcmp0(kind, "control.status") == 0) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "version");
        json_builder_add_string_value(builder, CLAWT_VERSION_STRING);
        json_builder_set_member_name(builder, "config");
        json_builder_add_string_value(builder, self->config_path);
        json_builder_set_member_name(builder, "agents");
        json_builder_add_int_value(
            builder, clawt_agent_manager_list(self->agents)->len);
        json_builder_set_member_name(builder, "connected");
        json_builder_add_int_value(
            builder, clawt_link_server_count_links(self->link_server));
        json_builder_set_member_name(builder, "clients");
        json_builder_add_int_value(
            builder, clawt_ipc_server_count_clients(self->ipc_server));
        json_builder_set_member_name(builder, "cursor");
        json_builder_add_int_value(
            builder, (gint64)clawt_event_bus_get_cursor(self->bus));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "control.reload") == 0) {
        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "control.shutdown") == 0) {
        JsonNode *reply = clawt_ipc_response_new(request, NULL);

        /*
         * Answered first, then queued.  Quitting the loop inside this
         * call would close the socket before the reply reached the client,
         * which looks to them like the daemon crashed.
         */
        g_idle_add((GSourceFunc)clawt_daemon_quit_idle, self);

        return reply;
    }

    /* ── agents ── */

    if (g_strcmp0(kind, "agent.list") == 0) {
        GPtrArray *agents = clawt_agent_manager_list(self->agents);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agents");
        json_builder_begin_array(builder);

        for (i = 0; i < agents->len; i++)
            add_agent_object(builder, g_ptr_array_index(agents, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.show") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtComputer *computer;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agent");
        add_agent_object(builder, agent);

        computer = clawt_agent_get_computer(agent);

        if (computer != NULL) {
            g_autofree gchar *described = clawt_computer_describe(computer);

            json_builder_set_member_name(builder, "computer_detail");
            json_builder_add_string_value(builder, described);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.start") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

        if (!clawt_daemon_start_agent(self, agent_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "agent.stop") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "stopped");
        json_builder_add_boolean_value(
            builder, clawt_daemon_stop_agent(self, agent_id));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.restart") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

        clawt_daemon_stop_agent(self, agent_id);

        if (!clawt_daemon_start_agent(self, agent_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "agent.logs") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtAgentRuntime *runtime;
        g_auto(GStrv) lines = NULL;
        gsize i;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        runtime = clawt_agent_get_runtime(agent);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "lines");
        json_builder_begin_array(builder);

        if (runtime != NULL) {
            lines = clawt_agent_runtime_get_log_tail(
                runtime,
                (guint)clawt_ipc_payload_int(payload, "limit", 200));

            for (i = 0; lines != NULL && lines[i] != NULL; i++)
                json_builder_add_string_value(builder, lines[i]);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.create") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "id");
        ClawtAgentConfig *created;

        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "an agent needs an id");

        created = clawt_config_add_agent(self->config, agent_id, &error);

        if (created == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        {
            static const struct {
                const gchar *from;
                const gchar *to;
            } fields[] = {
                { "name",        "name" },
                { "description", "description" },
                { "model",       "model.model" },
                { "provider",    "model.provider" },
                { "computer",    "computer.type" },
                { "confine",     "computer.host.confine" },
                { "workspace",   "workspace" },
                { NULL, NULL }
            };
            gsize i;

            for (i = 0; fields[i].from != NULL; i++) {
                const gchar *value = clawt_ipc_payload_string(payload,
                                                              fields[i].from);

                if (value != NULL)
                    clawt_agent_config_set_string(created, fields[i].to,
                                                  value);
            }
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * Reloaded so the new agent exists as an object, not merely as a
         * line in a file: the client that created it will immediately ask
         * to start it.
         */
        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_agent_manager_load(self->agents, NULL);
        clawt_event_bus_emit(self->bus, "agent.created", agent_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, agent_id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.remove") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");

        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which agent?");

        clawt_daemon_stop_agent(self, agent_id);

        if (!clawt_config_remove_agent(self->config, agent_id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The agent's state directory -- its mailbox, its transcripts, its
         * rendered config -- is deliberately left on disk.  Removing an
         * agent from the fleet is reversible; deleting its history is not.
         */
        clawt_agent_manager_load(self->agents, NULL);
        clawt_event_bus_emit(self->bus, "agent.removed", agent_id);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "agent.set") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *key = clawt_ipc_payload_string(payload, "key");
        const gchar *value = clawt_ipc_payload_string(payload, "value");
        ClawtAgentConfig *config;

        if (agent_id == NULL || key == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and key are both required");

        config = clawt_config_get_agent(self->config, agent_id);

        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        clawt_agent_config_set_string(config, key, value);

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "agent.changed", agent_id);

        return clawt_ipc_response_new(request, NULL);
    }

    /* ── messages and rooms ── */

    if (g_strcmp0(kind, "msg.send") == 0) {
        const gchar *target = clawt_ipc_payload_string(payload, "target");
        const gchar *body = clawt_ipc_payload_string(payload, "body");
        const gchar *from = clawt_ipc_payload_string(payload, "from");
        gint queued;

        if (target == NULL || body == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "target and body are both required");

        queued = clawt_mailbox_router_send_to(self->router,
                                              from != NULL ? from : "user",
                                              target, body, NULL, 0, &error);

        if (queued < 0)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "queued");
        json_builder_add_int_value(builder, queued);

        /*
         * Whether anything is going to read it.  A mailbox accepts a
         * message for a stopped agent by design -- that is the point of
         * making it durable -- but a client that cannot tell "queued" from
         * "delivered" leaves the user watching a spinner for an agent that
         * is not running and never will answer.  Reported only for a
         * single agent; for a room the members each have their own state
         * and the client can ask for them.
         */
        {
            ClawtAgent *agent = clawt_agent_manager_get(self->agents, target);

            if (agent != NULL) {
                json_builder_set_member_name(builder, "target_state");
                json_builder_add_string_value(
                    builder, clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE,
                                                clawt_agent_get_state(agent)));
            }
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "room.list") == 0) {
        g_autoptr(GPtrArray) rooms = clawt_room_manager_list(self->rooms);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "rooms");
        json_builder_begin_array(builder);

        for (i = 0; i < rooms->len; i++) {
            ClawtRoom *room = g_ptr_array_index(rooms, i);
            GPtrArray *members = clawt_room_get_members(room);
            guint j;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, clawt_room_get_id(room));
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, clawt_room_get_name(room));
            json_builder_set_member_name(builder, "members");
            json_builder_begin_array(builder);

            for (j = 0; j < members->len; j++)
                json_builder_add_string_value(
                    builder, g_ptr_array_index(members, j));

            json_builder_end_array(builder);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "room.create") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *members = clawt_ipc_payload_string(payload, "members");
        ClawtRoom *room;

        room = clawt_room_manager_create(self->rooms, room_id, name, &error);

        if (room == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (members != NULL) {
            g_auto(GStrv) parts = g_strsplit(members, ",", -1);
            gsize i;

            for (i = 0; parts[i] != NULL; i++)
                clawt_room_add_member(room, g_strstrip(parts[i]));
        }

        clawt_event_bus_emit(self->bus, "room.created", room_id);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "room.add") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtRoom *room = clawt_room_manager_get(self->rooms, room_id);

        if (room == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such room");

        /*
         * Checked rather than passed straight through: without it a
         * request with no agent named added nobody and reported success,
         * which is the one answer a caller cannot act on.
         */
        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which agent should be added?");

        clawt_room_add_member(room, agent_id);
        clawt_event_bus_emit(self->bus, "room.changed", room_id);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "room.history") == 0) {
        const gchar *room_id = clawt_ipc_payload_string(payload, "room");
        const gchar *viewer = clawt_ipc_payload_string(payload, "as");
        ClawtRoom *room = clawt_room_manager_get(self->rooms, room_id);
        g_autoptr(GPtrArray) history = NULL;
        guint i;

        /*
         * An agent id means the direct room with that agent, the same way
         * it does for msg.send.  Without this a client showing a
         * conversation had to know how a direct room is named -- and the
         * GTK client did not, so every chat opened empty with a "no such
         * room" error behind it.
         */
        if (room == NULL && room_id != NULL &&
            clawt_agent_manager_get(self->agents, room_id) != NULL)
            room = clawt_room_manager_get_direct(
                self->rooms, viewer != NULL ? viewer : "user", room_id);

        if (room == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such room or agent");

        history = clawt_room_get_history(
            room, (guint)clawt_ipc_payload_int(payload, "limit", 50));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "messages");
        json_builder_begin_array(builder);

        for (i = 0; i < history->len; i++) {
            ClawtMessage *message = g_ptr_array_index(history, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder,
                                          clawt_message_get_id(message));
            json_builder_set_member_name(builder, "sender");
            json_builder_add_string_value(
                builder, clawt_message_get_sender_id(message));
            json_builder_set_member_name(builder, "body");
            json_builder_add_string_value(builder,
                                          clawt_message_get_body(message));
            json_builder_set_member_name(builder, "ts");
            json_builder_add_int_value(
                builder, clawt_message_get_timestamp(message));
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── mailboxes ── */

    if (g_strcmp0(kind, "mailbox.list") == 0 ||
        g_strcmp0(kind, "mailbox.dead") == 0) {
        ClawtMailbox *mailbox = mailbox_for(self, payload, &error);
        g_autoptr(GPtrArray) items = NULL;
        guint i;

        if (mailbox == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (g_strcmp0(kind, "mailbox.dead") == 0) {
            items = clawt_mailbox_dead_letters(mailbox);
        } else {
            ClawtMailboxFilter filter = { CLAWT_MAILBOX_PENDING, 50, TRUE };

            filter.limit = (guint)clawt_ipc_payload_int(payload, "limit", 50);
            items = clawt_mailbox_list(mailbox, &filter);
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "items");
        json_builder_begin_array(builder);

        for (i = 0; i < items->len; i++)
            add_mailbox_item(builder, g_ptr_array_index(items, i));

        json_builder_end_array(builder);
        json_builder_set_member_name(builder, "depth");
        json_builder_add_int_value(builder, clawt_mailbox_depth(mailbox));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "mailbox.ack") == 0 ||
        g_strcmp0(kind, "mailbox.requeue") == 0) {
        ClawtMailbox *mailbox = mailbox_for(self, payload, &error);
        const gchar *item_id = clawt_ipc_payload_string(payload, "item");
        gboolean ok;

        if (mailbox == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (item_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which message?");

        ok = (g_strcmp0(kind, "mailbox.ack") == 0)
             ? clawt_mailbox_ack(mailbox, item_id, &error)
             : clawt_mailbox_requeue(mailbox, item_id, &error);

        if (!ok)
            return clawt_ipc_error_new(request, error->code, error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "mailbox.purge") == 0) {
        ClawtMailbox *mailbox = mailbox_for(self, payload, &error);

        if (mailbox == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "purged");
        json_builder_add_int_value(builder,
                                   clawt_mailbox_purge_expired(mailbox));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── tasks ── */

    if (g_strcmp0(kind, "task.list") == 0) {
        g_autoptr(GPtrArray) tasks = NULL;
        guint i;

        tasks = clawt_task_manager_list(
            self->tasks, clawt_ipc_payload_string(payload, "agent"),
            clawt_ipc_payload_boolean(payload, "all", TRUE));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "tasks");
        json_builder_begin_array(builder);

        for (i = 0; i < tasks->len; i++)
            add_task_object(builder, g_ptr_array_index(tasks, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "task.show") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        ClawtTask *task = (task_id != NULL)
                          ? clawt_task_manager_get(self->tasks, task_id)
                          : NULL;

        if (task == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such task");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        add_task_object(builder, task);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "task.cancel") == 0) {
        const gchar *task_id = clawt_ipc_payload_string(payload, "task");
        guint cancelled;

        if (task_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which task?");

        cancelled = clawt_task_manager_cancel(self->tasks, task_id,
                                              "cancelled from a client");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "cancelled");
        json_builder_add_int_value(builder, cancelled);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── computers ── */

    if (g_strcmp0(kind, "computer.exec") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *command = clawt_ipc_payload_string(payload, "command");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtComputer *computer = (agent != NULL)
                                  ? clawt_agent_get_computer(agent) : NULL;
        g_autoptr(ClawtExecResult) result = NULL;
        g_auto(GStrv) argv = NULL;

        if (computer == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "that agent has no computer");

        if (command == NULL || !g_shell_parse_argv(command, NULL, &argv,
                                                   &error))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                error != NULL ? error->message : "no command given");

        result = clawt_computer_exec(
            computer, (const gchar * const *)argv,
            clawt_ipc_payload_string(payload, "cwd"),
            (guint)clawt_ipc_payload_int(payload, "timeout", 120), NULL,
            &error);

        if (result == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * Every host command is recorded, whoever asked for it.  Running
         * something on the machine is the most consequential thing this
         * socket can do, and an audit trail that only covers agents would
         * miss exactly the case a person would want to look up.
         */
        {
            ClawtEvent *event = clawt_event_new("computer.exec", agent_id);

            clawt_event_set_detail(event, "command", command);
            clawt_event_set_detail_int(
                event, "exit", clawt_exec_result_get_exit_status(result));
            clawt_event_bus_publish(self->bus, event);
            clawt_event_free(event);
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "exit");
        json_builder_add_int_value(
            builder, clawt_exec_result_get_exit_status(result));
        json_builder_set_member_name(builder, "stdout");
        json_builder_add_string_value(
            builder, clawt_exec_result_get_stdout(result));
        json_builder_set_member_name(builder, "stderr");
        json_builder_add_string_value(
            builder, clawt_exec_result_get_stderr(result));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.status") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtComputer *computer = (agent != NULL)
                                  ? clawt_agent_get_computer(agent) : NULL;
        g_autofree gchar *described = NULL;

        if (computer == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "that agent has no computer");

        described = clawt_computer_describe(computer);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "state");
        json_builder_add_string_value(
            builder, clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_STATE,
                                        clawt_computer_get_state(computer)));
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder, described);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "computer.copy") == 0) {
        const gchar *src = clawt_ipc_payload_string(payload, "src");
        const gchar *dst = clawt_ipc_payload_string(payload, "dst");
        g_auto(GStrv) src_parts = NULL;
        g_auto(GStrv) dst_parts = NULL;
        ClawtAgent *agent;
        ClawtComputer *computer;
        gboolean ok;

        if (src == NULL || dst == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "src and dst are both required");

        /*
         * Exactly one side may name an agent.  Copying between two agents
         * would need a temporary file on the host and a policy about who
         * owns it; the exchange directory already solves that case, and
         * saying so is better than half-implementing it.
         */
        src_parts = g_strsplit(src, ":", 2);
        dst_parts = g_strsplit(dst, ":", 2);

        if (g_strv_length(src_parts) == 2 && g_strv_length(dst_parts) == 2)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                "copying straight between two agents is not supported; "
                "copy through the exchange directory instead");

        /*
         * A copy into the exchange goes through the exchange's own rule,
         * which is what says an agent may write to shared/ and its own
         * directory and nowhere else.  Skipping it -- as this used to --
         * let any agent overwrite another's drop-box, the exact thing the
         * rule exists to prevent.
         */
        if (self->exchange != NULL && g_strv_length(dst_parts) == 2 &&
            g_str_has_prefix(dst_parts[1], CLAWT_EXCHANGE_MOUNT_POINT)) {
            g_autofree gchar *resolved =
                clawt_exchange_resolve(self->exchange, dst_parts[0],
                                       dst_parts[1], TRUE, &error);

            if (resolved == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        }

        if (g_strv_length(src_parts) == 2) {
            agent = clawt_agent_manager_get(self->agents, src_parts[0]);
            computer = (agent != NULL) ? clawt_agent_get_computer(agent)
                                       : NULL;

            if (computer == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "that agent has no computer");

            ok = clawt_computer_get_file(computer, src_parts[1], dst, &error);
        } else if (g_strv_length(dst_parts) == 2) {
            agent = clawt_agent_manager_get(self->agents, dst_parts[0]);
            computer = (agent != NULL) ? clawt_agent_get_computer(agent)
                                       : NULL;

            if (computer == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "that agent has no computer");

            ok = clawt_computer_put_file(computer, src, dst_parts[1], &error);
        } else {
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "one side must be <agent>:<path>");
        }

        if (!ok)
            return clawt_ipc_error_new(request, error->code, error->message);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "exchange.list") == 0) {
        g_autoptr(GPtrArray) entries = NULL;
        guint i;

        if (self->exchange == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this fleet has no exchange "
                                       "directory");

        entries = clawt_exchange_list(self->exchange,
                                      clawt_ipc_payload_string(payload,
                                                               "path"));

        if (entries == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such directory in the exchange");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "entries");
        json_builder_begin_array(builder);

        for (i = 0; i < entries->len; i++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(entries, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "design.agent") == 0) {
        const gchar *description = clawt_ipc_payload_string(payload,
                                                            "description");
        g_autoptr(ClawtAgentDesigner) designer = NULL;
        g_autofree gchar *preview = NULL;
        GHashTable *draft;

        if (description == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "describe what the agent should do");

        designer = clawt_agent_designer_new(self->config);

        if (!clawt_agent_designer_use_configured_provider(designer, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        draft = clawt_agent_designer_design(designer, description, NULL,
                                            &error);

        if (draft == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        preview = clawt_agent_designer_preview(designer);

        /*
         * Committed here only when asked for.  A design that wrote itself
         * into the config would mean the model's last word created
         * something nobody reviewed.
         */
        if (clawt_ipc_payload_boolean(payload, "commit", FALSE)) {
            if (clawt_agent_designer_commit(designer, &error) == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);

            if (!clawt_config_save(self->config, &error))
                return clawt_ipc_error_new(request, error->code,
                                           error->message);

            clawt_agent_manager_load(self->agents, NULL);
            render_all_agents(self);
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "yaml");
        json_builder_add_string_value(builder, preview);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder,
                                      g_hash_table_lookup(draft, "id"));
        json_builder_set_member_name(builder, "committed");
        json_builder_add_boolean_value(
            builder, clawt_ipc_payload_boolean(payload, "commit", FALSE));
        json_builder_set_member_name(builder, "notes");
        json_builder_add_string_value(
            builder, clawt_agent_designer_get_transcript(designer));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "model.list") == 0) {
        const ClawtProviderInfo *catalog;
        gsize n_providers = 0;
        gsize i;

        catalog = clawt_model_catalog_get(&n_providers);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "providers");
        json_builder_begin_array(builder);

        for (i = 0; i < n_providers; i++) {
            gsize j;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, catalog[i].id);
            json_builder_set_member_name(builder, "label");
            json_builder_add_string_value(builder, catalog[i].label);

            if (catalog[i].note != NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder, catalog[i].note);
            }

            /*
             * Passed on so a client knows to offer a way to type a name
             * that is not listed.  The catalogue is curated and goes
             * stale; nothing validates against it.
             */
            json_builder_set_member_name(builder, "open_ended");
            json_builder_add_boolean_value(builder, catalog[i].open_ended);

            json_builder_set_member_name(builder, "models");
            json_builder_begin_array(builder);

            for (j = 0; j < catalog[i].n_models; j++) {
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder,
                                              catalog[i].models[j].id);
                json_builder_set_member_name(builder, "label");
                json_builder_add_string_value(builder,
                                              catalog[i].models[j].label);

                if (catalog[i].models[j].note != NULL) {
                    json_builder_set_member_name(builder, "note");
                    json_builder_add_string_value(
                        builder, catalog[i].models[j].note);
                }

                json_builder_end_object(builder);
            }

            json_builder_end_array(builder);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "integration.list") == 0) {
        const ClawtIntegrationInfo *info;
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        gsize n_integrations = 0;
        gsize i;

        info = clawt_integration_list(&n_integrations);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "integrations");
        json_builder_begin_array(builder);

        for (i = 0; i < n_integrations; i++) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, info[i].id);
            json_builder_set_member_name(builder, "summary");
            json_builder_add_string_value(builder, info[i].summary);
            json_builder_set_member_name(builder, "enabled");
            json_builder_add_boolean_value(
                builder, agent_config != NULL &&
                         clawt_integration_is_enabled(agent_config,
                                                      info[i].id));
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "integration.health") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *integration = clawt_ipc_payload_string(payload,
                                                            "integration");
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_auto(GStrv) enabled = NULL;
        gsize i;

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (integration != NULL) {
            enabled = g_new0(gchar *, 2);
            enabled[0] = g_strdup(integration);
        } else {
            enabled = clawt_integration_enabled_for(agent_config);
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "checks");
        json_builder_begin_array(builder);

        for (i = 0; enabled[i] != NULL; i++) {
            g_autoptr(GError) check_error = NULL;
            gboolean ok;

            ok = clawt_integration_health_check(
                agent_config, enabled[i],
                (guint)clawt_ipc_payload_int(payload, "timeout", 10),
                &check_error);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, enabled[i]);
            json_builder_set_member_name(builder, "ok");
            json_builder_add_boolean_value(builder, ok);

            if (!ok) {
                json_builder_set_member_name(builder, "error");
                json_builder_add_string_value(builder,
                                              check_error->message);
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "plugin.list") == 0) {
        g_autoptr(GPtrArray) plugins = NULL;
        guint i;

        plugins = clawt_plugin_manager_list(self->plugins);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "plugins");
        json_builder_begin_array(builder);

        for (i = 0; i < plugins->len; i++) {
            ClawtPlugin *plugin = g_ptr_array_index(plugins, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder,
                                          clawt_plugin_get_id(plugin));
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder,
                                          clawt_plugin_get_name(plugin));
            json_builder_set_member_name(builder, "version");
            json_builder_add_string_value(builder,
                                          clawt_plugin_get_version(plugin));
            json_builder_set_member_name(builder, "description");
            json_builder_add_string_value(
                builder, clawt_plugin_get_description(plugin));
            json_builder_set_member_name(builder, "active");
            json_builder_add_boolean_value(builder,
                                           clawt_plugin_is_active(plugin));
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── config ── */

    if (g_strcmp0(kind, "config.show") == 0) {
        g_autofree gchar *text = clawt_config_to_string(self->config);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "yaml");
        json_builder_add_string_value(builder, text);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "config.render") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgentConfig *config = (agent_id != NULL)
                                   ? clawt_config_get_agent(self->config,
                                                            agent_id)
                                   : NULL;
        g_autofree gchar *rendered = NULL;
        g_autofree gchar *state_dir = NULL;

        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        state_dir = clawt_config_agent_state_dir(self->config, agent_id);
        rendered = clawt_config_render_agent(self->config, config,
                                             self->link_socket, state_dir,
                                             &error);

        if (rendered == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "yaml");
        json_builder_add_string_value(builder, rendered);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "config.validate") == 0) {
        GPtrArray *warnings;
        guint i;

        clawt_config_validate(self->config, &error);
        warnings = clawt_config_get_warnings(self->config);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "valid");
        json_builder_add_boolean_value(builder, error == NULL);

        if (error != NULL) {
            json_builder_set_member_name(builder, "error");
            json_builder_add_string_value(builder, error->message);
        }

        json_builder_set_member_name(builder, "warnings");
        json_builder_begin_array(builder);

        for (i = 0; warnings != NULL && i < warnings->len; i++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(warnings, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * An unknown kind names itself in the reply.  A client from a newer
     * build asking for something this daemon does not have should learn
     * that, not merely that "something went wrong".
     */
    {
        g_autofree gchar *message = g_strdup_printf(
            "this daemon does not understand '%s'",
            kind != NULL ? kind : "(none)");

        return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                   message);
    }
}

static void
clawt_daemon_dispose(GObject *object)
{
    ClawtDaemon *self = CLAWT_DAEMON(object);

    clawt_daemon_stop(self);
    release_components(self);

    G_OBJECT_CLASS(clawt_daemon_parent_class)->dispose(object);
}

static void
clawt_daemon_finalize(GObject *object)
{
    ClawtDaemon *self = CLAWT_DAEMON(object);

    g_free(self->config_path);
    g_free(self->libreclaw_binary);
    g_free(self->state_dir);
    g_free(self->link_socket);
    g_clear_pointer(&self->main_context, g_main_context_unref);

    G_OBJECT_CLASS(clawt_daemon_parent_class)->finalize(object);
}

static void
clawt_daemon_class_init(ClawtDaemonClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_daemon_dispose;
    object_class->finalize = clawt_daemon_finalize;

    signals[SIGNAL_STARTED] =
        g_signal_new("started", CLAWT_TYPE_DAEMON, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 0);

    signals[SIGNAL_STOPPED] =
        g_signal_new("stopped", CLAWT_TYPE_DAEMON, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 0);

    signals[SIGNAL_RELOADED] =
        g_signal_new("reloaded", CLAWT_TYPE_DAEMON, G_SIGNAL_RUN_LAST, 0,
                     NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
clawt_daemon_init(ClawtDaemon *self)
{
    self->running = FALSE;
}
