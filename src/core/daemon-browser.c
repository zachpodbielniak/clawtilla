/*
 * daemon-browser.c - The browser sessions a daemon holds, and the verbs
 *   that show them to a person
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The agent tools (clawt-mcp-tools.c) make sessions on first use; this
 * file owns the manager they share and the lifecycle around it:
 *
 * - a session stops after its idle minutes, checked on the idle tick;
 * - an agent's own session stops when the agent does -- a stopped agent
 *   holding a browser and an X server is what nobody would guess;
 * - every session stops with the daemon.
 *
 * And the IPC verbs the Computer page's Browser view and the CLI use:
 * `browser.list`, `browser.frame`, `browser.open`, `browser.click`,
 * `browser.stop`.  Each one that touches a browser is deferred and runs
 * on a worker: a frame waits on a page, and a start waits seconds for an
 * X server.  An IPC handler must not wait on any of that.
 */

#include "clawtilla.h"
#include "core/clawt-daemon.h"
#include "core/clawt-daemon-private.h"

#include <string.h>

/* ── Which session ────────────────────────────────────────────────── */

/* The session key for @agent_id, or NULL when it has no browser. */
static gchar *
key_for_agent(ClawtDaemon *self, const gchar *agent_id, ClawtAgentConfig **out_config)
{
    ClawtAgent *agent = agent_id != NULL ? clawt_agent_manager_get(self->agents, agent_id)
                                         : NULL;
    ClawtAgentConfig *config = agent != NULL ? clawt_agent_get_config(agent) : NULL;

    if (out_config != NULL)
        *out_config = config;
    if (config == NULL)
        return NULL;
    return clawt_browser_manager_session_key(
        (ClawtBrowserMode)clawt_agent_config_get_enum(config, "browser.mode"), agent_id,
        clawt_agent_config_get_string(config, "browser.use"));
}

/* The agent's session, configured from its keys; NULL with a reason. */
static ClawtBrowser *
session_for_agent(ClawtDaemon *self, const gchar *agent_id, GError **error)
{
    ClawtAgentConfig *config = NULL;
    g_autofree gchar *key = NULL;
    ClawtBrowser *browser;

    if (self->browsers == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "this daemon has no browser sessions");
        return NULL;
    }
    key = key_for_agent(self, agent_id, &config);
    if (config == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND, "no agent '%s'",
                    agent_id != NULL ? agent_id : "");
        return NULL;
    }
    if (key == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "%s has no browser (browser.mode is none)", agent_id);
        return NULL;
    }

    browser = clawt_browser_manager_obtain(self->browsers, key);
    clawt_browser_configure(browser, clawt_agent_config_get_string(config, "browser.home"),
                            (gint)clawt_agent_config_get_int(config, "browser.width"),
                            (gint)clawt_agent_config_get_int(config, "browser.height"),
                            (gint)clawt_agent_config_get_int(config, "browser.idle_minutes"));
    return browser;
}

/* ── Work off the main context ────────────────────────────────────── */

typedef enum {
    JOB_FRAME,
    JOB_OPEN,
    JOB_CLICK,
    JOB_STOP
} JobKind;

typedef struct {
    JobKind          kind;
    ClawtBrowser    *browser;
    ClawtIpcPending *pending;   /* NULL for a stop nobody waits on */
    gchar           *url;
    gint             x;
    gint             y;
    JsonNode        *payload;   /* the answer, built on the worker */
    GError          *error;
} Job;

static void
job_free(gpointer data)
{
    Job *job = data;

    g_clear_object(&job->browser);
    g_free(job->url);
    g_clear_pointer(&job->payload, json_node_unref);
    g_clear_error(&job->error);
    g_free(job);
}

/*
 * Whether a secret may be on the page, and the last one filled -- by the
 * entry's name and host, never by value.  The cached hold: this also
 * runs on the daemon's context (browser.list), which must not wait on a
 * page.
 */
static void
add_fill(JsonBuilder *builder, ClawtBrowser *browser)
{
    g_autofree gchar *name = NULL;
    g_autofree gchar *host = NULL;
    gint64 at = 0;

    json_builder_set_member_name(builder, "secret_hold");
    json_builder_add_boolean_value(builder, clawt_browser_get_secret_hold(browser));
    json_builder_set_member_name(builder, "last_fill");
    if (clawt_browser_dup_last_fill(browser, &name, &host, &at)) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
        json_builder_set_member_name(builder, "host");
        json_builder_add_string_value(builder, host != NULL ? host : "");
        json_builder_set_member_name(builder, "at");
        json_builder_add_int_value(builder, at);
        json_builder_end_object(builder);
    } else {
        json_builder_add_null_value(builder);
    }
}

/* The page as a client draws it; called on the worker. */
static void
add_page(JsonBuilder *builder, ClawtBrowser *browser)
{
    g_autofree gchar *uri = clawt_browser_dup_uri(browser);
    g_autofree gchar *title = clawt_browser_dup_title(browser);
    g_autofree gchar *driver = clawt_browser_dup_driver(browser,
                                                        g_get_real_time() / G_USEC_PER_SEC);

    json_builder_set_member_name(builder, "session");
    json_builder_add_string_value(builder, clawt_browser_get_id(browser));
    json_builder_set_member_name(builder, "running");
    json_builder_add_boolean_value(builder, clawt_browser_is_running(browser));
    json_builder_set_member_name(builder, "uri");
    json_builder_add_string_value(builder, uri != NULL ? uri : "");
    json_builder_set_member_name(builder, "title");
    json_builder_add_string_value(builder, title != NULL ? title : "");
    json_builder_set_member_name(builder, "driver");
    json_builder_add_string_value(builder, driver != NULL ? driver : "");
    add_fill(builder, browser);
}

static void
job_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable)
{
    Job *job = data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(GBytes) png = NULL;

    (void)source;
    (void)cancellable;

    switch (job->kind) {
    case JOB_STOP:
        clawt_browser_stop(job->browser);
        break;

    case JOB_OPEN: {
        g_autoptr(JsonNode) result = clawt_browser_navigate(job->browser, job->url, 20,
                                                            NULL, &job->error);

        (void)result;
        break;
    }

    case JOB_CLICK: {
        g_autoptr(JsonBuilder) params = json_builder_new();
        g_autoptr(JsonNode) node = NULL;
        g_autoptr(JsonNode) result = NULL;

        json_builder_begin_object(params);
        json_builder_set_member_name(params, "x");
        json_builder_add_int_value(params, job->x);
        json_builder_set_member_name(params, "y");
        json_builder_add_int_value(params, job->y);
        json_builder_end_object(params);
        node = json_builder_get_root(params);
        result = clawt_browser_call(job->browser, "page.click", node, 30, &job->error);
        if (result != NULL)
            clawt_browser_wait_loaded(job->browser, 5);
        break;
    }

    case JOB_FRAME:
        /*
         * A frame never starts a session: watching is not asking for one.
         * Nor is it taken while a secret is on the page -- gsurf would
         * refuse it anyway -- and that is not an error: the reply says
         * secret_hold, and the client draws that instead of a picture.
         */
        if (clawt_browser_is_running(job->browser) &&
            !clawt_browser_secret_held(job->browser))
            png = clawt_browser_screenshot(job->browser, &job->error);
        break;
    }

    if (job->error == NULL) {
        clawt_browser_refresh_page(job->browser);
        json_builder_begin_object(builder);
        add_page(builder, job->browser);
        if (png != NULL) {
            g_autofree gchar *encoded = g_base64_encode(g_bytes_get_data(png, NULL),
                                                        g_bytes_get_size(png));

            json_builder_set_member_name(builder, "png_base64");
            json_builder_add_string_value(builder, encoded);
        }
        json_builder_end_object(builder);
        job->payload = json_builder_get_root(builder);
    }

    g_task_return_boolean(task, TRUE);
}

/* On the daemon's context: answer whoever asked. */
static void
on_job_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Job *job = g_task_get_task_data(G_TASK(result));
    ClawtIpcPending *pending = g_steal_pointer(&job->pending);

    (void)source;
    (void)user_data;

    if (pending == NULL)
        return;
    if (job->error != NULL) {
        JsonNode *request = clawt_ipc_pending_get_request(pending);

        clawt_ipc_pending_respond(pending, clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                                               job->error->message));
        return;
    }
    clawt_ipc_pending_respond(pending,
                              clawt_ipc_response_new(clawt_ipc_pending_get_request(pending),
                                                     json_node_ref(job->payload)));
}

static void
run_job(ClawtDaemon *self, Job *job)
{
    g_autoptr(GTask) task = NULL;

    /* g_task_new() takes the thread-default; name the daemon's own. */
    if (self->main_context != NULL)
        g_main_context_push_thread_default(self->main_context);
    task = g_task_new(NULL, NULL, on_job_done, NULL);
    g_task_set_task_data(task, job, job_free);
    g_task_run_in_thread(task, job_thread);
    if (self->main_context != NULL)
        g_main_context_pop_thread_default(self->main_context);
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

static void
stop_in_background(ClawtDaemon *self, ClawtBrowser *browser)
{
    Job *job = g_new0(Job, 1);

    job->kind = JOB_STOP;
    job->browser = g_object_ref(browser);
    run_job(self, job);
}

/* An agent that stopped takes its own browser with it; a shared one waits
 * for its idle minutes, since another agent may be mid-task in it. */
static void
on_bus_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    ClawtDaemon *self = user_data;
    g_autofree gchar *key = NULL;
    g_autoptr(ClawtBrowser) browser = NULL;

    (void)bus;

    if (g_strcmp0(clawt_event_get_kind(event), "agent.stopped") != 0 ||
        clawt_event_get_subject(event) == NULL)
        return;

    /* A stopped agent has no turn left to spend a secret reference in. */
    if (self->mcp_tools != NULL)
        clawt_mcp_tools_forget_secret_refs(self->mcp_tools, clawt_event_get_subject(event));
    if (self->browsers == NULL)
        return;

    key = g_strdup_printf("agent:%s", clawt_event_get_subject(event));
    browser = clawt_browser_manager_lookup(self->browsers, key);
    if (browser != NULL && clawt_browser_is_running(browser))
        stop_in_background(self, browser);
}

void
clawt_daemon_browser_setup(ClawtDaemon *self)
{
    g_autofree gchar *root = NULL;
    g_autofree gchar *state_dir = NULL;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->browsers == NULL && self->config != NULL) {
        state_dir = clawt_config_get_path_value(self->config, "daemon.state_dir");
        if (state_dir != NULL) {
            root = g_build_filename(state_dir, "browsers", NULL);
            self->browsers = clawt_browser_manager_new(root);
        }
    }
    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_browsers(self->mcp_tools, self->browsers);
    if (self->bus != NULL && self->browser_bus_handler == 0)
        self->browser_bus_handler = g_signal_connect(self->bus, "event",
                                                     G_CALLBACK(on_bus_event), self);
}

void
clawt_daemon_browser_teardown(ClawtDaemon *self)
{
    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->browser_bus_handler != 0 && self->bus != NULL)
        g_signal_handler_disconnect(self->bus, self->browser_bus_handler);
    self->browser_bus_handler = 0;
    if (self->mcp_tools != NULL)
        clawt_mcp_tools_set_browsers(self->mcp_tools, NULL);

    /* Blocks a few seconds at most per session, and only at daemon stop. */
    if (self->browsers != NULL)
        clawt_browser_manager_stop_all(self->browsers);
    g_clear_object(&self->browsers);
}

void
clawt_daemon_browser_tick(ClawtDaemon *self, gint64 now)
{
    g_autoptr(GPtrArray) all = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_DAEMON(self));

    if (self->browsers == NULL)
        return;
    all = clawt_browser_manager_list(self->browsers);
    for (i = 0; i < all->len; i++) {
        ClawtBrowser *browser = g_ptr_array_index(all, i);

        if (clawt_browser_is_idle(browser, now)) {
            g_message("browser %s: idle; stopping it (browser.idle_minutes)",
                      clawt_browser_get_id(browser));
            stop_in_background(self, browser);
        }
    }
}

/* ── The verbs ────────────────────────────────────────────────────── */

/* One row of browser.list. */
static void
add_session(JsonBuilder *builder, ClawtDaemon *self, const gchar *key,
            GPtrArray *agent_ids)
{
    g_autoptr(ClawtBrowser) browser = self->browsers != NULL
        ? clawt_browser_manager_lookup(self->browsers, key) : NULL;
    const gchar *colon = strchr(key, ':');
    guint i;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "key");
    json_builder_add_string_value(builder, key);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, colon != NULL ? colon + 1 : key);
    json_builder_set_member_name(builder, "kind");
    json_builder_add_string_value(builder, g_str_has_prefix(key, "shared:") ? "shared"
                                                                            : "own");
    json_builder_set_member_name(builder, "agents");
    json_builder_begin_array(builder);
    for (i = 0; agent_ids != NULL && i < agent_ids->len; i++)
        json_builder_add_string_value(builder, g_ptr_array_index(agent_ids, i));
    json_builder_end_array(builder);

    if (browser != NULL) {
        add_page(builder, browser);
        json_builder_set_member_name(builder, "last_used");
        json_builder_add_int_value(builder, clawt_browser_get_last_used(browser));
    } else {
        json_builder_set_member_name(builder, "running");
        json_builder_add_boolean_value(builder, FALSE);
        json_builder_set_member_name(builder, "last_used");
        json_builder_add_int_value(builder, 0);
    }
    json_builder_end_object(builder);
}

/*
 * vault.list: the `vault:` entries by name, kind, description and sites,
 * and -- with `agent` -- which of them that agent may ask for.  Never a
 * value, and never the reference a value is resolved through: a client
 * managing grants needs to know an entry exists, not where it lives.
 */
static JsonNode *
vault_list(ClawtDaemon *self, JsonNode *request, JsonObject *payload)
{
    const gchar *agent_id = clawt_ipc_payload_string(payload, "agent");
    ClawtAgentConfig *config = NULL;
    g_autoptr(GPtrArray) entries = clawt_config_get_vault(self->config);
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_auto(GStrv) granted = NULL;
    guint i;

    if (agent_id != NULL && *agent_id != '\0') {
        config = clawt_config_get_agent(self->config, agent_id);
        if (config == NULL)
            return clawt_ipc_error_new(request, CLAWT_ERROR_NOT_FOUND, "no such agent");
        granted = clawt_agent_config_get_string_list(config, "browser.secrets");
    }

    json_builder_begin_object(builder);
    if (config != NULL) {
        json_builder_set_member_name(builder, "agent");
        json_builder_add_string_value(builder, agent_id);
        json_builder_set_member_name(builder, "browser");
        json_builder_add_boolean_value(builder,
                                       clawt_agent_config_get_enum(config, "browser.mode") !=
                                           CLAWT_BROWSER_NONE);
    }
    json_builder_set_member_name(builder, "entries");
    json_builder_begin_array(builder);
    for (i = 0; i < entries->len; i++) {
        ClawtVaultEntry *entry = g_ptr_array_index(entries, i);
        guint j;

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, entry->name);
        json_builder_set_member_name(builder, "kind");
        json_builder_add_string_value(builder,
                                      entry->kind == CLAWT_VAULT_TOTP ? "totp" : "password");
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder,
                                      entry->description != NULL ? entry->description : "");
        json_builder_set_member_name(builder, "sites");
        json_builder_begin_array(builder);
        for (j = 0; entry->sites != NULL && entry->sites[j] != NULL; j++)
            json_builder_add_string_value(builder, entry->sites[j]);
        json_builder_end_array(builder);
        if (config != NULL) {
            json_builder_set_member_name(builder, "granted");
            json_builder_add_boolean_value(builder,
                                           granted != NULL &&
                                           g_strv_contains((const gchar * const *)granted,
                                                           entry->name));
        }
        json_builder_end_object(builder);
    }
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

JsonNode *
clawt_daemon_handle_browser(ClawtDaemon *self, const gchar *kind, JsonNode *request,
                            JsonObject *payload, gboolean *handled)
{
    const gchar *agent_id;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtBrowser) browser = NULL;
    Job *job;

    *handled = TRUE;

    if (g_strcmp0(kind, "browser.list") == 0) {
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(GHashTable) users = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free, (GDestroyNotify)g_ptr_array_unref);
        g_autoptr(GPtrArray) keys = g_ptr_array_new_with_free_func(g_free);
        g_autoptr(GPtrArray) shared = clawt_config_get_browser_ids(self->config);
        GPtrArray *agents = clawt_agent_manager_list(self->agents);
        guint i;

        /* Every declared shared session, used or not, then each agent's. */
        for (i = 0; i < shared->len; i++) {
            gchar *key = g_strdup_printf("shared:%s", (gchar *)g_ptr_array_index(shared, i));

            g_hash_table_insert(users, g_strdup(key), g_ptr_array_new_with_free_func(g_free));
            g_ptr_array_add(keys, key);
        }
        for (i = 0; agents != NULL && i < agents->len; i++) {
            const gchar *id = clawt_agent_get_id(g_ptr_array_index(agents, i));
            g_autofree gchar *key = key_for_agent(self, id, NULL);
            GPtrArray *list;

            if (key == NULL)
                continue;
            list = g_hash_table_lookup(users, key);
            if (list == NULL) {
                list = g_ptr_array_new_with_free_func(g_free);
                g_hash_table_insert(users, g_strdup(key), list);
                g_ptr_array_add(keys, g_strdup(key));
            }
            g_ptr_array_add(list, g_strdup(id));
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "available");
        json_builder_add_boolean_value(builder, clawt_browser_build_available());
        json_builder_set_member_name(builder, "sessions");
        json_builder_begin_array(builder);
        for (i = 0; i < keys->len; i++)
            add_session(builder, self, g_ptr_array_index(keys, i),
                        g_hash_table_lookup(users, g_ptr_array_index(keys, i)));
        json_builder_end_array(builder);
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    if (g_strcmp0(kind, "vault.list") == 0)
        return vault_list(self, request, payload);

    if (!g_str_has_prefix(kind, "browser.")) {
        *handled = FALSE;
        return NULL;
    }

    agent_id = clawt_ipc_payload_string(payload, "agent");

    if (g_strcmp0(kind, "browser.frame") != 0 && g_strcmp0(kind, "browser.open") != 0 &&
        g_strcmp0(kind, "browser.click") != 0 && g_strcmp0(kind, "browser.stop") != 0) {
        *handled = FALSE;
        return NULL;
    }

    browser = session_for_agent(self, agent_id, &error);
    if (browser == NULL)
        return clawt_ipc_error_new(request, error->code, error->message);

    /* Not running: say so at once, rather than spend a thread on it. */
    if ((g_strcmp0(kind, "browser.frame") == 0 || g_strcmp0(kind, "browser.stop") == 0) &&
        !clawt_browser_is_running(browser)) {
        g_autoptr(JsonBuilder) builder = json_builder_new();

        json_builder_begin_object(builder);
        add_page(builder, browser);
        json_builder_end_object(builder);
        return clawt_ipc_response_new(request, json_builder_get_root(builder));
    }

    job = g_new0(Job, 1);
    job->browser = g_steal_pointer(&browser);

    if (g_strcmp0(kind, "browser.frame") == 0) {
        job->kind = JOB_FRAME;
    } else if (g_strcmp0(kind, "browser.stop") == 0) {
        job->kind = JOB_STOP;
    } else if (g_strcmp0(kind, "browser.open") == 0) {
        const gchar *url = clawt_ipc_payload_string(payload, "url");

        if (url == NULL || *url == '\0') {
            job_free(job);
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "browser.open needs a url");
        }
        job->kind = JOB_OPEN;
        job->url = strstr(url, "://") != NULL || g_str_has_prefix(url, "about:")
            ? g_strdup(url) : g_strconcat("https://", url, NULL);
    } else {
        job->kind = JOB_CLICK;
        job->x = (gint)clawt_ipc_payload_int(payload, "x", -1);
        job->y = (gint)clawt_ipc_payload_int(payload, "y", -1);
        if (job->x < 0 || job->y < 0) {
            job_free(job);
            return clawt_ipc_error_new(request, CLAWT_ERROR_INVALID_ARGUMENT,
                                       "browser.click needs x and y");
        }
    }

    job->pending = clawt_ipc_server_defer(self->ipc_server, request);
    if (job->pending == NULL) {
        job_free(job);
        return clawt_ipc_error_new(request, CLAWT_ERROR_FAILED,
                                   "this request cannot be answered later");
    }
    run_job(self, job);
    return NULL;
}
