/*
 * clawt-net.h - Which addresses the daemon may reach on an agent's behalf
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Private to the library: not in <clawtilla.h>, not scanned into the GIR.
 *
 * A request the daemon makes because an agent named a URL is made from
 * the daemon's own network position -- the operator's LAN, the tailnet,
 * the cloud instance's metadata service -- none of which the agent was
 * given.  So the address policy is one function here rather than a
 * check at each caller: the wait probes use it now, and the egress gate
 * the roadmap describes is the next caller, which must not grow a second
 * opinion about what "private" means.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/**
 * clawt_net_address_is_public:
 * @address: an address, IPv4 or IPv6
 *
 * Whether @address is on the public internet, in the sense that matters
 * for a request the daemon makes on an agent's behalf: not this machine,
 * not a network this machine is on, not a cloud metadata service, not a
 * range reserved for documentation, benchmarking or multicast.
 *
 * Refused for IPv4: 0.0.0.0/8, 10.0.0.0/8, 100.64.0.0/10 (carrier-grade
 * NAT, which is also the tailnet -- and not an octet boundary, so
 * 100.63.255.255 is public and 100.64.0.1 is not), 127.0.0.0/8,
 * 169.254.0.0/16 (link-local, and every cloud's metadata address),
 * 172.16.0.0/12, 192.0.0.0/24, 192.0.2.0/24, 192.88.99.0/24,
 * 192.168.0.0/16, 198.18.0.0/15, 198.51.100.0/24, 203.0.113.0/24,
 * 224.0.0.0/4 and 240.0.0.0/4 (which includes the broadcast address).
 *
 * Refused for IPv6: the unspecified and loopback addresses, fc00::/7
 * (unique local, where AWS puts its IPv6 metadata service), fe80::/10,
 * fec0::/10, ff00::/8, 2001:db8::/32, 100::/64, 2001::/32 (Teredo) and
 * the deprecated IPv4-compatible ::/96.  An address that carries an IPv4
 * address inside it -- IPv4-mapped ::ffff:0:0/96, NAT64 64:ff9b::/96 and
 * 6to4 2002::/16 -- is judged by the IPv4 address it carries, because
 * that is where the packet goes.
 *
 * Returns: %TRUE when the daemon may connect to @address for an agent
 */
gboolean clawt_net_address_is_public(GInetAddress *address);

/**
 * clawt_net_address_string_is_public:
 * @text: an address in text form
 *
 * clawt_net_address_is_public() for an address written as text.
 *
 * Returns: %TRUE when @text parses as an address and that address is
 *   public; %FALSE for anything else, a name included
 */
gboolean clawt_net_address_string_is_public(const gchar *text);

/**
 * clawt_net_url_refusal:
 * @url: (nullable): a URL an agent supplied
 *
 * Why the daemon will not fetch @url, judged from the URL alone, before
 * any name is looked up.
 *
 * Only `http` and `https`; no user name or password in the URL, since
 * the daemon never sends a credential on an agent's behalf; a host that
 * is present; and a host that is not `localhost`, a name under
 * `.localhost`, or an address literal clawt_net_address_is_public()
 * refuses.  A name that resolves to a private address is not caught here
 * -- that is clawt_net_address_is_public()'s job on every address the
 * name resolves to, at the time of the request.
 *
 * Returns: (transfer full) (nullable): a sentence saying why not, or
 *   %NULL when @url may be fetched
 */
gchar *clawt_net_url_refusal(const gchar *url);

G_END_DECLS
