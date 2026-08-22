/*
 * clawt-image-catalog.h - Container images a client can offer
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * ClawtImageInfo:
 * @reference: the full image reference, registry included
 * @label: what to show in a list
 * @note: (nullable): one line on when to pick it
 * @group: which heading it belongs under
 *
 * One image a client can offer.
 */
typedef struct {
    const gchar *reference;
    const gchar *label;
    const gchar *note;
    const gchar *group;
} ClawtImageInfo;

/**
 * clawt_image_catalog_get:
 * @n_images: (out): how many
 *
 * The images clawtilla suggests, best first.
 *
 * A starting point, never a restriction: any reference podman can pull
 * is valid, and a client must offer a way to type one that is not here.
 * Fedora leads because that is what clawtilla is developed and tested
 * on, and because a container whose package manager the user already
 * knows is one they can debug.
 *
 * Every entry names its registry explicitly. A bare "fedora:44" resolves
 * through podman's search list, which differs per machine, so the same
 * config can pull different images on two hosts.
 *
 * Returns: (transfer none) (array length=n_images): the catalogue
 */
const ClawtImageInfo *clawt_image_catalog_get(gsize *n_images);

/**
 * clawt_image_catalog_default:
 *
 * The image an agent gets when its config does not name one.
 *
 * Returns: (transfer none): a full image reference
 */
const gchar *clawt_image_catalog_default(void);

G_END_DECLS
