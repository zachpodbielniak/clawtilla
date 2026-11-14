/*
 * clawt-net-policy.c - Which addresses an agent may connect to
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "net/clawt-net-policy.h"

#include <string.h>

/* ── Classification ──────────────────────────────────────────────── */

/*
 * One IPv4 address, as four bytes.  Separate from the GInetAddress entry
 * point because IPv6 hands its embedded IPv4 addresses down here, and
 * those are bytes inside another address rather than an object.
 */
static ClawtNetAddressClass
classify_v4(
	const guint8 *b
){
	/* 0.0.0.0 is "this host" to connect(); the rest of 0/8 is reserved. */
	if (b[0] == 0 && b[1] == 0 && b[2] == 0 && b[3] == 0)
		return CLAWT_NET_ADDRESS_UNSPECIFIED;

	if (b[0] == 0)
		return CLAWT_NET_ADDRESS_RESERVED;

	if (b[0] == 127)
		return CLAWT_NET_ADDRESS_LOOPBACK;

	if (b[0] == 10 ||
	    (b[0] == 172 && (b[1] & 0xf0) == 16) ||
	    (b[0] == 192 && b[1] == 168))
		return CLAWT_NET_ADDRESS_PRIVATE;

	/*
	 * Metadata before link-local, because it is inside it and is the
	 * one address there an allowlist must never be able to open: AWS,
	 * GCP and Azure all answer credentials on 169.254.169.254, and ECS
	 * hands out task credentials on 169.254.170.2.
	 */
	if (b[0] == 169 && b[1] == 254 &&
	    ((b[2] == 169 && b[3] == 254) || (b[2] == 170 && b[3] == 2)))
		return CLAWT_NET_ADDRESS_METADATA;

	if (b[0] == 169 && b[1] == 254)
		return CLAWT_NET_ADDRESS_LINK_LOCAL;

	/*
	 * 100.64.0.0/10: the second octet's top two bits are 01, so 64
	 * through 127.  Not "starts with 100." -- 100.63.x and 100.128.x
	 * are public.  Alibaba Cloud's metadata service sits inside it.
	 */
	if (b[0] == 100 && b[1] == 100 && b[2] == 100 && b[3] == 200)
		return CLAWT_NET_ADDRESS_METADATA;

	if (b[0] == 100 && (b[1] & 0xc0) == 64)
		return CLAWT_NET_ADDRESS_CGNAT;

	if (b[0] >= 224 && b[0] <= 239)
		return CLAWT_NET_ADDRESS_MULTICAST;

	if (b[0] == 255 && b[1] == 255 && b[2] == 255 && b[3] == 255)
		return CLAWT_NET_ADDRESS_MULTICAST;

	if (b[0] >= 240)
		return CLAWT_NET_ADDRESS_RESERVED;

	/*
	 * IETF protocol assignments (192.0.0.0/24), the three documentation
	 * networks, benchmarking (198.18.0.0/15, which fake-IP proxies also
	 * answer every name from) and the retired 6to4 relay anycast.
	 */
	if ((b[0] == 192 && b[1] == 0 && b[2] == 0) ||
	    (b[0] == 192 && b[1] == 0 && b[2] == 2) ||
	    (b[0] == 198 && b[1] == 51 && b[2] == 100) ||
	    (b[0] == 203 && b[1] == 0 && b[2] == 113) ||
	    (b[0] == 198 && (b[1] & 0xfe) == 18) ||
	    (b[0] == 192 && b[1] == 88 && b[2] == 99))
		return CLAWT_NET_ADDRESS_RESERVED;

	return CLAWT_NET_ADDRESS_PUBLIC;
}

static gboolean
all_zero(
	const guint8 *b,
	gsize         length
){
	gsize i;

	for (i = 0; i < length; i++)
		if (b[i] != 0)
			return FALSE;

	return TRUE;
}

static ClawtNetAddressClass
classify_v6(
	const guint8 *b
){
	static const guint8 metadata[16] = {
		0xfd, 0x00, 0x0e, 0xc2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x54
	};

	if (all_zero(b, 16))
		return CLAWT_NET_ADDRESS_UNSPECIFIED;

	if (all_zero(b, 15) && b[15] == 1)
		return CLAWT_NET_ADDRESS_LOOPBACK;

	/*
	 * The forms that carry an IPv4 address, each classified by what it
	 * carries.  Without this ::ffff:127.0.0.1 is "some IPv6 address",
	 * and a dual-stack socket connects it to this machine.
	 */
	if (all_zero(b, 10) && b[10] == 0xff && b[11] == 0xff)
		return classify_v4(b + 12);

	if (all_zero(b, 12))
		return classify_v4(b + 12);

	if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b &&
	    all_zero(b + 4, 8))
		return classify_v4(b + 12);

	if (b[0] == 0x20 && b[1] == 0x02)
		return classify_v4(b + 2);

	/* The local-use NAT64 prefix, 64:ff9b:1::/48, can carry anything. */
	if (b[0] == 0x00 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b)
		return CLAWT_NET_ADDRESS_RESERVED;

	if (memcmp(b, metadata, 16) == 0)
		return CLAWT_NET_ADDRESS_METADATA;

	if ((b[0] & 0xfe) == 0xfc)
		return CLAWT_NET_ADDRESS_UNIQUE_LOCAL;

	if (b[0] == 0xfe && (b[1] & 0xc0) == 0x80)
		return CLAWT_NET_ADDRESS_LINK_LOCAL;

	if (b[0] == 0xfe && (b[1] & 0xc0) == 0xc0)
		return CLAWT_NET_ADDRESS_PRIVATE;

	if (b[0] == 0xff)
		return CLAWT_NET_ADDRESS_MULTICAST;

	/* Teredo hides its client; documentation; the discard prefix. */
	if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x00 && b[3] == 0x00)
		return CLAWT_NET_ADDRESS_RESERVED;

	if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0x0d && b[3] == 0xb8)
		return CLAWT_NET_ADDRESS_RESERVED;

	if (b[0] == 0x01 && b[1] == 0x00 && all_zero(b + 2, 6))
		return CLAWT_NET_ADDRESS_RESERVED;

	/* Everything outside global unicast, 2000::/3, is not the internet. */
	if ((b[0] & 0xe0) != 0x20)
		return CLAWT_NET_ADDRESS_RESERVED;

	return CLAWT_NET_ADDRESS_PUBLIC;
}

ClawtNetAddressClass
clawt_net_address_classify(
	GInetAddress *address
){
	const guint8 *bytes;

	g_return_val_if_fail(G_IS_INET_ADDRESS(address),
	                     CLAWT_NET_ADDRESS_RESERVED);

	bytes = g_inet_address_to_bytes(address);

	if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4)
		return classify_v4(bytes);

	return classify_v6(bytes);
}

gboolean
clawt_net_address_classify_string(
	const gchar          *text,
	ClawtNetAddressClass *out_class
){
	g_autoptr(GInetAddress) address = NULL;

	if (text == NULL)
		return FALSE;

	address = g_inet_address_new_from_string(text);

	if (address == NULL)
		return FALSE;

	if (out_class != NULL)
		*out_class = clawt_net_address_classify(address);

	return TRUE;
}

gboolean
clawt_net_address_is_public(
	GInetAddress *address
){
	return clawt_net_address_classify(address) == CLAWT_NET_ADDRESS_PUBLIC;
}

const gchar *
clawt_net_address_class_describe(
	ClawtNetAddressClass klass
){
	switch (klass) {
	case CLAWT_NET_ADDRESS_PUBLIC:
		return "the public internet";
	case CLAWT_NET_ADDRESS_LOOPBACK:
		return "this machine (loopback)";
	case CLAWT_NET_ADDRESS_PRIVATE:
		return "the local network (RFC 1918)";
	case CLAWT_NET_ADDRESS_LINK_LOCAL:
		return "a link-local address";
	case CLAWT_NET_ADDRESS_CGNAT:
		return "carrier-grade NAT or a tailnet (100.64.0.0/10)";
	case CLAWT_NET_ADDRESS_METADATA:
		return "a cloud metadata service";
	case CLAWT_NET_ADDRESS_UNIQUE_LOCAL:
		return "an IPv6 local network (fc00::/7)";
	case CLAWT_NET_ADDRESS_MULTICAST:
		return "a multicast or broadcast address";
	case CLAWT_NET_ADDRESS_UNSPECIFIED:
		return "the unspecified address, which means this machine";
	case CLAWT_NET_ADDRESS_RESERVED:
		return "reserved address space";
	}

	return "an unknown kind of address";
}

gboolean
clawt_net_address_class_is_allowable(
	ClawtNetAddressClass klass
){
	switch (klass) {
	case CLAWT_NET_ADDRESS_PUBLIC:
	case CLAWT_NET_ADDRESS_LOOPBACK:
	case CLAWT_NET_ADDRESS_PRIVATE:
	case CLAWT_NET_ADDRESS_LINK_LOCAL:
	case CLAWT_NET_ADDRESS_CGNAT:
	case CLAWT_NET_ADDRESS_UNIQUE_LOCAL:
		return TRUE;
	case CLAWT_NET_ADDRESS_METADATA:
	case CLAWT_NET_ADDRESS_MULTICAST:
	case CLAWT_NET_ADDRESS_UNSPECIFIED:
	case CLAWT_NET_ADDRESS_RESERVED:
		return FALSE;
	}

	return FALSE;
}

/* ── The allowlist ───────────────────────────────────────────────── */

typedef struct {
	gchar  *name;          /* lower-case, no trailing dot; NULL for a network */
	guint8  network[16];
	gsize   length;        /* 4 or 16; 0 for a name */
	guint   prefix;
	guint16 port;          /* 0 = any */
} AllowEntry;

struct _ClawtEgressAllowlist {
	gint    ref_count;
	GArray *entries;       /* AllowEntry */
};

G_DEFINE_BOXED_TYPE(ClawtEgressAllowlist, clawt_egress_allowlist,
                    clawt_egress_allowlist_ref, clawt_egress_allowlist_unref)

static void
allow_entry_clear(
	gpointer data
){
	AllowEntry *entry = data;

	g_clear_pointer(&entry->name, g_free);
}

/*
 * The bytes an address is compared by: an IPv4-mapped IPv6 address is
 * its IPv4 address, so an entry for 10.0.0.5 means the same host however
 * a resolver or a literal happened to spell it.
 */
static gsize
address_bytes(
	GInetAddress *address,
	guint8       *out
){
	const guint8 *b = g_inet_address_to_bytes(address);

	if (g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV4) {
		memcpy(out, b, 4);
		return 4;
	}

	if (all_zero(b, 10) && b[10] == 0xff && b[11] == 0xff) {
		memcpy(out, b + 12, 4);
		return 4;
	}

	memcpy(out, b, 16);
	return 16;
}

static gboolean
prefix_matches(
	const guint8 *network,
	const guint8 *candidate,
	guint         prefix
){
	guint whole = prefix / 8;
	guint rest = prefix % 8;
	guint8 mask;

	if (whole > 0 && memcmp(network, candidate, whole) != 0)
		return FALSE;

	if (rest == 0)
		return TRUE;

	mask = (guint8)(0xff << (8 - rest));

	return (network[whole] & mask) == (candidate[whole] & mask);
}

static gboolean
valid_name(
	const gchar *name
){
	g_auto(GStrv) labels = NULL;
	gsize length = strlen(name);
	guint i;

	if (length == 0 || length > 253)
		return FALSE;

	labels = g_strsplit(name, ".", -1);

	for (i = 0; labels[i] != NULL; i++) {
		const gchar *p;
		gsize label_length = strlen(labels[i]);

		if (label_length == 0 || label_length > 63)
			return FALSE;

		for (p = labels[i]; *p != '\0'; p++)
			if (!g_ascii_isalnum(*p) && *p != '-' && *p != '_')
				return FALSE;

		if (labels[i][0] == '-' || labels[i][label_length - 1] == '-')
			return FALSE;
	}

	return TRUE;
}

static gboolean
parse_port(
	const gchar  *text,
	guint16      *out
){
	guint64 value = 0;

	if (text == NULL || *text == '\0')
		return FALSE;

	if (!g_ascii_string_to_unsigned(text, 10, 1, 65535, &value, NULL))
		return FALSE;

	*out = (guint16)value;
	return TRUE;
}

/*
 * An address or address/prefix, into @entry.  FALSE when it is neither,
 * which for a part with no slash may still be a name.
 */
static gboolean
parse_network(
	const gchar  *text,
	AllowEntry   *entry,
	const gchar  *whole,
	GError      **error
){
	g_autofree gchar *address_text = NULL;
	g_autoptr(GInetAddress) address = NULL;
	const gchar *slash = strchr(text, '/');
	guint max;

	address_text = (slash != NULL) ? g_strndup(text, (gsize)(slash - text))
	                               : g_strdup(text);
	address = g_inet_address_new_from_string(address_text);

	if (address == NULL) {
		if (slash != NULL)
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "egress.allow entry '%s': %s is not an address, so "
			            "it cannot have a prefix", whole, address_text);
		return FALSE;
	}

	entry->length = address_bytes(address, entry->network);
	max = (guint)entry->length * 8;
	entry->prefix = max;

	if (slash != NULL) {
		guint64 prefix = 0;

		if (!g_ascii_string_to_unsigned(slash + 1, 10, 0, max, &prefix,
		                                NULL)) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "egress.allow entry '%s': the prefix must be a "
			            "number from 0 to %u", whole, max);
			return FALSE;
		}

		/* A mapped address came down to four bytes; so does its prefix. */
		if (entry->length == 4 &&
		    g_inet_address_get_family(address) == G_SOCKET_FAMILY_IPV6) {
			if (prefix < 96) {
				g_set_error(error, CLAWT_ERROR,
				            CLAWT_ERROR_INVALID_ARGUMENT,
				            "egress.allow entry '%s': write an IPv4 "
				            "network as IPv4", whole);
				return FALSE;
			}
			prefix -= 96;
		}

		entry->prefix = (guint)prefix;
	}

	/*
	 * Refused here, not merely ignored at match time: an operator who
	 * wrote 169.254.169.254 believes they opened it, and the remedy is
	 * to tell them they cannot.  A network is judged by its own address,
	 * and permits() checks each address again, so 169.254.0.0/16 opens
	 * link-local and still not the metadata service inside it.
	 */
	{
		ClawtNetAddressClass klass = clawt_net_address_classify(address);

		if (!clawt_net_address_class_is_allowable(klass)) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "egress.allow entry '%s' is %s, which an "
			            "allowlist cannot open", whole,
			            clawt_net_address_class_describe(klass));
			return FALSE;
		}
	}

	return TRUE;
}

static gboolean
parse_entry(
	const gchar  *raw,
	AllowEntry   *entry,
	GError      **error
){
	g_autofree gchar *text = NULL;
	g_autofree gchar *host = NULL;
	const gchar *port_text = NULL;
	guint colons = 0;
	const gchar *p;

	memset(entry, 0, sizeof(*entry));

	if (raw == NULL) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "an egress.allow entry is empty");
		return FALSE;
	}

	text = g_strstrip(g_strdup(raw));

	if (*text == '\0') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "an egress.allow entry is empty");
		return FALSE;
	}

	for (p = text; *p != '\0'; p++)
		if (*p == ':')
			colons++;

	/*
	 * Three shapes: [v6 or v6/prefix] with an optional :port; a bare
	 * IPv6 address or network (two or more colons, so no port can be
	 * told apart); anything else, whose last colon is a port.
	 */
	if (text[0] == '[') {
		const gchar *close = strchr(text, ']');

		if (close == NULL || (close[1] != '\0' && close[1] != ':')) {
			g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
			            "egress.allow entry '%s': a bracketed address is "
			            "[address] or [address]:port", raw);
			return FALSE;
		}

		host = g_strndup(text + 1, (gsize)(close - text - 1));

		if (close[1] == ':')
			port_text = close + 2;
	} else if (colons >= 2) {
		host = g_strdup(text);
	} else if (colons == 1) {
		const gchar *colon = strchr(text, ':');

		host = g_strndup(text, (gsize)(colon - text));
		port_text = colon + 1;
	} else {
		host = g_strdup(text);
	}

	if (port_text != NULL && !parse_port(port_text, &entry->port)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "egress.allow entry '%s': the port must be a number "
		            "from 1 to 65535", raw);
		return FALSE;
	}

	{
		GError *local = NULL;

		if (parse_network(host, entry, raw, &local))
			return TRUE;

		if (local != NULL) {
			g_propagate_error(error, local);
			return FALSE;
		}
	}

	/* Not an address, so a name -- or nothing. */
	{
		gsize length = strlen(host);

		if (length > 0 && host[length - 1] == '.')
			host[length - 1] = '\0';
	}

	if (text[0] == '[' || !valid_name(host)) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "egress.allow entry '%s' is not an address, a network "
		            "(address/prefix) or a host name", raw);
		return FALSE;
	}

	entry->name = g_ascii_strdown(host, -1);
	return TRUE;
}

gboolean
clawt_egress_allowlist_check_entry(
	const gchar  *entry,
	GError      **error
){
	AllowEntry parsed;
	gboolean ok;

	ok = parse_entry(entry, &parsed, error);
	allow_entry_clear(&parsed);

	return ok;
}

ClawtEgressAllowlist *
clawt_egress_allowlist_new(
	const gchar * const  *entries,
	GError              **error
){
	g_autoptr(ClawtEgressAllowlist) self = NULL;
	gsize i;

	self = g_new0(ClawtEgressAllowlist, 1);
	self->ref_count = 1;
	self->entries = g_array_new(FALSE, TRUE, sizeof(AllowEntry));
	g_array_set_clear_func(self->entries, allow_entry_clear);

	for (i = 0; entries != NULL && entries[i] != NULL; i++) {
		AllowEntry entry;

		if (!parse_entry(entries[i], &entry, error)) {
			allow_entry_clear(&entry);
			return NULL;
		}

		g_array_append_val(self->entries, entry);
	}

	return g_steal_pointer(&self);
}

ClawtEgressAllowlist *
clawt_egress_allowlist_ref(
	ClawtEgressAllowlist *self
){
	g_return_val_if_fail(self != NULL, NULL);

	g_atomic_int_inc(&self->ref_count);
	return self;
}

void
clawt_egress_allowlist_unref(
	ClawtEgressAllowlist *self
){
	if (self == NULL)
		return;

	if (!g_atomic_int_dec_and_test(&self->ref_count))
		return;

	g_array_unref(self->entries);
	g_free(self);
}

gboolean
clawt_egress_allowlist_permits(
	ClawtEgressAllowlist *self,
	const gchar          *host,
	GInetAddress         *address,
	guint16               port
){
	ClawtNetAddressClass klass;
	g_autofree gchar *wanted = NULL;
	guint8 bytes[16];
	gsize length;
	guint i;

	if (self == NULL || address == NULL)
		return FALSE;

	klass = clawt_net_address_classify(address);

	if (!clawt_net_address_class_is_allowable(klass))
		return FALSE;

	length = address_bytes(address, bytes);

	if (host != NULL) {
		gsize host_length;

		wanted = g_ascii_strdown(host, -1);
		host_length = strlen(wanted);

		if (host_length > 0 && wanted[host_length - 1] == '.')
			wanted[host_length - 1] = '\0';
	}

	for (i = 0; i < self->entries->len; i++) {
		const AllowEntry *entry = &g_array_index(self->entries,
		                                         AllowEntry, i);

		if (entry->port != 0 && entry->port != port)
			continue;

		if (entry->name != NULL) {
			if (klass == CLAWT_NET_ADDRESS_LOOPBACK)
				continue;

			if (wanted != NULL && g_strcmp0(entry->name, wanted) == 0)
				return TRUE;

			continue;
		}

		if (entry->length != length)
			continue;

		if (prefix_matches(entry->network, bytes, entry->prefix))
			return TRUE;
	}

	return FALSE;
}

/* ── The decision ────────────────────────────────────────────────── */

gchar *
clawt_egress_decide(
	const gchar          *host,
	guint16               port,
	GPtrArray            *addresses,
	ClawtEgressAllowlist *allow
){
	guint i;

	if (port == 0)
		return g_strdup("port 0 is not a destination");

	if (addresses == NULL || addresses->len == 0)
		return g_strdup_printf("%s did not resolve to any address",
		                       host != NULL ? host : "the host");

	for (i = 0; i < addresses->len; i++) {
		GInetAddress *address = g_ptr_array_index(addresses, i);
		ClawtNetAddressClass klass = clawt_net_address_classify(address);
		g_autofree gchar *text = NULL;

		if (klass == CLAWT_NET_ADDRESS_PUBLIC)
			continue;

		if (clawt_egress_allowlist_permits(allow, host, address, port))
			continue;

		text = g_inet_address_to_string(address);

		/*
		 * The address is named as well as the name, because "refused"
		 * about a public-looking name is exactly what DNS rebinding
		 * produces, and the address is what says so.
		 */
		if (host != NULL && g_ascii_strcasecmp(host, text) != 0)
			return g_strdup_printf("%s resolves to %s, which is %s",
			                       host, text,
			                       clawt_net_address_class_describe(klass));

		return g_strdup_printf("%s is %s", text,
		                       clawt_net_address_class_describe(klass));
	}

	return NULL;
}
