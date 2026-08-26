/*
 * clawt-daemon.c - The fleet, assembled
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <errno.h>
#include "core/clawt-daemon.h"

#include <string.h>

/* How often expired mailbox items and abandoned leases are cleaned up. */
#define SWEEP_INTERVAL_SECONDS 60

/*
 * One `--bind` from the command line, already parsed.
 *
 * Parsed at the point the person typed it rather than carried as text to
 * daemon start, so `clawtillad --bind nonsense` is refused immediately
 * with the word that was wrong, instead of after the state directory and
 * every agent workspace have been written.
 */
typedef struct {
    gchar   *host;
    guint16  port;
} BindSpec;

struct _ClawtDaemon {
    GObject parent_instance;

    /*
     * Set by clawt_daemon_set_bind_addresses(), which replaces whatever
     * the config says about network listeners.  The flag is separate from
     * the list because "bind nothing" and "bind whatever is configured"
     * are different answers and an empty list has to mean the first.
     */
    gboolean   bind_override;
    GPtrArray *bind_specs;   /* BindSpec*, owned */

    gchar        *config_path;
    GMainContext *main_context;
    GMainLoop    *loop;

    ClawtConfig        *config;
    ClawtAgentManager  *agents;
    ClawtRoomManager   *rooms;
    ClawtTaskManager   *tasks;
    ClawtMailboxRouter *router;
    ClawtLoopGuard     *guard;
    ClawtUsage         *usage;
    ClawtEventBus      *bus;
    ClawtEventLog      *log;

    /*
     * Choices agents need a person to make.
     *
     * Beside the alerts rather than inside them: an alert is something
     * that happened and a decision is something that needs you, so one
     * badge meaning both would be a badge nobody could act on.  Durable
     * for the same reason the mailbox is -- an agent that asked and got
     * no answer carried on with its default, and an operator who never
     * saw the question has no way to know that happened.
     */
    ClawtDecisionStore *decisions;
    ClawtExchange      *exchange;
    ClawtLinkServer    *link_server;
    ClawtIpcServer     *ipc_server;
    ClawtMcpTools      *mcp_tools;
    ClawtPodBridge     *pod_bridge;
    ClawtPluginManager *plugins;
    ClawtVmImageStore  *vm_images;

    /*
     * The connector catalogue, and the authorizations in progress.
     *
     * The catalogue is cached because it is read on paths a person is
     * waiting on; it is dropped on reload so that editing a file in
     * connectors.dir takes effect without a restart.
     */
    GPtrArray  *connector_catalog;
    GHashTable *connector_flows;   /* flow id -> ConnectorFlow */
    GSource    *connector_refresh;

    /*
     * Designs waiting to be reviewed.
     *
     * design.agent used to run the model, show a preview, and then --
     * when the person said yes -- run the model *again* with commit set.
     * The second run is a fresh conversation, so what was created was not
     * what was reviewed, which is the one thing the preview exists to
     * guarantee. The designer is kept here between the two steps instead.
     */
    GHashTable *drafts;          /* draft id -> ClawtAgentDesigner */

    /*
     * What each provider says it runs, cached.
     *
     * model.list used to ask the providers while the client waited, and
     * both the new-agent dialog and the agent inspector ask on every
     * build -- so pressing + or clicking an agent stalled for as long as
     * the slowest provider took. Warmed in the background instead, and
     * every request answers from here at once.
     */
    GHashTable *model_cache;     /* provider id -> GStrv (owned) */
    gint64      model_cache_at;  /* monotonic, when it was last warmed */

    /*
     * Who to tell when something is worth interrupting somebody for.
     *
     * Rebuilt on every reload, because that is when its credentials are
     * resolved -- see ClawtNotifier.
     */
    ClawtNotifier *notifier;

    /* Standing work, and when it is next due. */
    ClawtRoutineRunner *routines;

    /* Pods that watch the fleet and act on it. */
    ClawtAutomation *automation;

    gchar   *libreclaw_binary;
    gchar   *state_dir;
    gchar   *link_socket;

    /*
     * Where a file an agent sent its operator is kept, so `attachment.get`
     * can serve the bytes to a client that may be on another machine.
     */
    gchar   *attachment_dir;
    guint    sweep_source_id;
    gboolean running;
};

/*
 * How many designs may sit unreviewed at once.  Small on purpose: a
 * draft is a step in a conversation somebody is having right now, not
 * something to accumulate.
 */
#define MAX_PENDING_DRAFTS (8)

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

ClawtDecisionStore *
clawt_daemon_get_decisions(ClawtDaemon *self)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

    return self->decisions;
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

/* ── The state directory as a repository ─────────────────────────── */

/*
 * The patterns clawtilla insists on, between markers.
 *
 * A marked block rather than a whole file, because this is written on
 * every start and people edit .gitignore. Everything outside the
 * markers is left exactly as it was found; everything inside is ours to
 * keep current. It is the same shape shell installers use on rc files,
 * and for the same reason.
 */
#define GIT_BLOCK_BEGIN "# >>> clawtilla >>>"
#define GIT_BLOCK_END   "# <<< clawtilla <<<"

static const gchar GIT_BLOCK[] =
    GIT_BLOCK_BEGIN "\n"
    "# Everything below is a secret, a database, or a socket. None of it\n"
    "# belongs in history. Edit outside these markers; clawtilla rewrites\n"
    "# what is between them on every start.\n"
    "\n"
    "credentials/\n"
    "secrets/\n"
    "*/token\n"
    "agents/*/token\n"
    "agents/*/credentials/\n"
    "\n"
    "*.db\n"
    "*.db-wal\n"
    "*.db-shm\n"
    "agents/*/sessions/\n"
    "agents/*.discarded/\n"
    "sessions.reset-*/\n"
    "\n"
    "*.sock\n"
    "events/\n"
    "exchange/\n"
    "\n"
    "# Regenerated on every start from clawtilla.yaml.\n"
    "agents/*/config.yaml\n"
    GIT_BLOCK_END "\n";

/*
 * Replaces clawtilla's block in an existing .gitignore, or appends it.
 *
 * Returns: (transfer full): the file's new contents
 */
static gchar *
merge_gitignore(const gchar *existing)
{
    const gchar *begin;
    const gchar *end;

    if (existing == NULL || existing[0] == '\0')
        return g_strdup(GIT_BLOCK);

    begin = strstr(existing, GIT_BLOCK_BEGIN);
    end = (begin != NULL) ? strstr(begin, GIT_BLOCK_END) : NULL;

    if (begin == NULL || end == NULL) {
        /* Theirs, then ours, with a blank line between. */
        return g_strconcat(existing,
                           g_str_has_suffix(existing, "\n") ? "" : "\n",
                           "\n", GIT_BLOCK, NULL);
    }

    {
        g_autofree gchar *before = g_strndup(existing,
                                             (gsize)(begin - existing));
        const gchar *after = end + strlen(GIT_BLOCK_END);

        /* Skip the newline that closed the old block. */
        if (after[0] == '\n')
            after++;

        return g_strconcat(before, GIT_BLOCK, after, NULL);
    }
}

/*
 * Makes the state directory a git repository, and keeps its ignore file
 * current.
 *
 * The ignore file is the point. The state directory holds credentials,
 * link tokens, the agents' mailboxes and memory databases right beside
 * the workspaces somebody actually wants to version -- a state
 * directory in git without it is an accident waiting to be pushed. So
 * the ignore file is written even when the repository is not created,
 * which also protects the case below.
 *
 * Not initialised when the directory is already inside somebody else's
 * repository: a nested repository there is a surprise, and their outer
 * repository is the one that needs the ignore rules anyway.
 */
/*
 * Publishes a download's progress.
 *
 * The subject is the image's name so a client can match a row to a bar
 * without keeping its own bookkeeping, the same way a message event
 * carries its room.
 */
static void
on_image_progress(ClawtVmImageStore *store,
                  const gchar       *name,
                  gint64             done,
                  gint64             total,
                  gpointer           user_data)
{
    ClawtDaemon *self = user_data;
    ClawtEvent *event = clawt_event_new("image.progress", name);

    (void)store;

    clawt_event_set_detail_int(event, "done", done);
    clawt_event_set_detail_int(event, "total", total);
    clawt_event_bus_publish(self->bus, event);
    clawt_event_free(event);
}

static void
on_image_finished(ClawtVmImageStore *store,
                  const gchar       *name,
                  const gchar       *path,
                  const gchar       *error_message,
                  gpointer           user_data)
{
    ClawtDaemon *self = user_data;
    ClawtEvent *event = clawt_event_new("image.finished", name);

    (void)store;

    if (path != NULL)
        clawt_event_set_detail(event, "path", path);

    if (error_message != NULL)
        clawt_event_set_detail(event, "error", error_message);

    clawt_event_bus_publish(self->bus, event);
    clawt_event_free(event);

    if (error_message != NULL)
        g_warning("image %s: %s", name, error_message);
    else
        g_message("image %s is ready", name);
}

static gboolean
prepare_state_git(const gchar *state_dir, gboolean init_repo,
                  gboolean *created, gchar **ignore_path, GError **error)
{
    g_autofree gchar *path = g_build_filename(state_dir, ".gitignore", NULL);
    g_autofree gchar *git_dir = g_build_filename(state_dir, ".git", NULL);
    g_autofree gchar *existing = NULL;
    g_autofree gchar *merged = NULL;
    gboolean is_repo = g_file_test(git_dir, G_FILE_TEST_IS_DIR);

    if (created != NULL)
        *created = FALSE;

    g_file_get_contents(path, &existing, NULL, NULL);
    merged = merge_gitignore(existing);

    /*
     * Only written when it differs, so an editor with the file open
     * does not see it change on every daemon start.
     */
    if (g_strcmp0(existing, merged) != 0 &&
        !clawt_write_file_atomic(path, merged, -1, 0600, existing != NULL,
                                 error))
        return FALSE;

    if (ignore_path != NULL)
        *ignore_path = g_steal_pointer(&path);

    /*
     * The ignore file is written above whatever @init_repo says: a
     * state directory somebody has put in git by hand needs those rules
     * more than one clawtilla made itself, not less.
     */
    if (!init_repo || is_repo)
        return TRUE;

    {
        g_autoptr(GSubprocess) git = NULL;
        g_autoptr(GError) local = NULL;
        g_autofree gchar *toplevel = NULL;

        /*
         * Already inside a repository?  Then leave it alone: the ignore
         * rules above are what that repository needed, and a nested
         * repository is not something to create behind somebody's back.
         */
        git = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                               G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL,
                               "git", "-C", state_dir, "rev-parse",
                               "--show-toplevel", NULL);

        if (git != NULL &&
            g_subprocess_communicate_utf8(git, NULL, NULL, &toplevel, NULL,
                                          NULL) &&
            g_subprocess_get_successful(git)) {
            g_info("state: %s is already inside the git repository at %s; "
                   "not creating another", state_dir, g_strstrip(toplevel));
            return TRUE;
        }

        g_clear_object(&git);

        git = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                               G_SUBPROCESS_FLAGS_STDERR_SILENCE, &local,
                               "git", "-C", state_dir, "init", "-q", NULL);

        if (git == NULL || !g_subprocess_wait_check(git, NULL, &local)) {
            /*
             * Not fatal. A daemon that refuses to start because git is
             * missing would be trading a real service for a
             * convenience.
             */
            g_info("state: could not make %s a git repository (%s); "
                   "the ignore file is written either way", state_dir,
                   local != NULL ? local->message : "unknown");
            return TRUE;
        }

        if (created != NULL)
            *created = TRUE;
    }

    return TRUE;
}

/*
 * Everything a pod is allowed to do.
 *
 * A closed list rather than the daemon's whole IPC surface. A pod runs
 * unattended and reacts to the fleet's own events, so a mistake in one
 * is a mistake nobody is watching -- `agent.remove` behind an automation
 * rule is not a feature anybody asked for, and adding it later is easier
 * than taking it back.
 */
static gboolean
pod_action(const gchar *action, GHashTable *params, GHashTable **out_result,
           gpointer user_data, GError **error)
{
    ClawtDaemon *self = user_data;
    const gchar *agent = g_hash_table_lookup(params, "agent");
    GHashTable *result = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free, g_free);

    *out_result = result;
    g_hash_table_insert(result, g_strdup("ok"), g_strdup("true"));

    if (g_strcmp0(action, "message_agent") == 0 ||
        g_strcmp0(action, "post_room") == 0) {
        const gchar *target = (agent != NULL)
            ? agent : g_hash_table_lookup(params, "room");
        const gchar *body = g_hash_table_lookup(params, "body");

        if (target == NULL || body == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "a target and a body are both needed");
            return FALSE;
        }

        return clawt_mailbox_router_send_to(self->router, "user", target,
                                            body, NULL, 0, error) >= 0;
    }

    if (g_strcmp0(action, "delegate") == 0) {
        const gchar *prompt = g_hash_table_lookup(params, "prompt");
        ClawtTask *task;

        if (agent == NULL || prompt == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "an agent and a prompt are both needed");
            return FALSE;
        }

        task = clawt_task_manager_create(self->tasks, "user", agent, prompt,
                                         NULL, error);

        if (task == NULL)
            return FALSE;

        clawt_task_manager_start(self->tasks, clawt_task_get_id(task));

        if (clawt_mailbox_router_send_to(self->router, "user", agent, prompt,
                                         clawt_task_get_id(task), 0,
                                         error) < 0)
            return FALSE;

        g_hash_table_insert(result, g_strdup("id"),
                            g_strdup(clawt_task_get_id(task)));
        return TRUE;
    }

    if (g_strcmp0(action, "start_agent") == 0)
        return clawt_daemon_start_agent(self, agent, error);

    if (g_strcmp0(action, "stop_agent") == 0)
        return clawt_daemon_stop_agent(self, agent);

    if (g_strcmp0(action, "restart_agent") == 0) {
        clawt_daemon_stop_agent(self, agent);
        return clawt_daemon_start_agent(self, agent, error);
    }

    if (g_strcmp0(action, "run_routine") == 0) {
        const gchar *routine = g_hash_table_lookup(params, "routine");
        const gchar *task_id;

        if (self->routines == NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                                "this daemon has no scheduler");
            return FALSE;
        }

        task_id = clawt_routine_runner_run_now(self->routines, routine, error);

        if (task_id == NULL)
            return FALSE;

        g_hash_table_insert(result, g_strdup("id"), g_strdup(task_id));
        return TRUE;
    }

    if (g_strcmp0(action, "notify") == 0) {
        const gchar *title = g_hash_table_lookup(params, "title");
        g_autoptr(ClawtNotification) notification = NULL;

        if (self->notifier == NULL) {
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                                "this daemon has no notifier");
            return FALSE;
        }

        if (title == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "a notification needs a title");
            return FALSE;
        }

        /*
         * Raised as a question, which is the one event notifiers are on
         * for by default: a pod that took the trouble to say something
         * meant it to arrive.
         */
        notification = clawt_notification_new(
            CLAWT_NOTIFY_EVENTS_QUESTION, agent, NULL, title,
            g_hash_table_lookup(params, "body"));

        clawt_notifier_notify(self->notifier, notification);
        return TRUE;
    }

    if (g_strcmp0(action, "memory_add") == 0) {
        const gchar *content = g_hash_table_lookup(params, "content");
        ClawtMemoryStore *store;
        ClawtAgent *target;

        if (agent == NULL || content == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "an agent and something to remember are "
                                "both needed");
            return FALSE;
        }

        target = clawt_agent_manager_get(self->agents, agent);
        store = (target != NULL) ? clawt_agent_get_memory(target) : NULL;

        if (store == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "%s has no memory store; memories.enabled is off",
                        agent);
            return FALSE;
        }

        {
            g_autoptr(ClawtMemory) memory = clawt_memory_new(content);
            const gchar *category = g_hash_table_lookup(params, "category");
            g_autofree gchar *id = NULL;

            if (category != NULL)
                g_object_set(memory, "category", category, NULL);

            id = clawt_memory_store_add(store, memory, error);

            if (id == NULL)
                return FALSE;

            g_hash_table_insert(result, g_strdup("id"),
                                g_steal_pointer(&id));
            return TRUE;
        }
    }

    if (g_strcmp0(action, "computer_exec") == 0) {
        const gchar *command = g_hash_table_lookup(params, "command");

        /*
         * Deliberately refused rather than run. Every other action here
         * is a fleet operation the daemon already owns; this one is
         * arbitrary code on somebody's machine, triggered by an event,
         * with nobody watching -- and podomation can already run a
         * command through its own modules, where it is at least visible
         * as one in the pod.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "computer_exec is not available to a pod: use "
                    "podomation's own command module, or delegate it to "
                    "the agent (%s)", command != NULL ? command : "");
        return FALSE;
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "there is no action called '%s'", action);

    return FALSE;
}

/*
 * Whether this routine asked for a conversation of its own.
 *
 * Read from the config each run rather than remembered, so turning it on
 * takes effect at the next run rather than at the next daemon start --
 * which is when somebody would look for it, having just changed it.
 */
static gboolean
routine_is_isolated(ClawtDaemon *self, const gchar *routine_id)
{
    GPtrArray *routines = clawt_config_get_routines(self->config);
    guint i;

    for (i = 0; routines != NULL && i < routines->len; i++) {
        ClawtRoutine *routine = g_ptr_array_index(routines, i);

        if (g_strcmp0(clawt_routine_get_id(routine), routine_id) == 0)
            return clawt_routine_get_boolean(routine, "isolate");
    }

    return FALSE;
}

/*
 * Starting one scheduled run.
 *
 * A routine is a delegated task rather than a message: it appears in the
 * task list while it runs and has a result afterwards.
 *
 * It does *not* get a libreclaw session of its own, whatever this
 * comment and docs/routines.org used to claim.  libreclaw keys a session
 * on channel, room and sender and deliberately not on the thread --
 * lc_router_resolve_session_key() says so in a note of its own -- and
 * this sends from "user" to the agent, which is the operator's own room.
 * So a run shares the operator's session and its queue: it inherits the
 * last conversation's context and waits for whatever is in flight.
 *
 * ...unless `routines.isolate` is set, which is what makes the isolation
 * real: a room of its own and a sender of its own is a session key of
 * its own, so no shared context and no waiting behind a conversation.
 * It is opt-in because it moves the run's output out of the operator's
 * transcript into the task result and the Flow tab, and moving somebody's
 * output is not a thing to do to them silently.
 */
static const gchar *
run_routine(const gchar *routine_id, const gchar *agent_id,
            const gchar *prompt, gpointer user_data, GError **error)
{
    ClawtDaemon *self = user_data;
    ClawtTask *task;
    g_autofree gchar *room_id = NULL;
    const gchar *sender = "user";
    const gchar *target = agent_id;

    if (clawt_agent_manager_get(self->agents, agent_id) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "routine '%s' names '%s', which is not an agent",
                    routine_id, agent_id);
        return NULL;
    }

    /*
     * A room and a sender of its own, when the routine asked for them.
     *
     * Both, not either: libreclaw's session key is channel, room and
     * sender together, so a distinct room reached from `user` would
     * still be a distinct session but every routine on that agent would
     * share one, and a distinct sender in the operator's room would put
     * the run in their transcript anyway.
     *
     * One room per routine rather than per run.  A room per run would
     * give perfect isolation and no continuity at all -- and continuity
     * between a routine's own runs is the thing worth having, since it
     * is what lets Tuesday's brief know what Monday's said.
     */
    if (routine_is_isolated(self, routine_id)) {
        ClawtRoom *room = clawt_room_manager_get_routine(self->rooms,
                                                         routine_id,
                                                         agent_id);

        if (room != NULL) {
            room_id = g_strdup(clawt_room_get_id(room));
            sender = "routine";
            target = room_id;
        }
    }

    task = clawt_task_manager_create(self->tasks, sender, agent_id, prompt,
                                     NULL, error);

    if (task == NULL)
        return NULL;

    /*
     * Started before it is delivered.  An agent that answers instantly
     * would otherwise complete a task that had not been marked as
     * running, which reads in the task list as a run that never happened.
     */
    clawt_task_manager_start(self->tasks, clawt_task_get_id(task));

    if (clawt_mailbox_router_send_to(self->router, sender, target, prompt,
                                     clawt_task_get_id(task), 0, error) < 0) {
        clawt_task_manager_fail(self->tasks, clawt_task_get_id(task),
                                (error != NULL && *error != NULL)
                                    ? (*error)->message
                                    : "it could not be delivered");
        return NULL;
    }

    clawt_event_bus_emit(self->bus, "routine.ran", routine_id);

    return clawt_task_get_id(task);
}

/* ── What is worth interrupting somebody for ─────────────────────── */

/*
 * An agent's state changed.
 *
 * Published for every state, because a client that has to poll to find
 * out an agent crashed is a client that shows a running agent that is
 * not -- but notified about only for the states nobody asked for.
 */
static void
on_agent_state_changed(ClawtAgentManager *manager,
                       const gchar       *agent_id,
                       gint               state,
                       const gchar       *detail,
                       gpointer           user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(ClawtEvent) event = NULL;
    ClawtAgentConfig *config;
    g_autoptr(ClawtNotification) notification = NULL;
    const gchar *nick;

    (void)manager;

    nick = clawt_enum_to_nick(CLAWT_TYPE_AGENT_STATE, state);

    event = clawt_event_new("agent.state", agent_id);
    clawt_event_set_detail(event, "state", nick != NULL ? nick : "unknown");

    if (detail != NULL)
        clawt_event_set_detail(event, "detail", detail);

    clawt_event_bus_publish(self->bus, event);

    if (state != CLAWT_AGENT_STATE_ERROR &&
        state != CLAWT_AGENT_STATE_DEGRADED)
        return;

    if (self->notifier == NULL)
        return;

    config = clawt_config_get_agent(self->config, agent_id);

    notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_ERROR, agent_id,
        config != NULL ? clawt_agent_config_get_string(config, "name") : NULL,
        state == CLAWT_AGENT_STATE_ERROR ? "stopped with an error"
                                         : "is degraded",
        detail);

    clawt_notifier_notify(self->notifier, notification);
}

/*
 * A task changed state.
 *
 * Only a finished one is worth a notification, and only because
 * somebody asked: `done` is off by default, since a fleet that works is
 * a fleet finishing tasks all day.
 */
static void
on_task_changed(ClawtTaskManager *manager,
                const gchar      *task_id,
                gint              state,
                gpointer          user_data)
{
    ClawtDaemon *self = user_data;
    ClawtTask *task;
    ClawtAgentConfig *config;
    g_autoptr(ClawtNotification) notification = NULL;
    g_autofree gchar *summary = NULL;

    (void)manager;

    if (state != CLAWT_TASK_COMPLETED || self->notifier == NULL)
        return;

    task = clawt_task_manager_get(self->tasks, task_id);

    if (task == NULL)
        return;

    config = clawt_config_get_agent(self->config,
                                    clawt_task_get_assignee(task));

    summary = clawt_notify_summarize(clawt_task_get_result(task), 0);

    notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_DONE, clawt_task_get_assignee(task),
        config != NULL ? clawt_agent_config_get_string(config, "name") : NULL,
        "finished a task", summary);

    clawt_notifier_notify(self->notifier, notification);
}

/*
 * Whether a room is the private conversation between an agent and the
 * person running it.
 *
 * Every room has the human as an implicit member, so membership cannot
 * answer this -- by that test every message an agent ever sent would be
 * worth a buzz. A direct room is `dm:<a>:<b>` with the pair sorted, so
 * the question is whether one half of it is the operator.
 */
static gboolean
is_operator_room(const gchar *room_id)
{
    g_auto(GStrv) parts = NULL;

    if (room_id == NULL || !g_str_has_prefix(room_id, "dm:"))
        return FALSE;

    parts = g_strsplit(room_id + 3, ":", 2);

    return g_strcmp0(parts[0], "user") == 0 ||
           g_strcmp0(parts[1], "user") == 0;
}

/*
 * An agent said something to its operator.
 *
 * This is the one that matters. `clawtilla_message_user` is the only way
 * an agent can reach a person, and until now it put the message
 * somewhere they would see it *if they looked* -- so an agent that asked
 * a question and waited was an agent that had silently stopped.
 */
static void
notify_user_message(ClawtDaemon *self, const gchar *from, const gchar *body,
                    const gchar *room_id)
{
    ClawtAgentConfig *config;
    g_autoptr(ClawtNotification) notification = NULL;
    g_autofree gchar *summary = NULL;

    if (self->notifier == NULL || from == NULL)
        return;

    config = clawt_config_get_agent(self->config, from);
    summary = clawt_notify_summarize(body, 0);

    notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_QUESTION, from,
        config != NULL ? clawt_agent_config_get_string(config, "name") : NULL,
        summary, NULL);

    notification->room_id = g_strdup(room_id);

    clawt_notifier_notify(self->notifier, notification);
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
    ClawtAgent *agent;
    ClawtEvent *event;

    (void)server;

    if (agent_id == NULL)
        return;

    /*
     * Recorded on the agent as well as published, so a client that
     * connects while a turn is already running is not left thinking
     * the agent is idle until the next transition.
     */
    agent = clawt_agent_manager_get(self->agents, agent_id);

    if (agent != NULL)
        clawt_agent_set_activity(agent, typing, NULL);

    event = clawt_event_new("agent.typing", agent_id);
    clawt_event_set_detail(event, "typing", typing ? "true" : "false");

    /*
     * Who the turn is for travels with it, because that is the part a
     * client cannot work out: an agent busy for three minutes on a
     * peer's question looks exactly like one busy on yours.
     */
    if (agent != NULL && clawt_agent_get_activity_peer(agent) != NULL)
        clawt_event_set_detail(event, "peer",
                               clawt_agent_get_activity_peer(agent));

    if (room_id != NULL)
        clawt_event_set_detail(event, "room", room_id);

    clawt_event_bus_publish(self->bus, event);
    clawt_event_free(event);
}

/*
 * Charge what an agent has spent since its last turn to the task it is
 * answering about.
 *
 * `orchestration.task_budget_usd` has been enabled by default since the
 * schema was written, the guard has always checked it, and nothing had
 * ever called clawt_loop_guard_record_spend() outside a test -- so the
 * one limit built to stop an expensive loop could never fire.  Same
 * shape as the hop counter before it: a limit nothing reaches.
 *
 * Drained on every reply, not only on the ones carrying a task, because
 * the drain is what advances the watermark.  Skipping the untasked
 * turns would bank them and hand the whole accumulated bill to whatever
 * task happened to be answered next.
 */
static void
charge_turn_to_task(ClawtDaemon *self, const gchar *agent_id,
                    const gchar *task_id)
{
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *db_path = NULL;
    gint64 cost_micros;

    if (self->usage == NULL)
        return;

    state_dir = clawt_config_agent_state_dir(self->config, agent_id);
    if (state_dir == NULL)
        return;

    db_path = clawt_usage_database_path(state_dir);
    cost_micros = clawt_usage_drain(self->usage, agent_id, db_path);

    if (task_id == NULL || cost_micros <= 0)
        return;

    clawt_loop_guard_record_spend(self->guard, task_id,
                                  (gdouble)cost_micros / 1000000.0);
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

    /*
     * One hop further than the message being answered, not a flat 1.
     *
     * The router records how far each delivery had travelled, and the
     * orchestration tools already read it back -- but an agent's
     * ordinary reply comes through here, and this hardcoded 1 made
     * every one of them look like the start of a fresh conversation. So
     * max_hops, which exists for precisely the case of two agents
     * replying politely to each other for ever, could never be reached
     * on the path where that actually happens: two agents traded fifty
     * messages of "Idle." and nothing stopped them.
     */
    {
        ClawtAgent *sender = clawt_agent_manager_get(self->agents, agent_id);

        clawt_message_set_depth(
            message,
            (sender != NULL) ? clawt_agent_get_hop_depth(sender) + 1 : 1);

        /*
         * And then forget it, now that the reply carries it.
         *
         * hop_depth answers "how far had the message I am handling
         * come", which is true of a turn rather than of an agent. The
         * router is its only other writer, so a turn that began
         * somewhere the daemon never sees -- Matrix, webhook, local,
         * cmacs -- used to inherit whatever the last agent-to-agent
         * delivery had left, and add one to it. Enough of those and an
         * agent could not start a delegation at all.
         *
         * Cleared *here* rather than when the turn ends, which is where
         * it was first tried. libreclaw drops its typing indicator
         * before it posts the answer, so clearing on that transition
         * lands in the window between the two: the reply is then stamped
         * from zero, every chain restarts at one, and max_hops stops
         * being reachable on the one path it exists for -- two agents
         * answering each other for ever. The reply is the last thing
         * that needs the number, so it is the right place to drop it.
         */
        if (sender != NULL)
            clawt_agent_set_hop_depth(sender, 0);
    }

    /*
     * No event is published here.  The router publishes one for every
     * message once it knows the room, and doing it here as well meant
     * two events for one message -- one of which could not say where it
     * had gone.
     */

    /*
     * Before the reply is routed, so the spend is on the books by the
     * time the guard is asked whether the next message may be sent.
     */
    charge_turn_to_task(self, agent_id, thread_id);

    /*
     * A task id on the *last* message of a turn means the delegated work
     * is finished.  An agent that replies without also calling
     * clawtilla_task_complete would otherwise leave the delegator
     * waiting on a task that is already done.
     *
     * On the last message, and not on any message carrying the id, which
     * is what this used to do.  libreclaw sends more than the answer in
     * a thread: a progress note every five minutes by default, a
     * guardian refusal, a restart notice.  Each of those completed the
     * task the moment it arrived -- so a routine reported `completed`
     * within seconds of starting, its result was the text "Still
     * working...", and the work itself happened minutes later against a
     * task nothing was waiting on any more.  A state that says finished
     * while the work runs is worse than no state at all: anything
     * polling it gets a false positive and stops looking.
     *
     * The turn is what separates them.  libreclaw brackets a turn with
     * its typing indicator and stops it in on_process_message_finish()
     * *before* the answer is posted, so a message that arrives while the
     * agent is still marked busy is by construction not the answer.  An
     * agent that never sends the indicator -- it needs a room, and is
     * skipped without one -- is busy=FALSE throughout and completes as
     * it always did, which is the safe way round: a task that ends late
     * is a delay, one that ends early is a lie.
     */
    if (thread_id != NULL) {
        ClawtAgent *replier = clawt_agent_manager_get(self->agents, agent_id);

        if (replier != NULL && clawt_agent_get_busy(replier))
            g_info("daemon: %s is still working, so this is not the answer "
                   "to %s", agent_id, thread_id);
        else
            clawt_task_manager_complete(self->tasks, thread_id, body);
    }

    if (clawt_mailbox_router_send(self->router, message, &error) < 0) {
        g_info("daemon: %s's message was not routed: %s", agent_id,
               error->message);
        return;
    }

    if (is_operator_room(destination))
        notify_user_message(self, agent_id, body, destination);
}

/*
 * How the orchestration tools actually send anything.
 *
 * Routed through the same path as every other message rather than posting
 * straight into a mailbox, so the hop limits, rate limits and cycle
 * detection apply to tool calls exactly as they do to ordinary chat.
 */
/*
 * Everything one agent owns on disk.
 *
 * Three places, and they are three because they can be configured
 * apart: the workspace (its persona and its org files), the state
 * directory (its mailbox, its memories, its token and its rendered
 * libreclaw config), and its transcripts. By default the first two are
 * the same directory, which is exactly the sort of coincidence that
 * hides a missing one.
 *
 * Every removal is fenced with clawt_remove_tree(), which refuses a path
 * outside the root it was derived from. The paths come from
 * configuration somebody edits, and there is no undo on the other side
 * of this.
 */
static gboolean
clawt_daemon_purge_agent_files(ClawtDaemon      *self,
                               ClawtAgentConfig *config,
                               gboolean         *out_was_linked,
                               GError          **error)
{
    const gchar *agent_id = clawt_agent_config_get_id(config);
    g_autofree gchar *state_root = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *state_dir = NULL;
    g_autofree gchar *transcripts = NULL;

    state_root = clawt_config_get_path_value(self->config, "daemon.state_dir");

    if (state_root == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "no state directory to remove anything from");
        return FALSE;
    }

    /*
     * The workspace is fenced by its own root rather than by the state
     * directory: somebody may keep agent workspaces in a source tree,
     * and the check has to be against the root they configured.
     */
    workspace = clawt_agent_config_get_workspace(config);

    /*
     * Whether the workspace was a link, asked before it is removed.
     *
     * A linked agent's files are not deleted -- only the link is -- and
     * the client's own line said "Its files are gone too" regardless,
     * which is exactly wrong for the mode chosen precisely so they
     * would not be.
     */
    if (out_was_linked != NULL)
        *out_was_linked = (workspace != NULL &&
                           g_file_test(workspace, G_FILE_TEST_IS_SYMLINK));

    if (workspace != NULL) {
        g_autofree gchar *workspace_root =
            clawt_config_get_path_value(self->config, "defaults.workspace_root");

        if (workspace_root != NULL &&
            !clawt_remove_tree(workspace, workspace_root, error))
            return FALSE;
    }

    state_dir = g_build_filename(state_root, "agents", agent_id, NULL);

    if (!clawt_remove_tree(state_dir, state_root, error))
        return FALSE;

    /*
     * Transcripts are named for the room rather than the agent, so this
     * is the one place a name has to be matched rather than built.
     */
    transcripts = g_build_filename(state_root, "transcripts", NULL);

    if (g_file_test(transcripts, G_FILE_TEST_IS_DIR)) {
        g_autoptr(GDir) dir = g_dir_open(transcripts, 0, NULL);
        g_autofree gchar *needle = g_strdup_printf(":%s:", agent_id);
        g_autofree gchar *prefix = g_strdup_printf("%s:", agent_id);
        const gchar *name;

        while (dir != NULL && (name = g_dir_read_name(dir)) != NULL) {
            g_autofree gchar *path = NULL;

            if (strstr(name, needle) == NULL &&
                !g_str_has_prefix(name, prefix))
                continue;

            path = g_build_filename(transcripts, name, NULL);

            if (!clawt_remove_tree(path, state_root, error))
                return FALSE;
        }
    }

    return TRUE;
}

/*
 * Lowest `order` first, and ties keep the order the configuration file
 * has them in.
 *
 * Stable on purpose: agents all sitting at the default 0 must come back
 * exactly as they were written, or a fleet nobody has reordered would
 * shuffle itself on every listing.
 */
/*
 * Where an agent's group sits: teamless first, then teams in their own
 * order.
 *
 * Teamless first because that is where the chief of staff lives, and
 * anything not yet assigned -- putting it at the bottom would bury the
 * one agent somebody talks to most under every team in the fleet.
 */
static gint
group_position(GPtrArray *teams, ClawtAgentConfig *config)
{
    const gchar *team = clawt_agent_config_get_string(config, "team");
    guint i;

    if (team == NULL || *team == '\0')
        return G_MININT;

    for (i = 0; i < teams->len; i++) {
        ClawtTeamSpec *spec = g_ptr_array_index(teams, i);

        if (g_strcmp0(spec->id, team) == 0)
            return (gint)i;
    }

    /*
     * A team nobody declared. Sorted after every declared one rather
     * than dropped, because the agent is real and hiding it is how a
     * typo in `agents.team` survives being looked at.
     */
    return G_MAXINT;
}

static gint
compare_by_order(gconstpointer a, gconstpointer b, gpointer user_data)
{
    GPtrArray *teams = user_data;
    ClawtAgentConfig *first = clawt_agent_get_config(*(ClawtAgent *const *)a);
    ClawtAgentConfig *second = clawt_agent_get_config(*(ClawtAgent *const *)b);
    gint left_group = group_position(teams, first);
    gint right_group = group_position(teams, second);
    gint64 left;
    gint64 right;

    /*
     * Grouped before ordered, so the sidebar can put a header out
     * whenever the team changes rather than gathering the fleet itself.
     * Two answers to what order the fleet is in is one too many.
     */
    if (left_group != right_group)
        return left_group < right_group ? -1 : 1;

    left = clawt_agent_config_get_int(first, "order");
    right = clawt_agent_config_get_int(second, "order");

    if (left == right)
        return 0;

    return left < right ? -1 : 1;
}

/* Defined below, beside the frame that is its other caller. */
static ClawtAgentConfig *daemon_create_agent(ClawtDaemon  *self,
                                             const gchar  *agent_id,
                                             GHashTable   *fields,
                                             const gchar  *purpose,
                                             gboolean     *purpose_landed,
                                             GError      **error);

/*
 * An agent creating an agent, through the same door a person uses.
 *
 * It goes to daemon_create_agent() rather than reimplementing any of it,
 * which is the whole point: the validation, the rollback on a bad
 * computer, the reload and the start are one implementation. An agent
 * asking is not a reason to trust the request more, and the last time
 * two creation paths existed one of them skipped the check that refuses
 * a VM with no disk.
 */
/*
 * An agent files a decision for its operator.
 *
 * The reply tells it what it is expected to do next, in as many words:
 * carry on with your default.  An agent that filed a question and then
 * waited would have turned a non-blocking inbox back into the blocking
 * one it replaces, and the tool description alone is not enough --
 * whatever a tool *returns* is what shapes the next turn.
 */
static gchar *
file_decision_for_tools(const gchar    *agent_id,
                        ClawtDecision  *decision,
                        gpointer        user_data,
                        GError        **error)
{
    ClawtDaemon *self = user_data;
    g_autofree gchar *id = NULL;

    if (self->decisions == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "this daemon keeps no decision inbox");
        return NULL;
    }

    id = clawt_decision_store_post(self->decisions, decision, error);

    if (id == NULL)
        return NULL;

    /*
     * Published, so a client that is open right now grows a badge
     * without polling -- the same stream every other surface folds over.
     */
    if (self->bus != NULL)
        clawt_event_bus_emit(self->bus, "decision.asked", agent_id);

    return g_strdup_printf(
        "Filed as %s. Do not wait for it: carry on with what you said "
        "you would do (%s). If it is answered you will get a message, "
        "and you can change course then.",
        id,
        clawt_decision_get_default(decision) != NULL
            ? clawt_decision_get_default(decision)
            : "your default");
}

static gchar *
create_agent_for_tools(const gchar  *agent_id,
                       const gchar  *purpose,
                       GHashTable   *settings,
                       gboolean      start,
                       gpointer      user_data,
                       GError      **error)
{
    ClawtDaemon *self = user_data;
    g_autoptr(GString) out = NULL;
    gboolean purpose_landed = FALSE;

    if (daemon_create_agent(self, agent_id, settings, purpose,
                            &purpose_landed, error) == NULL)
        return NULL;

    out = g_string_new(NULL);
    g_string_append_printf(out, "Created %s.", agent_id);

    /*
     * Said rather than swallowed.  The workspace directory outlives the
     * agent that was removed from it, so creating one under an id that
     * has been used before finds a SOUL.org already there -- and it is
     * left alone, because it is somebody's work.  Whoever wrote the
     * purpose has to be told it is not in the file, or they go on
     * believing the agent read it.
     */
    if (purpose != NULL && *purpose != '\0' && !purpose_landed)
        g_string_append(out,
            " Its workspace already had a SOUL.org, so that file was left "
            "alone and the purpose is not in it -- edit SOUL.org if it "
            "should be.");

    if (start) {
        g_autoptr(GError) start_error = NULL;

        if (clawt_daemon_start_agent(self, agent_id, &start_error)) {
            g_string_append(out, " It is running.");
        } else {
            /*
             * Said rather than swallowed, and not an error: the agent
             * exists and its configuration is on disk. Whoever asked for
             * it needs to know it is not working *and* that it is there,
             * because the second half is what stops them making it
             * twice.
             */
            g_string_append_printf(out,
                " It exists but did not start: %s. It can be started "
                "again once that is fixed -- do not create it a second "
                "time.",
                start_error != NULL ? start_error->message : "unknown");
        }
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static gboolean
deliver_for_tools(const gchar *from_agent, const gchar *target,
                  const gchar *body, const gchar *task_id, gint depth,
                  gpointer user_data, GError **error)
{
    ClawtDaemon *self = user_data;

    if (clawt_mailbox_router_send_to(self->router, from_agent, target, body,
                                     task_id, depth, error) < 0)
        return FALSE;

    /*
     * This is where clawtilla_message_user arrives, which is the whole
     * point of the notifier: an agent that asked a question and waited
     * had, until now, silently stopped.
     */
    if (is_operator_room(target))
        notify_user_message(self, from_agent, body, target);

    return TRUE;
}

/* ── Agents ──────────────────────────────────────────────────────── */

static void
apply_mounts(ClawtDaemon *self, ClawtAgent *agent, ClawtComputer *computer)
{
    ClawtAgentConfig *config = clawt_agent_get_config(agent);
    guint i;

    /*
     * The agent's own mounts are NOT applied here.
     *
     * clawt_computer_factory_create() already did it -- building a
     * computer from a config is its whole job, and it also fills in the
     * mount type per backend. Doing it again here added every mount
     * twice, and podman refuses a create with a duplicate destination:
     * "\"/work\": duplicate mount destination". It went unseen because
     * no client could add a mount until now, so nobody had one.
     */

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
 * An agent whose files clawtilla refused to render, and why.
 *
 * A refusal is not always the daemon's fault: the renderer turns down a
 * `libreclaw:` passthrough that redeclares a section clawtilla renders
 * itself, and that section is something the operator just typed.  Left as
 * a g_warning, the refusal reached the journal and nowhere else -- so the
 * agent kept running on its previous config.yaml and the edit looked
 * ignored rather than rejected.  Collected here so the caller can say so.
 */
typedef struct {
    gchar *agent_id;
    gchar *message;
} RenderRefusal;

static void
render_refusal_free(gpointer data)
{
    RenderRefusal *refusal = data;

    g_free(refusal->agent_id);
    g_free(refusal->message);
    g_free(refusal);
}

/*
 * Renders every agent's files, whether or not it is running.
 *
 * Done up front rather than at start time only, because the link token is
 * written here and an agent started by hand -- or by systemd, or inside a
 * container someone else brought up -- has to be able to authenticate
 * without clawtilla having launched it.
 *
 * One agent's refusal never stops the others: a fleet that would not
 * reload because one agent's block is wrong is a fleet one typo can hold
 * hostage.  @refusals, when it is not %NULL, collects the ones that were
 * turned down so whoever asked can be told.
 */
static void
render_all_agents_into(ClawtDaemon *self, GPtrArray *refusals)
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
                                            self->link_socket, NULL,
                                            &error)) {
            g_warning("agent %s: %s", clawt_agent_get_id(agent),
                      error->message);

            if (refusals != NULL) {
                RenderRefusal *refusal = g_new0(RenderRefusal, 1);

                refusal->agent_id = g_strdup(clawt_agent_get_id(agent));
                refusal->message = g_strdup(error->message);
                g_ptr_array_add(refusals, refusal);
            }
        }

        /*
         * ...and the tools it actually has, from the live gate.
         *
         * Written here rather than in the renderer because this is the
         * only place that knows both the agent's capabilities and its
         * permissions. Without it TOOLS.org carries whatever table was
         * scaffolded, and a tool granted afterwards never appears --
         * which a chief-of-staff read as not having it, on the day it
         * was given the tool to create agents.
         */
        if (self->mcp_tools != NULL) {
            g_autofree gchar *listing =
                clawt_mcp_tools_describe_for_agent(self->mcp_tools,
                                                   clawt_agent_get_id(agent));
            g_autoptr(GError) tools_error = NULL;

            if (listing != NULL &&
                !clawt_workspace_update_tool_list(config, listing,
                                                  &tools_error))
                g_warning("agent %s: %s", clawt_agent_get_id(agent),
                          tools_error->message);
        }
    }
}

/*
 * There is deliberately no render_all_agents() convenience taking no
 * refusal array.  Every caller in the tree passes one now, and a wrapper
 * that discards them is exactly how six handlers came to report success
 * about an agent left running on its previous config.
 */
static GPtrArray *
render_refusals_new(void)
{
    return g_ptr_array_new_with_free_func(render_refusal_free);
}

/*
 * Writes the `refused` array into whatever object the builder is inside.
 *
 * Every handler that re-renders the fleet gets one, always present even
 * when it is empty, so a client can tell "nothing was refused" from "this
 * daemon does not report refusals".  control.reload was the first to do
 * this and for a while the only one -- six other handlers rewrite the
 * same files and told the caller they had succeeded while the agent they
 * were about to affect kept the config.yaml it already had.  agent.set is
 * the one that mattered day to day: rendering is the whole point of the
 * call there, and a refusal left it doing nothing at all.
 */
static void
add_render_refusals(JsonBuilder *builder, GPtrArray *refusals)
{
    guint i;

    json_builder_set_member_name(builder, "refused");
    json_builder_begin_array(builder);

    for (i = 0; refusals != NULL && i < refusals->len; i++) {
        RenderRefusal *refusal = g_ptr_array_index(refusals, i);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, refusal->agent_id);
        json_builder_set_member_name(builder, "message");
        json_builder_add_string_value(builder, refusal->message);
        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
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

        /*
         * A computer that cannot be built from the config is a shadow
         * agent: an unknown type or an unusable mount is not going to
         * fix itself, and the fleet should carry on around it.
         */
        if (computer == NULL) {
            clawt_agent_mark_shadow(agent, local->message);
            g_propagate_error(error, g_steal_pointer(&local));
            return FALSE;
        }

        apply_mounts(self, agent, computer);

        /*
         * The desktop is an add-on to whatever computer that turned out
         * to be, so it is built alongside rather than inside it.  It is
         * attached even when it turns out to be unreachable: the agent
         * asking about a desktop it was granted should be told why it
         * cannot have it, not told it never had one.
         */
        {
            g_autoptr(ClawtDesktop) desktop =
                clawt_computer_factory_create_desktop(config);

            clawt_agent_set_desktop(agent, desktop);
        }

        /*
         * Attached before it is started, and kept even when starting
         * fails.  Dropping it left `computer status` answering "that
         * agent has no computer" for an agent that plainly has one
         * configured, which reads as a different fault entirely.
         */
        clawt_agent_set_computer(agent, computer);

        if (!clawt_computer_start(computer, &local)) {
            /*
             * ERROR, not SHADOW.  A podman that is not running, an image
             * that is not pulled, a name still held by yesterday's
             * container -- these are all transient, and SHADOW refuses
             * every later start with the message frozen from the first
             * one, so a fixed problem still looked broken until the
             * daemon was restarted.
             */
            clawt_agent_set_error(agent, local->message);
            g_propagate_error(error, g_steal_pointer(&local));
            return FALSE;
        }
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

    }

    /*
     * The restart policy is retaken on every start, outside the block
     * that builds the runtime.
     *
     * Nothing ever sets a runtime back to NULL, so inside that block
     * this ran exactly once in an agent's life -- at its first start --
     * and a reload reconciles agents rather than rebuilding them, so
     * the object holding the stale answer is precisely the one that
     * survives.  `restart: always` was accepted, written, reloaded and
     * reported applied while the runtime went on refusing to restart a
     * clean exit, because it still held the on-failure default.  Every
     * surface agreed the change had landed.
     *
     * Reading it here is also the documented contract: a configuration
     * change applies at the agent's next start.  The backoff and the
     * ceiling come with it -- one of the three reaching the runtime and
     * the other two not would be worse than none of them doing.
     */
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
    /*
     * Belt as well as braces.  clawt_daemon_stop() takes this down for a
     * daemon that ran; this catches one that was built and never did.
     */
    if (self->connector_refresh != NULL) {
        g_source_destroy(self->connector_refresh);
        g_clear_pointer(&self->connector_refresh, g_source_unref);
    }

    g_clear_object(&self->plugins);

    /*
     * Emptied, not freed: a stopped daemon can be started again, and a
     * NULL table would crash the first design after it.  It has to
     * happen before the config goes, because every pending designer
     * holds a reference to it.
     */
    if (self->drafts != NULL)
        g_hash_table_remove_all(self->drafts);

    g_clear_object(&self->mcp_tools);
    g_clear_object(&self->ipc_server);
    g_clear_object(&self->link_server);
    g_clear_object(&self->pod_bridge);
    g_clear_object(&self->router);
    g_clear_object(&self->guard);
    g_clear_object(&self->usage);
    g_clear_object(&self->tasks);
    g_clear_object(&self->rooms);
    g_clear_object(&self->agents);
    g_clear_object(&self->vm_images);
    g_clear_object(&self->exchange);
    g_clear_object(&self->decisions);
    g_clear_object(&self->log);
    g_clear_object(&self->bus);
    g_clear_object(&self->config);

    g_clear_pointer(&self->state_dir, g_free);
    g_clear_pointer(&self->link_socket, g_free);
    g_clear_pointer(&self->attachment_dir, g_free);
}

/*
 * How long a cached model list is trusted.
 *
 * Providers add models in weeks, not minutes, so this only has to be
 * short enough that a daemon left running for days notices.
 */
#define MODEL_CACHE_TTL_SECONDS (6 * 60 * 60)

static void
on_models_ready(const gchar *provider_id, GStrv models, gpointer user_data)
{
    ClawtDaemon *self = user_data;

    if (self->model_cache == NULL || models == NULL || models[0] == NULL)
        return;

    g_hash_table_insert(self->model_cache, g_strdup(provider_id),
                        g_strdupv(models));
}

/*
 * Asks every provider that can be asked, without waiting for any.
 *
 * Answers land in the cache as they arrive; a request made in the
 * meantime is served from the built-in table rather than blocked.
 */
static void
warm_model_cache(ClawtDaemon *self)
{
    const ClawtProviderInfo *catalog;
    gsize n_providers = 0;
    gsize i;

    catalog = clawt_model_catalog_get(&n_providers);
    self->model_cache_at = g_get_monotonic_time();

    for (i = 0; i < n_providers; i++) {
        if (!catalog[i].tools)
            continue;

        clawt_model_catalog_fetch_models_async(catalog[i].id,
                                                on_models_ready, self);
    }
}

static gboolean
on_sweep(gpointer user_data)
{
    ClawtDaemon *self = user_data;

    clawt_mailbox_router_sweep(self->router);
    clawt_event_log_sweep(self->log);

    return G_SOURCE_CONTINUE;
}

/* ── Connectors ──────────────────────────────────────────────────── */

/* Saves repeating the two-line dance for every optional string field. */
static void
add_string_member(JsonBuilder *builder, const gchar *name, const gchar *value)
{
    json_builder_set_member_name(builder, name);
    json_builder_add_string_value(builder, value);
}

/*
 * A flow in progress.
 *
 * Authorising takes as long as a person takes, which is far longer than
 * an IPC request may block -- so `connector.begin` answers as soon as
 * there is something to show them, and `connector.await` is the deferred
 * one that finishes when they have done it.  Splitting it in two is what
 * lets a client display the code the instant it exists rather than after
 * the whole thing has completed, which would be no use to anybody.
 */
typedef struct {
    ClawtDaemon     *daemon;      /* not owned; the daemon outlives a flow */
    gchar           *id;
    gchar           *name;
    gchar           *token_url;
    gchar           *client_id;
    gchar           *client_secret;
    gchar           *verifier;
    gchar           *redirect_uri;
    ClawtIpcPending *waiter;
    gboolean         settled;
    gboolean         ok;
    gchar           *message;
    gint64           settled_at;
} ConnectorFlow;

static void
connector_flow_free(ConnectorFlow *flow)
{
    if (flow == NULL)
        return;

    g_free(flow->id);
    g_free(flow->name);
    g_free(flow->token_url);
    g_free(flow->client_id);
    g_free(flow->client_secret);
    g_free(flow->verifier);
    g_free(flow->redirect_uri);
    g_free(flow->message);
    g_free(flow);
}

/*
 * Read once and kept, because it is read on paths a person is waiting
 * on -- opening the connector list re-reads every file in connectors.d
 * otherwise.  Dropped on reload, so editing a connector file and
 * reloading the daemon picks it up.
 */
static GPtrArray *
daemon_catalog(ClawtDaemon *self)
{
    if (self->connector_catalog == NULL) {
        g_autofree gchar *dir =
            clawt_config_get_path_value(self->config, "connectors.dir");

        self->connector_catalog = clawt_connector_catalog_load(dir, NULL);
    }

    return self->connector_catalog;
}

/*
 * The integration instance and its catalogue entry together, which is
 * what every connector operation needs and neither half is any use
 * without.
 */
static ClawtIntegrationBinding *
connector_binding(ClawtDaemon               *self,
                  const gchar               *name,
                  const ClawtConnectorInfo **out_info,
                  GError                   **error)
{
    ClawtIntegrationConfig *instance = (name != NULL)
        ? clawt_config_get_integration(self->config, name) : NULL;
    const ClawtIntegrationInfo *info;
    const ClawtConnectorInfo *connector;
    const gchar *provider;

    if (instance == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                            "there is no integration called that");
        return NULL;
    }

    info = clawt_integration_find(
        clawt_integration_config_get_type_id(instance));

    if (info == NULL || g_strcmp0(info->id, "connector") != 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "that integration is not a connector");
        return NULL;
    }

    provider = clawt_integration_config_get_string(instance, NULL, "provider");
    connector = clawt_connector_catalog_find(daemon_catalog(self), provider);

    if (connector == NULL) {
        g_autofree gchar *dir =
            clawt_config_get_path_value(self->config, "connectors.dir");

        /*
         * Names the directory as well as the provider.  The fix is
         * almost always a file that adds it, and somebody who has never
         * needed one has no reason to know where it goes.
         */
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no connector called '%s'; add one in %s or pick from "
                    "`clawtilla connector catalog`",
                    provider != NULL ? provider : "(unset)", dir);
        return NULL;
    }

    if (out_info != NULL)
        *out_info = connector;

    return clawt_integration_binding_for_instance(instance, info, NULL);
}

/*
 * A client secret is a secret reference like any other, so it is
 * resolved rather than read: somebody who put theirs in `pass` should
 * not have to make an exception for this one field.
 */
static gchar *
connector_client_secret(ClawtDaemon *self, ClawtIntegrationBinding *binding)
{
    g_autoptr(ClawtSecretRef) ref =
        clawt_integration_binding_get_secret(binding, "client_secret");
    g_autofree gchar *secrets_dir = NULL;

    if (ref == NULL)
        return NULL;

    secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");

    return clawt_secret_ref_resolve(
        ref, secrets_dir,
        (guint)clawt_config_get_int(self->config,
                                    "secrets.command_timeout_seconds"),
        NULL);
}

/*
 * Writes the credential and remembers where it went.
 *
 * The path goes in the config; the value never does.  What is
 * deliberately *not* written back is the granted scope list -- it lives
 * in the token file, and copying it over `scopes:` would quietly rewrite
 * what the person asked for into what they were given, so re-connecting
 * later would ask for less each time.
 */
static gboolean
store_connector_token(ClawtDaemon      *self,
                      const gchar      *name,
                      ClawtOauthToken  *token,
                      GError          **error)
{
    g_autofree gchar *secrets_dir =
        clawt_config_get_path_value(self->config, "secrets.dir");
    g_autofree gchar *path = clawt_connector_token_path(secrets_dir, name);
    ClawtIntegrationConfig *instance =
        clawt_config_get_integration(self->config, name);

    if (instance == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                            "the integration went away while connecting");
        return FALSE;
    }

    if (!clawt_ensure_dir(secrets_dir, 0700, error))
        return FALSE;

    if (!clawt_oauth_token_save(token, path, error))
        return FALSE;

    clawt_integration_config_set_string(instance, NULL, "token_file", path);

    if (!clawt_config_save(self->config, error))
        return FALSE;

    return clawt_daemon_reload(self, error);
}

/*
 * Answers whoever is waiting, or remembers the answer for whoever asks
 * next.  A client may call `await` before or after the flow finishes and
 * must get the same answer either way -- a person who walked away and
 * came back should not find that the result was delivered to nobody.
 */
static void
connector_flow_settle(ConnectorFlow *flow, gboolean ok, const gchar *message)
{
    flow->settled = TRUE;
    flow->ok = ok;
    flow->settled_at = g_get_real_time() / G_USEC_PER_SEC;

    g_free(flow->message);
    flow->message = g_strdup(message);

    if (flow->waiter == NULL)
        return;

    if (ok) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "connected");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, flow->name);
        json_builder_end_object(builder);

        clawt_ipc_pending_respond(
            flow->waiter,
            clawt_ipc_response_new(clawt_ipc_pending_get_request(flow->waiter),
                                   json_builder_get_root(builder)));
    } else {
        clawt_ipc_pending_respond(
            flow->waiter,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(flow->waiter),
                                CLAWT_ERROR_AUTH,
                                message != NULL ? message
                                                : "the flow did not complete"));
    }

    flow->waiter = NULL;

    g_hash_table_remove(flow->daemon->connector_flows, flow->id);
}

static void
connector_flow_finish_token(ConnectorFlow *flow, ClawtOauthToken *token)
{
    g_autoptr(GError) error = NULL;

    if (!store_connector_token(flow->daemon, flow->name, token, &error)) {
        connector_flow_settle(flow, FALSE, error->message);
        return;
    }

    clawt_event_bus_emit(flow->daemon->bus, "integration.changed", flow->name);
    connector_flow_settle(flow, TRUE, NULL);
}

/*
 * The client waiting to be shown a user code.
 *
 * Separate from the flow because it is answered once, the moment the
 * provider hands over the codes -- long before the flow itself settles.
 */
typedef struct {
    ConnectorFlow   *flow;
    ClawtIpcPending *pending;
} BeginWait;

/*
 * Deletes the credential and forgets where it was.
 *
 * Both halves, and in that order: a config still naming a token_file
 * that is gone reads as connected right up until something tries to use
 * it.
 */
static gboolean
forget_connector_token(ClawtDaemon *self, const gchar *name, GError **error)
{
    ClawtIntegrationConfig *instance =
        clawt_config_get_integration(self->config, name);
    const gchar *token_file;

    if (instance == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                            "there is no integration called that");
        return FALSE;
    }

    token_file = clawt_integration_config_get_string(instance, NULL,
                                                     "token_file");

    if (token_file != NULL)
        g_unlink(token_file);

    clawt_integration_config_set_string(instance, NULL, "token_file", NULL);

    if (!clawt_config_save(self->config, error))
        return FALSE;

    return clawt_daemon_reload(self, error);
}

typedef struct {
    gchar           *name;
    ClawtIpcPending *pending;
} RevokeJob;

static void
on_connector_revoked(GObject *source, GAsyncResult *result, gpointer user_data)
{
    RevokeJob *job = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GError) error = NULL;
    gboolean told = clawt_oauth_revoke_finish(result, &error);

    /*
     * Not an error either way.  The credential is already gone from
     * here, which is what was asked for; whether the provider was
     * reachable is a separate fact and is reported as one.
     */
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "forgotten");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_set_member_name(builder, "told_provider");
    json_builder_add_boolean_value(builder, told);

    if (!told)
        add_string_member(builder, "note", error->message);

    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        job->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(job->pending),
                               json_builder_get_root(builder)));

    g_free(job->name);
    g_free(job);
}

static void
on_connector_polled(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ConnectorFlow *flow = user_data;
    g_autoptr(ClawtOauthToken) token = NULL;
    g_autoptr(GError) error = NULL;

    token = clawt_oauth_device_poll_finish(result, &error);

    if (token == NULL) {
        connector_flow_settle(flow, FALSE, error->message);
        return;
    }

    connector_flow_finish_token(flow, token);
}

static void
on_connector_exchanged(GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
    ConnectorFlow *flow = user_data;
    g_autoptr(ClawtOauthToken) token = NULL;
    g_autoptr(GError) error = NULL;

    token = clawt_oauth_exchange_finish(result, &error);

    if (token == NULL) {
        connector_flow_settle(flow, FALSE, error->message);
        return;
    }

    connector_flow_finish_token(flow, token);
}

static void
on_connector_redirected(GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
    ConnectorFlow *flow = user_data;
    g_autofree gchar *code = NULL;
    g_autoptr(GError) error = NULL;

    code = clawt_oauth_await_redirect_finish(result, &error);

    if (code == NULL) {
        connector_flow_settle(flow, FALSE, error->message);
        return;
    }

    clawt_oauth_exchange_async(flow->token_url, flow->client_id,
                               flow->client_secret, code, flow->redirect_uri,
                               flow->verifier, NULL, on_connector_exchanged,
                               flow);
}

/*
 * Answers the client with the code, then starts polling.
 *
 * The device code is deliberately absent from the reply.  It is the
 * secret half of the pair -- it authorises the exchange -- and the user
 * code is the half meant to be read aloud.  Sending both would put a
 * live credential in every client's memory and in anybody's scrollback.
 */
static void
on_connector_begun(GObject *source, GAsyncResult *result, gpointer user_data)
{
    BeginWait *begin = user_data;
    ConnectorFlow *flow = begin->flow;
    g_autoptr(ClawtDeviceCode) code = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GError) error = NULL;

    code = clawt_oauth_device_begin_finish(result, &error);

    if (code == NULL) {
        clawt_ipc_pending_respond(
            begin->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(begin->pending),
                                CLAWT_ERROR_AUTH, error->message));

        g_hash_table_remove(flow->daemon->connector_flows, flow->id);
        g_free(begin);
        return;
    }

    json_builder_begin_object(builder);
    add_string_member(builder, "flow", flow->id);
    add_string_member(builder, "method", "device");
    add_string_member(builder, "user_code", code->user_code);
    add_string_member(builder, "verification_uri", code->verification_uri);
    add_string_member(builder, "verification_uri_complete",
                      code->verification_uri_complete);
    json_builder_set_member_name(builder, "expires_at");
    json_builder_add_int_value(builder, code->expires_at);
    json_builder_set_member_name(builder, "interval");
    json_builder_add_int_value(builder, code->interval);
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        begin->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(begin->pending),
                               json_builder_get_root(builder)));

    g_free(begin);

    clawt_oauth_device_poll_async(flow->token_url, flow->client_id,
                                  flow->client_secret, code, NULL,
                                  on_connector_polled, flow);
}

/*
 * Drops flows that finished long enough ago that nobody is coming back
 * for the answer.  Without it a daemon that runs for months accumulates
 * one entry per connection attempt that was started and abandoned.
 */
static void
sweep_connector_flows(ClawtDaemon *self)
{
    GHashTableIter iter;
    gpointer value;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    g_hash_table_iter_init(&iter, self->connector_flows);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        ConnectorFlow *flow = value;

        if (flow->settled && now - flow->settled_at > 600)
            g_hash_table_iter_remove(&iter);
    }
}

/* ── Renewal ─────────────────────────────────────────────────────── */

/*
 * A renewal, which may or may not have somebody waiting on it: the timer
 * starts these with no client attached, and `connector.refresh` starts
 * one with a deferred request to answer.
 */
typedef struct {
    ClawtDaemon     *daemon;
    gchar           *name;
    ClawtIpcPending *pending;
} RefreshJob;

static void
refresh_job_free(RefreshJob *job)
{
    g_free(job->name);
    g_free(job);
}

static void
refresh_job_answer(RefreshJob *job, gboolean ok, const gchar *message)
{
    g_autoptr(JsonBuilder) builder = NULL;

    if (job->pending == NULL)
        return;

    if (!ok) {
        clawt_ipc_pending_respond(
            job->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(job->pending),
                                CLAWT_ERROR_AUTH, message));
        return;
    }

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "renewed");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        job->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(job->pending),
                               json_builder_get_root(builder)));
}

static void
on_connector_refreshed(GObject *source, GAsyncResult *result,
                       gpointer user_data)
{
    RefreshJob *job = user_data;
    g_autoptr(ClawtOauthToken) token = NULL;
    g_autoptr(GError) error = NULL;

    token = clawt_oauth_refresh_finish(result, &error);

    if (token == NULL) {
        g_warning("could not renew the credential for '%s': %s", job->name,
                  error->message);
        refresh_job_answer(job, FALSE, error->message);
        refresh_job_free(job);
        return;
    }

    /*
     * A provider that issues no new refresh token expects the old one to
     * keep working, and storing the blank would mean the next renewal
     * has nothing to renew with -- so the person is asked to authorise
     * again for a reason they cannot see.
     */
    if (token->refresh_token == NULL) {
        g_autofree gchar *secrets_dir =
            clawt_config_get_path_value(job->daemon->config, "secrets.dir");
        g_autofree gchar *path =
            clawt_connector_token_path(secrets_dir, job->name);
        g_autoptr(ClawtOauthToken) previous =
            clawt_oauth_token_load(path, NULL);

        if (previous != NULL && previous->refresh_token != NULL)
            token->refresh_token = g_strdup(previous->refresh_token);
    }

    if (!store_connector_token(job->daemon, job->name, token, &error)) {
        g_warning("could not store the renewed credential for '%s': %s",
                  job->name, error->message);
        refresh_job_answer(job, FALSE, error->message);
    } else {
        g_debug("renewed the credential for connector '%s'", job->name);
        clawt_event_bus_emit(job->daemon->bus, "integration.changed",
                             job->name);
        refresh_job_answer(job, TRUE, NULL);
    }

    refresh_job_free(job);
}

static void
refresh_connector(ClawtDaemon *self, ClawtIntegrationConfig *instance,
                  gint64 margin)
{
    const gchar *name = clawt_integration_config_get_name(instance);
    const gchar *token_file =
        clawt_integration_config_get_string(instance, NULL, "token_file");
    const ClawtConnectorInfo *connector;
    g_autoptr(ClawtOauthToken) token = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autofree gchar *token_url = NULL;
    const gchar *client_id;
    RefreshJob *job;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    if (token_file == NULL)
        return;

    token = clawt_oauth_token_load(token_file, NULL);

    if (token == NULL || token->refresh_token == NULL)
        return;

    if (!clawt_oauth_token_is_expired(token, now, margin))
        return;

    binding = connector_binding(self, name, &connector, NULL);

    if (binding == NULL)
        return;

    client_id = clawt_integration_binding_get_string(binding, "client_id");
    token_url = clawt_connector_resolve_url(
        connector, connector->token_url,
        clawt_integration_binding_get_string(binding, "instance"));

    if (token_url == NULL || client_id == NULL)
        return;

    job = g_new0(RefreshJob, 1);
    job->daemon = self;
    job->name = g_strdup(name);

    {
        g_autofree gchar *secret = connector_client_secret(self, binding);

        clawt_oauth_refresh_async(token_url, client_id, secret,
                                  token->refresh_token, NULL,
                                  on_connector_refreshed, job);
    }
}

/*
 * Renewal happens here rather than when a tool server starts, because a
 * server started with a token that expires forty minutes later keeps
 * running with it.  The agent's first failure would then come long after
 * anything connected the two.
 */
static gboolean
on_connector_refresh_tick(gpointer user_data)
{
    ClawtDaemon *self = user_data;
    GPtrArray *integrations = clawt_config_get_integrations(self->config);
    gint64 margin = clawt_config_get_int(self->config,
                                         "connectors.refresh_margin_seconds");
    guint i;

    for (i = 0; integrations != NULL && i < integrations->len; i++) {
        ClawtIntegrationConfig *instance = g_ptr_array_index(integrations, i);

        if (g_strcmp0(clawt_integration_config_get_type_id(instance),
                      "connector") != 0)
            continue;

        if (!clawt_integration_config_get_enabled(instance))
            continue;

        refresh_connector(self, instance, margin);
    }

    return G_SOURCE_CONTINUE;
}

/*
 * Creating an agent, once.
 *
 * Two callers: the agent.create frame, and the orchestration tool a
 * chief-of-staff uses. They disagreed about nothing yet, which is the
 * moment to make sure they cannot -- the designer's commit path is on
 * record for claiming to be "the same path as creating an agent by hand"
 * while skipping the validation around it.
 *
 * @fields is keyed by configuration key, not by whatever each caller
 * calls things, so the translation stays with the caller that has the
 * vocabulary.
 */
static ClawtAgentConfig *
daemon_create_agent(ClawtDaemon  *self,
                    const gchar  *agent_id,
                    GHashTable   *fields,
                    const gchar  *purpose,
                    gboolean     *purpose_landed,
                    GError      **error)
{
    ClawtAgentConfig *created;
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    if (purpose_landed != NULL)
        *purpose_landed = FALSE;

    created = clawt_config_add_agent(self->config, agent_id, error);

    if (created == NULL)
        return NULL;

    if (fields != NULL) {
        g_hash_table_iter_init(&iter, fields);

        while (g_hash_table_iter_next(&iter, &key, &value)) {
            /*
             * Through the schema, not straight to a scalar: a
             * STRING_LIST key written as a scalar is read back as its
             * default, so `computer.host.allow_paths` given at creation
             * reached the sandbox as an empty allowlist.
             */
            if (value != NULL)
                clawt_agent_config_set_from_string(created, key, value);
        }
    }

    /*
     * The persona, into SOUL.org, before the save below reloads and
     * scaffolds every agent from the templates.
     *
     * SOUL.org is the one file that decides what an agent does when
     * nobody has told it what to do, and scaffolding never overwrites --
     * so writing it afterwards would land on a file that already exists
     * and lose the persona all over again.  Not `persona.system_prompt`:
     * an inline prompt *replaces* the identity files rather than adding
     * to them, and an agent without its TOOLS.org spends its first turns
     * finding out what computer it has.
     */
    if (purpose != NULL && *purpose != '\0') {
        if (!clawt_workspace_scaffold_with_mission(created, purpose,
                                                    purpose_landed, error)) {
            clawt_config_remove_agent(self->config, agent_id);
            return NULL;
        }
    }

    /*
     * After the fields, so it sees what was written rather than what was
     * asked for -- and rolled back rather than saved, because an agent
     * that exists and cannot work is worse than one that was never
     * added: somebody has to find out it is broken and then remove it.
     */
    if (!clawt_agent_config_validate_computer(created, error)) {
        clawt_config_remove_agent(self->config, agent_id);
        return NULL;
    }

    if (!clawt_config_save(self->config, error))
        return NULL;

    /*
     * Reloaded so the agent exists as an object rather than only as a
     * line in a file: whoever created it is about to start it.
     */
    if (!clawt_daemon_reload(self, error))
        return NULL;

    clawt_agent_manager_load(self->agents, NULL);
    clawt_event_bus_emit(self->bus, "agent.created", agent_id);

    /*
     * The config object from before the reload belongs to a ClawtConfig
     * that has just been freed.
     */
    return clawt_config_get_agent(self->config, agent_id);
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

/*
 * The bearer token a remote client must present.
 *
 * `daemon.token_file` wins when it names a readable file, because that is
 * a token the person chose and may have copied to the other machine
 * already.  Otherwise one is generated into the state directory and kept.
 *
 * Generating rather than refusing is the point.  A TCP listener without a
 * token is refused outright by ClawtIpcServer, so leaving the token to be
 * configured by hand would mean `daemon.tailscale: true` -- a default --
 * failing daemon start on every machine that had never set one.  The
 * failure mode of the alternative is worse in the other direction: a
 * default that quietly listened with no authentication at all.
 */
static gchar *
ensure_tcp_token(ClawtDaemon *self, GError **error)
{
    g_autofree gchar *configured =
        clawt_config_get_path_value(self->config, "daemon.token_file");
    g_autofree gchar *path = NULL;
    gchar *token = NULL;

    if (configured != NULL && *configured != '\0') {
        if (!g_file_get_contents(configured, &token, NULL, error)) {
            g_prefix_error(error, "daemon.token_file %s: ", configured);
            return NULL;
        }

        g_strstrip(token);

        if (*token == '\0') {
            g_free(token);
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID,
                        "daemon.token_file %s is empty", configured);
            return NULL;
        }

        return token;
    }

    path = g_build_filename(self->state_dir, "tcp-token", NULL);

    if (g_file_get_contents(path, &token, NULL, NULL)) {
        g_strstrip(token);

        if (*token != '\0')
            return token;

        /* Truncated by a crash mid-write; make a fresh one. */
        g_clear_pointer(&token, g_free);
    }

    token = clawt_generate_token(error);

    if (token == NULL)
        return NULL;

    /*
     * 0600, like every other secret the daemon writes.  It is the whole
     * authentication for anything reaching the daemon over the network.
     */
    if (!clawt_write_file_atomic(path, token, -1, 0600, FALSE, error)) {
        g_free(token);
        return NULL;
    }

    return token;
}

gboolean
clawt_daemon_start(ClawtDaemon *self, GError **error)
{
    g_autofree gchar *transcript_dir = NULL;
    g_autofree gchar *tailnet_address = NULL;
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

    /*
     * The state directory as a repository, from the first start.
     *
     * Asked for rather than left to a command somebody has to remember:
     * the workspaces, the org files and clawtilla.yaml are worth having
     * a history of, and the moment to write the ignore file that keeps
     * credentials out of that history is before there is anything to
     * commit -- not after.
     */
    {
        g_autoptr(GError) git_error = NULL;
        g_autofree gchar *ignore = NULL;
        gboolean created = FALSE;

        if (!prepare_state_git(self->state_dir,
                               clawt_config_get_boolean(self->config,
                                                        "daemon.git"),
                               &created, &ignore, &git_error))
            g_warning("state: %s", git_error->message);
        else if (created)
            g_message("state: %s is now a git repository; %s keeps "
                      "credentials, tokens and databases out of it",
                      self->state_dir, ignore);
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

    {
        g_autofree gchar *decision_path =
            g_build_filename(self->state_dir, "decisions.db", NULL);
        g_autoptr(GError) decision_error = NULL;

        self->decisions = clawt_decision_store_new(decision_path,
                                                   &decision_error);

        /*
         * A store that cannot open is a warning, not a refusal to
         * start.  Losing the decision inbox is bad; losing the fleet
         * because of it is worse, and every other surface still works.
         */
        if (self->decisions == NULL)
            g_warning("decisions: %s", decision_error->message);
    }

    {
        /*
         * Download progress reaches clients as ordinary events, so a
         * progress bar is a fold over the same stream everything else
         * uses -- and a client that connects mid-download is told where
         * it has got to by image.vm_list, rather than showing nothing
         * until it finishes.
         */
        g_autofree gchar *image_dir =
            clawt_config_get_path_value(self->config, "defaults.image_dir");

        self->vm_images = clawt_vm_image_store_new(image_dir);
        g_signal_connect(self->vm_images, "progress",
                         G_CALLBACK(on_image_progress), self);
        g_signal_connect(self->vm_images, "finished",
                         G_CALLBACK(on_image_finished), self);
    }

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

    /*
     * And the direct rooms, which nothing in the config names.  They are
     * made on demand, so without this a conversation between two agents
     * was invisible after a restart until they happened to speak again.
     */
    clawt_room_manager_load_direct(self->rooms);

    self->tasks = clawt_task_manager_new();
    self->guard = clawt_loop_guard_new();
    self->usage = clawt_usage_new();
    configure_limits(self);

    self->notifier = clawt_notifier_new(self->config);

    {
        g_autofree gchar *routine_state =
            g_build_filename(self->state_dir, "routines.yaml", NULL);

        self->routines = clawt_routine_runner_new(self->config,
                                                  routine_state);
        clawt_routine_runner_set_run_func(self->routines, run_routine, self);

        /*
         * Before the tick starts, so a routine whose time passed while
         * the daemon was down is dealt with once rather than found by
         * the first tick and treated as due right now.
         */
        clawt_routine_runner_catch_up(self->routines);
        clawt_routine_runner_start(self->routines, self->main_context);
    }

    {
        g_autofree gchar *pods =
            clawt_config_get_path_value(self->config, "daemon.automation_dir");
        g_autoptr(GError) local = NULL;

        self->automation = clawt_automation_new(self->bus, pod_action, self);

        /*
         * A failure here disables the automation and nothing else. Pods
         * are a convenience on top of a fleet; a fleet that would not
         * start because one of them had a syntax error would be the
         * convenience taking the thing it decorates down with it.
         */
        if (!clawt_automation_load(self->automation, pods, &local))
            g_warning("automation: %s", local->message);
    }

    /*
     * The manager has emitted `agent-state-changed` since it was
     * written and nothing had ever connected to it, so an agent that
     * crashed produced no event at all: clients found out by polling,
     * and nobody found out at 3am. It is now both an event on the bus
     * and, for the states nobody asked for, a notification.
     */
    g_signal_connect(self->agents, "agent-state-changed",
                     G_CALLBACK(on_agent_state_changed), self);
    g_signal_connect(self->tasks, "task-changed",
                     G_CALLBACK(on_task_changed), self);

    self->router = clawt_mailbox_router_new(self->agents, self->rooms,
                                            self->guard);
    clawt_mailbox_router_set_event_bus(self->router, self->bus);

    self->mcp_tools = clawt_mcp_tools_new(self->agents, self->tasks,
                                          self->guard);
    clawt_mcp_tools_set_deliver_func(self->mcp_tools, deliver_for_tools,
                                     self, NULL);
    clawt_mcp_tools_set_room_manager(self->mcp_tools, self->rooms);

    /*
     * Where a file an agent sends its operator is kept.
     *
     * Under the daemon's own state directory rather than the exchange:
     * the exchange is mounted into computers and readable by every agent
     * that shares it, and a message's attachment belongs to the
     * conversation rather than to the fleet.
     */
    {
        g_autofree gchar *state_dir =
            clawt_config_get_path_value(self->config, "daemon.state_dir");
        g_autofree gchar *dir = (state_dir != NULL)
            ? g_build_filename(state_dir, "attachments", NULL) : NULL;

        clawt_mcp_tools_set_attachment_dir(self->mcp_tools, dir);
        g_free(self->attachment_dir);
        self->attachment_dir = g_steal_pointer(&dir);
    }

    /*
     * The fleet tools are not offered at all without these, whatever an
     * agent's permissions say -- a library embedded without a daemon has
     * no fleet to add to, and a tool that is listed and then fails
     * teaches an agent to keep trying.
     */
    clawt_mcp_tools_set_create_agent_func(self->mcp_tools,
                                          create_agent_for_tools, self, NULL);
    clawt_mcp_tools_set_ask_decision_func(self->mcp_tools,
                                          file_decision_for_tools, self,
                                          NULL);
    clawt_mcp_tools_set_image_store(self->mcp_tools, self->vm_images);

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

    {
        guint16 port =
            (guint16)clawt_config_get_int(self->config, "daemon.tcp_port");
        gboolean want_network;

        /*
         * A command-line override replaces the configuration wholesale
         * rather than adding to it.  `clawtillad --bind 10.0.0.5:9000`
         * that also brought up the tailnet address would be listening
         * somewhere the person did not ask for and did not see.
         */
        if (self->bind_override) {
            want_network = self->bind_specs != NULL &&
                           self->bind_specs->len > 0;
        } else {
            gboolean tcp = clawt_config_get_boolean(self->config,
                                                     "daemon.tcp_enabled");

            /*
             * Looked up before deciding, because a token is only worth
             * generating for a listener that will exist: on a machine
             * without Tailscale this whole block is a no-op and writing
             * a secret for it would be litter.
             */
            if (clawt_config_get_boolean(self->config, "daemon.tailscale"))
                tailnet_address = clawt_tailscale_find_address();

            want_network = tcp || tailnet_address != NULL;
        }

        if (want_network) {
            g_autofree gchar *token = ensure_tcp_token(self, error);

            if (token == NULL) {
                clawt_link_server_stop(self->link_server);

                if (self->main_context != NULL)
                    g_main_context_pop_thread_default(self->main_context);
                return FALSE;
            }

            clawt_ipc_server_set_token(self->ipc_server, token);

            if (self->bind_override) {
                guint spec_index;

                for (spec_index = 0; spec_index < self->bind_specs->len;
                     spec_index++) {
                    const BindSpec *spec =
                        g_ptr_array_index(self->bind_specs, spec_index);

                    /*
                     * Never optional.  Somebody wrote it on a command
                     * line, so a daemon that could not bind it and
                     * carried on regardless would be running somewhere
                     * other than where they asked.
                     */
                    clawt_ipc_server_add_listener(self->ipc_server,
                                                  spec->host, spec->port,
                                                  FALSE);
                }
            } else {
                if (clawt_config_get_boolean(self->config,
                                             "daemon.tcp_enabled")) {
                    const gchar *configured =
                        clawt_config_get_string(self->config,
                                                "daemon.tcp_address");

                    if (configured != NULL && *configured != '\0')
                        clawt_ipc_server_add_listener(self->ipc_server,
                                                      configured, port,
                                                      FALSE);
                }

                if (tailnet_address != NULL)
                    clawt_ipc_server_add_listener(self->ipc_server,
                                                  tailnet_address, port,
                                                  TRUE);
            }

            clawt_ipc_server_set_tls(
                self->ipc_server,
                clawt_config_get_string(self->config, "daemon.tls_cert"),
                clawt_config_get_string(self->config, "daemon.tls_key"));
        }
    }

    if (!clawt_ipc_server_start(self->ipc_server, error)) {
        clawt_link_server_stop(self->link_server);

        if (self->main_context != NULL)
            g_main_context_pop_thread_default(self->main_context);
        return FALSE;
    }

    /*
     * Announced after the bind, and only if it took.  Saying it first
     * meant a daemon whose tailnet port was already held printed that it
     * was reachable there and then warned that it was not, one line
     * apart.
     *
     * Said at all because a daemon that quietly became reachable from
     * another machine is something a person should learn from its own
     * output rather than from a port scan.
     */
    if (tailnet_address != NULL &&
        clawt_ipc_server_is_listening_on(
            self->ipc_server, tailnet_address,
            (guint16)clawt_config_get_int(self->config, "daemon.tcp_port")))
        g_message("ipc: reachable on the tailnet at %s:%" G_GINT64_FORMAT
                  " -- `clawtilla daemon token` prints the token a remote "
                  "client needs", tailnet_address,
                  clawt_config_get_int(self->config, "daemon.tcp_port"));

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

    /*
     * Started here rather than earlier, because start can still refuse
     * after the components are built -- a second daemon on the same
     * fleet is turned away at the socket.  A timer armed before that
     * point belonged to a daemon that never ran, and nothing would take
     * it down again: clawt_daemon_stop() returns early when `running`
     * was never set.
     *
     * clawt_timeout_add_seconds() rather than g_timeout_add_seconds():
     * the latter attaches to the global default context, so in an
     * embedded daemon this would never fire and every credential would
     * quietly be left to expire.
     */
    self->connector_refresh = clawt_timeout_add_seconds(
        60, on_connector_refresh_tick, self);

    self->running = TRUE;

    /*
     * The one caller with nobody to answer.  Every other site that
     * re-renders the fleet hands the refusals back in its reply; a start
     * has no reply, so it says so on the console -- once, naming every
     * agent, after the per-agent warnings rather than among them.  A
     * start that refused to come up over one operator-typed block would
     * be a far worse failure than a fleet that starts and says which
     * agents are running against a stale config.
     */
    {
        g_autoptr(GPtrArray) refusals = render_refusals_new();
        g_autoptr(GString) names = g_string_new(NULL);
        guint refused;

        render_all_agents_into(self, refusals);

        for (refused = 0; refused < refusals->len; refused++) {
            RenderRefusal *refusal = g_ptr_array_index(refusals, refused);

            if (names->len > 0)
                g_string_append(names, ", ");

            g_string_append(names, refusal->agent_id);
        }

        if (refusals->len == 1)
            g_warning("%s is starting against the config.yaml it already "
                      "had: clawtilla would not render its files",
                      names->str);
        else if (refusals->len > 1)
            g_warning("%u agents are starting against the config.yaml they "
                      "already had, because clawtilla would not render "
                      "their files: %s", refusals->len, names->str);
    }

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

    /*
     * The model cache is not warmed here.
     *
     * It was, so that the first client to open a model list got the
     * real one -- but that made every daemon start call five provider
     * APIs whether or not anybody was ever going to look, and it made
     * `make test` reach the network from every daemon fixture, which is
     * the one thing the suite is not allowed to do. It is warmed on the
     * first `model.list refresh: true` instead, which is a client
     * actually asking.
     */
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

    if (self->connector_refresh != NULL) {
        g_source_destroy(self->connector_refresh);
        g_clear_pointer(&self->connector_refresh, g_source_unref);
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

/*
 * @refusals, when it is not %NULL, comes back holding one #RenderRefusal
 * for every agent whose files this reload could not write.  Those are not
 * a failed reload -- the new configuration is in force, and the rest of
 * the fleet was rendered from it -- so they are reported alongside
 * success rather than instead of it.
 */
static gboolean
daemon_reload(ClawtDaemon *self, GPtrArray *refusals, GError **error)
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

    /*
     * Dropped rather than rebuilt here: connectors.dir may itself have
     * changed, and nothing needs the catalogue until something asks.
     */
    g_clear_pointer(&self->connector_catalog, g_ptr_array_unref);

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
    clawt_room_manager_load_direct(self->rooms);

    /*
     * The notifier holds a reference too, and this is also when it
     * resolves credentials -- so a token rotated in the file reaches it
     * on a reload rather than on a restart.
     */
    if (self->notifier != NULL)
        clawt_notifier_reload(self->notifier, self->config);

    if (self->routines != NULL)
        clawt_routine_runner_set_config(self->routines, self->config);

    /*
     * Files are re-rendered for running agents too, so a restart picks up
     * the change -- but nothing is restarted here.  A reload that
     * interrupted every agent mid-turn would make editing one description
     * cost the whole fleet's work.
     */
    render_all_agents_into(self, refusals);

    clawt_event_bus_emit(self->bus, "daemon.reloaded", NULL);
    g_signal_emit(self, signals[SIGNAL_RELOADED], 0);

    return TRUE;
}

gboolean
clawt_daemon_reload(ClawtDaemon *self, GError **error)
{
    return daemon_reload(self, NULL, error);
}


/* ── The client surface ──────────────────────────────────────────── */

/*
 * Every per-agent option, with the value this agent has for it.
 *
 * Walked from the schema rather than listed, which is the same rule the
 * integrations and the starter config already follow: a hand-written list
 * of an option's keys drifts silently, and the symptom is a setting that
 * is accepted, reported as saved, and then ignored at the default.
 *
 * Reported as strings throughout. A client puts these into form fields
 * and hands them back to `agent.set`, which parses them against the same
 * schema -- so a second opinion here about what an integer looks like
 * would be a second parser to disagree with.
 */
static void
add_agent_settings(JsonBuilder *builder, ClawtAgent *agent)
{
    ClawtAgentConfig *config = clawt_agent_get_config(agent);
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                       g_free, NULL);
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;

    schema = clawt_config_schema_get(&n_entries);

    json_builder_set_member_name(builder, "settings");
    json_builder_begin_object(builder);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *key;

        /*
         * What this option is called inside an agent block, or nothing
         * if it is not settable there. The rule lives in the schema so
         * that a client building an editor derives the same set -- it
         * did not, once, and the daemon and the web client disagreed
         * about whether nine options existed.
         */
        key = clawt_config_schema_agent_name(entry);

        if (key == NULL)
            continue;

        /*
         * Two entries resolving to one agent-relative name would emit
         * the member twice, and json-glib keeps the last -- the same
         * silent overwrite this project already refuses in generated
         * YAML.
         */
        if (g_hash_table_contains(seen, key)) {
            g_warning("agent.show: two schema keys claim '%s'; "
                      "reporting the first", key);
            continue;
        }

        g_hash_table_add(seen, g_strdup(key));

        json_builder_set_member_name(builder, key);

        if (entry->type == CLAWT_SCHEMA_SECRET) {
            g_autoptr(ClawtSecretRef) secret =
                clawt_agent_config_get_secret(config, key);

            /*
             * Whether one is configured, never what it is. A client
             * needs only that much to offer replacing it, and a value
             * put into an IPC response is a value in every client's
             * memory and in every transcript of this exchange.
             */
            json_builder_add_string_value(builder,
                                          secret != NULL ? "(set)" : "");
            continue;
        }

        switch (entry->type) {
        case CLAWT_SCHEMA_BOOLEAN:
            json_builder_add_string_value(
                builder,
                clawt_agent_config_get_boolean(config, key) ? "true" : "false");
            break;

        case CLAWT_SCHEMA_INT: {
            g_autofree gchar *text = g_strdup_printf(
                "%" G_GINT64_FORMAT, clawt_agent_config_get_int(config, key));

            json_builder_add_string_value(builder, text);
            break;
        }

        case CLAWT_SCHEMA_ENUM: {
            const gchar *nick = clawt_agent_config_get_string(config, key);

            json_builder_add_string_value(builder, nick != NULL ? nick : "");
            break;
        }

        case CLAWT_SCHEMA_STRING_LIST: {
            g_auto(GStrv) values =
                clawt_agent_config_get_string_list(config, key);
            g_autofree gchar *joined =
                (values != NULL) ? g_strjoinv(", ", values) : NULL;

            json_builder_add_string_value(builder,
                                          joined != NULL ? joined : "");
            break;
        }

        default: {
            const gchar *value = clawt_agent_config_get_string(config, key);

            json_builder_add_string_value(builder, value != NULL ? value : "");
            break;
        }
        }
    }

    json_builder_end_object(builder);
}

/*
 * An answered decision goes back to whoever asked.
 *
 * Without this the inbox is a suggestion box: the operator answers into
 * the void and the agent never learns.  Sent as an ordinary message so
 * it arrives through the machinery everything else uses and costs the
 * agent a turn, which is the point -- an answer that did not interrupt
 * anything would not change what the agent does next.
 *
 * From `user`, because it *is* from the person, and carrying the task
 * id so an answer arriving after the agent has moved on can still be
 * attached to what it was about.  Depth 0: this starts a new exchange
 * rather than continuing the one that raised the question, and stamping
 * it deeper would spend the hop budget on the operator's own reply.
 */
static void
deliver_decision_answer(ClawtDaemon *self, ClawtDecision *decision)
{
    g_autofree gchar *body = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *agent;

    if (self->router == NULL || decision == NULL)
        return;

    agent = clawt_decision_get_agent(decision);

    if (agent == NULL || *agent == '\0')
        return;

    /*
     * The question is repeated back.  The agent asked it some time ago
     * and may have handled a hundred messages since; an answer of
     * "after the release" with nothing around it is unattributable.
     */
    body = g_strdup_printf(
        "[clawtilla] Your decision was answered.\n\n"
        "You asked: %s\n"
        "The answer is: %s\n\n"
        "This replaces the default you said you would take (%s). If you "
        "have already acted on that default, say so rather than silently "
        "redoing the work.",
        clawt_decision_get_question(decision),
        clawt_decision_get_answer(decision),
        clawt_decision_get_default(decision) != NULL
            ? clawt_decision_get_default(decision)
            : "none stated");

    if (clawt_mailbox_router_send_to(self->router, "user", agent, body,
                                     clawt_decision_get_task(decision),
                                     0, &error) < 0)
        g_warning("decisions: could not tell %s its answer: %s", agent,
                  error != NULL ? error->message : "unknown");
}

/*
 * One decision on the wire.
 *
 * `urgent` and `settled_by_default` are computed here rather than left
 * to each client, because both are the same rule about the same clock
 * and two clients each deriving them would differ exactly once -- on
 * the item whose deadline had just passed, which is the one that
 * matters.  The raw deadline goes too, so a client can still say when.
 */
static void
add_decision_object(JsonBuilder   *builder,
                    ClawtDecision *decision,
                    gint64         now)
{
    const gchar * const *options = clawt_decision_get_options(decision);

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, clawt_decision_get_id(decision));

    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder,
                                  clawt_decision_get_agent(decision));

    json_builder_set_member_name(builder, "question");
    json_builder_add_string_value(builder,
                                  clawt_decision_get_question(decision));

    json_builder_set_member_name(builder, "options");
    json_builder_begin_array(builder);

    {
        guint i;

        for (i = 0; options != NULL && options[i] != NULL; i++)
            json_builder_add_string_value(builder, options[i]);
    }

    json_builder_end_array(builder);

    json_builder_set_member_name(builder, "default");
    json_builder_add_string_value(builder,
                                  clawt_decision_get_default(decision));

    json_builder_set_member_name(builder, "default_reason");
    json_builder_add_string_value(
        builder, clawt_decision_get_default_reason(decision));

    json_builder_set_member_name(builder, "task");
    json_builder_add_string_value(builder,
                                  clawt_decision_get_task(decision));

    json_builder_set_member_name(builder, "answer");
    json_builder_add_string_value(builder,
                                  clawt_decision_get_answer(decision));

    json_builder_set_member_name(builder, "reversible_until");
    json_builder_add_int_value(
        builder, clawt_decision_get_reversible_until(decision));

    json_builder_set_member_name(builder, "created_at");
    json_builder_add_int_value(builder,
                               clawt_decision_get_created_at(decision));

    json_builder_set_member_name(builder, "state");
    json_builder_add_int_value(builder,
                               (gint)clawt_decision_get_state(decision));

    json_builder_set_member_name(builder, "urgent");
    json_builder_add_boolean_value(builder,
                                   clawt_decision_is_urgent(decision, now));

    json_builder_set_member_name(builder, "settled_by_default");
    json_builder_add_boolean_value(
        builder, clawt_decision_default_has_taken_effect(decision, now));

    json_builder_end_object(builder);
}

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

    /*
     * Which room the operator's conversation with this agent is.
     *
     * Reported rather than derived, because how a direct room is named
     * is the daemon's business and a client that takes "dm:a:b" apart
     * breaks when that changes -- the comment saying so is already in
     * clawt-window.c.  A client needs it to tell a message meant for the
     * person from the fleet's own peer traffic, and it needs it for
     * agents it has *never opened*, which is exactly the agent an unread
     * count exists for.  Asking room.history would resolve one agent and
     * create the room as a side effect.
     */
    {
        g_autofree gchar *dm =
            clawt_room_manager_direct_id("user", clawt_agent_get_id(agent));

        json_builder_set_member_name(builder, "dm_room");
        json_builder_add_string_value(builder, dm);
    }

    /*
     * How the agent looks, for the clients that draw a face beside its
     * turns.  Both keys were declared in the schema from the beginning
     * and read by nothing at all -- a config surface that existed and
     * reached no code, which is the same "no caller" gap this codebase
     * has already found twice.
     *
     * Neither is required: a client with neither derives the initials
     * and a colour from the name, which is what makes an avatar cost
     * nothing to have and these two an improvement rather than a
     * prerequisite.
     */
    add_string_member(builder, "avatar",
                      clawt_agent_config_get_string(config, "avatar"));
    add_string_member(builder, "color",
                      clawt_agent_config_get_string(config, "color"));

    /*
     * Reported so a client can show it beside chief_of_staff. The two
     * are separate settings and the obvious-sounding one is not the one
     * that grants the tool -- which is how somebody enabled the wrong
     * switch and was told by their own agent that it could not create
     * agents.
     */
    /*
     * The team and the standing in it, so a client can group the sidebar
     * and an inspector can show what an agent may assign.
     */
    json_builder_set_member_name(builder, "team");
    json_builder_add_string_value(
        builder, clawt_agent_config_get_string(clawt_agent_get_config(agent),
                                               "team"));

    json_builder_set_member_name(builder, "team_role");
    json_builder_add_string_value(
        builder,
        clawt_team_role_of(clawt_agent_get_config(agent)) == CLAWT_TEAM_LEAD
            ? "lead" : "member");

    json_builder_set_member_name(builder, "manage_fleet");
    json_builder_add_boolean_value(
        builder, clawt_agent_config_get_boolean(clawt_agent_get_config(agent),
                                                "tools.manage_fleet"));

    json_builder_set_member_name(builder, "connected");
    json_builder_add_boolean_value(
        builder, clawt_agent_get_link(agent) != NULL &&
                 clawt_link_is_open(clawt_agent_get_link(agent)));

    json_builder_set_member_name(builder, "computer");
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(
                     CLAWT_TYPE_COMPUTER_TYPE,
                     clawt_agent_config_get_enum(config, "computer.type")));

    /*
     * Reported for a container agent so a client can show what it will
     * run in, including the default it inherited.  Left out otherwise:
     * no other backend reads it, and reporting a key the agent does not
     * use invites a client to offer editing it.
     */
    if (clawt_agent_config_get_enum(config, "computer.type") ==
        CLAWT_COMPUTER_CONTAINER) {
        json_builder_set_member_name(builder, "image");
        json_builder_add_string_value(
            builder, clawt_agent_config_get_string(config,
                                                   "computer.container.image"));
    }

    /*
     * The VM's disk, size and address, so a client can show and edit
     * them.  Reported only for a VM: no other backend reads these, and
     * reporting a key an agent does not use invites a client to offer
     * editing it.
     *
     * The disk matters most.  clawtilla ships no image and downloads
     * none, so a VM agent with this unset has no disk at all and cannot
     * boot -- which a client has no way to say unless it is told.
     */
    if (clawt_agent_config_get_enum(config, "computer.type") ==
        CLAWT_COMPUTER_VM) {
        g_autofree gchar *cpus = g_strdup_printf(
            "%" G_GINT64_FORMAT,
            clawt_agent_config_get_int(config, "computer.vm.cpus"));
        g_autofree gchar *memory = g_strdup_printf(
            "%" G_GINT64_FORMAT,
            clawt_agent_config_get_int(config, "computer.vm.memory_mb"));
        g_autofree gchar *disk = g_strdup_printf(
            "%" G_GINT64_FORMAT,
            clawt_agent_config_get_int(config, "computer.vm.disk_gb"));

        json_builder_set_member_name(builder, "vm_image");
        json_builder_add_string_value(
            builder, clawt_agent_config_get_string(config,
                                                   "computer.vm.image"));
        json_builder_set_member_name(builder, "vm_cpus");
        json_builder_add_string_value(builder, cpus);
        json_builder_set_member_name(builder, "vm_memory_mb");
        json_builder_add_string_value(builder, memory);
        json_builder_set_member_name(builder, "vm_disk_gb");
        json_builder_add_string_value(builder, disk);

        /*
         * Reported so the editor can show what this VM actually has.
         * Without it the row would open at the default every time and
         * saving the page would write that back over a size somebody
         * had chosen.
         */
        json_builder_set_member_name(builder, "vm_resolution");
        json_builder_add_string_value(
            builder, clawt_agent_config_get_string(config,
                                                   "computer.vm.resolution"));
        json_builder_set_member_name(builder, "vm_ssh_host");
        json_builder_add_string_value(
            builder, clawt_agent_config_get_string(config,
                                                   "computer.vm.ssh_host"));

        /*
         * Whether the guest gets a GNOME session, and whether the agent
         * may click in it.  Two grants rather than one: an agent that can
         * screenshot but not type is a genuinely useful amount of access
         * and a much smaller thing to hand over.
         */
        json_builder_set_member_name(builder, "desktop_enabled");
        json_builder_add_boolean_value(
            builder, clawt_agent_config_get_boolean(
                         config, "computer.desktop.enabled"));
        json_builder_set_member_name(builder, "desktop_input");
        json_builder_add_boolean_value(
            builder, clawt_agent_config_get_boolean(
                         config, "computer.desktop.allow_input"));
    }

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

    /*
     * What it is doing, so a listing can say more than "running".
     *
     * Note the nesting: these belong to the agent object, not to the
     * credentials one above it. Put a member in the wrong object and it
     * is still valid JSON -- it simply never reaches the client that
     * was looking for it, which is what happened here.
     */
    json_builder_set_member_name(builder, "busy");
    json_builder_add_boolean_value(builder, clawt_agent_get_busy(agent));

    if (clawt_agent_get_activity_peer(agent) != NULL) {
        json_builder_set_member_name(builder, "peer");
        json_builder_add_string_value(builder,
                                      clawt_agent_get_activity_peer(agent));
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

/* ── Integrations ────────────────────────────────────────────────── */

static void
add_key_array(JsonBuilder *builder, const gchar *member,
              const gchar *const *keys)
{
    gsize i;

    json_builder_set_member_name(builder, member);
    json_builder_begin_array(builder);

    for (i = 0; keys != NULL && keys[i] != NULL; i++)
        json_builder_add_string_value(builder, keys[i]);

    json_builder_end_array(builder);
}

/*
 * The keys an integration entry may hold, taken from the schema.
 *
 * Not a list written out here.  There were two of them -- one to read an
 * instance and one to write it -- plus a third in the CLI, and adding
 * `backend` to the schema without adding it to all three produced a
 * notifier that accepted the setting, reported success, saved a file
 * without it and then used the default. Nothing warned. The schema is
 * the single source of truth for what an option *is*; it may as well be
 * the source of truth for what the options *are*.
 */
static gboolean
is_own_key(const gchar *leaf)
{
    static const gchar *const structural[] = {
        "name", "type", "enabled", "scope", "agents", "per_agent", NULL
    };
    gsize i;

    for (i = 0; structural[i] != NULL; i++) {
        if (g_strcmp0(structural[i], leaf) == 0)
            return FALSE;
    }

    return TRUE;
}

static const gchar *
integration_leaf(const ClawtSchemaEntry *entry)
{
    const gchar *leaf;

    if (!g_str_has_prefix(entry->key, "integrations."))
        return NULL;

    leaf = entry->key + strlen("integrations.");

    /* One level only: nothing nested belongs to an entry. */
    if (strchr(leaf, '.') != NULL)
        return NULL;

    return is_own_key(leaf) ? leaf : NULL;
}

/*
 * Every key of an instance except its secrets, which are described
 * rather than read.
 *
 * A secret's *reference* is not a secret -- `{env: MATRIX_TOKEN}` is a
 * variable name, and a client that could not show it would leave a
 * person unable to tell a configured integration from an unconfigured
 * one.  The value behind it never leaves the daemon.
 */
static void
add_integration_values(JsonBuilder            *builder,
                       ClawtIntegrationConfig *instance,
                       const gchar            *agent_id)
{
    const ClawtSchemaEntry *entries;
    gsize n_entries = 0;
    gsize i;

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const gchar *leaf = integration_leaf(&entries[i]);

        if (leaf == NULL)
            continue;

        switch (entries[i].type) {
        case CLAWT_SCHEMA_SECRET: {
            g_autoptr(ClawtSecretRef) ref = NULL;
            g_autofree gchar *described = NULL;

            ref = clawt_integration_config_get_secret(instance, agent_id,
                                                      leaf);

            if (ref == NULL)
                break;

            /*
             * The reference, never the value.  `{env: MATRIX_TOKEN}` is
             * a variable name, and a client that could not show it would
             * leave a person unable to tell a configured integration
             * from an unconfigured one.
             */
            described = clawt_secret_ref_describe(ref);
            json_builder_set_member_name(builder, leaf);
            json_builder_add_string_value(builder, described);
            break;
        }

        case CLAWT_SCHEMA_INT:
            if (!clawt_integration_config_has_key(instance, agent_id, leaf))
                break;

            json_builder_set_member_name(builder, leaf);
            json_builder_add_int_value(
                builder,
                clawt_integration_config_get_int(instance, agent_id, leaf));
            break;

        case CLAWT_SCHEMA_BOOLEAN:
            if (!clawt_integration_config_has_key(instance, agent_id, leaf))
                break;

            json_builder_set_member_name(builder, leaf);
            json_builder_add_boolean_value(
                builder,
                clawt_integration_config_get_boolean(instance, agent_id,
                                                     leaf));
            break;

        case CLAWT_SCHEMA_STRING_LIST: {
            g_auto(GStrv) values = NULL;
            guint k;

            if (!clawt_integration_config_has_key(instance, agent_id, leaf))
                break;

            values = clawt_integration_config_get_string_list(instance,
                                                              agent_id, leaf);

            json_builder_set_member_name(builder, leaf);
            json_builder_begin_array(builder);

            for (k = 0; values != NULL && values[k] != NULL; k++)
                json_builder_add_string_value(builder, values[k]);

            json_builder_end_array(builder);
            break;
        }

        case CLAWT_SCHEMA_MAPPING:
            /*
             * Left out on purpose: the only mapping an entry has is an
             * MCP server's `env`, whose values may be secret references
             * and whose resolved form must never leave the daemon.
             */
            break;

        default: {
            const gchar *value =
                clawt_integration_config_get_string(instance, agent_id, leaf);

            if (value == NULL)
                break;

            json_builder_set_member_name(builder, leaf);
            json_builder_add_string_value(builder, value);
            break;
        }
        }
    }
}

static void
add_integration_object(JsonBuilder            *builder,
                       ClawtConfig            *config,
                       ClawtIntegrationConfig *instance,
                       const gchar            *agent_id)
{
    const ClawtIntegrationInfo *info;
    g_auto(GStrv) agents = NULL;
    GPtrArray *all;
    guint i;

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder,
                                  clawt_integration_config_get_name(instance));
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(
        builder, clawt_integration_config_get_type_id(instance));

    info = clawt_integration_find(
        clawt_integration_config_get_type_id(instance));

    json_builder_set_member_name(builder, "kind");
    json_builder_add_string_value(
        builder, info != NULL
                 ? clawt_enum_to_nick(CLAWT_TYPE_INTEGRATION_KIND,
                                      (gint)info->kind)
                 : "unknown");

    json_builder_set_member_name(builder, "summary");
    json_builder_add_string_value(builder,
                                  info != NULL ? info->summary : "");

    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(
        builder, clawt_integration_config_get_enabled(instance));

    json_builder_set_member_name(builder, "scope");
    json_builder_add_string_value(
        builder,
        clawt_enum_to_nick(CLAWT_TYPE_INTEGRATION_SCOPE,
                           (gint)clawt_integration_config_get_scope(instance)));

    agents = clawt_integration_config_get_agents(instance);

    json_builder_set_member_name(builder, "agents");
    json_builder_begin_array(builder);

    for (i = 0; agents != NULL && agents[i] != NULL; i++)
        json_builder_add_string_value(builder, agents[i]);

    json_builder_end_array(builder);

    /*
     * Who it actually reaches, worked out here rather than by the client.
     * `scope: all` names nobody in the file, so a client rendering the
     * `agents` list alone would show "all agents" beside an empty box and
     * leave a person guessing whether that meant none.
     */
    json_builder_set_member_name(builder, "effective_agents");
    json_builder_begin_array(builder);

    all = clawt_config_get_agents(config);

    for (i = 0; all != NULL && i < all->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(all, i);

        if (clawt_integration_config_covers(instance,
                                            clawt_agent_config_get_id(agent)))
            json_builder_add_string_value(builder,
                                          clawt_agent_config_get_id(agent));
    }

    json_builder_end_array(builder);

    if (agent_id != NULL) {
        json_builder_set_member_name(builder, "covers");
        json_builder_add_boolean_value(
            builder, clawt_integration_config_covers(instance, agent_id));
    }

    if (clawt_integration_config_is_shadow(instance)) {
        json_builder_set_member_name(builder, "shadow_reason");
        json_builder_add_string_value(
            builder, clawt_integration_config_get_shadow_reason(instance));
    }

    add_integration_values(builder, instance, agent_id);

    json_builder_end_object(builder);
}

/*
 * One integration as one agent has it, whether it came from an instance
 * or from the agent's own block.
 */
static void
add_binding_object(JsonBuilder *builder, ClawtIntegrationBinding *binding)
{
    const ClawtIntegrationInfo *info =
        clawt_integration_binding_get_info(binding);
    g_autoptr(GError) valid = NULL;

    json_builder_begin_object(builder);

    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder,
                                  clawt_integration_binding_get_name(binding));
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, info->id);
    json_builder_set_member_name(builder, "kind");
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(CLAWT_TYPE_INTEGRATION_KIND,
                                    (gint)info->kind));
    json_builder_set_member_name(builder, "summary");
    json_builder_add_string_value(builder, info->summary);
    json_builder_set_member_name(builder, "shared");
    json_builder_add_boolean_value(
        builder, clawt_integration_binding_is_shared(binding));

    json_builder_set_member_name(builder, "valid");
    json_builder_add_boolean_value(
        builder, clawt_integration_binding_validate(binding, &valid));

    if (valid != NULL) {
        json_builder_set_member_name(builder, "problem");
        json_builder_add_string_value(builder, valid->message);
    }

    json_builder_end_object(builder);
}

/*
 * Applies whatever the client sent, ignoring what it did not.
 *
 * Absent and empty are deliberately different: a member that is not
 * there is left alone, and one sent as null is cleared.  Without that a
 * dialog editing one field would have to send every other field back or
 * silently erase them.
 */
static gboolean
apply_integration_fields(ClawtIntegrationConfig  *instance,
                         JsonObject              *payload,
                         GError                 **error)
{
    const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
    const ClawtSchemaEntry *entries;
    gsize n_entries = 0;
    gsize i;

    entries = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const gchar *leaf = integration_leaf(&entries[i]);

        if (leaf == NULL || !json_object_has_member(payload, leaf))
            continue;

        switch (entries[i].type) {
        case CLAWT_SCHEMA_SECRET:
            /*
             * Never from a plain member.  A secret is written by naming
             * its reference -- secret_key, secret_backend, secret_locator
             * -- so there is no path here that could put a value in the
             * file even if a client sent one.
             */
            break;

        case CLAWT_SCHEMA_INT:
            clawt_integration_config_set_int(
                instance, agent_id, leaf,
                clawt_ipc_payload_int(payload, leaf, 0));
            break;

        case CLAWT_SCHEMA_BOOLEAN:
            clawt_integration_config_set_boolean(
                instance, agent_id, leaf,
                clawt_ipc_payload_boolean(payload, leaf, TRUE));
            break;

        case CLAWT_SCHEMA_STRING_LIST: {
            g_auto(GStrv) values = clawt_ipc_payload_strv(payload, leaf);

            clawt_integration_config_set_string_list(
                instance, agent_id, leaf, (const gchar *const *)values);
            break;
        }

        case CLAWT_SCHEMA_MAPPING:
            break;

        default:
            clawt_integration_config_set_string(
                instance, agent_id, leaf,
                clawt_ipc_payload_string(payload, leaf));
            break;
        }
    }

    if (json_object_has_member(payload, "enabled"))
        clawt_integration_config_set_enabled(
            instance, clawt_ipc_payload_boolean(payload, "enabled", TRUE));

    if (json_object_has_member(payload, "scope")) {
        const gchar *nick = clawt_ipc_payload_string(payload, "scope");
        g_auto(GStrv) agents = NULL;
        gint scope = 0;

        if (!clawt_enum_from_nick(CLAWT_TYPE_INTEGRATION_SCOPE, nick,
                                  &scope)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' is not a scope: use all, selected or none",
                        nick != NULL ? nick : "");
            return FALSE;
        }

        if (json_object_has_member(payload, "agents"))
            agents = clawt_ipc_payload_strv(payload, "agents");

        clawt_integration_config_set_scope(
            instance, (ClawtIntegrationScope)scope,
            (const gchar *const *)agents);
    } else if (json_object_has_member(payload, "agents")) {
        g_auto(GStrv) agents = clawt_ipc_payload_strv(payload, "agents");

        clawt_integration_config_set_string_list(
            instance, NULL, "agents", (const gchar *const *)agents);
    }

    /*
     * A secret arrives as a reference and never as a value.  The client
     * says which backend and what to look up in it; the daemon reads it
     * when it is needed.
     */
    if (json_object_has_member(payload, "secret_key")) {
        const gchar *key = clawt_ipc_payload_string(payload, "secret_key");
        const gchar *backend_nick =
            clawt_ipc_payload_string(payload, "secret_backend");
        const gchar *locator =
            clawt_ipc_payload_string(payload, "secret_locator");
        const ClawtSchemaEntry *entry = NULL;
        gint backend = CLAWT_SECRET_BACKEND_FILE;

        if (key != NULL) {
            g_autofree gchar *full = g_strdup_printf("integrations.%s", key);

            entry = clawt_config_schema_lookup(full);
        }

        if (entry == NULL || entry->type != CLAWT_SCHEMA_SECRET) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' is not a secret an integration holds",
                        key != NULL ? key : "");
            return FALSE;
        }

        if (backend_nick != NULL &&
            !clawt_enum_from_nick(CLAWT_TYPE_SECRET_BACKEND, backend_nick,
                                  &backend)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' is not a secret backend: use file, env or "
                        "command", backend_nick);
            return FALSE;
        }

        clawt_integration_config_set_secret(instance, agent_id, key,
                                            (ClawtSecretBackend)backend,
                                            locator);
    }

    return TRUE;
}

/* ── Health, which has to wait on the network ────────────────────── */

typedef struct {
    gchar    *name;
    gchar    *type_id;
    gboolean  ok;
    gchar    *message;
} HealthResult;

static void
health_result_free(HealthResult *self)
{
    if (self == NULL)
        return;

    g_free(self->name);
    g_free(self->type_id);
    g_free(self->message);
    g_free(self);
}

typedef struct {
    ClawtIpcPending *pending;
    GPtrArray       *checks;    /* ClawtIntegrationBinding* */
    GPtrArray       *results;   /* HealthResult* */
    guint            timeout;
    guint            next;
} HealthRun;

static void health_run_step(HealthRun *run);

static void
health_run_free(HealthRun *run)
{
    if (run == NULL)
        return;

    g_clear_pointer(&run->checks, g_ptr_array_unref);
    g_clear_pointer(&run->results, g_ptr_array_unref);
    g_free(run);
}

static void
health_run_finish(HealthRun *run)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    guint i;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "checks");
    json_builder_begin_array(builder);

    for (i = 0; i < run->results->len; i++) {
        HealthResult *result = g_ptr_array_index(run->results, i);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, result->name);
        json_builder_set_member_name(builder, "type");
        json_builder_add_string_value(builder, result->type_id);
        json_builder_set_member_name(builder, "ok");
        json_builder_add_boolean_value(builder, result->ok);

        if (result->message != NULL) {
            json_builder_set_member_name(builder, "error");
            json_builder_add_string_value(builder, result->message);
        }

        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        run->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(run->pending),
                               json_builder_get_root(builder)));

    health_run_free(run);
}

static void
on_health_checked(GObject *source, GAsyncResult *result, gpointer user_data)
{
    HealthRun *run = user_data;
    ClawtIntegrationBinding *binding;
    g_autoptr(GError) error = NULL;
    HealthResult *entry = g_new0(HealthResult, 1);

    /*
     * The binding is taken from the run rather than from the task's
     * source object, which is NULL: a binding is not a #GObject and
     * cannot be one.
     */
    binding = g_ptr_array_index(run->checks, run->next - 1);

    entry->name = g_strdup(clawt_integration_binding_get_name(binding));
    entry->type_id =
        g_strdup(clawt_integration_binding_get_info(binding)->id);
    entry->ok = clawt_integration_health_check_finish(binding, result, &error);

    if (!entry->ok)
        entry->message = g_strdup(error != NULL ? error->message
                                                : "it did not answer");

    g_ptr_array_add(run->results, entry);

    health_run_step(run);
}

/*
 * One check at a time.
 *
 * Sequential rather than parallel on purpose: these are almost always a
 * handful of hosts, and a fleet-wide check firing thirty simultaneous
 * connects at one homeserver looks like something it should not.
 */
static void
health_run_step(HealthRun *run)
{
    ClawtIntegrationBinding *binding;

    if (run->next >= run->checks->len) {
        health_run_finish(run);
        return;
    }

    binding = g_ptr_array_index(run->checks, run->next++);

    clawt_integration_health_check_async(binding, run->timeout, NULL,
                                         on_health_checked, run);
}

static void
health_run_start(HealthRun *run)
{
    health_run_step(run);
}

/* ── Matrix sign-in ──────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon     *daemon;      /* unowned; it outlives the request */
    ClawtIpcPending *pending;
    gchar           *name;
    gchar           *agent_id;
    gchar           *homeserver;
} MatrixLogin;

static void
matrix_login_free(MatrixLogin *self)
{
    if (self == NULL)
        return;

    g_free(self->name);
    g_free(self->agent_id);
    g_free(self->homeserver);
    g_free(self);
}

static void
on_matrix_login(GObject *source, GAsyncResult *result, gpointer user_data)
{
    MatrixLogin *login = user_data;
    ClawtDaemon *self = login->daemon;
    g_autoptr(ClawtMatrixLogin) session = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autofree gchar *secrets_dir = NULL;
    g_autofree gchar *token_path = NULL;
    g_autofree gchar *file_name = NULL;
    ClawtIntegrationConfig *instance;

    session = clawt_matrix_login_finish(result, &error);

    if (session == NULL) {
        clawt_ipc_pending_respond(
            login->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(login->pending),
                                CLAWT_ERROR_AUTH, error->message));
        matrix_login_free(login);
        return;
    }

    instance = clawt_config_get_integration(self->config, login->name);

    if (instance == NULL) {
        clawt_ipc_pending_respond(
            login->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(login->pending),
                                CLAWT_ERROR_NOT_FOUND,
                                "that integration was removed while signing "
                                "in"));
        matrix_login_free(login);
        return;
    }

    /*
     * The token goes to a file and the config gets a reference to it.
     * It is never in the reply: a client asked to sign in, and handing
     * it back the credential would put a live Matrix token into every
     * client's memory, and into whatever that client logs.
     */
    secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");

    if (!clawt_ensure_dir(secrets_dir, 0700, &error)) {
        clawt_ipc_pending_respond(
            login->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(login->pending),
                                CLAWT_ERROR_SECRET, error->message));
        matrix_login_free(login);
        return;
    }

    file_name = (login->agent_id != NULL)
        ? g_strdup_printf("%s-%s-matrix-token", login->name, login->agent_id)
        : g_strdup_printf("%s-matrix-token", login->name);
    token_path = g_build_filename(secrets_dir, file_name, NULL);

    if (!clawt_write_file_atomic(token_path, session->access_token, -1, 0600,
                                 FALSE, &error)) {
        clawt_ipc_pending_respond(
            login->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(login->pending),
                                CLAWT_ERROR_SECRET, error->message));
        matrix_login_free(login);
        return;
    }

    clawt_integration_config_set_string(instance, login->agent_id,
                                        "homeserver", login->homeserver);

    /*
     * The user id is the server's, not the one that was typed.  Signing
     * in as `agent` gives `@agent:example.org`, and the short form in the
     * config authenticates perfectly and matches no mention.
     */
    if (session->user_id != NULL)
        clawt_integration_config_set_string(instance, login->agent_id,
                                            "user_id", session->user_id);

    clawt_integration_config_set_secret(instance, login->agent_id,
                                        "access_token",
                                        CLAWT_SECRET_BACKEND_FILE, file_name);

    if (!clawt_config_save(self->config, &error)) {
        clawt_ipc_pending_respond(
            login->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(login->pending),
                                error->code, error->message));
        matrix_login_free(login);
        return;
    }

    clawt_daemon_reload(self, NULL);
    clawt_event_bus_emit(self->bus, "integration.changed", login->name);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "user_id");
    json_builder_add_string_value(builder, session->user_id);
    json_builder_set_member_name(builder, "device_id");
    json_builder_add_string_value(builder, session->device_id);
    json_builder_set_member_name(builder, "token_file");
    json_builder_add_string_value(builder, token_path);
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        login->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(login->pending),
                               json_builder_get_root(builder)));

    matrix_login_free(login);
}

static void
on_notify_tested(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtIpcPending *pending = user_data;
    g_autoptr(GError) error = NULL;

    if (!clawt_notifier_test_finish(CLAWT_NOTIFIER(source), result, &error)) {
        clawt_ipc_pending_respond(
            pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(pending),
                                CLAWT_ERROR_NOT_CONNECTED, error->message));
        return;
    }

    clawt_ipc_pending_respond(
        pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(pending), NULL));
}

static void
on_matrix_rooms(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtIpcPending *pending = user_data;
    g_autoptr(GPtrArray) rooms = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    guint i;

    rooms = clawt_matrix_rooms_finish(result, &error);

    if (rooms == NULL) {
        clawt_ipc_pending_respond(
            pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(pending),
                                CLAWT_ERROR_NOT_CONNECTED, error->message));
        return;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "rooms");
    json_builder_begin_array(builder);

    for (i = 0; i < rooms->len; i++) {
        ClawtMatrixRoom *room = g_ptr_array_index(rooms, i);
        g_autofree gchar *label = clawt_matrix_room_describe(room);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, room->id);
        json_builder_set_member_name(builder, "label");
        json_builder_add_string_value(builder, label);

        if (room->name != NULL) {
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, room->name);
        }

        if (room->alias != NULL) {
            json_builder_set_member_name(builder, "alias");
            json_builder_add_string_value(builder, room->alias);
        }

        json_builder_end_object(builder);
    }

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(pending),
                               json_builder_get_root(builder)));
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
        g_autoptr(GPtrArray) refusals = render_refusals_new();

        if (!daemon_reload(self, refusals, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * A response and not an error: the configuration was reloaded and
         * every agent clawtilla could render was.  But an agent it
         * refused is still running against the config.yaml it had, and
         * the only person who can fix that is the one who just asked for
         * the reload -- so they are handed the names and the reasons
         * rather than a bare success and a warning in the journal.
         *
         * The array is always present, so a client can tell "nothing was
         * refused" from "this daemon does not report refusals".
         */
        json_builder_begin_object(builder);
        add_render_refusals(builder, refusals);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "attachment.get") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        g_autofree gchar *path = NULL;
        g_autofree gchar *name = NULL;
        g_autofree gchar *contents = NULL;
        g_autofree gchar *encoded = NULL;
        gsize length = 0;

        /*
         * The bytes, not the path.
         *
         * A client may be on another machine entirely -- that is what
         * connection profiles are for -- so handing it a filename would
         * work on this host and show nothing anywhere else, which reads
         * as a broken image rather than as an unsupported setup.
         *
         * The id is checked rather than trusted: clawt_attachment_path()
         * refuses anything outside the character set an id is made of,
         * which is what stops a request for a path of somebody's
         * choosing reading a file this was never meant to serve.
         */
        if (id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which attachment?");

        if (self->attachment_dir == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no attachments");

        path = clawt_attachment_path(self->attachment_dir, id);

        if (path == NULL || !g_file_get_contents(path, &contents, &length,
                                                 NULL))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such attachment");

        name = clawt_attachment_name(id);
        encoded = g_base64_encode((const guchar *)contents, length);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, (gint64)length);
        json_builder_set_member_name(builder, "base64");
        json_builder_add_string_value(builder, encoded);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.list") == 0) {
        gboolean open_only = clawt_ipc_payload_boolean(payload, "open", TRUE);
        g_autoptr(GPtrArray) decisions = NULL;
        gint64 now = g_get_real_time() / G_USEC_PER_SEC;
        guint i;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");

        decisions = clawt_decision_store_list(self->decisions, open_only);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "decisions");
        json_builder_begin_array(builder);

        for (i = 0; decisions != NULL && i < decisions->len; i++)
            add_decision_object(builder, g_ptr_array_index(decisions, i),
                                now);

        json_builder_end_array(builder);
        json_builder_set_member_name(builder, "open");
        json_builder_add_int_value(
            builder, clawt_decision_store_count_open(self->decisions));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.answer") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "decision");
        const gchar *answer = clawt_ipc_payload_string(payload, "answer");
        g_autoptr(ClawtDecision) settled = NULL;
        g_autoptr(GError) answer_error = NULL;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");

        if (id == NULL || answer == NULL || *answer == '\0')
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "answering needs a decision and an answer");

        settled = clawt_decision_store_answer(self->decisions, id, answer,
                                              &answer_error);

        if (settled == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       answer_error->message);

        /*
         * And it goes back to whoever asked.
         *
         * Without this the inbox is a suggestion box: the operator
         * answers into the void and the agent never learns.  Routed as
         * an ordinary message so it costs the agent a turn and reaches
         * it through the machinery everything else uses -- and carrying
         * the task id, so an answer that arrives after the agent has
         * moved on can still be attached to what it was about.
         */
        deliver_decision_answer(self, settled);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "decision");
        add_decision_object(builder, settled,
                            g_get_real_time() / G_USEC_PER_SEC);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "decision.dismiss") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "decision");
        g_autoptr(GError) dismiss_error = NULL;

        if (self->decisions == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no decisions");

        if (!clawt_decision_store_dismiss(self->decisions, id,
                                          &dismiss_error))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       dismiss_error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "dismissed");
        json_builder_add_string_value(builder, id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "event.list") == 0) {
        const gchar *subject = clawt_ipc_payload_string(payload, "subject");
        guint limit = (guint)clawt_ipc_payload_int(payload, "limit", 200);
        g_autoptr(GPtrArray) events = NULL;
        guint i;

        /*
         * What the fleet has been doing, from the log that has been
         * recording it all along.
         *
         * ClawtEventLog has written every published event to NDJSON
         * since the daemon was written, sweeps on `daemon.event_log_days`
         * -- and was read back by nobody.  A client can hold the recent
         * ones in memory; anything older than that was on disk and
         * unreachable, which is why diagnosing a message loop meant
         * running sqlite3 and grep on the host.
         *
         * Fleet-wide unless a subject is named, because the case that
         * sends somebody to the shell is watching several agents at
         * once.
         */
        if (self->log == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "this daemon keeps no event log");

        events = clawt_event_log_read(self->log, subject, limit);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "events");
        json_builder_begin_array(builder);

        for (i = 0; events != NULL && i < events->len; i++) {
            ClawtEvent *event = g_ptr_array_index(events, i);
            g_autoptr(JsonNode) node = clawt_ipc_event_new(event);
            JsonObject *frame = json_node_get_object(node);

            /*
             * The event frame's own payload, rather than a second
             * spelling of what an event is.  clawt_ipc_event_new() is
             * what a subscriber receives, so a client reading history
             * and a client receiving live events parse one shape.
             */
            json_builder_add_value(
                builder,
                json_node_ref(json_object_get_member(frame, "payload")));
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
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

        {
            /*
             * Sorted here rather than in the manager, which keeps the
             * fleet in the order the file has it -- that order is what a
             * tie falls back to, so it has to survive.
             */
            g_autoptr(GPtrArray) ordered = g_ptr_array_new();

            for (i = 0; i < agents->len; i++)
                g_ptr_array_add(ordered, g_ptr_array_index(agents, i));

            {
                g_autoptr(GPtrArray) teams =
                    clawt_config_get_teams(self->config);

                g_ptr_array_sort_with_data(ordered, compare_by_order, teams);
            }

            for (i = 0; i < ordered->len; i++)
                add_agent_object(builder, g_ptr_array_index(ordered, i));
        }

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

        /*
         * Every settable key, so a client can build an editor from the
         * schema instead of from a list of its own. The GTK inspector
         * predates this and names its rows by hand, which is why a
         * setting added to the schema shows up there only when somebody
         * remembers to add a row for it.
         */
        add_agent_settings(builder, agent);

        computer = clawt_agent_get_computer(agent);

        if (computer != NULL) {
            g_autofree gchar *described =
                clawt_agent_describe_computer(agent);

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

    if (g_strcmp0(kind, "agent.mount.add") == 0 ||
        g_strcmp0(kind, "agent.mount.remove") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *target = clawt_ipc_payload_string(payload, "target");
        ClawtAgentConfig *agent_config;

        /*
         * Shared folders, settable.
         *
         * computer.mounts has always been read and applied -- bind
         * mounts for a container, virtiofs devices for a VM -- and no
         * client could write one, so the only way to share a folder
         * with an agent was to edit the YAML by hand.
         */
        if (agent_id == NULL || target == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and target are both required");

        agent_config = clawt_config_get_agent(self->config, agent_id);

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (g_strcmp0(kind, "agent.mount.remove") == 0) {
            if (!clawt_agent_config_remove_mount(agent_config, target))
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "that agent has no such mount");
        } else {
            const gchar *source = clawt_ipc_payload_string(payload, "source");
            const gchar *mode = clawt_ipc_payload_string(payload, "mode");
            const gchar *type = clawt_ipc_payload_string(payload, "type");
            const gchar *relabel = clawt_ipc_payload_string(payload,
                                                             "relabel");
            const gchar *size = clawt_ipc_payload_string(payload, "size");
            g_autoptr(ClawtMount) mount = clawt_mount_new(source, target);
            gint parsed = 0;

            if (mode != NULL) {
                if (!clawt_enum_from_nick(CLAWT_TYPE_MOUNT_MODE, mode,
                                           &parsed))
                    return clawt_ipc_error_new(request,
                                               CLAWT_ERROR_INVALID_ARGUMENT,
                                               "mode is ro or rw");

                clawt_mount_set_mode(mount, (ClawtMountMode)parsed);
            }

            if (type != NULL) {
                if (!clawt_enum_from_nick(CLAWT_TYPE_MOUNT_TYPE, type,
                                           &parsed))
                    return clawt_ipc_error_new(request,
                                               CLAWT_ERROR_INVALID_ARGUMENT,
                                               "type is bind, volume, "
                                               "virtiofs, 9p or tmpfs");

                clawt_mount_set_mount_type(mount, (ClawtMountType)parsed);
            }

            if (relabel != NULL) {
                if (!clawt_enum_from_nick(CLAWT_TYPE_RELABEL, relabel,
                                           &parsed))
                    return clawt_ipc_error_new(request,
                                               CLAWT_ERROR_INVALID_ARGUMENT,
                                               "relabel is none, shared or "
                                               "private");

                clawt_mount_set_relabel(mount, (ClawtRelabel)parsed);
            }

            if (size != NULL)
                clawt_mount_set_size(mount, size);

            /*
             * Validated before it is written, so a mount that could
             * never work is refused here rather than at the agent's
             * next start -- by which time the config has been saved and
             * the cause is a start failure that mentions neither the
             * path nor the reason.
             */
            if (!clawt_mount_validate(mount, &error))
                return clawt_ipc_error_new(request, error->code,
                                           error->message);

            if (!clawt_agent_config_add_mount(agent_config, mount))
                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           "could not add the mount");
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_agent_manager_load(self->agents, NULL);

        {
            g_autoptr(GPtrArray) refusals = render_refusals_new();

            render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "agent.changed", agent_id);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "target");
            json_builder_add_string_value(builder, target);
            add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.mount.list") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_autoptr(GPtrArray) mounts = NULL;
        guint i;

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        mounts = clawt_agent_config_get_mounts(agent_config);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "mounts");
        json_builder_begin_array(builder);

        for (i = 0; i < mounts->len; i++) {
            ClawtMount *mount = g_ptr_array_index(mounts, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "source");
            json_builder_add_string_value(builder,
                                          clawt_mount_get_source(mount));
            json_builder_set_member_name(builder, "target");
            json_builder_add_string_value(builder,
                                          clawt_mount_get_target(mount));
            json_builder_set_member_name(builder, "mode");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_MOUNT_MODE,
                                            clawt_mount_get_mode(mount)));
            json_builder_set_member_name(builder, "type");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_MOUNT_TYPE,
                                            clawt_mount_get_mount_type(mount)));
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * Reading and writing one workspace file.
     *
     * The GTK client opens these in $EDITOR, which is a local program on
     * the machine a person is sitting at -- so a client reached over the
     * network has no way to offer the same thing without a wire path.
     * These are that path, and nothing else uses them.
     *
     * The name goes through clawt_workspace_file_path(), which refuses
     * anything containing a separator or "..": this is reached from an
     * IPC request, and a client that could name "../../secrets" would be
     * reading another agent's credentials.
     */
    if (g_strcmp0(kind, "agent.file_read") == 0 ||
        g_strcmp0(kind, "agent.file_write") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        ClawtAgentConfig *config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_autofree gchar *path = NULL;

        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (name == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "name is required");

        path = clawt_workspace_file_path(config, name);

        if (path == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_INVALID_ARGUMENT,
                "that is not a plain file name inside the workspace");

        if (g_strcmp0(kind, "agent.file_write") == 0) {
            const gchar *content = clawt_ipc_payload_string(payload,
                                                            "content");

            if (content == NULL)
                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT,
                                           "content is required");

            if (!g_file_set_contents(path, content, -1, &error))
                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           error->message);

            clawt_event_bus_emit(self->bus, "agent.changed", agent_id);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "path");
            json_builder_add_string_value(builder, path);
            json_builder_set_member_name(builder, "bytes");
            json_builder_add_int_value(builder, (gint64)strlen(content));
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }

        {
            g_autofree gchar *content = NULL;

            /*
             * A file that is not there yet is empty rather than an
             * error. The standard set is scaffolded at first start, so
             * asking for one before then is an ordinary thing to do --
             * and an editor that refused to open a file it is about to
             * create would be a strange editor.
             */
            if (!g_file_get_contents(path, &content, NULL, NULL))
                content = g_strdup("");

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, name);
            json_builder_set_member_name(builder, "path");
            json_builder_add_string_value(builder, path);
            json_builder_set_member_name(builder, "content");
            json_builder_add_string_value(builder, content);
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }
    }

    if (g_strcmp0(kind, "agent.files") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtAgentConfig *agent_config;
        const ClawtWorkspaceFile *files;
        guint n_files = 0;
        guint i;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        agent_config = clawt_agent_get_config(agent);

        /*
         * Scaffolded on the way out, so `agent edit` works on an agent
         * that has never been started.  Nothing is overwritten.
         */
        if (!clawt_workspace_scaffold(agent_config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * .mcp.json too, so `agent edit <id> .mcp.json` opens a real
         * file on an agent that has never been started.  It is written
         * here rather than by the full render because that resolves
         * credentials, which can run a command, and a handler runs on
         * the daemon's main context while the client waits.
         */
        {
            g_autofree gchar *state_dir = clawt_config_agent_state_dir(
                self->config, clawt_agent_config_get_id(agent_config));
            g_autofree gchar *socket_path =
                clawt_config_get_path_value(self->config, "daemon.socket");

            if (!clawt_workspace_write_mcp_config(self->config, agent_config,
                                                  socket_path, state_dir,
                                                  &error))
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        }

        files = clawt_workspace_files(&n_files);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "workspace");

        {
            g_autofree gchar *workspace =
                clawt_agent_config_get_workspace(agent_config);

            json_builder_add_string_value(builder, workspace);
        }

        json_builder_set_member_name(builder, "files");
        json_builder_begin_array(builder);

        for (i = 0; i < n_files; i++) {
            g_autofree gchar *path =
                clawt_workspace_file_path(agent_config, files[i].name);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, files[i].name);
            json_builder_set_member_name(builder, "path");
            json_builder_add_string_value(builder, path != NULL ? path : "");
            json_builder_set_member_name(builder, "title");
            json_builder_add_string_value(builder, files[i].title);
            json_builder_set_member_name(builder, "identity");
            json_builder_add_boolean_value(builder, files[i].identity);
            json_builder_set_member_name(builder, "generated");
            json_builder_add_boolean_value(builder, files[i].generated);
            json_builder_set_member_name(builder, "exists");
            json_builder_add_boolean_value(
                builder,
                path != NULL && g_file_test(path, G_FILE_TEST_EXISTS));
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
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

    if (g_strcmp0(kind, "memory.list") == 0 ||
        g_strcmp0(kind, "memory.search") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtMemoryStore *store;
        g_autoptr(GPtrArray) memories = NULL;
        guint i;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        store = clawt_agent_get_memory(agent);

        if (store == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_SUPPORTED,
                "that agent has no memory store; memories.enabled is off");

        if (g_strcmp0(kind, "memory.search") == 0)
            memories = clawt_memory_store_search(
                store, clawt_ipc_payload_string(payload, "query"),
                clawt_ipc_payload_string(payload, "category"),
                (guint)clawt_ipc_payload_int(payload, "limit", 20), NULL);
        else
            memories = clawt_memory_store_list(
                store, clawt_ipc_payload_string(payload, "category"),
                clawt_ipc_payload_boolean(payload, "pinned", FALSE),
                (guint)clawt_ipc_payload_int(payload, "limit", 20), NULL);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "total");
        json_builder_add_int_value(builder,
                                   clawt_memory_store_count(store, FALSE));
        json_builder_set_member_name(builder, "memories");
        json_builder_begin_array(builder);

        for (i = 0; memories != NULL && i < memories->len; i++) {
            ClawtMemory *memory = g_ptr_array_index(memories, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, memory->id);
            json_builder_set_member_name(builder, "content");
            json_builder_add_string_value(builder, memory->content);

            if (memory->summary != NULL) {
                json_builder_set_member_name(builder, "summary");
                json_builder_add_string_value(builder, memory->summary);
            }

            json_builder_set_member_name(builder, "category");
            json_builder_add_string_value(builder, memory->category);
            json_builder_set_member_name(builder, "importance");
            json_builder_add_string_value(builder, memory->importance);

            if (memory->tags != NULL) {
                json_builder_set_member_name(builder, "tags");
                json_builder_add_string_value(builder, memory->tags);
            }

            json_builder_set_member_name(builder, "pinned");
            json_builder_add_boolean_value(builder, memory->pinned);
            json_builder_set_member_name(builder, "created_at");
            json_builder_add_int_value(builder, memory->created_at);
            json_builder_set_member_name(builder, "access_count");
            json_builder_add_int_value(builder, memory->access_count);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.reset") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        g_autofree gchar *state_dir = NULL;
        g_autofree gchar *sessions = NULL;
        g_autofree gchar *aside = NULL;
        g_autofree gchar *db_path = NULL;
        gboolean was_running;
        guint cleared = 0;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        was_running = clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_STOPPED;

        /*
         * Stopped first, and not only to be tidy: the agent holds its
         * own session files and its own sqlite connection open, and
         * clearing either underneath a running process is how you get a
         * half-reset session that resumes anyway.
         */
        if (was_running)
            clawt_daemon_stop_agent(self, agent_id);

        state_dir = clawt_config_agent_state_dir(self->config, agent_id);
        sessions = g_build_filename(state_dir, "sessions", NULL);
        db_path = clawt_usage_database_path(state_dir);

        /*
         * Moved aside rather than deleted. A reset is what you reach for
         * when something is wedged, which is exactly when you might want
         * to look at what it was doing.
         */
        if (g_file_test(sessions, G_FILE_TEST_IS_DIR)) {
            aside = g_strdup_printf("%s.reset-%" G_GINT64_FORMAT, sessions,
                                    g_get_real_time() / G_USEC_PER_SEC);

            if (g_rename(sessions, aside) != 0)
                g_clear_pointer(&aside, g_free);
        }

        /*
         * And the database rows, because libreclaw restores a session
         * from either place -- clearing only the files leaves the agent
         * resuming the same CLI session from sqlite and looking like the
         * reset did nothing.
         *
         * Through libreclaw's own API rather than by opening its schema:
         * the daemon links liblc, and the agent is stopped, so this is
         * the same code the agent itself would run.
         *
         * The path comes from clawt_usage_database_path() because this
         * block spelled it itself for a long time, as
         * `<state_dir>/libreclaw.db` -- which is not where libreclaw
         * puts it.  Its sqlite backend builds the name from
         * `session.persist_dir` and never reads `database.path`, so the
         * file tested for here has never existed on any machine: the
         * branch was skipped every time and `sessions_cleared` was
         * always 0.  Reset appeared to work only because moving the
         * sessions directory aside takes the database with it, which is
         * luck rather than the two-places-to-clear this was written for.
         */
        if (g_file_test(db_path, G_FILE_TEST_EXISTS)) {
            g_autoptr(LcDatabase) db = LC_DATABASE(lc_sqlite_database_new());
            g_autoptr(GError) db_error = NULL;

            if (lc_database_open(db, db_path, &db_error)) {
                GPtrArray *rows = lc_database_load_sessions(db, NULL);
                guint i;

                for (i = 0; rows != NULL && i < rows->len; i++) {
                    LcDbSession *row = g_ptr_array_index(rows, i);

                    if (lc_database_remove_session(db, row->session_key,
                                                   NULL))
                        cleared++;
                }

                g_clear_pointer(&rows, g_ptr_array_unref);
                lc_database_close(db);
            } else {
                g_warning("agent %s: could not clear its session rows: %s",
                          agent_id, db_error->message);
            }
        }

        /*
         * The database that comes back is a new one, numbering its rows
         * from 1 again.  A watermark from the old one would suppress
         * every row in it, so the agent would appear to spend nothing
         * ever again.
         */
        if (self->usage != NULL)
            clawt_usage_forget(self->usage, agent_id);

        if (was_running && !clawt_daemon_start_agent(self, agent_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "sessions_cleared");
        json_builder_add_int_value(builder, cleared);
        json_builder_set_member_name(builder, "moved");
        json_builder_add_string_value(builder, aside != NULL ? aside : "");
        json_builder_set_member_name(builder, "restarted");
        json_builder_add_boolean_value(builder, was_running);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "attachment.remove") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        g_autofree gchar *safe = NULL;
        g_autofree gchar *relative = NULL;
        g_autofree gchar *host_path = NULL;

        if (agent_id == NULL || name == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and name are both required");

        if (self->exchange == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "there is no exchange directory");

        /*
         * Rebuilt from its basename and resolved through the exchange,
         * exactly as attachment.put does. A client asking to delete
         * "../../../etc/passwd" gets a refusal about a file in its own
         * drop-box that does not exist.
         */
        safe = g_path_get_basename(name);
        relative = g_build_filename(agent_id, safe, NULL);
        host_path = clawt_exchange_resolve(self->exchange, agent_id, relative,
                                           TRUE, &error);

        if (host_path == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (g_unlink(host_path) != 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       g_strerror(errno));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "removed");
        json_builder_add_string_value(builder, host_path);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.discover") == 0) {
        g_autofree gchar *agents_dir = NULL;
        g_autofree gchar *workspace_root = NULL;
        g_autoptr(GHashTable) seen = NULL;
        gsize d;
        static const gchar *interesting[] = {
            "mailbox.db", "memory.db", "config.yaml", "SOUL.org",
            "IDENTITY.org", "AGENTS.md", NULL
        };

        /*
         * Directories that look like agents but are not in the config.
         *
         * They accumulate: an agent removed from the config keeps its
         * state, a design that was never committed leaves a workspace,
         * and a config restored from a backup leaves everything it did
         * not mention. None of that is visible anywhere, so it just
         * sits on disk and surprises people.
         */
        agents_dir = g_build_filename(self->state_dir, "agents", NULL);
        workspace_root = clawt_config_get_path_value(
            self->config, "defaults.workspace_root");

        seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "found");
        json_builder_begin_array(builder);

        for (d = 0; d < 2; d++) {
            const gchar *root = (d == 0) ? agents_dir : workspace_root;
            g_autoptr(GDir) dir = NULL;
            const gchar *name;

            if (root == NULL)
                continue;

            /* One root may be inside the other; do not list twice. */
            if (d == 1 && g_strcmp0(root, agents_dir) == 0)
                continue;

            dir = g_dir_open(root, 0, NULL);

            if (dir == NULL)
                continue;

            while ((name = g_dir_read_name(dir)) != NULL) {
                g_autofree gchar *path = g_build_filename(root, name, NULL);
                GStatBuf info;
                gsize i;

                if (!g_file_test(path, G_FILE_TEST_IS_DIR))
                    continue;

                /*
                 * Already put aside once.  Listing it again would ask
                 * the same question a second time, which is the one
                 * thing "forget" was supposed to stop.
                 */
                if (g_str_has_suffix(name, ".discarded"))
                    continue;

                if (clawt_agent_manager_get(self->agents, name) != NULL)
                    continue;

                if (g_hash_table_contains(seen, name))
                    continue;

                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "id");
                json_builder_add_string_value(builder, name);
                json_builder_set_member_name(builder, "path");
                json_builder_add_string_value(builder, path);
                json_builder_set_member_name(builder, "kind");
                json_builder_add_string_value(builder,
                                              d == 0 ? "state" : "workspace");

                /*
                 * What is actually in there, so a person can tell a
                 * real agent's leftovers from an empty directory
                 * somebody made by hand.
                 */
                json_builder_set_member_name(builder, "holds");
                json_builder_begin_array(builder);

                for (i = 0; interesting[i] != NULL; i++) {
                    g_autofree gchar *file =
                        g_build_filename(path, interesting[i], NULL);

                    if (g_file_test(file, G_FILE_TEST_EXISTS))
                        json_builder_add_string_value(builder, interesting[i]);
                }

                json_builder_end_array(builder);

                json_builder_set_member_name(builder, "modified");
                json_builder_add_int_value(
                    builder, g_stat(path, &info) == 0 ? info.st_mtime : 0);

                json_builder_end_object(builder);

                g_hash_table_add(seen, g_strdup(name));
            }
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.import") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "id");
        const gchar *from = clawt_ipc_payload_string(payload, "from");
        const gchar *mode_nick = clawt_ipc_payload_string(payload, "mode");
        gboolean keep_git = clawt_ipc_payload_boolean(payload, "keep_git",
                                                       FALSE);
        ClawtImportMode mode = CLAWT_IMPORT_COPY;
        g_autofree gchar *source = NULL;
        g_autofree gchar *workspace = NULL;
        g_autofree gchar *detail = NULL;
        ClawtAgentConfig *created;
        guint copied = 0;

        if (agent_id == NULL || from == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "id and from are both required");

        /*
         * Refused rather than quietly defaulted. An unrecognised mode
         * would otherwise become a copy, so somebody who typed `--lnik`
         * would get a fork of their workspace instead of a link to it
         * and find out only when their edits stopped reaching the agent.
         */
        if (mode_nick != NULL &&
            g_strcmp0(mode_nick,
                      clawt_import_mode_nth_nick(
                          clawt_import_mode_from_nick(mode_nick))) != 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "mode must be copy, link or git");

        mode = clawt_import_mode_from_nick(mode_nick);

        /*
         * A git import names a URL rather than a directory, so the
         * directory check belongs to the two modes that take one --
         * clawt_workspace_adopt() makes it, where it can say which kind
         * of thing was expected.
         */
        source = g_strdup(from);

        if (clawt_agent_manager_get(self->agents, agent_id) != NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_ALREADY_EXISTS,
                                       "there is already an agent with that "
                                       "id");

        /*
         * The config entry first, so the workspace path is whatever
         * clawtilla would have chosen -- an import is an agent like any
         * other afterwards, not one that remembers where it came from.
         */
        created = clawt_config_add_agent(self->config, agent_id, &error);

        if (created == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        workspace = clawt_agent_config_get_workspace(created);

        if (!clawt_workspace_adopt(mode, source, workspace, keep_git,
                                   &copied, &detail, &error)) {
            clawt_config_remove_agent(self->config, agent_id);
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        /*
         * Anything the source said about itself that clawtilla owns
         * too. A standalone libreclaw instance keeps its provider and
         * model in its own config.yaml, and an import that dropped them
         * would quietly move the agent onto the fleet defaults.
         */
        {
            g_autofree gchar *imported = g_build_filename(workspace,
                                                          "config.yaml", NULL);

            clawt_config_adopt_libreclaw(created, imported);
        }

        /*
         * And the persona it already had.
         *
         * clawtilla names its identity files in org, a workspace from
         * anywhere else names them in markdown, and the two sets do not
         * collide -- so the copy above brought a complete persona across
         * and the scaffolder then wrote a blank .org beside every file
         * of it. The agent loaded the blanks: an import that succeeded,
         * reported every file copied, and produced something wearing the
         * right name with "/(fill in)/" where its character should be.
         *
         * Only when nothing was configured. An id and a persona given on
         * the import frame are the caller's, not ours to overrule.
         */
        if (!clawt_agent_config_has_key(created, "persona.identity_files")) {
            g_auto(GStrv) adopted =
                clawt_workspace_detect_identity_files(workspace);

            if (adopted != NULL && adopted[0] != NULL) {
                g_autofree gchar *joined = g_strjoinv(", ", adopted);

                clawt_agent_config_set_string_list(
                    created, "persona.identity_files",
                    (const gchar *const *)adopted);

                g_message("import: '%s' already had a persona; loading %s "
                          "rather than scaffolding blanks beside it",
                          agent_id, joined);
            }
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The same two steps agent.create takes.  Saving the config is
         * not enough to make an agent exist: the manager builds its
         * agents from a reloaded config, and without this the import
         * succeeded, wrote everything correctly, and then did not
         * appear in `agent list`.
         */
        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_agent_manager_load(self->agents, NULL);
        clawt_event_bus_emit(self->bus, "agent.created", agent_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, agent_id);
        json_builder_set_member_name(builder, "workspace");
        json_builder_add_string_value(builder, workspace);
        json_builder_set_member_name(builder, "files");
        json_builder_add_int_value(builder, copied);
        json_builder_set_member_name(builder, "mode");
        json_builder_add_string_value(builder,
                                      clawt_import_mode_nth_nick(mode));

        /*
         * What actually happened, in a sentence.
         *
         * Two of the three modes have an outcome the client could not
         * predict -- a git import is a submodule only where the
         * workspace root is inside a repository -- and the difference
         * between a workspace somebody's `git status` tracks and one it
         * does not is worth saying rather than leaving to be discovered.
         */
        if (detail != NULL) {
            json_builder_set_member_name(builder, "detail");
            json_builder_add_string_value(builder, detail);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /*
     * Cloud images.  A VM needs one, clawtilla ships none, and they are
     * several hundred megabytes -- so they are fetched deliberately,
     * ahead of any agent needing one, with progress to watch.
     */
    if (g_strcmp0(kind, "image.vm_catalog") == 0) {
        const ClawtVmImageSource *catalog;
        gsize n_sources = 0;
        gsize i;

        catalog = clawt_vm_image_catalog(&n_sources);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "sources");
        json_builder_begin_array(builder);

        for (i = 0; i < n_sources; i++) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, catalog[i].id);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, catalog[i].name);
            json_builder_set_member_name(builder, "group");
            json_builder_add_string_value(builder, catalog[i].group);
            json_builder_set_member_name(builder, "url");
            json_builder_add_string_value(builder, catalog[i].url);

            if (catalog[i].note != NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder, catalog[i].note);
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.vm_list") == 0) {
        g_autoptr(GPtrArray) images = clawt_vm_image_store_list(self->vm_images);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "images");
        json_builder_begin_array(builder);

        for (i = 0; images != NULL && i < images->len; i++) {
            ClawtVmImage *image = g_ptr_array_index(images, i);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder, image->name);
            json_builder_set_member_name(builder, "path");
            json_builder_add_string_value(builder, image->path);
            json_builder_set_member_name(builder, "bytes");
            json_builder_add_int_value(builder, image->bytes);
            json_builder_set_member_name(builder, "total");
            json_builder_add_int_value(builder, image->total);
            json_builder_set_member_name(builder, "downloading");
            json_builder_add_boolean_value(builder, image->downloading);

            if (image->url != NULL) {
                json_builder_set_member_name(builder, "url");
                json_builder_add_string_value(builder, image->url);
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.vm_download") == 0) {
        const gchar *url = clawt_ipc_payload_string(payload, "url");
        g_autoptr(GError) start_error = NULL;
        g_autofree gchar *name = NULL;

        if (url == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a url or a catalog id is required");

        /*
         * Returns as soon as the transfer is under way.  A handler runs on
         * the daemon's main context while the client blocks, so waiting
         * here for half a gigabyte would stall every other client for the
         * length of the download.
         */
        name = clawt_vm_image_store_start(self->vm_images, url,
                                          clawt_ipc_payload_string(payload,
                                                                   "name"),
                                          &start_error);

        if (name == NULL)
            return clawt_ipc_error_new(request, start_error->code,
                                       start_error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.vm_cancel") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");

        if (name == NULL || !clawt_vm_image_store_cancel(self->vm_images,
                                                         name))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "nothing by that name is downloading");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "cancelled");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.vm_remove") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        g_autoptr(GError) remove_error = NULL;
        g_autofree gchar *image_path = NULL;

        if (name == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a name is required");

        /*
         * An agent's overlay records the base it was built on, so
         * deleting the base breaks that VM the next time it starts --
         * with an error from qemu about a missing backing file, a long
         * way from the button that caused it.
         */
        image_path = clawt_vm_image_store_path(self->vm_images, name);

        if (image_path != NULL &&
            !clawt_ipc_payload_boolean(payload, "force", FALSE)) {
            GPtrArray *agents = clawt_agent_manager_list(self->agents);
            g_autoptr(GString) users = g_string_new(NULL);
            guint i;

            for (i = 0; agents != NULL && i < agents->len; i++) {
                ClawtAgent *agent = g_ptr_array_index(agents, i);
                g_autofree gchar *configured = clawt_agent_config_get_path_value(
                    clawt_agent_get_config(agent), "computer.vm.image");

                if (g_strcmp0(configured, image_path) != 0)
                    continue;

                if (users->len > 0)
                    g_string_append(users, ", ");

                g_string_append(users, clawt_agent_get_id(agent));
            }

            if (users->len > 0) {
                g_autofree gchar *refusal = g_strdup_printf(
                    "%s is the disk image for %s. Deleting it breaks that "
                    "VM the next time it starts, because its overlay is "
                    "built on this file. Point the agent at another image "
                    "first, or pass force to delete it anyway.",
                    name, users->str);

                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT,
                                           refusal);
            }
        }

        if (!clawt_vm_image_store_remove(self->vm_images, name,
                                         &remove_error))
            return clawt_ipc_error_new(request, remove_error->code,
                                       remove_error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "removed");
        json_builder_add_string_value(builder, name);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request,
                                     json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "state.git_init") == 0) {
        g_autofree gchar *ignore_path = NULL;
        gboolean created = FALSE;

        if (self->state_dir == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "there is no state directory yet");

        if (!prepare_state_git(self->state_dir, TRUE, &created, &ignore_path,
                               &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "path");
        json_builder_add_string_value(builder, self->state_dir);
        json_builder_set_member_name(builder, "created");
        json_builder_add_boolean_value(builder, created);
        json_builder_set_member_name(builder, "gitignore");
        json_builder_add_string_value(builder, ignore_path);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.forget") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "id");
        g_autofree gchar *state_path = NULL;
        g_autofree gchar *workspace_root = NULL;
        g_autofree gchar *workspace_path = NULL;

        if (agent_id == NULL || strchr(agent_id, '/') != NULL ||
            g_strcmp0(agent_id, "..") == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "not a plain agent id");

        if (clawt_agent_manager_get(self->agents, agent_id) != NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_AGENT_STATE,
                "that agent is in the config; remove it with agent.remove");

        state_path = g_build_filename(self->state_dir, "agents", agent_id,
                                      NULL);
        workspace_root = clawt_config_get_path_value(
            self->config, "defaults.workspace_root");

        if (workspace_root != NULL)
            workspace_path = g_build_filename(workspace_root, agent_id, NULL);

        /*
         * Moved aside rather than deleted.  This is somebody's agent --
         * its transcripts, its memories, whatever it was told about
         * them -- and "I never created this" is a thing people say
         * about directories they later want back.
         */
        {
            g_autoptr(GString) moved = g_string_new(NULL);
            const gchar *paths[] = { state_path, workspace_path };
            gsize i;

            for (i = 0; i < G_N_ELEMENTS(paths); i++) {
                g_autofree gchar *aside = NULL;

                if (paths[i] == NULL ||
                    !g_file_test(paths[i], G_FILE_TEST_IS_DIR))
                    continue;

                aside = g_strconcat(paths[i], ".discarded", NULL);

                if (g_rename(paths[i], aside) == 0) {
                    if (moved->len > 0)
                        g_string_append(moved, ", ");

                    g_string_append(moved, aside);
                }
            }

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "moved");
            json_builder_add_string_value(builder, moved->str);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.create") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "id");
        g_autoptr(GHashTable) fields = NULL;
        ClawtAgentConfig *created;

        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "an agent needs an id");

        /*
         * The frame's vocabulary, translated into configuration keys
         * here so the shared implementation never has to know it.
         */
        {
            static const struct {
                const gchar *from;
                const gchar *to;
            } names[] = {
                { "name",           "name" },
                { "description",    "description" },
                { "model",          "model.model" },
                { "provider",       "model.provider" },
                { "computer",       "computer.type" },
                { "confine",        "computer.host.confine" },
                { "image",          "computer.container.image" },
                { "vm_image",       "computer.vm.image" },
                { "vm_cpus",        "computer.vm.cpus" },
                { "vm_memory_mb",   "computer.vm.memory_mb" },
                { "vm_disk_gb",     "computer.vm.disk_gb" },
                { "vm_resolution",  "computer.vm.resolution" },
                { "team",           "team" },
                { "team_role",      "team_role" },
                { "workspace",      "workspace" },
                { NULL, NULL }
            };
            gsize i;

            fields = g_hash_table_new(g_str_hash, g_str_equal);

            for (i = 0; names[i].from != NULL; i++) {
                const gchar *value = clawt_ipc_payload_string(payload,
                                                              names[i].from);

                if (value != NULL)
                    g_hash_table_insert(fields, (gpointer)names[i].to,
                                        (gpointer)value);
            }
        }

        created = daemon_create_agent(self, agent_id, fields, NULL, NULL,
                                      &error);

        if (created == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, agent_id);

        /*
         * ...and started, because creating an agent and building the
         * thing it works in were two steps and only one of them had a
         * button.
         *
         * A computer is built at *start*, not at create: a VM agent
         * created and left alone has a config file and no machine.
         * `defaults.autostart` does not cover it either -- it is false
         * by default and means "comes back with the daemon", which is a
         * different question from whether the thing somebody just asked
         * for exists.
         */
        if (clawt_ipc_payload_boolean(payload, "start", TRUE)) {
            g_autoptr(GError) start_error = NULL;
            gboolean started = clawt_daemon_start_agent(self, agent_id,
                                                        &start_error);

            json_builder_set_member_name(builder, "started");
            json_builder_add_boolean_value(builder, started);

            /*
             * Reported, never fatal. The agent exists and its
             * configuration is on disk; rolling that back because a
             * hypervisor was busy would throw away everything the person
             * had just typed.
             */
            if (!started && start_error != NULL) {
                json_builder_set_member_name(builder, "start_error");
                json_builder_add_string_value(builder, start_error->message);
            }
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.remove") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        gboolean with_computer =
            clawt_ipc_payload_boolean(payload, "remove_computer", FALSE);
        g_autofree gchar *computer_detail = NULL;
        g_autofree gchar *files_detail = NULL;
        gboolean linked_workspace = FALSE;

        if (agent_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which agent?");

        /*
         * The container or VM, torn down before the agent goes.
         *
         * Removing an agent used to leave its computer running, with a
         * name derived from an agent that no longer existed -- so the
         * only way to find it again was to remember what it had been
         * called. Opt-in, because a container may hold work that was
         * never anywhere else.
         *
         * Done before the config entry is dropped: the computer is built
         * from that config, and afterwards there is nothing left to
         * build it from.
         */
        if (with_computer) {
            ClawtAgent *agent = clawt_agent_manager_get(self->agents,
                                                         agent_id);
            ClawtComputer *computer = (agent != NULL)
                                      ? clawt_agent_get_computer(agent)
                                      : NULL;
            g_autoptr(ClawtComputer) built = NULL;

            /*
             * An agent that was never started has no computer object,
             * but its container may still be there from a previous run.
             * Building one from the config finds it by name.
             */
            if (computer == NULL) {
                ClawtAgentConfig *agent_config =
                    clawt_config_get_agent(self->config, agent_id);

                if (agent_config != NULL) {
                    built = clawt_computer_factory_create(agent_config,
                                                           self->pod_bridge,
                                                           NULL);
                    computer = built;
                }
            }

            if (computer != NULL &&
                clawt_computer_get_computer_type(computer) !=
                    CLAWT_COMPUTER_NONE) {
                g_autoptr(GError) teardown_error = NULL;

                if (clawt_computer_teardown(computer, &teardown_error)) {
                    computer_detail = g_strdup("removed");
                } else {
                    /*
                     * Reported, not fatal.  The agent is still going, and
                     * refusing to remove it because its container had
                     * already been deleted by hand would be absurd.
                     */
                    computer_detail = g_strdup(
                        teardown_error != NULL ? teardown_error->message
                                               : "could not be removed");
                    g_warning("agent %s: computer not removed: %s", agent_id,
                              computer_detail);
                }
            }
        }

        clawt_daemon_stop_agent(self, agent_id);

        /*
         * The files, before the config entry goes: every path is
         * derived from that entry, and afterwards there is nothing left
         * to derive them from.
         */
        if (clawt_ipc_payload_boolean(payload, "remove_files", FALSE)) {
            ClawtAgentConfig *doomed = clawt_config_get_agent(self->config,
                                                              agent_id);
            g_autoptr(GError) purge_error = NULL;
            gboolean was_linked = FALSE;

            /*
             * Reported beside `files` rather than instead of it. `files`
             * is the success sentinel every client already branches on,
             * and a linked workspace genuinely was removed -- what
             * differs is what survived, which is a sentence rather than
             * an outcome.
             */
            if (doomed != NULL &&
                !clawt_daemon_purge_agent_files(self, doomed, &was_linked,
                                                &purge_error))
                files_detail = g_strdup(purge_error->message);
            else if (doomed != NULL)
                files_detail = g_strdup("removed");

            linked_workspace = was_linked;
        }

        if (!clawt_config_remove_agent(self->config, agent_id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * Without remove_files the agent's state directory -- its
         * mailbox, its transcripts, its rendered config -- is left on
         * disk. Removing an agent from the fleet is reversible; deleting
         * its history is not, so it is asked for rather than assumed.
         */
        clawt_agent_manager_load(self->agents, NULL);
        clawt_event_bus_emit(self->bus, "agent.removed", agent_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, agent_id);

        /* What happened to the computer, so a client can say so. */
        if (computer_detail != NULL) {
            json_builder_set_member_name(builder, "computer");
            json_builder_add_string_value(builder, computer_detail);
        }

        /* ...and to the files, which is the irreversible half. */
        if (files_detail != NULL) {
            json_builder_set_member_name(builder, "files");
            json_builder_add_string_value(builder, files_detail);
        }

        if (linked_workspace) {
            json_builder_set_member_name(builder, "linked_workspace");
            json_builder_add_boolean_value(builder, TRUE);
        }

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    /* ── teams ── */

    if (g_strcmp0(kind, "team.list") == 0) {
        g_autoptr(GPtrArray) teams = clawt_config_get_teams(self->config);
        GPtrArray *agents = clawt_agent_manager_list(self->agents);
        g_auto(GStrv) warnings = NULL;
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "teams");
        json_builder_begin_array(builder);

        for (i = 0; i < teams->len; i++) {
            ClawtTeamSpec *team = g_ptr_array_index(teams, i);
            guint running = 0;
            guint total = 0;
            const gchar *lead = NULL;
            guint j;

            json_builder_begin_object(builder);
            add_string_member(builder, "id", team->id);
            add_string_member(builder, "name",
                              team->name != NULL ? team->name : team->id);
            add_string_member(builder, "description", team->description);
            add_string_member(builder, "color", team->color);

            json_builder_set_member_name(builder, "order");
            json_builder_add_int_value(builder, team->order);

            json_builder_set_member_name(builder, "members");
            json_builder_begin_array(builder);

            for (j = 0; agents != NULL && j < agents->len; j++) {
                ClawtAgent *agent = g_ptr_array_index(agents, j);
                ClawtAgentConfig *config = clawt_agent_get_config(agent);

                if (g_strcmp0(clawt_agent_config_get_string(config, "team"),
                              team->id) != 0)
                    continue;

                json_builder_add_string_value(builder,
                                              clawt_agent_get_id(agent));
                total++;

                if (clawt_agent_get_state(agent) ==
                    CLAWT_AGENT_STATE_RUNNING)
                    running++;

                if (clawt_team_role_of(config) == CLAWT_TEAM_LEAD)
                    lead = clawt_agent_get_id(agent);
            }

            json_builder_end_array(builder);

            add_string_member(builder, "lead", lead);

            /*
             * Counted here rather than in each client. Three clients
             * counting the same thing is three chances to disagree about
             * what "active" means.
             */
            json_builder_set_member_name(builder, "running");
            json_builder_add_int_value(builder, running);
            json_builder_set_member_name(builder, "total");
            json_builder_add_int_value(builder, total);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);

        /*
         * What only the whole fleet can show: two leads on one team, an
         * agent naming a team nobody declared. Reported rather than
         * enforced, because a fleet is edited by hand and half-built
         * states are ordinary.
         */
        clawt_team_validate_fleet(self->config, &warnings);

        json_builder_set_member_name(builder, "warnings");
        json_builder_begin_array(builder);

        for (i = 0; warnings != NULL && warnings[i] != NULL; i++)
            json_builder_add_string_value(builder, warnings[i]);

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "team.create") == 0) {
        const gchar *team_id = clawt_ipc_payload_string(payload, "id");

        if (team_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a team needs an id");

        if (!clawt_config_add_team(self->config, team_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        {
            static const gchar *const fields[] = {
                "name", "description", "color", NULL
            };
            gsize i;

            for (i = 0; fields[i] != NULL; i++) {
                const gchar *value = clawt_ipc_payload_string(payload,
                                                              fields[i]);

                if (value != NULL)
                    clawt_config_set_team_string(self->config, team_id,
                                                 fields[i], value);
            }
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "team.changed", team_id);

        json_builder_begin_object(builder);
        add_string_member(builder, "id", team_id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "team.set") == 0) {
        const gchar *team_id = clawt_ipc_payload_string(payload, "team");
        const gchar *key = clawt_ipc_payload_string(payload, "key");
        const gchar *value = clawt_ipc_payload_string(payload, "value");

        if (team_id == NULL || key == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "team and key are both required");

        if (!clawt_config_set_team_string(self->config, team_id, key, value))
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_NOT_FOUND,
                g_strcmp0(key, "id") == 0
                    ? "a team's id cannot be changed: everything refers to "
                      "it by that. Create the new one, move the agents, "
                      "remove the old."
                    : "no such team");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The agents' own files describe their team, so they are
         * rewritten here for the same reason agent.set rewrites them:
         * a description that changed and did not reach the prompt is a
         * second answer to what the team is for.
         */
        {
            g_autoptr(GPtrArray) refusals = render_refusals_new();

            render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "team.changed", team_id);

            json_builder_begin_object(builder);
            add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request,
                                      json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "team.remove") == 0) {
        const gchar *team_id = clawt_ipc_payload_string(payload, "team");

        if (team_id == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "which team?");

        if (!clawt_config_remove_team(self->config, team_id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such team");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The agents that were on it are left naming a team that is no
         * longer declared, which is a state they are allowed to be in --
         * and saying how many is more use than silently reassigning
         * them somewhere nobody chose.
         */
        {
            GPtrArray *agents;
            g_autoptr(GPtrArray) refusals = render_refusals_new();
            guint orphaned = 0;
            guint i;

            render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "team.changed", team_id);

            agents = clawt_agent_manager_list(self->agents);

            for (i = 0; agents != NULL && i < agents->len; i++) {
                ClawtAgent *agent = g_ptr_array_index(agents, i);

                if (g_strcmp0(clawt_agent_config_get_string(
                                  clawt_agent_get_config(agent), "team"),
                              team_id) == 0)
                    orphaned++;
            }

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "orphaned");
            json_builder_add_int_value(builder, orphaned);
            add_render_refusals(builder, refusals);
            json_builder_end_object(builder);
        }

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "agent.reorder") == 0) {
        const gchar *ids = clawt_ipc_payload_string(payload, "agents");
        g_auto(GStrv) wanted = NULL;
        gsize i;

        if (ids == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agents is required: the ids in the "
                                       "order you want them, comma "
                                       "separated");

        wanted = g_strsplit(ids, ",", -1);

        /*
         * Numbered from one, in steps of ten.
         *
         * The gap is not decoration: it leaves room to place one agent
         * between two others by setting a single number by hand, which
         * is the only way to do it in a text editor without renumbering
         * the whole file.
         */
        for (i = 0; wanted[i] != NULL; i++) {
            const gchar *agent_id = g_strstrip(wanted[i]);
            ClawtAgentConfig *config;
            g_autofree gchar *position = NULL;

            if (*agent_id == '\0')
                continue;

            config = clawt_config_get_agent(self->config, agent_id);

            /*
             * An id that is not there is skipped rather than refused.
             * The list comes from a client's view of the fleet, which
             * may be a moment behind one that has just been removed --
             * and failing the whole reorder over that would lose the
             * arrangement somebody had just made.
             */
            if (config == NULL)
                continue;

            position = g_strdup_printf("%u", (guint)((i + 1) * 10));
            clawt_agent_config_set_string(config, "order", position);
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "agent.changed", NULL);

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

        /*
         * Dispatch on what the schema says the key *is*, rather than
         * writing every value as a scalar.
         *
         * A STRING_LIST written as a scalar is not merely ugly: the
         * reader refuses anything that is not a YAML sequence, so the
         * value was accepted here, echoed back to the client, saved to
         * clawtilla.yaml, and then read back as the schema default. The
         * one that exposed it was persona.identity_files -- an agent
         * pointed at its real persona files went on loading the seven
         * generated .org stubs, and every surface agreed the setting
         * had been saved.
         */
        clawt_agent_config_set_from_string(config, key, value);

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * ...and the agent's own files, which are derived from what was
         * just changed.
         *
         * Saving used to be the whole of it, so a setting was written to
         * clawtilla.yaml and nothing the agent reads was touched. The
         * one that made that visible was tools.manage_fleet: the gate
         * answers from the live config and was right immediately, while
         * TOOLS.org went on listing the tools as they were at the last
         * daemon start. Two answers to "what do I have", and the file is
         * the one that reaches the agent's prompt.
         */
        {
            g_autoptr(GPtrArray) refusals = render_refusals_new();

            render_all_agents_into(self, refusals);
            clawt_event_bus_emit(self->bus, "agent.changed", agent_id);

            json_builder_begin_object(builder);
            add_render_refusals(builder, refusals);
        }

        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, agent_id);

        /*
         * An AI CLI reads some things once, when its session starts. So a
         * setting changed under a running agent reaches its files and not
         * its session, and saying nothing here is how somebody concludes
         * the setting does not work.
         *
         * Tools are the known case: a CLI lists them at session start.
         * Persona is the same shape and was not reported -- an AI CLI is
         * not handed a system prompt when it *resumes* a session, so
         * editing an identity file, or repointing
         * persona.identity_files, leaves a running agent with the
         * identity it was created with. Clearing the session is what
         * applies it, which is `clawtilla agent reset`.
         */
        json_builder_set_member_name(builder, "restart_required");
        json_builder_add_boolean_value(
            builder, (g_str_has_prefix(key, "tools.") ||
                      g_str_has_prefix(key, "persona.")) &&
                     clawt_agent_get_state(
                         clawt_agent_manager_get(self->agents, agent_id)) ==
                     CLAWT_AGENT_STATE_RUNNING);

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
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

            /*
             * Enough for a client to draw a conversation list without
             * fetching every transcript to find out which rooms have
             * anything in them.  A fleet accumulates a direct room per
             * pair, and most of them are empty.
             */
            json_builder_set_member_name(builder, "messages");
            json_builder_add_int_value(
                builder, clawt_room_get_message_count(room));

            {
                g_autoptr(GPtrArray) last = clawt_room_get_history(room, 1);

                if (last->len > 0) {
                    ClawtMessage *message = g_ptr_array_index(last, 0);

                    json_builder_set_member_name(builder, "last_sender");
                    json_builder_add_string_value(
                        builder, clawt_message_get_sender_id(message));
                    json_builder_set_member_name(builder, "last_body");
                    json_builder_add_string_value(
                        builder, clawt_message_get_body(message));
                    json_builder_set_member_name(builder, "last_ts");
                    json_builder_add_int_value(
                        builder, clawt_message_get_timestamp(message));
                }
            }

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

        /*
         * Which room this actually is, because the request may have
         * named an agent and let the daemon resolve the direct room. A
         * client that shows a conversation has to be able to tell
         * whether an incoming message belongs in it, and comparing
         * against the agent it asked for is not the same question --
         * that is how a reply from an agent to one of its peers ended up
         * drawn in the user's own chat with it.
         */
        json_builder_set_member_name(builder, "room");
        json_builder_add_string_value(builder, clawt_room_get_id(room));

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

            /*
             * The task this message belongs to, when it belongs to one.
             * It is what turns a transcript into a chain you can follow:
             * without it a delegated reply is just another line from an
             * agent, with no sign of what asked for it.
             */
            if (clawt_message_get_task_id(message) != NULL) {
                json_builder_set_member_name(builder, "task");
                json_builder_add_string_value(
                    builder, clawt_message_get_task_id(message));
            }

            /*
             * How far this message had travelled agent-to-agent.  It is
             * what makes a runaway visible: a conversation whose hop
             * count climbs towards max_hops is a loop, and reading two
             * agents politely agreeing to do nothing gives no sign of
             * that at all.
             */
            json_builder_set_member_name(builder, "depth");
            json_builder_add_int_value(builder,
                                       clawt_message_get_depth(message));

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

    /* ── usage ── */

    if (g_strcmp0(kind, "usage.summary") == 0) {
        GPtrArray *agents = clawt_agent_manager_list(self->agents);
        ClawtUsageTotals fleet = { 0, 0, 0, 0 };
        gint64 since = clawt_ipc_payload_int(payload, "since", 0);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "agents");
        json_builder_begin_array(builder);

        for (i = 0; i < agents->len; i++) {
            ClawtAgent *agent = g_ptr_array_index(agents, i);
            const gchar *agent_id = clawt_agent_get_id(agent);
            g_autofree gchar *state_dir = NULL;
            g_autofree gchar *db_path = NULL;
            g_autoptr(GError) read_error = NULL;
            ClawtUsageTotals totals = { 0, 0, 0, 0 };

            state_dir = clawt_config_agent_state_dir(self->config, agent_id);
            if (state_dir == NULL)
                continue;

            db_path = clawt_usage_database_path(state_dir);

            /*
             * One unreadable database does not fail the summary.  A
             * fleet report that refuses because one agent's file is
             * mid-write tells you nothing about the other nine.
             */
            if (!clawt_usage_read_totals(db_path, since, &totals,
                                         &read_error)) {
                g_debug("usage: %s: %s", agent_id,
                        read_error != NULL ? read_error->message : "unknown");
            }

            clawt_usage_totals_add(&fleet, &totals);

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, agent_id);
            json_builder_set_member_name(builder, "name");
            json_builder_add_string_value(builder,
                                          clawt_agent_get_name(agent));
            json_builder_set_member_name(builder, "turns");
            json_builder_add_int_value(builder, totals.turns);
            json_builder_set_member_name(builder, "input_tokens");
            json_builder_add_int_value(builder, totals.input_tokens);
            json_builder_set_member_name(builder, "output_tokens");
            json_builder_add_int_value(builder, totals.output_tokens);
            json_builder_set_member_name(builder, "cost_micros");
            json_builder_add_int_value(builder, totals.cost_micros);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);

        json_builder_set_member_name(builder, "total");
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "turns");
        json_builder_add_int_value(builder, fleet.turns);
        json_builder_set_member_name(builder, "input_tokens");
        json_builder_add_int_value(builder, fleet.input_tokens);
        json_builder_set_member_name(builder, "output_tokens");
        json_builder_add_int_value(builder, fleet.output_tokens);
        json_builder_set_member_name(builder, "cost_micros");
        json_builder_add_int_value(builder, fleet.cost_micros);
        json_builder_end_object(builder);

        json_builder_set_member_name(builder, "since");
        json_builder_add_int_value(builder, since);

        /*
         * What the budget would refuse right now, so a client can show
         * the cap beside the spend rather than making somebody go and
         * read the config to find out what the number means.
         */
        json_builder_set_member_name(builder, "task_budget_usd");
        json_builder_add_double_value(
            builder,
            clawt_config_get_double(self->config,
                                    "orchestration.task_budget_usd"));

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

        /*
         * The computer is built when the agent starts, so a stopped
         * agent has none -- and "that agent has no computer" then reads
         * as a configuration mistake rather than a stopped agent, which
         * is a different thing to go and check.
         */
        if (computer == NULL) {
            const gchar *configured =
                (agent != NULL)
                ? clawt_agent_config_get_string(clawt_agent_get_config(agent),
                                                "computer.type")
                : NULL;
            g_autofree gchar *detail = NULL;

            if (configured != NULL && g_strcmp0(configured, "none") != 0)
                detail = g_strdup_printf(
                    "%s has a %s computer configured, but it is only built "
                    "when the agent starts. Start it first.",
                    agent_id, configured);
            else
                detail = g_strdup_printf("%s has no computer", agent_id);

            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       detail);
        }

        /*
         * An argv is taken as it stands.  A caller that already has the
         * arguments separated -- the CLI has them from the shell that
         * split them -- must not have them joined and re-split here:
         * `echo 'x\\ny'` came back as `xny`, because the backslash the
         * user quoted was consumed a second time, and `sh -c 'a; b'`
         * turned into four arguments and ran nothing.
         *
         * The string form stays for callers that genuinely have a
         * command line, which is what a model writes.
         */
        argv = clawt_ipc_payload_strv(payload, "argv");

        if (argv != NULL && argv[0] == NULL)
            g_clear_pointer(&argv, g_strfreev);

        if (argv == NULL &&
            (command == NULL || !g_shell_parse_argv(command, NULL, &argv,
                                                    &error)))
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

    if (g_strcmp0(kind, "computer.rebuild") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_autoptr(ClawtComputer) built = NULL;
        g_autoptr(GError) teardown_error = NULL;
        g_autofree gchar *removed = NULL;

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        /*
         * Refused while it runs, rather than done carefully.  Rebuilding
         * is destroying the machine the agent is working on; there is no
         * version of that which is safe to do underneath it.
         */
        if (agent != NULL &&
            clawt_agent_get_state(agent) != CLAWT_AGENT_STATE_STOPPED)
            return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                       "stop the agent first: rebuilding "
                                       "destroys the computer it is using");

        if ((ClawtComputerType)clawt_agent_config_get_enum(
                agent_config, "computer.type") == CLAWT_COMPUTER_NONE)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this agent has no computer to "
                                       "rebuild");

        /*
         * Built from the config rather than taken from the agent: a
         * stopped agent has no computer object, and stopped is the only
         * state this is allowed in.
         */
        built = clawt_computer_factory_create(agent_config, self->pod_bridge,
                                              &error);

        if (built == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * A teardown that fails is reported and not fatal.  The common
         * reason to reach for this is that the guest is already gone --
         * deleted by hand in virt-manager -- and refusing to rebuild
         * because there was nothing to tear down would be absurd.
         */
        if (!clawt_computer_teardown(built, &teardown_error)) {
            removed = g_strdup(teardown_error->message);
            g_message("agent %s: nothing to tear down before rebuilding "
                      "(%s)", agent_id, removed);
        }

        if (!clawt_computer_provision(built, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "agent.changed", agent_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "rebuilt");
        json_builder_add_boolean_value(builder, TRUE);
        add_string_member(builder, "agent", agent_id);

        if (removed != NULL)
            add_string_member(builder, "note", removed);

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

        described = clawt_agent_describe_computer(agent);

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

    if (g_strcmp0(kind, "computer.desktop") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        ClawtComputer *computer = (agent != NULL)
                                  ? clawt_agent_get_computer(agent) : NULL;
        ClawtAgentConfig *agent_config = (agent != NULL)
                                         ? clawt_agent_get_config(agent)
                                         : NULL;
        g_autoptr(ClawtDesktop) built = NULL;
        ClawtDesktop *desktop = NULL;
        g_auto(GStrv) argv = NULL;
        g_auto(GStrv) tools = NULL;
        gsize i;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no such agent");

        /*
         * The attached desktop when there is one, and otherwise one built
         * from the config.
         *
         * An agent only gets a ClawtDesktop when it is started, so a
         * stopped agent with the grant plainly set was told it "has no
         * desktop; set computer.desktop.enabled" -- naming the key that
         * was already true. The policy is a pure function of the config,
         * so it can be answered without the agent running.
         */
        desktop = clawt_agent_get_desktop(agent);

        if (desktop == NULL) {
            built = clawt_computer_factory_create_desktop(agent_config);
            desktop = built;
        }

        if (desktop == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "that agent has no desktop; set "
                                       "computer.desktop.enabled");

        if (clawt_agent_config_get_enum(agent_config, "computer.type") !=
            CLAWT_COMPUTER_VM)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "that agent's desktop is not in a "
                                       "VM, so there is nothing to relay "
                                       "to");

        /*
         * Configured for a VM but without one built means the agent is
         * not running, which is a different thing from being misconfigured
         * and deserves saying so.
         */
        if (computer == NULL ||
            clawt_computer_get_computer_type(computer) != CLAWT_COMPUTER_VM)
            return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                       "that agent is not running, so its "
                                       "VM has no address yet. Start the "
                                       "agent first.");

        /*
         * Built here rather than written into the agent's .mcp.json,
         * because the port that reaches the guest is chosen when the VM
         * is provisioned -- which is after the workspace files are
         * written, and again after anybody edits the config. A command
         * line captured at render time would name a port nothing is
         * listening on.
         */
        argv = clawt_vm_computer_build_desktop_argv(CLAWT_VM_COMPUTER(computer));

        if (argv == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_COMPUTER_EXEC,
                "nothing reaches that agent's VM yet: it may not be "
                "running, or no port is forwarded to it. Start the agent "
                "and try again.");

        tools = clawt_desktop_get_tool_names(desktop);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "backend");
        json_builder_add_string_value(
            builder, clawt_enum_to_nick(CLAWT_TYPE_DESKTOP_BACKEND,
                                        clawt_desktop_resolve_backend(desktop,
                                                                      NULL)));

        json_builder_set_member_name(builder, "argv");
        json_builder_begin_array(builder);
        for (i = 0; argv[i] != NULL; i++)
            json_builder_add_string_value(builder, argv[i]);
        json_builder_end_array(builder);

        /*
         * The permitted tools travel with the command, so the relay does
         * not need its own copy of the policy -- and so an agent whose
         * allow_input was turned off stops being able to click the moment
         * the daemon is reloaded, rather than whenever its MCP client is
         * next restarted.
         */
        json_builder_set_member_name(builder, "tools");
        json_builder_begin_array(builder);
        for (i = 0; tools != NULL && tools[i] != NULL; i++)
            json_builder_add_string_value(builder, tools[i]);
        json_builder_end_array(builder);
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

    if (g_strcmp0(kind, "attachment.put") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *encoded = clawt_ipc_payload_string(payload, "data");
        ClawtAgent *agent = (agent_id != NULL)
                            ? clawt_agent_manager_get(self->agents, agent_id)
                            : NULL;
        g_autofree guchar *bytes = NULL;
        g_autofree gchar *safe = NULL;
        g_autofree gchar *relative = NULL;
        g_autofree gchar *host_path = NULL;
        gsize length = 0;

        if (agent == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        if (name == NULL || encoded == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "name and data are both required");

        if (self->exchange == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "there is no exchange directory");

        /*
         * The name is taken apart and rebuilt rather than trusted.  It
         * comes from a filename a person dragged in or a clipboard
         * suggestion, and "../../.ssh/authorized_keys" is a name.
         */
        safe = g_path_get_basename(name);

        if (safe[0] == '\0' || g_strcmp0(safe, ".") == 0 ||
            g_strcmp0(safe, "..") == 0 || g_strcmp0(safe, G_DIR_SEPARATOR_S) == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "that is not a usable file name");

        bytes = g_base64_decode(encoded, &length);

        if (bytes == NULL || length == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "the attachment is empty");

        /*
         * The agent's own directory, made if this is the first thing
         * ever put in it. resolve() answers where a path *would* be, so
         * without this the very first attachment failed on a directory
         * that had never been created.
         */
        if (!clawt_exchange_prepare(self->exchange, agent_id, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        relative = g_build_filename(agent_id, safe, NULL);
        host_path = clawt_exchange_resolve(self->exchange, agent_id, relative,
                                           TRUE, &error);

        if (host_path == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_write_file_atomic(host_path, (const gchar *)bytes,
                                     (gssize)length, 0600, FALSE, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, safe);
        json_builder_set_member_name(builder, "host_path");
        json_builder_add_string_value(builder, host_path);

        /*
         * The path to *tell the agent*, which is not the host path when
         * it lives in a container: the exchange is mounted, so the
         * agent sees it somewhere else entirely and a host path would
         * send it looking for a file that is not there.
         */
        json_builder_set_member_name(builder, "path");

        {
            const gchar *computer = clawt_agent_config_get_string(
                clawt_agent_get_config(agent), "computer.type");

            if (g_strcmp0(computer, "container") == 0 ||
                g_strcmp0(computer, "vm") == 0) {
                g_autofree gchar *guest = g_build_filename(
                    CLAWT_EXCHANGE_MOUNT_POINT, agent_id, safe, NULL);

                json_builder_add_string_value(builder, guest);
            } else {
                json_builder_add_string_value(builder, host_path);
            }
        }

        json_builder_set_member_name(builder, "bytes");
        json_builder_add_int_value(builder, (gint64)length);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
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
        /*
         * The questionnaire.
         *
         * One free-text box asked the person to write a paragraph that
         * happened to contain everything the model needed, and a
         * paragraph that leaves out the boundaries produces an agent
         * with none.  Named questions ask for each thing once, and an
         * unanswered one is visibly unanswered rather than silently
         * absent.
         */
        static const struct {
            const gchar *field;
            const gchar *question;
        } questions[] = {
            { "purpose",     "What should this agent do?" },
            { "boundaries",  "What should it never do?" },
            { "needs",       "What does it need to work on: files, "
                             "commands, the network, nothing?" },
            { "personality", "How should it come across?" },
            { "projects",    "What is it working on, and where does that "
                             "live?" },
            { "notes",       "Anything else it should know?" },
            { NULL, NULL }
        };
        const gchar *description = clawt_ipc_payload_string(payload,
                                                            "description");
        g_autoptr(GString) brief = g_string_new(NULL);
        g_autoptr(ClawtAgentDesigner) designer = NULL;
        g_autofree gchar *preview = NULL;
        g_autofree gchar *draft_id = NULL;
        GHashTable *draft;
        gsize i;

        for (i = 0; questions[i].field != NULL; i++) {
            const gchar *answer = clawt_ipc_payload_string(payload,
                                                            questions[i].field);

            if (answer == NULL || *answer == '\0')
                continue;

            g_string_append_printf(brief, "%s\n%s\n\n",
                                   questions[i].question, answer);
        }

        /*
         * The old single-field form still works.  The CLI takes a
         * sentence, and a client that has not been updated should keep
         * designing agents rather than start failing.
         */
        if (brief->len == 0 && description != NULL && *description != '\0')
            g_string_append(brief, description);

        if (brief->len == 0)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "answer at least one question, or "
                                       "send a description");

        designer = clawt_agent_designer_new(self->config);

        /*
         * An id or name the person typed is theirs.  Models rename
         * routinely -- to something they consider more descriptive --
         * and the agent then appears under a name nobody chose, with
         * any script that asked for the original looking at the wrong
         * agent.
         */
        clawt_agent_designer_pin_identity(
            designer, clawt_ipc_payload_string(payload, "id"),
            clawt_ipc_payload_string(payload, "name"));

        /*
         * And so is the computer, for a sharper reason than the name.
         * The designer cannot name a disk image, so a VM it chose by
         * itself never provisions -- it refuses naming computer.vm.image,
         * a setting nothing in the design ever set. The client collects
         * that above the Design button; this is where it arrives.
         */
        if (clawt_ipc_payload_string(payload, "computer") != NULL) {
            g_autoptr(GHashTable) settings =
                g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      g_free);
            static const struct {
                const gchar *member;
                const gchar *key;
            } carried[] = {
                { "image",     "computer.container.image" },
                { "vm_image",  "computer.vm.image" },
                { "vm_cpus",   "computer.vm.cpus" },
                { "vm_memory", "computer.vm.memory_mb" },
                { "vm_disk",   "computer.vm.disk_gb" },
                /*
                 * The team is a choice made on the form, and the model
                 * has no way to know which teams exist -- so it is
                 * carried through rather than left for the designer to
                 * guess at, the same as the disk image.
                 */
                { "team",      "team" },
                { NULL, NULL }
            };
            gsize c;

            for (c = 0; carried[c].member != NULL; c++) {
                const gchar *value =
                    clawt_ipc_payload_string(payload, carried[c].member);

                if (value != NULL && *value != '\0')
                    g_hash_table_insert(settings, g_strdup(carried[c].key),
                                        g_strdup(value));
            }

            clawt_agent_designer_pin_computer(
                designer, clawt_ipc_payload_string(payload, "computer"),
                settings);
        }

        /*
         * The model that designs is chosen per request, falling back to
         * ai_assist.  The one that drafts an agent and the one that then
         * runs it have no reason to be the same: a person will often
         * want their best model for the first and a cheap one for the
         * second.
         */
        if (clawt_ipc_payload_string(payload, "provider") != NULL) {
            if (!clawt_config_get_boolean(self->config, "ai_assist.enabled"))
                return clawt_ipc_error_new(
                    request, CLAWT_ERROR_NOT_SUPPORTED,
                    "AI-assisted agent creation is turned off; set "
                    "ai_assist.enabled: true");

            if (!clawt_agent_designer_set_provider_by_name(
                    designer,
                    clawt_ipc_payload_string(payload, "provider"),
                    clawt_ipc_payload_string(payload, "model"), &error))
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        } else if (!clawt_agent_designer_use_configured_provider(designer,
                                                                 &error)) {
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        draft = clawt_agent_designer_design(designer, brief->str, NULL,
                                            &error);

        if (draft == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        preview = clawt_agent_designer_preview(designer);

        /*
         * Kept so design.commit creates exactly what was reviewed.
         * Bounded, because a client that designs and walks away should
         * not grow the daemon without limit.
         */
        draft_id = clawt_generate_token(NULL);

        if (draft_id == NULL)
            draft_id = g_strdup(g_hash_table_lookup(draft, "id"));

        if (g_hash_table_size(self->drafts) >= MAX_PENDING_DRAFTS) {
            GHashTableIter iter;
            gpointer oldest = NULL;

            g_hash_table_iter_init(&iter, self->drafts);

            if (g_hash_table_iter_next(&iter, &oldest, NULL))
                g_hash_table_remove(self->drafts, oldest);
        }

        g_hash_table_insert(self->drafts, g_strdup(draft_id),
                            g_object_ref(designer));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "draft");
        json_builder_add_string_value(builder, draft_id);
        json_builder_set_member_name(builder, "yaml");
        json_builder_add_string_value(builder, preview);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder,
                                      g_hash_table_lookup(draft, "id"));

        /* The org files the model wrote, so a client can show them. */
        {
            GHashTable *files = clawt_agent_designer_get_files(designer);
            g_autoptr(GList) names = g_hash_table_get_keys(files);
            GList *f;

            names = g_list_sort(names, (GCompareFunc)g_strcmp0);

            json_builder_set_member_name(builder, "files");
            json_builder_begin_array(builder);

            for (f = names; f != NULL; f = f->next) {
                json_builder_begin_object(builder);
                json_builder_set_member_name(builder, "name");
                json_builder_add_string_value(builder, f->data);
                json_builder_set_member_name(builder, "content");
                json_builder_add_string_value(
                    builder, g_hash_table_lookup(files, f->data));
                json_builder_end_object(builder);
            }

            json_builder_end_array(builder);
        }

        json_builder_set_member_name(builder, "committed");
        json_builder_add_boolean_value(builder, FALSE);
        json_builder_set_member_name(builder, "notes");
        json_builder_add_string_value(
            builder, clawt_agent_designer_get_transcript(designer));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "design.commit") == 0) {
        const gchar *draft_id = clawt_ipc_payload_string(payload, "draft");
        ClawtAgentDesigner *designer = (draft_id != NULL)
            ? g_hash_table_lookup(self->drafts, draft_id) : NULL;
        ClawtAgentConfig *created;

        /*
         * Creates the design that was reviewed, rather than asking the
         * model again.  A second run is a fresh conversation and would
         * produce something else -- which makes the preview a
         * demonstration rather than a decision.
         */
        if (designer == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such draft; design it again");

        created = clawt_agent_designer_commit(designer, &error);

        if (created == NULL) {
            g_hash_table_remove(self->drafts, draft_id);
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_agent_manager_load(self->agents, NULL);

        {
            g_autoptr(GPtrArray) refusals = render_refusals_new();

            render_all_agents_into(self, refusals);

            json_builder_begin_object(builder);
            add_render_refusals(builder, refusals);
        }

        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder,
                                      clawt_agent_config_get_id(created));
        json_builder_set_member_name(builder, "committed");
        json_builder_add_boolean_value(builder, TRUE);

        /*
         * The same start agent.create does, and for the same reason: an
         * agent designed and committed is an agent somebody wanted. The
         * designer's own comment says it commits "the same path as
         * creating an agent by hand", which was true of the config call
         * and had already stopped being true of the validation around
         * it once before.
         */
        if (clawt_ipc_payload_boolean(payload, "start", TRUE)) {
            const gchar *created_id = clawt_agent_config_get_id(created);
            g_autoptr(GError) start_error = NULL;
            gboolean started = clawt_daemon_start_agent(self, created_id,
                                                        &start_error);

            json_builder_set_member_name(builder, "started");
            json_builder_add_boolean_value(builder, started);

            if (!started && start_error != NULL) {
                json_builder_set_member_name(builder, "start_error");
                json_builder_add_string_value(builder, start_error->message);
            }
        }

        json_builder_end_object(builder);

        g_hash_table_remove(self->drafts, draft_id);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "design.discard") == 0) {
        const gchar *draft_id = clawt_ipc_payload_string(payload, "draft");

        if (draft_id != NULL)
            g_hash_table_remove(self->drafts, draft_id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "discarded");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "image.list") == 0) {
        const ClawtImageInfo *catalog;
        g_auto(GStrv) configured = NULL;
        gsize n_images = 0;
        gsize i;

        catalog = clawt_image_catalog_get(&n_images);
        configured = clawt_config_get_string_list(self->config,
                                                  "defaults.container_images");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "default");
        json_builder_add_string_value(
            builder, clawt_config_get_string(self->config,
                                             "defaults.container_image"));

        /*
         * Nothing here is a restriction: any reference podman can pull
         * is valid.  Said explicitly so a client offers a way to type
         * one that is not listed rather than treating this as a menu.
         */
        json_builder_set_member_name(builder, "open_ended");
        json_builder_add_boolean_value(builder, TRUE);

        json_builder_set_member_name(builder, "images");
        json_builder_begin_array(builder);

        /*
         * The user's own first.  A list where the images they added sit
         * below a dozen they will never pick is one they scroll past.
         */
        for (i = 0; configured != NULL && configured[i] != NULL; i++) {
            const gchar *entry = configured[i];
            const gchar *separator = strstr(entry, " -- ");
            g_autofree gchar *reference = NULL;

            if (*entry == '\0')
                continue;

            reference = (separator != NULL)
                        ? g_strndup(entry, separator - entry)
                        : g_strdup(entry);
            g_strstrip(reference);

            if (*reference == '\0')
                continue;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "reference");
            json_builder_add_string_value(builder, reference);

            /*
             * The last path component as the label.  A registry-and-org
             * prefix is the same on all of a user's own images, so a
             * list showing the whole reference truncates to
             * "registry.exampl..." for every one of them and
             * distinguishes none.  The full reference is still on the
             * row's subtitle once selected.
             */
            json_builder_set_member_name(builder, "label");
            {
                const gchar *slash = strrchr(reference, '/');

                json_builder_add_string_value(
                    builder, (slash != NULL && slash[1] != '\0') ? slash + 1
                                                                 : reference);
            }

            if (separator != NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder, separator + 4);
            }

            json_builder_set_member_name(builder, "group");
            json_builder_add_string_value(builder, "Yours");
            json_builder_end_object(builder);
        }

        for (i = 0; i < n_images; i++) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "reference");
            json_builder_add_string_value(builder, catalog[i].reference);
            json_builder_set_member_name(builder, "label");
            json_builder_add_string_value(builder, catalog[i].label);

            if (catalog[i].note != NULL) {
                json_builder_set_member_name(builder, "note");
                json_builder_add_string_value(builder, catalog[i].note);
            }

            json_builder_set_member_name(builder, "group");
            json_builder_add_string_value(builder, catalog[i].group);
            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "tool.rpc") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *token = clawt_ipc_payload_string(payload, "token");
        JsonNode *rpc = (payload != NULL &&
                         json_object_has_member(payload, "request"))
                        ? json_object_get_member(payload, "request") : NULL;
        g_autoptr(JsonNode) rpc_response = NULL;

        /*
         * The orchestration tools, reachable over IPC.
         *
         * They were served only over the agent's link, as mcp.request
         * frames -- which assumed something on the agent side would
         * relay them into its AI session. Nothing did, and nothing
         * could: an agent runs a CLI whose only way of being given
         * tools is an --mcp-config pointing at a real MCP server. This
         * is the verb clawtilla-mcp-server speaks so that server can
         * exist.
         */
        if (agent_id == NULL || rpc == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "agent and request are both required");

        /*
         * The agent's own token, checked the same way the link checks
         * it. The socket's permissions are the first line; this stops
         * one agent on this machine calling tools as another.
         */
        if (!authenticate_agent(agent_id, token, self))
            return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                       "that is not this agent's token");

        rpc_response = clawt_mcp_tools_call(self->mcp_tools, agent_id, rpc);

        if (rpc_response == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "the tool produced no response");

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "response");
        json_builder_add_value(builder, json_node_ref(rpc_response));
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "model.list") == 0) {
        const ClawtProviderInfo *catalog;
        gboolean refresh = clawt_ipc_payload_boolean(payload, "refresh",
                                                      FALSE);
        gsize n_providers = 0;
        gsize i;

        catalog = clawt_model_catalog_get(&n_providers);

        /*
         * A stale cache is refreshed behind this request rather than
         * during it. The caller gets whatever is known now; the next
         * one gets the fresh answer.
         */
        if (refresh &&
            (self->model_cache_at == 0 ||
             g_get_monotonic_time() - self->model_cache_at >
                 (gint64)MODEL_CACHE_TTL_SECONDS * G_USEC_PER_SEC))
            warm_model_cache(self);

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

            /*
             * Whether libreclaw can actually run an agent on this
             * provider.  Its provider table is command-line only and
             * rewrites anything else to claude-code with a warning, so a
             * client that offers every provider here lets someone pick
             * OpenAI and quietly get Claude Code with "gpt-4o" in the
             * model field.
             */
            json_builder_set_member_name(builder, "agent");
            json_builder_add_boolean_value(builder, catalog[i].agent);

            /*
             * Whether this provider can be given tools, which decides
             * whether it can design an agent.  A client that offers
             * every provider for designing offers ones that will be
             * refused after the person has filled in the whole form.
             */
            json_builder_set_member_name(builder, "tools");
            json_builder_add_boolean_value(builder, catalog[i].tools);

            json_builder_set_member_name(builder, "models");
            json_builder_begin_array(builder);

            /*
             * The provider's own list, when asked for and reachable.
             *
             * The hardcoded table goes stale -- it offered grok-3 and
             * grok-4 well after 4.5 and 4.6 had shipped -- so a person
             * choosing a model should be shown what the provider
             * actually runs. Falls back to the table rather than
             * failing: no key, or no network, is not a reason to offer
             * nothing.
             */
            if (refresh && catalog[i].tools) {
                /*
                 * From the cache, never by asking now.  Asking here made
                 * the request take as long as the slowest provider, and
                 * both the new-agent dialog and the agent inspector ask
                 * on every build -- so pressing + or clicking an agent
                 * appeared to hang.
                 */
                GStrv live = g_hash_table_lookup(self->model_cache,
                                                  catalog[i].id);

                if (live != NULL && live[0] != NULL) {
                    gsize k;

                    for (k = 0; live[k] != NULL; k++) {
                        json_builder_begin_object(builder);
                        json_builder_set_member_name(builder, "id");
                        json_builder_add_string_value(builder, live[k]);
                        json_builder_set_member_name(builder, "label");
                        json_builder_add_string_value(builder, live[k]);
                        json_builder_end_object(builder);
                    }

                    json_builder_end_array(builder);
                    json_builder_set_member_name(builder, "live");
                    json_builder_add_boolean_value(builder, TRUE);
                    json_builder_end_object(builder);
                    continue;
                }
            }

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

    if (g_strcmp0(kind, "connector.catalog") == 0) {
        GPtrArray *catalog = daemon_catalog(self);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "connectors");
        json_builder_begin_array(builder);

        for (i = 0; catalog != NULL && i < catalog->len; i++) {
            const ClawtConnectorInfo *info = g_ptr_array_index(catalog, i);

            json_builder_begin_object(builder);
            add_string_member(builder, "id", info->id);
            add_string_member(builder, "name", info->name);
            add_string_member(builder, "summary", info->summary);
            add_string_member(builder, "category", info->category);
            add_string_member(builder, "auth",
                              clawt_enum_to_nick(CLAWT_TYPE_CONNECTOR_AUTH,
                                                 (gint)info->auth));
            add_string_member(builder, "scopes", info->scopes);
            add_string_member(builder, "client_id_help", info->client_id_help);
            add_string_member(builder, "docs_url", info->docs_url);
            add_string_member(builder, "default_instance",
                              info->default_instance);

            /*
             * Whether a server is known matters as much as the auth
             * does: a connector with neither this nor a `command` in
             * the integration authenticates perfectly and hands the
             * agent nothing.
             */
            json_builder_set_member_name(builder, "has_server");
            json_builder_add_boolean_value(builder,
                                           info->server_command != NULL ||
                                           info->server_url != NULL);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "connector.list") == 0) {
        GPtrArray *integrations = clawt_config_get_integrations(self->config);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "connectors");
        json_builder_begin_array(builder);

        for (i = 0; integrations != NULL && i < integrations->len; i++) {
            ClawtIntegrationConfig *instance =
                g_ptr_array_index(integrations, i);
            const gchar *token_file;
            g_autoptr(ClawtOauthToken) token = NULL;

            if (g_strcmp0(clawt_integration_config_get_type_id(instance),
                          "connector") != 0)
                continue;

            json_builder_begin_object(builder);
            add_string_member(builder, "name",
                              clawt_integration_config_get_name(instance));
            add_string_member(builder, "provider",
                              clawt_integration_config_get_string(
                                  instance, NULL, "provider"));
            add_string_member(builder, "account",
                              clawt_integration_config_get_string(
                                  instance, NULL, "account"));
            add_string_member(builder, "scope",
                              clawt_enum_to_nick(
                                  CLAWT_TYPE_INTEGRATION_SCOPE,
                                  (gint)clawt_integration_config_get_scope(
                                      instance)));

            json_builder_set_member_name(builder, "enabled");
            json_builder_add_boolean_value(
                builder, clawt_integration_config_get_enabled(instance));

            token_file = clawt_integration_config_get_string(instance, NULL,
                                                             "token_file");

            if (token_file != NULL)
                token = clawt_oauth_token_load(token_file, NULL);

            /*
             * Everything about the credential except the credential.
             * Whether it exists, when it stops working and whether it
             * can renew itself are the three things somebody looking at
             * this list needs; the value is the one thing that must
             * never come back over IPC.
             */
            json_builder_set_member_name(builder, "connected");
            json_builder_add_boolean_value(builder, token != NULL);

            json_builder_set_member_name(builder, "expires_at");
            json_builder_add_int_value(builder,
                                       token != NULL ? token->expires_at : 0);

            json_builder_set_member_name(builder, "renewable");
            json_builder_add_boolean_value(
                builder, token != NULL && token->refresh_token != NULL);

            if (token != NULL)
                add_string_member(builder, "granted_scopes", token->scopes);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "connector.begin") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autofree gchar *auth_url = NULL;
        g_autofree gchar *token_url = NULL;
        const gchar *client_id;
        const gchar *instance_url;
        const gchar *scopes;
        ConnectorFlow *flow;

        sweep_connector_flows(self);

        binding = connector_binding(self, name, &connector, &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (connector->auth == CLAWT_CONNECTOR_AUTH_API_KEY ||
            connector->auth == CLAWT_CONNECTOR_AUTH_NONE)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this connector takes a key rather "
                                       "than an authorization; use "
                                       "`clawtilla connector key`");

        client_id = clawt_integration_binding_get_string(binding, "client_id");

        if (client_id == NULL)
            return clawt_ipc_error_new(
                request, CLAWT_ERROR_CONFIG_INVALID,
                connector->client_id_help != NULL
                ? connector->client_id_help
                : "this connector needs a client_id you registered with "
                  "the provider");

        instance_url = clawt_integration_binding_get_string(binding,
                                                            "instance");
        auth_url = clawt_connector_resolve_url(connector, connector->auth_url,
                                               instance_url);
        token_url = clawt_connector_resolve_url(connector,
                                                connector->token_url,
                                                instance_url);

        if (auth_url == NULL || token_url == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_CONFIG_INVALID,
                                       "this connector has no authorization "
                                       "endpoints");

        scopes = clawt_integration_binding_get_string(binding, "scopes");

        if (scopes == NULL)
            scopes = connector->scopes;

        flow = g_new0(ConnectorFlow, 1);
        flow->daemon = self;
        flow->id = g_uuid_string_random();
        flow->name = g_strdup(name);
        flow->token_url = g_steal_pointer(&token_url);
        flow->client_id = g_strdup(client_id);
        flow->client_secret = connector_client_secret(self, binding);

        g_hash_table_insert(self->connector_flows, g_strdup(flow->id), flow);

        if (connector->auth == CLAWT_CONNECTOR_AUTH_DEVICE) {
            BeginWait *begin = g_new0(BeginWait, 1);

            begin->flow = flow;
            begin->pending = clawt_ipc_server_defer(self->ipc_server, request);

            if (begin->pending == NULL) {
                g_free(begin);
                g_hash_table_remove(self->connector_flows, flow->id);

                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           "this request cannot be answered "
                                           "later");
            }

            /*
             * Deferred because the codes come from the provider, and
             * there is nothing to show anybody until they do.  The poll
             * that follows is *not* deferred onto this request -- it
             * takes as long as a person takes, which is what
             * connector.await is for.
             */
            clawt_oauth_device_begin_async(auth_url, client_id, scopes, NULL,
                                           on_connector_begun, begin);
            return NULL;
        }

        /* The authorization-code flow, for providers with no device grant. */
        {
            g_autofree gchar *state = clawt_oauth_pkce_verifier();
            g_autofree gchar *challenge = NULL;
            g_autofree gchar *url = NULL;
            gint64 port = clawt_config_get_int(self->config,
                                               "connectors.redirect_port");

            flow->verifier = clawt_oauth_pkce_verifier();

            if (flow->verifier == NULL || state == NULL) {
                g_hash_table_remove(self->connector_flows, flow->id);

                return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                           "this machine has no usable "
                                           "randomness, and a guessable "
                                           "verifier is no protection at "
                                           "all");
            }

            flow->redirect_uri =
                g_strdup_printf("http://127.0.0.1:%d/callback", (gint)port);

            challenge = clawt_oauth_pkce_challenge(flow->verifier);
            url = clawt_oauth_authorize_url(auth_url, client_id,
                                            flow->redirect_uri, scopes, state,
                                            challenge);

            /*
             * The listener goes up before the URL is handed out.  A
             * person who is quick would otherwise be redirected to a
             * port nothing is listening on, and the browser would show
             * a connection refused for an authorization that in fact
             * succeeded.
             */
            clawt_oauth_await_redirect_async((guint)port, state, 600, NULL,
                                             on_connector_redirected, flow);

            json_builder_begin_object(builder);
            add_string_member(builder, "flow", flow->id);
            add_string_member(builder, "method", "pkce");
            add_string_member(builder, "authorize_url", url);
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }
    }

    if (g_strcmp0(kind, "connector.await") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "flow");
        ConnectorFlow *flow = (id != NULL)
            ? g_hash_table_lookup(self->connector_flows, id) : NULL;

        if (flow == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no connection attempt with "
                                       "that id");

        /*
         * A flow that finished before anybody asked keeps its answer.
         * Somebody who started an authorization, walked away and came
         * back should not find that the result was delivered to nobody.
         */
        if (flow->settled) {
            gboolean ok = flow->ok;
            g_autofree gchar *message = g_strdup(flow->message);
            g_autofree gchar *flow_name = g_strdup(flow->name);

            g_hash_table_remove(self->connector_flows, id);

            if (!ok)
                return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                           message != NULL ? message
                                           : "the flow did not complete");

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "connected");
            json_builder_add_boolean_value(builder, TRUE);
            add_string_member(builder, "name", flow_name);
            json_builder_end_object(builder);

            return clawt_ipc_response_new(request,
                                          json_builder_get_root(builder));
        }

        flow->waiter = clawt_ipc_server_defer(self->ipc_server, request);

        if (flow->waiter == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");

        return NULL;
    }

    if (g_strcmp0(kind, "connector.key") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *key = clawt_ipc_payload_string(payload, "key");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autoptr(ClawtOauthToken) token = NULL;
        g_autofree gchar *secrets_dir = NULL;
        g_autofree gchar *path = NULL;

        binding = connector_binding(self, name, &connector, &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (key == NULL || *key == '\0')
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "no key was given");

        /*
         * Accepted for any connector, not only the api_key ones.  A
         * personal access token is a perfectly good credential for
         * GitHub or GitLab, and taking one here means somebody who
         * wants an agent reading their repositories does not first have
         * to go and register an OAuth application.
         *
         * It is stored in the same shape as a negotiated token, so
         * everything downstream -- the relay, the health check, the
         * list -- has one thing to read rather than two.
         */
        token = g_new0(ClawtOauthToken, 1);
        token->access_token = g_strdup(key);

        if (!store_connector_token(self, name, token, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "integration.changed", name);

        secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");
        path = clawt_connector_token_path(secrets_dir, name);

        /*
         * The path, never the value.  Handing the key back to the client
         * that sent it would put a live credential into the memory of
         * every client that asked.
         */
        json_builder_begin_object(builder);
        add_string_member(builder, "token_file", path);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "connector.refresh") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autoptr(ClawtOauthToken) token = NULL;
        g_autofree gchar *token_url = NULL;
        const gchar *token_file;
        const gchar *client_id;
        RefreshJob *job;

        binding = connector_binding(self, name, &connector, &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        token_file = clawt_integration_binding_get_string(binding,
                                                          "token_file");
        token = (token_file != NULL) ? clawt_oauth_token_load(token_file, NULL)
                                     : NULL;

        if (token == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_AUTH,
                                       "it is not connected yet");

        if (token->refresh_token == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "the provider issued nothing to renew "
                                       "with; connect again instead");

        client_id = clawt_integration_binding_get_string(binding, "client_id");
        token_url = clawt_connector_resolve_url(
            connector, connector->token_url,
            clawt_integration_binding_get_string(binding, "instance"));

        if (client_id == NULL || token_url == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_CONFIG_INVALID,
                                       "there is nowhere to renew it");

        job = g_new0(RefreshJob, 1);
        job->daemon = self;
        job->name = g_strdup(name);
        job->pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (job->pending == NULL) {
            refresh_job_free(job);

            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        {
            g_autofree gchar *secret = connector_client_secret(self, binding);

            clawt_oauth_refresh_async(token_url, client_id, secret,
                                      token->refresh_token, NULL,
                                      on_connector_refreshed, job);
        }

        return NULL;
    }

    if (g_strcmp0(kind, "connector.revoke") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const ClawtConnectorInfo *connector = NULL;
        g_autoptr(ClawtIntegrationBinding) binding = NULL;
        g_autoptr(ClawtOauthToken) token = NULL;
        g_autofree gchar *revoke_url = NULL;
        const gchar *token_file;

        binding = connector_binding(self, name, &connector, &error);

        if (binding == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        token_file = clawt_integration_binding_get_string(binding,
                                                          "token_file");
        token = (token_file != NULL) ? clawt_oauth_token_load(token_file, NULL)
                                     : NULL;

        revoke_url = clawt_connector_resolve_url(
            connector, connector->revoke_url,
            clawt_integration_binding_get_string(binding, "instance"));

        /*
         * The local copy goes whatever the provider says.  Somebody who
         * asked to revoke wants the fleet to stop using it now, and a
         * provider that is unreachable must not leave an agent holding
         * a working credential until the network comes back.
         */
        if (!forget_connector_token(self, name, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "integration.changed", name);

        if (token != NULL && revoke_url != NULL) {
            RevokeJob *job = g_new0(RevokeJob, 1);

            job->pending = clawt_ipc_server_defer(self->ipc_server, request);

            if (job->pending != NULL) {
                job->name = g_strdup(name);

                clawt_oauth_revoke_async(
                    revoke_url, clawt_integration_binding_get_string(
                                    binding, "client_id"),
                    NULL, token->access_token, NULL, on_connector_revoked,
                    job);

                return NULL;
            }

            g_free(job);
        }

        /*
         * Says plainly when the provider was not told.  A person who
         * believes a token is dead and finds it working months later
         * has been misled by this reply, and the fix -- their settings
         * page -- is somewhere only they can go.
         */
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "forgotten");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_set_member_name(builder, "told_provider");
        json_builder_add_boolean_value(builder, FALSE);

        if (token != NULL && revoke_url == NULL)
            add_string_member(builder, "note",
                              "this provider offers no revocation endpoint; "
                              "the credential is gone from here but remains "
                              "valid until you withdraw it in their "
                              "settings");

        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "integration.types") == 0) {
        const ClawtIntegrationInfo *info;
        gsize n_integrations = 0;
        gsize i;

        info = clawt_integration_list(&n_integrations);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "types");
        json_builder_begin_array(builder);

        for (i = 0; i < n_integrations; i++) {
            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, info[i].id);
            json_builder_set_member_name(builder, "kind");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_INTEGRATION_KIND,
                                            (gint)info[i].kind));
            json_builder_set_member_name(builder, "summary");
            json_builder_add_string_value(builder, info[i].summary);
            json_builder_set_member_name(builder, "one_per_agent");
            json_builder_add_boolean_value(builder, info[i].one_per_agent);
            json_builder_set_member_name(builder, "one_per_fleet");
            json_builder_add_boolean_value(builder, info[i].one_per_fleet);

            add_key_array(builder, "required_keys", info[i].required_keys);
            add_key_array(builder, "credential_keys", info[i].credential_keys);
            add_key_array(builder, "identity_keys", info[i].identity_keys);

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "integration.list") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        GPtrArray *instances = clawt_config_get_integrations(self->config);
        g_autoptr(GPtrArray) warnings = NULL;
        guint i;

        if (agent_id != NULL && agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        json_builder_begin_object(builder);

        /*
         * The shared instances, whatever was asked for.  A settings page
         * shows all of them; an agent inspector shows which of them reach
         * that agent, which is the `covers` flag rather than a filter --
         * the dialog needs the ones it could turn on as well as the ones
         * that are on.
         */
        json_builder_set_member_name(builder, "integrations");
        json_builder_begin_array(builder);

        for (i = 0; instances != NULL && i < instances->len; i++) {
            ClawtIntegrationConfig *instance =
                g_ptr_array_index(instances, i);

            add_integration_object(builder, self->config, instance, agent_id);
        }

        json_builder_end_array(builder);

        /*
         * And what one agent actually has, inline blocks included.  A
         * client cannot work this out from the list above, because an
         * agent's own `integrations:` block is not an instance and never
         * appears there.
         */
        if (agent_config != NULL) {
            g_autoptr(GPtrArray) bindings =
                clawt_integration_resolve_for_agent(self->config,
                                                    agent_config);

            json_builder_set_member_name(builder, "bindings");
            json_builder_begin_array(builder);

            for (i = 0; i < bindings->len; i++)
                add_binding_object(builder,
                                   g_ptr_array_index(bindings, i));

            json_builder_end_array(builder);
        }

        clawt_integration_validate_fleet(self->config, &warnings);

        json_builder_set_member_name(builder, "warnings");
        json_builder_begin_array(builder);

        for (i = 0; warnings != NULL && i < warnings->len; i++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(warnings, i));

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "integration.add") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        const gchar *type_id = clawt_ipc_payload_string(payload, "type");
        ClawtIntegrationConfig *instance;

        instance = clawt_config_add_integration(self->config, name, type_id,
                                                &error);

        if (instance == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        if (clawt_integration_find(type_id) == NULL) {
            /*
             * Rolled back rather than left as a shadow.  A shadow agent
             * earns its keep because the config was already on disk when
             * we met it; here somebody has just typed a type that does
             * not exist, and the honest answer is to say so and change
             * nothing.
             */
            clawt_config_remove_integration(self->config, name);

            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "there is no integration type called "
                                       "that");
        }

        if (!apply_integration_fields(instance, payload, &error)) {
            clawt_config_remove_integration(self->config, name);
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (!clawt_config_save(self->config, &error)) {
            clawt_config_remove_integration(self->config, name);
            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "integration.changed", name);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "integration.update") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");
        ClawtIntegrationConfig *instance = (name != NULL)
            ? clawt_config_get_integration(self->config, name) : NULL;

        if (instance == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no integration called that");

        if (!apply_integration_fields(instance, payload, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "integration.changed", name);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "integration.remove") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "name");

        if (name == NULL ||
            !clawt_config_remove_integration(self->config, name))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no integration called that");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        /*
         * The credential file it wrote is deliberately left where it is.
         * Removing an integration is a config change, and taking a token
         * off disk as a side effect of it is the kind of helpfulness that
         * is only noticed when it was wrong.
         */
        clawt_event_bus_emit(self->bus, "integration.changed", name);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "integration.health") == 0) {
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        const gchar *name = clawt_ipc_payload_string(payload, "integration");
        ClawtAgentConfig *agent_config = (agent_id != NULL)
            ? clawt_config_get_agent(self->config, agent_id) : NULL;
        g_autoptr(GPtrArray) bindings = NULL;
        HealthRun *run;
        guint i;

        if (agent_config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "no such agent");

        bindings = clawt_integration_resolve_for_agent(self->config,
                                                       agent_config);

        run = g_new0(HealthRun, 1);
        run->pending = clawt_ipc_server_defer(self->ipc_server, request);
        run->checks = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_integration_binding_unref);
        run->results = g_ptr_array_new_with_free_func(
            (GDestroyNotify)health_result_free);
        run->timeout = (guint)clawt_ipc_payload_int(payload, "timeout", 10);

        if (run->pending == NULL) {
            health_run_free(run);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        for (i = 0; i < bindings->len; i++) {
            ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, i);

            if (name != NULL &&
                g_strcmp0(clawt_integration_binding_get_name(binding),
                          name) != 0 &&
                g_strcmp0(clawt_integration_binding_get_info(binding)->id,
                          name) != 0)
                continue;

            g_ptr_array_add(run->checks,
                            clawt_integration_binding_ref(binding));
        }

        health_run_start(run);

        /*
         * NULL, not a frame: the answer goes out from health_run_finish()
         * when the last check comes back.  A handler that waits here
         * would hold the daemon's main context for the whole timeout,
         * which is exactly the ten seconds in which nothing else is
         * routed.
         */
        return NULL;
    }

    if (g_strcmp0(kind, "integration.notify_test") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "integration");
        ClawtIpcPending *pending;

        if (self->notifier == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this daemon has no notifier");

        pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (pending == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");

        /*
         * A notifier is the one thing in a fleet you cannot tell is
         * working by looking at it: it is correct precisely when nothing
         * happens. This is the button that makes something happen.
         */
        clawt_notifier_test_async(self->notifier, name, NULL,
                                  on_notify_tested, pending);

        return NULL;
    }

    if (g_strcmp0(kind, "integration.matrix_login") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "integration");
        const gchar *homeserver = clawt_ipc_payload_string(payload,
                                                           "homeserver");
        const gchar *user = clawt_ipc_payload_string(payload, "user");
        const gchar *password = clawt_ipc_payload_string(payload, "password");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtIntegrationConfig *instance = (name != NULL)
            ? clawt_config_get_integration(self->config, name) : NULL;
        MatrixLogin *login;

        if (instance == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no integration called that");

        if (homeserver == NULL)
            homeserver = clawt_integration_config_get_string(instance,
                                                             agent_id,
                                                             "homeserver");

        if (homeserver == NULL || user == NULL || password == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "a homeserver, a user and a password "
                                       "are all needed");

        login = g_new0(MatrixLogin, 1);
        login->daemon = self;
        login->pending = clawt_ipc_server_defer(self->ipc_server, request);
        login->name = g_strdup(name);
        login->agent_id = g_strdup(agent_id);
        login->homeserver = g_strdup(homeserver);

        if (login->pending == NULL) {
            matrix_login_free(login);
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");
        }

        {
            g_autofree gchar *device = g_strdup_printf(
                "clawtilla (%s)", agent_id != NULL ? agent_id : name);

            clawt_matrix_login_async(homeserver, user, password, device,
                                     NULL, on_matrix_login, login);
        }

        return NULL;
    }

    if (g_strcmp0(kind, "integration.matrix_rooms") == 0) {
        const gchar *name = clawt_ipc_payload_string(payload, "integration");
        const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
        ClawtIntegrationConfig *instance = (name != NULL)
            ? clawt_config_get_integration(self->config, name) : NULL;
        g_autoptr(ClawtSecretRef) ref = NULL;
        g_autofree gchar *token = NULL;
        g_autofree gchar *secrets_dir = NULL;
        const gchar *homeserver;
        ClawtIpcPending *pending;

        if (instance == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no integration called that");

        homeserver = clawt_integration_config_get_string(instance, agent_id,
                                                         "homeserver");
        ref = clawt_integration_config_get_secret(instance, agent_id,
                                                  "access_token");

        if (homeserver == NULL || ref == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_CONFIG_INVALID,
                                       "sign in first: there is no "
                                       "homeserver and token to list with");

        secrets_dir = clawt_config_get_path_value(self->config, "secrets.dir");
        token = clawt_secret_ref_resolve(
            ref, secrets_dir,
            (guint)clawt_config_get_int(self->config,
                                        "secrets.command_timeout_seconds"),
            &error);

        if (token == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_SECRET,
                                       error->message);

        pending = clawt_ipc_server_defer(self->ipc_server, request);

        if (pending == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                       "this request cannot be answered "
                                       "later");

        clawt_matrix_rooms_async(homeserver, token, NULL, on_matrix_rooms,
                                 pending);

        return NULL;
    }

    /* ── routines ── */

    if (g_strcmp0(kind, "routine.list") == 0) {
        GPtrArray *routines = clawt_config_get_routines(self->config);
        guint i;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "routines");
        json_builder_begin_array(builder);

        for (i = 0; routines != NULL && i < routines->len; i++) {
            ClawtRoutine *routine = g_ptr_array_index(routines, i);
            const gchar *id = clawt_routine_get_id(routine);
            g_autofree gchar *expression = NULL;
            g_autoptr(GDateTime) next = NULL;
            g_autoptr(GError) cron_error = NULL;
            const ClawtSchemaEntry *entries;
            ClawtRunState state = CLAWT_RUN_NEVER;
            const gchar *detail = NULL;
            gint64 last;
            gsize n_entries = 0;
            gsize k;

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "id");
            json_builder_add_string_value(builder, id);

            /*
             * The fields come from the schema rather than a list here,
             * for the reason the integration ones now do: a list in the
             * daemon and a list in the schema drift, and the drift is
             * silent.
             */
            entries = clawt_config_schema_get(&n_entries);

            for (k = 0; k < n_entries; k++) {
                const gchar *leaf;

                if (!g_str_has_prefix(entries[k].key, "routines."))
                    continue;

                leaf = entries[k].key + strlen("routines.");

                if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0)
                    continue;

                switch (entries[k].type) {
                case CLAWT_SCHEMA_BOOLEAN:
                    json_builder_set_member_name(builder, leaf);
                    json_builder_add_boolean_value(
                        builder, clawt_routine_get_boolean(routine, leaf));
                    break;

                case CLAWT_SCHEMA_INT:
                    json_builder_set_member_name(builder, leaf);
                    json_builder_add_int_value(
                        builder, clawt_routine_get_int(routine, leaf));
                    break;

                default: {
                    const gchar *value =
                        clawt_routine_get_string(routine, leaf);

                    if (value == NULL)
                        break;

                    json_builder_set_member_name(builder, leaf);
                    json_builder_add_string_value(builder, value);
                    break;
                }
                }
            }

            /*
             * What it actually means, worked out here.  A client that
             * had to turn "weekdays at 09:00" into an expression itself
             * would be a second implementation of the schedule.
             */
            expression = clawt_routine_get_cron(routine, &cron_error);

            if (expression != NULL) {
                json_builder_set_member_name(builder, "expression");
                json_builder_add_string_value(builder, expression);
            } else if (cron_error != NULL) {
                json_builder_set_member_name(builder, "problem");
                json_builder_add_string_value(builder, cron_error->message);
            }

            next = (self->routines != NULL)
                ? clawt_routine_runner_next_run(self->routines, id) : NULL;

            if (next != NULL) {
                g_autofree gchar *formatted =
                    g_date_time_format_iso8601(next);

                json_builder_set_member_name(builder, "next_run");
                json_builder_add_string_value(builder, formatted);
            }

            last = (self->routines != NULL)
                ? clawt_routine_runner_last_run(self->routines, id, &state,
                                                &detail) : 0;

            json_builder_set_member_name(builder, "last_run");
            json_builder_add_int_value(builder, last);
            json_builder_set_member_name(builder, "last_state");
            json_builder_add_string_value(
                builder, clawt_enum_to_nick(CLAWT_TYPE_RUN_STATE,
                                            (gint)state));

            if (detail != NULL) {
                json_builder_set_member_name(builder, "last_detail");
                json_builder_add_string_value(builder, detail);
            }

            json_builder_end_object(builder);
        }

        json_builder_end_array(builder);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "routine.add") == 0 ||
        g_strcmp0(kind, "routine.update") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        gboolean adding = g_strcmp0(kind, "routine.add") == 0;
        ClawtRoutine *routine;
        const ClawtSchemaEntry *entries;
        gsize n_entries = 0;
        gsize i;

        if (adding) {
            routine = clawt_config_add_routine(self->config, id, &error);

            if (routine == NULL)
                return clawt_ipc_error_new(request, error->code,
                                           error->message);
        } else {
            routine = (id != NULL)
                ? clawt_config_get_routine(self->config, id) : NULL;

            if (routine == NULL)
                return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                           "there is no routine called "
                                           "that");
        }

        entries = clawt_config_schema_get(&n_entries);

        for (i = 0; i < n_entries; i++) {
            const gchar *leaf;

            if (!g_str_has_prefix(entries[i].key, "routines."))
                continue;

            leaf = entries[i].key + strlen("routines.");

            if (strchr(leaf, '.') != NULL || g_strcmp0(leaf, "id") == 0 ||
                !json_object_has_member(payload, leaf))
                continue;

            switch (entries[i].type) {
            case CLAWT_SCHEMA_BOOLEAN:
                clawt_routine_set_boolean(
                    routine, leaf,
                    clawt_ipc_payload_boolean(payload, leaf, FALSE));
                break;

            case CLAWT_SCHEMA_INT:
                clawt_routine_set_int(routine, leaf,
                                      clawt_ipc_payload_int(payload, leaf, 0));
                break;

            default:
                clawt_routine_set_string(
                    routine, leaf, clawt_ipc_payload_string(payload, leaf));
                break;
            }
        }

        /*
         * The schedule is checked here, while somebody is still looking
         * at what they typed -- rather than at the next tick, in a
         * warning nobody is watching for.
         */
        {
            g_autofree gchar *expression = NULL;
            g_autoptr(GError) cron_error = NULL;

            expression = clawt_routine_get_cron(routine, &cron_error);

            if (expression == NULL && cron_error != NULL) {
                if (adding)
                    clawt_config_remove_routine(self->config, id);

                return clawt_ipc_error_new(request,
                                           CLAWT_ERROR_INVALID_ARGUMENT,
                                           cron_error->message);
            }
        }

        if (!clawt_config_save(self->config, &error)) {
            if (adding)
                clawt_config_remove_routine(self->config, id);

            return clawt_ipc_error_new(request, error->code, error->message);
        }

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "routine.changed", id);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "id");
        json_builder_add_string_value(builder, id);
        json_builder_end_object(builder);

        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "routine.remove") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");

        if (id == NULL || !clawt_config_remove_routine(self->config, id))
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND,
                                       "there is no routine called that");

        if (!clawt_config_save(self->config, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        if (!clawt_daemon_reload(self, &error))
            return clawt_ipc_error_new(request, error->code, error->message);

        clawt_event_bus_emit(self->bus, "routine.changed", id);

        return clawt_ipc_response_new(request, NULL);
    }

    if (g_strcmp0(kind, "routine.run") == 0) {
        const gchar *id = clawt_ipc_payload_string(payload, "id");
        const gchar *task_id;

        if (self->routines == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_SUPPORTED,
                                       "this daemon has no scheduler");

        task_id = clawt_routine_runner_run_now(self->routines, id, &error);

        if (task_id == NULL)
            return clawt_ipc_error_new(request, error->code, error->message);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "task");
        json_builder_add_string_value(builder, task_id);
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
bind_spec_free(gpointer data)
{
    BindSpec *spec = data;

    g_free(spec->host);
    g_free(spec);
}

gboolean
clawt_daemon_set_bind_addresses(ClawtDaemon        *self,
                                const gchar *const *addresses,
                                GError            **error)
{
    gsize i;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    g_clear_pointer(&self->bind_specs, g_ptr_array_unref);
    self->bind_specs = g_ptr_array_new_with_free_func(bind_spec_free);

    /*
     * The call itself is the override, not the contents.  Passing NULL or
     * an empty list means "listen on no network address at all", which is
     * what --no-bind asks for; not calling this at all leaves the
     * configuration in charge.
     */
    self->bind_override = TRUE;

    for (i = 0; addresses != NULL && addresses[i] != NULL; i++) {
        BindSpec *spec = g_new0(BindSpec, 1);

        if (!clawt_ipc_parse_listen_address(addresses[i],
                                            CLAWT_DEFAULT_TCP_PORT,
                                            &spec->host, &spec->port,
                                            error)) {
            g_free(spec);

            /*
             * Rolled back rather than left half applied.  A daemon that
             * kept the addresses it managed to parse would listen on some
             * of what was asked for and report an error about the rest,
             * which is the worst of both.
             */
            g_clear_pointer(&self->bind_specs, g_ptr_array_unref);
            self->bind_override = FALSE;
            return FALSE;
        }

        g_ptr_array_add(self->bind_specs, spec);
    }

    return TRUE;
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

    g_clear_pointer(&self->drafts, g_hash_table_unref);
    g_clear_object(&self->notifier);
    g_clear_object(&self->routines);
    g_clear_object(&self->automation);
    g_clear_pointer(&self->model_cache, g_hash_table_unref);
    g_clear_pointer(&self->connector_flows, g_hash_table_unref);
    g_clear_pointer(&self->connector_catalog, g_ptr_array_unref);
    g_clear_pointer(&self->bind_specs, g_ptr_array_unref);
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
    self->drafts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          g_object_unref);
    self->model_cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                               g_free,
                                               (GDestroyNotify)g_strfreev);
    self->connector_flows =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                              (GDestroyNotify)connector_flow_free);
    self->running = FALSE;
}
