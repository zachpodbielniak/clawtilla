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

/* ── Reading the block back, and a conversation's files ──────────── */

/**
 * ClawtAttachmentRef:
 * @daemon_copy: %TRUE for `clawt:<id>` (a copy the daemon keeps, fetched
 *   with `attachment.get`), %FALSE for a host path
 * @value: the id or the path
 * @name: (nullable): the item's name, from its "- name" line (or the
 *   basename of an old "- /path" item)
 *
 * One file an attachment block names.
 */
typedef struct {
    gboolean  daemon_copy;
    gchar    *value;
    gchar    *name;
} ClawtAttachmentRef;

/**
 * clawt_attachment_ref_free:
 * @ref: (nullable): a #ClawtAttachmentRef
 */
void clawt_attachment_ref_free(ClawtAttachmentRef *ref);

/**
 * clawt_attachment_refs_parse:
 * @body: (nullable): a message body
 *
 * The files a message's attachment block names, in order, one per item.
 *
 * The block is the list under #CLAWT_ATTACHMENT_MARKER: its "- " items
 * and their indented continuation lines.  Anything at the left margin
 * ends it, so a path an agent quotes further down does not count.  The
 * container path (in brackets) is skipped.  An operator's file is named
 * twice -- host path for the agent, `clawt:<id>` for the clients -- and
 * the id wins, because the path opens only on the daemon's machine.  An
 * older message with only a path (or a "- /path" item) gives the path.
 *
 * The one reader of the format: the libregnum previews, the web
 * client's previews and `attachment.list` all call it, so a block that
 * draws a preview is a block the gallery lists.
 *
 * Returns: (transfer full) (element-type ClawtAttachmentRef): the refs,
 *   possibly empty
 */
GPtrArray *clawt_attachment_refs_parse(const gchar *body);

/**
 * clawt_attachment_prose:
 * @body: (nullable): a message body
 *
 * @body with its attachment block taken out: the marker line and the
 * block's items, ended where clawt_attachment_refs_parse() ends it, so
 * a line written after the files is kept.
 *
 * Returns: (transfer full): the prose, trailing whitespace removed
 */
gchar *clawt_attachment_prose(const gchar *body);

/**
 * clawt_attachment_kind:
 * @name: (nullable): a file name
 *
 * What a file is, from its extension: "image", "pdf", "text" or
 * "other".  By name and not by content, because the clients choose how
 * to draw a tile before fetching any bytes; the daemon renders a
 * thumbnail only for "pdf" and "text", and a file that is not what its
 * name says simply fails to render.
 *
 * Returns: (transfer none): the kind; never %NULL
 */
const gchar *clawt_attachment_kind(const gchar *name);

/**
 * ClawtAttachmentItem:
 * @id: (nullable): the daemon's copy, when the block named one
 * @name: the file's name
 * @path: (nullable): the host path, when the block named no id
 * @kind: from clawt_attachment_kind()
 * @bytes: its size, or 0 when it is gone
 * @exists: whether the file is still there
 * @message: the id of the message that carried it
 * @sender: who sent that message
 * @ts: when, in seconds since the epoch
 *
 * One file in a conversation, as `attachment.list` answers it.
 */
typedef struct {
    gchar       *id;
    gchar       *name;
    gchar       *path;
    const gchar *kind;
    gint64       bytes;
    gboolean     exists;
    gchar       *message;
    gchar       *sender;
    gint64       ts;
} ClawtAttachmentItem;

/**
 * clawt_attachment_item_free:
 * @item: (nullable): a #ClawtAttachmentItem
 */
void clawt_attachment_item_free(ClawtAttachmentItem *item);

/**
 * clawt_attachment_list:
 * @dir: (nullable): the directory attachments are kept in
 * @messages: (element-type ClawtMessage): a room's history, oldest first
 *   (as clawt_room_get_history() returns it)
 *
 * Every file the conversation's messages carried, newest message first
 * and in block order within a message.
 *
 * Built from the transcript rather than from the attachment directory:
 * that directory is shared by every conversation and keeps no record of
 * which one a file came from, while the block in each message says
 * exactly that.  Whether each file still exists is checked as it is
 * listed, so a file deleted since it was sent is listed as gone rather
 * than offered to open.
 *
 * Returns: (transfer full) (element-type ClawtAttachmentItem): the items
 */
GPtrArray *clawt_attachment_list(const gchar *dir, GPtrArray *messages);

/**
 * CLAWT_ATTACHMENT_THUMB_MIN:
 *
 * The smallest thumbnail edge attachment.thumbnail draws, in pixels.
 */
#define CLAWT_ATTACHMENT_THUMB_MIN (16)

/**
 * CLAWT_ATTACHMENT_THUMB_MAX:
 *
 * The largest thumbnail edge attachment.thumbnail draws, in pixels.
 * The request comes off the wire; without a ceiling one frame could ask
 * the daemon for a gigapixel surface.
 */
#define CLAWT_ATTACHMENT_THUMB_MAX (512)

/**
 * CLAWT_ATTACHMENT_THUMB_DEFAULT:
 *
 * The edge used when a request names none.
 */
#define CLAWT_ATTACHMENT_THUMB_DEFAULT (256)

/**
 * clawt_attachment_thumbnail_size:
 * @requested: the size a request asked for, or 0 or less for the default
 *
 * Returns: @requested clamped to #CLAWT_ATTACHMENT_THUMB_MIN ..
 *   #CLAWT_ATTACHMENT_THUMB_MAX
 */
guint clawt_attachment_thumbnail_size(gint64 requested);

/**
 * clawt_attachment_can_render_pdf:
 *
 * Whether this build was made with poppler-glib (`CLAWT_HAVE_POPPLER`).
 *
 * Returns: %TRUE when PDF thumbnails can be drawn
 */
gboolean clawt_attachment_can_render_pdf(void);

/**
 * clawt_attachment_thumbnail:
 * @dir: the directory attachments are kept in
 * @id: an id from clawt_attachment_store() naming a PDF
 * @size: the longest edge wanted; clamped by
 *   clawt_attachment_thumbnail_size()
 * @width: (out) (optional): the PNG's width
 * @height: (out) (optional): the PNG's height
 * @error: (out) (optional): return location for a #GError
 *
 * The first page of a stored PDF as a PNG whose longer edge is @size.
 *
 * Only page 1 is ever drawn, and only into a surface no larger than
 * #CLAWT_ATTACHMENT_THUMB_MAX square: the file is untrusted input parsed
 * inside the daemon, so what it can cost is bounded.  The PNG is cached
 * as `<dir>/.thumbs/<id>.<size>.png` and served from there afterwards;
 * an id names immutable bytes, so the cache never goes stale.  An id
 * that is not one of ours is refused before anything is opened.
 *
 * Returns: (transfer full) (nullable): the PNG, or %NULL with @error set
 *   (%CLAWT_ERROR_NOT_SUPPORTED in a build without poppler)
 */
GBytes *clawt_attachment_thumbnail(const gchar  *dir,
                                   const gchar  *id,
                                   guint         size,
                                   guint        *width,
                                   guint        *height,
                                   GError      **error);

/**
 * clawt_attachment_text_head:
 * @dir: the directory attachments are kept in
 * @id: an id from clawt_attachment_store()
 * @lines: how many lines to return at most
 * @error: (out) (optional): return location for a #GError
 *
 * The first @lines lines of a stored text file, for a gallery tile.
 * At most 8 KiB is read and each line is cut at 160 characters; the
 * result is valid UTF-8 whatever the file held.
 *
 * Returns: (transfer full) (nullable): the text, or %NULL with @error
 *   set
 */
gchar *clawt_attachment_text_head(const gchar  *dir,
                                  const gchar  *id,
                                  guint         lines,
                                  GError      **error);

G_END_DECLS

#endif /* CLAWT_ATTACHMENT_H */
