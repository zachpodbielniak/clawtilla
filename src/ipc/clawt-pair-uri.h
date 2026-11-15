/*
 * clawt-pair-uri.h - The pairing URI a QR code carries
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Pairing a phone used to mean reading an address and six digits off one
 * screen and typing both into another.  `device.pair` now also answers
 * with a URI holding the same two things, which a client can draw as a QR
 * code for the phone to scan:
 *
 *   clawtilla://pair?v=1&code=123456&port=8792&host=100.101.102.103
 *       &host=fd7a%3A115c%3Aa1e0%3A%3A1&name=phone
 *
 * Every value is percent-encoded, so an IPv6 address travels bare (no
 * brackets) with its colons as `%3A`.  `host` repeats, in the order the
 * daemon would rather be dialled.  `name` is optional.  Nothing in it is
 * a credential: the code is the same short-lived, single-use,
 * guess-limited code `device.pair` already returns, and a token only ever
 * comes back from `device.redeem`, to the device that redeemed it.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_PAIR_URI_SCHEME:
 *
 * The scheme of a pairing URI.  A phone registers for it, so the camera
 * app can hand a scanned code straight to the client.
 */
#define CLAWT_PAIR_URI_SCHEME "clawtilla"

/**
 * CLAWT_PAIR_URI_VERSION:
 *
 * The `v` a pairing URI carries.  A reader refuses any other, rather than
 * guessing at what a newer daemon meant by a parameter it does not know.
 */
#define CLAWT_PAIR_URI_VERSION (1)

/**
 * clawt_pair_uri_build:
 * @code: the pairing code; spaces and dashes are dropped, so the
 *   displayed form ("123 456") works as well as the bare digits
 * @hosts: (array zero-terminated=1) (nullable): addresses to dial, bare
 *   (an IPv6 address without brackets), in order of preference
 * @port: the port every host listens on; 0 for %CLAWT_DEFAULT_TCP_PORT
 * @name: (nullable): what to call the device being paired -- the name
 *   `device.pair` was given, which the daemon files the device under
 *   whatever `device.redeem` says; left out when %NULL or empty
 *
 * Builds `clawtilla://pair?v=1&code=…&port=…&host=…[&host=…][&name=…]`.
 * Every value goes through g_uri_escape_string() with nothing reserved
 * allowed, so a colon in an IPv6 address or an ampersand in a name cannot
 * be read back as structure.
 *
 * Returns: (transfer full) (nullable): the URI, or %NULL when @code has
 *   no characters left after normalising
 */
gchar *clawt_pair_uri_build(const gchar        *code,
                            const gchar * const *hosts,
                            guint16              port,
                            const gchar        *name);

/**
 * clawt_pair_uri_build_from_addresses:
 * @code: the pairing code
 * @addresses: (array zero-terminated=1) (nullable): what
 *   clawt_ipc_server_list_bound() returns -- `host:port`, `[v6]:port`,
 *   or a bare address
 * @name: (nullable): what to call the device being paired
 *
 * The URI for the addresses a daemon actually bound.
 *
 * A wildcard (`0.0.0.0`, `::`) and a loopback address are left out: a
 * phone dialling either reaches itself, and the attempt costs the person
 * a timeout before it tries the address that would have worked.  The
 * port is the first remaining address's; an address on a different port
 * is left out too, because the URI has room for one port and dialling a
 * host on the wrong one is the same timeout.
 *
 * Returns: (transfer full) (nullable): the URI, or %NULL when no address
 *   is left to dial or @code is empty
 */
gchar *clawt_pair_uri_build_from_addresses(const gchar         *code,
                                           const gchar * const *addresses,
                                           const gchar         *name);

/**
 * clawt_pair_uri_parse:
 * @uri: a pairing URI
 * @out_code: (out) (optional) (transfer full): the code, digits only
 * @out_hosts: (out) (optional) (transfer full) (array zero-terminated=1):
 *   the hosts, in the order the URI gives them
 * @out_port: (out) (optional): the port; %CLAWT_DEFAULT_TCP_PORT when the
 *   URI names none
 * @out_name: (out) (optional) (transfer full) (nullable): the name, or
 *   %NULL when the URI carries none
 * @error: (out) (optional): return location for a #GError
 *
 * Reads a URI made by clawt_pair_uri_build().  Refuses another scheme, a
 * version other than %CLAWT_PAIR_URI_VERSION, a code that is not digits,
 * a port outside 1-65535 and a URI with no host -- each would otherwise
 * surface as a connection failure on the phone, naming none of them.
 * Parameters it does not know are ignored, so a later version can add
 * one without breaking this reader.
 *
 * Returns: %TRUE if @uri was a usable pairing URI
 */
gboolean clawt_pair_uri_parse(const gchar   *uri,
                              gchar        **out_code,
                              gchar       ***out_hosts,
                              guint16       *out_port,
                              gchar        **out_name,
                              GError       **error);

G_END_DECLS
