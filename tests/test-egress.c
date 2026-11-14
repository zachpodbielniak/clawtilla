/*
 * test-egress.c - The egress gate and the provider-key broker, as data
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: the address table, the allowlist, the CONNECT parser, the
 * refusal decision, the refusal bookkeeping, the broker's environment
 * rewrite and its token swap are all pure functions, and the process
 * runtime's gated command is read through clawt_process_runtime_prepare()
 * with a stub bwrap and a stub relay -- nothing is resolved and nothing
 * connects.  The one socket is the broker's, a unix socket in a
 * temporary directory that nothing dials.  The tests that run a real
 * gate and a real box are in test-egress-integration.c, behind
 * CLAWT_TEST_INTEGRATION=1.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"
#include "clawt-net.h"

/* ── The address table ───────────────────────────────────────────── */

typedef struct {
	const gchar          *address;
	ClawtNetAddressClass  klass;
} AddressCase;

/*
 * Each class, its edges, and every IPv6 form that carries an IPv4
 * address -- the forms a check written for IPv4 alone lets through.
 */
static const AddressCase address_cases[] = {
	{ "8.8.8.8",                  CLAWT_NET_ADDRESS_PUBLIC },
	{ "1.1.1.1",                  CLAWT_NET_ADDRESS_PUBLIC },
	{ "127.0.0.1",                CLAWT_NET_ADDRESS_LOOPBACK },
	{ "127.255.0.1",              CLAWT_NET_ADDRESS_LOOPBACK },
	{ "10.1.2.3",                 CLAWT_NET_ADDRESS_PRIVATE },
	{ "172.16.0.1",               CLAWT_NET_ADDRESS_PRIVATE },
	{ "172.31.255.255",           CLAWT_NET_ADDRESS_PRIVATE },
	{ "172.15.255.255",           CLAWT_NET_ADDRESS_PUBLIC },
	{ "172.32.0.1",               CLAWT_NET_ADDRESS_PUBLIC },
	{ "192.168.1.1",              CLAWT_NET_ADDRESS_PRIVATE },
	{ "169.254.1.1",              CLAWT_NET_ADDRESS_LINK_LOCAL },
	{ "169.254.169.254",          CLAWT_NET_ADDRESS_METADATA },
	{ "169.254.170.2",            CLAWT_NET_ADDRESS_METADATA },
	/* 100.64.0.0/10 is not an octet boundary. */
	{ "100.64.0.1",               CLAWT_NET_ADDRESS_CGNAT },
	{ "100.101.102.103",          CLAWT_NET_ADDRESS_CGNAT },
	{ "100.127.255.255",          CLAWT_NET_ADDRESS_CGNAT },
	{ "100.63.255.255",           CLAWT_NET_ADDRESS_PUBLIC },
	{ "100.128.0.0",              CLAWT_NET_ADDRESS_PUBLIC },
	{ "100.100.100.200",          CLAWT_NET_ADDRESS_METADATA },
	{ "0.0.0.0",                  CLAWT_NET_ADDRESS_UNSPECIFIED },
	{ "0.1.2.3",                  CLAWT_NET_ADDRESS_RESERVED },
	{ "224.0.0.1",                CLAWT_NET_ADDRESS_MULTICAST },
	{ "239.255.255.250",          CLAWT_NET_ADDRESS_MULTICAST },
	{ "255.255.255.255",          CLAWT_NET_ADDRESS_MULTICAST },
	{ "240.0.0.1",                CLAWT_NET_ADDRESS_RESERVED },
	{ "192.0.2.1",                CLAWT_NET_ADDRESS_RESERVED },
	{ "198.18.0.1",               CLAWT_NET_ADDRESS_RESERVED },
	{ "198.19.255.255",           CLAWT_NET_ADDRESS_RESERVED },
	{ "198.20.0.1",               CLAWT_NET_ADDRESS_PUBLIC },
	{ "::",                       CLAWT_NET_ADDRESS_UNSPECIFIED },
	{ "::1",                      CLAWT_NET_ADDRESS_LOOPBACK },
	{ "::ffff:127.0.0.1",         CLAWT_NET_ADDRESS_LOOPBACK },
	{ "::ffff:10.0.0.1",          CLAWT_NET_ADDRESS_PRIVATE },
	{ "::ffff:169.254.169.254",   CLAWT_NET_ADDRESS_METADATA },
	{ "::ffff:100.100.1.1",       CLAWT_NET_ADDRESS_CGNAT },
	{ "::ffff:8.8.8.8",           CLAWT_NET_ADDRESS_PUBLIC },
	{ "::a00:1",                  CLAWT_NET_ADDRESS_PRIVATE },
	{ "64:ff9b::7f00:1",          CLAWT_NET_ADDRESS_LOOPBACK },
	{ "64:ff9b::808:808",         CLAWT_NET_ADDRESS_PUBLIC },
	{ "64:ff9b:1::1",             CLAWT_NET_ADDRESS_RESERVED },
	{ "2002:7f00:1::",            CLAWT_NET_ADDRESS_LOOPBACK },
	{ "2002:a9fe:a9fe::1",        CLAWT_NET_ADDRESS_METADATA },
	{ "2002:808:808::1",          CLAWT_NET_ADDRESS_PUBLIC },
	{ "fd00:ec2::254",            CLAWT_NET_ADDRESS_METADATA },
	{ "fd00:ec2::253",            CLAWT_NET_ADDRESS_UNIQUE_LOCAL },
	{ "fd7a:115c:a1e0::1",        CLAWT_NET_ADDRESS_UNIQUE_LOCAL },
	{ "fc00::1",                  CLAWT_NET_ADDRESS_UNIQUE_LOCAL },
	{ "fe80::1",                  CLAWT_NET_ADDRESS_LINK_LOCAL },
	{ "febf::1",                  CLAWT_NET_ADDRESS_LINK_LOCAL },
	{ "fec0::1",                  CLAWT_NET_ADDRESS_PRIVATE },
	{ "ff02::1",                  CLAWT_NET_ADDRESS_MULTICAST },
	{ "2001:db8::1",              CLAWT_NET_ADDRESS_RESERVED },
	{ "2001::1",                  CLAWT_NET_ADDRESS_RESERVED },
	{ "100::1",                   CLAWT_NET_ADDRESS_RESERVED },
	{ "4000::1",                  CLAWT_NET_ADDRESS_RESERVED },
	{ "2606:4700:4700::1111",     CLAWT_NET_ADDRESS_PUBLIC },
	{ "2a00:1450:4001::200e",     CLAWT_NET_ADDRESS_PUBLIC }
};

static void
test_address_table(void)
{
	guint i;

	for (i = 0; i < G_N_ELEMENTS(address_cases); i++) {
		ClawtNetAddressClass klass = CLAWT_NET_ADDRESS_PUBLIC;
		gboolean parsed;

		parsed = clawt_net_address_classify_string(address_cases[i].address,
		                                           &klass);

		if (!parsed || klass != address_cases[i].klass)
			g_error("%s: expected %s, got %s", address_cases[i].address,
			        clawt_enum_to_nick(CLAWT_TYPE_NET_ADDRESS_CLASS,
			                           address_cases[i].klass),
			        parsed ? clawt_enum_to_nick(CLAWT_TYPE_NET_ADDRESS_CLASS,
			                                    klass)
			               : "unparsed");
	}

	/*
	 * The wait probes ask clawt_net_address_is_public(); it must be this
	 * table's yes or no, address for address.
	 */
	for (i = 0; i < G_N_ELEMENTS(address_cases); i++) {
		g_autoptr(GInetAddress) address =
			g_inet_address_new_from_string(address_cases[i].address);

		g_assert_cmpint(clawt_net_address_is_public(address), ==,
		                address_cases[i].klass == CLAWT_NET_ADDRESS_PUBLIC);
	}

	g_assert_false(clawt_net_address_classify_string("example.com", NULL));
	g_assert_false(clawt_net_address_classify_string(NULL, NULL));
}

/*
 * Every class is described in words and has an answer about whether an
 * allowlist can open it -- walked from the enum, so a class added later
 * cannot be left without either.
 */
static void
test_address_class_words(void)
{
	g_autoptr(GEnumClass) klass =
		g_type_class_ref(CLAWT_TYPE_NET_ADDRESS_CLASS);
	guint i;

	for (i = 0; i < klass->n_values; i++) {
		const gchar *words = clawt_net_address_class_describe(
			(ClawtNetAddressClass)klass->values[i].value);

		g_assert_nonnull(words);
		g_assert_null(strstr(words, "unknown"));
	}

	g_assert_true(clawt_net_address_class_is_allowable(
		CLAWT_NET_ADDRESS_CGNAT));
	g_assert_true(clawt_net_address_class_is_allowable(
		CLAWT_NET_ADDRESS_LOOPBACK));
	g_assert_false(clawt_net_address_class_is_allowable(
		CLAWT_NET_ADDRESS_METADATA));
	g_assert_false(clawt_net_address_class_is_allowable(
		CLAWT_NET_ADDRESS_MULTICAST));
	g_assert_false(clawt_net_address_class_is_allowable(
		CLAWT_NET_ADDRESS_UNSPECIFIED));
	g_assert_false(clawt_net_address_class_is_allowable(
		CLAWT_NET_ADDRESS_RESERVED));
}

/* ── The allowlist ───────────────────────────────────────────────── */

static gboolean
permits(
	ClawtEgressAllowlist *allow,
	const gchar          *host,
	const gchar          *address,
	guint16               port
){
	g_autoptr(GInetAddress) parsed = g_inet_address_new_from_string(address);

	g_assert_nonnull(parsed);

	return clawt_egress_allowlist_permits(allow, host, parsed, port);
}

static void
test_allowlist(void)
{
	const gchar *entries[] = {
		"100.101.102.103",
		"192.168.1.0/24:8080",
		"fd7a:115c:a1e0::/48",
		"[fd00::1]:443",
		"NAS.Example.ts.net.",
		"127.0.0.1:9000",
		"169.254.0.0/16",
		NULL
	};
	g_autoptr(ClawtEgressAllowlist) allow = NULL;
	g_autoptr(GError) error = NULL;

	allow = clawt_egress_allowlist_new(entries, &error);
	g_assert_no_error(error);
	g_assert_nonnull(allow);

	/* A tailnet address, on any port. */
	g_assert_true(permits(allow, "100.101.102.103", "100.101.102.103", 22));
	g_assert_false(permits(allow, "100.101.102.104", "100.101.102.104", 22));

	/* A LAN network, on one port only. */
	g_assert_true(permits(allow, "printer", "192.168.1.77", 8080));
	g_assert_false(permits(allow, "printer", "192.168.1.77", 80));
	g_assert_false(permits(allow, "printer", "192.168.2.77", 8080));

	/* IPv6 networks and a bracketed address with a port. */
	g_assert_true(permits(allow, "x", "fd7a:115c:a1e0:ab12::5", 443));
	g_assert_false(permits(allow, "x", "fd7a:115c:a1e1::5", 443));
	g_assert_true(permits(allow, "x", "fd00::1", 443));
	g_assert_false(permits(allow, "x", "fd00::1", 444));

	/* A name, case and trailing dot aside -- but never onto loopback. */
	g_assert_true(permits(allow, "nas.example.ts.net", "10.9.9.9", 5000));
	g_assert_true(permits(allow, "nas.example.ts.net.", "100.64.9.9", 1));
	g_assert_false(permits(allow, "nas.example.ts.net", "127.0.0.1", 5000));
	g_assert_false(permits(allow, "other.example.ts.net", "10.9.9.9", 5000));

	/* Loopback by address and port, and a mapped address is its IPv4. */
	g_assert_true(permits(allow, "127.0.0.1", "127.0.0.1", 9000));
	g_assert_true(permits(allow, "x", "::ffff:127.0.0.1", 9000));
	g_assert_false(permits(allow, "127.0.0.1", "127.0.0.1", 9001));

	/* Link-local is open, the metadata service inside it is not. */
	g_assert_true(permits(allow, "x", "169.254.10.10", 80));
	g_assert_false(permits(allow, "x", "169.254.169.254", 80));

	/* No list permits nothing. */
	g_assert_false(permits(NULL, "100.101.102.103", "100.101.102.103", 22));
}

static void
test_allowlist_refuses_bad_entries(void)
{
	static const gchar *const bad[] = {
		"169.254.169.254",
		"fd00:ec2::254",
		"224.0.0.0/4",
		"0.0.0.0",
		"10.0.0.0/33",
		"10.0.0.5:0",
		"10.0.0.5:70000",
		"not a host",
		"host/24",
		"[fd00::1",
		"[nas.lan]:80",
		"",
		NULL
	};
	guint i;

	for (i = 0; bad[i] != NULL; i++) {
		g_autoptr(GError) error = NULL;

		if (clawt_egress_allowlist_check_entry(bad[i], &error))
			g_error("'%s' was accepted", bad[i]);

		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
	}

	/* One bad entry refuses the list, naming it. */
	{
		const gchar *entries[] = { "10.0.0.1", "169.254.169.254", NULL };
		g_autoptr(GError) error = NULL;
		g_autoptr(ClawtEgressAllowlist) allow =
			clawt_egress_allowlist_new(entries, &error);

		g_assert_null(allow);
		g_assert_nonnull(strstr(error->message, "169.254.169.254"));
		g_assert_nonnull(strstr(error->message, "metadata"));
	}

	g_assert_true(clawt_egress_allowlist_check_entry("10.0.0.5:8080", NULL));
	g_assert_true(clawt_egress_allowlist_check_entry("[fd00::/64]:443",
	                                                 NULL));
	g_assert_true(clawt_egress_allowlist_check_entry("fd00::1", NULL));
	g_assert_true(clawt_egress_allowlist_check_entry("nas.lan:5000", NULL));
}

/* ── The decision ────────────────────────────────────────────────── */

static GPtrArray *
addresses(
	const gchar *first,
	...
){
	GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
	const gchar *text;
	va_list args;

	va_start(args, first);

	for (text = first; text != NULL; text = va_arg(args, const gchar *))
		g_ptr_array_add(out, g_inet_address_new_from_string(text));

	va_end(args);

	return out;
}

static void
test_decide(void)
{
	g_autoptr(GPtrArray) public = addresses("93.184.216.34",
	                                        "2606:2800:220:1::1", NULL);
	g_autoptr(GPtrArray) rebound = addresses("93.184.216.34", "127.0.0.1",
	                                         NULL);
	g_autoptr(GPtrArray) metadata = addresses("169.254.169.254", NULL);
	g_autoptr(GPtrArray) tailnet = addresses("100.101.102.103", NULL);
	g_autoptr(GPtrArray) none = g_ptr_array_new();
	g_autofree gchar *reason = NULL;
	const gchar *entries[] = { "100.101.102.103", NULL };
	g_autoptr(ClawtEgressAllowlist) allow =
		clawt_egress_allowlist_new(entries, NULL);

	g_assert_null(clawt_egress_decide("example.com", 443, public, NULL));

	/*
	 * One private answer among public ones refuses the name, and the
	 * reason names the address -- which is what makes rebinding legible.
	 */
	reason = clawt_egress_decide("rebind.example", 443, rebound, NULL);
	g_assert_nonnull(reason);
	g_assert_nonnull(strstr(reason, "rebind.example"));
	g_assert_nonnull(strstr(reason, "127.0.0.1"));
	g_assert_nonnull(strstr(reason, "loopback"));
	g_clear_pointer(&reason, g_free);

	reason = clawt_egress_decide("169.254.169.254", 80, metadata, allow);
	g_assert_nonnull(strstr(reason, "metadata"));
	g_clear_pointer(&reason, g_free);

	reason = clawt_egress_decide("100.101.102.103", 22, tailnet, NULL);
	g_assert_nonnull(strstr(reason, "tailnet"));
	g_clear_pointer(&reason, g_free);

	g_assert_null(clawt_egress_decide("100.101.102.103", 22, tailnet,
	                                  allow));

	reason = clawt_egress_decide("nowhere.example", 443, none, NULL);
	g_assert_nonnull(strstr(reason, "did not resolve"));
	g_clear_pointer(&reason, g_free);

	reason = clawt_egress_decide("example.com", 0, public, NULL);
	g_assert_nonnull(reason);
}

/* ── Requests ────────────────────────────────────────────────────── */

static ClawtEgressRequest *
parse(
	const gchar  *head,
	GError      **error
){
	return clawt_egress_parse_request(head, strlen(head), error);
}

static void
test_parse_connect(void)
{
	g_autoptr(ClawtEgressRequest) request = NULL;
	g_autoptr(GError) error = NULL;

	request = parse("CONNECT api.anthropic.com:443 HTTP/1.1\r\n"
	                "Host: api.anthropic.com:443\r\n\r\n", &error);
	g_assert_no_error(error);
	g_assert_true(request->is_connect);
	g_assert_cmpstr(request->host, ==, "api.anthropic.com");
	g_assert_cmpuint(request->port, ==, 443);
	g_assert_null(request->forward_head);
	g_clear_pointer(&request, clawt_egress_request_free);

	request = parse("CONNECT [2606:4700::1]:8443 HTTP/1.1\r\n\r\n", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(request->host, ==, "2606:4700::1");
	g_assert_cmpuint(request->port, ==, 8443);
	g_clear_pointer(&request, clawt_egress_request_free);

	request = parse("connect 10.0.0.1:22 HTTP/1.0\r\n\r\n", &error);
	g_assert_no_error(error);
	g_assert_cmpstr(request->host, ==, "10.0.0.1");
	g_clear_pointer(&request, clawt_egress_request_free);

	{
		static const gchar *const bad[] = {
			"CONNECT example.com HTTP/1.1\r\n\r\n",
			"CONNECT example.com:0 HTTP/1.1\r\n\r\n",
			"CONNECT example.com:99999 HTTP/1.1\r\n\r\n",
			"CONNECT 2606:4700::1:443 HTTP/1.1\r\n\r\n",
			"CONNECT [example.com]:443 HTTP/1.1\r\n\r\n",
			"CONNECT ex ample.com:443 HTTP/1.1\r\n\r\n",
			"CONNECT ex%61mple.com:443 HTTP/1.1\r\n\r\n",
			"CONNECT example.com:443\r\n\r\n",
			"\r\n\r\n",
			NULL
		};
		guint i;

		for (i = 0; bad[i] != NULL; i++) {
			g_autoptr(GError) local = NULL;
			g_autoptr(ClawtEgressRequest) refused = parse(bad[i], &local);

			if (refused != NULL)
				g_error("accepted: %s", bad[i]);

			g_assert_error(local, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
		}
	}
}

static void
test_parse_plain_http(void)
{
	g_autoptr(ClawtEgressRequest) request = NULL;
	g_autoptr(GError) error = NULL;

	request = parse("GET http://example.com:8080/a/b?c=d HTTP/1.1\r\n"
	                "Host: example.com:8080\r\n"
	                "Proxy-Connection: keep-alive\r\n"
	                "Proxy-Authorization: Basic eDp5\r\n"
	                "Connection: keep-alive\r\n"
	                "Accept: */*\r\n\r\n", &error);
	g_assert_no_error(error);
	g_assert_false(request->is_connect);
	g_assert_cmpstr(request->host, ==, "example.com");
	g_assert_cmpuint(request->port, ==, 8080);
	g_assert_cmpstr(request->forward_head, ==,
	                "GET /a/b?c=d HTTP/1.1\r\n"
	                "Host: example.com:8080\r\n"
	                "Accept: */*\r\n"
	                "Connection: close\r\n\r\n");
	g_clear_pointer(&request, clawt_egress_request_free);

	request = parse("POST http://example.com HTTP/1.1\r\n\r\n", &error);
	g_assert_no_error(error);
	g_assert_cmpuint(request->port, ==, 80);
	g_assert_true(g_str_has_prefix(request->forward_head,
	                               "POST / HTTP/1.1\r\n"));
	g_clear_pointer(&request, clawt_egress_request_free);

	{
		static const gchar *const bad[] = {
			"GET https://example.com/ HTTP/1.1\r\n\r\n",
			"GET / HTTP/1.1\r\nHost: example.com\r\n\r\n",
			"GET http://user:pw@example.com/ HTTP/1.1\r\n\r\n",
			"GET http://good.example@127.0.0.1/ HTTP/1.1\r\n\r\n",
			"GET ftp://example.com/ HTTP/1.1\r\n\r\n",
			NULL
		};
		guint i;

		for (i = 0; bad[i] != NULL; i++) {
			g_autoptr(GError) local = NULL;
			g_autoptr(ClawtEgressRequest) refused = parse(bad[i], &local);

			if (refused != NULL)
				g_error("accepted: %s", bad[i]);
		}
	}

	/* A NUL is not HTTP, and must not truncate what is checked. */
	{
		static const gchar head[] =
			"CONNECT a.example:443\0 HTTP/1.1\r\n\r\n";
		g_autoptr(GError) local = NULL;
		g_autoptr(ClawtEgressRequest) refused =
			clawt_egress_parse_request(head, sizeof(head) - 1, &local);

		g_assert_null(refused);
		g_assert_error(local, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL);
	}
}

static void
test_find_head_end(void)
{
	static const gchar complete[] = "CONNECT a:1 HTTP/1.1\r\n\r\nTLS";
	static const gchar partial[] = "CONNECT a:1 HTTP/1.1\r\n\r";

	g_assert_cmpint(clawt_egress_find_head_end(
		(const guint8 *)complete, sizeof(complete) - 1), ==,
		(gssize)(sizeof(complete) - 1 - 3));
	g_assert_cmpint(clawt_egress_find_head_end(
		(const guint8 *)partial, sizeof(partial) - 1), ==, -1);
	g_assert_cmpint(clawt_egress_find_head_end(NULL, 0), ==, -1);
}

/* ── Refusals, per agent ─────────────────────────────────────────── */

static guint refused_signals;

static void
on_refused(
	ClawtEgressGate    *gate,
	const gchar        *agent_id,
	ClawtEgressRefusal *refusal,
	gpointer            user_data
){
	refused_signals++;
	g_assert_nonnull(refusal->reason);
}

/*
 * Two agents, because the count is keyed: one agent's refusals must
 * never be shown on another's view.
 */
static void
test_gate_counts_refusals_per_agent(void)
{
	g_autofree gchar *dir = g_dir_make_tmp("clawt-egress-XXXXXX", NULL);
	g_autoptr(ClawtEgressGate) gate = clawt_egress_gate_new(NULL, dir);
	g_autoptr(GPtrArray) recent = NULL;
	g_autofree gchar *path = NULL;
	ClawtEgressRefusal *newest;
	guint i;

	g_signal_connect(gate, "refused", G_CALLBACK(on_refused), NULL);
	refused_signals = 0;

	for (i = 0; i < 25; i++) {
		g_autofree gchar *host = g_strdup_printf("h%u.example", i);

		clawt_egress_gate_record_refusal(gate, "kudu", host, 443,
		                                 "10.0.0.1", "the local network");
	}

	clawt_egress_gate_record_refusal(gate, "oryx", "metadata", 80,
	                                 "169.254.169.254", "metadata");

	g_assert_cmpuint(refused_signals, ==, 26);
	g_assert_cmpuint(clawt_egress_gate_refused_today(gate, "kudu"), ==, 25);
	g_assert_cmpuint(clawt_egress_gate_refused_today(gate, "oryx"), ==, 1);
	g_assert_cmpuint(clawt_egress_gate_refused_today(gate, "nobody"), ==, 0);

	recent = clawt_egress_gate_recent_refusals(gate, "kudu");
	g_assert_cmpuint(recent->len, ==, 20);
	newest = g_ptr_array_index(recent, 0);
	g_assert_cmpstr(newest->host, ==, "h24.example");
	g_assert_cmpstr(newest->address, ==, "10.0.0.1");
	g_clear_pointer(&recent, g_ptr_array_unref);

	recent = clawt_egress_gate_recent_refusals(gate, "oryx");
	g_assert_cmpuint(recent->len, ==, 1);
	g_assert_cmpstr(((ClawtEgressRefusal *)g_ptr_array_index(recent, 0))
	                ->address, ==, "169.254.169.254");

	/* One socket per agent, under the gate's directory. */
	path = clawt_egress_gate_socket_path(gate, "kudu");
	g_assert_true(g_str_has_prefix(path, dir));
	g_assert_true(g_str_has_suffix(path, "/kudu.sock"));

	clawt_test_remove_tree(dir);
}

static void
test_client_words(void)
{
	g_autofree gchar *gated = clawt_egress_status_line("gated", 3);
	g_autofree gchar *open = clawt_egress_status_line("open", 0);
	g_autofree gchar *line = clawt_egress_refusal_line(
		"rebind.example", 443, "127.0.0.1", "loopback");
	g_autoptr(GEnumClass) klass =
		g_type_class_ref(CLAWT_TYPE_RUNTIME_NETWORK);
	guint i;

	g_assert_cmpstr(gated, ==, "egress: gated (3 refused today)");
	g_assert_cmpstr(open, ==, "egress: open");
	g_assert_cmpstr(line, ==, "rebind.example:443 (127.0.0.1) -- loopback");

	/* Every choice has a name and a line, walked from the enum. */
	for (i = 0; i < klass->n_values; i++) {
		const gchar *nick = klass->values[i].value_nick;

		g_assert_cmpstr(clawt_egress_network_choice_label(nick), !=, nick);
		g_assert_nonnull(clawt_egress_network_choice_explanation(nick));
	}

	/* Greyed without the sandbox; the shadow reason when it was set. */
	g_assert_null(clawt_egress_network_notice("gated", "bwrap"));
	g_assert_nonnull(clawt_egress_network_notice("open", "none"));
	g_assert_nonnull(strstr(clawt_egress_network_notice("gated", "none"),
	                        "shadow"));
}

/* ── The broker ──────────────────────────────────────────────────── */

static GHashTable *
environment_with(
	const gchar *first,
	...
){
	GHashTable *out = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
	                                        g_free);
	const gchar *name;
	va_list args;

	va_start(args, first);

	for (name = first; name != NULL; name = va_arg(args, const gchar *))
		g_hash_table_insert(out, g_strdup(name),
		                    g_strdup(va_arg(args, const gchar *)));

	va_end(args);

	return out;
}

static ClawtEgressKey *
find_key(
	ClawtEgressBrokerPlan *plan,
	const gchar           *name
){
	guint i;

	for (i = 0; i < plan->keys->len; i++) {
		ClawtEgressKey *key = g_ptr_array_index(plan->keys, i);

		if (g_strcmp0(key->name, name) == 0)
			return key;
	}

	return NULL;
}

/* The negative: the real key is nowhere in what the agent is handed. */
static void
assert_absent(
	GHashTable  *environment,
	const gchar *secret
){
	GHashTableIter iter;
	gpointer key;
	gpointer value;

	g_hash_table_iter_init(&iter, environment);

	while (g_hash_table_iter_next(&iter, &key, &value))
		if (strstr(value, secret) != NULL || strstr(key, secret) != NULL)
			g_error("%s carries the real key", (const gchar *)key);
}

static void
test_broker_plan(void)
{
	g_autoptr(GHashTable) env = environment_with(
		"ANTHROPIC_API_KEY", "sk-ant-real",
		"GITHUB_TOKEN", "ghp-real",
		"AGENT_FLAVOUR", "salty",
		NULL);
	g_autoptr(ClawtEgressBrokerPlan) plan = NULL;
	ClawtEgressGrant *grant;
	ClawtEgressKey *key;

	plan = clawt_egress_broker_plan(env, "claude-code",
	                                CLAWT_RUNTIME_NETWORK_GATED,
	                                "clawt-broker-token");

	g_assert_cmpstr(g_hash_table_lookup(plan->environment,
	                                    "ANTHROPIC_API_KEY"), ==,
	                "clawt-broker-token");
	g_assert_cmpstr(g_hash_table_lookup(plan->environment,
	                                    "ANTHROPIC_BASE_URL"), ==,
	                "http://127.0.0.1:3129/anthropic");
	g_assert_cmpstr(g_hash_table_lookup(plan->environment, "AGENT_FLAVOUR"),
	                ==, "salty");
	assert_absent(plan->environment, "sk-ant-real");

	g_assert_cmpuint(plan->grants->len, ==, 1);
	grant = g_ptr_array_index(plan->grants, 0);
	g_assert_cmpstr(grant->name, ==, "anthropic");
	g_assert_cmpstr(grant->upstream, ==, "https://api.anthropic.com");
	g_assert_cmpstr(g_hash_table_lookup(grant->headers, "x-api-key"), ==,
	                "sk-ant-real");

	g_assert_true(clawt_egress_broker_plan_is_brokered(plan,
	                                                   "ANTHROPIC_API_KEY"));

	/* Not every key: one with no base URL is passed and named. */
	key = find_key(plan, "GITHUB_TOKEN");
	g_assert_nonnull(key);
	g_assert_nonnull(key->reason);
	g_assert_false(clawt_egress_broker_plan_is_brokered(plan,
	                                                    "GITHUB_TOKEN"));
	g_assert_cmpstr(g_hash_table_lookup(plan->environment, "GITHUB_TOKEN"),
	                ==, "ghp-real");
	g_assert_null(find_key(plan, "AGENT_FLAVOUR"));
}

static void
test_broker_plan_unbrokered(void)
{
	g_autoptr(GHashTable) env = environment_with(
		"ANTHROPIC_API_KEY", "sk-ant-real", NULL);
	g_autoptr(GHashTable) custom = environment_with(
		"ANTHROPIC_API_KEY", "sk-ant-real",
		"ANTHROPIC_BASE_URL", "https://llm.example.com/anthropic", NULL);
	g_autoptr(GHashTable) cleartext = environment_with(
		"ANTHROPIC_API_KEY", "sk-ant-real",
		"ANTHROPIC_BASE_URL", "http://llm.example.com", NULL);
	g_autoptr(GHashTable) openai = environment_with(
		"OPENAI_API_KEY", "sk-openai-real", NULL);
	ClawtEgressBrokerPlan *plan;

	/* An open network brokers nothing, and says why. */
	plan = clawt_egress_broker_plan(env, "claude-code",
	                                CLAWT_RUNTIME_NETWORK_OPEN, "t");
	g_assert_cmpuint(plan->grants->len, ==, 0);
	g_assert_cmpstr(g_hash_table_lookup(plan->environment,
	                                    "ANTHROPIC_API_KEY"), ==,
	                "sk-ant-real");
	g_assert_nonnull(strstr(find_key(plan, "ANTHROPIC_API_KEY")->reason,
	                        "open"));
	clawt_egress_broker_plan_free(plan);

	/* A CLI that does not read the base URL cannot be brokered. */
	plan = clawt_egress_broker_plan(env, "cursor",
	                                CLAWT_RUNTIME_NETWORK_GATED, "t");
	g_assert_cmpuint(plan->grants->len, ==, 0);
	g_assert_nonnull(strstr(find_key(plan, "ANTHROPIC_API_KEY")->reason,
	                        "ANTHROPIC_BASE_URL"));
	clawt_egress_broker_plan_free(plan);

	/* An operator's https endpoint is pinned instead of the provider's. */
	plan = clawt_egress_broker_plan(custom, "claude-code",
	                                CLAWT_RUNTIME_NETWORK_NONE, "t");
	g_assert_cmpuint(plan->grants->len, ==, 1);
	g_assert_cmpstr(((ClawtEgressGrant *)g_ptr_array_index(plan->grants, 0))
	                ->upstream, ==, "https://llm.example.com/anthropic");
	g_assert_cmpstr(g_hash_table_lookup(plan->environment,
	                                    "ANTHROPIC_BASE_URL"), ==,
	                "http://127.0.0.1:3129/anthropic");
	assert_absent(plan->environment, "sk-ant-real");
	clawt_egress_broker_plan_free(plan);

	/* ...and a cleartext one is not sent the real key. */
	plan = clawt_egress_broker_plan(cleartext, "claude-code",
	                                CLAWT_RUNTIME_NETWORK_GATED, "t");
	g_assert_cmpuint(plan->grants->len, ==, 0);
	g_assert_nonnull(find_key(plan, "ANTHROPIC_API_KEY")->reason);
	clawt_egress_broker_plan_free(plan);

	/* Codex's key, as a bearer token, with /v1 in the pin. */
	plan = clawt_egress_broker_plan(openai, "codex-cli",
	                                CLAWT_RUNTIME_NETWORK_GATED, "t");
	g_assert_cmpuint(plan->grants->len, ==, 1);
	{
		ClawtEgressGrant *grant = g_ptr_array_index(plan->grants, 0);

		g_assert_cmpstr(grant->upstream, ==, "https://api.openai.com/v1");
		g_assert_cmpstr(g_hash_table_lookup(grant->headers,
		                                    "authorization"), ==,
		                "Bearer sk-openai-real");
	}
	assert_absent(plan->environment, "sk-openai-real");
	clawt_egress_broker_plan_free(plan);

	/* With no token the key is dropped, never left in place. */
	plan = clawt_egress_broker_plan(env, "claude-code",
	                                CLAWT_RUNTIME_NETWORK_GATED, NULL);
	g_assert_false(g_hash_table_contains(plan->environment,
	                                     "ANTHROPIC_API_KEY"));
	clawt_egress_broker_plan_free(plan);

	/* No key at all -- an OAuth login -- is nothing to broker. */
	plan = clawt_egress_broker_plan(NULL, "claude-code",
	                                CLAWT_RUNTIME_NETWORK_GATED, "t");
	g_assert_cmpuint(plan->keys->len, ==, 0);
	g_assert_cmpuint(plan->grants->len, ==, 0);
	clawt_egress_broker_plan_free(plan);
}

static gboolean
headers_contain(
	SoupMessageHeaders *headers,
	const gchar        *needle
){
	SoupMessageHeadersIter iter;
	const gchar *name;
	const gchar *value;

	soup_message_headers_iter_init(&iter, headers);

	while (soup_message_headers_iter_next(&iter, &name, &value))
		if (strstr(value, needle) != NULL)
			return TRUE;

	return FALSE;
}

static void
test_broker_token_swap(void)
{
	g_autoptr(ClawtEgressBroker) broker = clawt_egress_broker_new(NULL);
	g_autoptr(GHashTable) env = environment_with(
		"ANTHROPIC_API_KEY", "sk-ant-real", NULL);
	g_autoptr(ClawtEgressBrokerPlan) plan = NULL;
	g_autofree gchar *token = NULL;
	g_autofree gchar *url = NULL;
	g_autofree gchar *agent = NULL;
	g_autoptr(GHashTable) credentials = NULL;
	g_autoptr(GError) error = NULL;
	SoupMessageHeaders *in;
	SoupMessageHeaders *out;
	guint status = 0;

	token = clawt_egress_broker_mint_token(&error);
	g_assert_no_error(error);
	g_assert_true(g_str_has_prefix(token, "clawt-broker-"));
	g_assert_cmpuint(strlen(token), >, 40);

	plan = clawt_egress_broker_plan(env, "claude-code",
	                                CLAWT_RUNTIME_NETWORK_GATED, token);
	clawt_egress_broker_register(broker, "kudu", token, plan->grants);

	in = soup_message_headers_new(SOUP_MESSAGE_HEADERS_REQUEST);
	soup_message_headers_append(in, "X-Api-Key", token);
	soup_message_headers_append(in, "Host", "127.0.0.1:3129");
	soup_message_headers_append(in, "Cookie", "session=leak");
	soup_message_headers_append(in, "Anthropic-Version", "2023-06-01");
	soup_message_headers_append(in, "Connection", "keep-alive");

	g_assert_true(clawt_egress_broker_authorize(
		broker, "/anthropic/v1/messages", "beta=true", in, &url,
		&credentials, &agent, &status, &error));
	g_assert_no_error(error);
	g_assert_cmpstr(url, ==,
	                "https://api.anthropic.com/v1/messages?beta=true");
	g_assert_cmpstr(agent, ==, "kudu");
	g_assert_cmpuint(status, ==, 200);

	out = soup_message_headers_new(SOUP_MESSAGE_HEADERS_REQUEST);
	clawt_egress_broker_copy_headers(in, out, credentials);

	g_assert_cmpstr(soup_message_headers_get_one(out, "x-api-key"), ==,
	                "sk-ant-real");
	g_assert_cmpstr(soup_message_headers_get_one(out, "Anthropic-Version"),
	                ==, "2023-06-01");
	g_assert_null(soup_message_headers_get_one(out, "Host"));
	g_assert_null(soup_message_headers_get_one(out, "Cookie"));
	g_assert_null(soup_message_headers_get_one(out, "Connection"));
	g_assert_false(headers_contain(out, token));

	/* A bearer token works the same way. */
	{
		SoupMessageHeaders *bearer =
			soup_message_headers_new(SOUP_MESSAGE_HEADERS_REQUEST);
		g_autofree gchar *auth = g_strdup_printf("Bearer %s", token);
		g_autofree gchar *again = NULL;
		g_autoptr(GHashTable) creds = NULL;

		soup_message_headers_append(bearer, "Authorization", auth);
		g_assert_true(clawt_egress_broker_authorize(
			broker, "/anthropic", NULL, bearer, &again, &creds, NULL,
			NULL, NULL));
		g_assert_cmpstr(again, ==, "https://api.anthropic.com");
		soup_message_headers_unref(bearer);
	}

	/* The wrong token, a grant it was not given, and "..". */
	{
		SoupMessageHeaders *wrong =
			soup_message_headers_new(SOUP_MESSAGE_HEADERS_REQUEST);
		g_autofree gchar *u1 = NULL;
		g_autofree gchar *u2 = NULL;
		g_autofree gchar *u3 = NULL;
		g_autoptr(GHashTable) c1 = NULL;
		g_autoptr(GHashTable) c2 = NULL;
		g_autoptr(GHashTable) c3 = NULL;
		g_autoptr(GError) e1 = NULL;
		g_autoptr(GError) e2 = NULL;
		g_autoptr(GError) e3 = NULL;

		soup_message_headers_append(wrong, "X-Api-Key", "sk-ant-real");
		g_assert_false(clawt_egress_broker_authorize(
			broker, "/anthropic/v1/messages", NULL, wrong, &u1, &c1, NULL,
			&status, &e1));
		g_assert_cmpuint(status, ==, 401);
		g_assert_null(u1);

		g_assert_false(clawt_egress_broker_authorize(
			broker, "/openai/responses", NULL, in, &u2, &c2, NULL, &status,
			&e2));
		g_assert_cmpuint(status, ==, 403);

		g_assert_false(clawt_egress_broker_authorize(
			broker, "/anthropic/v1/../../evil", NULL, in, &u3, &c3, NULL,
			&status, &e3));
		soup_message_headers_unref(wrong);
	}

	/* A new process's token retires the old one; an exit revokes it. */
	{
		g_autofree gchar *fresh = clawt_egress_broker_mint_token(NULL);
		g_autofree gchar *u = NULL;
		g_autoptr(GHashTable) c = NULL;

		g_assert_cmpstr(fresh, !=, token);
		clawt_egress_broker_register(broker, "kudu", fresh, plan->grants);
		g_assert_false(clawt_egress_broker_authorize(
			broker, "/anthropic/v1/messages", NULL, in, &u, &c, NULL,
			&status, NULL));
		g_assert_cmpuint(status, ==, 401);
	}

	clawt_egress_broker_register(broker, "kudu", token, plan->grants);
	clawt_egress_broker_revoke(broker, "kudu");
	{
		g_autofree gchar *u = NULL;
		g_autoptr(GHashTable) c = NULL;

		g_assert_false(clawt_egress_broker_authorize(
			broker, "/anthropic/v1/messages", NULL, in, &u, &c, NULL,
			&status, NULL));
	}

	soup_message_headers_unref(in);
	soup_message_headers_unref(out);
}

/* ── The runtime and the configuration ───────────────────────────── */

typedef struct {
	gchar *base;
	gchar *workspace;
	gchar *state;
	gchar *bin;
	gchar *saved_path;
} Fixture;

static void
write_executable(
	const gchar *path,
	const gchar *text
){
	g_assert_true(g_file_set_contents(path, text, -1, NULL));
	g_assert_cmpint(g_chmod(path, 0755), ==, 0);
}

static void
fixture_setup(
	Fixture *fixture
){
	g_autofree gchar *made = g_dir_make_tmp("clawt-egress-XXXXXX", NULL);

	fixture->base = clawt_canonicalize_missing(made);
	fixture->workspace = g_build_filename(fixture->base, "workspace", NULL);
	fixture->state = g_build_filename(fixture->base, "state", NULL);
	fixture->bin = g_build_filename(fixture->base, "bin", NULL);

	g_assert_cmpint(g_mkdir_with_parents(fixture->workspace, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(fixture->state, 0700), ==, 0);
	g_assert_cmpint(g_mkdir_with_parents(fixture->bin, 0700), ==, 0);

	{
		g_autofree gchar *bwrap = g_build_filename(fixture->bin, "bwrap",
		                                           NULL);
		g_autofree gchar *relay = g_build_filename(fixture->bin,
		                                           "clawtilla-relay", NULL);
		g_autofree gchar *libreclaw = g_build_filename(fixture->bin,
		                                               "libreclaw", NULL);

		write_executable(bwrap, "#!/bin/sh\nexit 0\n");
		write_executable(relay, "#!/bin/sh\nexit 0\n");
		write_executable(libreclaw, "#!/bin/sh\nexit 0\n");
	}

	/* bubblewrap "installed": the stub is the only one on PATH. */
	fixture->saved_path = g_strdup(g_getenv("PATH"));
	g_setenv("PATH", fixture->bin, TRUE);
}

static void
fixture_teardown(
	Fixture *fixture
){
	if (fixture->saved_path != NULL)
		g_setenv("PATH", fixture->saved_path, TRUE);
	else
		g_unsetenv("PATH");

	clawt_test_remove_tree(fixture->base);

	g_clear_pointer(&fixture->base, g_free);
	g_clear_pointer(&fixture->workspace, g_free);
	g_clear_pointer(&fixture->state, g_free);
	g_clear_pointer(&fixture->bin, g_free);
	g_clear_pointer(&fixture->saved_path, g_free);
}

static ClawtConfig *
load(
	Fixture     *fixture,
	const gchar *top_extra,
	const gchar *agent_extra
){
	g_autoptr(GError) error = NULL;
	g_autofree gchar *yaml = g_strdup_printf(
		"daemon:\n"
		"  state_dir: \"%s\"\n"
		"%s"
		"agents:\n"
		"  - id: boxed\n"
		"    workspace: \"%s\"\n"
		"%s",
		fixture->state, top_extra != NULL ? top_extra : "",
		fixture->workspace, agent_extra != NULL ? agent_extra : "");
	ClawtConfig *config = clawt_config_load_from_string(yaml, &error);

	g_assert_no_error(error);
	g_assert_nonnull(config);

	return config;
}

/* The value of NAME in an environment array, or NULL. */
static const gchar *
env_value(
	GStrv        env,
	const gchar *name
){
	gsize length = strlen(name);
	guint i;

	for (i = 0; env[i] != NULL; i++)
		if (strncmp(env[i], name, length) == 0 && env[i][length] == '=')
			return env[i] + length + 1;

	return NULL;
}

static gint
index_of(
	GStrv        argv,
	const gchar *word
){
	guint i;

	for (i = 0; argv[i] != NULL; i++)
		if (g_strcmp0(argv[i], word) == 0)
			return (gint)i;

	return -1;
}

/*
 * Gated without the sandbox is a shadow with the reason, never an agent
 * on the open network; an unknown value is reported, not read as open.
 */
static void
test_gated_without_bwrap_is_refused(void)
{
	Fixture fixture = { 0 };
	ClawtConfig *config;
	ClawtAgentConfig *agent;

	fixture_setup(&fixture);

	config = load(&fixture, NULL, "    runtime:\n      network: gated\n");
	agent = clawt_config_get_agent(config, "boxed");
	g_assert_true(clawt_agent_config_is_shadow(agent));
	g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
	                        "runtime.confine: bwrap"));
	g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
	                        "not run on an open network"));
	g_object_unref(config);

	config = load(&fixture, NULL, "    runtime:\n      network: closed\n");
	agent = clawt_config_get_agent(config, "boxed");
	g_assert_true(clawt_agent_config_is_shadow(agent));
	g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
	                        "unknown runtime.network"));
	g_object_unref(config);

	/* With the sandbox (a stub bwrap on PATH) it is a working agent. */
	config = load(&fixture, NULL,
	              "    runtime:\n      confine: bwrap\n      network: gated\n");
	agent = clawt_config_get_agent(config, "boxed");
	g_assert_false(clawt_agent_config_is_shadow(agent));
	g_object_unref(config);

	/* Through defaults.confine too: the relation is effective values. */
	config = load(&fixture, "defaults:\n  confine: bwrap\n",
	              "    runtime:\n      network: none\n");
	agent = clawt_config_get_agent(config, "boxed");
	g_assert_false(clawt_agent_config_is_shadow(agent));
	g_object_unref(config);

	/* Open is the default, and needs nothing. */
	config = load(&fixture, NULL, NULL);
	agent = clawt_config_get_agent(config, "boxed");
	g_assert_false(clawt_agent_config_is_shadow(agent));
	g_assert_cmpint(clawt_agent_config_get_enum(agent, "runtime.network"),
	                ==, CLAWT_RUNTIME_NETWORK_OPEN);
	g_object_unref(config);

	fixture_teardown(&fixture);
}

/* A bad egress.allow is a warning that opens nothing, not a dead fleet. */
static void
test_bad_allowlist_is_a_warning(void)
{
	Fixture fixture = { 0 };
	g_autoptr(ClawtConfig) config = NULL;
	GPtrArray *warnings;
	gboolean found = FALSE;
	guint i;

	fixture_setup(&fixture);

	config = load(&fixture,
	              "egress:\n  allow:\n    - 169.254.169.254\n", NULL);
	warnings = clawt_config_get_warnings(config);

	for (i = 0; warnings != NULL && i < warnings->len; i++)
		if (strstr(g_ptr_array_index(warnings, i), "egress.allow") != NULL)
			found = TRUE;

	g_assert_true(found);

	fixture_teardown(&fixture);
}

static ClawtProcessRuntime *
gated_runtime(
	Fixture             *fixture,
	ClawtConfig         *config,
	GHashTable          *env,
	ClawtEgressBroker   *broker,
	const gchar         *gate_socket,
	ClawtRuntimeNetwork  network
){
	ClawtAgentConfig *agent = clawt_config_get_agent(config, "boxed");
	g_autoptr(ClawtSandbox) sandbox = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *config_path =
		g_build_filename(fixture->state, "config.yaml", NULL);
	g_autofree gchar *binary = g_build_filename(fixture->bin, "libreclaw",
	                                            NULL);
	g_autofree gchar *relay = g_build_filename(fixture->bin,
	                                           "clawtilla-relay", NULL);
	ClawtProcessRuntime *runtime;

	g_assert_true(g_file_set_contents(config_path, "", 0, NULL));

	runtime = clawt_process_runtime_new(agent, config_path);
	clawt_process_runtime_set_binary(runtime, binary);
	clawt_process_runtime_set_relay_binary(runtime, relay);

	if (env != NULL)
		clawt_process_runtime_set_environment(runtime, env);

	sandbox = clawt_agent_confine_build(agent, fixture->state, NULL, NULL,
	                                    NULL, &error);
	g_assert_no_error(error);
	clawt_process_runtime_set_sandbox(runtime, sandbox);
	clawt_process_runtime_set_egress(runtime, network, gate_socket, broker);

	return runtime;
}

/*
 * The whole gated command as data: a namespace of its own, the relay
 * bridging the gate and the broker, the proxy variables, and -- the
 * point of the broker -- a token where the key was.
 */
static void
test_prepare_gated(void)
{
	Fixture fixture = { 0 };
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(ClawtProcessRuntime) runtime = NULL;
	g_autoptr(ClawtEgressBroker) broker = NULL;
	g_autoptr(GHashTable) env_in = environment_with(
		"ANTHROPIC_API_KEY", "sk-ant-real", NULL);
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) argv = NULL;
	g_auto(GStrv) env = NULL;
	g_autofree gchar *gate = NULL;
	g_autofree gchar *broker_socket = NULL;
	g_autofree gchar *relay = NULL;
	g_autofree gchar *forward_gate = NULL;
	g_autofree gchar *forward_broker = NULL;
	gint separator;
	gint at;
	guint i;

	fixture_setup(&fixture);

	config = load(&fixture, NULL,
	              "    runtime:\n      confine: bwrap\n      network: gated\n");

	/*
	 * Paths only: neither socket is bound.  The broker is "listening"
	 * as far as prepare() can tell by being given a socket path, which
	 * is the one thing it reads; the real listen is in the integration
	 * test.
	 */
	/* Outside the agent's state directory, as the daemon's are. */
	gate = g_build_filename(fixture.base, "egress", "gate.sock", NULL);
	broker_socket = g_build_filename(fixture.base, "egress", "broker.sock",
	                                 NULL);
	g_assert_cmpint(g_mkdir_with_parents(gate, 0700), ==, 0);
	g_assert_cmpint(g_rmdir(gate), ==, 0);
	g_assert_true(g_file_set_contents(gate, "", 0, NULL));
	broker = clawt_egress_broker_new(NULL);
	g_assert_true(clawt_egress_broker_listen(broker, broker_socket, &error));
	g_assert_no_error(error);

	runtime = gated_runtime(&fixture, config, env_in, broker, gate,
	                        CLAWT_RUNTIME_NETWORK_GATED);

	g_assert_true(clawt_process_runtime_prepare(runtime, &argv, &env,
	                                            &error));
	g_assert_no_error(error);

	/* Its own namespace. */
	separator = index_of(argv, "--");
	g_assert_cmpint(separator, >, 0);
	at = index_of(argv, "--unshare-net");
	g_assert_cmpint(at, >, 0);
	g_assert_cmpint(at, <, separator);

	/* The relay runs libreclaw, bridging both sockets. */
	relay = g_build_filename(fixture.bin, "clawtilla-relay", NULL);
	g_assert_cmpstr(argv[separator + 1], ==, relay);
	forward_gate = g_strdup_printf("127.0.0.1:3128=%s", gate);
	forward_broker = g_strdup_printf("127.0.0.1:3129=%s", broker_socket);
	g_assert_cmpint(index_of(argv, forward_gate), >, separator);
	g_assert_cmpint(index_of(argv, forward_broker), >, separator);

	/* Both sockets bound into the box, before the separator. */
	at = index_of(argv, gate);
	g_assert_cmpint(at, >, 0);
	g_assert_cmpint(at, <, separator);
	at = index_of(argv, broker_socket);
	g_assert_cmpint(at, >, 0);
	g_assert_cmpint(at, <, separator);

	/* The proxy, both spellings, and the broker kept off it. */
	g_assert_cmpstr(env_value(env, "HTTPS_PROXY"), ==,
	                "http://127.0.0.1:3128");
	g_assert_cmpstr(env_value(env, "http_proxy"), ==,
	                "http://127.0.0.1:3128");
	g_assert_nonnull(strstr(env_value(env, "NO_PROXY"), "127.0.0.1"));
	g_assert_cmpstr(env_value(env, "ANTHROPIC_BASE_URL"), ==,
	                "http://127.0.0.1:3129/anthropic");

	/* The real key is nowhere: not in the environment, not in argv. */
	for (i = 0; env[i] != NULL; i++)
		g_assert_null(strstr(env[i], "sk-ant-real"));

	for (i = 0; argv[i] != NULL; i++)
		g_assert_null(strstr(argv[i], "sk-ant-real"));

	/* ...and what is there is a token the broker accepts. */
	{
		const gchar *token = env_value(env, "ANTHROPIC_API_KEY");
		SoupMessageHeaders *headers =
			soup_message_headers_new(SOUP_MESSAGE_HEADERS_REQUEST);
		g_autofree gchar *url = NULL;
		g_autoptr(GHashTable) credentials = NULL;
		g_autofree gchar *agent = NULL;

		g_assert_true(g_str_has_prefix(token, "clawt-broker-"));
		soup_message_headers_append(headers, "X-Api-Key", token);
		g_assert_true(clawt_egress_broker_authorize(
			broker, "/anthropic/v1/messages", NULL, headers, &url,
			&credentials, &agent, NULL, &error));
		g_assert_cmpstr(agent, ==, "boxed");
		g_assert_cmpstr(g_hash_table_lookup(credentials, "x-api-key"), ==,
		                "sk-ant-real");
		soup_message_headers_unref(headers);
	}

	/* A second start mints a new token and the first stops working. */
	{
		g_auto(GStrv) again_argv = NULL;
		g_auto(GStrv) again_env = NULL;
		const gchar *first = env_value(env, "ANTHROPIC_API_KEY");

		g_assert_true(clawt_process_runtime_prepare(runtime, &again_argv,
		                                            &again_env, NULL));
		g_assert_cmpstr(env_value(again_env, "ANTHROPIC_API_KEY"), !=,
		                first);
	}

	g_clear_object(&runtime);
	clawt_egress_broker_stop(broker);
	fixture_teardown(&fixture);
}

/*
 * none: the broker's port only, no proxy variables -- and an agent with
 * nothing to broker gets no broker forward at all.
 */
static void
test_prepare_none(void)
{
	Fixture fixture = { 0 };
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(ClawtProcessRuntime) runtime = NULL;
	g_autoptr(GError) error = NULL;
	g_auto(GStrv) argv = NULL;
	g_auto(GStrv) env = NULL;
	guint i;

	fixture_setup(&fixture);

	config = load(&fixture, NULL,
	              "    runtime:\n      confine: bwrap\n      network: none\n");
	runtime = gated_runtime(&fixture, config, NULL, NULL, NULL,
	                        CLAWT_RUNTIME_NETWORK_NONE);

	g_assert_true(clawt_process_runtime_prepare(runtime, &argv, &env,
	                                            &error));
	g_assert_no_error(error);
	g_assert_cmpint(index_of(argv, "--unshare-net"), >, 0);
	g_assert_null(env_value(env, "HTTPS_PROXY"));

	for (i = 0; argv[i] != NULL; i++)
		g_assert_false(g_str_has_prefix(argv[i], "127.0.0.1:"));

	fixture_teardown(&fixture);
}

/*
 * Refusals, never a downgrade: no sandbox, no relay, no gate socket, or
 * a key to broker with no broker listening.
 */
static void
test_prepare_refuses(void)
{
	Fixture fixture = { 0 };
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(GHashTable) env_in = environment_with(
		"ANTHROPIC_API_KEY", "sk-ant-real", NULL);

	fixture_setup(&fixture);

	config = load(&fixture, NULL,
	              "    runtime:\n      confine: bwrap\n      network: gated\n");

	/* No sandbox. */
	{
		g_autoptr(ClawtProcessRuntime) runtime =
			gated_runtime(&fixture, config, NULL, NULL, "/x/gate.sock",
			              CLAWT_RUNTIME_NETWORK_GATED);
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) argv = NULL;
		g_auto(GStrv) env = NULL;

		clawt_process_runtime_set_sandbox(runtime, NULL);
		g_assert_false(clawt_process_runtime_prepare(runtime, &argv, &env,
		                                             &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
		g_assert_null(argv);
	}

	/* No relay anywhere. */
	{
		g_autoptr(ClawtProcessRuntime) runtime =
			gated_runtime(&fixture, config, NULL, NULL, "/x/gate.sock",
			              CLAWT_RUNTIME_NETWORK_GATED);
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) argv = NULL;
		g_auto(GStrv) env = NULL;
		g_autofree gchar *missing =
			g_build_filename(fixture.base, "no-such-relay", NULL);

		clawt_process_runtime_set_relay_binary(runtime, missing);
		g_assert_false(clawt_process_runtime_prepare(runtime, &argv, &env,
		                                             &error));
	}

	/* Gated with no gate socket. */
	{
		g_autoptr(ClawtProcessRuntime) runtime =
			gated_runtime(&fixture, config, NULL, NULL, NULL,
			              CLAWT_RUNTIME_NETWORK_GATED);
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) argv = NULL;
		g_auto(GStrv) env = NULL;

		g_assert_false(clawt_process_runtime_prepare(runtime, &argv, &env,
		                                             &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
	}

	/* A key to broker and no broker: refused, not handed over. */
	{
		g_autoptr(ClawtProcessRuntime) runtime =
			gated_runtime(&fixture, config, env_in, NULL, "/x/gate.sock",
			              CLAWT_RUNTIME_NETWORK_GATED);
		g_autoptr(GError) error = NULL;
		g_auto(GStrv) argv = NULL;
		g_auto(GStrv) env = NULL;

		g_assert_false(clawt_process_runtime_prepare(runtime, &argv, &env,
		                                             &error));
		g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT);
		g_assert_null(env);
	}

	fixture_teardown(&fixture);
}

/*
 * A brokered credential is not written to the credentials directory,
 * which the box binds read-write: a key in a file there is a key the
 * agent can read.
 */
static void
test_brokered_credential_not_written(void)
{
	Fixture fixture = { 0 };
	g_autoptr(ClawtConfig) config = NULL;
	g_autoptr(GError) error = NULL;
	g_autofree gchar *secret = NULL;
	g_autofree gchar *yaml = NULL;
	g_autofree gchar *anthropic = NULL;
	g_autofree gchar *github = NULL;
	g_autofree gchar *agent_state = NULL;
	ClawtAgentConfig *agent;

	fixture_setup(&fixture);

	secret = g_build_filename(fixture.base, "secret", NULL);
	g_assert_true(g_file_set_contents(secret, "sk-ant-real", -1, NULL));

	yaml = g_strdup_printf(
		"    runtime:\n      confine: bwrap\n      network: gated\n"
		"    credentials:\n"
		"      anthropic_api_key: {file: \"%s\"}\n"
		"      github_token: {file: \"%s\"}\n", secret, secret);
	config = load(&fixture, "defaults:\n  workspace_root: \"/nonexistent\"\n",
	              yaml);
	agent = clawt_config_get_agent(config, "boxed");
	g_assert_false(clawt_agent_config_is_shadow(agent));

	agent_state = clawt_config_agent_state_dir(config, "boxed");
	g_assert_true(clawt_config_write_agent_files(
		config, agent, "/nonexistent/agents.sock", NULL, &error));
	g_assert_no_error(error);

	anthropic = g_build_filename(agent_state, "credentials",
	                             "anthropic_api_key", NULL);
	github = g_build_filename(agent_state, "credentials", "github_token",
	                          NULL);

	g_assert_false(g_file_test(anthropic, G_FILE_TEST_EXISTS));
	g_assert_true(g_file_test(github, G_FILE_TEST_EXISTS));

	fixture_teardown(&fixture);
}

int
main(
	int    argc,
	char **argv
){
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/egress/policy/table", test_address_table);
	g_test_add_func("/egress/policy/words", test_address_class_words);
	g_test_add_func("/egress/allowlist/permits", test_allowlist);
	g_test_add_func("/egress/allowlist/refuses",
	                test_allowlist_refuses_bad_entries);
	g_test_add_func("/egress/decide", test_decide);
	g_test_add_func("/egress/request/connect", test_parse_connect);
	g_test_add_func("/egress/request/plain-http", test_parse_plain_http);
	g_test_add_func("/egress/request/head-end", test_find_head_end);
	g_test_add_func("/egress/gate/refusals-per-agent",
	                test_gate_counts_refusals_per_agent);
	g_test_add_func("/egress/clients/words", test_client_words);
	g_test_add_func("/egress/broker/plan", test_broker_plan);
	g_test_add_func("/egress/broker/plan-unbrokered",
	                test_broker_plan_unbrokered);
	g_test_add_func("/egress/broker/token-swap", test_broker_token_swap);
	g_test_add_func("/egress/config/gated-needs-bwrap",
	                test_gated_without_bwrap_is_refused);
	g_test_add_func("/egress/config/bad-allowlist",
	                test_bad_allowlist_is_a_warning);
	g_test_add_func("/egress/runtime/gated", test_prepare_gated);
	g_test_add_func("/egress/runtime/none", test_prepare_none);
	g_test_add_func("/egress/runtime/refuses", test_prepare_refuses);
	g_test_add_func("/egress/render/brokered-credential",
	                test_brokered_credential_not_written);

	return g_test_run();
}
