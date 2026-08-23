/*
 * test-cloud-init.c - Handing a stock cloud image a login
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

/*
 * A cloud image creates its own default account -- fedora, debian, ubuntu
 * -- unless the user list says otherwise, and that account is not the one
 * clawtilla holds a key for.  Naming a user without "default" is what
 * suppresses it, so its absence is the assertion.
 */
static void
test_user_data_creates_only_the_named_user(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("agent", "ssh-ed25519 AAAA x",
                                         "clawt-scribe", NULL);

    g_assert_nonnull(strstr(data, "#cloud-config"));
    g_assert_nonnull(strstr(data, "- name: \"agent\""));
    g_assert_null(strstr(data, "- default"));
}

/*
 * Left to itself cloud-init replaces root's authorized key with a command
 * that prints a refusal, so the key looks installed and the login still
 * fails.
 */
static void
test_user_data_permits_root_when_root_is_the_login(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("root", "ssh-ed25519 AAAA x", NULL,
                                         NULL);

    g_assert_nonnull(strstr(data, "disable_root: false"));

    /* root does not need to be given sudo, and asking is a config error. */
    g_assert_null(strstr(data, "sudo:"));
}

/*
 * The bug a real guest found: cloud-init installs a key from a `users:`
 * entry only while creating that account, and skips an account that
 * already exists.  root always exists, so the default login booted with
 * a key it had never been told about and refused every connection.  The
 * top-level list is written by a different module and does reach root.
 */
static void
test_user_data_authorises_the_key_at_the_top_level_too(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("root", "ssh-ed25519 AAAA x", NULL,
                                         NULL);
    const gchar *first;
    const gchar *second;

    first = strstr(data, "ssh_authorized_keys:");
    g_assert_nonnull(first);

    second = strstr(first + 1, "ssh_authorized_keys:");
    g_assert_nonnull(second);
}

static void
test_user_data_gives_a_non_root_login_sudo(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("agent", "ssh-ed25519 AAAA x", NULL,
                                         NULL);

    g_assert_nonnull(strstr(data, "disable_root: true"));
    g_assert_nonnull(strstr(data, "ALL=(ALL) NOPASSWD:ALL"));
}

/*
 * Passwords are never set on the account, so password authentication could
 * only ever admit somebody who is not us.
 */
static void
test_user_data_refuses_password_authentication(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("agent", "ssh-ed25519 AAAA x", NULL,
                                         NULL);

    g_assert_nonnull(strstr(data, "ssh_pwauth: false"));
}

/*
 * A key's comment is whatever was on the machine that generated it.  A
 * quote in it must not end the scalar and turn the rest into YAML, which
 * would be a parse error cloud-init reports on the guest's console where
 * nobody is looking.
 */
static void
test_user_data_escapes_a_quote_in_the_key(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("agent",
                                         "ssh-ed25519 AAAA zach@\"host\"",
                                         NULL, NULL);

    g_assert_nonnull(strstr(data, "\\\"host\\\""));
    g_assert_null(strstr(data, "zach@\"host\""));
}

static void
test_user_data_without_a_key_authorises_nothing(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data("agent", NULL, NULL, NULL);

    g_assert_nonnull(strstr(data, "- name: \"agent\""));
    g_assert_null(strstr(data, "ssh_authorized_keys"));
}

/*
 * cloud-init reruns its per-instance modules when instance-id changes, so
 * a stable one is what keeps a reboot from redoing first-boot setup.
 */
static void
test_meta_data_carries_the_instance_id(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_meta_data("clawt-scribe", "clawt-scribe");

    g_assert_nonnull(strstr(data, "instance-id: \"clawt-scribe\""));
    g_assert_nonnull(strstr(data, "local-hostname: \"clawt-scribe\""));
}

/*
 * NoCloud finds the seed by filesystem label and by nothing else, so the
 * volume id is not a name we are free to choose.
 */
static void
test_iso_argv_labels_the_volume_cidata(void)
{
    g_auto(GStrv) argv = clawt_cloud_init_build_iso_argv("xorrisofs",
                                                         "/tmp/seed.iso",
                                                         "/tmp/seed");
    gboolean saw_label = FALSE;
    guint i;

    g_assert_cmpstr(argv[0], ==, "xorrisofs");

    for (i = 0; argv[i] != NULL; i++) {
        if (g_strcmp0(argv[i], "-V") == 0 &&
            g_strcmp0(argv[i + 1], "cidata") == 0)
            saw_label = TRUE;
    }

    g_assert_true(saw_label);
}

/*
 * The whole point of the seed, end to end: a real ISO, actually labelled
 * so cloud-init will find it.  Skipped rather than failed where no ISO
 * writer is installed, since the seed builder reports that case itself.
 */
static void
test_write_seed_builds_a_labelled_image(void)
{
    g_autofree gchar *dir = NULL;
    g_autofree gchar *iso = NULL;
    g_autofree gchar *again = NULL;
    g_autoptr(GError) error = NULL;
    GStatBuf before;
    GStatBuf after;

    if (clawt_cloud_init_find_tool() == NULL) {
        g_test_skip("no ISO writer is installed here");
        return;
    }

    dir = g_dir_make_tmp("clawt-seed-XXXXXX", &error);
    g_assert_no_error(error);

    iso = clawt_cloud_init_write_seed(dir, "clawt-scribe", "agent",
                                      "ssh-ed25519 AAAA x", "clawt-scribe",
                                      NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(iso);
    g_assert_true(g_file_test(iso, G_FILE_TEST_EXISTS));

    g_assert_cmpint(g_stat(iso, &before), ==, 0);

    /*
     * An unchanged seed must not be rebuilt: it would be wasted work on
     * every start, and would keep handing the guest a new image to notice.
     */
    again = clawt_cloud_init_write_seed(dir, "clawt-scribe", "agent",
                                        "ssh-ed25519 AAAA x", "clawt-scribe",
                                        NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(again, ==, iso);

    g_assert_cmpint(g_stat(iso, &after), ==, 0);
    g_assert_cmpint(before.st_mtime, ==, after.st_mtime);

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/cloud-init/user-data/only-the-named-user",
                    test_user_data_creates_only_the_named_user);
    g_test_add_func("/cloud-init/user-data/root-permitted",
                    test_user_data_permits_root_when_root_is_the_login);
    g_test_add_func("/cloud-init/user-data/top-level-key",
                    test_user_data_authorises_the_key_at_the_top_level_too);
    g_test_add_func("/cloud-init/user-data/non-root-sudo",
                    test_user_data_gives_a_non_root_login_sudo);
    g_test_add_func("/cloud-init/user-data/no-passwords",
                    test_user_data_refuses_password_authentication);
    g_test_add_func("/cloud-init/user-data/escapes-quotes",
                    test_user_data_escapes_a_quote_in_the_key);
    g_test_add_func("/cloud-init/user-data/no-key",
                    test_user_data_without_a_key_authorises_nothing);
    g_test_add_func("/cloud-init/meta-data/instance-id",
                    test_meta_data_carries_the_instance_id);
    g_test_add_func("/cloud-init/iso/volume-id",
                    test_iso_argv_labels_the_volume_cidata);
    g_test_add_func("/cloud-init/seed/builds-and-reuses",
                    test_write_seed_builds_a_labelled_image);

    return g_test_run();
}
