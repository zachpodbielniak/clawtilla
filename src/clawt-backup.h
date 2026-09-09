/* SPDX-License-Identifier: AGPL-3.0-or-later */
#pragma once
#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif
#include <glib.h>
G_BEGIN_DECLS
/**
 * clawt_backup_create:
 * @state_dir: quiescent daemon state directory
 * @archive: new archive filename
 * @error: return location for an error
 *
 * Holds daemon.lock throughout the snapshot. Includes fleet config.yaml; excludes agents/ID/config.yaml,
 * credentials, tokens, sessions, .git, .env, .ssh, .mcp.json and nonregular files. Archives are
 * bounded to 256 MiB; files are bounded to 64 MiB. Never overwrites an archive.
 * Returns: whether the archive was created
 */
gboolean clawt_backup_create(const gchar *state_dir, const gchar *archive, GError **error);
/**
 * clawt_backup_restore:
 * @archive: archive filename
 * @destination: (nullable): new directory, or NULL for verification only
 * @error: (out) (optional): return location for an error
 *
 * Validates the complete manifest and SHA256 checksums before creating a private
 * destination. Existing destinations are rejected. Returns a path manifest for
 * preview, or the restored manifest. An I/O failure may leave a partial destination.
 * Returns: (transfer full) (nullable): newline separated paths, or NULL on failure
 */
gchar *clawt_backup_restore(const gchar *archive, const gchar *destination, GError **error);
G_END_DECLS
