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


/* ── Which distribution is in there ──────────────────────────────── */

static gchar *render_for(ClawtGuestFlavour flavour);

/*
 * The catalog is authoritative, and every image in it must place itself.
 * An entry that resolves to AUTO would install Fedora names into
 * whatever it actually is, and the only symptom is a failed package
 * install inside a guest's log.
 */
static void
test_every_catalog_image_knows_its_family(void)
{
    gsize n = 0;
    const ClawtVmImageSource *catalog = clawt_vm_image_catalog(&n);
    gsize i;

    g_assert_cmpuint(n, >, 0);

    for (i = 0; i < n; i++) {
        g_assert_cmpint(catalog[i].flavour, !=, CLAWT_GUEST_FLAVOUR_AUTO);
        g_assert_cmpint(clawt_vm_image_flavour(catalog[i].id), ==,
                        catalog[i].flavour);
    }
}

/*
 * An image somebody fetched keeps the distribution in its filename, and
 * that is the only clue there is short of booting it.
 */
static void
test_a_downloaded_image_is_placed_by_its_name(void)
{
    g_assert_cmpint(clawt_vm_image_flavour(
                        "/var/lib/img/Fedora-Cloud-Base-Generic-44-1.5"
                        ".x86_64.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_FEDORA);

    g_assert_cmpint(clawt_vm_image_flavour(
                        "/var/lib/img/debian-13-generic-amd64.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_DEBIAN);

    g_assert_cmpint(clawt_vm_image_flavour(
                        "/var/lib/img/CentOS-Stream-GenericCloud-10.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_ENTERPRISE);

    /* A directory is as good a clue as a filename. */
    g_assert_cmpint(clawt_vm_image_flavour("~/images/ubuntu/disk.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_UBUNTU);
}

/*
 * Ubuntu's own cloud images never say "ubuntu" -- the file is
 * `noble-server-cloudimg-amd64.img`, named after the release adjective.
 * Matching only on the distribution name would place every Ubuntu image
 * as unknown.
 */
static void
test_an_ubuntu_image_does_not_say_ubuntu(void)
{
    g_assert_cmpint(clawt_vm_image_flavour(
                        "noble-server-cloudimg-amd64.img"),
                    ==, CLAWT_GUEST_FLAVOUR_UBUNTU);
}

/*
 * The one entry in the whole table where Debian and Ubuntu disagree, and
 * it fails on both if a single family has to pick.  Debian stable has no
 * `firefox` package -- only `firefox-esr` -- and Ubuntu has no
 * `firefox-esr`; cloud-init treats a package it cannot find as a failure
 * of the whole install, so the wrong name costs the desktop rather than
 * the browser.
 */
static void
test_debian_and_ubuntu_want_different_firefoxes(void)
{
    g_autofree gchar *debian = render_for(CLAWT_GUEST_FLAVOUR_DEBIAN);
    g_autofree gchar *ubuntu = render_for(CLAWT_GUEST_FLAVOUR_UBUNTU);

    g_assert_nonnull(strstr(debian, "- \"firefox-esr\""));
    g_assert_null(strstr(debian, "- \"firefox\""));

    g_assert_nonnull(strstr(ubuntu, "- \"firefox\""));
    g_assert_null(strstr(ubuntu, "firefox-esr"));

    /* Everything else about them is the same. */
    g_assert_nonnull(strstr(ubuntu, "gdm3.service"));
    g_assert_nonnull(strstr(ubuntu, "- \"python3-gi\""));
    g_assert_nonnull(strstr(ubuntu, "- \"python3-venv\""));
}

/*
 * Arch is the only rolling distribution here, and the only one where
 * refreshing the index without upgrading is actively dangerous: that is
 * the partial upgrade pacman warns about, where a package installs
 * against libraries the image still has old versions of.  It breaks
 * inside the guest, after the desktop appears to have installed.
 */
static void
test_arch_upgrades_before_it_installs(void)
{
    g_autofree gchar *arch = render_for(CLAWT_GUEST_FLAVOUR_ARCH);
    g_autofree gchar *fedora = render_for(CLAWT_GUEST_FLAVOUR_FEDORA);

    g_assert_nonnull(strstr(arch, "package_upgrade: true"));

    /* Arch's names drop the 3 that everyone else puts in. */
    g_assert_nonnull(strstr(arch, "- \"python-gobject\""));
    g_assert_nonnull(strstr(arch, "- \"python-pip\""));
    g_assert_null(strstr(arch, "python3-"));

    /*
     * glib-compile-schemas is in glib2-devel since Arch split the
     * development tools out; a cloud image has the library and not the
     * compiler, so the extension's schema would never build.
     */
    g_assert_nonnull(strstr(arch, "- \"glib2-devel\""));

    g_assert_nonnull(strstr(arch, "[systemctl, enable, --now, gdm.service]"));

    /*
     * And a point release does not pay for an upgrade it does not need:
     * this is per family rather than always on.
     */
    g_assert_null(strstr(fedora, "package_upgrade"));
}

/*
 * "arch" is a substring of words that turn up in ordinary paths, and
 * matching on it would place `~/archived-vms/debian.qcow2` as Arch and
 * install pacman's names into a Debian.
 */
static void
test_an_ordinary_path_is_not_arch(void)
{
    g_assert_cmpint(clawt_vm_image_flavour(
                        "/home/me/research/disk.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_AUTO);

    g_assert_cmpint(clawt_vm_image_flavour(
                        "~/archived-vms/debian-13-generic-amd64.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_DEBIAN);

    /* The real thing still resolves, by either spelling. */
    g_assert_cmpint(clawt_vm_image_flavour(
                        "Arch-Linux-x86_64-cloudimg.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_ARCH);
    g_assert_cmpint(clawt_vm_image_flavour("~/vms/archlinux.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_ARCH);
}

/*
 * A desktop with no browser on it gives an agent a wallpaper to look at.
 */
static void
test_every_family_gets_a_browser(void)
{
    ClawtGuestFlavour families[] = {
        CLAWT_GUEST_FLAVOUR_FEDORA, CLAWT_GUEST_FLAVOUR_ENTERPRISE,
        CLAWT_GUEST_FLAVOUR_DEBIAN, CLAWT_GUEST_FLAVOUR_UBUNTU,
        CLAWT_GUEST_FLAVOUR_ARCH
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(families); i++) {
        g_autofree gchar *data = render_for(families[i]);

        g_assert_nonnull(strstr(data, "firefox"));
    }
}

static void
test_a_configured_flavour_beats_the_image(void)
{
    /* The case the key exists for: an image with an unhelpful name. */
    g_assert_cmpint(clawt_guest_desktop_resolve_flavour("debian",
                                                        "disk.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_DEBIAN);

    /* And it wins even when the image says otherwise -- a rebuild. */
    g_assert_cmpint(clawt_guest_desktop_resolve_flavour(
                        "enterprise", "fedora-44"),
                    ==, CLAWT_GUEST_FLAVOUR_ENTERPRISE);

    /* "auto" is not an answer; it means keep looking. */
    g_assert_cmpint(clawt_guest_desktop_resolve_flavour("auto", "debian-13"),
                    ==, CLAWT_GUEST_FLAVOUR_DEBIAN);
}

/*
 * Reported rather than guessed.  The caller warns and names the key; a
 * silent fallback here would install the wrong package names and the
 * failure would surface inside a guest nobody is looking at.
 */
static void
test_an_unplaceable_image_says_so(void)
{
    g_assert_cmpint(clawt_guest_desktop_resolve_flavour(NULL, "disk.qcow2"),
                    ==, CLAWT_GUEST_FLAVOUR_AUTO);
    g_assert_cmpint(clawt_guest_desktop_resolve_flavour(NULL, NULL),
                    ==, CLAWT_GUEST_FLAVOUR_AUTO);
}

/* ── What each family actually gets ──────────────────────────────── */

static gchar *
render_for(ClawtGuestFlavour flavour)
{
    g_autoptr(ClawtGuestDesktop) desktop = clawt_guest_desktop_new("clawt");

    clawt_guest_desktop_set_mcp_repo(desktop, "https://example.invalid/x.git");
    clawt_guest_desktop_set_flavour(desktop, flavour);
    clawt_guest_desktop_set_autologin(desktop, TRUE);

    return clawt_cloud_init_build_user_data("root", NULL, "clawt-vm",
                                            desktop);
}

/*
 * The one that was wrong for the whole life of the feature.
 *
 * Debian's package is gdm3 and so is its unit, PyGObject is python3-gi,
 * and a Debian cloud image has neither `python3 -m venv` nor
 * `glib-compile-schemas` nor the `dconf` binary until something asks.
 * Each of those on its own leaves a guest that installed a desktop and
 * has no session, or a session with no automation.
 */
static void
test_a_debian_guest_gets_debian_names(void)
{
    g_autofree gchar *data = render_for(CLAWT_GUEST_FLAVOUR_DEBIAN);

    g_assert_nonnull(strstr(data, "- \"gdm3\""));
    g_assert_nonnull(strstr(data, "gdm3.service"));

    g_assert_nonnull(strstr(data, "- \"python3-gi\""));
    g_assert_nonnull(strstr(data, "- \"python3-venv\""));
    g_assert_nonnull(strstr(data, "- \"libglib2.0-bin\""));
    g_assert_nonnull(strstr(data, "- \"dconf-cli\""));

    /* And none of Fedora's spellings for the same things. */
    g_assert_null(strstr(data, "- \"gdm\""));
    g_assert_null(strstr(data, "- \"python3-gobject\""));
    g_assert_null(strstr(data, "[systemctl, enable, --now, gdm.service]"));
}

static void
test_a_fedora_guest_gets_fedora_names(void)
{
    g_autofree gchar *data = render_for(CLAWT_GUEST_FLAVOUR_FEDORA);

    g_assert_nonnull(strstr(data, "- \"gdm\""));
    g_assert_nonnull(strstr(data, "- \"python3-gobject\""));
    g_assert_nonnull(strstr(data,
                            "[systemctl, enable, --now, gdm.service]"));

    g_assert_null(strstr(data, "- \"gdm3\""));
    g_assert_null(strstr(data, "- \"python3-gi\""));
}

/*
 * cloud-init treats a package it cannot find as a failure of the whole
 * install, so asking Enterprise Linux for a Fedora-only package takes
 * the desktop down with it rather than merely missing an editor.
 */
static void
test_enterprise_asks_for_nothing_fedora_only(void)
{
    g_autofree gchar *data = render_for(CLAWT_GUEST_FLAVOUR_ENTERPRISE);

    g_assert_nonnull(strstr(data, "- \"gdm\""));
    g_assert_nonnull(strstr(data, "- \"gnome-shell\""));
    g_assert_nonnull(strstr(data,
                            "[systemctl, enable, --now, gdm.service]"));

    g_assert_null(strstr(data, "gnome-console"));
    g_assert_null(strstr(data, "gnome-text-editor"));
    g_assert_null(strstr(data, "gnome-tweaks"));
    g_assert_null(strstr(data, "gnome-shell-extension-common"));
}

/*
 * A list somebody wrote replaces the family's rather than merging with
 * it -- merging would quietly reinstate a package they removed on
 * purpose, and there would be no way to say "not that one".
 *
 * It does not reach the packages the MCP server needs, which are not the
 * desktop: trimming this list must not switch automation off by
 * accident.
 */
static void
test_an_explicit_list_replaces_the_familys(void)
{
    g_autoptr(ClawtGuestDesktop) desktop = clawt_guest_desktop_new("clawt");
    static const gchar *const only[] = { "gdm3", "gnome-shell", NULL };
    g_autofree gchar *data = NULL;

    clawt_guest_desktop_set_flavour(desktop, CLAWT_GUEST_FLAVOUR_DEBIAN);
    clawt_guest_desktop_set_mcp_repo(desktop, "https://example.invalid/x.git");
    clawt_guest_desktop_set_packages(desktop, only);

    data = clawt_cloud_init_build_user_data("root", NULL, "clawt-vm",
                                            desktop);

    g_assert_nonnull(strstr(data, "- \"gdm3\""));
    g_assert_null(strstr(data, "nautilus"));

    /* Still Debian's, because these are not the desktop. */
    g_assert_nonnull(strstr(data, "- \"python3-gi\""));
    g_assert_nonnull(strstr(data, "- \"python3-venv\""));
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

    g_test_add_func("/guest-desktop/flavour/catalog-places-itself",
                    test_every_catalog_image_knows_its_family);
    g_test_add_func("/guest-desktop/flavour/from-a-filename",
                    test_a_downloaded_image_is_placed_by_its_name);
    g_test_add_func("/guest-desktop/flavour/ubuntu-never-says-ubuntu",
                    test_an_ubuntu_image_does_not_say_ubuntu);
    g_test_add_func("/guest-desktop/flavour/configured-wins",
                    test_a_configured_flavour_beats_the_image);
    g_test_add_func("/guest-desktop/flavour/unplaceable-says-so",
                    test_an_unplaceable_image_says_so);

    g_test_add_func("/guest-desktop/cloud-config/debian-names",
                    test_a_debian_guest_gets_debian_names);
    g_test_add_func("/guest-desktop/cloud-config/fedora-names",
                    test_a_fedora_guest_gets_fedora_names);
    g_test_add_func("/guest-desktop/cloud-config/enterprise-names",
                    test_enterprise_asks_for_nothing_fedora_only);
    g_test_add_func("/guest-desktop/cloud-config/explicit-list-replaces",
                    test_an_explicit_list_replaces_the_familys);
    g_test_add_func("/guest-desktop/cloud-config/firefox-differs",
                    test_debian_and_ubuntu_want_different_firefoxes);
    g_test_add_func("/guest-desktop/cloud-config/every-family-browses",
                    test_every_family_gets_a_browser);
    g_test_add_func("/guest-desktop/cloud-config/arch-upgrades-first",
                    test_arch_upgrades_before_it_installs);
    g_test_add_func("/guest-desktop/flavour/arch-is-not-a-substring",
                    test_an_ordinary_path_is_not_arch);

    return g_test_run();
}
