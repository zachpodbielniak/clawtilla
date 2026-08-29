/*
 * clawt-avatar.h - Reading, sniffing and writing an agent's profile picture
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
 * clawt_avatar_resolve_path:
 * @configured: (nullable): `agents.avatar`, absolute or workspace-relative
 * @workspace: the agent's own directory
 *
 * Where this agent's profile picture is, if it has one.
 *
 * @configured wins when it names a readable regular file. A relative
 * value is resolved against @workspace. When @configured is set but
 * names nothing readable, that is reported with a warning -- naming a
 * path is a deliberate choice, and a mistyped one must not look like no
 * picture was ever wanted -- and resolution falls through to the
 * auto-detected file exactly as if @configured had been unset.
 *
 * With no usable @configured, this is clawt_workspace_find_profile_picture()
 * on @workspace.
 *
 * Returns: (transfer full) (nullable): the picture's path, or %NULL if
 *   this agent has none
 */
gchar *clawt_avatar_resolve_path(const gchar *configured,
                                 const gchar *workspace);

/**
 * clawt_avatar_sniff_mime_type:
 * @data: the file's bytes
 * @length: how many
 *
 * What kind of image @data is, judged by its own leading bytes rather
 * than by any name it arrived with.
 *
 * A file extension is a claim the sender made; the bytes are what a
 * client would actually have to decode. The three magic numbers checked
 * are PNG, JPEG and WEBP -- the same three the auto-detected file names
 * can end in -- and nothing else is recognised as a profile picture.
 *
 * Returns: (transfer none) (nullable): `"image/png"`, `"image/jpeg"` or
 *   `"image/webp"`, or %NULL if @data is not one of them
 */
const gchar *clawt_avatar_sniff_mime_type(const guchar *data, gsize length);

/**
 * clawt_avatar_extension_for_mime_type:
 * @mime: a value clawt_avatar_sniff_mime_type() returned
 *
 * The file extension a sniffed type is stored under.
 *
 * `image/jpeg` is stored as `.jpg`: the auto-detect order tries `.jpg`
 * before `.jpeg`, so a freshly written picture is found by the first
 * comparison rather than the third.
 *
 * Returns: (transfer none) (nullable): `"png"`, `"jpg"` or `"webp"`, or
 *   %NULL if @mime is not one clawt_avatar_sniff_mime_type() produces
 */
const gchar *clawt_avatar_extension_for_mime_type(const gchar *mime);

/**
 * clawt_avatar_compute_etag:
 * @data: the file's bytes
 * @length: how many
 *
 * A tag that changes exactly when the bytes do.
 *
 * SHA-256 of the content, hex-encoded, so a client can hold a decoded
 * texture keyed on (agent, etag) and know to throw it away the moment
 * the picture underneath it is replaced -- without polling the file or
 * trusting a timestamp that a copy or a restore can leave unchanged.
 *
 * Returns: (transfer full): the etag
 */
gchar *clawt_avatar_compute_etag(const guchar *data, gsize length);

/**
 * clawt_avatar_read:
 * @configured: (nullable): `agents.avatar`, as clawt_avatar_resolve_path() takes it
 * @workspace: the agent's own directory
 * @max_bytes: refuse anything over this; 0 or negative means no limit
 * @out_bytes: (out) (transfer full): the file's bytes
 * @out_length: (out): how many
 * @out_mime: (out) (transfer full): the sniffed type
 * @out_etag: (out) (transfer full): clawt_avatar_compute_etag() of the bytes
 * @error: (out) (optional): return location for a #GError
 *
 * The bytes a client should be sent for this agent's face.
 *
 * Bytes, never a path: this is read on the daemon's host and answered
 * over IPC to a client that may be on a different machine entirely, so a
 * filename would work here and show nothing anywhere else.
 *
 * The size is checked from the file's stat, before anything is read into
 * memory, so an oversized file costs a stat rather than an allocation --
 * and is refused outright rather than served truncated, which would
 * surface as a corrupt image a long way from the cause.
 *
 * The type comes from the bytes, never from the file's extension: a
 * plain-text file saved as `profile-picture.png` is refused here, the
 * same as it would be arriving through clawt_avatar_write().
 *
 * Returns: %TRUE on success
 */
gboolean clawt_avatar_read(const gchar   *configured,
                           const gchar   *workspace,
                           gint64         max_bytes,
                           guchar       **out_bytes,
                           gsize         *out_length,
                           gchar        **out_mime,
                           gchar        **out_etag,
                           GError       **error);

/**
 * clawt_avatar_write:
 * @workspace: the agent's own directory
 * @data: the picture's bytes
 * @length: how many
 * @out_mime: (out) (optional) (transfer full): the sniffed type
 * @error: (out) (optional): return location for a #GError
 *
 * Stores @data as this agent's auto-detected profile picture.
 *
 * The extension is chosen from clawt_avatar_sniff_mime_type(), never from
 * anything a client claimed -- so a client cannot ask the daemon to
 * believe a payload is a PNG when it is not one. Any of the four
 * auto-detected names already present are removed first, so a picture
 * replaced with one of a different type does not leave the old file
 * behind to be found ahead of the new one on the next resolution.
 *
 * @data that clawt_avatar_sniff_mime_type() does not recognise is refused
 * outright -- nothing is written.
 *
 * Returns: %TRUE on success
 */
gboolean clawt_avatar_write(const gchar   *workspace,
                            const guchar  *data,
                            gsize          length,
                            gchar        **out_mime,
                            GError       **error);

/**
 * clawt_avatar_clear:
 * @workspace: the agent's own directory
 *
 * Removes any auto-detected profile picture from @workspace.
 *
 * Idempotent: called on an agent with no picture, this removes nothing
 * and answers %FALSE -- "there is now no picture" was already true of
 * it, which is not a failure, only nothing having changed.
 *
 * Returns: %TRUE if a file was actually removed
 */
gboolean clawt_avatar_clear(const gchar *workspace);

G_END_DECLS
