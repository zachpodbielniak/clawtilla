/*
 * clawt-office-sheet.c - A character's sprite sheet, cut from its frames
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * PixelLab answers a character as one PNG per direction and a walk as
 * one PNG per frame per direction; the office draws a character from one
 * sheet with a row per animation (clients/office/office-scene.c).  This
 * puts the one into the other.  cairo is the only image library the
 * daemon may have, and only when built with POPPLER=1, so without it
 * this says so and the caller falls back to one frame.
 */

#include "clawtilla.h"

#include <string.h>

#ifdef CLAWT_HAVE_POPPLER
#include <cairo.h>
#endif

gboolean
clawt_office_sheet_available(void)
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
} Reader;

static cairo_status_t
read_bytes(
	void          *closure,
	unsigned char *out,
	unsigned int   length
){
	Reader *reader = closure;

	if (reader->at + length > reader->length)
		return CAIRO_STATUS_READ_ERROR;
	memcpy(out, reader->data + reader->at, length);
	reader->at += length;
	return CAIRO_STATUS_SUCCESS;
}

static cairo_status_t
write_bytes(
	void                *closure,
	const unsigned char *data,
	unsigned int         length
){
	g_byte_array_append(closure, data, length);
	return CAIRO_STATUS_SUCCESS;
}

/* A PNG as a cairo surface, or NULL when it is not one cairo can read. */
static cairo_surface_t *
surface_from(
	GBytes *png
){
	Reader reader = { NULL, 0, 0 };
	cairo_surface_t *surface;

	reader.data = g_bytes_get_data(png, &reader.length);
	surface = cairo_image_surface_create_from_png_stream(read_bytes, &reader);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}
	return surface;
}
#endif

GBytes *
clawt_office_sheet_compose(
	GPtrArray  *rows,
	guint      *out_frame_width,
	guint      *out_frame_height,
	GError    **error
){
#ifdef CLAWT_HAVE_POPPLER
	g_autoptr(GPtrArray) surfaces = NULL;
	cairo_surface_t *sheet;
	cairo_t *cr;
	GByteArray *out;
	gint cell_w = 0;
	gint cell_h = 0;
	guint columns = 0;
	guint r;
	guint c;

	g_return_val_if_fail(rows != NULL, NULL);

	/* Every frame read first: the cell is the largest of them. */
	surfaces = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	for (r = 0; r < rows->len; r++) {
		GPtrArray *row = g_ptr_array_index(rows, r);
		GPtrArray *decoded = g_ptr_array_new_with_free_func(
			(GDestroyNotify)cairo_surface_destroy);

		for (c = 0; row != NULL && c < row->len; c++) {
			cairo_surface_t *frame = surface_from(g_ptr_array_index(row, c));

			if (frame == NULL) {
				g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
				            "frame %u of row %u is not a PNG", c, r);
				g_ptr_array_unref(decoded);
				return NULL;
			}
			cell_w = MAX(cell_w, cairo_image_surface_get_width(frame));
			cell_h = MAX(cell_h, cairo_image_surface_get_height(frame));
			g_ptr_array_add(decoded, frame);
		}
		columns = MAX(columns, decoded->len);
		g_ptr_array_add(surfaces, decoded);
	}

	if (cell_w == 0 || cell_h == 0 || columns == 0) {
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
		                    "there are no frames to lay out");
		return NULL;
	}

	sheet = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
	                                   cell_w * (gint)columns,
	                                   cell_h * (gint)rows->len);
	cr = cairo_create(sheet);

	for (r = 0; r < surfaces->len; r++) {
		GPtrArray *row = g_ptr_array_index(surfaces, r);

		for (c = 0; c < row->len; c++) {
			cairo_surface_t *frame = g_ptr_array_index(row, c);
			gint w = cairo_image_surface_get_width(frame);
			gint h = cairo_image_surface_get_height(frame);

			/* centred, so a frame narrower than the cell does not jump */
			cairo_set_source_surface(cr, frame,
			                         (gdouble)(c * cell_w + (cell_w - w) / 2),
			                         (gdouble)(r * cell_h + (cell_h - h) / 2));
			cairo_paint(cr);
		}
	}

	cairo_destroy(cr);
	out = g_byte_array_new();
	if (cairo_surface_write_to_png_stream(sheet, write_bytes, out) !=
	    CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(sheet);
		g_byte_array_unref(out);
		g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
		                    "the sheet could not be written");
		return NULL;
	}
	cairo_surface_destroy(sheet);

	*out_frame_width = (guint)cell_w;
	*out_frame_height = (guint)cell_h;
	return g_byte_array_free_to_bytes(out);
#else
	(void)rows;
	(void)out_frame_width;
	(void)out_frame_height;
	g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
	                    "this daemon was built without cairo (POPPLER=1), so it "
	                    "cannot draw a sprite sheet");
	return NULL;
#endif
}

JsonObject *
clawt_office_sheet_walk_layout(
	GPtrArray *rows,
	guint      fps
){
	static const gchar *const names[] = {
		"idle", "walk_down", "walk_up", "walk_left", "walk_right"
	};
	JsonObject *animations = json_object_new();
	guint r;

	g_return_val_if_fail(rows != NULL, animations);

	for (r = 0; r < rows->len && r < G_N_ELEMENTS(names); r++) {
		GPtrArray *row = g_ptr_array_index(rows, r);
		JsonObject *one;

		if (row == NULL || row->len == 0)
			continue;

		one = json_object_new();
		json_object_set_int_member(one, "row", r);
		json_object_set_int_member(one, "frames", r == 0 ? 1 : row->len);
		if (r > 0)
			json_object_set_int_member(one, "fps", fps);
		json_object_set_object_member(animations, names[r], one);
	}

	return animations;
}
