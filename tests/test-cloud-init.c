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
                                      NULL, NULL, &error);
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
                                        NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(again, ==, iso);

    g_assert_cmpint(g_stat(iso, &after), ==, 0);
    g_assert_cmpint(before.st_mtime, ==, after.st_mtime);

    clawt_test_remove_tree(dir);
}


/* ── Shares the guest actually mounts ────────────────────────────── */

static GPtrArray *
virtiofs_mounts(void)
{
    GPtrArray *mounts =
        g_ptr_array_new_with_free_func((GDestroyNotify)clawt_mount_free);
    ClawtMount *mount;

    /* Deliberately out of order: the renderer has to sort them. */
    mount = clawt_mount_new("/host/exchange/scribe",
                            "/mnt/clawtilla/exchange/scribe");
    clawt_mount_set_mount_type(mount, CLAWT_MOUNT_VIRTIOFS);
    clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RW);
    g_ptr_array_add(mounts, mount);

    mount = clawt_mount_new("/host/exchange", "/mnt/clawtilla/exchange");
    clawt_mount_set_mount_type(mount, CLAWT_MOUNT_VIRTIOFS);
    clawt_mount_set_mode(mount, CLAWT_MOUNT_MODE_RO);
    g_ptr_array_add(mounts, mount);

    return mounts;
}

/*
 * A <filesystem> device hands the guest a tag and nothing else.
 *
 * Nothing mounted it, so every share was a device the guest never used --
 * and an agent that went looking found an empty directory, created it by
 * hand, and reported the share as missing. The host side was right the
 * whole time.
 */
static void
test_the_guest_gets_fstab_entries_for_its_shares(void)
{
    g_autoptr(GPtrArray) mounts = virtiofs_mounts();
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data_full("agent", NULL, NULL, NULL,
                                              mounts);

    g_assert_nonnull(strstr(data, "mounts:"));
    g_assert_nonnull(strstr(data, "\"virtiofs\""));
    g_assert_nonnull(strstr(data, "\"/mnt/clawtilla/exchange\""));
    g_assert_nonnull(strstr(data, "\"/mnt/clawtilla/exchange/scribe\""));

    /*
     * nofail: a share whose backing daemon did not start must not stop
     * the guest booting. A VM that will not boot because a directory is
     * missing is far worse than one that boots without it.
     */
    g_assert_nonnull(strstr(data, "nofail"));

    /* Read-only on the host stays read-only in the guest. */
    g_assert_nonnull(strstr(data, "\"ro,nofail\""));
}

/*
 * Parent before child, because the exchange nests: the whole directory
 * read-only with read-write mounts inside it.  A child mounted first
 * disappears under its parent the moment the parent arrives.
 */
static void
test_a_nested_share_is_mounted_after_its_parent(void)
{
    g_autoptr(GPtrArray) mounts = virtiofs_mounts();
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data_full("agent", NULL, NULL, NULL,
                                              mounts);
    const gchar *block = strstr(data, "mounts:");
    const gchar *parent;
    const gchar *child;

    g_assert_nonnull(block);

    parent = strstr(block, "\"/mnt/clawtilla/exchange\"");
    child = strstr(block, "\"/mnt/clawtilla/exchange/scribe\"");

    g_assert_nonnull(parent);
    g_assert_nonnull(child);
    g_assert_true(parent < child);
}

/* A guest with nothing shared gets no mounts block at all. */
static void
test_no_shares_means_no_mounts_block(void)
{
    g_autofree gchar *data =
        clawt_cloud_init_build_user_data_full("agent", NULL, NULL, NULL,
                                              NULL);

    g_assert_null(strstr(data, "mounts:"));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/cloud-init/mounts/guest-gets-fstab-entries",
                    test_the_guest_gets_fstab_entries_for_its_shares);
    g_test_add_func("/cloud-init/mounts/nested-after-its-parent",
                    test_a_nested_share_is_mounted_after_its_parent);
    g_test_add_func("/cloud-init/mounts/none-means-no-block",
                    test_no_shares_means_no_mounts_block);
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
