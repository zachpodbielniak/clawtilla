/*
 * clawt-connection.c - Saved ways of reaching a daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "ipc/clawt-connection.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <yaml-glib.h>

struct _ClawtConnection {
    gchar   *name;
    gchar   *socket_path;   /* set for a local connection */
    gchar   *host;          /* set for a remote one */
    guint16  port;
    gchar   *token;
    gchar   *token_file;    /* where token was read from, if a file */
    gboolean tls;
    gboolean accept_unknown_certificate;
};

G_DEFINE_BOXED_TYPE(ClawtConnection, clawt_connection, clawt_connection_copy,
                    clawt_connection_free)

ClawtConnection *
clawt_connection_new_local(const gchar *name, const gchar *socket_path)
{
    ClawtConnection *self = g_new0(ClawtConnection, 1);

    self->name = g_strdup(name != NULL ? name : "Local");
    self->socket_path = g_strdup(socket_path);

    return self;
}

ClawtConnection *
clawt_connection_new_remote(const gchar *name, const gchar *host,
                            guint16 port, const gchar *token)
{
    ClawtConnection *self = g_new0(ClawtConnection, 1);

    self->name = g_strdup(name != NULL ? name : host);
    self->host = g_strdup(host);
    self->port = port;
    self->token = g_strdup(token);

    return self;
}

ClawtConnection *
clawt_connection_copy(ClawtConnection *self)
{
    ClawtConnection *copy;

    g_return_val_if_fail(self != NULL, NULL);

    copy = g_new0(ClawtConnection, 1);
    copy->name = g_strdup(self->name);
    copy->socket_path = g_strdup(self->socket_path);
    copy->host = g_strdup(self->host);
    copy->port = self->port;
    copy->token = g_strdup(self->token);
    copy->token_file = g_strdup(self->token_file);
    copy->tls = self->tls;
    copy->accept_unknown_certificate = self->accept_unknown_certificate;

    return copy;
}

void
clawt_connection_free(ClawtConnection *self)
{
    if (self == NULL)
        return;

    g_free(self->name);
    g_free(self->socket_path);
    g_free(self->host);
    g_free(self->token_file);

    /*
     * Wiped rather than merely freed.  This is the one long-lived copy of
     * a bearer token in the client's address space, and the client stays
     * running for hours; leaving it in a freed block is how it ends up in
     * a core dump attached to a bug report.
     */
    if (self->token != NULL) {
        memset(self->token, 0, strlen(self->token));
        g_free(self->token);
    }

    g_free(self);
}

const gchar *
clawt_connection_get_name(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->name;
}

gboolean
clawt_connection_is_local(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->host == NULL || *self->host == '\0';
}

const gchar *
clawt_connection_get_socket_path(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->socket_path;
}

const gchar *
clawt_connection_get_host(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->host;
}

guint16
clawt_connection_get_port(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, 0);

    return self->port;
}

const gchar *
clawt_connection_get_token(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->token;
}

gboolean
clawt_connection_get_tls(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->tls;
}

gboolean
clawt_connection_get_accept_unknown_certificate(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, FALSE);

    return self->accept_unknown_certificate;
}

void
clawt_connection_set_name(ClawtConnection *self, const gchar *name)
{
    g_return_if_fail(self != NULL);

    g_free(self->name);
    self->name = g_strdup(name);
}

void
clawt_connection_set_tls(ClawtConnection *self, gboolean enabled,
                         gboolean accept_unknown_certificate)
{
    g_return_if_fail(self != NULL);

    self->tls = enabled;
    self->accept_unknown_certificate = accept_unknown_certificate;
}

gchar *
clawt_connection_describe(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    if (clawt_connection_is_local(self)) {
        if (self->socket_path != NULL && *self->socket_path != '\0')
            return g_strdup(self->socket_path);

        return g_strdup("this machine");
    }

    return g_strdup_printf("%s:%u%s", self->host, self->port,
                           self->tls ? " (TLS)" : "");
}

ClawtClient *
clawt_connection_create_client(ClawtConnection *self)
{
    ClawtClient *client;

    g_return_val_if_fail(self != NULL, NULL);

    if (clawt_connection_is_local(self))
        return clawt_client_new(self->socket_path);

    client = clawt_client_new_tcp(self->host, self->port, self->token);
    clawt_client_set_tls(client, self->tls,
                         self->accept_unknown_certificate);

    return client;
}

void
clawt_connection_set_token(ClawtConnection *self, const gchar *token)
{
    g_return_if_fail(self != NULL);

    /* wiped, for the reason clawt_connection_free() gives */
    if (self->token != NULL)
        memset(self->token, 0, strlen(self->token));

    g_free(self->token);
    self->token = g_strdup(token);
}

const gchar *
clawt_connection_get_token_file(ClawtConnection *self)
{
    g_return_val_if_fail(self != NULL, NULL);

    return self->token_file;
}

gboolean
clawt_connection_load_token_file(ClawtConnection  *self,
                                 const gchar      *path,
                                 GError          **error)
{
    g_autofree gchar *resolved = NULL;
    gchar *text = NULL;
    gsize length = 0;

    g_return_val_if_fail(self != NULL, FALSE);
    g_return_val_if_fail(path != NULL, FALSE);

    resolved = clawt_expand_path(path);

    if (!g_file_get_contents(resolved, &text, &length, error)) {
        g_prefix_error(error, "--token-file: ");
        return FALSE;
    }

    /*
     * Trimmed in place, so the only copy left is the connection's own:
     * the buffer read is wiped below whichever way this goes, and a
     * token file written by `echo` ends in a newline the daemon would
     * otherwise be asked to match.
     */
    g_strstrip(text);

    if (*text == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "--token-file: %s is empty", resolved);
        memset(text, 0, length);
        g_free(text);
        return FALSE;
    }

    clawt_connection_set_token(self, text);
    g_free(self->token_file);
    self->token_file = g_steal_pointer(&resolved);

    memset(text, 0, length);
    g_free(text);

    return TRUE;
}

/*
 * The exchange itself, on whatever context is thread-default -- which
 * clawt_connection_pair() has just made a private one.
 */
static gboolean
pair_on_this_context(ClawtConnection *self, const gchar *code,
                     const gchar *device_name, GError **error)
{
    g_autoptr(ClawtClient) client = NULL;
    const gchar *token;

    client = clawt_connection_create_client(self);

    /*
     * Off: a pairing that reconnected in the background would try a
     * code that the first attempt already spent.
     */
    clawt_client_set_auto_reconnect(client, FALSE);
    clawt_client_set_pairing_code(client, code, device_name);

    if (!clawt_client_connect(client, error))
        return FALSE;

    token = clawt_client_get_token(client);

    if (token == NULL || *token == '\0') {
        clawt_client_disconnect(client);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                    "the daemon accepted the code but sent no token back; "
                    "is it older than this client?");
        return FALSE;
    }

    clawt_connection_set_token(self, token);
    clawt_client_disconnect(client);

    return TRUE;
}

gboolean
clawt_connection_pair(ClawtConnection *self, const gchar *code,
                      const gchar *device_name, GError **error)
{
    g_autoptr(GMainContext) context = NULL;
    g_autofree gchar *digits = NULL;
    gboolean paired;

    g_return_val_if_fail(self != NULL, FALSE);

    /*
     * A daemon on this machine already trusts this user completely; a
     * token for it would be a credential that opens nothing new.
     */
    if (clawt_connection_is_local(self)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "a daemon on this machine needs no pairing -- its "
                    "socket already lets you in");
        return FALSE;
    }

    digits = clawt_device_code_normalize(code);

    if (digits == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "a pairing code is %d digits, like 123 456",
                    CLAWT_DEVICE_CODE_DIGITS);
        return FALSE;
    }

    if (device_name == NULL || *device_name == '\0')
        device_name = g_get_host_name();

    context = g_main_context_new();
    g_main_context_push_thread_default(context);

    paired = pair_on_this_context(self, digits, device_name, error);

    /* Drained before it is dropped, for clawt_connection_probe()'s reason. */
    while (g_main_context_iteration(context, FALSE))
        ;

    g_main_context_pop_thread_default(context);

    return paired;
}

ClawtConnection *
clawt_connection_new_paired(const gchar *name, const gchar *host,
                            guint16 port, gboolean tls,
                            gboolean accept_unknown_certificate,
                            const gchar *code, const gchar *device_name,
                            GError **error)
{
    g_autoptr(ClawtConnection) connection = NULL;

    if (host == NULL || *host == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "pairing needs the daemon's host -- the address "
                    "`clawtilla device pair` printed beside the code");
        return NULL;
    }

    connection = clawt_connection_new_remote(
        (name != NULL && *name != '\0') ? name : host, host,
        port != 0 ? port : CLAWT_DEFAULT_TCP_PORT, NULL);
    clawt_connection_set_tls(connection, tls, accept_unknown_certificate);

    if (!clawt_connection_pair(connection, code, device_name, error))
        return NULL;

    return g_steal_pointer(&connection);
}

gchar *
clawt_connection_list_default_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "clawtilla",
                            "connections.yaml", NULL);
}

/* ── Reading ─────────────────────────────────────────────────────── */

static const gchar *
member_string(YamlMapping *mapping, const gchar *key)
{
    YamlNode *node;

    if (mapping == NULL || key == NULL)
        return NULL;

    node = yaml_mapping_get_member(mapping, key);

    if (node == NULL || yaml_node_get_node_type(node) == YAML_NODE_NULL)
        return NULL;

    return yaml_node_get_string(node);
}

static gboolean
member_boolean(YamlMapping *mapping, const gchar *key)
{
    const gchar *text = member_string(mapping, key);

    return g_strcmp0(text, "true") == 0 || g_strcmp0(text, "yes") == 0;
}

GPtrArray *
clawt_connection_list_parse(const gchar *text, GError **error)
{
    g_autoptr(YamlParser) parser = NULL;
    g_autoptr(GPtrArray) connections = NULL;
    YamlNode *root;
    YamlNode *list;
    YamlSequence *sequence;
    gsize i;
    gsize length;

    g_return_val_if_fail(text != NULL, NULL);

    connections = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_connection_free);

    /* An empty file is an empty list, not a parse error. */
    if (*text == '\0')
        return g_steal_pointer(&connections);

    parser = yaml_parser_new();

    if (!yaml_parser_load_from_data(parser, text, -1, error))
        return NULL;

    root = yaml_parser_get_root(parser);

    if (root == NULL || yaml_node_get_node_type(root) != YAML_NODE_MAPPING)
        return g_steal_pointer(&connections);

    list = yaml_mapping_get_member(yaml_node_get_mapping(root), "connections");

    if (list == NULL || yaml_node_get_node_type(list) != YAML_NODE_SEQUENCE)
        return g_steal_pointer(&connections);

    sequence = yaml_node_get_sequence(list);
    length = yaml_sequence_get_length(sequence);

    for (i = 0; i < length; i++) {
        YamlNode *element = yaml_sequence_get_element(sequence, i);
        YamlMapping *entry;
        ClawtConnection *connection;
        const gchar *host;
        const gchar *port_text;

        if (element == NULL ||
            yaml_node_get_node_type(element) != YAML_NODE_MAPPING)
            continue;

        entry = yaml_node_get_mapping(element);
        host = member_string(entry, "host");

        if (host != NULL && *host != '\0') {
            gint64 port = 0;

            port_text = member_string(entry, "port");

            if (port_text != NULL)
                port = g_ascii_strtoll(port_text, NULL, 10);

            /*
             * A profile with no usable port is skipped rather than bound
             * to a default: connecting to the wrong port produces a
             * refusal that reads as "the daemon is not running there",
             * which sends a person to the wrong machine.
             */
            if (port <= 0 || port > G_MAXUINT16)
                continue;

            connection = clawt_connection_new_remote(
                member_string(entry, "name"), host, (guint16)port,
                member_string(entry, "token"));

            clawt_connection_set_tls(connection, member_boolean(entry, "tls"),
                                     member_boolean(
                                         entry, "accept_unknown_certificate"));
        } else {
            connection = clawt_connection_new_local(
                member_string(entry, "name"), member_string(entry, "socket"));
        }

        g_ptr_array_add(connections, connection);
    }

    return g_steal_pointer(&connections);
}

/* ── Writing ─────────────────────────────────────────────────────── */

/*
 * Quoted with single quotes, which in YAML have no escapes at all beyond
 * a doubled quote -- so a token containing a backslash survives, where a
 * double-quoted string would have to escape it and a round trip would
 * have to unescape it exactly right.
 */
static void
append_quoted(GString *out, const gchar *key, const gchar *value)
{
    const gchar *p;

    g_string_append_printf(out, "    %s: '", key);

    for (p = value; *p != '\0'; p++) {
        if (*p == '\'')
            g_string_append(out, "''");
        else
            g_string_append_c(out, *p);
    }

    g_string_append(out, "'\n");
}

gchar *
clawt_connection_list_to_data(GPtrArray *connections)
{
    GString *out = g_string_new(NULL);
    guint i;

    g_string_append(
        out,
        "# clawtilla connection profiles\n"
        "#\n"
        "# Where the clients look for daemons other than the local one.\n"
        "# Written by `clawtilla remote add` and by the connection menu in\n"
        "# clawtilla-gtk; edit it by hand if you prefer.\n"
        "#\n"
        "# This file holds bearer tokens, so it is kept at mode 0600.\n"
        "# `clawtilla daemon token` prints the one a given daemon expects.\n"
        "\n"
        "connections:\n");

    if (connections == NULL || connections->len == 0)
        g_string_append(out, "  []\n");

    for (i = 0; connections != NULL && i < connections->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(connections, i);

        g_string_append(out, "  -\n");
        append_quoted(out, "name", connection->name != NULL
                                       ? connection->name : "");

        if (clawt_connection_is_local(connection)) {
            if (connection->socket_path != NULL &&
                *connection->socket_path != '\0')
                append_quoted(out, "socket", connection->socket_path);

            continue;
        }

        append_quoted(out, "host", connection->host);
        g_string_append_printf(out, "    port: %u\n", connection->port);

        if (connection->token != NULL && *connection->token != '\0')
            append_quoted(out, "token", connection->token);

        if (connection->tls) {
            g_string_append(out, "    tls: true\n");

            /*
             * Only written when it is on, so its absence is never
             * mistaken for a considered "no".  It disables the check that
             * would notice somebody else answering on that address.
             */
            if (connection->accept_unknown_certificate)
                g_string_append(out,
                                "    accept_unknown_certificate: true\n");
        }
    }

    return g_string_free(out, FALSE);
}

GPtrArray *
clawt_connection_list_load(const gchar *path, GError **error)
{
    g_autofree gchar *resolved = NULL;
    g_autofree gchar *text = NULL;

    resolved = path != NULL ? clawt_expand_path(path)
                            : clawt_connection_list_default_path();

    if (!g_file_test(resolved, G_FILE_TEST_EXISTS))
        return g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_connection_free);

    if (!g_file_get_contents(resolved, &text, NULL, error)) {
        g_prefix_error(error, "%s: ", resolved);
        return NULL;
    }

    return clawt_connection_list_parse(text, error);
}

gboolean
clawt_connection_list_save(const gchar *path, GPtrArray *connections,
                           GError **error)
{
    g_autofree gchar *resolved = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *text = NULL;

    resolved = path != NULL ? clawt_expand_path(path)
                            : clawt_connection_list_default_path();
    dir = g_path_get_dirname(resolved);

    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;

    text = clawt_connection_list_to_data(connections);

    return clawt_write_file_atomic(resolved, text, -1, 0600, FALSE, error);
}

ClawtConnection *
clawt_connection_list_find(GPtrArray *connections, const gchar *name)
{
    guint i;

    g_return_val_if_fail(name != NULL, NULL);

    for (i = 0; connections != NULL && i < connections->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(connections, i);

        if (g_strcmp0(connection->name, name) == 0)
            return connection;
    }

    return NULL;
}

ClawtReachability
clawt_reachability_from_error(const GError *error)
{
    if (error == NULL)
        return CLAWT_REACH_REACHABLE;

    /*
     * Only the daemon's own authentication refusal is a credential
     * problem. G_IO_ERROR_CONNECTION_REFUSED means nothing was
     * listening, which is the network, and reading it as a bad token
     * would send somebody to rotate a credential that was never seen.
     */
    if (error->domain == CLAWT_ERROR && error->code == CLAWT_ERROR_AUTH)
        return CLAWT_REACH_REFUSED;

    return CLAWT_REACH_UNREACHABLE;
}

const gchar *
clawt_reachability_word(ClawtReachability reach)
{
    switch (reach) {
    case CLAWT_REACH_REACHABLE:
        return "reachable";
    case CLAWT_REACH_REFUSED:
        return "refused";
    case CLAWT_REACH_UNREACHABLE:
        return "unreachable";
    case CLAWT_REACH_UNKNOWN:
    default:
        return "not checked";
    }
}

void
clawt_connection_status_free(ClawtConnectionStatus *self)
{
    if (self == NULL)
        return;

    g_free(self->version);
    g_free(self->detail);
    g_free(self);
}

static ClawtConnectionStatus *
probe_on_this_context(ClawtConnection *self)
{
    ClawtConnectionStatus *status = g_new0(ClawtConnectionStatus, 1);
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GError) error = NULL;

    client = clawt_connection_create_client(self);

    if (client == NULL) {
        status->reach = CLAWT_REACH_UNREACHABLE;
        status->detail = g_strdup("that connection could not be opened");
        return status;
    }

    /*
     * Explicitly off. A probe that reconnected in the background would
     * leave a client alive for a connection nobody switched to, quietly
     * dialling a host somebody only hovered over in a menu.
     */
    clawt_client_set_auto_reconnect(client, FALSE);

    if (!clawt_client_connect(client, &error)) {
        status->reach = clawt_reachability_from_error(error);
        status->detail = g_strdup(error->message);
        return status;
    }

    reply = clawt_client_request(client, "control.status", NULL, &error);

    if (reply == NULL) {
        status->reach = clawt_reachability_from_error(error);
        status->detail = g_strdup(error->message);
        clawt_client_disconnect(client);
        return status;
    }

    status->reach = CLAWT_REACH_REACHABLE;

    if (JSON_NODE_HOLDS_OBJECT(reply)) {
        JsonObject *payload = json_node_get_object(reply);

        if (json_object_has_member(payload, "version"))
            status->version =
                g_strdup(json_object_get_string_member(payload, "version"));

        if (json_object_has_member(payload, "agents"))
            status->agents =
                (guint)json_object_get_int_member(payload, "agents");
    }

    clawt_client_disconnect(client);

    return status;
}

/*
 * On a context this thread owns, whichever thread that is.
 *
 * A ClawtClient arms its reader and waits for its replies on the
 * thread-default context at the moment it connects, and a fresh worker
 * thread has none -- so it settled on the *global* default, the one the
 * GTK client's main loop is running.  Pushing that from the worker made
 * g_main_context_acquire() fail, which is a pair of GLib criticals, and
 * then g_main_context_iteration() sat waiting for the main thread to let
 * go of it and dispatched that thread's sources -- GTK's among them --
 * off the main thread while it had it.
 *
 * Both callers run this on a thread precisely because it blocks: the GTK
 * client from a GTask worker, the web client from a request handler.  A
 * private context is what makes that safe, and it is no worse on the
 * main thread: a probe must not pump the application's loop while it
 * waits, which is how a menu re-enters the code that is drawing it.
 */
ClawtConnectionStatus *
clawt_connection_probe(ClawtConnection *self)
{
    g_autoptr(GMainContext) context = NULL;
    ClawtConnectionStatus *status;

    g_return_val_if_fail(self != NULL, g_new0(ClawtConnectionStatus, 1));

    context = g_main_context_new();
    g_main_context_push_thread_default(context);

    status = probe_on_this_context(self);

    /*
     * Drained before it is dropped.  Disconnecting only *closes* the
     * stream: the reader parked in read_line_async() still holds its
     * references -- the input stream, its buffer, the client -- and
     * releases them when its callback is dispatched, which takes one
     * more turn of this context.  Skipping that stranded the whole
     * reader on a context nobody would ever iterate again: one leaked
     * client per probe, and the connection menu probes on hover.  The
     * dispatched callback finds the input already cleared and stops, so
     * this settles rather than spinning.
     */
    while (g_main_context_iteration(context, FALSE))
        ;

    g_main_context_pop_thread_default(context);

    return status;
}


/* ── How a daemon's version stands against ours ──────────────────── */

/*
 * Three numbers out of "0.1.0", or FALSE.
 *
 * A missing micro is 0 rather than a refusal: "0.2" is a version, and
 * treating it as unreadable would report a mismatch as an unknown.
 */
static gboolean
parse_version(const gchar *text, gint64 *out)
{
    g_auto(GStrv) parts = NULL;
    guint count;
    guint i;

    if (text == NULL || *text == '\0')
        return FALSE;

    parts = g_strsplit(text, ".", -1);
    count = g_strv_length(parts);

    /*
     * Two or three components, and split with no limit.  Splitting into
     * at most four leaves everything past the third in parts[3], which
     * the loop below never reads -- so "0.1.0.0.0" parsed as 0.1.0 and
     * came back *equal* to this build.  A version we cannot place has to
     * say so; quietly agreeing with it is the one answer that stops
     * anybody looking.
     */
    if (count < 2 || count > 3)
        return FALSE;

    for (i = 0; i < 3; i++) {
        gchar *end = NULL;

        if (parts[i] == NULL) {
            out[i] = 0;
            continue;
        }

        out[i] = g_ascii_strtoll(parts[i], &end, 10);

        /*
         * The whole component has to be a number.  "0.1.0-dirty" parses
         * its micro as 0 and stops at the dash, which would silently
         * call a development build equal to the release it came after.
         */
        if (end == parts[i] || (end != NULL && *end != '\0'))
            return FALSE;
    }

    return TRUE;
}

ClawtVersionVerdict
clawt_version_compare(const gchar *daemon_version,
                      const gchar *client_version)
{
    gint64 theirs[3] = { 0, 0, 0 };
    gint64 ours[3] = { 0, 0, 0 };
    guint i;

    if (!parse_version(daemon_version, theirs))
        return CLAWT_VERSION_UNKNOWN;

    /*
     * Ours is a compile-time constant, so a failure to parse it is a
     * broken build rather than a daemon we cannot place -- but answering
     * UNKNOWN is still the right thing, because asserting about the
     * other end on the strength of a version string we cannot read
     * ourselves would be worse.
     */
    if (!parse_version(client_version, ours))
        return CLAWT_VERSION_UNKNOWN;

    for (i = 0; i < 3; i++) {
        if (theirs[i] == ours[i])
            continue;

        return (theirs[i] > ours[i]) ? CLAWT_VERSION_DAEMON_NEWER
                                     : CLAWT_VERSION_DAEMON_OLDER;
    }

    return CLAWT_VERSION_SAME;
}

ClawtVersionVerdict
clawt_version_compare_to_client(const gchar *daemon_version)
{
    return clawt_version_compare(daemon_version, CLAWT_VERSION_STRING);
}

gchar *
clawt_version_mismatch_text(const gchar *daemon_version)
{
    switch (clawt_version_compare_to_client(daemon_version)) {
    case CLAWT_VERSION_DAEMON_NEWER:
        return g_strdup_printf(
            "This daemon is clawtilla %s and this client is %s. The daemon "
            "is newer, so it may offer things this client cannot ask for; "
            "update the client.",
            daemon_version, CLAWT_VERSION_STRING);

    case CLAWT_VERSION_DAEMON_OLDER:
        return g_strdup_printf(
            "This daemon is clawtilla %s and this client is %s. The daemon "
            "is older, so it may refuse something this client sends; update "
            "the daemon.",
            daemon_version, CLAWT_VERSION_STRING);

    case CLAWT_VERSION_SAME:
    case CLAWT_VERSION_UNKNOWN:
    default:
        /*
         * Nothing for either.  A client that announced "this daemon's
         * version is unreadable" on every connect to a daemon that
         * simply predates the field would be crying wolf at the case
         * this exists to make quiet.
         */
        return NULL;
    }
}

ClawtDaemonLink
clawt_daemon_link_state(ClawtClient *client, gboolean reached_once)
{
    if (client != NULL && clawt_client_is_connected(client))
        return CLAWT_DAEMON_LINK_UP;

    /*
     * Never outranks lost even when a retry is scheduled.  A client that
     * has never reached its daemon is retrying too, so the retry cannot
     * be what distinguishes them; only whether it ever got there can.
     */
    if (!reached_once)
        return CLAWT_DAEMON_LINK_NEVER;

    return CLAWT_DAEMON_LINK_LOST;
}

gboolean
clawt_connection_offer_local(ClawtDaemonLink        link,
                             const ClawtConnection *connection,
                             gboolean               remembered)
{
    /*
     * All three, and each one rules out a case where the button would
     * be wrong.  Never rather than lost: a daemon that answered once
     * this session is a blip somebody is waiting out, not a choice that
     * went stale.  Remembered rather than asked for: somebody who typed
     * --host or --profile meant that machine and is not helped by an
     * offer to undo what they typed.  Not local: Local cannot be offered
     * as the way out of Local.
     */
    if (link != CLAWT_DAEMON_LINK_NEVER || !remembered || connection == NULL)
        return FALSE;

    return !clawt_connection_is_local((ClawtConnection *)connection);
}

gchar *
clawt_connection_notice_text(ClawtDaemonLink        link,
                             const ClawtConnection *connection,
                             gboolean               remembered,
                             const gchar           *daemon_version,
                             const gchar           *available_update)
{
    const gchar *name = (connection != NULL)
                        ? clawt_connection_get_name(
                              (ClawtConnection *)connection)
                        : "the daemon";

    switch (link) {
    case CLAWT_DAEMON_LINK_NEVER:
        if (clawt_connection_offer_local(link, connection, remembered)) {
            g_autofree gchar *where =
                clawt_connection_describe((ClawtConnection *)connection);

            /*
             * Said, because nobody asked for this machine this time:
             * the client chose it from what it was last switched to,
             * and somebody who has forgotten that is looking at an
             * empty window with no idea why it is pointed elsewhere.
             * The clients draw the Use Local button beside it, from
             * the same clawt_connection_offer_local().
             */
            return g_strdup_printf("Not connected to %s (%s), the daemon "
                                   "this client was last on. Still trying; "
                                   "use Local, or choose another "
                                   "connection.", name, where);
        }

        if (connection != NULL &&
            !clawt_connection_is_local((ClawtConnection *)connection)) {
            g_autofree gchar *where =
                clawt_connection_describe((ClawtConnection *)connection);

            return g_strdup_printf("Not connected to %s (%s). Still trying; "
                                   "choose another connection to switch.",
                                   name, where);
        }

        /*
         * The local case is the one somebody hits by opening the
         * application from a menu, so it names the command that fixes
         * it -- and offers the other machines in the same breath,
         * because a client whose whole point is reaching daemons
         * elsewhere should not read as broken when the least important
         * one is down.
         */
        return g_strdup_printf("Not connected to %s. Start clawtillad, or "
                               "choose another connection.", name);

    case CLAWT_DAEMON_LINK_LOST:
        return g_strdup_printf("Lost the connection to %s. Trying again -- "
                               "what is shown is from before it went.", name);

    case CLAWT_DAEMON_LINK_UP:
    default:
        {
            gchar *mismatch = clawt_version_mismatch_text(daemon_version);

            /*
             * A client and a daemon that disagree is broken now; an
             * update is better later.  Saying the second over the first
             * would answer the wrong question, so the mismatch wins and
             * the update waits until there is nothing else to say.
             */
            if (mismatch != NULL)
                return mismatch;
        }

        if (available_update != NULL && *available_update != '\0')
            return g_strdup_printf(
                "clawtilla %s is available. Nothing has been installed -- "
                "update when it suits you.", available_update);

        return NULL;
    }
}

/* ── The daemon a client was last on ───────────────────────────────── */

gchar *
clawt_connection_last_default_path(void)
{
    /*
     * State, not configuration: it is what a client did last, not
     * anything a person wrote, and keeping it out of connections.yaml
     * means saving the profiles can never drop it, nor it a profile.
     */
    return g_build_filename(g_get_user_state_dir(), "clawtilla",
                            "last-connection", NULL);
}

gchar *
clawt_connection_last_load(const gchar *path)
{
    g_autofree gchar *resolved = NULL;
    g_autofree gchar *text = NULL;

    resolved = path != NULL ? clawt_expand_path(path)
                            : clawt_connection_last_default_path();

    if (!g_file_get_contents(resolved, &text, NULL, NULL))
        return NULL;

    g_strstrip(text);

    if (*text == '\0')
        return NULL;

    return g_steal_pointer(&text);
}

gboolean
clawt_connection_last_save(const gchar     *path,
                           ClawtConnection *connection,
                           GError         **error)
{
    g_autofree gchar *resolved = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *text = NULL;

    resolved = path != NULL ? clawt_expand_path(path)
                            : clawt_connection_last_default_path();

    /* the local daemon is the default: nothing to remember */
    if (connection == NULL || clawt_connection_is_local(connection)) {
        if (g_unlink(resolved) != 0 && errno != ENOENT) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                        "could not forget the last connection at %s: %s",
                        resolved, g_strerror(errno));
            return FALSE;
        }

        return TRUE;
    }

    dir = g_path_get_dirname(resolved);

    if (!clawt_ensure_dir(dir, 0700, error))
        return FALSE;

    /* the name alone: never the address, never the token */
    text = g_strconcat(clawt_connection_get_name(connection), "\n", NULL);

    return clawt_write_file_atomic(resolved, text, -1, 0600, FALSE, error);
}

/* ── The daemon a running client is on now ─────────────────────────── */

#define CURRENT_PREFIX "current-connection-"
#define CURRENT_GROUP  "connection"

gchar *
clawt_connection_current_default_dir(void)
{
    /*
     * Runtime, not state: it describes processes that are running, so
     * it belongs somewhere a reboot empties.  A directory of its own
     * rather than the daemon's `clawtilla/` itself, because saving
     * forces the directory to 0700 and the daemon's is not ours to
     * re-mode.
     */
    return g_build_filename(g_get_user_runtime_dir(), "clawtilla",
                            "clients", NULL);
}

/* The file for @pid in @dir. */
static gchar *
current_path_for(const gchar *dir, gint pid)
{
    g_autofree gchar *base = g_strdup_printf(CURRENT_PREFIX "%d", pid);

    return g_build_filename(dir, base, NULL);
}

gboolean
clawt_connection_current_save(const gchar      *dir,
                              ClawtConnection  *connection,
                              GError          **error)
{
    g_autofree gchar *resolved = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *text = NULL;
    g_autoptr(GKeyFile) file = NULL;
    const gchar *client;

    resolved = dir != NULL ? clawt_expand_path(dir)
                           : clawt_connection_current_default_dir();
    path = current_path_for(resolved, (gint)getpid());

    /* no connection: this process is on nothing, so it says nothing */
    if (connection == NULL) {
        if (g_unlink(path) != 0 && errno != ENOENT) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                        "could not remove %s: %s", path, g_strerror(errno));
            return FALSE;
        }

        return TRUE;
    }

    if (!clawt_ensure_dir(resolved, 0700, error))
        return FALSE;

    /*
     * What identifies the daemon, and where its token can be found
     * again -- never the token.  A reader resolves a remote one through
     * connections.yaml or the token file named here, both of which the
     * person controls and can rotate; a token copied into a runtime
     * file would outlive a rotation and be one more place to leak from.
     */
    file = g_key_file_new();
    client = g_get_prgname();
    g_key_file_set_string(file, CURRENT_GROUP, "client",
                          client != NULL ? client : "a client");
    g_key_file_set_int64(file, CURRENT_GROUP, "written", g_get_real_time());
    g_key_file_set_string(file, CURRENT_GROUP, "name",
                          clawt_connection_get_name(connection));

    if (clawt_connection_is_local(connection)) {
        g_key_file_set_string(file, CURRENT_GROUP, "kind", "local");

        if (connection->socket_path != NULL)
            g_key_file_set_string(file, CURRENT_GROUP, "socket",
                                  connection->socket_path);
    } else {
        g_key_file_set_string(file, CURRENT_GROUP, "kind", "remote");
        g_key_file_set_string(file, CURRENT_GROUP, "host", connection->host);
        g_key_file_set_integer(file, CURRENT_GROUP, "port",
                               (gint)connection->port);
        g_key_file_set_boolean(file, CURRENT_GROUP, "tls", connection->tls);
        g_key_file_set_boolean(file, CURRENT_GROUP,
                               "accept-unknown-certificate",
                               connection->accept_unknown_certificate);

        if (connection->token_file != NULL)
            g_key_file_set_string(file, CURRENT_GROUP, "token-file",
                                  connection->token_file);
    }

    text = g_key_file_to_data(file, NULL, NULL);

    return clawt_write_file_atomic(path, text, -1, 0600, FALSE, error);
}

/*
 * Whether @pid is a process that is still there.  EPERM is alive: the
 * process exists and is somebody else's, which in a per-user runtime
 * directory should not happen, but is not a dead client either.
 */
static gboolean
pid_is_alive(gint pid)
{
    if (pid <= 0)
        return FALSE;

    return kill((pid_t)pid, 0) == 0 || errno == EPERM;
}

/*
 * The live file written most recently in @dir, loaded, or %NULL.  A file
 * whose process is gone -- a client that crashed, or was killed before
 * it could clean up -- is ignored and, being nobody's, removed.
 */
static GKeyFile *
current_newest(const gchar *dir, gint *pid_out)
{
    g_autoptr(GDir) listing = NULL;
    g_autoptr(GKeyFile) best = NULL;
    gint64 best_written = G_MININT64;
    gint best_pid = 0;
    const gchar *entry;

    listing = g_dir_open(dir, 0, NULL);

    if (listing == NULL)
        return NULL;

    while ((entry = g_dir_read_name(listing)) != NULL) {
        g_autofree gchar *path = NULL;
        g_autoptr(GKeyFile) file = NULL;
        guint64 pid = 0;
        gint64 written;

        if (!g_str_has_prefix(entry, CURRENT_PREFIX))
            continue;

        /* the name carries the pid; anything else there is not ours */
        if (!g_ascii_string_to_unsigned(entry + strlen(CURRENT_PREFIX), 10,
                                        1, G_MAXINT, &pid, NULL))
            continue;

        path = g_build_filename(dir, entry, NULL);

        if (!pid_is_alive((gint)pid)) {
            (void)g_unlink(path);
            continue;
        }

        file = g_key_file_new();

        if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
            continue;

        written = g_key_file_get_int64(file, CURRENT_GROUP, "written", NULL);

        /*
         * Newest wins; a tie (two writes inside the same microsecond)
         * goes to the higher pid, which is arbitrary but at least the
         * same answer every time it is asked.
         */
        if (best == NULL || written > best_written ||
            (written == best_written && (gint)pid > best_pid)) {
            g_clear_pointer(&best, g_key_file_unref);
            best = g_steal_pointer(&file);
            best_written = written;
            best_pid = (gint)pid;
        }
    }

    if (pid_out != NULL)
        *pid_out = best_pid;

    return g_steal_pointer(&best);
}

/*
 * A remote record's profile in @saved: the one it is named for, when
 * that still points at the same daemon, else @by_address and any
 * profile for the same host and port.
 */
static ClawtConnection *
find_profile_for(GPtrArray   *saved,
                 const gchar *name,
                 const gchar *host,
                 guint16      port,
                 gboolean     by_address)
{
    guint i;

    for (i = 0; saved != NULL && i < saved->len; i++) {
        ClawtConnection *candidate = g_ptr_array_index(saved, i);

        if (clawt_connection_is_local(candidate) ||
            g_strcmp0(candidate->host, host) != 0 || candidate->port != port)
            continue;

        if (by_address || g_strcmp0(candidate->name, name) == 0)
            return candidate;
    }

    return NULL;
}

/*
 * The connection @file describes, with a token found the way the header
 * says, or %NULL with @notice saying why when it names a remote daemon
 * whose token cannot be found.
 */
static ClawtConnection *
current_resolve(GKeyFile    *file,
                const gchar *connections_path,
                const gchar *who,
                gint         pid,
                gchar      **notice)
{
    g_autofree gchar *kind = NULL;
    g_autofree gchar *name = NULL;
    g_autofree gchar *host = NULL;
    g_autofree gchar *token_file = NULL;
    g_autoptr(GPtrArray) saved = NULL;
    g_autoptr(ClawtConnection) connection = NULL;
    g_autoptr(GError) error = NULL;
    ClawtConnection *found;
    gint port;

    kind = g_key_file_get_string(file, CURRENT_GROUP, "kind", NULL);
    name = g_key_file_get_string(file, CURRENT_GROUP, "name", NULL);

    /* a local daemon needs no token: the socket is the whole address */
    if (g_strcmp0(kind, "local") == 0) {
        g_autofree gchar *socket_path =
            g_key_file_get_string(file, CURRENT_GROUP, "socket", NULL);

        return clawt_connection_new_local(name != NULL ? name : "Local",
                                          socket_path);
    }

    host = g_key_file_get_string(file, CURRENT_GROUP, "host", NULL);
    port = g_key_file_get_integer(file, CURRENT_GROUP, "port", NULL);

    if (g_strcmp0(kind, "remote") != 0 || host == NULL || port <= 0 ||
        port > G_MAXUINT16)
        return NULL;

    /*
     * A remote one's token, in order.  The profile it is named for, if
     * that still points at the same daemon: read afresh, so a token
     * rotated since is the one used.  Then the token file it was started
     * with.  Then any profile for the same host and port -- a client
     * started with --host and --token on a daemon that is also saved.
     * The current-connection file is never asked: it holds no token, on
     * purpose.
     */
    saved = clawt_connection_list_load(connections_path, NULL);
    found = find_profile_for(saved, name, host, (guint16)port, FALSE);

    if (found != NULL)
        return clawt_connection_copy(found);

    token_file = g_key_file_get_string(file, CURRENT_GROUP, "token-file",
                                       NULL);

    if (token_file != NULL) {
        connection = clawt_connection_new_remote(name, host, (guint16)port,
                                                 NULL);
        clawt_connection_set_tls(
            connection,
            g_key_file_get_boolean(file, CURRENT_GROUP, "tls", NULL),
            g_key_file_get_boolean(file, CURRENT_GROUP,
                                   "accept-unknown-certificate", NULL));

        if (clawt_connection_load_token_file(connection, token_file, &error))
            return g_steal_pointer(&connection);

        if (notice != NULL)
            *notice = g_strdup_printf(
                "%s (pid %d) is on %s:%d, but its token file cannot be "
                "read (%s).", who, pid, host, port, error->message);

        return NULL;
    }

    found = find_profile_for(saved, name, host, (guint16)port, TRUE);

    if (found != NULL)
        return clawt_connection_copy(found);

    /*
     * Started with --host and a --token this process cannot see.  Not
     * guessed at and not dialled without one: a daemon that refuses us
     * is a worse start than saying so and choosing something else.
     */
    if (notice != NULL)
        *notice = g_strdup_printf(
            "%s (pid %d) is on %s:%d with a token given on its command "
            "line, which is not shared; save that daemon with `clawtilla "
            "remote add`, or start the client with --token-file.",
            who, pid, host, port);

    return NULL;
}

ClawtConnection *
clawt_connection_current_load(const gchar  *dir,
                              const gchar  *connections_path,
                              gint         *pid,
                              gchar       **client,
                              gchar       **notice)
{
    g_autofree gchar *resolved = NULL;
    g_autofree gchar *who = NULL;
    g_autoptr(GKeyFile) file = NULL;
    ClawtConnection *connection;
    gint found_pid = 0;

    if (pid != NULL)
        *pid = 0;

    if (client != NULL)
        *client = NULL;

    if (notice != NULL)
        *notice = NULL;

    resolved = dir != NULL ? clawt_expand_path(dir)
                           : clawt_connection_current_default_dir();
    file = current_newest(resolved, &found_pid);

    if (file == NULL)
        return NULL;

    who = g_key_file_get_string(file, CURRENT_GROUP, "client", NULL);

    if (who == NULL)
        who = g_strdup("a client");

    connection = current_resolve(file, connections_path, who, found_pid,
                                 notice);

    if (connection == NULL)
        return NULL;

    if (pid != NULL)
        *pid = found_pid;

    if (client != NULL)
        *client = g_steal_pointer(&who);

    return connection;
}

ClawtConnection *
clawt_connection_choose(const gchar  *host,
                        guint16       port,
                        const gchar  *token,
                        const gchar  *profile,
                        const gchar  *socket_path,
                        const gchar  *connections_path,
                        const gchar  *last_path,
                        gboolean      remembered,
                        gboolean     *from_last,
                        gchar       **notice,
                        GError      **error)
{
    g_autofree gchar *last = NULL;

    /* nothing to report until the remembered file is what decided */
    if (from_last != NULL)
        *from_last = FALSE;

    if (notice != NULL)
        *notice = NULL;

    if (host != NULL)
        return clawt_connection_new_remote(
            host, host, port > 0 ? port : CLAWT_DEFAULT_TCP_PORT, token);

    if (profile != NULL) {
        g_autoptr(GPtrArray) saved = clawt_connection_list_load(
            connections_path, error);
        ClawtConnection *found;

        if (saved == NULL)
            return NULL;

        found = clawt_connection_list_find(saved, profile);

        if (found == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                        "there is no saved connection called '%s'", profile);
            return NULL;
        }

        /* copied: `saved` owns it and goes out of scope here */
        return clawt_connection_copy(found);
    }

    /* an explicit socket is an explicit choice of the local daemon */
    if (socket_path != NULL || !remembered)
        return clawt_connection_new_local("Local", socket_path);

    last = clawt_connection_last_load(last_path);

    if (last != NULL) {
        g_autoptr(GPtrArray) saved = clawt_connection_list_load(
            connections_path, NULL);
        ClawtConnection *found = saved != NULL
            ? clawt_connection_list_find(saved, last) : NULL;

        if (found != NULL) {
            if (from_last != NULL)
                *from_last = TRUE;

            return clawt_connection_copy(found);
        }

        /*
         * Gone since -- deleted, renamed, or the file unreadable: the
         * local daemon, as an unknown --profile is not an error.  The
         * name was remembered, not asked for.  But no longer silently:
         * somebody who was on the workstation yesterday and opens on
         * Local today, with nothing said, reads it as the workstation's
         * fleet having vanished.  The sentence is the caller's to show
         * (a toast in both graphical clients).
         */
        if (notice != NULL)
            *notice = g_strdup_printf(
                "The connection you last used, “%s”, is no "
                "longer saved; connected to Local.", last);
    }

    return clawt_connection_new_local("Local", NULL);
}

/* The sentence saying which daemon @source chose, and why. */
static gchar *
source_text(ClawtConnectionSource  source,
            ClawtConnection       *connection,
            const gchar           *client,
            gint                   pid,
            gboolean               passed_over)
{
    g_autofree gchar *where = clawt_connection_describe(connection);
    const gchar *name = clawt_connection_get_name(connection);

    switch (source) {
    case CLAWT_CONNECTION_FROM_HOST:
        return g_strdup_printf("Opening on %s, as --host asked.", where);

    case CLAWT_CONNECTION_FROM_PROFILE:
        return g_strdup_printf("Opening on %s (%s), as --profile asked.",
                               name, where);

    case CLAWT_CONNECTION_FROM_SOCKET:
        return g_strdup_printf("Opening on the local daemon (%s), as "
                               "--socket asked.", where);

    case CLAWT_CONNECTION_FROM_RUNNING_CLIENT:
        return g_strdup_printf("Opening on %s (%s), the daemon %s (pid %d) "
                               "is on.", name, where,
                               client != NULL ? client : "a client", pid);

    case CLAWT_CONNECTION_FROM_LAST:
        return g_strdup_printf("Opening on %s (%s), the daemon the clients "
                               "were last on.", name, where);

    case CLAWT_CONNECTION_FROM_LOCAL:
    default:
        /* the sentence before this one has already said why */
        if (passed_over)
            return g_strdup_printf("Opening on the local daemon (%s).",
                                   where);

        return g_strdup_printf("Opening on the local daemon (%s): no client "
                               "is running, and none was last on another.",
                               where);
    }
}

ClawtConnection *
clawt_connection_choose_following(const gchar            *host,
                                  guint16                 port,
                                  const gchar            *token,
                                  const gchar            *token_file,
                                  const gchar            *profile,
                                  const gchar            *socket_path,
                                  const gchar            *connections_path,
                                  const gchar            *last_path,
                                  const gchar            *current_dir,
                                  ClawtConnectionSource  *source,
                                  gchar                 **reason,
                                  GError                **error)
{
    g_autoptr(ClawtConnection) connection = NULL;
    g_autofree gchar *running_notice = NULL;
    g_autofree gchar *last_notice = NULL;
    g_autofree gchar *client = NULL;
    g_autofree gchar *sentence = NULL;
    ClawtConnectionSource chosen;
    gboolean from_last = FALSE;
    gint pid = 0;

    if (source != NULL)
        *source = CLAWT_CONNECTION_FROM_LOCAL;

    if (reason != NULL)
        *reason = NULL;

    /*
     * A token file is a --host's token.  A profile carries its own, and
     * the local socket wants none, so one given with either is a
     * mistake to point out rather than a file to read and ignore.
     */
    if (token != NULL && token_file != NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "--token or --token-file, not both");
        return NULL;
    }

    if (token_file != NULL && host == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "--token-file goes with --host; a profile keeps its "
                    "own token");
        return NULL;
    }

    if (host != NULL || profile != NULL || socket_path != NULL) {
        /*
         * Named on the command line: exactly what clawt_connection_choose()
         * does with it, and nothing remembered or running is consulted.
         */
        connection = clawt_connection_choose(host, port, token, profile,
                                             socket_path, connections_path,
                                             last_path, FALSE, NULL, NULL,
                                             error);

        if (connection == NULL)
            return NULL;

        if (host != NULL && token_file != NULL &&
            !clawt_connection_load_token_file(connection, token_file, error))
            return NULL;

        chosen = host != NULL    ? CLAWT_CONNECTION_FROM_HOST
               : profile != NULL ? CLAWT_CONNECTION_FROM_PROFILE
                                 : CLAWT_CONNECTION_FROM_SOCKET;
    } else {
        connection = clawt_connection_current_load(current_dir,
                                                   connections_path, &pid,
                                                   &client, &running_notice);

        if (connection != NULL) {
            chosen = CLAWT_CONNECTION_FROM_RUNNING_CLIENT;
        } else {
            connection = clawt_connection_choose(NULL, 0, NULL, NULL, NULL,
                                                 connections_path, last_path,
                                                 TRUE, &from_last,
                                                 &last_notice, error);

            if (connection == NULL)
                return NULL;

            chosen = from_last ? CLAWT_CONNECTION_FROM_LAST
                               : CLAWT_CONNECTION_FROM_LOCAL;
        }
    }

    if (source != NULL)
        *source = chosen;

    /*
     * Why a level was passed over comes first, then what was chosen: the
     * sentence is read top to bottom by somebody wondering why this is
     * not the daemon they expected.
     */
    if (reason != NULL) {
        GString *text = g_string_new(NULL);

        sentence = source_text(chosen, connection, client, pid,
                               running_notice != NULL ||
                               last_notice != NULL);

        if (running_notice != NULL)
            g_string_append_printf(text, "%s ", running_notice);

        if (last_notice != NULL)
            g_string_append_printf(text, "%s ", last_notice);

        g_string_append(text, sentence);
        *reason = g_string_free(text, FALSE);
    }

    return g_steal_pointer(&connection);
}
