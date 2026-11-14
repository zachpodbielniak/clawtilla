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
 * The table is clawt_net_address_classify() (net/clawt-net-policy.c),
 * which the egress gate also uses to say *why* an address is refused and
 * whether an allowlist may open it.  This is its yes or no.  Two tables
 * were written -- this file's, for waits, and the gate's -- and they were
 * merged into one rather than kept in step: the comment above the wait
 * probes said the gate "must not grow a second opinion about what
 * private means", and two tables are two opinions however carefully they
 * agree today.
 */
gboolean
clawt_net_address_is_public(
	GInetAddress *address
){
	g_return_val_if_fail(G_IS_INET_ADDRESS(address), FALSE);

	return clawt_net_address_classify(address) == CLAWT_NET_ADDRESS_PUBLIC;
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
