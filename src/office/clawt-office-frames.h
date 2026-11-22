/*
 * clawt-office-frames.h - The host side of libregnum's frame protocol
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawtilla-office draws the office on libregnum, and nothing that links
 * GTK may link libregnum.  So a client embeds it as a child: the child
 * renders offscreen and streams frames over a unix socket ("LFRM v1",
 * libregnum's docs/modules/remote/frame-protocol.org), and the client
 * shows them and sends input back.  This is that protocol's host side,
 * written from the specification rather than linked, with no toolkit in
 * it, so the GTK client and the tests drive the same code.
 */

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#ifndef CLAWT_OFFICE_FRAMES_H
#define CLAWT_OFFICE_FRAMES_H

#include <glib-object.h>

#include "ipc/clawt-connection.h"

G_BEGIN_DECLS

/**
 * CLAWT_OFFICE_FRAMES_MAGIC:
 *
 * The first four bytes of every message: "LFRM", 0x4D52464C read as a
 * little-endian u32.
 */
#define CLAWT_OFFICE_FRAMES_MAGIC ((guint32)0x4D52464Cu)

/**
 * CLAWT_OFFICE_FRAMES_VERSION:
 *
 * The protocol version spoken here.  A message with another version
 * breaks the connection.
 */
#define CLAWT_OFFICE_FRAMES_VERSION (1)

/**
 * CLAWT_OFFICE_FRAMES_HEADER_SIZE:
 *
 * Bytes in a message header: u32 magic, u16 version, u16 type, u32 body
 * size, u32 reserved, all little-endian.
 */
#define CLAWT_OFFICE_FRAMES_HEADER_SIZE (16)

/**
 * CLAWT_OFFICE_FRAMES_FRAME_BODY_SIZE:
 *
 * Bytes in the fixed part of a frame message's body.
 */
#define CLAWT_OFFICE_FRAMES_FRAME_BODY_SIZE (48)

/**
 * CLAWT_OFFICE_FRAMES_MAX_BODY:
 *
 * The largest body the protocol allows (256 MiB).  A header announcing
 * more breaks the connection before a byte of the body is read.
 */
#define CLAWT_OFFICE_FRAMES_MAX_BODY ((guint32)256u * 1024u * 1024u)

/**
 * CLAWT_OFFICE_FRAMES_MAX_DIMENSION:
 *
 * The largest frame width or height accepted.
 */
#define CLAWT_OFFICE_FRAMES_MAX_DIMENSION (16384)

/**
 * CLAWT_OFFICE_FRAMES_FORMAT_RGBA8:
 *
 * The one pixel format of version 1: R, G, B, A bytes, straight alpha,
 * rows top to bottom.
 */
#define CLAWT_OFFICE_FRAMES_FORMAT_RGBA8 (1)

/**
 * CLAWT_OFFICE_FRAMES_FLAG_PIXELS_IN_FD:
 *
 * Frame flag: the pixels are in a sealed memfd passed with SCM_RIGHTS on
 * the message's first byte, not in the body.
 */
#define CLAWT_OFFICE_FRAMES_FLAG_PIXELS_IN_FD ((guint32)1u << 0)

/**
 * CLAWT_OFFICE_FRAMES_CAPABILITY_MEMFD:
 *
 * Hello capability bit: the application may send frames in memfds.
 */
#define CLAWT_OFFICE_FRAMES_CAPABILITY_MEMFD ((guint32)1u << 0)

/**
 * ClawtOfficeFramesMessage:
 * @CLAWT_OFFICE_FRAMES_MSG_HELLO: application to host, first: capabilities and size
 * @CLAWT_OFFICE_FRAMES_MSG_FRAME: application to host: one frame
 * @CLAWT_OFFICE_FRAMES_MSG_POINTER_MOVE: host to application: f32 x, f32 y
 * @CLAWT_OFFICE_FRAMES_MSG_POINTER_BUTTON: host to application: u32 button,
 *   u32 pressed, f32 x, f32 y
 * @CLAWT_OFFICE_FRAMES_MSG_SCROLL: host to application: f32 dx, f32 dy
 * @CLAWT_OFFICE_FRAMES_MSG_KEY: host to application: u32 key, u32 pressed,
 *   u32 modifiers, u32 code point
 * @CLAWT_OFFICE_FRAMES_MSG_RESIZE: host to application: u32 width, u32 height
 *
 * The message types of version 1.  Any other type is skipped whole.
 */
typedef enum {
    CLAWT_OFFICE_FRAMES_MSG_HELLO          = 1,
    CLAWT_OFFICE_FRAMES_MSG_FRAME          = 2,
    CLAWT_OFFICE_FRAMES_MSG_POINTER_MOVE   = 16,
    CLAWT_OFFICE_FRAMES_MSG_POINTER_BUTTON = 17,
    CLAWT_OFFICE_FRAMES_MSG_SCROLL         = 18,
    CLAWT_OFFICE_FRAMES_MSG_KEY            = 19,
    CLAWT_OFFICE_FRAMES_MSG_RESIZE         = 20
} ClawtOfficeFramesMessage;

#define CLAWT_TYPE_OFFICE_FRAMES (clawt_office_frames_get_type())
G_DECLARE_FINAL_TYPE(ClawtOfficeFrames, clawt_office_frames, CLAWT, OFFICE_FRAMES, GObject)

/**
 * clawt_office_frames_new:
 * @fd: a connected AF_UNIX SOCK_STREAM socket to the drawing application
 * @take_fd: whether the receiver closes @fd when it is finalized
 *
 * A receiver for the frames an embedded renderer sends.  Every read and
 * write is non-blocking whatever @fd's own mode, so nothing here ever
 * waits on the application.  Nothing is read until
 * clawt_office_frames_dispatch() is called -- typically from a source
 * watching clawt_office_frames_get_fd() for input.
 *
 * Returns: (transfer full): a new #ClawtOfficeFrames
 */
ClawtOfficeFrames *clawt_office_frames_new(gint fd, gboolean take_fd);

/**
 * clawt_office_frames_get_fd:
 * @self: a #ClawtOfficeFrames
 *
 * The socket, for a caller to watch.
 *
 * Returns: the descriptor given to clawt_office_frames_new()
 */
gint clawt_office_frames_get_fd(ClawtOfficeFrames *self);

/**
 * clawt_office_frames_dispatch:
 * @self: a #ClawtOfficeFrames
 * @error: return location for a #GError
 *
 * Reads what the socket has, up to a fixed amount per call so a fast
 * sender cannot hold the caller's loop, and handles each whole message:
 * a hello is remembered, a frame becomes the latest frame, any other
 * type is skipped.  A message is never accumulated past its own size,
 * and a header announcing more than %CLAWT_OFFICE_FRAMES_MAX_BODY is
 * refused before its body is read.  Also flushes queued input.
 *
 * Once this has failed the connection is broken for good: a stream
 * cannot be resynchronised.  Errors are %CLAWT_ERROR_PROTOCOL for
 * something that is not this protocol (bad magic or version, an
 * oversized body, a frame that does not add up, a memfd frame without
 * its descriptor), %CLAWT_ERROR_NOT_CONNECTED when the application
 * closed the socket, and %CLAWT_ERROR_FAILED for a failed system call.
 *
 * Returns: %TRUE while the connection is good
 */
gboolean clawt_office_frames_dispatch(ClawtOfficeFrames *self, GError **error);

/**
 * clawt_office_frames_get_hello:
 * @self: a #ClawtOfficeFrames
 * @capabilities: (out) (optional): the hello's capability bits
 * @width: (out) (optional): the size the application started at
 * @height: (out) (optional): likewise
 *
 * What the application said first.
 *
 * Returns: %TRUE once a hello has arrived
 */
gboolean clawt_office_frames_get_hello(ClawtOfficeFrames *self,
                                       guint32           *capabilities,
                                       guint             *width,
                                       guint             *height);

/**
 * clawt_office_frames_take_frame:
 * @self: a #ClawtOfficeFrames
 * @width: (out) (optional): the frame's width in pixels
 * @height: (out) (optional): its height
 * @stride: (out) (optional): bytes from one row to the next
 * @frame_number: (out) (optional): the application's frame counter, which
 *   counts dropped frames too, so gaps show drops
 *
 * The newest frame that arrived since the last call: RGBA8, straight
 * alpha, rows top to bottom, @stride bytes apart.  Frames that arrived
 * in between are dropped -- a late frame is worth less than none.  A
 * frame that came in a memfd is the read-only mapping itself, unmapped
 * when the last reference goes, so it costs no copy.
 *
 * Returns: (transfer full) (nullable): the pixels, or %NULL when no new
 *   frame has arrived
 */
GBytes *clawt_office_frames_take_frame(ClawtOfficeFrames *self,
                                       guint             *width,
                                       guint             *height,
                                       guint             *stride,
                                       guint64           *frame_number);

/**
 * clawt_office_frames_get_frames_received:
 * @self: a #ClawtOfficeFrames
 *
 * How many frames have been received, taken or not.
 *
 * Returns: the count
 */
guint64 clawt_office_frames_get_frames_received(ClawtOfficeFrames *self);

/**
 * clawt_office_frames_has_pending_output:
 * @self: a #ClawtOfficeFrames
 *
 * Whether input is queued because the socket would not take it all; a
 * caller watching the socket should then also watch for it becoming
 * writable and call clawt_office_frames_dispatch() or
 * clawt_office_frames_flush().
 *
 * Returns: %TRUE when bytes are queued
 */
gboolean clawt_office_frames_has_pending_output(ClawtOfficeFrames *self);

/**
 * clawt_office_frames_flush:
 * @self: a #ClawtOfficeFrames
 * @error: return location for a #GError
 *
 * Writes as much queued input as the socket takes now.
 *
 * Returns: %TRUE unless the connection failed
 */
gboolean clawt_office_frames_flush(ClawtOfficeFrames *self, GError **error);

/**
 * clawt_office_frames_send_pointer_move:
 * @self: a #ClawtOfficeFrames
 * @x: pointer position in frame pixels, origin top-left
 * @y: likewise
 * @error: return location for a #GError
 *
 * Sends POINTER_MOVE.  Input is never dropped: what the socket does not
 * take now is queued (up to a fixed bound, past which the connection is
 * declared failed rather than growing without end).
 *
 * Returns: %TRUE unless the connection failed
 */
gboolean clawt_office_frames_send_pointer_move(ClawtOfficeFrames *self,
                                               gfloat             x,
                                               gfloat             y,
                                               GError           **error);

/**
 * clawt_office_frames_send_button:
 * @self: a #ClawtOfficeFrames
 * @button: 0 left, 1 right, 2 middle (raylib's numbering, not GDK's)
 * @pressed: %TRUE for a press, %FALSE for a release
 * @x: pointer position in frame pixels
 * @y: likewise
 * @error: return location for a #GError
 *
 * Sends POINTER_BUTTON.
 *
 * Returns: %TRUE unless the connection failed
 */
gboolean clawt_office_frames_send_button(ClawtOfficeFrames *self,
                                         guint              button,
                                         gboolean           pressed,
                                         gfloat             x,
                                         gfloat             y,
                                         GError           **error);

/**
 * clawt_office_frames_send_scroll:
 * @self: a #ClawtOfficeFrames
 * @dx: horizontal wheel steps
 * @dy: vertical wheel steps, positive away from the user (GDK's sign is
 *   the opposite)
 * @error: return location for a #GError
 *
 * Sends SCROLL.
 *
 * Returns: %TRUE unless the connection failed
 */
gboolean clawt_office_frames_send_scroll(ClawtOfficeFrames *self,
                                         gfloat             dx,
                                         gfloat             dy,
                                         GError           **error);

/**
 * clawt_office_frames_send_key:
 * @self: a #ClawtOfficeFrames
 * @key: a raylib key code (GrlKey), 0 for text only
 * @pressed: %TRUE for a press
 * @modifiers: shift 1, control 2, alt 4, super 8
 * @codepoint: the character typed, 0 for none
 * @error: return location for a #GError
 *
 * Sends KEY.
 *
 * Returns: %TRUE unless the connection failed
 */
gboolean clawt_office_frames_send_key(ClawtOfficeFrames *self,
                                      guint              key,
                                      gboolean           pressed,
                                      guint              modifiers,
                                      gunichar           codepoint,
                                      GError           **error);

/**
 * clawt_office_frames_send_resize:
 * @self: a #ClawtOfficeFrames
 * @width: the width frames should come at
 * @height: the height
 * @error: return location for a #GError
 *
 * Sends RESIZE.  The application ignores 0 and anything past
 * %CLAWT_OFFICE_FRAMES_MAX_DIMENSION, so those are refused here with
 * %CLAWT_ERROR_INVALID_ARGUMENT rather than sent to be ignored.
 *
 * Returns: %TRUE unless the size is unusable or the connection failed
 */
gboolean clawt_office_frames_send_resize(ClawtOfficeFrames *self,
                                         guint              width,
                                         guint              height,
                                         GError           **error);

/**
 * clawt_office_renderer_resolve:
 * @error: return location for a #GError
 *
 * Where clawtilla-office is: beside the running binary, then the install
 * location, then PATH.  The error names all three, and how to build it
 * (`make OFFICE=1 LRG_BACKEND=1`), because a renderer somebody built
 * into another tree is otherwise found out by nothing being drawn.
 *
 * Returns: (transfer full) (nullable): the path, or %NULL with @error set
 *   to %CLAWT_ERROR_NOT_FOUND
 */
gchar *clawt_office_renderer_resolve(GError **error);

/**
 * clawt_office_renderer_embed_argv:
 * @binary: the renderer, from clawt_office_renderer_resolve()
 * @fd: the application's end of the socket, as the child will see it
 * @connection: (nullable): the daemon the window is connected to
 * @saved: whether @connection is a saved connection profile
 * @width: the size to start at
 * @height: likewise
 * @error: return location for a #GError
 *
 * The command line that embeds the renderer:
 * `clawtilla-office --embed=FD [--socket PATH | --profile NAME] --width W
 * --height H`.  A local connection is named by its socket (nothing for
 * the default one); a remote one only by its saved profile name, because
 * its token must never reach an argv, which any user can read.  So a
 * remote connection that is not saved is refused with
 * %CLAWT_ERROR_NOT_SUPPORTED.
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable): the argv
 */
gchar **clawt_office_renderer_embed_argv(const gchar      *binary,
                                         gint              fd,
                                         ClawtConnection  *connection,
                                         gboolean          saved,
                                         guint             width,
                                         guint             height,
                                         GError          **error);

G_END_DECLS

#endif /* CLAWT_OFFICE_FRAMES_H */
