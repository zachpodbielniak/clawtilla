/*
 * clawt-fed-transport.c - Which pipe an endpoint means, and whether it is allowed
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "federation/clawt-fed-transport.h"

#include <errno.h>
#include <string.h>

static const GEnumValue seal_values[] = {
    { CLAWT_FED_SEAL_AUTO, "CLAWT_FED_SEAL_AUTO", "auto" },
    { CLAWT_FED_SEAL_ALWAYS, "CLAWT_FED_SEAL_ALWAYS", "always" },
    { CLAWT_FED_SEAL_NEVER, "CLAWT_FED_SEAL_NEVER", "never" },
    { 0, NULL, NULL }
};

GType
clawt_fed_seal_get_type(void)
{
    static gsize type_id = 0;

    if (g_once_init_enter(&type_id)) {
        GType t = g_enum_register_static("ClawtFedSeal", seal_values);
        g_once_init_leave(&type_id, t);
    }
    return type_id;
}

const gchar *
clawt_fed_path_label(
    ClawtFedPath path
){
    switch (path) {
    case CLAWT_FED_PATH_TLS:     return "TLS";
    case CLAWT_FED_PATH_LOCAL:   return "this machine";
    case CLAWT_FED_PATH_TAILNET: return "tailnet (WireGuard)";
    case CLAWT_FED_PATH_LAN:     return "local network, plain text";
    case CLAWT_FED_PATH_PUBLIC:  return "internet, plain text";
    }
    return "unknown";
}

/* Tailscale's IPv6 range, fd7a:115c:a1e0::/48. */
static gboolean
is_tailnet_v6(
    GInetAddress *address
){
    const guint8 *b;

    if (g_inet_address_get_family(address) != G_SOCKET_FAMILY_IPV6)
        return FALSE;
    b = g_inet_address_to_bytes(address);
    return b[0] == 0xfd && b[1] == 0x7a && b[2] == 0x11 && b[3] == 0x5c
        && b[4] == 0xa1 && b[5] == 0xe0;
}

ClawtFedPath
clawt_fed_host_path(
    const gchar *host
){
    g_autoptr(GInetAddress) address = NULL;
    g_autofree gchar *lower = NULL;

    if (host == NULL || *host == '\0')
        return CLAWT_FED_PATH_PUBLIC;
    address = g_inet_address_new_from_string(host);
    if (address != NULL) {
        if (is_tailnet_v6(address))
            return CLAWT_FED_PATH_TAILNET;
        switch (clawt_net_address_classify(address)) {
        case CLAWT_NET_ADDRESS_LOOPBACK:
            return CLAWT_FED_PATH_LOCAL;
        case CLAWT_NET_ADDRESS_CGNAT:
            return CLAWT_FED_PATH_TAILNET;
        case CLAWT_NET_ADDRESS_PRIVATE:
        case CLAWT_NET_ADDRESS_LINK_LOCAL:
        case CLAWT_NET_ADDRESS_UNIQUE_LOCAL:
            return CLAWT_FED_PATH_LAN;
        case CLAWT_NET_ADDRESS_PUBLIC:
        case CLAWT_NET_ADDRESS_METADATA:
        case CLAWT_NET_ADDRESS_MULTICAST:
        case CLAWT_NET_ADDRESS_UNSPECIFIED:
        case CLAWT_NET_ADDRESS_RESERVED:
            return CLAWT_FED_PATH_PUBLIC;
        }
        return CLAWT_FED_PATH_PUBLIC;
    }

    lower = g_ascii_strdown(host, -1);
    if (strcmp(lower, "localhost") == 0 || g_str_has_suffix(lower, ".localhost"))
        return CLAWT_FED_PATH_LOCAL;
    if (g_str_has_suffix(lower, ".ts.net"))
        return CLAWT_FED_PATH_TAILNET;
    /* An unresolved name: assume the stricter of tailnet and LAN. */
    return CLAWT_FED_PATH_LAN;
}

/* host[:port] or [v6][:port], into a host without brackets and a port. */
static gboolean
split_authority(
    const gchar  *authority,
    guint16       default_port,
    gchar       **out_host,
    guint16      *out_port
){
    const gchar *colon = NULL;
    g_autofree gchar *host = NULL;
    gchar *end = NULL;
    guint64 port = default_port;

    if (*authority == '[') {
        const gchar *close = strchr(authority, ']');
        if (close == NULL)
            return FALSE;
        host = g_strndup(authority + 1, close - authority - 1);
        if (close[1] == ':')
            colon = close + 1;
    } else {
        colon = strchr(authority, ':');
        host = colon != NULL ? g_strndup(authority, colon - authority) : g_strdup(authority);
    }
    if (colon != NULL) {
        errno = 0;
        port = g_ascii_strtoull(colon + 1, &end, 10);
        if (errno != 0 || end == colon + 1 || *end != '\0' || port < 1 || port > 65535)
            return FALSE;
    }
    if (*host == '\0')
        return FALSE;
    if (out_host != NULL)
        *out_host = g_steal_pointer(&host);
    if (out_port != NULL)
        *out_port = (guint16)port;
    return TRUE;
}

gboolean
clawt_fed_endpoint_parse(
    const gchar        *endpoint,
    ClawtFedTransport  *out_transport,
    gchar             **out_host,
    guint16            *out_port,
    gchar             **out_socket,
    gboolean           *out_tls
){
    if (out_host != NULL)
        *out_host = NULL;
    if (out_socket != NULL)
        *out_socket = NULL;
    if (out_port != NULL)
        *out_port = 0;
    if (out_tls != NULL)
        *out_tls = FALSE;
    if (out_transport != NULL)
        *out_transport = CLAWT_FED_TRANSPORT_HTTP;
    if (endpoint == NULL)
        return FALSE;

    if (g_str_has_prefix(endpoint, "unix:")) {
        if (endpoint[5] != '/')
            return FALSE;
        /* `#ipc` names a daemon's own socket rather than a listener's. */
        if (g_str_has_suffix(endpoint, "#ipc")) {
            if (out_transport != NULL)
                *out_transport = CLAWT_FED_TRANSPORT_IPC;
            if (out_socket != NULL)
                *out_socket = g_strndup(endpoint + 5, strlen(endpoint) - 5 - 4);
            return TRUE;
        }
        if (out_socket != NULL)
            *out_socket = g_strdup(endpoint + 5);
        return TRUE;
    }
    if (g_str_has_prefix(endpoint, "clawtilla://")) {
        g_autofree gchar *authority = g_strdup(endpoint + strlen("clawtilla://"));
        gchar *slash = strchr(authority, '/');
        if (slash != NULL)
            *slash = '\0';
        if (out_transport != NULL)
            *out_transport = CLAWT_FED_TRANSPORT_IPC;
        return split_authority(authority, CLAWT_DEFAULT_TCP_PORT, out_host, out_port);
    }
    if (g_str_has_prefix(endpoint, "http://") || g_str_has_prefix(endpoint, "https://")) {
        g_autoptr(GUri) uri = g_uri_parse(endpoint, G_URI_FLAGS_NONE, NULL);
        gboolean tls = g_str_has_prefix(endpoint, "https://");
        gint port;

        if (uri == NULL || g_uri_get_host(uri) == NULL)
            return FALSE;
        port = g_uri_get_port(uri);
        if (out_host != NULL)
            *out_host = g_strdup(g_uri_get_host(uri));
        if (out_port != NULL)
            *out_port = (guint16)(port > 0 ? port : (tls ? 443 : 80));
        if (out_tls != NULL)
            *out_tls = tls;
        return TRUE;
    }
    return FALSE;
}

gboolean
clawt_fed_transport_check(
    const gchar   *endpoint,
    gboolean       allow_lan_plaintext,
    gboolean       tls_configured,
    ClawtFedPath  *out_path,
    GError       **error
){
    ClawtFedTransport transport;
    g_autofree gchar *host = NULL;
    g_autofree gchar *socket = NULL;
    gboolean tls;
    ClawtFedPath path;

    if (!clawt_fed_endpoint_check(endpoint, error))
        return FALSE;
    if (!clawt_fed_endpoint_parse(endpoint, &transport, &host, NULL, &socket, &tls)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "cannot read the endpoint %s", endpoint);
        return FALSE;
    }

    if (socket != NULL)
        path = CLAWT_FED_PATH_LOCAL;
    else if (tls || (transport == CLAWT_FED_TRANSPORT_IPC && tls_configured))
        path = CLAWT_FED_PATH_TLS;
    else
        path = clawt_fed_host_path(host);

    if (out_path != NULL)
        *out_path = path;

    switch (path) {
    case CLAWT_FED_PATH_TLS:
    case CLAWT_FED_PATH_LOCAL:
    case CLAWT_FED_PATH_TAILNET:
        return TRUE;
    case CLAWT_FED_PATH_LAN:
        if (allow_lan_plaintext)
            return TRUE;
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "%s is a local-network address in plain text; set "
                    "federation.allow_lan_plaintext: true to allow it (bodies "
                    "are then sealed, see federation.seal), or use the "
                    "peer's tailnet address, TLS, or a tunnel", endpoint);
        return FALSE;
    case CLAWT_FED_PATH_PUBLIC:
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "%s would cross the internet in plain text; use https:// "
                    "(a Cloudflare tunnel or a TLS proxy), the peer's tailnet "
                    "address, or give the IPC listener a certificate", endpoint);
        return FALSE;
    }
    return FALSE;
}

gboolean
clawt_fed_should_seal(
    ClawtFedSeal mode,
    ClawtFedPath path
){
    switch (mode) {
    case CLAWT_FED_SEAL_ALWAYS:
        return TRUE;
    case CLAWT_FED_SEAL_NEVER:
        return FALSE;
    case CLAWT_FED_SEAL_AUTO:
        return path == CLAWT_FED_PATH_LAN || path == CLAWT_FED_PATH_PUBLIC;
    }
    return TRUE;
}
