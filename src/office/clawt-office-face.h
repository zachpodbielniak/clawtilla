/*
 * clawt-office-face.h - Which face a person in the office wears
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The web office and the native office (clawtilla-office, the GTK Office
 * page, the lrg Office page) used to decide this separately, and the
 * native one never looked at a profile picture.  One rule, so they cannot
 * drift apart again.  A character sheet or mesh still wins; then a
 * picture; then the generated face; then the letter disc.
 */

#ifndef CLAWT_OFFICE_FACE_H
#define CLAWT_OFFICE_FACE_H

#if !defined(CLAWT_INSIDE) && !defined(CLAWT_COMPILATION)
#error "Only <clawtilla.h> can be included directly."
#endif

#include <glib.h>

G_BEGIN_DECLS

/**
 * CLAWT_OFFICE_PORTRAIT_SIZE:
 *
 * The square, in pixels, a decoded profile picture is scaled to.  The
 * office draws it as a small token, so the full file is never uploaded.
 */
#define CLAWT_OFFICE_PORTRAIT_SIZE (128)

/**
 * CLAWT_OFFICE_PORTRAIT_MAX_BYTES:
 *
 * A picture larger than this is refused before it is decoded.
 */
#define CLAWT_OFFICE_PORTRAIT_MAX_BYTES ((gsize)2 * 1024 * 1024)

/**
 * CLAWT_OFFICE_PORTRAIT_MAX_EDGE:
 *
 * A PNG whose width or height is over this is refused.  A huge image
 * would decode into a surface the cache is not willing to hold.
 */
#define CLAWT_OFFICE_PORTRAIT_MAX_EDGE (2048)

/**
 * ClawtOfficeFaceKind:
 * @CLAWT_OFFICE_FACE_CHARACTER: a character sheet or mesh is worn
 * @CLAWT_OFFICE_FACE_PICTURE: a profile picture is worn
 * @CLAWT_OFFICE_FACE_GENERATED: a generated face (a shape, not the letter)
 * @CLAWT_OFFICE_FACE_BLOB: the letter disc, the colour blob
 *
 * What a participant is drawn as.  The order is the enum order: a
 * character wins over a picture, a picture over a generated face, and a
 * generated face over the letter disc.
 */
typedef enum {
    CLAWT_OFFICE_FACE_CHARACTER = 0,
    CLAWT_OFFICE_FACE_PICTURE,
    CLAWT_OFFICE_FACE_GENERATED,
    CLAWT_OFFICE_FACE_BLOB
} ClawtOfficeFaceKind;

/**
 * ClawtOfficeFace:
 * @kind: which face is worn
 * @shape: the generated shape, or -1 (%CLAWT_FACE_INITIALS) for the letter
 * @has_picture: %TRUE when a profile picture is stored, even if a
 *   character sheet is what is actually worn
 *
 * The decision clawt_office_face_resolve() writes.  The colour and the
 * letter are written into the buffers the caller passes, because a
 * fixed-size array in a public struct is awkward for introspection.
 */
typedef struct {
    ClawtOfficeFaceKind kind;
    gint                shape;
    gboolean            has_picture;
} ClawtOfficeFace;

/**
 * clawt_office_face_resolve:
 * @out: (out caller-allocates): the kind, the shape and whether a picture is stored
 * @id: (nullable): the participant's id
 * @name: (nullable): the name to take the letter from
 * @configured_color: (nullable): a `#rgb` or `#rrggbb`, or %NULL to derive one
 * @face: (nullable): the resolved face name (`agent.list`'s `face`), or %NULL
 * @has_avatar: %TRUE when a profile picture is stored for this participant
 * @has_character: %TRUE when a character sheet or mesh wins
 * @color: (out caller-allocates) (array length=color_len): the body colour, NUL-terminated
 * @color_len: the size of @color, at least 8
 * @initial: (out caller-allocates) (array length=initial_len): the letter, UTF-8, NUL-terminated
 * @initial_len: the size of @initial, at least 8
 *
 * Which face @id wears, and the colour and letter that go with it.
 *
 * A character wins, then a picture, then a generated face, then the
 * letter disc.  @face %NULL (a Grok bot, a visitor, anyone not in
 * agent.list) is the generated face of @id, the same fallback the web
 * office already used.  An unknown shape name is the letter disc.  The
 * colour is clawt_avatar_color().
 */
void clawt_office_face_resolve(ClawtOfficeFace *out,
                               const gchar     *id,
                               const gchar     *name,
                               const gchar     *configured_color,
                               const gchar     *face,
                               gboolean         has_avatar,
                               gboolean         has_character,
                               gchar           *color,
                               gsize            color_len,
                               gchar           *initial,
                               gsize            initial_len);

/**
 * clawt_office_face_picture_url:
 * @id: (nullable): the participant's id
 *
 * The path the web office already serves this picture at.
 *
 * Returns: (transfer full) (nullable): `/a/<escaped id>/avatar`, or %NULL
 *   when @id is empty
 */
gchar *clawt_office_face_picture_url(const gchar *id);

/**
 * clawt_office_portrait_decode_available:
 *
 * Whether clawt_office_portrait_decode() can run.  It needs cairo, which
 * this build has when POPPLER=1.  gdk-pixbuf is not used: its loader
 * sandboxes with glycin, and that sandbox does not run in the containers
 * the office is tested in.
 *
 * Returns: %TRUE when a PNG can be decoded here
 */
gboolean clawt_office_portrait_decode_available(void);

/**
 * clawt_office_portrait_decode:
 * @data: (array length=length): the picture's bytes
 * @length: how many
 * @out_rgba: (out) (transfer full): square straight RGBA, row by row
 * @out_size: (out): the square's side, %CLAWT_OFFICE_PORTRAIT_SIZE
 * @error: (out) (optional): where a failure is written
 *
 * A PNG as a circle-masked token.  Scaled with a bilinear filter so a
 * pixel-art portrait stays legible when the office draws it small, and
 * refused when it is not a PNG, when it is over
 * %CLAWT_OFFICE_PORTRAIT_MAX_BYTES, or when an edge is over
 * %CLAWT_OFFICE_PORTRAIT_MAX_EDGE.  JPEG is not decoded here.
 *
 * Returns: %TRUE when @out_rgba holds the token
 */
gboolean clawt_office_portrait_decode(const guint8 *data,
                                     gsize         length,
                                     guint8      **out_rgba,
                                     gint         *out_size,
                                     GError      **error);

G_END_DECLS

#endif /* CLAWT_OFFICE_FACE_H */
