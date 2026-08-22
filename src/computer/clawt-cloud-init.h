/*
 * clawt-cloud-init.h - A NoCloud seed, so a stock cloud image is reachable
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A distribution's cloud image has no user, no password and no authorized
 * key: it expects to be handed its configuration on first boot.  Without
 * that it boots to a login prompt nobody can get past, which is the one
 * failure that looks exactly like a broken VM.
 *
 * NoCloud is the datasource every cloud image supports without being
 * asked: a filesystem labelled `cidata` holding `user-data` and
 * `meta-data`, attached as a CD-ROM.  cloud-init finds it by label, so it
 * needs no kernel arguments and no cooperation from the image.
 *
 * The SSH key helpers live here rather than beside the VM because the seed
 * is the only reason clawtilla needs a key at all -- the key exists to be
 * written into `user-data`.
 */

#pragma once

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * clawt_cloud_init_build_user_data:
 * @user: the login to create in the guest
 * @authorized_key: (nullable): one OpenSSH public key line
 * @hostname: (nullable): the guest's hostname
 *
 * Renders the `#cloud-config` document.
 *
 * Naming a user without `default` in the list means the distribution's own
 * default login (`fedora`, `debian`, `ubuntu` and so on) is never created,
 * so the guest has exactly the one account clawtilla knows about.
 *
 * Returns: (transfer full): the document
 */
gchar *clawt_cloud_init_build_user_data(const gchar *user,
                                        const gchar *authorized_key,
                                        const gchar *hostname);

/**
 * clawt_cloud_init_build_meta_data:
 * @instance_id: identifies this instance to cloud-init
 * @hostname: (nullable): the guest's hostname
 *
 * cloud-init reruns its per-instance modules when @instance_id changes, so
 * a stable one keeps a reboot from re-running first-boot setup.
 *
 * Returns: (transfer full): the document
 */
gchar *clawt_cloud_init_build_meta_data(const gchar *instance_id,
                                        const gchar *hostname);

/**
 * clawt_cloud_init_find_tool:
 *
 * Finds an ISO 9660 writer.
 *
 * Returns: (nullable): `xorrisofs`, `genisoimage` or `mkisofs`, whichever
 *   is installed, or %NULL
 */
const gchar *clawt_cloud_init_find_tool(void);

/**
 * clawt_cloud_init_build_iso_argv:
 * @tool: the writer, from clawt_cloud_init_find_tool()
 * @iso_path: where to write the image
 * @source_dir: the directory holding `user-data` and `meta-data`
 *
 * Only short options are used, because they are the ones all three writers
 * spell the same way.
 *
 * Returns: (transfer full): the argv
 */
GStrv clawt_cloud_init_build_iso_argv(const gchar *tool,
                                      const gchar *iso_path,
                                      const gchar *source_dir);

/**
 * clawt_cloud_init_write_seed:
 * @dir: the VM's state directory
 * @instance_id: identifies this instance to cloud-init
 * @user: the login to create in the guest
 * @authorized_key: (nullable): one OpenSSH public key line
 * @hostname: (nullable): the guest's hostname
 * @error: return location for a #GError
 *
 * Writes the two documents and builds `seed.iso` beside them.  The build
 * is skipped when the documents have not changed, so a restart does not
 * hand cloud-init a byte-identical image with a new timestamp.
 *
 * Returns: (transfer full) (nullable): the path to the ISO, or %NULL
 */
gchar *clawt_cloud_init_write_seed(const gchar  *dir,
                                   const gchar  *instance_id,
                                   const gchar  *user,
                                   const gchar  *authorized_key,
                                   const gchar  *hostname,
                                   GError      **error);

/**
 * clawt_cloud_init_public_key:
 * @private_key_path: an OpenSSH private key
 * @error: return location for a #GError
 *
 * Reads `<path>.pub` when it is there, and otherwise recovers the public
 * key from the private one with `ssh-keygen -y`.
 *
 * Returns: (transfer full) (nullable): the public key line, or %NULL
 */
gchar *clawt_cloud_init_public_key(const gchar  *private_key_path,
                                   GError      **error);

/**
 * clawt_cloud_init_ensure_key:
 * @dir: the VM's state directory
 * @comment: the key's comment
 * @key_path: (out) (transfer full): the private key
 * @public_key: (out) (transfer full): the public key line
 * @error: return location for a #GError
 *
 * Generates an unencrypted ed25519 key in @dir if there is not one there
 * already.  Unencrypted because the daemon has to use it without anyone
 * present to type a passphrase; it is mode 0600 in a mode 0700 directory,
 * and it reaches exactly one VM.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_cloud_init_ensure_key(const gchar  *dir,
                                     const gchar  *comment,
                                     gchar       **key_path,
                                     gchar       **public_key,
                                     GError      **error);

G_END_DECLS
