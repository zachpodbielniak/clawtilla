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
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

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

G_DEFINE_FINAL_TYPE(ClawtDaemon, clawt_daemon, G_TYPE_OBJECT)

enum {
    SIGNAL_STARTED,
    SIGNAL_STOPPED,
    SIGNAL_RELOADED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

static gboolean deliver_for_tools(const gchar   *from_agent,
                                  const gchar   *target,
                                  const gchar   *body,
                                  const gchar   *task_id,
                                  gint           depth,
                                  ClawtPriority  priority,
                                  gpointer       user_data,
                                  GError       **error);

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
gboolean
clawt_daemon_authenticate_agent(const gchar *agent_id, const gchar *token,
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
    "daemon.lock\n"
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

/*
 * One daemon per state directory, enforced by the kernel.
 *
 * Two daemons sharing a state directory is not a degraded mode, it is
 * data loss: each keeps its own room manager and save_room() rewrites
 * the *whole* transcript from memory on every message, so the last one
 * to write wins and the other's messages are gone.  It has happened on
 * a real fleet -- four messages of a conversation with the chief of
 * staff, routed correctly, delivered, and then deleted by the other
 * daemon's next flush.
 *
 * The socket was already guarded, by a connect probe, and a probe is the
 * wrong instrument: it answers "did anything reply just now", which a
 * busy daemon fails.  One that has been wrong once unlinks a live socket
 * and leaves the running daemon's clients talking to a path that no
 * longer exists -- exactly what clear_stale_socket()'s own comment says
 * must not happen, and exactly what did happen.
 *
 * A lock cannot be wrong that way: the kernel holds it, it is released
 * when the last descriptor closes, and that includes a daemon killed
 * with SIGKILL, so there is no stale lock to reason about.  The state
 * directory is the thing that must not be shared, so it is what carries
 * the lock -- not the socket, which is only one of the things two
 * daemons would fight over.
 *
 * The pid is written in so the refusal can name who holds it. It is a
 * courtesy for the person reading the error, never a check: the lock is
 * what excludes, and the file's contents are not consulted.
 */
static gboolean
acquire_state_lock(const gchar *state_dir, gint *out_fd, GError **error)
{
    g_autofree gchar *path = g_build_filename(state_dir, "daemon.lock", NULL);
    g_autofree gchar *held_by = NULL;
    g_autofree gchar *pid_text = NULL;
    gint fd;

    fd = g_open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);

    if (fd < 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not open the state lock %s: %s",
                    path, g_strerror(errno));
        return FALSE;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        gint saved = errno;

        if (g_file_get_contents(path, &held_by, NULL, NULL) &&
            held_by != NULL && held_by[0] != '\0')
            g_strstrip(held_by);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS,
                    "another clawtilla daemon is using the state directory "
                    "%s%s%s%s. Two daemons sharing one state directory "
                    "overwrite each other's transcripts, so this one will "
                    "not start; use a different daemon.state_dir, or stop "
                    "the other one (%s)",
                    state_dir,
                    (held_by != NULL && held_by[0] != '\0') ? " (held by pid " : "",
                    (held_by != NULL && held_by[0] != '\0') ? held_by : "",
                    (held_by != NULL && held_by[0] != '\0') ? ")" : "",
                    g_strerror(saved));
        close(fd);
        return FALSE;
    }

    /*
     * Truncated and rewritten only after the lock is ours, so a refused
     * daemon never overwrites the pid of the one that holds it.
     */
    if (ftruncate(fd, 0) == 0) {
        pid_text = g_strdup_printf("%d\n", (gint)getpid());
        if (write(fd, pid_text, strlen(pid_text)) < 0)
            g_debug("state lock: could not record the pid: %s",
                    g_strerror(errno));
    }

    *out_fd = fd;
    return TRUE;
}

gboolean
clawt_daemon_prepare_state_git(const gchar *state_dir, gboolean init_repo,
                               gboolean *created, gchar **ignore_path,
                               GError **error)
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
        const gchar *band = g_hash_table_lookup(params, "priority");
        ClawtPriority priority = CLAWT_PRIORITY_NORMAL;
        g_autofree gchar *refusal = NULL;

        if (target == NULL || body == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "a target and a body are both needed");
            return FALSE;
        }

        /*
         * `priority` has been in message_agent's declared parameters for
         * as long as the pod module has existed, and nothing here read
         * it -- so a pod that set it was accepted, reported ok, and
         * queued at NORMAL like everything else.  Judged by the same
         * function the tool uses, so a pod and an agent cannot come to
         * mean different things by "urgent".
         *
         * Refused rather than defaulted, and refused *before* anything is
         * queued.  A pod runs unattended: a band nobody noticed was
         * wrong would be wrong on every run of that rule, and the one
         * message written to be expedited would sit at the band
         * `drop-oldest` sheds first.
         */
        if (!clawt_message_priority_from_nick(band, &priority, &refusal)) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT, refusal);
            return FALSE;
        }

        return clawt_mailbox_router_send_to_full(self->router, "user", target,
                                                 body, NULL, 0, priority,
                                                 error) >= 0;
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

    if (g_strcmp0(action, "start_agent") == 0 ||
        g_strcmp0(action, "stop_agent") == 0 ||
        g_strcmp0(action, "restart_agent") == 0) {
        /*
         * Checked here as well as in the callee, and named for the
         * action.
         *
         * These three used to hand the parameter straight through:
         * start_agent onto a g_return_val_if_fail() that printed a GLib
         * critical and returned FALSE with no #GError, and stop_agent
         * onto a lookup that simply answered FALSE. So a pod that forgot
         * the argument got "it did not work", which is what every other
         * failure here says too. Refusing at the action is what puts the
         * action's own name in the sentence.
         */
        if (agent == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "%s needs an agent", action);
            return FALSE;
        }

        if (g_strcmp0(action, "start_agent") == 0)
            return clawt_daemon_start_agent(self, agent, error);

        if (g_strcmp0(action, "stop_agent") == 0)
            return clawt_daemon_stop_agent(self, agent, TRUE);

        clawt_daemon_stop_agent(self, agent, TRUE);
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
            const gchar *summary = g_hash_table_lookup(params, "summary");
            const gchar *tags = g_hash_table_lookup(params, "tags");
            g_autofree gchar *importance = NULL;
            g_autofree gchar *refusal = NULL;
            g_autofree gchar *id = NULL;

            /*
             * Assigned, not set through g_object_set().
             *
             * #ClawtMemory is a G_DEFINE_BOXED_TYPE with public fields
             * and no properties at all, so g_object_set() was reading a
             * GTypeInstance out of a plain struct: it does not warn and
             * return, it takes the daemon down with SIGSEGV.  Any pod
             * that classified what it was recording killed the process
             * that was running it, and the fleet came back without the
             * memory, without the pod's remaining actions, and with
             * nothing in the log naming the line.
             *
             * Not judged against clawt_memory_categories(): that list is
             * a shared vocabulary and says of itself that it is not a
             * constraint, so refusing an unlisted one here would quietly
             * make it one.
             */
            if (category != NULL && category[0] != '\0') {
                g_free(memory->category);
                memory->category = g_strdup(category);
            }

            /*
             * The level *is* judged, by the same function
             * clawtilla_memory_add calls -- so a pod and an agent cannot
             * come to mean different things by "critical", and neither can
             * be the one that is wrong because it gets less use.
             *
             * Refused before anything is written rather than defaulted.
             * The column is plain text and the store binds what it is
             * handed, so a mistyped level is stored, sorts as nothing, and
             * is invisible from both ends -- and a pod runs unattended, so
             * it would be wrong on every run of that rule.  This is the
             * decision `priority` on message_agent above already takes.
             */
            if (!clawt_memory_importance_from_nick(
                    g_hash_table_lookup(params, "importance"),
                    &importance, &refusal)) {
                g_set_error_literal(error, CLAWT_ERROR,
                                    CLAWT_ERROR_INVALID_ARGUMENT, refusal);
                return FALSE;
            }

            if (importance != NULL) {
                g_free(memory->importance);
                memory->importance = g_steal_pointer(&importance);
            }

            if (summary != NULL && summary[0] != '\0')
                memory->summary = g_strdup(summary);

            if (tags != NULL && tags[0] != '\0')
                memory->tags = g_strdup(tags);

            /*
             * Stamped as the pod, not as the agent whose store it landed
             * in -- which is what clawtilla_memory_add records, because
             * there the agent really did write it.
             *
             * An automation files a memory out of an event payload, and
             * clawt_memory_provenance_rule() is the whole reason that
             * distinction is kept: a row with no source reads back in a
             * later session as something the agent worked out itself.
             * There is no pod name to give -- the action callback is
             * handed the action and its parameters and nothing about the
             * rule that fired -- so this says the category of writer it
             * can honestly say.
             */
            memory->source = g_strdup("pod");

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
 * Published for every state, and notified about only for a finished one
 * -- and that only because somebody asked, since `done` is off by
 * default and a fleet that works is a fleet finishing tasks all day.
 *
 * Deciding the notification used to be the whole of this function, so
 * `ClawtTaskManager::task-changed` reached the daemon and stopped there:
 * nothing put a `task.` kind on the bus at all. Following a task meant
 * polling `task.list`, and podomation's `on_task_changed` binding --
 * declared, documented and mapped to `task.changed` -- named an event
 * that could never once fire.
 */
/*
 * A finished task, distilled into the assignee's memories.
 *
 * Off by default -- `memories.summarise` -- because it is a model call
 * nobody asked for, billed to whoever turned it on.  Asynchronous
 * because this runs on the daemon's main context: a completion is an
 * HTTP round trip, and taken synchronously here the whole fleet would
 * stop answering for the length of it.
 *
 * The result and the prompt, rather than the room transcript: a task
 * runs in a session of its own and what it *concluded* is the part
 * worth remembering.  Reading the room would fold in every unrelated
 * message the operator sent while it ran.
 */
static void
on_summary_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtSummariser *summariser = CLAWT_SUMMARISER(source);
    g_autofree gchar *task_id = user_data;
    g_autoptr(GError) error = NULL;
    guint written;

    written = clawt_summariser_summarise_finish(summariser, result, &error);

    if (error != NULL) {
        /*
         * A warning and nothing else.  A summary that did not happen is
         * a fleet that remembers less, not a fleet that is broken, and
         * the task itself completed regardless.
         */
        g_warning("summarise %s: %s", task_id, error->message);
        return;
    }

    if (written > 0)
        g_message("summarise %s: %u memor%s recorded", task_id, written,
                  written == 1 ? "y" : "ies");
}

static void
summarise_finished_work(ClawtDaemon *self, ClawtTask *task)
{
    const gchar *assignee = clawt_task_get_assignee(task);
    ClawtAgentConfig *config;
    ClawtMemoryStore *store;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *source = NULL;
    g_autofree gchar *transcript = NULL;

    if (assignee == NULL)
        return;

    config = clawt_config_get_agent(self->config, assignee);

    if (config == NULL ||
        !clawt_agent_config_get_boolean(config, "memories.summarise"))
        return;

    /*
     * Written to wherever this agent's memories.scope says, so a
     * summary of team work lands where the team can read it.
     */
    store = clawt_agent_manager_memory_write_store(self->agents, assignee,
                                                   &error);

    if (store == NULL) {
        g_warning("summarise %s: %s", clawt_task_get_id(task),
                  error->message);
        return;
    }

    if (self->summariser == NULL) {
        self->summariser = clawt_summariser_new(self->config);
        clawt_summariser_set_main_context(self->summariser,
                                          self->main_context);

        if (!clawt_summariser_use_configured_provider(self->summariser,
                                                      &error)) {
            /*
             * Dropped rather than kept in a state where every summary
             * fails the same way: the provider is built from config, and
             * a config edit should be enough to make the next one work.
             */
            g_warning("summarise: %s", error->message);
            g_clear_object(&self->summariser);
            return;
        }
    }

    source = g_strdup_printf("task:%s", clawt_task_get_id(task));
    transcript = g_strdup_printf("Task: %s\n\nResult:\n%s",
                                 clawt_task_get_prompt(task) != NULL
                                 ? clawt_task_get_prompt(task) : "(none)",
                                 clawt_task_get_result(task) != NULL
                                 ? clawt_task_get_result(task) : "(none)");

    clawt_summariser_summarise_async(
        self->summariser, store, source, transcript,
        clawt_task_get_created_at(task),
        g_get_real_time() / G_USEC_PER_SEC, NULL, on_summary_finished,
        g_strdup(clawt_task_get_id(task)));
}

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

    task = clawt_task_manager_get(self->tasks, task_id);

    {
        g_autoptr(ClawtEvent) event = clawt_event_new("task.changed",
                                                      task_id);
        const gchar *nick = clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE, state);

        clawt_event_set_detail(event, "state",
                               nick != NULL ? nick : "unknown");

        /*
         * The assignee goes with it, and it is load-bearing rather than
         * decoration: the pod module resolves an event's agent from this
         * detail for any kind that is not `agent.*` or `message`, and
         * that is what a pod's scope is matched against. Without it
         * `Clawtilla.New("researcher")` would hear every task in the
         * fleet change state, which is precisely what naming an agent in
         * the constructor asks not to happen.
         */
        if (task != NULL)
            clawt_event_set_detail(event, "agent",
                                   clawt_task_get_assignee(task));

        clawt_event_bus_publish(self->bus, event);
    }

    if (state != CLAWT_TASK_COMPLETED)
        return;

    if (task == NULL)
        return;

    summarise_finished_work(self, task);

    if (self->notifier == NULL)
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

    /*
     * A turn is starting, which is the moment that decides how far the
     * message being answered had come. Every message this turn sends
     * counts from there.
     */
    if (agent != NULL && typing) {
        clawt_agent_begin_turn(agent);

        /*
         * And if that turn is a delegated task, it is now running.
         *
         * This is the only moment anybody actually knows: creating a
         * task says work was handed out, and delivering it says the
         * message reached a mailbox, but neither says the assignee
         * looked at it -- an agent that is stopped has a full mailbox
         * and does nothing.  clawtilla_delegate therefore left every
         * task it created reading `pending` from creation until it went
         * straight to `completed`, and a delegator reading that as
         * "nobody picked it up" delegates the work again.  The tool
         * output had to carry a paragraph apologising for the column.
         *
         * A no-op for a task already running, so the operator and
         * routine paths that start theirs at delivery are unaffected.
         */
        if (self->tasks != NULL)
            clawt_task_manager_start(self->tasks,
                                     clawt_agent_get_turn_task_id(agent));
    }

    /*
     * And the budgets that end a turn nothing else would.  Both edges of
     * the same frame: this is the only place the daemon is told a turn
     * began or ended, so it is the only place either can be started or
     * stopped from.
     */
    if (typing)
        clawt_daemon_turn_begin(self, agent_id, room_id);
    else
        clawt_daemon_turn_settle(self, agent_id);

    /*
     * And the moment that decides whether the last frame is worth
     * taking.  Only a turn that actually went near the screen settles
     * one -- a one-word reply must not end with a fresh picture of an
     * idle desktop, grabbed down the connection the agent works over.
     */
    if (agent != NULL && !typing && self->observer != NULL)
        clawt_observer_settle_turn(self->observer, agent_id);

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
     * A sign of life, so the activity watchdog does not stop a turn that
     * is talking.  An agent's own bash and read never reach the daemon,
     * so a message and a clawtilla tool call are the whole of what
     * "activity" can honestly mean from out here.
     */
    clawt_daemon_turn_activity(self, agent_id);

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
     * This is the reply, so it invites none of its own.
     *
     * Everything else that reaches a mailbox was written on purpose: an
     * agent calling clawtilla_message_agent, an operator typing. This
     * path is the one the AI CLI takes by simply finishing a turn, and
     * it takes it whether or not the agent had anything to add -- which
     * is why two of them acknowledging each other could not stop. A
     * deliberate message earns one answer and the answer earns none, so
     * an exchange settles at one round instead of running to max_hops.
     */
    clawt_message_set_invites_reply(message, FALSE);

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
         * And it is *not* cleared here.
         *
         * It was, on the reasoning that the reply is the last thing that
         * needs the number -- which is true of a turn that sends one
         * message and false of every other kind. A chief-of-staff
         * answers its operator and hands work to a peer in the same
         * turn; clearing on the first of those started the second at
         * depth 1, so two agents signing off at each other never reached
         * max_hops however long they kept it up. Six rounds of "nothing
         * further, ending turn" on a real fleet is what found it.
         *
         * clawt_agent_begin_turn() drops it instead, at the start of a
         * turn no delivery preceded -- which is the case the clearing
         * was protecting against.
         */
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
     *
     * The turn boundary is still not work completion, and the busy flag
     * only closed the mid-turn half of that.  An assignee that finishes
     * its share, hands the rest on and ends its turn with a status note
     * is not busy, so the task completed carrying text that said in so
     * many words that the report had not been sent yet.  The rest of
     * that rule is in clawt_task_manager_complete_on_turn_end().
     */
    if (thread_id != NULL) {
        ClawtAgent *replier = clawt_agent_manager_get(self->agents, agent_id);

        if (replier != NULL && clawt_agent_get_busy(replier)) {
            g_info("daemon: %s is still working, so this is not the answer "
                   "to %s", agent_id, thread_id);
        } else {
            g_autofree gchar *held = NULL;

            /*
             * Through the manager rather than straight to
             * clawt_task_manager_complete(), because the busy flag is
             * only one of three things that make this the wrong answer
             * and it is the only one the daemon can see.  The other two
             * are facts about the task -- the assignee asked for the
             * task to be held open, or it has work of its own still
             * running -- so they live where the task does, and there is
             * one rule instead of one per caller who noticed.
             */
            if (!clawt_task_manager_complete_on_turn_end(self->tasks,
                                                         thread_id, body,
                                                         &held) &&
                held != NULL)
                g_info("daemon: %s ended its turn but %s is still open: %s",
                       agent_id, thread_id, held);
        }
    }

    /*
     * And the turn that produced it may have had nowhere to send it.
     *
     * A turn started by another agent's reply is the end of that
     * exchange: the delivery preamble told the agent so, and said to use
     * clawtilla_message_agent for anything that genuinely has to reach
     * them. What it writes here is what an AI CLI writes at the end of
     * every turn whether or not it has anything to say, so routing it
     * would restart the exchange the preamble just closed -- and the
     * agent would be answering a message that was itself only an answer.
     *
     * Checked with a task id in hand, because a task delivery always
     * invites its result: suppressing one would leave the delegator
     * waiting on work that is finished. A turn that has both is a turn
     * the flag was never set on.
     *
     * And never for the operator's own room, whatever else is true. A
     * person waiting on an answer must not be met with silence because
     * of a rule about how agents talk among themselves.
     */
    if (thread_id == NULL && !is_operator_room(destination)) {
        ClawtAgent *replier = clawt_agent_manager_get(self->agents, agent_id);

        if (replier != NULL && !clawt_agent_get_turn_replies(replier)) {
            g_info("daemon: %s ended a closed exchange, so its reply to %s "
                   "was not sent", agent_id, destination);
            return;
        }
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
/*
 * A mount from an IPC payload, validated.
 *
 * One parser for the per-agent frames and the fleet defaults. Two would
 * differ exactly once, and the case they would differ on is the enum
 * spellings -- a client sending "read-only" instead of "ro" gets a
 * refusal naming what is accepted, and only from whichever copy was
 * kept up to date.
 *
 * Validated here rather than at the agent's next start: a mount that
 * could never work would otherwise be saved and then surface as a start
 * failure mentioning neither the path nor the reason.
 *
 * Returns: (transfer full) (nullable): the mount, or %NULL with @error
 */
/*
 * A string list on the current object, omitted when empty.
 *
 * An empty array and an absent key mean the same thing to every client
 * here, and writing `[]` into every mount that has no scope list is
 * noise in a reply somebody reads with `jq`.
 */
void
clawt_daemon_add_string_array(JsonBuilder *builder, const gchar *name,
                              const gchar * const *items)
{
    gsize i;

    if (items == NULL || items[0] == NULL)
        return;

    json_builder_set_member_name(builder, name);
    json_builder_begin_array(builder);

    for (i = 0; items[i] != NULL; i++)
        json_builder_add_string_value(builder, items[i]);

    json_builder_end_array(builder);
}

ClawtMount *
clawt_daemon_mount_from_payload(ClawtConfig  *config,
                                JsonObject   *payload,
                                const gchar  *target,
                                GError      **error)
{
    const gchar *source = clawt_ipc_payload_string(payload, "source");
    const gchar *mode = clawt_ipc_payload_string(payload, "mode");
    const gchar *type = clawt_ipc_payload_string(payload, "type");
    const gchar *relabel = clawt_ipc_payload_string(payload, "relabel");
    const gchar *size = clawt_ipc_payload_string(payload, "size");
    g_autoptr(ClawtMount) mount = clawt_mount_new(source, target);
    gint parsed = 0;

    if (mode != NULL) {
        if (!clawt_enum_from_nick(CLAWT_TYPE_MOUNT_MODE, mode, &parsed)) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "mode is ro or rw");
            return NULL;
        }

        clawt_mount_set_mode(mount, (ClawtMountMode)parsed);
    }

    if (type != NULL) {
        if (!clawt_enum_from_nick(CLAWT_TYPE_MOUNT_TYPE, type, &parsed)) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "type is bind, volume, virtiofs, 9p or "
                                "tmpfs");
            return NULL;
        }

        clawt_mount_set_mount_type(mount, (ClawtMountType)parsed);
    }

    /*
     * Absent means shared, matching what the YAML reader does with an
     * entry that omits it. On an SELinux system an unlabelled bind
     * mount is visible inside the container with every access denied,
     * so the two readers agreeing about this is what keeps a folder
     * added from a client working like one written by hand.
     */
    if (relabel != NULL) {
        if (!clawt_enum_from_nick(CLAWT_TYPE_RELABEL, relabel, &parsed)) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "relabel is none, shared or private");
            return NULL;
        }

        clawt_mount_set_relabel(mount, (ClawtRelabel)parsed);
    } else {
        clawt_mount_set_relabel(mount, CLAWT_RELABEL_SHARED);
    }

    if (size != NULL)
        clawt_mount_set_size(mount, size);

    /*
     * Who it is for. Only meaningful for a fleet default -- an agent's
     * own mount is already agent-scoped -- but parsed here because both
     * frames share this function, and a field silently dropped on one of
     * them is how the two would come to disagree.
     */
    {
        const gchar *scope = clawt_ipc_payload_string(payload, "scope");
        g_auto(GStrv) agents =
            clawt_ipc_payload_strv(payload, "agents");
        g_auto(GStrv) teams = clawt_ipc_payload_strv(payload, "teams");
        g_auto(GStrv) who = clawt_ipc_payload_strv(payload, "who");

        /*
         * One list of names, sorted here.
         *
         * The graphical clients offer a single field -- a name is an
         * agent or a team and the fleet already knows which, so asking
         * somebody to classify it is asking them to know something the
         * daemon does. The GTK client used to sort it itself by asking
         * `team.list`, which reports the teams somebody *declared*; an
         * agent can be on a team nobody declared, so every such name was
         * filed under `agents:` where it matched nothing. The folder
         * reached nobody and the warning about it named the control that
         * had caused it.
         */
        if (who != NULL && who[0] != NULL) {
            g_auto(GStrv) sorted_agents = NULL;
            g_auto(GStrv) sorted_teams = NULL;

            clawt_mount_sort_scope(config, (const gchar *const *)who,
                                   &sorted_agents, &sorted_teams);

            if (sorted_agents != NULL) {
                g_strfreev(agents);
                agents = g_steal_pointer(&sorted_agents);
            }

            if (sorted_teams != NULL) {
                g_strfreev(teams);
                teams = g_steal_pointer(&sorted_teams);
            }
        }

        if (scope != NULL) {
            if (!clawt_enum_from_nick(CLAWT_TYPE_SCOPE, scope, &parsed)) {
                g_set_error_literal(error, CLAWT_ERROR,
                                    CLAWT_ERROR_INVALID_ARGUMENT,
                                    "scope is all, selected or none");
                return NULL;
            }

            clawt_mount_set_scope(mount, (ClawtScope)parsed);
        }

        clawt_mount_set_agents(mount, (const gchar *const *)agents);
        clawt_mount_set_teams(mount, (const gchar *const *)teams);

        if (scope == NULL &&
            ((agents != NULL && agents[0] != NULL) ||
             (teams != NULL && teams[0] != NULL)))
            clawt_mount_set_scope(mount, CLAWT_SCOPE_SELECTED);
    }

    clawt_mount_set_create(
        mount, clawt_ipc_payload_boolean(payload, "create", FALSE));
    clawt_mount_set_required(
        mount, clawt_ipc_payload_boolean(payload, "required", TRUE));

    if (!clawt_mount_validate(mount, error))
        return NULL;

    return g_steal_pointer(&mount);
}

gboolean
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

gint
clawt_daemon_compare_by_order(gconstpointer a, gconstpointer b,
                              gpointer user_data)
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
ClawtAgentConfig *clawt_daemon_create_agent(ClawtDaemon  *self,
                                             const gchar  *agent_id,
                                             GHashTable   *fields,
                                             const gchar  *purpose,
                                             gboolean     *purpose_landed,
                                             GError      **error);

/*
 * An agent creating an agent, through the same door a person uses.
 *
 * It goes to clawt_daemon_create_agent() rather than reimplementing any of it,
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

    /*
     * And both turn budgets hold while the question is open.  Waiting on
     * a person is not a stall, and stopping a turn under an unanswered
     * question manufactures a stranded decision the daemon then has to
     * repair.
     */
    clawt_daemon_turn_hold(self, agent_id);

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

    if (clawt_daemon_create_agent(self, agent_id, settings, purpose,
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
                  ClawtPriority priority, gpointer user_data, GError **error)
{
    ClawtDaemon *self = user_data;

    if (clawt_mailbox_router_send_to_full(self->router, from_agent, target,
                                          body, task_id, depth, priority,
                                          error) < 0)
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
     * Every computer that can hold one gets the exchange unless the
     * agent turned it off, because the alternative is each pair of
     * agents needing a hand-wired mount before they can pass a file.
     *
     * An ssh computer cannot hold one. The exchange is a directory under
     * the daemon's state directory and there is no mount to make it
     * appear on another machine, so adding it would tell an agent about
     * /mnt/clawtilla/exchange over there and leave it looking for files
     * its peers had "handed" it.
     */
    if (self->exchange != NULL &&
        clawt_computer_type_shares_host_paths(
            clawt_computer_get_computer_type(computer)) &&
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
/*
 * Rebuild the skill library from whatever the config now says.
 *
 * Called from start and from reload rather than from a lazy getter,
 * because the library holds a #GFileMonitor: a getter would attach one
 * on whichever context happened to call it first, and for an embedded
 * daemon that is the difference between a watch that works and one that
 * never fires.  Here it is always the daemon's own.
 */
void
clawt_daemon_reload_skills(ClawtDaemon *self)
{
    g_autofree gchar *directory = NULL;

    g_clear_object(&self->skills);

    /*
     * Cleared first, on every path.  Three of the four ways out of this
     * function leave no library at all, and the tools holding a pointer
     * to the one that was just unreffed is a use-after-free reached by
     * an ordinary tool call.
     */
    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_skill_library(self->mcp_tools, NULL);

    if (self->config == NULL)
        return;

    /*
     * Off means off, not empty.  A fleet that has turned skills off
     * should pay nothing -- no scan, no watch, no links written into a
     * workspace -- and every reader treats a NULL library as "no
     * skills" rather than as a failure.
     */
    if (!clawt_config_get_boolean(self->config, "skills.enabled"))
        return;

    directory = clawt_config_get_path_value(self->config, "skills.dir");

    if (directory == NULL || *directory == '\0')
        return;

    self->skills = clawt_skill_library_new(directory);
    clawt_skill_library_scan(self->skills);

    /*
     * Watching is what makes editing a SKILL.md take effect without a
     * reload.  The monitor and its debounce are attached to whatever is
     * thread-default here, which is the daemon's context: start() and
     * reload() both run on it.
     */
    clawt_skill_library_set_watching(self->skills, TRUE);

    {
        GPtrArray *problems = clawt_skill_library_get_problems(self->skills);
        guint i;

        for (i = 0; i < problems->len; i++)
            g_warning("skills: %s", (const gchar *)
                      g_ptr_array_index(problems, i));
    }

    /*
     * The agent-facing tools read the same library, so they are handed
     * the new one here.  The old pointer is dangling the moment
     * g_clear_object() above runs, and an agent calling
     * clawtilla_skill_list between a reload and a restart is entirely
     * ordinary.
     */
    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_skill_library(self->mcp_tools, self->skills);
}

void
clawt_daemon_render_all_agents_into(ClawtDaemon *self, GPtrArray *refusals)
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

        /*
         * ...and the skills, linked where this agent's own CLI looks.
         *
         * Here rather than in the renderer for the same reason the tool
         * list is: the renderer writes a config file, and this writes
         * into a workspace against a library only the daemon holds.
         * Both halves go together -- a link with no paragraph in
         * TOOLS.org is a procedure the agent has and does not know it
         * has, and this project has already paid for that once with a
         * tool table written at scaffold time and never again.
         */
        if (self->skills != NULL) {
            g_autoptr(GPtrArray) skill_warnings = NULL;
            g_autoptr(GPtrArray) bindings = NULL;
            g_autoptr(GError) skill_error = NULL;
            guint w;

            if (!clawt_skill_provision(self->config, config, self->skills,
                                       &skill_warnings, &skill_error))
                g_warning("agent %s: %s", clawt_agent_get_id(agent),
                          skill_error->message);

            for (w = 0; skill_warnings != NULL && w < skill_warnings->len;
                 w++)
                g_warning("skills: %s", (const gchar *)
                          g_ptr_array_index(skill_warnings, w));

            bindings = clawt_skill_resolve_for_agent(self->config, config,
                                                     self->skills);

            {
                g_autofree gchar *described =
                    clawt_skill_provision_describe(bindings);
                g_autoptr(GError) region_error = NULL;

                if (!clawt_workspace_update_skills(config, described,
                                                   &region_error))
                    g_warning("agent %s: %s", clawt_agent_get_id(agent),
                              region_error->message);
            }
        }

        /*
         * ...and what the fleet knows about the person it works for.
         *
         * Here rather than at agent creation, so an agent made on day
         * one and an agent made on day ninety are told the same thing.
         * `memories.operator_profile` off writes NULL, which removes the
         * region: a setting somebody turned off must leave nothing
         * behind in a prompt that is already written.
         */
        if (self->operator_profile != NULL) {
            g_autofree gchar *profile = NULL;
            g_autoptr(GError) profile_error = NULL;

            if (clawt_config_get_boolean(self->config,
                                         "memories.operator_profile"))
                profile = clawt_operator_profile_render(
                    self->operator_profile, 0);

            if (!clawt_workspace_update_operator_profile(config, profile,
                                                         &profile_error))
                g_warning("agent %s: %s", clawt_agent_get_id(agent),
                          profile_error->message);
        }
    }
}

/*
 * There is deliberately no render_all_agents() convenience taking no
 * refusal array.  Every caller in the tree passes one now, and a wrapper
 * that discards them is exactly how six handlers came to report success
 * about an agent left running on its previous config.
 */
GPtrArray *
clawt_daemon_render_refusals_new(void)
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
void
clawt_daemon_add_render_refusals(JsonBuilder *builder, GPtrArray *refusals)
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


/*
 * Everything an agent needs doing before its computer is started.
 *
 * Split out because starting a computer is the one step in here that
 * waits on something else's socket -- a podman API request per container
 * agent, a libvirt round trip per VM -- and the daemon's main loop must
 * not be inside it.  Held together as one function, an autostarting
 * fleet meant the loop dispatched nothing until every agent was up: no
 * IPC frame answered, and no signal source run, so the daemon could
 * neither be talked to nor asked to stop.
 *
 * The computer that still has to be started comes back in @pending, or
 * %NULL when the agent already had one and there is nothing to wait on.
 */
static gboolean
start_agent_prepare(ClawtDaemon    *self,
                    const gchar    *agent_id,
                    gchar         **config_path_out,
                    ClawtComputer **pending,
                    GError        **error)
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

    /*
     * How large the persona has become, said out loud before anything
     * tries to spawn with it.
     *
     * An agent whose identity files exceed a single execve argument
     * cannot start a *fresh* session on a backend that passes the system
     * prompt as one, and the kernel's own refusal -- "Argument list too
     * long" -- names neither the files, the size, nor the limit.  It is
     * silent right up to the cliff, and the scaffolding encourages the
     * growth: the generated AGENTS.org tells the agent to keep
     * PROJECTS.org current, and PROJECTS.org is an identity file.
     *
     * Said and not refused.  ai-glib spills the prompt to a temporary
     * file for claude-code, so this is not fatal there and refusing would
     * stop an agent that works; the diagnosis for the backends that still
     * build an argument belongs where that argument is built.  What
     * clawtilla can say -- and nothing below it can -- is *which* file
     * accounts for it.
     *
     * Measured after the files are written, so the managed region in
     * TOOLS.org is the one this start will actually use.
     */
    {
        g_autoptr(ClawtIdentitySize) size =
            clawt_workspace_measure_identity(config);
        g_autofree gchar *verdict = clawt_workspace_identity_verdict(size);

        if (verdict != NULL) {
            ClawtEvent *event = clawt_event_new("agent.identity", agent_id);

            g_warning("%s: %s", agent_id, verdict);

            clawt_event_set_detail(event, "verdict", verdict);
            clawt_event_set_detail_int(event, "bytes", (gint64)size->total);
            clawt_event_set_detail_int(event, "limit", (gint64)size->limit);
            clawt_event_set_detail(event, "over",
                                   (size->total >= size->limit) ? "true"
                                                                : "false");
            clawt_event_bus_publish(self->bus, event);
            clawt_event_free(event);
        }
    }

    /* The computer first: an agent that starts before its computer is
     * ready spends its first turns discovering it cannot reach it. */
    if (clawt_agent_get_computer(agent) == NULL) {
        g_autoptr(ClawtComputer) computer = NULL;
        g_autoptr(GPtrArray) defaults =
            clawt_config_get_default_mounts(self->config);

        computer = clawt_computer_factory_create(config, defaults,
                                                 self->pod_bridge, &local);

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

        /*
         * And write what that computer turned out to be into the
         * agent's own TOOLS.org, before the child is spawned to read it.
         *
         * Here rather than in clawt_daemon_render_all_agents_into(),
         * which also runs for a *stopped* agent -- there the computer is
         * NULL, so that version filled the region on the first start and
         * emptied it on the next restart. Worse than not writing it at
         * all: an agent that had been told about a shared folder stopped
         * being told.
         *
         * From the built computer rather than from the config, because
         * the config does not know what the fleet shared: a default
         * mount reaches every agent without any agent block mentioning
         * it. Without this an agent had a directory it was never told
         * about, findable only by calling a tool it had no reason to
         * call -- which is the "an agent believes its own file" failure
         * this tree has recorded twice.
         */
        if (clawt_computer_get_computer_type(computer) !=
            CLAWT_COMPUTER_NONE) {
            g_autofree gchar *described =
                clawt_agent_describe_computer(agent);
            g_autoptr(GError) computer_error = NULL;

            if (described != NULL &&
                !clawt_workspace_update_computer(config, described,
                                                 &computer_error))
                g_warning("agent %s: %s", agent_id, computer_error->message);
        }

        /*
         * Nothing at all for an agent with no computer.
         *
         * A null computer is still a computer *object*, so this wrote a
         * region saying "You have no computer" wrapped in prose about
         * which side of an `=` is the host -- a distinction with no
         * paths to apply it to, appended to the end of the file because
         * that scaffold path carries no markers. The scaffolded section
         * already says it, in words chosen for it, and says it better.
         */

        *pending = g_object_ref(computer);
    }

    /*
     * An agent that already has a computer hands that one back.
     *
     * This assignment used to live only inside the branch above, which
     * builds a computer for an agent that has none -- so from an agent's
     * first start until the daemon exited, clawt_computer_start() was
     * unreachable for it.  Destroy its container behind the daemon's back
     * and neither `agent start` nor `agent restart` could bring it back:
     * both reported success having asked nothing, because relaunching the
     * child genuinely does succeed.  The libreclaw process runs on the
     * host and does not need the container to exist, so the one thing
     * that was broken was the one thing nobody checked.
     *
     * Starting a computer that is already up is a no-op at every backend
     * -- podman's start on a running container, the host's sandbox check
     * -- and that is the behaviour worth having: `agent start` is what an
     * operator reaches for to recover, and refusing it because the daemon
     * believes the computer is fine would refuse it in exactly the case
     * where the daemon's belief is the thing that is wrong.
     */
    if (*pending == NULL) {
        ClawtComputer *existing = clawt_agent_get_computer(agent);

        if (existing != NULL)
            *pending = g_object_ref(existing);
    }

    *config_path_out = g_steal_pointer(&config_path);

    return TRUE;
}

/*
 * A computer that would not start is an ERROR agent, not a failed daemon.
 *
 * Its own function because both the synchronous and the asynchronous
 * start have to do exactly this with the result, and two copies of "what
 * a refusal means" is how one of them ends up reporting a fleet-level
 * failure for a container whose image was not pulled.
 */
static gboolean
start_agent_computer_failed(ClawtDaemon *self, const gchar *agent_id,
                            GError *started_error, GError **error)
{
    ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);

    /*
     * ERROR, not SHADOW.  A podman that is not running, an image that is
     * not pulled, a name still held by yesterday's container -- these are
     * all transient, and SHADOW refuses every later start with the
     * message frozen from the first one, so a fixed problem still looked
     * broken until the daemon was restarted.
     */
    if (agent != NULL)
        clawt_agent_set_error(agent, started_error->message);

    g_propagate_error(error, started_error);

    return FALSE;
}

/*
 * And everything after the computer is up.
 *
 * Takes the id rather than the agent: between preparing an agent and
 * launching it the main loop has run, which is the whole point, and a
 * client is free to have removed it in the meantime.
 */
static gboolean
start_agent_launch(ClawtDaemon *self, const gchar *agent_id,
                   const gchar *config_path, GError **error)
{
    ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);
    ClawtAgentConfig *config;

    if (agent == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no agent called '%s'", agent_id);
        return FALSE;
    }

    config = clawt_agent_get_config(agent);

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
clawt_daemon_start_agent(ClawtDaemon *self, const gchar *agent_id,
                         GError **error)
{
    g_autofree gchar *config_path = NULL;
    g_autoptr(ClawtComputer) pending = NULL;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    /*
     * An error rather than g_return_val_if_fail(), which is what this
     * was.
     *
     * The id here does not come from code: it comes from an IPC payload,
     * from an agent's config, or from a pod file somebody wrote. A
     * critical is the right answer to a programmer's mistake and the
     * wrong one to a missing argument -- it printed a stack trace into
     * the daemon's log and returned FALSE with @error untouched, so
     * every caller had a failure with nothing in it to report. The pod
     * action path was where that showed: `clawtilla->start_agent()` with
     * no agent warned "it did not work" above a GLib critical about an
     * assertion nobody had written.
     */
    if (agent_id == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "starting an agent needs its id");
        return FALSE;
    }

    if (!start_agent_prepare(self, agent_id, &config_path, &pending, error))
        return FALSE;

    /*
     * Still blocking, deliberately.  Every caller of this one has
     * somebody waiting on the answer -- an `agent start` frame, a test,
     * a restart -- and for a single agent the wait is bounded by
     * podomation's socket timeout.  It is the *fleet* coming up that
     * must not hold the loop, and that goes through the async form
     * below.
     */
    if (pending != NULL) {
        g_autoptr(GError) local = NULL;

        if (!clawt_computer_start(pending, &local))
            return start_agent_computer_failed(self, agent_id,
                                               g_steal_pointer(&local),
                                               error);
    }

    return start_agent_launch(self, agent_id, config_path, error);
}

typedef struct {
    gchar         *agent_id;
    gchar         *config_path;
    ClawtComputer *computer;   /* NULL when there is nothing to wait on */
    gboolean       prepared;
} StartAgentJob;

static void
start_agent_job_free(gpointer data)
{
    StartAgentJob *job = data;

    g_free(job->agent_id);
    g_free(job->config_path);
    g_clear_object(&job->computer);
    g_free(job);
}

/*
 * The one step that waits on somebody else's socket, on somebody else's
 * thread.
 *
 * Only the computer is touched here.  Everything that reads or writes
 * daemon state -- the agent manager, the config, the workspace files --
 * stays on the main thread, in prepare before this and in launch after
 * it, so this adds a thread without adding shared state to reason about.
 */
static void
start_agent_worker(GTask *task, gpointer source, gpointer data,
                   GCancellable *cancellable)
{
    StartAgentJob *job = data;
    GError *local = NULL;

    (void)source;
    (void)cancellable;

    if (clawt_computer_start(job->computer, &local))
        g_task_return_boolean(task, TRUE);
    else
        g_task_return_error(task, local);
}

/*
 * Back on the main thread, whichever way the computer went.
 */
static void
on_agent_computer_started(GObject *source, GAsyncResult *result,
                          gpointer user_data)
{
    ClawtDaemon *self = CLAWT_DAEMON(source);
    StartAgentJob *job = g_task_get_task_data(G_TASK(result));
    GSourceFunc done = (GSourceFunc)user_data;
    g_autoptr(GError) local = NULL;

    if (!g_task_propagate_boolean(G_TASK(result), &local)) {
        /*
         * Reported here and not raised: the caller is the fleet coming
         * up, and one agent whose image is not pulled must not stop the
         * rest.  Preparation failures have already said their piece.
         */
        if (job->prepared) {
            g_autoptr(GError) reported = NULL;

            start_agent_computer_failed(self, job->agent_id,
                                        g_steal_pointer(&local), &reported);
            g_warning("agent %s did not start: %s", job->agent_id,
                      reported->message);
        }
    } else if (job->prepared) {
        g_autoptr(GError) launch_error = NULL;

        if (!start_agent_launch(self, job->agent_id, job->config_path,
                                &launch_error))
            g_warning("agent %s did not start: %s", job->agent_id,
                      launch_error->message);
    }

    if (done != NULL)
        done(self);
}

/*
 * Start an agent without holding the main loop while its computer comes
 * up.
 *
 * @done runs on the main context once the agent is up or has failed,
 * which is what lets a caller bring a fleet up one at a time without
 * ever being inside a blocking read.  Failures are reported rather than
 * returned, for the same reason.
 *
 * The task is created on whichever context this was dispatched from --
 * for the autostart idle, the daemon's own -- so the completion lands
 * back on the loop that scheduled it rather than on the process default.
 * Three real bugs in this tree came from the other assumption.
 */
static void
daemon_start_agent_async(ClawtDaemon *self, const gchar *agent_id,
                         GSourceFunc done)
{
    g_autoptr(GTask) task = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *config_path = NULL;
    g_autoptr(ClawtComputer) pending = NULL;
    StartAgentJob *job = g_new0(StartAgentJob, 1);

    /*
     * g_task_new() captures g_main_context_ref_thread_default(), and
     * dispatching a source does *not* make its context thread-default --
     * GLib pushes nothing on the way in.  So a task created from inside
     * the autostart idle takes the process default and completes on a
     * loop the daemon may never run, which for an embedded daemon is
     * every time.  The whole fleet then sat queued and nothing started.
     *
     * The same trap as the timers and the idle already recorded in this
     * tree, one API along: name the context rather than assuming the
     * ambient one.
     */
    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);

    task = g_task_new(self, NULL, on_agent_computer_started, done);

    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);

    job->agent_id = g_strdup(agent_id);
    g_task_set_task_data(task, job, start_agent_job_free);

    if (!start_agent_prepare(self, agent_id, &config_path, &pending,
                             &error)) {
        g_warning("agent %s did not start: %s", agent_id, error->message);
        g_task_return_boolean(task, TRUE);
        return;
    }

    job->prepared = TRUE;
    job->config_path = g_steal_pointer(&config_path);
    job->computer = (pending != NULL) ? g_object_ref(pending) : NULL;

    /*
     * Nothing to wait on, so nothing to put on a thread.  The completion
     * is still deferred -- g_task_return_boolean() from this thread
     * finishes in an idle -- because the caller drives one agent per turn
     * and a synchronous answer would run the whole fleet inside one
     * callback, which is the outage this exists to end.
     */
    if (job->computer == NULL) {
        g_task_return_boolean(task, TRUE);
        return;
    }

    g_task_run_in_thread(task, start_agent_worker);
}

gboolean
clawt_daemon_interrupt_agent(ClawtDaemon  *self,
                             const gchar  *agent_id,
                             guint        *out_killed,
                             GError      **error)
{
    ClawtAgent *agent;
    ClawtAgentRuntime *runtime;
    g_autoptr(ClawtEvent) event = NULL;
    guint killed = 0;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    if (out_killed != NULL)
        *out_killed = 0;

    agent = clawt_agent_manager_get(self->agents, agent_id);

    if (agent == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no agent called '%s'",
                    agent_id != NULL ? agent_id : "");
        return FALSE;
    }

    runtime = clawt_agent_get_runtime(agent);

    if (runtime == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_AGENT_STATE,
                    "%s is not running, so it has nothing in flight to stop",
                    agent_id);
        return FALSE;
    }

    if (!clawt_agent_runtime_interrupt(runtime, &killed, error))
        return FALSE;

    if (out_killed != NULL)
        *out_killed = killed;

    /*
     * Marked idle here rather than waiting for libreclaw to lower its
     * typing indicator.
     *
     * Killing a turn mid-flight is the one case where that indicator may
     * never arrive -- the code that lowers it runs when the turn
     * finishes, and the turn was just taken out from under it. An agent
     * that shows as working for ever after somebody pressed stop is
     * precisely the state the button exists to get out of, so the daemon
     * says so itself. A later typing frame from libreclaw is idempotent.
     */
    clawt_agent_set_activity(agent, FALSE, NULL);

    /*
     * Handoffs first, and dropped rather than run.  A turn somebody
     * stopped did not finish deciding, so carrying out the transfers it
     * had queued would be acting on half a decision -- which is the
     * opposite of what pressing stop means.  It has to happen *before*
     * the settle, because the settle is what would otherwise run them.
     *
     * Deliberately unlike the steer queue, which survives an interrupt:
     * a steer is what somebody typed *instead*, and a handoff is part of
     * what was stopped.
     */
    clawt_daemon_handoff_drop_queued(
        self, agent_id,
        "the turn that asked for it was interrupted before it finished");

    /*
     * And everything that ends with a turn.  A steer typed while the
     * agent was working is drained here on purpose: pressing stop is how
     * somebody says "not that, this", and dropping the "this" would
     * leave them having only cancelled.
     */
    clawt_daemon_turn_settle(self, agent_id);

    event = clawt_event_new("agent.interrupted", agent_id);
    clawt_event_set_detail_int(event, "killed", (gint64)killed);
    clawt_event_bus_publish(self->bus, event);

    return TRUE;
}

gboolean
clawt_daemon_stop_agent(ClawtDaemon *self, const gchar *agent_id,
                        gboolean stop_machine)
{
    ClawtAgent *agent;
    ClawtComputer *computer;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), FALSE);

    agent = clawt_agent_manager_get(self->agents, agent_id);

    if (agent == NULL ||
        clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_STOPPED)
        return FALSE;

    /*
     * Read before the agent is stopped, though it survives that -- the
     * computer is only dropped when the agent object is finalised. Read
     * here anyway, so the order this function does things in is not a
     * thing the next reader has to know.
     */
    computer = clawt_agent_get_computer(agent);

    clawt_agent_stop(agent);

    /*
     * And its machine, when the caller meant the whole thing.
     *
     * `computer.container.keep` said "keep the container when the agent
     * stops, instead of removing it" for as long as it has existed, and
     * nothing stopped the computer when an agent stopped -- so the
     * setting described a moment that never happened, and every
     * container an agent had ever used went on running under a stopped
     * agent until somebody found it in `podman ps`.
     */
    if (stop_machine && computer != NULL) {
        g_autoptr(GError) local = NULL;

        if (!clawt_computer_stop(computer, &local))
            g_warning("agent %s: its machine did not stop: %s", agent_id,
                      local != NULL ? local->message : "no reason given");
    }
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
gboolean
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

static void autostart_cancel(ClawtDaemon *self);
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

    autostart_cancel(self);

    /*
     * Recordings before anything else.
     *
     * A demonstration that is still running holds a token in a
     * compositor and has a red frame on somebody's screen; releasing
     * the daemon's other components first would leave the observer it
     * subscribes to gone while the recorder still holds a reference to
     * it.
     */
    clawt_daemon_teach_teardown(self);

    /*
     * The listener before anything it can reach.  A delivery dispatched
     * after the router and the agents are gone would find a half-built
     * daemon, and the ingress holds a pointer to this one.
     */
    clawt_daemon_triggers_stop(self);
    clawt_daemon_venture_stop(self);

    g_clear_object(&self->plugins);

    /*
     * Emptied, not freed: a stopped daemon can be started again, and a
     * NULL table would crash the first design after it.  It has to
     * happen before the config goes, because every pending designer
     * holds a reference to it.
     */
    if (self->drafts != NULL)
        g_hash_table_remove_all(self->drafts);

    /*
     * Idempotent, and repeated here because a daemon that was never
     * started -- a construction that failed, or a test that built one and
     * dropped it -- never reaches clawt_daemon_stop().
     */
    clawt_daemon_turn_teardown(self);
    clawt_daemon_handoff_teardown(self);

    /*
     * The observer first, and before the context it attached its timers
     * to can go: a timer that outlives its main context is a source
     * dispatched against freed memory, and the symptom would be a crash
     * in the loop rather than anywhere near here.
     */
    if (self->observer != NULL)
        clawt_observer_stop_all(self->observer);

    g_clear_object(&self->observer);
    g_clear_object(&self->takeover);

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
    g_clear_object(&self->operator_profile);
    g_clear_object(&self->summariser);
    g_clear_object(&self->transcripts);
    g_clear_object(&self->log);
    g_clear_object(&self->bus);
    g_clear_object(&self->config);

    g_clear_pointer(&self->state_dir, g_free);
    g_clear_pointer(&self->link_socket, g_free);
    g_clear_pointer(&self->attachment_dir, g_free);
}

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
void
clawt_daemon_warm_model_cache(ClawtDaemon *self)
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

/*
 * Shared by the periodic path below and by `connector.registry_refresh`
 * in daemon-connector.c: whichever one finished must drop the cached
 * catalogue and clear the in-flight flag, or the other could never run
 * again and a freshly imported entry would wait for a reload to appear.
 */
void
clawt_daemon_registry_refresh_landed(ClawtDaemon *self)
{
    self->registry_refreshing = FALSE;
    g_clear_pointer(&self->connector_catalog, g_ptr_array_unref);
}

static void
on_registry_swept(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(GError) error = NULL;
    guint imported = 0;

    if (!clawt_connector_registry_refresh_finish(result, &imported, &error)) {
        clawt_daemon_registry_refresh_landed(self);
        g_warning("connector registry: periodic refresh failed: %s",
                  error->message);
        return;
    }

    clawt_daemon_registry_refresh_landed(self);
    g_message("connector registry: refreshed, %u entr%s cached", imported,
              (imported == 1) ? "y" : "ies");
}

/*
 * Refreshes the imported registry when it is due, never sooner.
 *
 * The cache's own `fetched_at` is the one clock consulted -- not a
 * daemon-side timestamp -- because a daemon that restarted an hour ago
 * must not treat that restart as a fresh refresh and wait a whole
 * `registry_refresh_hours` again for no reason.
 */
static void
sweep_connector_registry(ClawtDaemon *self)
{
    gint64 fetched_at = 0;
    gint64 refresh_hours;
    g_autofree gchar *cache_path = NULL;
    g_autoptr(GPtrArray) probe = NULL;

    if (self->registry_refreshing)
        return;

    if (!clawt_config_get_boolean(self->config, "connectors.registry_enabled"))
        return;

    cache_path = clawt_connector_registry_cache_path(self->state_dir);
    probe = clawt_connector_registry_cache_load(cache_path, &fetched_at);

    refresh_hours = clawt_config_get_int(self->config,
                                         "connectors.registry_refresh_hours");

    if (refresh_hours <= 0)
        refresh_hours = 24;

    if (fetched_at != 0 &&
        g_get_real_time() / G_USEC_PER_SEC - fetched_at <
            refresh_hours * 3600)
        return;

    self->registry_refreshing = TRUE;
    clawt_connector_registry_refresh_async(
        clawt_config_get_string(self->config, "connectors.registry_url"),
        cache_path, NULL, on_registry_swept, self);
}

/*
 * `connector.registry_refresh`'s own completion -- the explicit, waited
 * for path, as distinct from the periodic one above.  Both call
 * clawt_daemon_registry_refresh_landed() so a person pressing the button
 * and the sweep timer firing a moment later cannot leave the fleet
 * believing an import is still running when it is not.
 */
void
clawt_daemon_on_registry_refresh_requested(GObject *source, GAsyncResult *result,
                                           gpointer user_data)
{
    RegistryRefreshJob *job = user_data;
    g_autoptr(GError) error = NULL;
    guint imported = 0;
    gboolean ok;

    ok = clawt_connector_registry_refresh_finish(result, &imported, &error);
    clawt_daemon_registry_refresh_landed(job->daemon);

    if (job->pending != NULL) {
        if (!ok) {
            clawt_ipc_pending_respond(
                job->pending,
                clawt_ipc_error_new(
                    clawt_ipc_pending_get_request(job->pending),
                    CLAWT_ERROR_FAILED, error->message));
        } else {
            g_autoptr(JsonBuilder) builder = json_builder_new();

            json_builder_begin_object(builder);
            json_builder_set_member_name(builder, "imported");
            json_builder_add_int_value(builder, imported);
            json_builder_end_object(builder);

            clawt_ipc_pending_respond(
                job->pending,
                clawt_ipc_response_new(
                    clawt_ipc_pending_get_request(job->pending),
                    json_builder_get_root(builder)));
        }
    }

    g_free(job);
}

void
clawt_daemon_sweep(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->router == NULL)
        return;

    clawt_mailbox_router_sweep(self->router);
    clawt_event_log_sweep(self->log);
    sweep_connector_registry(self);

    /*
     * And the exchange, which until now had no periodic writer at all.
     *
     * clawt_exchange_prepare() applies the cap, which covers an agent
     * starting and a file put through the daemon -- but not the case the
     * exchange exists for: a file written through the mount from *inside*
     * a computer, by an agent's own shell, which the daemon never sees.
     * A long-running fleet doing that grew past defaults.exchange_max_bytes
     * and stayed there until somebody happened to restart an agent.
     *
     * 0 for the age, so this is the size cap and nothing else. Deleting a
     * file for being old is a policy nobody has asked for and there is no
     * key to ask for it with; deleting one to stay under a limit somebody
     * set is the limit doing its job.
     *
     * Skipped outright when the cap is off, because the sweep walks the
     * whole exchange to find out it has nothing to do -- and "0 disables
     * the limit" should mean the daemon stops looking, not that it looks
     * and always answers no.
     */
    if (self->exchange != NULL &&
        clawt_config_get_int(self->config, "defaults.exchange_max_bytes") > 0) {
        guint removed = clawt_exchange_sweep(self->exchange, 0);

        /*
         * Said out loud, as clawt_exchange_prepare() already does on its
         * own path. Files disappearing from a shared directory on a timer
         * nobody triggered is how a cap gets reported as data loss.
         */
        if (removed > 0)
            g_message("exchange: removed %u file%s on the periodic sweep to "
                      "stay under defaults.exchange_max_bytes",
                      removed, (removed == 1) ? "" : "s");
    }
}

static gboolean
on_sweep(gpointer user_data)
{
    clawt_daemon_sweep(user_data);

    return G_SOURCE_CONTINUE;
}

/* ── Connectors ──────────────────────────────────────────────────── */

/* Saves repeating the two-line dance for every optional string field. */
void
clawt_daemon_add_string_member(JsonBuilder *builder, const gchar *name,
                               const gchar *value)
{
    json_builder_set_member_name(builder, name);
    json_builder_add_string_value(builder, value);
}

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
GPtrArray *
clawt_daemon_catalog(ClawtDaemon *self)
{
    if (self->connector_catalog == NULL) {
        g_autofree gchar *dir =
            clawt_config_get_path_value(self->config, "connectors.dir");

        self->connector_catalog = clawt_connector_catalog_load(dir, NULL);

        /*
         * A pure local file read -- never the network -- so this is
         * safe on the same lazy path the overlay directory already
         * uses.  Gated on the flag as well as on there being a cache: a
         * fleet that has since turned the registry off should not keep
         * showing entries from when it was on, and clawt_daemon_reload()
         * already drops this cache whenever connectors.* changes.
         */
        if (clawt_config_get_boolean(self->config,
                                     "connectors.registry_enabled")) {
            g_autofree gchar *cache_path =
                clawt_connector_registry_cache_path(self->state_dir);
            g_autoptr(GPtrArray) imported =
                clawt_connector_registry_cache_load(cache_path, NULL);

            clawt_connector_catalog_merge_registry(self->connector_catalog,
                                                   imported);
        }
    }

    return self->connector_catalog;
}

/*
 * The integration instance and its catalogue entry together, which is
 * what every connector operation needs and neither half is any use
 * without.
 */
ClawtIntegrationBinding *
clawt_daemon_connector_binding(ClawtDaemon               *self,
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
    connector = clawt_connector_catalog_find(clawt_daemon_catalog(self),
                                             provider);

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
gchar *
clawt_daemon_connector_client_secret(ClawtDaemon *self,
                                     ClawtIntegrationBinding *binding)
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
gboolean
clawt_daemon_store_connector_token(ClawtDaemon      *self,
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

    if (!clawt_daemon_store_connector_token(flow->daemon, flow->name, token,
                                            &error)) {
        connector_flow_settle(flow, FALSE, error->message);
        return;
    }

    clawt_event_bus_emit(flow->daemon->bus, "integration.changed", flow->name);
    connector_flow_settle(flow, TRUE, NULL);
}

/*
 * Deletes the credential and forgets where it was.
 *
 * Both halves, and in that order: a config still naming a token_file
 * that is gone reads as connected right up until something tries to use
 * it.
 */
gboolean
clawt_daemon_forget_connector_token(ClawtDaemon *self, const gchar *name,
                                    GError **error)
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

void
clawt_daemon_on_connector_revoked(GObject *source, GAsyncResult *result,
                                  gpointer user_data)
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
        clawt_daemon_add_string_member(builder, "note", error->message);

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

void
clawt_daemon_on_connector_redirected(GObject *source, GAsyncResult *result,
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
void
clawt_daemon_on_connector_begun(GObject *source, GAsyncResult *result,
                                gpointer user_data)
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
    clawt_daemon_add_string_member(builder, "flow", flow->id);
    clawt_daemon_add_string_member(builder, "method", "device");
    clawt_daemon_add_string_member(builder, "user_code", code->user_code);
    clawt_daemon_add_string_member(builder, "verification_uri",
                                   code->verification_uri);
    clawt_daemon_add_string_member(builder, "verification_uri_complete",
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
void
clawt_daemon_sweep_connector_flows(ClawtDaemon *self)
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

void
clawt_daemon_refresh_job_free(RefreshJob *job)
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

void
clawt_daemon_on_connector_refreshed(GObject *source, GAsyncResult *result,
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
        clawt_daemon_refresh_job_free(job);
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

    if (!clawt_daemon_store_connector_token(job->daemon, job->name, token,
                                            &error)) {
        g_warning("could not store the renewed credential for '%s': %s",
                  job->name, error->message);
        refresh_job_answer(job, FALSE, error->message);
    } else {
        g_debug("renewed the credential for connector '%s'", job->name);
        clawt_event_bus_emit(job->daemon->bus, "integration.changed",
                             job->name);
        refresh_job_answer(job, TRUE, NULL);
    }

    clawt_daemon_refresh_job_free(job);
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

    binding = clawt_daemon_connector_binding(self, name, &connector, NULL);

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
        g_autofree gchar *secret =
            clawt_daemon_connector_client_secret(self, binding);

        clawt_oauth_refresh_async(token_url, client_id, secret,
                                  token->refresh_token, NULL,
                                  clawt_daemon_on_connector_refreshed, job);
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
ClawtAgentConfig *
clawt_daemon_create_agent(ClawtDaemon  *self,
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

/*
 * One tool call an agent made, on its way to the turn watch.
 *
 * A thin adapter rather than the work itself: what is done with it lives
 * in daemon-turn.c beside the counters it feeds.
 */
static void
on_tool_call_observed(const gchar *agent_id, const gchar *tool,
                      const gchar *args, gpointer user_data)
{
    clawt_daemon_turn_note_tool_call(CLAWT_DAEMON(user_data), agent_id, tool,
                                     args);

    /*
     * The same hook feeds a recording.  This is the one place that sees
     * every tool call an agent makes, including clawtilla_computer_exec
     * -- adding a second would be a second place for a call to be
     * missed, and the trace would be missing exactly the steps nobody
     * thought about.
     */
    clawt_daemon_teach_note_tool_call(CLAWT_DAEMON(user_data), agent_id,
                                      tool, args);
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

    clawt_loop_guard_set_cycle_seconds(
        self->guard,
        (guint)clawt_config_get_int(self->config,
                                    "orchestration.cycle_seconds"));

    clawt_loop_guard_set_task_budget(
        self->guard,
        clawt_config_get_double(self->config, "orchestration.task_budget_usd"));

    clawt_task_manager_set_max_depth(
        self->tasks,
        (guint)clawt_config_get_int(self->config, "orchestration.max_hops"));

    /*
     * The turn budgets go through here too, so a reload reaches them.
     * A no-op before clawt_daemon_turn_setup() has run, which is what
     * lets start call this before the objects exist.
     */
    clawt_daemon_turn_configure(self);
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


/*
 * Everything the fleet has left to bring up, and nothing else.
 */
static void
autostart_cancel(ClawtDaemon *self)
{
    if (self->autostart_source != NULL) {
        g_source_destroy(self->autostart_source);
        g_clear_pointer(&self->autostart_source, g_source_unref);
    }

    g_clear_pointer(&self->autostart_queue, g_ptr_array_unref);
    self->autostart_next = 0;
}

/*
 * One agent at a time, and never with the loop inside a blocking read.
 *
 * Two things had to be true and only one of them is about ordering.
 * Autostart used to run inside clawt_daemon_start(), before any main
 * loop existed, so nothing was dispatched until the whole fleet was up.
 * Moving it to an idle fixes that and *not* the rest: a container
 * agent's start is a blocking podman request, so an idle that called it
 * directly still held the loop for the length of that request -- which,
 * measured against a podman socket that accepted and went quiet, was 60
 * seconds an agent.  `agent list` timed out and SIGTERM went unanswered
 * exactly as before, from a version that looked fixed.
 *
 * So the wait itself goes to a worker thread, and the next agent is
 * scheduled when the previous one comes back.  Between the two, the loop
 * is doing what a loop does.
 */
static gboolean on_autostart_tick(gpointer user_data);

static gboolean
autostart_next(gpointer user_data)
{
    ClawtDaemon *self = user_data;

    if (!self->running)
        return G_SOURCE_REMOVE;

    /*
     * Through an idle rather than straight on, so a fleet whose agents
     * all finish instantly still yields between them.  Without it the
     * whole queue would run inside one completion callback and the
     * blocking-read fix would be the only half that worked.
     */
    if (self->autostart_source != NULL) {
        g_source_destroy(self->autostart_source);
        g_clear_pointer(&self->autostart_source, g_source_unref);
    }

    self->autostart_source = g_idle_source_new();
    g_source_set_callback(self->autostart_source, on_autostart_tick, self,
                          NULL);
    g_source_attach(self->autostart_source, self->main_context);

    return G_SOURCE_REMOVE;
}

/*
 * An idle rather than a timeout, and left at G_PRIORITY_DEFAULT_IDLE on
 * purpose.  Sockets and signal sources sit at G_PRIORITY_DEFAULT, which
 * is higher, so anything already waiting is served before the next agent
 * is reached for rather than after it.
 */
static gboolean
on_autostart_tick(gpointer user_data)
{
    ClawtDaemon *self = user_data;
    const gchar *agent_id;

    g_clear_pointer(&self->autostart_source, g_source_unref);

    if (!self->running || self->autostart_queue == NULL ||
        self->autostart_next >= self->autostart_queue->len) {
        g_clear_pointer(&self->autostart_queue, g_ptr_array_unref);
        self->autostart_next = 0;

        return G_SOURCE_REMOVE;
    }

    agent_id = g_ptr_array_index(self->autostart_queue,
                                 self->autostart_next);
    self->autostart_next++;

    daemon_start_agent_async(self, agent_id, autostart_next);

    return G_SOURCE_REMOVE;
}

/*
 * @self takes the ids rather than the agents, because an agent can be
 * removed while the fleet is still coming up -- a client is answered
 * throughout now, which is the whole point -- and a held ClawtAgent
 * would then be started after it had been deleted.  A stale id is
 * refused by start_agent_prepare() and warned about, which is the right
 * amount of noise for something somebody just did on purpose.
 */
static void
autostart_schedule(ClawtDaemon *self)
{
    GPtrArray *agents = clawt_agent_manager_list(self->agents);
    guint i;

    autostart_cancel(self);

    self->autostart_queue = g_ptr_array_new_with_free_func(g_free);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgent *agent = g_ptr_array_index(agents, i);
        ClawtAgentConfig *config = clawt_agent_get_config(agent);

        if (clawt_agent_get_state(agent) == CLAWT_AGENT_STATE_SHADOW)
            continue;

        if (!clawt_agent_config_get_boolean(config, "enabled"))
            continue;

        if (!clawt_agent_config_get_boolean(config, "runtime.autostart"))
            continue;

        g_ptr_array_add(self->autostart_queue,
                        g_strdup(clawt_agent_get_id(agent)));
    }

    if (self->autostart_queue->len == 0) {
        g_clear_pointer(&self->autostart_queue, g_ptr_array_unref);
        return;
    }

    self->autostart_source = g_idle_source_new();
    g_source_set_callback(self->autostart_source, on_autostart_tick, self,
                          NULL);
    g_source_attach(self->autostart_source, self->main_context);
}

gboolean
clawt_daemon_start(ClawtDaemon *self, GError **error)
{
    g_autofree gchar *transcript_dir = NULL;
    g_autofree gchar *tailnet_address = NULL;
    g_autofree gchar *event_dir = NULL;
    g_autofree gchar *exchange_dir = NULL;
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

    /*
     * And what only the whole fleet can see about the shared folders: an
     * entry scoped to an agent or a team that is not there. Said at
     * start because the symptom otherwise is an agent missing a
     * directory it was meant to have, noticed by the agent, days later.
     */
    {
        g_auto(GStrv) mount_warnings = NULL;

        clawt_mount_validate_fleet(self->config, &mount_warnings);

        for (i = 0; mount_warnings != NULL && mount_warnings[i] != NULL; i++)
            g_warning("config: %s", mount_warnings[i]);
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
     * Before anything else reads or writes in here.  Everything below
     * this point -- the git repository, the transcripts, the event log,
     * the mailboxes -- assumes it is the only writer.
     */
    if (!acquire_state_lock(self->state_dir, &self->state_lock_fd, error)) {
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

        if (!clawt_daemon_prepare_state_git(
                self->state_dir,
                clawt_config_get_boolean(self->config, "daemon.git"),
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
        g_autofree gchar *transcript_db =
            g_build_filename(self->state_dir, "transcripts.db", NULL);
        g_autoptr(GError) index_error = NULL;

        self->transcripts = clawt_transcript_index_new(transcript_db,
                                                       &index_error);

        /*
         * A warning, like the decision store: a fleet that cannot search
         * what it said is a fleet with one feature missing, and refusing
         * to start over it would take the other twenty with it.
         */
        if (self->transcripts == NULL)
            g_warning("transcripts: %s", index_error->message);
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

    /*
     * The model of the person this fleet works for.
     *
     * After the agent manager, because the learned half of the profile
     * lives in the fleet-scope memory database and the manager is what
     * owns the shared scopes.  Written at a fixed place --
     * `<state_dir>/OPERATOR.org` -- so it can be opened in an editor
     * whether or not anything has been recorded in it yet.
     */
    self->operator_profile = clawt_operator_profile_new(
        self->state_dir,
        clawt_agent_manager_get_memory_scopes(self->agents));

    self->rooms = clawt_room_manager_new(transcript_dir);
    clawt_room_manager_load(self->rooms, self->config);

    /*
     * And the direct rooms, which nothing in the config names.  They are
     * made on demand, so without this a conversation between two agents
     * was invisible after a restart until they happened to speak again.
     */
    clawt_room_manager_load_direct(self->rooms);

    /*
     * The screen, before any client can ask about it.
     *
     * The observer is given this daemon's context explicitly. It attaches
     * a timer per watched agent, and g_timeout_add() would put those on
     * the global default -- which for an embedded daemon is a loop
     * nobody runs, so the preview would show its first frame and never
     * move again with nothing logged to say why.
     */
    {
        g_autofree gchar *frame_dir =
            g_build_filename(self->state_dir, "frames", NULL);

        self->observer = clawt_observer_new(frame_dir, self->main_context);
    }

    self->takeover = clawt_takeover_new();

    g_signal_connect(self->observer, "frame",
                     G_CALLBACK(clawt_daemon_on_observer_frame), self);
    g_signal_connect(self->observer, "failed",
                     G_CALLBACK(clawt_daemon_on_observer_failed), self);
    g_signal_connect(self->takeover, "changed",
                     G_CALLBACK(clawt_daemon_on_takeover_changed), self);

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

    clawt_daemon_triggers_start(self);
    clawt_daemon_venture_start(self);

    {
        g_autofree gchar *pods =
            clawt_config_get_path_value(self->config, "daemon.automation_dir");
        g_autoptr(GError) local = NULL;

        self->automation = clawt_automation_new(self->bus, self->main_context,
                                                pod_action, self);

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
    clawt_mailbox_router_set_transcript_index(self->router,
                                              self->transcripts);

    /*
     * And what the fleet said before there was an index to say it into.
     *
     * Only when the index is empty.  Every message from here on is
     * indexed by the router as it is routed, so the one gap is the
     * conversations that predate the index -- and re-walking every room
     * on every start would cost a large fleet seconds of a daemon that
     * answers nothing, to write rows it already has.  Deleting
     * transcripts.db is therefore how a rebuild is asked for, which is
     * also the only way to ask for one that cannot go wrong.
     *
     * The transcripts are already in memory: clawt_room_manager_load()
     * has just read every one of them back through clawt_room_restore().
     */
    if (self->transcripts != NULL &&
        clawt_transcript_index_count(self->transcripts) == 0) {
        g_autoptr(GPtrArray) rooms = clawt_room_manager_list(self->rooms);
        guint indexed = 0;
        guint room_index;

        for (room_index = 0; rooms != NULL && room_index < rooms->len;
             room_index++) {
            ClawtRoom *room = g_ptr_array_index(rooms, room_index);
            g_autoptr(GPtrArray) history = clawt_room_get_history(room, 0);
            guint message_index;

            for (message_index = 0;
                 history != NULL && message_index < history->len;
                 message_index++) {
                if (clawt_transcript_index_add(
                        self->transcripts, clawt_room_get_id(room),
                        g_ptr_array_index(history, message_index), NULL))
                    indexed++;
            }
        }

        if (indexed > 0)
            g_message("transcripts: indexed %u message(s) already on disk",
                      indexed);
    }

    self->mcp_tools = clawt_mcp_tools_new(self->agents, self->tasks,
                                          self->guard);

    /*
     * The daemon is the only thing that knows which context its answers
     * must arrive on, so it says so rather than leaving the tools to take
     * whatever is thread-default when a call comes in.  NULL here would
     * be correct for an embedding host that runs the process default.
     */
    clawt_mcp_tools_set_main_context(self->mcp_tools, self->main_context);

    clawt_mcp_tools_set_deliver_func(self->mcp_tools, deliver_for_tools,
                                     self, NULL);

    /*
     * After the tools exist, because reload_skills() hands them the
     * library it just built.  Doing it earlier and setting the pointer
     * again here would be the same rule at two call sites, which is how
     * one of them comes to be forgotten.
     */
    clawt_daemon_reload_skills(self);

    /*
     * So an agent's own tool calls land on the same audit trail as a
     * person's.  The client `computer.exec` handler has published one
     * per command since the daemon was written; the tools had no route
     * to a bus at all, so exactly the half somebody would want to look
     * up -- what the agent ran on its own initiative -- was missing.
     */
    clawt_mcp_tools_set_event_bus(self->mcp_tools, self->bus);
    clawt_mcp_tools_set_takeover(self->mcp_tools, self->takeover);
    clawt_mcp_tools_set_room_manager(self->mcp_tools, self->rooms);
    clawt_mcp_tools_set_transcript_index(self->mcp_tools,
                                         self->transcripts);

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

    /*
     * Every tool call is reported to the turn watch: it is both the
     * clearest sign of life the daemon gets and the only place a
     * repeated call can be counted.
     */
    clawt_mcp_tools_set_observer(self->mcp_tools, on_tool_call_observed,
                                 self, NULL);

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
    clawt_link_server_set_auth_func(
        self->link_server, clawt_daemon_authenticate_agent, self, NULL);
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
     * The turn budgets, the repeat counter and the steer queue.  Here
     * rather than beside the guard, because this builds a timer of its
     * own and start can still refuse after the components exist -- a
     * timer armed before that point belongs to a daemon that never ran
     * and nothing takes it down again.
     */
    clawt_daemon_turn_setup(self);

    /*
     * After the turn machinery, because the handoff queue drains from
     * clawt_daemon_turn_settle() and setup runs anything that was left
     * queued when this daemon last stopped -- which settles turns.
     */
    clawt_daemon_handoff_setup(self);

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
        g_autoptr(GPtrArray) refusals = clawt_daemon_render_refusals_new();
        g_autoptr(GString) names = g_string_new(NULL);
        guint refused;

        clawt_daemon_render_all_agents_into(self, refusals);

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

    /*
     * Queued, not started.
     *
     * This ran inline, and starting a container agent is a blocking
     * podman request each -- so on a real fleet clawt_daemon_start() sat
     * there for minutes, before the main loop existed.  For that whole
     * window the daemon answered no IPC frame and dispatched no signal
     * source: `agent list` hung with no reply and no error, `kill -TERM`
     * did nothing because the handler could not run, and systemd's stop
     * timed out and escalated to SIGABRT -- so the agents were SIGKILLed
     * rather than stopped, and the daemon dumped core on the way out.
     *
     * The comment immediately below this one records the model cache
     * being moved out of here for the same reason, and the rule was
     * already written down as "an IPC handler must not wait on the
     * network -- nor may daemon start".  It was applied to the caller
     * that had been noticed rather than to the function, so the second
     * blocking thing in the same function was never looked at.
     */
    autostart_schedule(self);

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

    /*
     * First, and before the fleet is stopped: a pending autostart would
     * otherwise carry on starting agents behind a stop that had already
     * walked past them.
     */
    autostart_cancel(self);

    if (self->sweep_source_id != 0) {
        GSource *source = g_main_context_find_source_by_id(
            self->main_context, self->sweep_source_id);

        if (source != NULL)
            g_source_destroy(source);

        self->sweep_source_id = 0;
    }

    /*
     * Before the fleet is stopped, because a grace timer left armed past
     * this point fires into a daemon that has already dropped the agent
     * manager it would look an agent up in.
     */
    clawt_daemon_turn_teardown(self);
    clawt_daemon_handoff_teardown(self);

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

    /*
     * Last, after the rooms have been flushed and the servers are down:
     * until here this daemon is still the one writing in the state
     * directory, and releasing the lock early would let another take it
     * while the flush was still running.
     *
     * Released explicitly rather than left to process exit, because an
     * embedded host may stop and start the daemon in one process and the
     * second start has to be able to take the lock back.
     */
    if (self->state_lock_fd >= 0) {
        close(self->state_lock_fd);
        self->state_lock_fd = -1;
    }

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
gboolean
clawt_daemon_reload_internal(ClawtDaemon *self, GPtrArray *refusals,
                             GError **error)
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
     * Rebuilt rather than rescanned, because `skills.dir` and
     * `skills.enabled` are both in the file that just changed.  Keeping
     * the old library would leave it watching a directory nobody has
     * configured any more, which is invisible: every listing would be
     * right about the wrong directory.
     */
    clawt_daemon_reload_skills(self);

    /*
     * And the venture bridge, for the same reason the notifier is
     * reloaded here: its credential is resolved from a file the config
     * names, so a connector added, removed or re-authorised reaches it
     * on a reload rather than on a restart.
     */
    clawt_daemon_venture_sync(self);

    /*
     * Files are re-rendered for running agents too, so a restart picks up
     * the change -- but nothing is restarted here.  A reload that
     * interrupted every agent mid-turn would make editing one description
     * cost the whole fleet's work.
     */
    clawt_daemon_render_all_agents_into(self, refusals);

    clawt_event_bus_emit(self->bus, "daemon.reloaded", NULL);
    g_signal_emit(self, signals[SIGNAL_RELOADED], 0);

    return TRUE;
}

gboolean
clawt_daemon_reload(ClawtDaemon *self, GError **error)
{
    return clawt_daemon_reload_internal(self, NULL, error);
}


/* ── The client surface ──────────────────────────────────────────── */

/*
 * The schema entry an agent-relative key names, or %NULL for one the
 * schema has never heard of.
 *
 * Found by asking every entry what it is called inside an agent block,
 * because that is the same question clawt_daemon_add_agent_settings()
 * below walks the schema to answer -- and the two have to agree.
 * `mailbox.overflow` in an agent is the schema's
 * `orchestration.mailbox.overflow`, so a lookup that only tried
 * `agents.<key>` would find nothing for exactly the nine options that
 * are settable in two places.
 */
const ClawtSchemaEntry *
clawt_daemon_agent_setting_entry(const gchar *key)
{
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;

    if (key == NULL)
        return NULL;

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        if (g_strcmp0(clawt_config_schema_agent_name(&schema[i]), key) == 0)
            return &schema[i];
    }

    return NULL;
}

/*
 * Whether changing @key reaches a running agent's files and not the
 * session it is in the middle of.
 *
 * An AI CLI reads two things exactly once, when a session starts: the
 * system prompt it is handed, and the tool list it asks its MCP servers
 * for. A setting in that set, changed under a running agent, therefore
 * lands everywhere except the conversation actually happening -- and the
 * agent then reports, accurately, not having what was just granted.
 *
 * The two prefixes were the whole rule and they do not state the set.
 * `chief_of_staff` and `team_role` are the two halves of one condition
 * in clawt_mcp_tools_is_permitted(): between them they decide whether
 * the delegation tool is offered at all, exactly as `tools.manage_fleet`
 * does, and neither of them begins with `tools.`. So a chief granted its
 * role while it ran got the gate, the files and the TOOLS.org region,
 * kept a session that had already listed its tools, and nothing said so.
 * Which is how a bug report gets answered with an instruction to flip a
 * switch that was flipped before it was read.
 *
 * Named here rather than derived, because nothing in the schema records
 * "read once per session" -- it is a property of the CLI on the other
 * side, not of the option.
 *
 * One answer for every key on it, though the remedy differs and the
 * clients say so: a tool list is read at session start, so a restart is
 * enough, while a resumed session is never handed a system prompt at
 * all and only `agent reset` applies a `persona.` change.
 */
gboolean
clawt_daemon_setting_needs_a_new_session(const gchar *key)
{
    static const gchar *const session_scoped[] = {
        /* Both gate NEEDS_ASSIGNMENT, which decides the tool list. */
        "chief_of_staff",
        "team_role"
    };
    gsize i;

    if (key == NULL)
        return FALSE;

    if (g_str_has_prefix(key, "tools.") || g_str_has_prefix(key, "persona."))
        return TRUE;

    for (i = 0; i < G_N_ELEMENTS(session_scoped); i++) {
        if (g_strcmp0(key, session_scoped[i]) == 0)
            return TRUE;
    }

    return FALSE;
}

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
void
clawt_daemon_add_agent_settings(JsonBuilder *builder, ClawtAgent *agent)
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
void
clawt_daemon_deliver_decision_answer(ClawtDaemon *self,
                                     ClawtDecision *decision)
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
     * The clock starts again with whatever was left.  Clamped at zero in
     * the watch itself, because a resolve can arrive for a card the
     * current turn never opened -- the stale cleanup after an interrupt
     * does exactly that.
     */
    clawt_daemon_turn_release(self, agent);

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
void
clawt_daemon_add_decision_object(JsonBuilder   *builder,
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

/*
 * Whether stopping this agent's machine destroys it.
 *
 * `computer.container.keep` is false by default, and container_stop()
 * removes the container when it is -- so a Stop is a Stop for a VM and a
 * Stop-and-delete for a container. Answered in one place because the
 * listing has to say it *before* somebody presses the button and the
 * stop handler has to report it afterwards, and two readings of one
 * setting is how those two would come to disagree.
 */
gboolean
clawt_daemon_computer_stop_removes(ClawtAgentConfig *config)
{
    ClawtComputerType type;

    if (config == NULL)
        return FALSE;

    type = (ClawtComputerType)clawt_agent_config_get_enum(config,
                                                          "computer.type");

    if (type == CLAWT_COMPUTER_CONTAINER)
        return !clawt_agent_config_get_boolean(config,
                                               "computer.container.keep");

    if (type == CLAWT_COMPUTER_DISTROBOX)
        return !clawt_agent_config_get_boolean(config,
                                               "computer.distrobox.keep");

    return FALSE;
}

void
clawt_daemon_add_agent_object(JsonBuilder *builder, ClawtAgent *agent)
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
    clawt_daemon_add_string_member(
        builder, "avatar", clawt_agent_config_get_string(config, "avatar"));
    clawt_daemon_add_string_member(
        builder, "color", clawt_agent_config_get_string(config, "color"));

    /*
     * Whether `agent.avatar` has anything to hand back, so a client
     * knows whether to ask for it at all -- a request for bytes that do
     * not exist is one round trip spent finding that out, on every
     * agent, every time a list is drawn.
     *
     * Never the raw "avatar" string above: that is the *setting*, and an
     * agent with none configured can still have a picture through
     * auto-detection.  This is the answer clawt_avatar_resolve_path()
     * would give, cheaply -- a handful of stats, no bytes read and no
     * etag computed, which is why the etag is only ever reported by
     * `agent.avatar` itself.
     */
    {
        g_autofree gchar *workspace = clawt_agent_config_get_workspace(config);
        g_autofree gchar *resolved =
            (workspace != NULL)
                ? clawt_avatar_resolve_path(
                      clawt_agent_config_get_string(config, "avatar"),
                      workspace)
                : NULL;

        json_builder_set_member_name(builder, "has_avatar");
        json_builder_add_boolean_value(builder, resolved != NULL);
    }

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
     * Whether that type has a machine of its own to power on and off,
     * and whether stopping it destroys it.
     *
     * Reported rather than worked out from the type by each client.
     * Which types have a machine is a library question --
     * clawt_computer_type_has_machine() -- and a client answering it
     * from a list of its own would offer Stop on a backend added later,
     * or fail to, with nothing to say which. `computer_stop_removes` has
     * to be known *before* the button is pressed: for a container the
     * contents do not come back.
     *
     * Flat, beside `computer`, rather than nested under it. The first
     * draft made `computer` an object and json-glib kept the *last* of
     * the two members with that name -- so the object was silently
     * discarded, and the only reason it showed up at all was a client
     * reading a string where an object was expected.
     */
    json_builder_set_member_name(builder, "computer_machine");
    json_builder_add_boolean_value(
        builder, clawt_computer_type_has_machine(
                     (ClawtComputerType)clawt_agent_config_get_enum(
                         config, "computer.type")));

    /*
     * And whether there could be a screen, for the same reason and from
     * the same kind of predicate: a client deciding for itself which
     * types have one would draw a Screen tab on a backend that has none.
     */
    json_builder_set_member_name(builder, "computer_screen");
    json_builder_add_boolean_value(
        builder, clawt_computer_type_has_screen(
                     (ClawtComputerType)clawt_agent_config_get_enum(
                         config, "computer.type")));

    json_builder_set_member_name(builder, "computer_stop_removes");
    json_builder_add_boolean_value(builder,
                                   clawt_daemon_computer_stop_removes(config));

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

void
clawt_daemon_add_task_object(JsonBuilder *builder, ClawtTask *task)
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

    /*
     * The latest word from the assignee on a task that has not ended.
     * A person looking at a task that has been running an hour is asking
     * the same question a delegating agent asks, and the answer was only
     * reachable through the agent-facing tools.
     */
    if (clawt_task_get_progress_note(task) != NULL) {
        json_builder_set_member_name(builder, "progress_note");
        json_builder_add_string_value(builder,
                                      clawt_task_get_progress_note(task));
    }

    /*
     * And whether anybody actually reported it finished.  Emitted only
     * when true, so it reads as a caveat on the tasks that have one
     * rather than as a column that is usually "no".
     */
    if (clawt_task_get_result_inferred(task)) {
        json_builder_set_member_name(builder, "result_inferred");
        json_builder_add_boolean_value(builder, TRUE);
    }

    if (clawt_task_get_parent_id(task) != NULL) {
        json_builder_set_member_name(builder, "parent");
        json_builder_add_string_value(builder,
                                      clawt_task_get_parent_id(task));
    }

    json_builder_set_member_name(builder, "depth");
    json_builder_add_int_value(builder, clawt_task_get_depth(task));

    json_builder_end_object(builder);
}

void
clawt_daemon_add_mailbox_item(JsonBuilder *builder, ClawtMailboxItem *item)
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

ClawtMailbox *
clawt_daemon_mailbox_for(ClawtDaemon *self, JsonObject *payload,
                         GError **error)
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

void
clawt_daemon_add_key_array(JsonBuilder *builder, const gchar *member,
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

void
clawt_daemon_add_integration_object(JsonBuilder            *builder,
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
        clawt_enum_to_nick(CLAWT_TYPE_SCOPE,
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
void
clawt_daemon_add_binding_object(JsonBuilder *builder,
                                ClawtIntegrationBinding *binding)
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
gboolean
clawt_daemon_apply_integration_fields(ClawtIntegrationConfig  *instance,
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

        if (!clawt_enum_from_nick(CLAWT_TYPE_SCOPE, nick,
                                  &scope)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' is not a scope: use all, selected or none",
                        nick != NULL ? nick : "");
            return FALSE;
        }

        if (json_object_has_member(payload, "agents"))
            agents = clawt_ipc_payload_strv(payload, "agents");

        clawt_integration_config_set_scope(
            instance, (ClawtScope)scope,
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

void
clawt_daemon_health_result_free(HealthResult *self)
{
    if (self == NULL)
        return;

    g_free(self->name);
    g_free(self->type_id);
    g_free(self->message);
    g_free(self);
}

static void health_run_step(HealthRun *run);

void
clawt_daemon_health_run_free(HealthRun *run)
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

    clawt_daemon_health_run_free(run);
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

void
clawt_daemon_health_run_start(HealthRun *run)
{
    health_run_step(run);
}

/* ── Matrix sign-in ──────────────────────────────────────────────── */

void
clawt_daemon_matrix_login_free(MatrixLogin *self)
{
    if (self == NULL)
        return;

    g_free(self->name);
    g_free(self->agent_id);
    g_free(self->homeserver);
    g_free(self);
}

void
clawt_daemon_on_matrix_login(GObject *source, GAsyncResult *result,
                             gpointer user_data)
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
        clawt_daemon_matrix_login_free(login);
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
        clawt_daemon_matrix_login_free(login);
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
        clawt_daemon_matrix_login_free(login);
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
        clawt_daemon_matrix_login_free(login);
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
        clawt_daemon_matrix_login_free(login);
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

    clawt_daemon_matrix_login_free(login);
}

void
clawt_daemon_on_notify_tested(GObject *source, GAsyncResult *result,
                              gpointer user_data)
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

void
clawt_daemon_on_matrix_rooms(GObject *source, GAsyncResult *result,
                             gpointer user_data)
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
/*
 * A deferred tool call has finished; answer the frame it came in on.
 *
 * The response is wrapped exactly as the synchronous path wraps it, so a
 * client cannot tell which way its request was served -- which is the
 * point: the protocol did not change, only where the waiting happens.
 */
void
clawt_daemon_on_tool_rpc_finished(GObject *source, GAsyncResult *result,
                                  gpointer user_data)
{
    ClawtIpcPending *pending = user_data;
    g_autoptr(JsonNode) rpc_response = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();

    rpc_response = clawt_mcp_tools_call_finish(CLAWT_MCP_TOOLS(source),
                                               result);

    if (rpc_response == NULL) {
        clawt_ipc_pending_respond(
            pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(pending),
                                CLAWT_ERROR_FAILED,
                                "the tool produced no response"));
        return;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "response");
    json_builder_add_value(builder, json_node_ref(rpc_response));
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(pending),
                               json_builder_get_root(builder)));
}


void
clawt_daemon_exec_job_free(ExecJob *job)
{
    if (job == NULL)
        return;

    g_clear_object(&job->computer);
    g_clear_object(&job->daemon);
    g_free(job->agent_id);
    g_free(job->command);
    g_free(job);
}

void
clawt_daemon_on_ipc_exec_finished(GObject *source, GAsyncResult *result,
                                  gpointer user_data)
{
    ExecJob *job = user_data;
    g_autoptr(ClawtExecResult) exec = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();

    exec = clawt_computer_exec_finish(CLAWT_COMPUTER(source), result, &error);

    /*
     * Every host command is recorded, whoever asked for it, and the
     * record is written here rather than beside the request -- moving the
     * wait off the loop moves the moment the command ended with it.
     * Running something on the machine is the most consequential thing
     * this socket can do, and an audit trail that only covers agents
     * would miss exactly the case a person would want to look up.
     *
     * A command that could not be run at all is recorded too, with an
     * exit of -1: a trail holding only the successes reads as "it did not
     * happen" rather than as "we do not know what it did".
     */
    {
        ClawtEvent *event = clawt_event_new("computer.exec", job->agent_id);

        clawt_event_set_detail(event, "command", job->command);
        clawt_event_set_detail_int(
            event, "exit",
            (exec != NULL) ? clawt_exec_result_get_exit_status(exec) : -1);
        clawt_event_bus_publish(job->daemon->bus, event);
        clawt_event_free(event);
    }

    if (exec == NULL) {
        clawt_ipc_pending_respond(
            job->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(job->pending),
                                error->code, error->message));
        clawt_daemon_exec_job_free(job);
        return;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "exit");
    json_builder_add_int_value(builder,
                               clawt_exec_result_get_exit_status(exec));
    json_builder_set_member_name(builder, "stdout");
    json_builder_add_string_value(builder, clawt_exec_result_get_stdout(exec));
    json_builder_set_member_name(builder, "stderr");
    json_builder_add_string_value(builder, clawt_exec_result_get_stderr(exec));
    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        job->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(job->pending),
                               json_builder_get_root(builder)));
    clawt_daemon_exec_job_free(job);
}


void
clawt_daemon_lifecycle_job_free(LifecycleJob *job)
{
    g_clear_object(&job->daemon);
    g_clear_object(&job->computer);
    g_free(job->agent_id);
    g_free(job);
}

static const gchar *
lifecycle_name(ClawtComputerLifecycle op)
{
    switch (op) {
    case CLAWT_COMPUTER_LIFECYCLE_START:
        return "start";
    case CLAWT_COMPUTER_LIFECYCLE_STOP:
        return "stop";
    case CLAWT_COMPUTER_LIFECYCLE_RESTART:
    default:
        return "restart";
    }
}

void
clawt_daemon_on_ipc_lifecycle_finished(GObject *source, GAsyncResult *result,
                                       gpointer user_data)
{
    LifecycleJob *job = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    gboolean ok;

    ok = clawt_computer_lifecycle_finish(CLAWT_COMPUTER(source), result,
                                         &error);

    /*
     * Recorded whichever way it went, and beside the exec trail rather
     * than in a log line: powering an agent's machine off is the second
     * most consequential thing this socket can do, and a trail holding
     * only the successes reads as "it did not happen" rather than as
     * "we do not know what it did".
     */
    {
        ClawtEvent *event = clawt_event_new("computer.power", job->agent_id);

        clawt_event_set_detail(event, "action", lifecycle_name(job->op));
        clawt_event_set_detail(event, "result", ok ? "ok" : "failed");
        clawt_event_bus_publish(job->daemon->bus, event);
        clawt_event_free(event);
    }

    /*
     * The state dot and the computer line both come from the agent, so
     * every client redraws whether or not the verb worked -- a failed
     * stop may still have left the machine somewhere new.
     */
    clawt_event_bus_emit(job->daemon->bus, "agent.changed", job->agent_id);

    if (!ok) {
        clawt_ipc_pending_respond(
            job->pending,
            clawt_ipc_error_new(clawt_ipc_pending_get_request(job->pending),
                                error->code, error->message));
        clawt_daemon_lifecycle_job_free(job);
        return;
    }

    json_builder_begin_object(builder);
    clawt_daemon_add_string_member(builder, "agent", job->agent_id);
    clawt_daemon_add_string_member(builder, "action", lifecycle_name(job->op));
    json_builder_set_member_name(builder, "state");
    json_builder_add_string_value(
        builder, clawt_enum_to_nick(CLAWT_TYPE_COMPUTER_STATE,
                                    clawt_computer_get_state(job->computer)));

    /*
     * Whether this machine survives a stop, not whether one was
     * destroyed just now.  The distinction matters: a fresh computer
     * object knows nothing about a machine it did not start, so
     * reporting an observed removal would mean claiming a destruction
     * that may not have happened -- which the first run of this did,
     * announcing that a container nobody had ever created was gone.
     *
     * As policy it is true either way, and it is the sentence somebody
     * needs: what they had is not coming back.
     */
    json_builder_set_member_name(builder, "removes");
    json_builder_add_boolean_value(
        builder, job->removes && job->op != CLAWT_COMPUTER_LIFECYCLE_START);

    json_builder_end_object(builder);

    clawt_ipc_pending_respond(
        job->pending,
        clawt_ipc_response_new(clawt_ipc_pending_get_request(job->pending),
                               json_builder_get_root(builder)));
    clawt_daemon_lifecycle_job_free(job);
}

/*
 * The client surface, one file per verb family.
 *
 * The order is the order these were in when this was one chain of
 * `if (g_strcmp0(kind, ...))`.  No kind is spelled in two families, so
 * nothing depends on it; it is kept so that a reader can follow this
 * against the chain it came from.
 */
static const ClawtDaemonFamilyFunc family_handlers[] = {
    clawt_daemon_handle_control,
    clawt_daemon_handle_misc,
    clawt_daemon_handle_agent,
    clawt_daemon_handle_memory,
    clawt_daemon_handle_mount,
    clawt_daemon_handle_image,
    clawt_daemon_handle_team,
    clawt_daemon_handle_room,
    clawt_daemon_handle_mailbox,
    clawt_daemon_handle_task,
    clawt_daemon_handle_computer,
    clawt_daemon_handle_screen,
    clawt_daemon_handle_design,
    clawt_daemon_handle_connector,
    clawt_daemon_handle_integration,
    clawt_daemon_handle_routine,
    clawt_daemon_handle_trigger,
    clawt_daemon_handle_config,
    clawt_daemon_handle_skill,
    clawt_daemon_handle_teach,
};

JsonNode *
clawt_daemon_handle_request(ClawtDaemon *self, JsonNode *request)
{
    JsonObject *payload;
    const gchar *kind;
    gsize i;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);
    g_return_val_if_fail(request != NULL, NULL);

    kind = clawt_ipc_frame_get_kind(request);
    payload = clawt_ipc_frame_get_payload(request);

    if (!self->running)
        return clawt_ipc_error_new(request, CLAWT_ERROR_AGENT_STATE,
                                   "the daemon is not running");

    for (i = 0; i < G_N_ELEMENTS(family_handlers); i++) {
        gboolean handled = FALSE;
        JsonNode *reply = family_handlers[i](self, kind, request, payload,
                                             &handled);

        if (handled)
            return reply;
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
    g_clear_object(&self->skills);
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
    /* 0 is a valid descriptor, so "no lock" has to be -1. */
    self->state_lock_fd = -1;

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
