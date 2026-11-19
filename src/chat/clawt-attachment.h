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

/**
 * clawt_attachment_block_new:
 * @body: (nullable): what the operator typed
 *
 * Starts the message an operator sends with files: @body, a blank line
 * when there is one, and the CLAWT_ATTACHMENT_MARKER line that tells the
 * agent how to open what follows.
 *
 * One copy of the block format for every client.  GTK, lrg and the web
 * client each wrote it by hand, and a client that spelled any part of
 * it differently -- the marker, the indentation, the `clawt:` prefix --
 * sends a message the others draw no previews for and the gallery
 * cannot index, with nothing to say why.
 *
 * Returns: (transfer full): the body so far; add each file with
 *   clawt_attachment_block_add() and free it with g_string_free()
 */
GString *clawt_attachment_block_new(const gchar *body);

/**
 * clawt_attachment_block_add:
 * @block: a block from clawt_attachment_block_new()
 * @name: the file's name
 * @host_path: (nullable): where `attachment.put` wrote it on the host
 * @path: (nullable): where the agent sees it (the container path for a
 *   container or VM agent)
 * @id: (nullable): the daemon's own copy, from `attachment.put`
 *
 * Names one saved file in the block:
 *
 * |[
 * - name
 *   /host/path
 *   (inside your container: /exchange/agent/name)
 *   clawt:<id>
 * ]|
 *
 * The host path comes first because an agent's own read tool runs on
 * the host even when its shell is in a container; the container line
 * appears only when @path differs from @host_path; the `clawt:` line
 * is what every client previews from and what `attachment.list` finds.
 */
void clawt_attachment_block_add(GString     *block,
                                const gchar *name,
                                const gchar *host_path,
                                const gchar *path,
                                const gchar *id);

/**
 * clawt_attachment_block_add_unsaved:
 * @block: a block from clawt_attachment_block_new()
 * @name: the file's name
 *
 * Names a file `attachment.put` refused, as "- name (could not be
 * saved)".  Listed rather than dropped: an agent told nothing about a
 * file it was meant to have answers as if it had been sent nothing,
 * and the operator never learns the upload failed.
 */
void clawt_attachment_block_add_unsaved(GString *block, const gchar *name);

G_END_DECLS

#endif /* CLAWT_ATTACHMENT_H */
