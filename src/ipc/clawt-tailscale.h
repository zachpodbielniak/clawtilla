/*
 * clawt-tailscale.h - Finding the address the tailnet reaches us on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A tailnet is the one network where listening beyond the loopback is
 * defensible without a lot of ceremony: every peer is a device the user
 * has enrolled and WireGuard has authenticated, and nothing else can
 * route to a 100.64/10 address at all.  So clawtillad binds its tailnet
 * address as well as its unix socket, and the desktop client on a laptop
 * can reach the fleet on a workstation without a tunnel set up by hand.
 *
 * The address is found with getifaddrs(), not by running `tailscale ip`.
 * Daemon start may not wait on anything that can hang, and a subprocess
 * talking to a wedged tailscaled hangs exactly as well as a network call
 * does -- the same rule that moved the model cache out of
 * clawt_daemon_start().  An interface list is a syscall against data the
 * kernel already has.
 *
 * That choice has one consequence worth knowing: in userspace-networking
 * mode there is no tailscale0 and so no address here.  That is correct
 * rather than a gap, because in that mode there is no address to bind
 * either.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_tailscale_is_tailnet_address:
 * @address: a numeric address, or %NULL
 *
 * Whether @address is one Tailscale hands out.
 *
 * That is 100.64.0.0/10 for IPv4 -- the CGNAT range Tailscale took for
 * itself -- and fd7a:115c:a1e0::/48 for IPv6.  Checked because binding
 * whatever an interface happens to hold is how a daemon ends up on a
 * café's wifi: an interface named tailscale0 is not proof that the
 * address on it is a tailnet one.
 *
 * Returns: %TRUE if @address is inside a Tailscale range
 */
gboolean clawt_tailscale_is_tailnet_address(const gchar *address);

/**
 * clawt_tailscale_is_tailnet_interface:
 * @name: an interface name, or %NULL
 *
 * Whether @name is the interface Tailscale creates.
 *
 * `tailscale0` on Linux, `utun*` on the BSDs -- which is why the address
 * range is checked too, since utun is the generic tunnel name there.
 *
 * Returns: %TRUE if @name looks like Tailscale's own interface
 */
gboolean clawt_tailscale_is_tailnet_interface(const gchar *name);

/**
 * clawt_tailscale_find_address:
 *
 * The address this machine is reachable on from its tailnet.
 *
 * Returns: (transfer full) (nullable): the address, or %NULL when
 *   Tailscale is not installed, not up, or in userspace-networking mode
 */
gchar *clawt_tailscale_find_address(void);

G_END_DECLS
