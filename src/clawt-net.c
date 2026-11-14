/*
 * clawt-net.c - Which addresses the daemon may reach on an agent's behalf
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "clawt-net.h"

#include <string.h>

/*
 * One IPv4 range: a network in host byte order and a prefix length.
 *
 * Written as a prefix rather than as a first octet, because the one
 * range most likely to be got wrong -- 100.64.0.0/10, carrier-grade NAT
 * and the tailnet -- does not end on an octet boundary.  A check that
 * read "100.64.*" missed 100.65 to 100.127; one that read "100.*"
 * refused a quarter of a /8 that is ordinary public address space.
 */
typedef struct {
	guint32 network;
	guint   prefix;
} Ipv4Range;

#define IPV4(a, b, c, d) \
	((((guint32)(a)) << 24) | (((guint32)(b)) << 16) | \
	 (((guint32)(c)) << 8) | ((guint32)(d)))

static const Ipv4Range refused_ipv4[] = {
	{ IPV4(0, 0, 0, 0),       8 },	/* "this network" */
	{ IPV4(10, 0, 0, 0),      8 },	/* RFC 1918 */
	{ IPV4(100, 64, 0, 0),   10 },	/* carrier-grade NAT, the tailnet,
					 * Alibaba's metadata service */
	{ IPV4(127, 0, 0, 0),     8 },	/* loopback */
	{ IPV4(169, 254, 0, 0),  16 },	/* link-local and every cloud's
					 * 169.254.169.254 */
	{ IPV4(172, 16, 0, 0),   12 },	/* RFC 1918 */
	{ IPV4(192, 0, 0, 0),    24 },	/* IETF protocol assignments,
					 * Oracle's 192.0.0.192 */
	{ IPV4(192, 0, 2, 0),    24 },	/* TEST-NET-1 */
	{ IPV4(192, 88, 99, 0),  24 },	/* 6to4 relay anycast */
	{ IPV4(192, 168, 0, 0),  16 },	/* RFC 1918 */
	{ IPV4(198, 18, 0, 0),   15 },	/* benchmarking */
	{ IPV4(198, 51, 100, 0), 24 },	/* TEST-NET-2 */
	{ IPV4(203, 0, 113, 0),  24 },	/* TEST-NET-3 */
	{ IPV4(224, 0, 0, 0),     4 },	/* multicast */
	{ IPV4(240, 0, 0, 0),     4 }	/* reserved, and broadcast */
};

/*
 * The IPv4 rule, on an address already in host byte order.
 */
static gboolean
ipv4_is_public(
	guint32 value
){
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(refused_ipv4); i++) {
		guint32 mask;

		mask = (refused_ipv4[i].prefix == 0)
			? (guint32)0
			: (guint32)(0xffffffffu << (32 - refused_ipv4[i].prefix));

		if ((value & mask) == (refused_ipv4[i].network & mask))
			return FALSE;
	}

	return TRUE;
}

static guint32
ipv4_from_bytes(
	const guint8 *bytes
){
	return IPV4(bytes[0], bytes[1], bytes[2], bytes[3]);
}

/*
 * Whether the first @bits bits of @bytes match @prefix.
 */
static gboolean
ipv6_has_prefix(
	const guint8 *bytes,
	const guint8 *prefix,
	guint         bits
){
	guint whole = bits / 8;
	guint rest = bits % 8;

	if (whole > 0 && memcmp(bytes, prefix, whole) != 0)
		return FALSE;

	if (rest != 0) {
		guint8 mask = (guint8)(0xff << (8 - rest));

		if ((bytes[whole] & mask) != (prefix[whole] & mask))
			return FALSE;
	}

	return TRUE;
}

/*
 * The IPv6 rule.
 *
 * Three forms carry an IPv4 address and are judged by it, because that
 * is where the packet ends up: ::ffff:10.0.0.1 is 10.0.0.1 to every
 * dual-stack socket, a NAT64 gateway translates 64:ff9b::a00:1 to the
 * same, and 6to4 tunnels to the embedded address.  Refusing them
 * outright would also be safe; judging them is the same answer for the
 * private ones and the right one for the rest.
 */
static gboolean
ipv6_is_public(
	const guint8 *bytes
){
	static const guint8 zero[16] = { 0 };
	static const guint8 mapped[12] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff
	};
	static const guint8 nat64[12] = {
		0x00, 0x64, 0xff, 0x9b, 0, 0, 0, 0, 0, 0, 0, 0
	};
	static const guint8 six_to_four[2] = { 0x20, 0x02 };
	static const guint8 ula[1] = { 0xfc };
	static const guint8 link_local[2] = { 0xfe, 0x80 };
	static const guint8 site_local[2] = { 0xfe, 0xc0 };
	static const guint8 multicast[1] = { 0xff };
	static const guint8 documentation[4] = { 0x20, 0x01, 0x0d, 0xb8 };
	static const guint8 discard[8] = { 0x01, 0x00, 0, 0, 0, 0, 0, 0 };
	static const guint8 teredo[4] = { 0x20, 0x01, 0x00, 0x00 };

	/* ::ffff:a.b.c.d */
	if (memcmp(bytes, mapped, sizeof(mapped)) == 0)
		return ipv4_is_public(ipv4_from_bytes(bytes + 12));

	/* 64:ff9b::a.b.c.d */
	if (memcmp(bytes, nat64, sizeof(nat64)) == 0)
		return ipv4_is_public(ipv4_from_bytes(bytes + 12));

	/* 2002:aabb:ccdd::/48 carries a.b.c.d in its second and third words. */
	if (ipv6_has_prefix(bytes, six_to_four, 16))
		return ipv4_is_public(ipv4_from_bytes(bytes + 2));

	/*
	 * ::/96, which holds :: and ::1 and the deprecated IPv4-compatible
	 * form.  None of it is a public destination.
	 */
	if (ipv6_has_prefix(bytes, zero, 96))
		return FALSE;

	if (ipv6_has_prefix(bytes, ula, 7) ||
	    ipv6_has_prefix(bytes, link_local, 10) ||
	    ipv6_has_prefix(bytes, site_local, 10) ||
	    ipv6_has_prefix(bytes, multicast, 8) ||
	    ipv6_has_prefix(bytes, documentation, 32) ||
	    ipv6_has_prefix(bytes, discard, 64) ||
	    ipv6_has_prefix(bytes, teredo, 32))
		return FALSE;

	return TRUE;
}

gboolean
clawt_net_address_is_public(
	GInetAddress *address
){
	const guint8 *bytes;

	g_return_val_if_fail(G_IS_INET_ADDRESS(address), FALSE);

	bytes = g_inet_address_to_bytes(address);

	switch (g_inet_address_get_family(address)) {
	case G_SOCKET_FAMILY_IPV4:
		return ipv4_is_public(ipv4_from_bytes(bytes));

	case G_SOCKET_FAMILY_IPV6:
		return ipv6_is_public(bytes);

	case G_SOCKET_FAMILY_INVALID:
	case G_SOCKET_FAMILY_UNIX:
		return FALSE;
	}

	return FALSE;
}

gboolean
clawt_net_address_string_is_public(
	const gchar *text
){
	g_autoptr(GInetAddress) address = NULL;

	if (text == NULL)
		return FALSE;

	address = g_inet_address_new_from_string(text);

	if (address == NULL)
		return FALSE;

	return clawt_net_address_is_public(address);
}

gchar *
clawt_net_url_refusal(
	const gchar *url
){
	g_autoptr(GUri) uri = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *host = NULL;
	const gchar *scheme;

	if (url == NULL || *url == '\0')
		return g_strdup("A URL is needed.");

	uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error);

	if (uri == NULL)
		return g_strdup_printf("That is not a URL clawtilla can read: %s",
		                       error->message);

	scheme = g_uri_get_scheme(uri);

	if (g_ascii_strcasecmp(scheme, "http") != 0 &&
	    g_ascii_strcasecmp(scheme, "https") != 0)
		return g_strdup_printf(
			"Only http and https URLs can be checked, not %s.", scheme);

	/*
	 * A credential in the URL is still a credential, and this request
	 * is made with the daemon's network position and nobody's identity.
	 */
	if (g_uri_get_userinfo(uri) != NULL)
		return g_strdup(
			"That URL carries a user name or password. clawtilla checks "
			"URLs without credentials, so leave them out.");

	if (g_uri_get_host(uri) == NULL || *g_uri_get_host(uri) == '\0')
		return g_strdup("That URL names no host.");

	host = g_ascii_strdown(g_uri_get_host(uri), -1);

	/* GUri keeps the brackets off an IPv6 literal; a zone id stays on. */
	if (strchr(host, '%') != NULL)
		return g_strdup("A link-local address cannot be checked.");

	if (g_strcmp0(host, "localhost") == 0 ||
	    g_str_has_suffix(host, ".localhost") ||
	    g_strcmp0(host, "localhost.") == 0)
		return g_strdup(
			"That URL is this machine. clawtilla only checks addresses "
			"on the public internet.");

	if (g_hostname_is_ip_address(host) &&
	    !clawt_net_address_string_is_public(host))
		return g_strdup_printf(
			"%s is a private, local or reserved address. clawtilla only "
			"checks addresses on the public internet.", host);

	return NULL;
}
