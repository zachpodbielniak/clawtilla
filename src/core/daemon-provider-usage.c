/*
 * daemon-provider-usage.c - usage.providers: each login's plan allowance
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A "login" is one CLI signed in one way: the default login of every CLI
 * an agent runs on without an account, plus every entry in `accounts:`.
 * Two agents on one login share one allowance, so the page lists logins,
 * not agents -- listing agents would show the same five-hour window
 * three times and invite somebody to add the three up.
 *
 * Reading a login spawns its CLI (ai-glib's report query), which takes
 * seconds and may reach the network, so the IPC handler never waits for
 * it: it answers from the cache at once, and when asked to refresh it
 * starts the queries behind the reply.  Each answer that lands is stored
 * and announced as `usage.providers_changed`; clients re-read on it.
 * Nothing is read at daemon start -- `make test` must not spawn a CLI --
 * and a client that never opens the page never costs a query.
 *
 * Every answer is also kept on disk (<state_dir>/usage/providers.json,
 * 0600), and a login the daemon has not asked since it started begins
 * from that, with the reading's own time.  The cache used to be memory
 * only, so a restart blanked every meter until somebody refreshed --
 * which spawns every login's CLI -- and a meter that knows its age is
 * worth more than one that knows nothing.
 */

#include "clawtilla.h"
#include "core/clawt-daemon-private.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <string.h>

/* A refresh asked for within this long of the last read is not repeated. */
#define USAGE_REFRESH_SECONDS (60)

struct _ClawtProviderUsageLogin {
    gchar        *id;
    gchar        *provider;      /* the libreclaw CLI type */
    gchar        *account;       /* NULL for the CLI's default login */
    gchar        *config_dir;    /* NULL for the default login */
    gchar        *description;
    gboolean      pay_per_use;
    gboolean      in_flight;
    const gchar  *state;         /* "idle", "pending", "ok", "unsupported", "error" */
    gchar        *message;
    gint64        read_at;       /* Unix seconds, 0 for never */
    gint64        asked_at;      /* Unix seconds of the last query started */
    JsonNode     *report;        /* clawt_provider_usage_normalize() */
    GCancellable *cancellable;
};

void
clawt_provider_usage_login_free(ClawtProviderUsageLogin *login)
{
    if (login == NULL)
        return;
    if (login->cancellable != NULL)
        g_cancellable_cancel(login->cancellable);
    g_clear_object(&login->cancellable);
    g_free(login->id);
    g_free(login->provider);
    g_free(login->account);
    g_free(login->config_dir);
    g_free(login->description);
    g_free(login->message);
    g_clear_pointer(&login->report, json_node_unref);
    g_free(login);
}

/* ── What was read before, on disk ──────────────────────────────── */

static gchar *
saved_path(ClawtDaemon *self)
{
    g_autofree gchar *state = clawt_config_get_path_value(self->config, "daemon.state_dir");

    return state != NULL ? g_build_filename(state, "usage", "providers.json", NULL) : NULL;
}

/* The saved answers, read once: login id -> {state, message, read_at, report}. */
static JsonObject *
saved(ClawtDaemon *self)
{
    JsonObject *object = g_object_get_data(G_OBJECT(self), "clawt-usage-saved");

    if (object == NULL) {
        g_autofree gchar *path = saved_path(self);
        g_autoptr(JsonParser) parser = json_parser_new();
        JsonNode *root;

        /* a damaged file is no readings, never a failure to start */
        if (path != NULL && json_parser_load_from_file(parser, path, NULL) &&
            (root = json_parser_get_root(parser)) != NULL && JSON_NODE_HOLDS_OBJECT(root))
            object = json_object_ref(json_node_get_object(root));
        else
            object = json_object_new();
        g_object_set_data_full(G_OBJECT(self), "clawt-usage-saved", object,
                               (GDestroyNotify)json_object_unref);
    }
    return object;
}

/* An answer worth keeping, kept: the file is rewritten whole, by rename. */
static void
save_login(ClawtDaemon *self, ClawtProviderUsageLogin *login)
{
    JsonObject *all = saved(self);
    JsonObject *entry = json_object_new();
    g_autofree gchar *path = saved_path(self);
    g_autofree gchar *dir = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) root = json_node_new(JSON_NODE_OBJECT);
    g_autoptr(GError) error = NULL;

    json_object_set_string_member(entry, "state", login->state);
    if (login->message != NULL)
        json_object_set_string_member(entry, "message", login->message);
    json_object_set_int_member(entry, "read_at", login->read_at);
    if (login->report != NULL)
        json_object_set_member(entry, "report", json_node_copy(login->report));
    json_object_set_object_member(all, login->id, entry);

    if (path == NULL)
        return;
    dir = g_path_get_dirname(path);
    json_node_set_object(root, all);
    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, NULL);
    if (g_mkdir_with_parents(dir, 0700) != 0 ||
        !g_file_set_contents_full(path, text, -1, G_FILE_SET_CONTENTS_CONSISTENT, 0600,
                                  &error))
        g_warning("usage: the reading for %s could not be kept: %s", login->id,
                  error != NULL ? error->message : g_strerror(errno));
}

/* A login made fresh in this daemon, started from what the last one read. */
static void
seed_login(ClawtDaemon *self, ClawtProviderUsageLogin *login)
{
    JsonObject *all = saved(self);
    JsonNode *node = json_object_get_member(all, login->id);
    JsonObject *entry;
    const gchar *state;

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return;
    entry = json_node_get_object(node);
    state = json_object_get_string_member_with_default(entry, "state", "");
    /* only answers: a stored "pending" would be a query nobody is running */
    if (g_strcmp0(state, "ok") == 0)
        login->state = "ok";
    else if (g_strcmp0(state, "unsupported") == 0)
        login->state = "unsupported";
    else
        return;
    login->read_at = json_object_get_int_member_with_default(entry, "read_at", 0);
    login->message = g_strdup(json_object_get_string_member_with_default(entry, "message",
                                                                         NULL));
    if (json_object_has_member(entry, "report"))
        login->report = json_node_copy(json_object_get_member(entry, "report"));
}

static GHashTable *
logins_table(ClawtDaemon *self)
{
    if (self->provider_usage == NULL)
        self->provider_usage = g_hash_table_new_full(
            g_str_hash, g_str_equal, NULL,
            (GDestroyNotify)clawt_provider_usage_login_free);
    return self->provider_usage;
}

/*
 * The fleet's logins, in a stable order: accounts as declared, then the
 * default login of each CLI some agent uses without one, in fleet order.
 * Existing cache entries are kept (with their last answer); a login no
 * longer anybody's is dropped, and its in-flight query cancelled with it.
 */
static GPtrArray *
collect_logins(ClawtDaemon *self)
{
    GHashTable *table = logins_table(self);
    g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GPtrArray) accounts = clawt_config_get_accounts(self->config);
    GPtrArray *agents = clawt_agent_manager_list(self->agents);  /* borrowed */
    GPtrArray *order = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key;
    guint i;

    for (i = 0; accounts != NULL && i < accounts->len; i++) {
        ClawtAccountSpec *spec = g_ptr_array_index(accounts, i);
        const gchar *cli = lc_provider_type_normalize(spec->provider, spec->id);
        g_autofree gchar *id = NULL;
        ClawtProviderUsageLogin *login;

        if (cli == NULL)
            continue;
        id = g_strdup_printf("%s:%s", cli, spec->id);
        login = g_hash_table_lookup(table, id);
        if (login == NULL) {
            login = g_new0(ClawtProviderUsageLogin, 1);
            login->id = g_steal_pointer(&id);
            login->state = "idle";
            seed_login(self, login);
            g_hash_table_insert(table, login->id, login);
        }
        /* the entry may have been edited since: its directory is today's */
        g_free(login->provider);
        login->provider = g_strdup(cli);
        g_free(login->account);
        login->account = g_strdup(spec->id);
        g_free(login->config_dir);
        login->config_dir = g_strdup(spec->config_dir);
        g_free(login->description);
        login->description = g_strdup(spec->description);
        login->pay_per_use = spec->pay_per_use;
        if (g_hash_table_add(seen, login->id))
            g_ptr_array_add(order, login);
    }

    for (i = 0; agents != NULL && i < agents->len; i++) {
        ClawtAgentConfig *config = clawt_agent_get_config(g_ptr_array_index(agents, i));
        const gchar *account;
        const gchar *cli;
        g_autofree gchar *id = NULL;
        ClawtProviderUsageLogin *login;

        if (config == NULL)
            continue;
        account = clawt_agent_config_get_string(config, "model.account");
        if (account != NULL && *account != '\0')
            continue;  /* listed above, under its account */
        cli = lc_provider_type_normalize(
            clawt_agent_config_get_string(config, "model.provider"), "agent");
        if (cli == NULL)
            continue;

        id = g_strdup_printf("%s:default", cli);
        login = g_hash_table_lookup(table, id);
        if (login == NULL) {
            login = g_new0(ClawtProviderUsageLogin, 1);
            login->id = g_steal_pointer(&id);
            login->provider = g_strdup(cli);
            login->state = "idle";
            seed_login(self, login);
            g_hash_table_insert(table, login->id, login);
        }
        if (g_hash_table_add(seen, login->id))
            g_ptr_array_add(order, login);
    }

    g_hash_table_iter_init(&iter, table);
    while (g_hash_table_iter_next(&iter, &key, NULL))
        if (!g_hash_table_contains(seen, key))
            g_hash_table_iter_remove(&iter);

    return order;
}

void
clawt_daemon_provider_usage_landed(ClawtDaemon  *self,
                                   const gchar  *login_id,
                                   JsonNode     *report,
                                   const GError *error)
{
    ClawtProviderUsageLogin *login;

    /* the daemon stopped, or the login went away, while the CLI answered */
    if (self->provider_usage == NULL ||
        (login = g_hash_table_lookup(self->provider_usage, login_id)) == NULL)
        return;
    if (error != NULL && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
        return;

    login->in_flight = FALSE;
    g_clear_object(&login->cancellable);
    g_clear_pointer(&login->message, g_free);

    if (error != NULL) {
        /*
         * "This login has no plan limits" (an API key, Bedrock, a CLI
         * with no reporting interface) is an answer, not a failure, and
         * must not read like one.  The previous good reading is kept on
         * a failure: a meter that blanks every time a CLI hiccups is
         * worse than one that says when it was last right.
         */
        login->state = g_error_matches(error, AI_ERROR, AI_ERROR_NOT_SUPPORTED)
                           ? "unsupported" : "error";
        login->message = g_strdup(error->message);
        /* an answer, so it counts as a read: not asked again every refresh */
        if (g_strcmp0(login->state, "unsupported") == 0) {
            g_clear_pointer(&login->report, json_node_unref);
            login->read_at = g_get_real_time() / G_USEC_PER_SEC;
        }
    } else {
        g_autoptr(JsonNode) normal = clawt_provider_usage_normalize(report);

        if (normal == NULL) {
            login->state = "error";
            login->message = g_strdup("the provider's report is in a format "
                                      "this clawtilla does not read");
        } else {
            login->state = "ok";
            g_clear_pointer(&login->report, json_node_unref);
            login->report = g_steal_pointer(&normal);
            login->read_at = g_get_real_time() / G_USEC_PER_SEC;
        }
    }

    /* answers are kept; a failure keeps the previous good reading, on disk too */
    if (g_strcmp0(login->state, "ok") == 0 || g_strcmp0(login->state, "unsupported") == 0)
        save_login(self, login);

    {
        g_autoptr(ClawtEvent) event = clawt_event_new("usage.providers_changed",
                                                      login->id);

        clawt_event_set_detail(event, "state", login->state);
        clawt_event_bus_publish(self->bus, event);
    }
}

typedef struct {
    ClawtDaemon *daemon;
    gchar       *login_id;
} QueryData;

static void
query_data_free(QueryData *data)
{
    g_object_unref(data->daemon);
    g_free(data->login_id);
    g_free(data);
}

static void
on_report(GObject *source, GAsyncResult *result, gpointer user_data)
{
    QueryData *data = user_data;
    g_autoptr(GError) error = NULL;
    g_autoptr(AiCliReport) report =
        ai_cli_client_query_report_finish(AI_CLI_CLIENT(source), result, &error);
    g_autoptr(JsonNode) document = report != NULL ? ai_cli_report_dup_data(report) : NULL;

    clawt_daemon_provider_usage_landed(data->daemon, data->login_id,
                                       document, error);
    query_data_free(data);
}

/* The real query: the login's own CLI, pointed at the login's directory. */
static void
query_with_cli(ClawtDaemon  *self,
               const gchar  *login_id,
               const gchar  *provider,
               const gchar  *config_dir,
               GCancellable *cancellable)
{
    g_autoptr(AiCliClient) client = lc_client_factory_new(provider, NULL, NULL,
                                                          NULL, FALSE);
    g_autoptr(GError) error = NULL;
    QueryData *data;

    if (client == NULL) {
        g_set_error(&error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "%s is not a CLI libreclaw runs", provider);
        clawt_daemon_provider_usage_landed(self, login_id, NULL, error);
        return;
    }
    if (config_dir != NULL && !ai_cli_client_set_config_dir(client, config_dir, &error)) {
        clawt_daemon_provider_usage_landed(self, login_id, NULL, error);
        return;
    }

    data = g_new0(QueryData, 1);
    data->daemon = g_object_ref(self);
    data->login_id = g_strdup(login_id);

    /*
     * The callback goes to the thread-default context at the call, so
     * push the daemon's own: under an embedded daemon the ambient one is
     * a loop nobody runs, and the answer would never land.
     */
    g_main_context_push_thread_default(self->main_context);
    ai_cli_client_query_report_async(client, AI_CLI_REPORT_USAGE, 20, cancellable,
                                     on_report, data);
    g_main_context_pop_thread_default(self->main_context);
}

static void
start_query(ClawtDaemon *self, ClawtProviderUsageLogin *login)
{
    g_autofree gchar *id = NULL;

    if (login->in_flight)
        return;
    login->in_flight = TRUE;
    login->state = "pending";
    login->asked_at = g_get_real_time() / G_USEC_PER_SEC;
    login->cancellable = g_cancellable_new();

    /* the query may land synchronously and must not find a borrowed id */
    id = g_strdup(login->id);
    if (self->provider_usage_query != NULL)
        self->provider_usage_query(self, id, login->provider, login->config_dir,
                                   login->cancellable);
    else
        query_with_cli(self, id, login->provider, login->config_dir,
                       login->cancellable);
}

static void
add_login(JsonBuilder *builder, ClawtProviderUsageLogin *login)
{
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, login->id);
    json_builder_set_member_name(builder, "provider");
    json_builder_add_string_value(builder, login->provider);
    if (login->account != NULL) {
        json_builder_set_member_name(builder, "account");
        json_builder_add_string_value(builder, login->account);
    }
    if (login->description != NULL) {
        json_builder_set_member_name(builder, "description");
        json_builder_add_string_value(builder, login->description);
    }
    json_builder_set_member_name(builder, "pay_per_use");
    json_builder_add_boolean_value(builder, login->pay_per_use);
    json_builder_set_member_name(builder, "state");
    json_builder_add_string_value(builder, login->state);
    if (login->message != NULL) {
        json_builder_set_member_name(builder, "message");
        json_builder_add_string_value(builder, login->message);
    }
    if (login->read_at > 0) {
        json_builder_set_member_name(builder, "read_at");
        json_builder_add_int_value(builder, login->read_at);
    }

    /* the report's own members, flattened in: plan, source, partial, windows */
    if (login->report != NULL && JSON_NODE_HOLDS_OBJECT(login->report)) {
        JsonObjectIter iter;
        const gchar *name;
        JsonNode *value;

        json_object_iter_init(&iter, json_node_get_object(login->report));
        while (json_object_iter_next(&iter, &name, &value)) {
            json_builder_set_member_name(builder, name);
            json_builder_add_value(builder, json_node_copy(value));
        }
    } else {
        json_builder_set_member_name(builder, "windows");
        json_builder_begin_array(builder);
        json_builder_end_array(builder);
    }
    json_builder_end_object(builder);
}

JsonNode *
clawt_daemon_handle_provider_usage(ClawtDaemon *self,
                                   const gchar *kind,
                                   JsonNode    *request,
                                   JsonObject  *payload,
                                   gboolean    *handled)
{
    g_autoptr(JsonBuilder) builder = NULL;
    g_autoptr(GPtrArray) logins = NULL;
    gboolean refresh;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    guint i;

    if (g_strcmp0(kind, "usage.providers") != 0) {
        *handled = FALSE;
        return NULL;
    }
    *handled = TRUE;

    refresh = clawt_ipc_payload_boolean(payload, "refresh", FALSE);
    logins = collect_logins(self);

    /* behind the reply, never during it: the caller gets what is known now */
    for (i = 0; refresh && i < logins->len; i++) {
        ClawtProviderUsageLogin *login = g_ptr_array_index(logins, i);

        /*
         * Throttled on the last *attempt*, not the last success: a CLI
         * that fails would otherwise be spawned again on every refresh,
         * and a page refreshes on every message.
         */
        if (login->asked_at == 0 || now - login->asked_at >= USAGE_REFRESH_SECONDS)
            start_query(self, login);
    }

    builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "now");
    json_builder_add_int_value(builder, now);
    json_builder_set_member_name(builder, "logins");
    json_builder_begin_array(builder);
    for (i = 0; i < logins->len; i++)
        add_login(builder, g_ptr_array_index(logins, i));
    json_builder_end_array(builder);
    json_builder_end_object(builder);

    return clawt_ipc_response_new(request, json_builder_get_root(builder));
}

void
clawt_daemon_provider_usage_stop(ClawtDaemon *self)
{
    /* each login's free cancels its query; the callbacks then find nothing */
    g_clear_pointer(&self->provider_usage, g_hash_table_unref);
}
