/*
 * clawt-office-face.c - Which face a person in the office wears
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * cairo reads the PNG.  gdk-pixbuf would hand it to glycin, and glycin's
 * bwrap sandbox does not run in the containers this is built and tested
 * in, so a picture decoded that way fails closed and the office draws a
 * blob while the web client draws the picture.  cairo is already how a
 * character sheet is read (clawt-office-sheet.c), and it comes with the
 * same POPPLER=1 build.
 */

#include "clawtilla.h"

#include <string.h>

#ifdef CLAWT_HAVE_POPPLER
#include <cairo.h>
#endif

static void
fill_initial(
	gchar       *initial,
	gsize        initial_len,
	const gchar *name
){
	gunichar first;

	memset(initial, 0, initial_len);
	if (name == NULL || initial_len < 2)
		return;
	first = g_utf8_get_char_validated(name, -1);
	if (first == (gunichar)-1 || first == (gunichar)-2)
		return;
	if (g_unichar_to_utf8(g_unichar_toupper(first), initial) >= (gint)initial_len)
		initial[0] = '\0';
}

static gint
shape_of(
	const gchar *id,
	const gchar *face
){
	gint shape;

	/*
	 * No face (a Grok bot, a visitor, a name that was never configured)
	 * is the generated face of the id.  A name this build does not know
	 * is the letter disc, which is what the web client does with a
	 * noted face it cannot draw.
	 */
	if (face == NULL || face[0] == '\0')
		return clawt_face_resolve(id, NULL);
	shape = clawt_face_shape_lookup(face);
	if (shape == -2)
		return CLAWT_FACE_INITIALS;
	return shape;
}

void
clawt_office_face_resolve(
	ClawtOfficeFace *out,
	const gchar     *id,
	const gchar     *name,
	const gchar     *configured_color,
	const gchar     *face,
	gboolean         has_avatar,
	gboolean         has_character,
	gchar           *color,
	gsize            color_len,
	gchar           *initial,
	gsize            initial_len
){
	const gchar *ink;
	gint shape;

	g_return_if_fail(out != NULL);
	g_return_if_fail(color != NULL && color_len >= 8);
	g_return_if_fail(initial != NULL && initial_len >= 8);

	shape = shape_of(id, face);
	ink = clawt_avatar_color(id, name, configured_color);
	g_strlcpy(color, ink != NULL ? ink : "", color_len);
	fill_initial(initial, initial_len, name != NULL ? name : id);

	out->shape = shape;
	out->has_picture = has_avatar;
	if (has_character)
		out->kind = CLAWT_OFFICE_FACE_CHARACTER;
	else if (has_avatar)
		out->kind = CLAWT_OFFICE_FACE_PICTURE;
	else if (shape == CLAWT_FACE_INITIALS)
		out->kind = CLAWT_OFFICE_FACE_BLOB;
	else
		out->kind = CLAWT_OFFICE_FACE_GENERATED;
}

gchar *
clawt_office_face_picture_url(
	const gchar *id
){
	g_autofree gchar *escaped = NULL;

	if (id == NULL || id[0] == '\0')
		return NULL;
	escaped = g_uri_escape_string(id, NULL, FALSE);
	return g_strdup_printf("/a/%s/avatar", escaped);
}

gboolean
clawt_office_portrait_decode_available(void)
{
#ifdef CLAWT_HAVE_POPPLER
	return TRUE;
#else
	return FALSE;
#endif
}

#ifdef CLAWT_HAVE_POPPLER
typedef struct {
	const guchar *data;
	gsize         length;
	gsize         at;
} PngReader;

static cairo_status_t
read_png(
	void          *closure,
	unsigned char *out,
	unsigned int   length
){
	PngReader *reader = closure;

	if (reader->at + length > reader->length)
		return CAIRO_STATUS_READ_ERROR;
	memcpy(out, reader->data + reader->at, length);
	reader->at += length;
	return CAIRO_STATUS_SUCCESS;
}

/* cairo's pixels are native-endian premultiplied 0xAARRGGBB. */
static void
straight_rgba(
	const guint32 *src,
	guint8        *dst,
	gint           count
){
	gint i;

	for (i = 0; i < count; i++) {
		guint32 pixel = src[i];
		guint8 alpha = (guint8)((pixel >> 24) & 0xff);
		guint8 red = (guint8)((pixel >> 16) & 0xff);
		guint8 green = (guint8)((pixel >> 8) & 0xff);
		guint8 blue = (guint8)(pixel & 0xff);

		if (alpha != 0 && alpha != 255) {
			red = (guint8)(((guint)red * 255) / alpha);
			green = (guint8)(((guint)green * 255) / alpha);
			blue = (guint8)(((guint)blue * 255) / alpha);
		} else if (alpha == 0) {
			red = 0;
			green = 0;
			blue = 0;
		}
		dst[i * 4 + 0] = red;
		dst[i * 4 + 1] = green;
		dst[i * 4 + 2] = blue;
		dst[i * 4 + 3] = alpha;
	}
}
#endif

gboolean
clawt_office_portrait_decode(
	const guint8 *data,
	gsize         length,
	guint8      **out_rgba,
	gint         *out_size,
	GError      **error
){
#ifdef CLAWT_HAVE_POPPLER
	PngReader reader = { NULL, 0, 0 };
	cairo_surface_t *source;
	cairo_surface_t *dest;
	cairo_t *cr;
	gint width;
	gint height;
	gint side = CLAWT_OFFICE_PORTRAIT_SIZE;
	gdouble scale;
	gdouble smaller;
	guint8 *rgba;
	const guint32 *pixels;

	g_return_val_if_fail(out_rgba != NULL, FALSE);
	g_return_val_if_fail(out_size != NULL, FALSE);

	if (data == NULL || length < 8 ||
	    data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G') {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "a profile picture has to be a PNG");
		return FALSE;
	}
	if (length > CLAWT_OFFICE_PORTRAIT_MAX_BYTES) {
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "a profile picture is %" G_GSIZE_FORMAT " bytes, over the %"
		            G_GSIZE_FORMAT " byte cap",
		            length, CLAWT_OFFICE_PORTRAIT_MAX_BYTES);
		return FALSE;
	}

	reader.data = data;
	reader.length = length;
	source = cairo_image_surface_create_from_png_stream(read_png, &reader);
	if (cairo_surface_status(source) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(source);
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "the profile picture is not a PNG cairo can read");
		return FALSE;
	}
	width = cairo_image_surface_get_width(source);
	height = cairo_image_surface_get_height(source);
	if (width < 1 || height < 1 ||
	    width > CLAWT_OFFICE_PORTRAIT_MAX_EDGE ||
	    height > CLAWT_OFFICE_PORTRAIT_MAX_EDGE) {
		cairo_surface_destroy(source);
		g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		            "the profile picture is %dx%d, over the %d pixel edge cap",
		            width, height, CLAWT_OFFICE_PORTRAIT_MAX_EDGE);
		return FALSE;
	}

	/*
	 * Cover the square, then clip to the circle.  The surface starts
	 * clear, so the corners outside the clip stay transparent and the
	 * office can draw the token without a clip of its own.
	 */
	dest = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, side, side);
	cr = cairo_create(dest);
	smaller = (gdouble)(width < height ? width : height);
	scale = (gdouble)side / smaller;
	cairo_arc(cr, side / 2.0, side / 2.0, side / 2.0, 0.0, 6.283185307179586);
	cairo_clip(cr);
	cairo_translate(cr, ((gdouble)side - (gdouble)width * scale) / 2.0,
	                ((gdouble)side - (gdouble)height * scale) / 2.0);
	cairo_scale(cr, scale, scale);
	cairo_set_source_surface(cr, source, 0.0, 0.0);
	cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
	cairo_paint(cr);
	cairo_destroy(cr);
	cairo_surface_destroy(source);
	if (cairo_surface_status(dest) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(dest);
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		                    "the profile picture could not be scaled");
		return FALSE;
	}
	cairo_surface_flush(dest);
	pixels = (const guint32 *)cairo_image_surface_get_data(dest);
	rgba = g_new(guint8, (gsize)side * (gsize)side * 4);
	straight_rgba(pixels, rgba, side * side);
	cairo_surface_destroy(dest);
	*out_rgba = rgba;
	*out_size = side;
	return TRUE;
#else
	(void)data;
	(void)length;
	g_return_val_if_fail(out_rgba != NULL, FALSE);
	g_return_val_if_fail(out_size != NULL, FALSE);
	g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
	                    "cairo PNG decode is not in this build (POPPLER=0); "
	                    "gdk-pixbuf is not used because glycin's bwrap sandbox "
	                    "does not run in these containers");
	return FALSE;
#endif
}
