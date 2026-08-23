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

#include <string.h>

#include <yaml-glib.h>

struct _ClawtConnection {
    gchar   *name;
    gchar   *socket_path;   /* set for a local connection */
    gchar   *host;          /* set for a remote one */
    guint16  port;
    gchar   *token;
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
