/*
 * test-vm-image.c - Cloud images, fetched once and kept
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

/* A mirror's index page, near enough: the real thing is this plus links. */
static const gchar *fedora_listing =
    "<html><body><h1>Index of /pub/fedora/linux/releases/44/Cloud/x86_64/"
    "images</h1><table>"
    "<tr><td><a href=\"Fedora-Cloud-Base-Generic-44-1.5.x86_64.qcow2\">"
    "Fedora-Cloud-Base-Generic-44-1.5.x86_64.qcow2</a></td></tr>"
    "<tr><td><a href=\"Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2\">"
    "Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2</a></td></tr>"
    "<tr><td><a href=\"Fedora-Cloud-Base-Generic-44-1.7.x86_64.raw.xz\">"
    "Fedora-Cloud-Base-Generic-44-1.7.x86_64.raw.xz</a></td></tr>"
    "<tr><td><a href=\"Fedora-Cloud-44-1.7-x86_64-CHECKSUM\">"
    "Fedora-Cloud-44-1.7-x86_64-CHECKSUM</a></td></tr>"
    "</table></body></html>";

/*
 * A distribution that stamps its compose into the filename would need this
 * catalog edited every time it rebuilt one.  Resolving the newest match
 * instead is what keeps an entry right until the release is retired.
 */
static void
test_picks_the_newest_matching_image(void)
{
    g_autofree gchar *picked = clawt_vm_image_pick_newest(
        fedora_listing, "Fedora-Cloud-Base-Generic-44-*.x86_64.qcow2");

    g_assert_cmpstr(picked, ==, "Fedora-Cloud-Base-Generic-44-1.7.x86_64.qcow2");
}

/* The same compose publishes a raw.xz beside the qcow2; only one boots. */
static void
test_picks_ignores_other_formats(void)
{
    g_autofree gchar *picked = clawt_vm_image_pick_newest(
        fedora_listing, "Fedora-Cloud-Base-Generic-44-*.x86_64.qcow2");

    g_assert_null(strstr(picked, "raw.xz"));
    g_assert_null(strstr(picked, "CHECKSUM"));
}

/*
 * A release that has been retired is the expected end of every catalog
 * entry, so finding nothing must be an answer rather than a crash.
 */
static void
test_picks_nothing_when_the_release_is_gone(void)
{
    g_autofree gchar *picked = clawt_vm_image_pick_newest(
        fedora_listing, "Fedora-Cloud-Base-Generic-39-*.x86_64.qcow2");

    g_assert_null(picked);
}

static void
test_catalog_entries_are_addressable(void)
{
    const ClawtVmImageSource *fedora =
        clawt_vm_image_catalog_lookup("fedora-44");
    const ClawtVmImageSource *sources;
    gsize n_sources = 0;
    gsize i;

    g_assert_nonnull(fedora);
    g_assert_nonnull(fedora->url);

    sources = clawt_vm_image_catalog(&n_sources);
    g_assert_cmpuint(n_sources, >, 0);

    /*
     * Every entry has to carry a group and a name, because a client shows
     * them as a grouped list and an entry missing either is a blank row.
     */
    for (i = 0; i < n_sources; i++) {
        g_assert_nonnull(sources[i].id);
        g_assert_nonnull(sources[i].name);
        g_assert_nonnull(sources[i].group);
        g_assert_nonnull(sources[i].url);
        g_assert_nonnull(clawt_vm_image_catalog_lookup(sources[i].id));
    }

    g_assert_null(clawt_vm_image_catalog_lookup("no-such-image"));
}

/*
 * The name is joined onto the store's directory, so a URL ending in
 * something that would climb out of it, or collide with the bookkeeping
 * files beside an image, is refused rather than quietly corrected.
 */
static void
test_a_url_cannot_name_a_file_outside_the_store(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-images-XXXXXX", NULL);
    g_autoptr(ClawtVmImageStore) store = clawt_vm_image_store_new(dir);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *name = NULL;

    name = clawt_vm_image_store_start(store, "https://example.invalid/a/..",
                                      NULL, &error);
    g_assert_null(name);
    g_assert_nonnull(error);

    g_clear_error(&error);
    g_assert_null(clawt_vm_image_store_start(
        store, "https://example.invalid/x.qcow2", "../escape", &error));
    g_assert_nonnull(error);

    /*
     * .part and .source sit beside an image; a download allowed to take
     * one of those names would be deleted as another's leftovers.
     */
    g_clear_error(&error);
    g_assert_null(clawt_vm_image_store_start(
        store, "https://example.invalid/x.qcow2", "mine.part", &error));
    g_assert_nonnull(error);

    clawt_test_remove_tree(dir);
}

/*
 * A local file needs no downloading, and a file:// URL reaching this would
 * be a way to have the daemon copy anything it can read into the store.
 */
static void
test_only_http_urls_are_fetched(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-images-XXXXXX", NULL);
    g_autoptr(ClawtVmImageStore) store = clawt_vm_image_store_new(dir);
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_vm_image_store_start(store, "file:///etc/passwd",
                                             NULL, &error));
    g_assert_nonnull(error);

    clawt_test_remove_tree(dir);
}

static void
test_an_empty_store_holds_nothing(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-images-XXXXXX", NULL);
    g_autoptr(ClawtVmImageStore) store = clawt_vm_image_store_new(dir);
    g_autoptr(GPtrArray) images = clawt_vm_image_store_list(store);

    g_assert_cmpuint(images->len, ==, 0);
    g_assert_null(clawt_vm_image_store_path(store, "nothing.qcow2"));

    clawt_test_remove_tree(dir);
}

/* An image on disk is listed, with the URL it came from. */
static void
test_an_image_on_disk_is_listed_with_its_source(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-images-XXXXXX", NULL);
    g_autoptr(ClawtVmImageStore) store = clawt_vm_image_store_new(dir);
    g_autofree gchar *image_path = g_build_filename(dir, "disk.qcow2", NULL);
    g_autofree gchar *source_path = g_build_filename(dir, "disk.qcow2.source",
                                                     NULL);
    g_autoptr(GPtrArray) images = NULL;
    g_autofree gchar *found = NULL;
    ClawtVmImage *image;

    g_assert_true(g_file_set_contents(image_path, "not really a disk", -1,
                                      NULL));
    g_assert_true(g_file_set_contents(source_path,
                                      "https://example.invalid/disk.qcow2\n",
                                      -1, NULL));

    images = clawt_vm_image_store_list(store);
    g_assert_cmpuint(images->len, ==, 1);

    image = g_ptr_array_index(images, 0);
    g_assert_cmpstr(image->name, ==, "disk.qcow2");
    g_assert_cmpstr(image->url, ==, "https://example.invalid/disk.qcow2");
    g_assert_false(image->downloading);
    g_assert_cmpint(image->bytes, ==, 17);

    found = clawt_vm_image_store_path(store, "disk.qcow2");
    g_assert_cmpstr(found, ==, image_path);

    clawt_test_remove_tree(dir);
}

/* Removing an image that is not there says so rather than succeeding. */
static void
test_removing_what_is_not_there_is_an_error(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-images-XXXXXX", NULL);
    g_autoptr(ClawtVmImageStore) store = clawt_vm_image_store_new(dir);
    g_autoptr(GError) error = NULL;

    g_assert_false(clawt_vm_image_store_remove(store, "absent.qcow2", &error));
    g_assert_nonnull(error);

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/vm-image/pick/newest",
                    test_picks_the_newest_matching_image);
    g_test_add_func("/vm-image/pick/other-formats",
                    test_picks_ignores_other_formats);
    g_test_add_func("/vm-image/pick/release-retired",
                    test_picks_nothing_when_the_release_is_gone);
    g_test_add_func("/vm-image/catalog/addressable",
                    test_catalog_entries_are_addressable);
    g_test_add_func("/vm-image/store/no-escaping",
                    test_a_url_cannot_name_a_file_outside_the_store);
    g_test_add_func("/vm-image/store/http-only",
                    test_only_http_urls_are_fetched);
    g_test_add_func("/vm-image/store/empty",
                    test_an_empty_store_holds_nothing);
    g_test_add_func("/vm-image/store/lists-with-source",
                    test_an_image_on_disk_is_listed_with_its_source);
    g_test_add_func("/vm-image/store/remove-absent",
                    test_removing_what_is_not_there_is_an_error);

    return g_test_run();
}
