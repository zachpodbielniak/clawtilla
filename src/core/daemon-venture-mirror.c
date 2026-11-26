/*
 * daemon-venture-mirror.c - The daemon's side of the VENTURE mirror
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Which agents mirror (a venture connector with `mirror_tasks` on, and a
 * token), the one HTTP transport the mirror sends through, where a
 * task's repository is, and the IPC that shows and retries it.  The
 * rules -- who writes what, what a status means -- are the mirror's own,
 * in clawt-venture-mirror.c.
 */

#include "clawtilla.h"
#include "clawt-http.h"
#include "core/clawt-daemon-private.h"
#include "integration/clawt-connector.h"
#include "clawt-git-remote.h"

#include <libsoup/soup.h>
#include <string.h>

#define USER_AGENT "clawtilla/" CLAWT_VERSION_STRING
#define MIRROR_TIMEOUT_SECONDS (20)
#define MIRROR_INTERVAL_SECONDS (15)
#define MIRROR_REMOTES_TTL_SECONDS (600)

/* ── The transport ───────────────────────────────────────────────── */

typedef struct {
    ClawtDaemon *daemon;     /* not owned */
    SoupSession *session;    /* owned */
} MirrorTransport;

typedef struct {
    SoupMessage            *message;
    ClawtVentureMirror     *mirror;    /* reffed */
    ClawtVentureMirrorReply reply;
    gpointer                reply_data;
} MirrorRequest;

static void
mirror_transport_free(gpointer data)
{
    MirrorTransport *self = data;

    if (self == NULL)
        return;

    g_clear_object(&self->session);
    g_free(self);
}

static void
on_mirror_answered(GObject *source, GAsyncResult *result, gpointer data)
{
    MirrorRequest *request = data;
    g_autoptr(GBytes) body = NULL;
    g_autofree gchar *text = NULL;
    guint status;

    body = soup_session_send_and_read_finish(SOUP_SESSION(source), result,
                                             NULL);
    status = (body != NULL) ? soup_message_get_status(request->message) : 0;

    if (body != NULL) {
        gsize length = 0;
        const gchar *bytes = g_bytes_get_data(body, &length);

        text = g_strndup(bytes != NULL ? bytes : "", length);
    }

    request->reply(status, text, request->reply_data);

    g_object_unref(request->message);
    g_object_unref(request->mirror);
    g_free(request);
}

/*
 * One request.  The token goes in the Authorization header and nowhere
 * else; the context is pushed so the answer lands on the daemon's loop
 * (the same reasoning as venture_send() in daemon-venture.c).
 */
static void
mirror_send(ClawtVentureMirror     *mirror,
            const gchar            *method,
            const gchar            *url,
            const gchar            *token,
            const gchar            *idempotency_key,
            const gchar            *body,
            ClawtVentureMirrorReply reply,
            gpointer                reply_data,
            gpointer                user_data)
{
    MirrorTransport *transport = user_data;
    SoupMessage *message;
    MirrorRequest *request;
    GMainContext *context;
    g_autofree gchar *bearer = g_strconcat("Bearer ", token, NULL);

    message = clawt_http_message_new(method, url,
                                     CLAWT_HTTP_CARRIES_CREDENTIALS);

    if (message == NULL) {
        reply(400, "{\"message\":\"not a URL clawtilla can dial\"}",
              reply_data);
        return;
    }

    soup_message_headers_replace(soup_message_get_request_headers(message),
                                 "Authorization", bearer);

    if (idempotency_key != NULL)
        soup_message_headers_replace(soup_message_get_request_headers(message),
                                     "Idempotency-Key", idempotency_key);

    if (body != NULL) {
        g_autoptr(GBytes) bytes = g_bytes_new(body, strlen(body));

        soup_message_set_request_body_from_bytes(message, "application/json",
                                                 bytes);
    }

    request = g_new0(MirrorRequest, 1);
    request->message = message;
    request->mirror = g_object_ref(mirror);
    request->reply = reply;
    request->reply_data = reply_data;

    context = transport->daemon->main_context;

    if (context != NULL)
        g_main_context_push_thread_default(context);

    soup_session_send_and_read_async(transport->session, message,
                                     G_PRIORITY_DEFAULT, NULL,
                                     on_mirror_answered, request);

    if (context != NULL)
        g_main_context_pop_thread_default(context);
}

/* ── Where a task's work is ──────────────────────────────────────── */

typedef struct {
    gint64 at;
    GStrv  urls;
} CachedRemotes;

static void
cached_remotes_free(gpointer data)
{
    CachedRemotes *self = data;

    g_strfreev(self->urls);
    g_free(self);
}

/*
 * The fetch URLs of a directory's remotes, origin first, remembered for
 * ten minutes: a repository's remotes rarely change, and asking git on
 * every task save would be a process per save.
 */
static GStrv
remotes_of(ClawtDaemon *self, const gchar *directory)
{
    CachedRemotes *cached;
    g_autoptr(GPtrArray) remotes = NULL;
    GPtrArray *urls;
    gint64 now = g_get_monotonic_time() / G_USEC_PER_SEC;
    guint i;

    if (directory == NULL || *directory == '\0')
        return NULL;

    if (self->mirror_remotes == NULL)
        self->mirror_remotes = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                     g_free,
                                                     cached_remotes_free);

    cached = g_hash_table_lookup(self->mirror_remotes, directory);

    if (cached != NULL && now - cached->at < MIRROR_REMOTES_TTL_SECONDS)
        return g_strdupv(cached->urls);

    remotes = clawt_git_remotes_for_dir(directory, NULL);
    urls = g_ptr_array_new();

    for (i = 0; remotes != NULL && i < remotes->len; i++) {
        ClawtGitRemote *remote = g_ptr_array_index(remotes, i);

        g_ptr_array_add(urls, g_strdup(remote->url));
    }

    g_ptr_array_add(urls, NULL);

    cached = g_new0(CachedRemotes, 1);
    cached->at = now;
    cached->urls = (GStrv)g_ptr_array_free(urls, FALSE);
    g_hash_table_replace(self->mirror_remotes, g_strdup(directory), cached);

    return g_strdupv(cached->urls);
}

static GStrv
mirror_target(ClawtVentureMirror *mirror,
              const gchar        *task_id,
              gint64             *repo_id,
              gpointer            user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(ClawtProjectSpec) project = NULL;
    const gchar *directory;

    (void)mirror;
    *repo_id = 0;

    task = clawt_task_store_get(self->task_store, task_id);

    if (task == NULL)
        return NULL;

    if (clawt_task_get_project(task) != NULL)
        project = clawt_config_get_project(self->config,
                                            clawt_task_get_project(task));

    if (project != NULL)
        *repo_id = project->venture_repo_id;

    /* The checkout's repository, else the project's directory. */
    directory = clawt_task_get_checkout_repo(task);

    if (directory == NULL && project != NULL)
        directory = project->directory;

    return remotes_of(self, directory);
}

/* ── Changes made in VENTURE (0.45.0) ────────────────────────────── */

static void
inbound_cancel(gpointer data, const gchar *task_id, const gchar *who)
{
    ClawtDaemon *self = data;
    g_autofree gchar *reason = (who == NULL || g_strcmp0(who, "VENTURE") == 0)
        ? g_strdup("cancelled in VENTURE")
        : g_strdup_printf("cancelled in VENTURE by %s", who);

    if (clawt_task_manager_cancel(self->tasks, task_id, reason, "user") > 0)
        g_message("venture mirror: task %s %s", task_id, reason);
}

static void
inbound_review(gpointer data, const gchar *task_id, gboolean accept,
               const gchar *notes, const gchar *who)
{
    ClawtDaemon *self = data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *said = NULL;

    (void)who;

    /* The operator's authority: the ticket is the operator's other door. */
    said = clawt_daemon_tasks_review(self, "user", task_id, accept,
                                     accept ? NULL : notes, &error);

    if (said == NULL)
        g_message("venture mirror: task %s: the review from VENTURE was not "
                  "taken: %s", task_id, error->message);
}

static void
inbound_comment(gpointer data, const gchar *task_id, const gchar *who,
                const gchar *text)
{
    ClawtDaemon *self = data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *author = g_strconcat("venture:", who, NULL);
    g_autofree gchar *said = clawt_venture_mirror_comment_body(who, task_id,
                                                               text);

    if (clawt_daemon_task_comment(self, task_id, author, said, text, NULL,
                                  &error) == 0)
        g_message("venture mirror: task %s: a VENTURE comment was not kept: "
                  "%s", task_id, error->message);
}

/*
 * A ticket assigned to an agent's VENTURE user becomes a task for it,
 * from the operator -- whose work it is to have let people assign the
 * fleet work there -- with the ticket's text fenced.
 */
static gchar *
inbound_intake(gpointer data, const gchar *agent_id, gint64 ticket,
               const gchar *title, const gchar *description, const gchar *url,
               const gchar *from_agent)
{
    ClawtDaemon *self = data;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *named = NULL;
    ClawtTask *task;

    if (clawt_agent_manager_get(self->agents, agent_id) == NULL)
        return NULL;

    body = clawt_venture_mirror_intake_body(ticket, url, title, description);

    /*
     * An agent that assigned the ticket through its own VENTURE tools
     * delegated it: the task is from that agent, and goes through the
     * router as its message -- so the pair budget and the hop limit hold
     * it exactly as clawtilla_delegate would (0.47.0).  From `user` it
     * would also renew the pair's count, which is how two agents could
     * hand each other work through VENTURE for ever.
     */
    if (from_agent != NULL &&
        clawt_agent_manager_get(self->agents, from_agent) == NULL)
        from_agent = NULL;

    task = clawt_task_manager_create(self->tasks,
                                     from_agent != NULL ? from_agent : "user",
                                     agent_id, body, NULL, &error);

    if (task == NULL) {
        g_message("venture mirror: ticket #%" G_GINT64_FORMAT " was not taken "
                  "in: %s", ticket, error->message);
        return NULL;
    }

    named = g_strdup_printf("#%" G_GINT64_FORMAT " %s", ticket, title);
    clawt_task_set_title(task, named);
    clawt_task_manager_save(self->tasks, task, NULL);

    if (from_agent != NULL) {
        g_autofree gchar *guidance = clawt_task_assignment_guidance_for(task);
        g_autofree gchar *sent = g_strconcat(clawt_task_get_prompt(task),
                                             guidance, NULL);

        if (clawt_mailbox_router_send_to(self->router, from_agent, agent_id,
                                         sent, clawt_task_get_id(task), 0,
                                         &error) < 0) {
            g_autofree gchar *reason = g_strdup_printf(
                "%s handed it over through VENTURE and the loop limits "
                "refused it: %s", from_agent, error->message);

            clawt_task_manager_fail(self->tasks, clawt_task_get_id(task),
                                    reason);
            g_message("venture mirror: ticket #%" G_GINT64_FORMAT ": %s",
                      ticket, reason);
            /* Linked anyway, so the next pass does not take it in again. */
            return g_strdup(clawt_task_get_id(task));
        }
    } else if (!clawt_daemon_deliver_operator_task(self, task, &error)) {
        g_message("venture mirror: ticket #%" G_GINT64_FORMAT " could not be "
                  "handed to %s: %s", ticket, agent_id, error->message);
        return NULL;
    }

    g_message("venture mirror: ticket #%" G_GINT64_FORMAT " became task %s "
              "for %s", ticket, clawt_task_get_id(task), agent_id);
    return g_strdup(clawt_task_get_id(task));
}

/*
 * How an agent stands for a ticket given to a VENTURE team it is in: the
 * tasks it has open -- everything assigned to it and not finished, ones
 * not yet started included, so a ticket taken this pass counts against
 * the next -- and whether it leads a clawtilla team.  Not a fleet agent: -1.
 */
gint
clawt_daemon_venture_mirror_standing(ClawtDaemon *self, const gchar *agent_id,
                                     gboolean *leads)
{
    ClawtAgent *agent = clawt_agent_manager_get(self->agents, agent_id);
    g_autoptr(GPtrArray) open = NULL;

    if (agent == NULL)
        return -1;

    *leads = (clawt_team_role_of(clawt_agent_get_config(agent)) ==
              CLAWT_TEAM_LEAD);
    open = clawt_task_manager_list(self->tasks, agent_id, FALSE);

    return (gint)open->len;
}

static gint
inbound_standing(gpointer data, const gchar *agent_id, gboolean *leads)
{
    return clawt_daemon_venture_mirror_standing(data, agent_id, leads);
}

static const ClawtVentureMirrorInbound inbound = {
    inbound_cancel, inbound_review, inbound_comment, inbound_intake,
    inbound_standing
};

/*
 * What a task cost: every agent's turns tagged with it, from their usage
 * ledgers -- the same reading the project budgets total.  Read when the
 * task's run is closed, once.
 */
static gint64
mirror_cost(ClawtVentureMirror *mirror, const gchar *task_id,
            gpointer user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(GHashTable) ids = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                      g_free, NULL);
    g_autoptr(GHashTable) spend = g_hash_table_new_full(g_str_hash,
                                                        g_str_equal, g_free,
                                                        NULL);
    GPtrArray *agents;  /* transfer none */
    gpointer value;
    guint i;

    (void)mirror;

    g_hash_table_add(ids, g_strdup(task_id));
    agents = clawt_agent_manager_list(self->agents);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        g_autofree gchar *state_dir = clawt_config_agent_state_dir(
            self->config, clawt_agent_get_id(g_ptr_array_index(agents, i)));
        g_autofree gchar *db_path = NULL;

        if (state_dir == NULL)
            continue;

        db_path = clawt_usage_database_path(state_dir);
        clawt_usage_task_spend(db_path, ids, spend, NULL);
    }

    value = g_hash_table_lookup(spend, task_id);
    return (gint64)GPOINTER_TO_SIZE(value);
}

/* A task's project's VENTURE milestone and goal (0.46.0). */
static void
mirror_context(ClawtVentureMirror *mirror, const gchar *task_id,
               gint64 *milestone_id, gint64 *goal_id, gpointer user_data)
{
    ClawtDaemon *self = user_data;
    g_autoptr(ClawtTask) task = NULL;
    g_autoptr(ClawtProjectSpec) project = NULL;

    (void)mirror;
    *milestone_id = 0;
    *goal_id = 0;

    task = clawt_task_store_get(self->task_store, task_id);

    if (task != NULL && clawt_task_get_project(task) != NULL)
        project = clawt_config_get_project(self->config,
                                           clawt_task_get_project(task));

    if (project != NULL) {
        *milestone_id = project->venture_milestone_id;
        *goal_id = project->venture_goal_id;
    }
}

/* What the poll read changed: the PROJECTS.org regions show it. */
static void
on_mirror_context_changed(ClawtVentureMirror *mirror, gpointer user_data)
{
    (void)mirror;
    clawt_daemon_refresh_projects_regions(CLAWT_DAEMON(user_data));
}

/*
 * whoami named an agent's VENTURE user, or a new one: kept, and its
 * TOOLS.org rewritten so the agent knows who it is there (0.50.0).
 */
static void
on_mirror_account_learned(ClawtVentureMirror *mirror,
                          const gchar        *agent_id,
                          const gchar        *username,
                          gpointer            user_data)
{
    ClawtDaemon *self = CLAWT_DAEMON(user_data);
    ClawtAgentConfig *agent;
    g_autoptr(GError) error = NULL;

    (void)mirror;

    if (agent_id == NULL || username == NULL)
        return;

    if (!clawt_workspace_set_venture_user(self->config, agent_id, username,
                                          &error)) {
        if (error != NULL)
            g_message("venture mirror: %s's VENTURE user %s was not kept: %s",
                      agent_id, username, error->message);
        return;
    }

    agent = clawt_config_get_agent(self->config, agent_id);
    if (agent != NULL &&
        !clawt_workspace_update_tools_org(self->config, agent, &error))
        g_message("venture mirror: %s's TOOLS.org was not rewritten: %s",
                  agent_id, error->message);
}

/* Every project's milestone and goal, for the poll to read. */
static void
mirror_sync_context(ClawtDaemon *self)
{
    g_autoptr(GPtrArray) projects = clawt_config_get_projects(self->config);
    g_autoptr(GArray) milestones = g_array_new(FALSE, FALSE, sizeof(gint64));
    g_autoptr(GArray) goals = g_array_new(FALSE, FALSE, sizeof(gint64));
    guint i;

    for (i = 0; projects != NULL && i < projects->len; i++) {
        ClawtProjectSpec *spec = g_ptr_array_index(projects, i);

        if (spec->venture_milestone_id > 0)
            g_array_append_val(milestones, spec->venture_milestone_id);
        if (spec->venture_goal_id > 0)
            g_array_append_val(goals, spec->venture_goal_id);
    }

    clawt_venture_mirror_set_context_ids(self->venture_mirror,
        (const gint64 *)(gpointer)milestones->data, milestones->len,
        (const gint64 *)(gpointer)goals->data, goals->len);
}

/* ── Which agents mirror ─────────────────────────────────────────── */

void
clawt_daemon_venture_mirror_sync(ClawtDaemon *self)
{
    GPtrArray *agents;
    g_autoptr(GPtrArray) catalog = NULL;
    g_autofree gchar *overlay_dir = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->venture_mirror == NULL || self->task_store == NULL)
        return;

    clawt_venture_mirror_clear_accounts(self->venture_mirror);

    overlay_dir = clawt_config_get_path_value(self->config, "connectors.dir");
    catalog = clawt_connector_catalog_load(overlay_dir, NULL);
    agents = clawt_config_get_agents(self->config);

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *agent = g_ptr_array_index(agents, i);
        g_autoptr(GPtrArray) bindings =
            clawt_integration_resolve_for_agent(self->config, agent);
        guint b;

        for (b = 0; bindings != NULL && b < bindings->len; b++) {
            ClawtIntegrationBinding *binding = g_ptr_array_index(bindings, b);
            const ClawtConnectorInfo *info;
            g_autofree gchar *base = NULL;
            g_autofree gchar *token = NULL;
            const gchar *agent_id;

            if (g_strcmp0(clawt_integration_binding_get_info(binding)->id,
                          "connector") != 0 ||
                !clawt_integration_binding_get_boolean(binding,
                                                       "mirror_tasks"))
                continue;

            info = clawt_connector_catalog_find(
                catalog,
                clawt_integration_binding_get_string(binding, "provider"));

            if (info == NULL || g_strcmp0(info->id, "venture") != 0)
                continue;

            agent_id = clawt_agent_config_get_id(agent);

            /* The first venture connector of an agent's is its account. */
            if (clawt_venture_mirror_has_account(self->venture_mirror,
                                                 agent_id))
                continue;

            base = clawt_connector_resolve_url(
                info, "",
                clawt_integration_binding_get_string(binding, "instance"));
            token = clawt_daemon_venture_token_for(binding);

            if (base == NULL || token == NULL) {
                g_message("venture mirror: %s's connector '%s' has no %s yet, "
                          "so its tasks are not mirrored", agent_id,
                          clawt_integration_binding_get_name(binding),
                          base == NULL ? "server" : "credential");
                continue;
            }

            while (g_str_has_suffix(base, "/"))
                base[strlen(base) - 1] = '\0';

            clawt_venture_mirror_set_account(
                self->venture_mirror, agent_id,
                clawt_integration_binding_get_name(binding), base, token);
            clawt_venture_mirror_set_account_intake(
                self->venture_mirror, agent_id,
                clawt_integration_binding_get_boolean(binding,
                                                      "mirror_intake"));
            memset(token, 0, strlen(token));

            /*
             * The operator's own account, once: the first mirroring
             * connector that names one.  Read here and nowhere near an
             * agent -- it is the operator's, not the agent's.
             */
            if (!clawt_venture_mirror_has_account(self->venture_mirror,
                                                  "user") &&
                clawt_integration_binding_get_string(binding,
                    "service_token_file") != NULL) {
                g_autofree gchar *path = clawt_expand_path(
                    clawt_integration_binding_get_string(binding,
                                                         "service_token_file"));
                g_autoptr(ClawtOauthToken) service =
                    clawt_oauth_token_load(path, NULL);

                if (service != NULL && service->access_token != NULL)
                    clawt_venture_mirror_set_account(
                        self->venture_mirror, "user",
                        clawt_integration_binding_get_name(binding), base,
                        service->access_token);
                else
                    g_message("venture mirror: the operator's token file %s "
                              "could not be read; the operator's tasks are "
                              "written as their assignee", path);
            }
        }
    }

    /*
     * The outbox is written only while somebody mirrors: a fleet with no
     * mirroring connector keeps no queue of changes nobody will send.
     */
    if (clawt_venture_mirror_account_count(self->venture_mirror) > 0) {
        mirror_sync_context(self);
        clawt_task_store_set_mirror(self->task_store, TRUE);
        clawt_venture_mirror_set_inbound(self->venture_mirror, &inbound, self,
                                         TRUE);
        clawt_venture_mirror_start(self->venture_mirror,
                                   MIRROR_INTERVAL_SECONDS);
    } else {
        clawt_task_store_set_mirror(self->task_store, FALSE);
        clawt_venture_mirror_stop(self->venture_mirror);
    }
}

/* ClawtTaskManager::task-changed: (task id, state). */
static void
on_task_changed_for_mirror(ClawtTaskManager *tasks, const gchar *task_id,
                           gint state, gpointer user_data)
{
    ClawtDaemon *self = user_data;

    (void)tasks;
    (void)task_id;
    (void)state;

    if (self->venture_mirror != NULL &&
        clawt_task_store_get_mirror(self->task_store))
        clawt_venture_mirror_kick(self->venture_mirror);
}

void
clawt_daemon_venture_mirror_start(ClawtDaemon *self)
{
    MirrorTransport *transport;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->task_store == NULL || self->venture_mirror != NULL)
        return;

    self->venture_mirror = clawt_venture_mirror_new(self->task_store,
                                                    self->main_context);

    transport = g_new0(MirrorTransport, 1);
    transport->daemon = self;
    transport->session = soup_session_new_with_options(
        "user-agent", USER_AGENT, "timeout", MIRROR_TIMEOUT_SECONDS, NULL);

    clawt_venture_mirror_set_request_func(self->venture_mirror, mirror_send,
                                          transport, mirror_transport_free);
    clawt_venture_mirror_set_target_func(self->venture_mirror, mirror_target,
                                         self, NULL);
    clawt_venture_mirror_set_cost_func(self->venture_mirror, mirror_cost,
                                       self, NULL);
    clawt_venture_mirror_set_context_func(self->venture_mirror,
                                          mirror_context, self, NULL);
    g_signal_connect(self->venture_mirror, "context-changed",
                     G_CALLBACK(on_mirror_context_changed), self);
    g_signal_connect(self->venture_mirror, "account-learned",
                     G_CALLBACK(on_mirror_account_learned), self);

    if (self->tasks != NULL)
        self->mirror_task_handler = g_signal_connect(
            self->tasks, "task-changed",
            G_CALLBACK(on_task_changed_for_mirror), self);

    /* Accounts now, requests on the first tick -- never on startup. */
    clawt_daemon_venture_mirror_sync(self);
}

void
clawt_daemon_venture_mirror_stop(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->mirror_task_handler != 0 && self->tasks != NULL)
        g_signal_handler_disconnect(self->tasks, self->mirror_task_handler);
    self->mirror_task_handler = 0;

    if (self->venture_mirror != NULL)
        clawt_venture_mirror_stop(self->venture_mirror);

    g_clear_object(&self->venture_mirror);
    g_clear_pointer(&self->mirror_remotes, g_hash_table_unref);
}

/* ── IPC ─────────────────────────────────────────────────────────── */

JsonNode *
clawt_daemon_venture_mirror_describe(ClawtDaemon *self, const gchar *task_id)
{
    g_return_val_if_fail(CLAWT_IS_DAEMON(self), NULL);

    if (self->task_store == NULL || task_id == NULL)
        return NULL;

    return clawt_venture_mirror_describe(self->task_store, task_id);
}

guint
clawt_daemon_venture_mirror_retry(ClawtDaemon *self, const gchar *task_id)
{
    guint revived;
    g_autoptr(ClawtVentureRef) ref = NULL;

    g_return_val_if_fail(CLAWT_IS_DAEMON(self), 0);

    if (self->task_store == NULL || task_id == NULL)
        return 0;

    revived = clawt_task_store_outbox_revive(self->task_store, task_id);

    /*
     * A task found unmatched, or whose ticket was lost, is looked at
     * afresh: a retry is the operator saying something has changed.
     */
    ref = clawt_task_store_venture_ref(self->task_store, task_id);

    if (ref == NULL || g_strcmp0(ref->state, "synced") != 0) {
        g_autoptr(ClawtTask) task = clawt_task_store_get(self->task_store,
                                                         task_id);

        if (task != NULL && clawt_task_store_get_mirror(self->task_store) &&
            clawt_task_store_save(self->task_store, task,
                                  "VENTURE mirror retried", NULL))
            revived++;
    }

    if (self->venture_mirror != NULL)
        clawt_venture_mirror_kick(self->venture_mirror);

    return revived;
}
