/*
 * test-tailscale.c - Which addresses the daemon will bind by default
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * daemon.tailscale is on by default, so this predicate is what decides
 * whether clawtillad opens a network listener at all.  Too generous in
 * either direction is a real fault: accepting an address outside
 * Tailscale's range would put the fleet on whatever network that
 * interface is really on, and refusing a genuine one silently disables
 * the feature on a machine that looks like it should have it.
 *
 * Hermetic on purpose -- the range arithmetic is a pure function, so it
 * is checked here without a tailnet, and the interface walk is verified
 * against a real one by hand.
 */

#include <clawtilla.h>

/*
 * 100.64.0.0/10.  A /10 does not fall on an octet boundary, so the second
 * octet is a range, and the two ends of it are the whole test: 100.63 is
 * below the range and 100.128 is above it, and both are ordinary
 * internet addresses belonging to somebody else.
 */
static void
test_the_v4_range_is_a_ten_bit_prefix(void)
{
    /* Inside. */
    g_assert_true(clawt_tailscale_is_tailnet_address("100.64.0.0"));
    g_assert_true(clawt_tailscale_is_tailnet_address("100.64.0.1"));
    g_assert_true(clawt_tailscale_is_tailnet_address("100.72.0.41"));
    g_assert_true(clawt_tailscale_is_tailnet_address("100.100.100.100"));
    g_assert_true(clawt_tailscale_is_tailnet_address("100.127.255.255"));

    /* One below, and one above. */
    g_assert_false(clawt_tailscale_is_tailnet_address("100.63.255.255"));
    g_assert_false(clawt_tailscale_is_tailnet_address("100.128.0.0"));
}

static void
test_other_addresses_are_refused(void)
{
    static const gchar *const others[] = {
        "10.0.0.41",        /* the machine's own LAN */
        "192.168.1.10",
        "172.16.0.1",
        "127.0.0.1",
        "0.0.0.0",          /* every interface, which is the worst case */
        "8.8.8.8",
        "101.64.0.1",       /* right second octet, wrong first */
        "99.64.0.1",
        NULL
    };
    gsize i;

    for (i = 0; others[i] != NULL; i++)
        g_assert_false(clawt_tailscale_is_tailnet_address(others[i]));
}

static void
test_the_v6_range_is_matched_on_its_prefix(void)
{
    /* fd7a:115c:a1e0::/48 */
    g_assert_true(clawt_tailscale_is_tailnet_address("fd7a:115c:a1e0::1"));
    g_assert_true(
        clawt_tailscale_is_tailnet_address("fd7a:115c:a1e0:ab12:4843:cd96::"));

    /* A different ULA, which is what a container network looks like. */
    g_assert_false(clawt_tailscale_is_tailnet_address("fd00::1"));
    g_assert_false(clawt_tailscale_is_tailnet_address("fd7a:115c:a1e1::1"));
    g_assert_false(clawt_tailscale_is_tailnet_address("::1"));
}

/*
 * Not an address at all.  This reaches the predicate from an interface
 * walk, so a garbage value here means a bug elsewhere -- and answering
 * TRUE would hand g_inet_address_new_from_string() something it refuses,
 * failing daemon start rather than skipping an interface.
 */
static void
test_nonsense_is_not_an_address(void)
{
    g_assert_false(clawt_tailscale_is_tailnet_address(NULL));
    g_assert_false(clawt_tailscale_is_tailnet_address(""));
    g_assert_false(clawt_tailscale_is_tailnet_address("100.64.0"));
    g_assert_false(clawt_tailscale_is_tailnet_address("tailscale0"));
    g_assert_false(clawt_tailscale_is_tailnet_address("100.64.0.1/10"));
}

static void
test_the_interface_is_recognised_by_name(void)
{
    g_assert_true(clawt_tailscale_is_tailnet_interface("tailscale0"));
    g_assert_true(clawt_tailscale_is_tailnet_interface("tailscale1"));

    /* The BSDs use a generic tunnel name, which is why the address is
     * checked as well rather than instead. */
    g_assert_true(clawt_tailscale_is_tailnet_interface("utun4"));

    g_assert_false(clawt_tailscale_is_tailnet_interface("eth0"));
    g_assert_false(clawt_tailscale_is_tailnet_interface("wlp192s0"));
    g_assert_false(clawt_tailscale_is_tailnet_interface("lo"));
    g_assert_false(clawt_tailscale_is_tailnet_interface("podman0"));
    g_assert_false(clawt_tailscale_is_tailnet_interface(NULL));
    g_assert_false(clawt_tailscale_is_tailnet_interface(""));
}

/*
 * Whatever this machine has, the lookup must not invent one.
 *
 * The suite runs on machines with a tailnet and machines without, so the
 * only thing that can be asserted either way is that an answer, if there
 * is one, is an address the daemon can actually bind -- which is the
 * failure that would take daemon start down with it.
 */
static void
test_any_address_found_is_bindable(void)
{
    g_autofree gchar *address = clawt_tailscale_find_address();
    g_autoptr(GInetAddress) parsed = NULL;

    if (address == NULL)
        return;

    g_assert_true(clawt_tailscale_is_tailnet_address(address));

    parsed = g_inet_address_new_from_string(address);
    g_assert_nonnull(parsed);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/tailscale/v4-range", test_the_v4_range_is_a_ten_bit_prefix);
    g_test_add_func("/tailscale/other-addresses",
                    test_other_addresses_are_refused);
    g_test_add_func("/tailscale/v6-range",
                    test_the_v6_range_is_matched_on_its_prefix);
    g_test_add_func("/tailscale/nonsense", test_nonsense_is_not_an_address);
    g_test_add_func("/tailscale/interface-name",
                    test_the_interface_is_recognised_by_name);
    g_test_add_func("/tailscale/found-is-bindable",
                    test_any_address_found_is_bindable);

    return g_test_run();
}
