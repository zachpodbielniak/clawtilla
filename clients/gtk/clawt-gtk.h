/*
 * clawt-gtk.h - Shared declarations for the GTK client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The widgets are built in C rather than from .ui files.  Two reasons:
 * everything here is driven by data that arrives over the socket, so the
 * interesting part is the binding rather than the layout; and a compiler
 * catches a mistake in this file, where a typo in a .ui file becomes a
 * warning at runtime that a person has to be looking to notice.
 */

#pragma once

#include <adwaita.h>
#include <clawtilla.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define CLAWT_TYPE_WINDOW (clawt_window_get_type())

G_DECLARE_FINAL_TYPE(ClawtWindow, clawt_window, CLAWT, WINDOW,
                     AdwApplicationWindow)

/**
 * clawt_window_new:
 * @app: the application
 * @client: (transfer none): the daemon connection
 * @connection: (nullable): the profile @client was built from
 *
 * @connection is what the window shows in its header bar and switches
 * away from.  It is passed in rather than derived, because a #ClawtClient
 * knows a host and a port and not the name a person gave that machine --
 * and two answers to "which daemon is this" is exactly how a window ends
 * up acting on one and labelled with another.
 *
 * Returns: (transfer none): the window
 */
ClawtWindow *clawt_window_new(AdwApplication  *app,
                              ClawtClient     *client,
                              ClawtConnection *connection);

/**
 * clawt_window_toast:
 * @self: a #ClawtWindow
 * @text: what to say
 *
 * Shows a transient message.
 *
 * Used for anything the daemon refused.  A dialog for every refusal would
 * mean dismissing one before reading the next; a toast lets the work
 * carry on.
 */
void clawt_window_toast(ClawtWindow *self, const gchar *text);

/**
 * clawt_window_request:
 * @self: a #ClawtWindow
 * @kind: the request kind
 * @payload: (transfer full) (nullable): the request body
 *
 * Sends a request and shows any failure as a toast.
 *
 * Returns: (transfer full) (nullable): the reply payload, or %NULL
 */
JsonNode *clawt_window_request(ClawtWindow *self, const gchar *kind,
                               JsonNode *payload);

/**
 * clawt_json_string:
 * @object: (nullable): a JSON object
 * @key: the member to read
 * @fallback: (nullable): what to return if it is absent or not a string
 *
 * Returns: (transfer none) (nullable): the value, or @fallback
 */
const gchar *clawt_json_string(JsonObject   *object,
                               const gchar  *key,
                               const gchar  *fallback);

/**
 * clawt_json_int:
 * @object: (nullable): a JSON object
 * @key: the member to read
 * @fallback: what to return if it is absent or not a number
 *
 * The integer twin of clawt_json_string(), for the same reason: a
 * client that calls json_object_get_int_member() on a member an older
 * daemon does not send aborts, and a missing field is an ordinary thing
 * to meet across a version boundary.
 *
 * Returns: the value, or @fallback
 */
gint64 clawt_json_int(JsonObject  *object,
                      const gchar *key,
                      gint64       fallback);

/**
 * clawt_json_boolean:
 * @object: (nullable): a JSON object
 * @key: the member to read
 * @fallback: what to return if it is absent or not a boolean
 *
 * The third of the set, and guarded for the same reason: a daemon older
 * than this client does not send the member at all, and reading it
 * unguarded aborts rather than returning anything.
 *
 * Returns: the value, or @fallback
 */
gboolean clawt_json_boolean(JsonObject  *object,
                            const gchar *key,
                            gboolean     fallback);

/**
 * clawt_payload_of:
 * @reply: a reply frame
 *
 * Returns: (transfer none) (nullable): the payload object
 */
JsonObject *clawt_payload_of(JsonNode *reply);

/**
 * clawt_build_payload:
 * @first_key: first member name, then its value, then more pairs, then %NULL
 *
 * Builds a flat string payload.
 *
 * Returns: (transfer full): the payload
 */
JsonNode *clawt_build_payload(const gchar *first_key, ...) G_GNUC_NULL_TERMINATED;

/**
 * CLAWT_AVATAR_DECODE_SIZE:
 *
 * The pixel size a profile picture is decoded at, once, regardless of
 * which of the three faces (sidebar, transcript, inspector) asks for it.
 *
 * A single size larger than any of the three slots means the texture
 * clawt_gtk_avatar_texture() caches is decoded once per agent rather
 * than once per slot, and GTK scaling a 128px texture down to a 32px
 * avatar costs nothing next to decoding a multi-megapixel JPEG into
 * that same slot -- the trap CLAUDE.md already records for a
 * `GtkPicture` with no maximum size, and the one the previous
 * `gdk_texture_new_from_filename()` call fell into at full resolution.
 */
#define CLAWT_AVATAR_DECODE_SIZE 128

/**
 * CLAWT_AVATAR_PREVIEW_SIZE:
 *
 * The pixel size a profile picture is decoded at when somebody clicks
 * one to look at it properly.
 *
 * A separate number from %CLAWT_AVATAR_DECODE_SIZE, and deliberately
 * not just a larger value for both: the cached texture is drawn at 24
 * to 48 pixels several dozen times a redraw, and decoding every agent's
 * picture at preview size to serve a face nobody has clicked would pay
 * the whole cost on the common path for the rare one.
 *
 * Bounded rather than "full resolution" for the reason the constant
 * above records -- #GtkPicture takes its natural size from its
 * paintable and GTK has no maximum size -- but far enough above any
 * plausible preview window that the picture is not what runs out of
 * detail first, including on a HiDPI screen.
 */
#define CLAWT_AVATAR_PREVIEW_SIZE 1024

/**
 * CLAWT_SCREEN_DECODE_WIDTH:
 *
 * The width a screen frame is decoded at before it becomes a texture.
 *
 * The same trap as %CLAWT_AVATAR_DECODE_SIZE and a worse one: GTK has no
 * maximum size, a size request is a floor, and #GtkPicture takes its
 * natural size from its paintable -- so a full-resolution decode into a
 * preview panel costs the memory of the whole framebuffer. An avatar
 * pays that once per agent; a frame arrives once a second.
 *
 * Wider than any panel it is drawn in on purpose, so the picture stays
 * legible when somebody widens the window, and a good deal narrower than
 * a 4K guest.
 */
#define CLAWT_SCREEN_DECODE_WIDTH 1280

/**
 * CLAWT_GTK_WATCHER_NAME:
 *
 * What this client calls itself when it subscribes to a screen.
 *
 * A watch is keyed by name so that asking twice does not count twice,
 * and so that letting go stops this client's watch rather than
 * somebody else's -- a browser looking at the same agent keeps its own.
 * The name is shown to nobody; it is the key.
 */
#define CLAWT_GTK_WATCHER_NAME "clawtilla-gtk"

/**
 * clawt_gtk_avatar_texture:
 * @client: the daemon connection
 * @agent_id: the agent to fetch a picture for
 *
 * This agent's profile picture, decoded once and cached by id.
 *
 * Fetches `agent.avatar` and decodes the reply through
 * gdk_pixbuf_new_from_stream_at_scale() to %CLAWT_AVATAR_DECODE_SIZE,
 * then gdk_memory_texture_new() -- never gdk_texture_new_from_filename(),
 * which only ever worked when the client and the daemon shared a
 * filesystem.
 *
 * A miss (no picture, a refused request, bytes that will not decode) is
 * cached too, as "nothing to draw", so a face-less agent costs one
 * request per session rather than one per redraw. Call
 * clawt_gtk_avatar_invalidate() when the agent's own `agent.changed`
 * arrives, since that is the only signal that the cached answer might
 * now be wrong.
 *
 * Returns: (transfer full) (nullable): a texture, or %NULL if this
 *   agent has no picture to show
 */
GdkTexture *clawt_gtk_avatar_texture(ClawtClient *client,
                                     const gchar *agent_id);

/**
 * clawt_gtk_avatar_invalidate:
 * @agent_id: (nullable): the agent whose cached picture is stale, or
 *   %NULL to drop every entry
 *
 * Forgets a cached answer from clawt_gtk_avatar_texture(), so the next
 * call fetches again rather than repeating a texture -- or a "no
 * picture" -- that `agent.avatar_set` or `agent.avatar_clear` has since
 * made wrong.
 */
void clawt_gtk_avatar_invalidate(const gchar *agent_id);

/**
 * clawt_gtk_avatar_preview_texture:
 * @client: the daemon connection
 * @agent_id: the agent to fetch a picture for
 *
 * The same picture at %CLAWT_AVATAR_PREVIEW_SIZE, for the window a
 * click on a face opens.
 *
 * Deliberately *not* cached.  Two reasons, and the second is the one
 * that matters: a preview-sized texture per agent would sit in memory
 * for the life of the client to serve a window somebody opened once,
 * and a click is the one moment where a fresh fetch is both affordable
 * and correct -- the cached row texture can be a picture the agent has
 * since replaced, and nothing invalidates it until `agent.changed`
 * arrives.
 *
 * Returns: (transfer full) (nullable): a texture, or %NULL if this
 *   agent has no picture to show
 */
GdkTexture *clawt_gtk_avatar_preview_texture(ClawtClient *client,
                                             const gchar *agent_id);

/**
 * clawt_gtk_build_avatar:
 * @client: the daemon connection
 * @name: the sender's name, for the derived initials and as the
 *   accessible label
 * @agent_id: (nullable): whose picture to look up, or %NULL to derive a
 *   face from @name alone (the Flow tab, which draws several senders)
 * @has_avatar: whether `agent.avatar` has anything for @agent_id -- from
 *   `agent.list`/`agent.show`'s own field, so this never has to ask just
 *   to find out there was nothing to ask for
 * @color: (nullable): `agents.color`, checked here through
 *   clawt_color_ink() before it reaches a stylesheet
 * @size: the avatar's diameter, in pixels
 *
 * One face, drawn the same way in the sidebar, the transcript and the
 * inspector -- the two row builders this codebase already had to
 * delete into one apply here too: three copies of this decision would
 * drift the moment one of them learned about pictures and the others
 * did not, which is exactly the bug this replaces.
 *
 * Resolution order: @agent_id's picture when @has_avatar says there is
 * one and it decodes, then @color through clawt_color_ink(), then the
 * initials and colour #AdwAvatar derives from @name on its own.
 *
 * Returns: (transfer full): a new, unparented #AdwAvatar
 */
GtkWidget *clawt_gtk_build_avatar(ClawtClient  *client,
                                  const gchar  *name,
                                  const gchar  *agent_id,
                                  gboolean      has_avatar,
                                  const gchar  *color,
                                  gint          size);

G_END_DECLS
