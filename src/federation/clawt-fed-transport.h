/*
 * clawt-fed-transport.h - Which pipe an endpoint means, and whether it is allowed
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The envelope makes every link authentic, whatever carries it.  Whether
 * it is *private* depends on the pipe, so plain text is allowed only
 * where something else already encrypts or isolates the traffic:
 *
 *   https://...                       TLS (a tunnel, a proxy, direct)
 *   unix:/path                        this machine (`unix:/path#ipc` is a
 *                                     daemon's own socket, spoken as IPC)
 *   http:// or clawtilla:// to
 *     127.0.0.0/8, ::1, localhost     this machine
 *     100.64.0.0/10, fd7a:115c:a1e0::/48, *.ts.net
 *                                     a tailnet: WireGuard encrypts it
 *     RFC 1918, link-local, unique-local, other names
 *                                     a LAN: only with
 *                                     federation.allow_lan_plaintext, and
 *                                     sealed under federation.seal: auto
 *     anything else                   refused: needs TLS
 *
 * A name is not resolved here (this runs where a request arrives, which
 * must not wait on the network).  A name that is not `localhost` or a
 * tailnet name is treated as a LAN name, the stricter of the two.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * ClawtFedPath:
 * @CLAWT_FED_PATH_TLS: encrypted by TLS
 * @CLAWT_FED_PATH_LOCAL: this machine (a unix socket or loopback)
 * @CLAWT_FED_PATH_TAILNET: a tailnet; WireGuard encrypts it
 * @CLAWT_FED_PATH_LAN: a local network in plain text
 * @CLAWT_FED_PATH_PUBLIC: the internet in plain text (refused)
 *
 * What protects the bytes on a link.
 */
typedef enum {
    CLAWT_FED_PATH_TLS = 0,
    CLAWT_FED_PATH_LOCAL,
    CLAWT_FED_PATH_TAILNET,
    CLAWT_FED_PATH_LAN,
    CLAWT_FED_PATH_PUBLIC
} ClawtFedPath;

/**
 * ClawtFedSeal:
 * @CLAWT_FED_SEAL_AUTO: seal on a plain-text LAN link only
 * @CLAWT_FED_SEAL_ALWAYS: seal every body, even through TLS (so a tunnel's
 *   edge sees only ciphertext)
 * @CLAWT_FED_SEAL_NEVER: never seal
 *
 * `federation.seal`.
 */
typedef enum {
    CLAWT_FED_SEAL_AUTO = 0,
    CLAWT_FED_SEAL_ALWAYS,
    CLAWT_FED_SEAL_NEVER
} ClawtFedSeal;

GType clawt_fed_seal_get_type(void) G_GNUC_CONST;
#define CLAWT_TYPE_FED_SEAL (clawt_fed_seal_get_type())

/**
 * ClawtFedTransport:
 * @CLAWT_FED_TRANSPORT_HTTP: HTTP(S) with a WebSocket for events
 * @CLAWT_FED_TRANSPORT_IPC: the daemon's own IPC port (`clawtilla://`)
 *
 * What an endpoint is spoken over.
 */
typedef enum {
    CLAWT_FED_TRANSPORT_HTTP = 0,
    CLAWT_FED_TRANSPORT_IPC
} ClawtFedTransport;

/**
 * clawt_fed_path_label:
 * @path: a #ClawtFedPath
 *
 * Returns: (transfer none): a few words for the peer page
 */
const gchar *clawt_fed_path_label(ClawtFedPath path);

/**
 * clawt_fed_host_path:
 * @host: a host name or address literal, without brackets
 *
 * Returns: what protects a plain-text connection to @host
 */
ClawtFedPath clawt_fed_host_path(const gchar *host);

/**
 * clawt_fed_endpoint_parse:
 * @endpoint: a well-formed endpoint (clawt_fed_endpoint_check())
 * @out_transport: (out) (optional): HTTP or IPC
 * @out_host: (out) (optional) (transfer full) (nullable): the host, or
 *   %NULL for a unix socket
 * @out_port: (out) (optional): the port (defaults: https 443, http 80,
 *   clawtilla %CLAWT_DEFAULT_TCP_PORT; 0 for a unix socket)
 * @out_socket: (out) (optional) (transfer full) (nullable): the socket
 *   path for `unix:`
 * @out_tls: (out) (optional): whether it is `https://`
 *
 * Returns: %TRUE if @endpoint could be taken apart
 */
gboolean clawt_fed_endpoint_parse(const gchar        *endpoint,
                                  ClawtFedTransport  *out_transport,
                                  gchar             **out_host,
                                  guint16            *out_port,
                                  gchar             **out_socket,
                                  gboolean           *out_tls);

/**
 * clawt_fed_transport_check:
 * @endpoint: an endpoint
 * @allow_lan_plaintext: `federation.allow_lan_plaintext`
 * @tls_configured: whether the IPC listener has a certificate (a
 *   `clawtilla://` endpoint is then TLS)
 * @out_path: (out) (optional): what protects it
 * @error: (out) (optional): return location for a #GError
 *
 * Refuses plain text to the internet, and to a LAN unless allowed, with a
 * message naming the alternatives.
 *
 * Returns: %TRUE if a link may use @endpoint
 */
gboolean clawt_fed_transport_check(const gchar   *endpoint,
                                   gboolean       allow_lan_plaintext,
                                   gboolean       tls_configured,
                                   ClawtFedPath  *out_path,
                                   GError       **error);

/**
 * clawt_fed_should_seal:
 * @mode: `federation.seal`
 * @path: the link's path
 *
 * Returns: %TRUE if bodies on this link are sealed
 */
gboolean clawt_fed_should_seal(ClawtFedSeal mode,
                               ClawtFedPath path);

G_END_DECLS
