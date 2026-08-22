/*
 * clawt-vm-image.h - Cloud images, fetched once and kept
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A VM needs a disk image, clawtilla ships none, and the ones worth
 * having are several hundred megabytes fetched over the network.  Left to
 * the moment an agent starts, that is a first start which appears to hang
 * for ten minutes -- so images are fetched deliberately, ahead of time,
 * with something to watch.
 *
 * The store is the daemon's: one copy of an image serves every agent, and
 * an overlay is what makes them independent.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib-object.h>

G_BEGIN_DECLS

/**
 * ClawtVmImageSource:
 * @id: a short stable name, e.g. `fedora-44`
 * @name: what to call it in a list
 * @note: (nullable): a line about why you would pick it
 * @group: a heading to file it under
 * @url: the image, or the directory holding it when @pattern is set
 * @pattern: (nullable): a glob picking the image out of that directory
 *
 * A suggested cloud image.
 *
 * @pattern exists because a distribution that stamps its compose into the
 * filename -- Fedora and CentOS both do -- would otherwise need this table
 * edited every time it rebuilt one.  With it, the newest matching file in
 * the directory is used, so the entry stays right until the release
 * itself is retired.
 */
typedef struct {
    const gchar *id;
    const gchar *name;
    const gchar *note;
    const gchar *group;
    const gchar *url;
    const gchar *pattern;
} ClawtVmImageSource;

/**
 * clawt_vm_image_catalog:
 * @n_sources: (out): how many there are
 *
 * The suggestions, never a restriction: any URL can be fetched.
 *
 * Returns: (transfer none) (array length=n_sources): the table
 */
const ClawtVmImageSource *clawt_vm_image_catalog(gsize *n_sources);

/**
 * clawt_vm_image_catalog_lookup:
 * @id: an id from the catalog
 *
 * Returns: (transfer none) (nullable): the entry, or %NULL
 */
const ClawtVmImageSource *clawt_vm_image_catalog_lookup(const gchar *id);

/**
 * clawt_vm_image_pick_newest:
 * @listing: a directory listing page, as a mirror served it
 * @pattern: a glob such as `Fedora-Cloud-Base-Generic-44-*.x86_64.qcow2`
 *
 * Picks the highest-sorting filename in @listing matching @pattern.
 *
 * The listing is HTML from whichever mirror answered, so this looks for
 * the pattern's literal prefix and takes what follows up to the next
 * delimiter rather than pretending to parse the page.
 *
 * Returns: (transfer full) (nullable): the filename, or %NULL if nothing
 *   there matches
 */
gchar *clawt_vm_image_pick_newest(const gchar *listing, const gchar *pattern);

/**
 * ClawtVmImage:
 * @name: the file's name, which is also its handle
 * @path: where it is on disk
 * @url: (nullable): where it came from
 * @bytes: its size, or how much has arrived so far
 * @total: the full size when downloading and the server said, else 0
 * @downloading: whether it is still arriving
 */
typedef struct {
    gchar    *name;
    gchar    *path;
    gchar    *url;
    gint64    bytes;
    gint64    total;
    gboolean  downloading;
} ClawtVmImage;

#define CLAWT_TYPE_VM_IMAGE (clawt_vm_image_get_type())

GType         clawt_vm_image_get_type(void) G_GNUC_CONST;
ClawtVmImage *clawt_vm_image_copy(ClawtVmImage *self);
void          clawt_vm_image_free(ClawtVmImage *self);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(ClawtVmImage, clawt_vm_image_free)

#define CLAWT_TYPE_VM_IMAGE_STORE (clawt_vm_image_store_get_type())

G_DECLARE_FINAL_TYPE(ClawtVmImageStore, clawt_vm_image_store,
                     CLAWT, VM_IMAGE_STORE, GObject)

/**
 * clawt_vm_image_store_new:
 * @directory: where images are kept
 *
 * Returns: (transfer full): a new #ClawtVmImageStore
 */
ClawtVmImageStore *clawt_vm_image_store_new(const gchar *directory);

/**
 * clawt_vm_image_store_list:
 * @self: a #ClawtVmImageStore
 *
 * Every image held, and every one still arriving.
 *
 * Returns: (transfer full) (element-type ClawtVmImage): the images
 */
GPtrArray *clawt_vm_image_store_list(ClawtVmImageStore *self);

/**
 * clawt_vm_image_store_path:
 * @self: a #ClawtVmImageStore
 * @name: an image's name
 *
 * Returns: (transfer full) (nullable): the path to a complete image, or
 *   %NULL if there is no such image
 */
gchar *clawt_vm_image_store_path(ClawtVmImageStore *self, const gchar *name);

/**
 * clawt_vm_image_store_start:
 * @self: a #ClawtVmImageStore
 * @url: an `http` or `https` URL, or a catalog id
 * @name: (nullable): what to call it, or %NULL to take the name from @url
 * @error: return location for a #GError
 *
 * Begins a download and returns immediately.  Watch ::progress and
 * ::finished, which is what lets a client draw a bar rather than block.
 *
 * Returns: (transfer full) (nullable): the name it will be stored under,
 *   or %NULL on error
 */
gchar *clawt_vm_image_store_start(ClawtVmImageStore  *self,
                                  const gchar        *url,
                                  const gchar        *name,
                                  GError            **error);

/**
 * clawt_vm_image_store_cancel:
 * @self: a #ClawtVmImageStore
 * @name: an image being downloaded
 *
 * Returns: %TRUE if there was one to cancel
 */
gboolean clawt_vm_image_store_cancel(ClawtVmImageStore *self,
                                     const gchar       *name);

/**
 * clawt_vm_image_store_remove:
 * @self: a #ClawtVmImageStore
 * @name: an image's name
 * @error: return location for a #GError
 *
 * Deletes it.  An agent configured to use it keeps its own overlay, which
 * has the base written into it, so this breaks that agent's VM at its
 * next provision rather than immediately.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_vm_image_store_remove(ClawtVmImageStore  *self,
                                     const gchar        *name,
                                     GError            **error);

G_END_DECLS
