/*
 * test-guest-desktop.c - A GNOME session inside the agent's own VM
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Everything here is a pure function, and that is the point.  The other
 * way to find out that this rendering is wrong is to boot a VM, wait for
 * a distribution to install a desktop over the network, and look at a
 * black screen with nothing to say why.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

static ClawtGuestDesktop *
build(const gchar *session_user)
{
    static const gchar *const packages[] = {
        "gdm", "gnome-shell", NULL
    };
    ClawtGuestDesktop *desktop = clawt_guest_desktop_new(session_user);

    clawt_guest_desktop_set_packages(desktop, packages);
    clawt_guest_desktop_set_mcp_repo(desktop, "https://example.invalid/x.git");

    return desktop;
}

/*
 * GDM refuses to log root in, and the default ssh_user is root -- so a VM
 * left entirely alone needs a second account or it has no session at all.
 */
static void
test_root_gets_a_separate_session_account(void)
{
    g_autofree gchar *user = clawt_guest_desktop_resolve_user(NULL, "root");

    g_assert_cmpstr(user, ==, "clawt");
}

/*
 * A login that GDM will accept is used as it stands, so there is one
 * account rather than two for no reason.
 */
static void
test_a_non_root_login_is_used_as_it_is(void)
{
    g_autofree gchar *user = clawt_guest_desktop_resolve_user(NULL, "fedora");

    g_assert_cmpstr(user, ==, "fedora");
}

static void
test_a_named_session_user_wins(void)
{
    g_autofree gchar *user =
        clawt_guest_desktop_resolve_user("kiosk", "root");

    g_assert_cmpstr(user, ==, "kiosk");
}

/*
 * Nothing configured at all still has to name somebody.  Returning NULL
 * here would render a cloud-config with an empty AutomaticLogin, which
 * GDM accepts and then logs nobody in.
 */
static void
test_no_login_at_all_still_names_an_account(void)
{
    g_autofree gchar *user = clawt_guest_desktop_resolve_user(NULL, NULL);

    g_assert_cmpstr(user, ==, "clawt");
}

/*
 * cloud-config is a mapping, and a duplicate key is not an error in YAML:
 * the last one wins and everything under the first is discarded without a
 * word. Both halves of the desktop -- the account and everything else --
 * are appended around an existing document, so this is the mistake that
 * is easiest to make and hardest to see.
 */
static void
test_the_document_names_no_key_twice(void)
{
    g_autoptr(ClawtGuestDesktop) desktop = build("clawt");
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("root", "ssh-ed25519 AAAA x",
                                         "clawt-vm", desktop);
    g_auto(GStrv) lines = g_strsplit(data, "\n", -1);
    g_autoptr(GHashTable) seen =
        g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gsize i;

    for (i = 0; lines[i] != NULL; i++) {
        const gchar *line = lines[i];
        const gchar *colon;

        /* Only top-level keys: anything indented belongs to one. */
        if (line[0] == '\0' || g_ascii_isspace(line[0]) || line[0] == '#')
            continue;

        colon = strchr(line, ':');

        if (colon == NULL)
            continue;

        {
            g_autofree gchar *key = g_strndup(line, (gsize)(colon - line));

            g_assert_false(g_hash_table_contains(seen, key));
            g_hash_table_add(seen, g_steal_pointer(&key));
        }
    }

    /* And the keys that matter are all there exactly once. */
    g_assert_true(g_hash_table_contains(seen, "users"));
    g_assert_true(g_hash_table_contains(seen, "packages"));
    g_assert_true(g_hash_table_contains(seen, "write_files"));
    g_assert_true(g_hash_table_contains(seen, "runcmd"));
}

/*
 * The session account is created, authorised with the same key, and the
 * root login it runs commands as is left alone.
 */
static void
test_both_accounts_are_created(void)
{
    g_autoptr(ClawtGuestDesktop) desktop = build("clawt");
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("root", "ssh-ed25519 AAAA x",
                                         "clawt-vm", desktop);

    g_assert_nonnull(strstr(data, "- name: \"root\""));
    g_assert_nonnull(strstr(data, "- name: \"clawt\""));

    /*
     * The key reaches the session account too, and has to: the MCP server
     * runs as whoever is logged in at the screen, so it is reached by an
     * SSH connection as that account rather than as root.
     */
    g_assert_cmpint(clawt_test_count_substrings(data, "ssh-ed25519 AAAA x"),
                    ==, 3);
}

/*
 * ...and one account when they are the same login.  cloud-init names a
 * duplicate as an error and then creates neither, which would leave a VM
 * nothing can log into.
 */
static void
test_one_account_when_the_logins_match(void)
{
    g_autoptr(ClawtGuestDesktop) desktop = build("fedora");
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("fedora", "ssh-ed25519 AAAA x",
                                         "clawt-vm", desktop);

    g_assert_cmpint(clawt_test_count_substrings(data, "- name: \"fedora\""),
                    ==, 1);
}

/*
 * There is nobody at the console to type a password, and until a session
 * exists there is nothing to drive: the extension runs inside GNOME Shell.
 */
static void
test_autologin_names_the_session_account(void)
{
    g_autoptr(ClawtGuestDesktop) desktop = build("clawt");
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("root", NULL, "clawt-vm", desktop);

    g_assert_nonnull(strstr(data, "AutomaticLoginEnable=true"));
    g_assert_nonnull(strstr(data, "AutomaticLogin=clawt"));

    /* Both families that ship GDM spell its config file differently. */
    g_assert_nonnull(strstr(data, "/etc/gdm/custom.conf"));
    g_assert_nonnull(strstr(data, "/etc/gdm3/daemon.conf"));

    /* A cloud image boots to multi-user.target and would never start it. */
    g_assert_nonnull(strstr(data, "set-default, graphical.target"));
}

static void
test_autologin_can_be_turned_off(void)
{
    g_autoptr(ClawtGuestDesktop) desktop = build("clawt");
    g_autofree gchar *data = NULL;

    clawt_guest_desktop_set_autologin(desktop, FALSE);
    data = clawt_cloud_init_build_user_data("root", NULL, "clawt-vm",
                                            desktop);

    g_assert_null(strstr(data, "AutomaticLoginEnable"));
    g_assert_null(strstr(data, "gdm.service"));
}

/*
 * The three things that would each, on their own, leave the agent looking
 * at a desktop it cannot touch: an extension that is not enabled, a
 * consent dialog nobody is there to dismiss, and a lock screen nobody can
 * answer.
 */
static void
test_automation_needs_nobody_present(void)
{
    g_autoptr(ClawtGuestDesktop) desktop = build("clawt");
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("root", NULL, "clawt-vm", desktop);

    g_assert_nonnull(strstr(data,
        "enabled-extensions=['desktop-automation@gnomemcp.github.io']"));
    g_assert_nonnull(strstr(data, "consent-acknowledged=true"));
    g_assert_nonnull(strstr(data, "lock-enabled=false"));

    /*
     * The extension still starts with automation switched off in memory,
     * and the only other way to switch it on is a click on the top bar.
     */
    g_assert_nonnull(strstr(data,
        "io.github.gnomemcp.DesktopAutomation.SetEnabled"));
    g_assert_nonnull(strstr(data, "/etc/xdg/autostart/"));

    /*
     * And it must NOT set X-GNOME-Autostart-Phase.  gnome-session stopped
     * managing session services, so an entry that sets it is skipped --
     * the script never ran, and every tool answered "Automation disabled
     * by user. Enable from top bar indicator." on a machine with no top
     * bar to click.
     */
    g_assert_null(strstr(data, "X-GNOME-Autostart-Phase"));
}

/*
 * The prerequisites for building the MCP server are added by the renderer
 * rather than listed in the config, so trimming the package list cannot
 * silently disable automation.
 */
static void
test_the_mcp_prerequisites_are_not_the_operators_problem(void)
{
    g_autoptr(ClawtGuestDesktop) desktop = build("clawt");
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("root", NULL, "clawt-vm", desktop);

    g_assert_nonnull(strstr(data, "- \"git\""));
    g_assert_nonnull(strstr(data, "- \"python3-gobject\""));
    g_assert_nonnull(strstr(data, "https://example.invalid/x.git"));

    /*
     * A virtualenv, because Fedora refuses a pip install into an
     * externally-managed /usr; --system-site-packages, because PyGObject
     * comes from the distribution and building it here would want a
     * compiler and a set of -devel packages for a library already
     * installed.
     */
    g_assert_nonnull(strstr(data, "venv, --system-site-packages"));

    /*
     * And no version constraints of clawtilla's own.  Those belong in the
     * pyproject of the thing being cloned, where they can be raised in
     * step with the code that needs raising; a copy held here would go
     * stale silently and in the wrong direction.
     */
    g_assert_null(strstr(data, "\"mcp<"));
    g_assert_null(strstr(data, "\"mcp>"));
}

static void
test_a_desktop_without_mcp_installs_no_automation(void)
{
    g_autoptr(ClawtGuestDesktop) desktop = build("clawt");
    g_autofree gchar *data = NULL;

    clawt_guest_desktop_set_install_mcp(desktop, FALSE);
    data = clawt_cloud_init_build_user_data("root", NULL, "clawt-vm",
                                            desktop);

    g_assert_null(strstr(data, "consent-acknowledged"));
    g_assert_null(strstr(data, "example.invalid"));

    /* ...but the desktop itself is still installed and logged in. */
    g_assert_nonnull(strstr(data, "AutomaticLogin=clawt"));

    /* The lock screen is still nobody's to answer. */
    g_assert_nonnull(strstr(data, "lock-enabled=false"));
}

/*
 * A headless VM is unchanged.  This is the configuration every existing
 * agent has, so it is the one that must not have moved.
 */
static void
test_no_desktop_renders_nothing_extra(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("root", "ssh-ed25519 AAAA x",
                                         "clawt-vm", NULL);

    g_assert_null(strstr(data, "packages:"));
    g_assert_null(strstr(data, "write_files:"));
    g_assert_null(strstr(data, "runcmd:"));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/guest-desktop/user/root-gets-its-own",
                    test_root_gets_a_separate_session_account);
    g_test_add_func("/guest-desktop/user/non-root-kept",
                    test_a_non_root_login_is_used_as_it_is);
    g_test_add_func("/guest-desktop/user/named-wins",
                    test_a_named_session_user_wins);
    g_test_add_func("/guest-desktop/user/always-names-somebody",
                    test_no_login_at_all_still_names_an_account);
    g_test_add_func("/guest-desktop/cloud-config/no-duplicate-keys",
                    test_the_document_names_no_key_twice);
    g_test_add_func("/guest-desktop/cloud-config/both-accounts",
                    test_both_accounts_are_created);
    g_test_add_func("/guest-desktop/cloud-config/one-account-when-same",
                    test_one_account_when_the_logins_match);
    g_test_add_func("/guest-desktop/cloud-config/autologin",
                    test_autologin_names_the_session_account);
    g_test_add_func("/guest-desktop/cloud-config/autologin-off",
                    test_autologin_can_be_turned_off);
    g_test_add_func("/guest-desktop/cloud-config/unattended-automation",
                    test_automation_needs_nobody_present);
    g_test_add_func("/guest-desktop/cloud-config/mcp-prerequisites",
                    test_the_mcp_prerequisites_are_not_the_operators_problem);
    g_test_add_func("/guest-desktop/cloud-config/mcp-off",
                    test_a_desktop_without_mcp_installs_no_automation);
    g_test_add_func("/guest-desktop/cloud-config/headless-unchanged",
                    test_no_desktop_renders_nothing_extra);

    return g_test_run();
}
