/*
 * test-bind-address.c - The <ip>:<port> form clawtillad --bind takes
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A person typing --bind is usually doing it because the default was
 * wrong for them, so getting this subtly wrong puts the daemon somewhere
 * other than where they asked -- which is the one outcome worse than
 * refusing outright.  IPv6 is where that happens: an address full of
 * colons split on its last one yields a host and a port that are each
 * almost plausible.
 */

#include <clawtilla.h>

#include <string.h>

#define DEFAULT_PORT (8792)

static void
check(const gchar *text, const gchar *want_host, guint16 want_port)
{
    g_autofree gchar *host = NULL;
    g_autoptr(GError) error = NULL;
    guint16 port = 0;

    g_assert_true(clawt_ipc_parse_listen_address(text, DEFAULT_PORT, &host,
                                                 &port, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(host, ==, want_host);
    g_assert_cmpuint(port, ==, want_port);
}

static void
refuse(const gchar *text)
{
    g_autofree gchar *host = NULL;
    g_autoptr(GError) error = NULL;
    guint16 port = 0;

    g_assert_false(clawt_ipc_parse_listen_address(text, DEFAULT_PORT, &host,
                                                  &port, &error));
    g_assert_nonnull(error);

    /* And says something about the text it was given. */
    g_assert_cmpuint(strlen(error->message), >, 0);
}

static void
test_ipv4_with_and_without_a_port(void)
{
    check("127.0.0.1:9000", "127.0.0.1", 9000);
    check("10.0.0.5:1", "10.0.0.5", 1);
    check("10.0.0.5:65535", "10.0.0.5", 65535);

    /* No port means the default, which is what makes --bind IP usable. */
    check("100.72.0.41", "100.72.0.41", DEFAULT_PORT);
    check("0.0.0.0", "0.0.0.0", DEFAULT_PORT);
}

/*
 * The reason this is not a strrchr(':').  Splitting `fd7a:115c:a1e0::1`
 * on its last colon gives a host of `fd7a:115c:a1e0:` and a port of `:1`,
 * and the bracket form is how the ambiguity is settled everywhere else.
 */
static void
test_ipv6_needs_brackets_only_for_a_port(void)
{
    check("[fd7a:115c:a1e0::1]:9000", "fd7a:115c:a1e0::1", 9000);
    check("[::1]:8080", "::1", 8080);

    /* Bracketed with no port is still an address. */
    check("[fd7a:115c:a1e0::1]", "fd7a:115c:a1e0::1", DEFAULT_PORT);

    /*
     * Bare, with no port, is accepted: it parses whole, so there is
     * nothing it could be mistaken for.
     */
    check("fd7a:115c:a1e0::1", "fd7a:115c:a1e0::1", DEFAULT_PORT);
    check("::1", "::1", DEFAULT_PORT);
}

static void
test_a_malformed_bracket_is_refused(void)
{
    refuse("[fd7a::1");           /* never closed */
    refuse("[fd7a::1]junk");      /* trailing rubbish */
    refuse("[fd7a::1]:");         /* colon and no port */
    refuse("[]:8792");            /* no address */
}

static void
test_a_bad_port_is_refused(void)
{
    refuse("127.0.0.1:");
    refuse("127.0.0.1:0");        /* 0 asks the kernel for any free port */
    refuse("127.0.0.1:65536");
    refuse("127.0.0.1:-1");
    refuse("127.0.0.1:http");
    refuse("127.0.0.1:80x");
}

/*
 * A name would have to be resolved, which is a network round trip on the
 * path that starts the daemon -- the one thing daemon start may not wait
 * on -- and it can resolve to an address on a different network than the
 * one intended.
 */
static void
test_a_hostname_is_refused(void)
{
    refuse("localhost");
    refuse("localhost:8792");
    refuse("workstation.tail1234.ts.net:8792");
}

static void
test_nonsense_is_refused(void)
{
    refuse(NULL);
    refuse("");
    refuse(":8792");              /* a port and no address */
    refuse("999.1.1.1");
    refuse("127.0.0.1:80:80");
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/bind-address/ipv4", test_ipv4_with_and_without_a_port);
    g_test_add_func("/bind-address/ipv6",
                    test_ipv6_needs_brackets_only_for_a_port);
    g_test_add_func("/bind-address/bad-brackets",
                    test_a_malformed_bracket_is_refused);
    g_test_add_func("/bind-address/bad-port", test_a_bad_port_is_refused);
    g_test_add_func("/bind-address/hostname", test_a_hostname_is_refused);
    g_test_add_func("/bind-address/nonsense", test_nonsense_is_refused);

    return g_test_run();
}
