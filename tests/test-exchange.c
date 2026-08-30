/*
 * test-exchange.c - The shared drop-box, and the cap on how big it gets
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * defaults.exchange_max_bytes had been declared, defaulted to a
 * gigabyte, generated into the example config and documented since the
 * schema was written, while the only function that reads it --
 * clawt_exchange_sweep() -- was called by nothing outside a test.  So
 * the tests here are about the caller as much as the arithmetic: a
 * limit needs a test that *reaches* it, not one that shows it exists.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>
#include <utime.h>

#include "clawt-test-util.h"

/*
 * Writes @bytes bytes at @relative and dates the result @age_days ago.
 *
 * The age is set rather than waited for: the sweep removes oldest
 * first, and a test that wrote three files in the same millisecond
 * would be asserting on whatever order the sort happened to produce.
 */
static void
write_aged_file(const gchar *root, const gchar *relative, gsize bytes,
                gint age_days)
{
    g_autofree gchar *path = g_build_filename(root, relative, NULL);
    g_autofree gchar *dir = g_path_get_dirname(path);
    g_autofree gchar *content = g_strnfill(bytes, 'x');
    struct utimbuf times;

    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(path, content, (gssize)bytes, NULL));

    times.actime = time(NULL) - (time_t)age_days * 24 * 60 * 60;
    times.modtime = times.actime;

    g_assert_cmpint(g_utime(path, &times), ==, 0);
}

static gboolean
exists(const gchar *root, const gchar *relative)
{
    g_autofree gchar *path = g_build_filename(root, relative, NULL);

    return g_file_test(path, G_FILE_TEST_EXISTS);
}

/*
 * Preparing the exchange applies the cap.
 *
 * This is the whole of the fix: the daemon prepares the exchange when an
 * agent starts and again before every file put, and those are the only
 * moments it knows the thing can have grown.  Before, nothing anywhere
 * called the sweep, so a gigabyte cap had never once removed a byte.
 */
static void
test_preparing_applies_the_cap(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-exchange-XXXXXX", NULL);
    g_autoptr(ClawtExchange) exchange = NULL;
    g_autoptr(GError) error = NULL;

    write_aged_file(root, "shared/oldest.bin", 1000, 30);
    write_aged_file(root, "shared/middle.bin", 1000, 20);
    write_aged_file(root, "alice/newest.bin", 1000, 1);

    exchange = clawt_exchange_new(root, 2500);
    g_assert_cmpint(clawt_exchange_get_size(exchange), ==, 3000);

    g_assert_true(clawt_exchange_prepare(exchange, "alice", &error));
    g_assert_no_error(error);

    g_assert_cmpint(clawt_exchange_get_size(exchange), <=, 2500);

    /* Oldest first, and only as far as it has to go. */
    g_assert_false(exists(root, "shared/oldest.bin"));
    g_assert_true(exists(root, "shared/middle.bin"));
    g_assert_true(exists(root, "alice/newest.bin"));

    clawt_test_remove_tree(root);
}

/*
 * Zero disables it, which the schema has always said.  A cap that
 * deleted files when it was switched off would be far worse than one
 * that never fired.
 */
static void
test_a_zero_cap_removes_nothing(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-exchange-XXXXXX", NULL);
    g_autoptr(ClawtExchange) exchange = NULL;
    g_autoptr(GError) error = NULL;

    write_aged_file(root, "shared/big.bin", 4000, 90);

    exchange = clawt_exchange_new(root, 0);

    g_assert_true(clawt_exchange_prepare(exchange, "alice", &error));
    g_assert_no_error(error);

    g_assert_true(exists(root, "shared/big.bin"));
    g_assert_cmpint(clawt_exchange_get_size(exchange), ==, 4000);

    clawt_test_remove_tree(root);
}

/*
 * An exchange under its cap is left entirely alone, including on the
 * path that runs at every agent start.  Sweeping a fleet's shared files
 * on every start would be a good deal worse than not capping them.
 */
static void
test_an_exchange_under_the_cap_is_untouched(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-exchange-XXXXXX", NULL);
    g_autoptr(ClawtExchange) exchange = NULL;
    g_autoptr(GError) error = NULL;

    write_aged_file(root, "shared/notes.txt", 100, 400);

    exchange = clawt_exchange_new(root, 1024 * 1024);

    g_assert_true(clawt_exchange_prepare(exchange, "alice", &error));
    g_assert_no_error(error);

    g_assert_true(exists(root, "shared/notes.txt"));

    clawt_test_remove_tree(root);
}

/*
 * Preparing still does what it always did.  The cap was added to this
 * function, so the directories it exists to create have to be asserted
 * on in the same place.
 */
static void
test_preparing_creates_the_directories(void)
{
    g_autofree gchar *root = g_build_filename(
        g_get_tmp_dir(), "clawt-exchange-fresh", NULL);
    g_autoptr(ClawtExchange) exchange = NULL;
    g_autoptr(GError) error = NULL;

    clawt_test_remove_tree(root);

    exchange = clawt_exchange_new(root, 1024);

    g_assert_true(clawt_exchange_prepare(exchange, "alice", &error));
    g_assert_no_error(error);

    g_assert_true(exists(root, "shared"));
    g_assert_true(exists(root, "alice"));

    /* And with no agent named, which is how the daemon prepares it first. */
    g_assert_true(clawt_exchange_prepare(exchange, NULL, &error));
    g_assert_no_error(error);

    clawt_test_remove_tree(root);
}

/*
 * The exchange reaches a VM guest's fstab, not only its device list.
 *
 * The mount type is what decides this and nothing was setting it. The
 * agent's own mounts were typed by clawt_computer_factory_create(); the
 * exchange is added by the daemon afterwards and kept the default,
 * CLAWT_MOUNT_BIND. The domain XML does not filter on the type, so the
 * guest was given a <filesystem> per exchange mount; the seed's
 * render_mounts() does filter, so none of them appeared in `mounts:`.
 * Five virtiofs devices, two fstab entries, and /mnt/clawtilla/exchange
 * simply absent inside every VM in the fleet -- which is the path every
 * qa persona tells an agent to hand its screenshots back through.
 *
 * Asserted through the seed rather than on the type alone, because the
 * type is only interesting for what it makes the renderer do: a test
 * that sets the type by hand and checks the block -- which is what
 * tests/test-cloud-init.c does -- passes throughout this bug.
 */
static void
test_the_exchange_reaches_a_guests_fstab(void)
{
    g_autofree gchar *root = g_build_filename(
        g_get_tmp_dir(), "clawt-exchange-vm", NULL);
    g_autoptr(ClawtExchange) exchange = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GPtrArray) mounts = NULL;
    g_autofree gchar *seed = NULL;
    const gchar *block;
    GPtrArray *applied;
    guint i;

    clawt_test_remove_tree(root);

    exchange = clawt_exchange_new(root, 0);
    mounts = clawt_exchange_get_mounts(exchange, "scribe");

    /* The whole exchange read-only, shared/, and the agent's own. */
    g_assert_cmpuint(mounts->len, ==, 3);

    /*
     * Added exactly as clawt_daemon_apply_computer() adds them, which is
     * the call site that had no type.
     */
    computer = clawt_vm_computer_new("scribe", CLAWT_VM_BACKEND_QEMU, NULL);

    for (i = 0; i < mounts->len; i++)
        clawt_computer_add_mount(computer, g_ptr_array_index(mounts, i));

    applied = clawt_computer_get_mounts(computer);
    g_assert_cmpuint(applied->len, ==, 3);

    for (i = 0; i < applied->len; i++)
        g_assert_cmpint(
            clawt_mount_get_mount_type(g_ptr_array_index(applied, i)),
            ==, CLAWT_MOUNT_VIRTIOFS);

    seed = clawt_cloud_init_build_user_data_full("root", "ssh-ed25519 AAAA",
                                                 "clawt-scribe", NULL,
                                                 applied);

    block = strstr(seed, "mounts:");
    g_assert_nonnull(block);
    g_assert_nonnull(strstr(block, "\"/mnt/clawtilla/exchange\""));
    g_assert_nonnull(strstr(block, "\"/mnt/clawtilla/exchange/shared\""));
    g_assert_nonnull(strstr(block, "\"/mnt/clawtilla/exchange/scribe\""));

    clawt_test_remove_tree(root);
}

/*
 * And a container's mounts are still bind mounts.
 *
 * The promotion is per backend, so the interesting half is the one that
 * must *not* happen: a virtiofs tag handed to podman is not a share, it
 * is a source path that does not exist.
 */
static void
test_a_container_keeps_bind_mounts(void)
{
    g_autofree gchar *root = g_build_filename(
        g_get_tmp_dir(), "clawt-exchange-ctr", NULL);
    g_autoptr(ClawtExchange) exchange = NULL;
    g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GPtrArray) mounts = NULL;
    GPtrArray *applied;
    guint i;

    clawt_test_remove_tree(root);

    exchange = clawt_exchange_new(root, 0);
    mounts = clawt_exchange_get_mounts(exchange, "scribe");

    computer = clawt_container_computer_new("scribe", bridge,
                                            "fedora:latest");

    for (i = 0; i < mounts->len; i++)
        clawt_computer_add_mount(computer, g_ptr_array_index(mounts, i));

    applied = clawt_computer_get_mounts(computer);

    for (i = 0; i < applied->len; i++)
        g_assert_cmpint(
            clawt_mount_get_mount_type(g_ptr_array_index(applied, i)),
            ==, CLAWT_MOUNT_BIND);

    clawt_test_remove_tree(root);
}

/*
 * A type the config named survives being added.
 *
 * The promotion fills in a blank; it must not overwrite an answer. The
 * only spelling that reaches a VM is virtiofs, so a bind mount written
 * out by hand on a VM is a mistake -- but it is the operator's mistake
 * to see, and quietly rewriting it would hide the one case where the
 * validation message is the useful output.
 */
static void
test_an_explicit_type_is_not_rewritten(void)
{
    g_autoptr(ClawtPodBridge) bridge = clawt_pod_bridge_new(NULL);
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtMount) mount = clawt_mount_new("/host/notes", "/notes");
    GPtrArray *applied;

    clawt_mount_set_mount_type(mount, CLAWT_MOUNT_VIRTIOFS);

    computer = clawt_container_computer_new("scribe", bridge,
                                            "fedora:latest");
    clawt_computer_add_mount(computer, mount);

    applied = clawt_computer_get_mounts(computer);
    g_assert_cmpuint(applied->len, ==, 1);
    g_assert_cmpint(
        clawt_mount_get_mount_type(g_ptr_array_index(applied, 0)),
        ==, CLAWT_MOUNT_VIRTIOFS);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/exchange/preparing-applies-the-cap",
                    test_preparing_applies_the_cap);
    g_test_add_func("/exchange/zero-cap-removes-nothing",
                    test_a_zero_cap_removes_nothing);
    g_test_add_func("/exchange/under-the-cap-is-untouched",
                    test_an_exchange_under_the_cap_is_untouched);
    g_test_add_func("/exchange/preparing-creates-directories",
                    test_preparing_creates_the_directories);
    g_test_add_func("/exchange/reaches-a-guests-fstab",
                    test_the_exchange_reaches_a_guests_fstab);
    g_test_add_func("/exchange/a-container-keeps-bind-mounts",
                    test_a_container_keeps_bind_mounts);
    g_test_add_func("/exchange/an-explicit-type-is-kept",
                    test_an_explicit_type_is_not_rewritten);

    return g_test_run();
}
