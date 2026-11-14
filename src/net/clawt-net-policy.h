/*
 * clawt-net-policy.h - Which addresses an agent may connect to
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A name is a claim and an address is a fact.  Every rule here is about
 * the address a connection actually goes to, after resolution, because
 * a public name can resolve to 127.0.0.1 (DNS rebinding), and a rule
 * phrased in names is a rule an attacker writes the input for.
 *
 * The classification is pure -- no resolver, no socket -- so the whole
 * table is testable as data.  clawt_net_address_is_public() (the private
 * clawt-net.h, which the wait probes use) is this table's yes or no, so
 * the gate and the probes cannot disagree about what "private" means.  100.64.0.0/10 is not an octet boundary:
 * 100.63.255.255 is public and 100.128.0.0 is public, and a check written
 * as "starts with 100." is wrong in both directions.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>
#include <gio/gio.h>

#include "clawt-enums.h"

G_BEGIN_DECLS

/**
 * clawt_net_address_classify:
 * @address: an address
 *
 * What kind of place @address is.
 *
 * An IPv6 address that carries an IPv4 one -- v4-mapped (::ffff:a.b.c.d),
 * v4-compatible (::a.b.c.d), NAT64 (64:ff9b::/96) or 6to4 (2002::/16) --
 * is classified by the IPv4 address inside it, since that is where a
 * kernel with the matching route sends it.  Teredo (2001::/32) hides its
 * client address and is reserved.
 *
 * Returns: the class
 */
ClawtNetAddressClass
clawt_net_address_classify(
	GInetAddress *address
);

/**
 * clawt_net_address_classify_string:
 * @text: an IPv4 or IPv6 address literal, without brackets
 * @out_class: (out) (optional): where to store the class
 *
 * clawt_net_address_classify() for a string.
 *
 * Returns: %TRUE if @text is an address literal
 */
gboolean
clawt_net_address_classify_string(
	const gchar          *text,
	ClawtNetAddressClass *out_class
);

/**
 * clawt_net_address_class_describe:
 * @klass: an address class
 *
 * A few words naming @klass for a person: "this machine (loopback)",
 * "the local network (RFC 1918)".  Used in refusals, which an agent and
 * its operator both read.
 *
 * Returns: (transfer none): the description
 */
const gchar *
clawt_net_address_class_describe(
	ClawtNetAddressClass klass
);

/**
 * clawt_net_address_class_is_allowable:
 * @klass: an address class
 *
 * Whether an operator's allowlist may open @klass at all.
 *
 * A tailnet service, a NAS on the LAN and a port on this machine are
 * things somebody can reasonably mean to let an agent reach.  A metadata
 * service, multicast, the unspecified address and reserved space are
 * not, and naming them in `egress.allow` is refused rather than obeyed.
 *
 * Returns: %TRUE if an allowlist entry can permit @klass
 */
gboolean
clawt_net_address_class_is_allowable(
	ClawtNetAddressClass klass
);

/**
 * ClawtEgressAllowlist:
 *
 * The addresses and names an operator explicitly lets a gated agent
 * reach although they are not public -- `egress.allow`.  Opaque and
 * immutable once parsed.
 */
typedef struct _ClawtEgressAllowlist ClawtEgressAllowlist;

#define CLAWT_TYPE_EGRESS_ALLOWLIST (clawt_egress_allowlist_get_type())

/**
 * clawt_egress_allowlist_get_type:
 *
 * Returns: the #GType of #ClawtEgressAllowlist
 */
GType clawt_egress_allowlist_get_type(void) G_GNUC_CONST;

/**
 * clawt_egress_allowlist_new:
 * @entries: (array zero-terminated=1) (nullable): the entries, as written
 *   in `egress.allow`
 * @error: (out) (optional): return location for a #GError
 *
 * Parses an allowlist.  Each entry is one of:
 *
 * - an address: `100.101.102.103`, `fd7a:115c:a1e0::1`
 * - a network: `192.168.1.0/24`, `fd7a:115c:a1e0::/48`
 * - a name: `nas.example.ts.net`, matched against the name an agent
 *   asked for, case-insensitively, and never opening loopback
 *
 * optionally followed by a port: `10.0.0.5:8080`, `[fd00::1]:443`,
 * `[fd00::/64]:443`, `nas.lan:5000`.  An IPv6 entry with a port needs
 * the brackets, since its own colons cannot be told from the port's.
 *
 * One bad entry refuses the whole list, naming it: a list that quietly
 * dropped a typo would be a list that permits less than its author
 * believes -- the safe direction, and still a lie.
 *
 * Returns: (transfer full) (nullable): the allowlist, or %NULL with
 *   @error set
 */
ClawtEgressAllowlist *
clawt_egress_allowlist_new(
	const gchar * const  *entries,
	GError              **error
);

/**
 * clawt_egress_allowlist_check_entry:
 * @entry: one entry
 * @error: (out) (optional): return location for a #GError
 *
 * Whether @entry would parse, for refusing it where it arrives.
 *
 * Returns: %TRUE if @entry is valid
 */
gboolean
clawt_egress_allowlist_check_entry(
	const gchar  *entry,
	GError      **error
);

/**
 * clawt_egress_allowlist_ref:
 * @self: an allowlist
 *
 * Returns: (transfer full): @self
 */
ClawtEgressAllowlist *
clawt_egress_allowlist_ref(
	ClawtEgressAllowlist *self
);

/**
 * clawt_egress_allowlist_unref:
 * @self: an allowlist
 */
void
clawt_egress_allowlist_unref(
	ClawtEgressAllowlist *self
);

/**
 * clawt_egress_allowlist_permits:
 * @self: (nullable): an allowlist; %NULL permits nothing
 * @host: the name or literal the agent asked for
 * @address: the address it resolved to
 * @port: the port
 *
 * Whether an entry permits connecting to @address on @port when the
 * agent asked for @host.  Never %TRUE for a class
 * clawt_net_address_class_is_allowable() refuses, and a name entry never
 * opens loopback: a name is the agent's input, and a name somebody
 * pointed at 127.0.0.1 is the attack the gate exists to stop.
 *
 * Returns: %TRUE if permitted
 */
gboolean
clawt_egress_allowlist_permits(
	ClawtEgressAllowlist *self,
	const gchar          *host,
	GInetAddress         *address,
	guint16               port
);

/**
 * clawt_egress_decide:
 * @host: the name or literal the agent asked for
 * @port: the port
 * @addresses: (element-type GInetAddress): every address @host resolved to
 * @allow: (nullable): the operator's allowlist
 *
 * Why a connection to @host:@port is refused, or %NULL when it may go
 * ahead.
 *
 * Every address must pass, not just the first: the gate connects only to
 * addresses this has seen, and a name that resolves to one public and one
 * private address is a name somebody built to have the private one tried.
 *
 * Returns: (transfer full) (nullable): the reason, or %NULL to allow
 */
gchar *
clawt_egress_decide(
	const gchar          *host,
	guint16               port,
	GPtrArray            *addresses,
	ClawtEgressAllowlist *allow
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtEgressAllowlist, clawt_egress_allowlist_unref)

G_END_DECLS
