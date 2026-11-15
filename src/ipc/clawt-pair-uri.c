/*
 * clawt-pair-uri.c - The pairing URI a QR code carries
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "ipc/clawt-pair-uri.h"

#include <string.h>

/*
 * The code as digits.  The display form groups it ("123 456") and people
 * type it back with a dash as often as a space, so both are dropped
 * rather than refused.
 */
static gchar *
normalise_code(const gchar *code)
{
    GString *out = g_string_new(NULL);
    const gchar *p;

    for (p = code; *p != '\0'; p++)
        if (*p != ' ' && *p != '-')
            g_string_append_c(out, *p);

    return g_string_free(out, FALSE);
}

static void
append_param(GString *uri, const gchar *key, const gchar *value)
{
    g_autofree gchar *escaped = g_uri_escape_string(value, NULL, FALSE);

    g_string_append_printf(uri, "&%s=%s", key, escaped);
}

gchar *
clawt_pair_uri_build(const gchar *code, const gchar * const *hosts,
                     guint16 port, const gchar *name)
{
    g_autofree gchar *digits = NULL;
    g_autofree gchar *port_text = NULL;
    GString *uri;
    gsize i;

    g_return_val_if_fail(code != NULL, NULL);

    digits = normalise_code(code);

    if (*digits == '\0')
        return NULL;

    port_text = g_strdup_printf("%u", port != 0 ? (guint)port
                                                : (guint)CLAWT_DEFAULT_TCP_PORT);

    /*
     * `v` first and on its own, so a reader can refuse a version it does
     * not know before it has read anything that version might mean
     * differently.
     */
    uri = g_string_new(CLAWT_PAIR_URI_SCHEME "://pair?v=");
    g_string_append_printf(uri, "%d", CLAWT_PAIR_URI_VERSION);
    append_param(uri, "code", digits);
    append_param(uri, "port", port_text);

    for (i = 0; hosts != NULL && hosts[i] != NULL; i++)
        if (*hosts[i] != '\0')
            append_param(uri, "host", hosts[i]);

    if (name != NULL && *name != '\0')
        append_param(uri, "name", name);

    /* the digits are a live code: do not leave a copy in freed memory */
    memset(digits, 0, strlen(digits));

    return g_string_free(uri, FALSE);
}

/*
 * Whether a phone could reach this daemon at @host.  An unparseable host
 * is kept: clawt_ipc_server_list_bound() only reports numeric addresses,
 * so anything else came from somewhere that knew what it was doing.
 */
static gboolean
host_is_dialable(const gchar *host)
{
    g_autoptr(GInetAddress) inet = g_inet_address_new_from_string(host);

    if (inet == NULL)
        return TRUE;

    return !g_inet_address_get_is_any(inet) &&
           !g_inet_address_get_is_loopback(inet);
}

gchar *
clawt_pair_uri_build_from_addresses(const gchar *code,
                                    const gchar * const *addresses,
                                    const gchar *name)
{
    g_autoptr(GPtrArray) hosts = g_ptr_array_new_with_free_func(g_free);
    guint16 chosen_port = 0;
    gsize i;

    g_return_val_if_fail(code != NULL, NULL);

    for (i = 0; addresses != NULL && addresses[i] != NULL; i++) {
        g_autofree gchar *host = NULL;
        guint16 port = 0;

        /*
         * The same reader `--bind` and `remote pair` use, so `[v6]:port`
         * and a bare IPv6 address (every colon its own) split the one
         * way everywhere.
         */
        if (!clawt_ipc_parse_listen_address(addresses[i],
                                            CLAWT_DEFAULT_TCP_PORT,
                                            &host, &port, NULL))
            continue;

        if (!host_is_dialable(host))
            continue;

        if (chosen_port == 0)
            chosen_port = port;
        else if (port != chosen_port)
            continue;

        g_ptr_array_add(hosts, g_steal_pointer(&host));
    }

    if (hosts->len == 0)
        return NULL;

    g_ptr_array_add(hosts, NULL);

    return clawt_pair_uri_build(code, (const gchar * const *)hosts->pdata,
                                chosen_port, name);
}

/*
 * The refusal does not quote the URI: it holds a live code, and an error
 * message is exactly the text that ends up in a log or a bug report.
 */
static gboolean
refuse(GError **error, const gchar *why)
{
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                "not a clawtilla pairing link: %s", why);
    return FALSE;
}

gboolean
clawt_pair_uri_parse(const gchar *uri, gchar **out_code, gchar ***out_hosts,
                     guint16 *out_port, gchar **out_name, GError **error)
{
    g_autoptr(GUri) parsed = NULL;
    g_autoptr(GPtrArray) hosts = NULL;
    g_autoptr(GError) local_error = NULL;
    g_autofree gchar *code = NULL;
    g_autofree gchar *name = NULL;
    g_autofree gchar *version = NULL;
    GUriParamsIter iter;
    const gchar *query;
    gchar *key = NULL;
    gchar *value = NULL;
    gint64 port = CLAWT_DEFAULT_TCP_PORT;
    const gchar *p;

    g_return_val_if_fail(uri != NULL, FALSE);

    /*
     * ENCODED_QUERY keeps the query as it was written, so the params
     * iterator below is what decodes it -- once, per value -- and an
     * escaped `&` inside a name stays inside the name.
     */
    parsed = g_uri_parse(uri, G_URI_FLAGS_ENCODED_QUERY, &local_error);

    if (parsed == NULL)
        return refuse(error, "it is not a URI");

    if (g_ascii_strcasecmp(g_uri_get_scheme(parsed),
                           CLAWT_PAIR_URI_SCHEME) != 0)
        return refuse(error, "the scheme is not " CLAWT_PAIR_URI_SCHEME);

    if (g_uri_get_host(parsed) == NULL ||
        g_ascii_strcasecmp(g_uri_get_host(parsed), "pair") != 0)
        return refuse(error, "it does not ask to pair");

    query = g_uri_get_query(parsed);

    if (query == NULL)
        return refuse(error, "it carries no code");

    hosts = g_ptr_array_new_with_free_func(g_free);

    /*
     * An iterator rather than g_uri_parse_params(): that returns a hash
     * table, and `host` repeats -- every address but the last would be
     * lost without a word.
     */
    g_uri_params_iter_init(&iter, query, -1, "&", G_URI_PARAMS_NONE);

    while (g_uri_params_iter_next(&iter, &key, &value, &local_error)) {
        if (g_strcmp0(key, "v") == 0) {
            g_free(version);
            version = g_steal_pointer(&value);
        } else if (g_strcmp0(key, "code") == 0) {
            g_free(code);
            code = normalise_code(value);
        } else if (g_strcmp0(key, "host") == 0) {
            if (value != NULL && *value != '\0')
                g_ptr_array_add(hosts, g_steal_pointer(&value));
        } else if (g_strcmp0(key, "port") == 0) {
            gchar *end = NULL;

            port = (value != NULL) ? g_ascii_strtoll(value, &end, 10) : 0;

            if (end == value || end == NULL || *end != '\0')
                port = 0;
        } else if (g_strcmp0(key, "name") == 0) {
            g_free(name);
            name = g_steal_pointer(&value);
        }

        g_clear_pointer(&key, g_free);
        g_clear_pointer(&value, g_free);
    }

    if (local_error != NULL)
        return refuse(error, local_error->message);

    if (version == NULL)
        return refuse(error, "it names no version");

    if (g_strcmp0(version, "1") != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "this pairing link is version %s, and this client "
                    "reads version %d; update the client", version,
                    CLAWT_PAIR_URI_VERSION);
        return FALSE;
    }

    if (code == NULL || *code == '\0')
        return refuse(error, "it carries no code");

    for (p = code; *p != '\0'; p++)
        if (!g_ascii_isdigit(*p))
            return refuse(error, "the code is not digits");

    if (port <= 0 || port > G_MAXUINT16)
        return refuse(error, "the port is not between 1 and 65535");

    if (hosts->len == 0)
        return refuse(error, "it names no address to dial");

    if (out_code != NULL)
        *out_code = g_steal_pointer(&code);

    if (out_hosts != NULL) {
        g_ptr_array_add(hosts, NULL);
        *out_hosts = (gchar **)g_ptr_array_free(g_steal_pointer(&hosts),
                                                FALSE);
    }

    if (out_port != NULL)
        *out_port = (guint16)port;

    if (out_name != NULL)
        *out_name = (name != NULL && *name != '\0')
            ? g_steal_pointer(&name) : NULL;

    return TRUE;
}
