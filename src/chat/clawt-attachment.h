/*
 * clawt-attachment.h - Files an agent sends to its operator
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#ifndef CLAWT_ATTACHMENT_H
#define CLAWT_ATTACHMENT_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_ATTACHMENT_MARKER:
 *
 * The line that introduces an attachment block in a message body.
 *
 * A transcript is rebuilt from what the daemon stored, so the only way
 * back to "this message had a picture on it" is to recognise the line
 * that was written.  Both clients match on this prefix, and it lives
 * here rather than in either of them because a client that spelled it
 * differently would draw no previews and say nothing about why.
 */
#define CLAWT_ATTACHMENT_MARKER "[clawtilla] Files sent with this message"

/**
 * clawt_attachment_store:
 * @dir: the directory to keep attachments in
 * @path: a file to copy in
 * @error: (out) (optional): return location for a #GError
 *
 * Copies a file an agent named into the daemon's own keeping.
 *
 * The bytes are taken **at send time** rather than the path being passed
 * through.  A path only works when the client and the file are on the
 * same machine, and the failure when they are not looks like a broken
 * image rather than an unsupported configuration -- so a remote client
 * would silently show nothing.  Copying also means the agent may delete
 * or rewrite its own copy afterwards without changing what was sent.
 *
 * Returns: (transfer full) (nullable): an id naming the stored copy, or
 *   %NULL on failure
 */
gchar *clawt_attachment_store(const gchar  *dir,
                              const gchar  *path,
                              GError      **error);

/**
 * clawt_attachment_path:
 * @dir: the directory attachments are kept in
 * @id: an id from clawt_attachment_store()
 *
 * Where the stored copy is.
 *
 * @id comes off the wire, so it is checked rather than trusted: anything
 * but the characters an id is made of is refused, which is what stops a
 * request for `../../secrets/token` reading a file this was never meant
 * to serve.
 *
 * Returns: (transfer full) (nullable): the path, or %NULL if @id is not
 *   one of ours
 */
gchar *clawt_attachment_path(const gchar *dir, const gchar *id);

/**
 * clawt_attachment_name:
 * @id: an id from clawt_attachment_store()
 *
 * The original filename, which is carried in the id after the first "-".
 *
 * Returns: (transfer full) (nullable): the name, or %NULL if @id is not
 *   one of ours
 */
gchar *clawt_attachment_name(const gchar *id);

G_END_DECLS

#endif /* CLAWT_ATTACHMENT_H */
