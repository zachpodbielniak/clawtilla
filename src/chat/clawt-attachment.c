/*
 * clawt-attachment.c - Files an agent sends to its operator
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "chat/clawt-attachment.h"
#include "chat/clawt-message.h"
#include "clawt-error.h"
#include "clawt-util.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#ifdef CLAWT_HAVE_POPPLER
#include <poppler.h>
#include <cairo.h>
#endif

/*
 * An id is <random>-<safe filename>.
 *
 * The name is carried in the id so a client can label the file without a
 * second field to keep in step, and it is sanitised on the way in rather
 * than escaped at each use -- the same rule agent ids follow.  Every
 * character outside the safe set becomes "_", which cannot produce a
 * separator. The random prefix prevents the resulting id from being a
 * dot component, even when the original basename starts with dots.
 */
static gchar *
safe_name(const gchar *path)
{
    g_autofree gchar *base = g_path_get_basename(path);
    GString *out = g_string_new(NULL);
    const gchar *p;

    for (p = base; *p != '\0'; p++) {
        if (g_ascii_isalnum(*p) || *p == '.' || *p == '_')
            g_string_append_c(out, *p);
        else
            g_string_append_c(out, '_');
    }

    /*
     * A name that is only dots would make "." or ".." after the id's
     * prefix is stripped, and an empty one would leave a trailing "-".
     */
    if (out->len == 0 || g_strcmp0(out->str, ".") == 0 ||
        g_strcmp0(out->str, "..") == 0) {
        g_string_assign(out, "file");
    }

    /* Bounded, because it becomes a filename. */
    if (out->len > 96)
        g_string_truncate(out, 96);

    return g_string_free(out, FALSE);
}

gchar *
clawt_attachment_store(const gchar *dir, const gchar *path, GError **error)
{
    g_autofree gchar *id = NULL;
    g_autofree gchar *random = NULL;
    g_autofree gchar *name = NULL;
    g_autofree gchar *target = NULL;
    g_autoptr(GFile) source_file = NULL;
    g_autoptr(GFile) target_file = NULL;

    g_return_val_if_fail(dir != NULL, NULL);
    g_return_val_if_fail(path != NULL, NULL);

    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "there is no file at %s", path);
        return NULL;
    }

    if (g_mkdir_with_parents(dir, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not create %s", dir);
        return NULL;
    }

    random = clawt_generate_id(NULL);
    name = safe_name(path);
    id = g_strdup_printf("%s-%s", random, name);
    target = g_build_filename(dir, id, NULL);

    source_file = g_file_new_for_path(path);
    target_file = g_file_new_for_path(target);

    if (!g_file_copy(source_file, target_file, G_FILE_COPY_OVERWRITE, NULL,
                     NULL, NULL, error))
        return NULL;

    return g_steal_pointer(&id);
}

/* The character set an id is made of, checked rather than trusted. */
static gboolean
id_is_ours(const gchar *id)
{
    const gchar *p;

    if (id == NULL || *id == '\0' || *id == '.')
        return FALSE;

    for (p = id; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return FALSE;
    }

	/*
	 * Adjacent dots inside a basename are ordinary filename bytes, not a
	 * parent-directory component. Leading dots and both separators were
	 * rejected above. Accepting these also repairs ids already stored by
	 * older versions, whose writer has always preserved dots in names.
	 */
	return TRUE;
}

gchar *
clawt_attachment_path(const gchar *dir, const gchar *id)
{
    g_return_val_if_fail(dir != NULL, NULL);

    if (!id_is_ours(id))
        return NULL;

    return g_build_filename(dir, id, NULL);
}

gchar *
clawt_attachment_name(const gchar *id)
{
    const gchar *dash;

    if (!id_is_ours(id))
        return NULL;

    dash = strchr(id, '-');

    if (dash == NULL || dash[1] == '\0')
        return g_strdup(id);

    return g_strdup(dash + 1);
}

/* ── The block an operator's message carries ─────────────────────── */

GString *
clawt_attachment_block_new(const gchar *body)
{
    GString *out = g_string_new(body);

    if (out->len > 0)
        g_string_append(out, "\n\n");

    /*
     * The marker, then the instruction.  The marker is what clients
     * match on; the sentence after it is for the agent, which reads
     * files with its own tools and needs telling which path those
     * tools can open.
     */
    g_string_append(out, CLAWT_ATTACHMENT_MARKER
                         ". Open them with your own read tool at the host "
                         "path below; it runs on the host even when your "
                         "shell does not.\n");

    return out;
}

void
clawt_attachment_block_add(GString     *block,
                           const gchar *name,
                           const gchar *host_path,
                           const gchar *path,
                           const gchar *id)
{
    g_return_if_fail(block != NULL);
    g_return_if_fail(name != NULL);

    g_string_append_printf(block, "- %s\n  %s\n", name,
                           host_path != NULL ? host_path : "");

    /*
     * Both paths, host first: given only the container one, an agent
     * could stat the file with computer_exec and never open it with its
     * own read tool -- which is what happened to the first image anybody
     * sent.
     */
    if (path != NULL && *path != '\0' && g_strcmp0(host_path, path) != 0)
        g_string_append_printf(block, "  (inside your container: %s)\n",
                               path);

    /*
     * The daemon's own copy, by id: what every client previews from,
     * since the host path opens only on the daemon's machine.
     */
    if (id != NULL && *id != '\0')
        g_string_append_printf(block, "  clawt:%s\n", id);
}

void
clawt_attachment_block_add_unsaved(GString *block, const gchar *name)
{
    g_return_if_fail(block != NULL);
    g_return_if_fail(name != NULL);

    g_string_append_printf(block, "- %s (could not be saved)\n", name);
}

/* ── Reading the block back ─────────────────────────────────────────── */

void
clawt_attachment_ref_free(ClawtAttachmentRef *ref)
{
    if (ref == NULL)
        return;

    g_free(ref->value);
    g_free(ref->name);
    g_free(ref);
}

/*
 * Whether a line still belongs to the block: list items and their
 * indented continuations.  Anything at the left margin ends it, so a path
 * an agent quotes further down does not sprout a preview.  (Leaving on
 * "no slash on this line" cost the first preview ever sent: every item
 * starts with a name line.)
 */
static gboolean
line_is_in_block(const gchar *line)
{
    /* "\r" too: a body typed in a browser arrives with CRLF line ends. */
    return line[0] == '\0' || line[0] == ' ' || line[0] == '-' ||
           line[0] == '\t' || line[0] == '\r';
}

/*
 * The name on an item's "- " line.  An old item was "- /host/path", so a
 * line that is a path is named by its basename; a refused file's
 * "(could not be saved)" is not part of its name.
 */
static gchar *
item_name(const gchar *line)
{
    g_autofree gchar *name = g_strdup(line + 1);

    g_strstrip(name);

    if (g_str_has_suffix(name, "(could not be saved)")) {
        name[strlen(name) - strlen("(could not be saved)")] = '\0';
        g_strstrip(name);
    }

    if (name[0] == '/')
        return g_path_get_basename(name);

    if (name[0] == '\0')
        return NULL;

    return g_steal_pointer(&name);
}

GPtrArray *
clawt_attachment_refs_parse(const gchar *body)
{
    GPtrArray *refs = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_attachment_ref_free);
    g_auto(GStrv) lines = NULL;
    g_autofree gchar *name = NULL;  /* the current item's name */
    gboolean in_block = FALSE;
    gint item_path = -1;    /* this item's host-path ref, while it has one */
    gboolean item_copy = FALSE;  /* this item already has a daemon copy */
    gsize i;

    if (body == NULL || strstr(body, CLAWT_ATTACHMENT_MARKER) == NULL)
        return refs;

    lines = g_strsplit(body, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        const gchar *start;
        ClawtAttachmentRef *ref;

        if (strstr(lines[i], CLAWT_ATTACHMENT_MARKER) != NULL) {
            in_block = TRUE;
            continue;
        }

        if (!in_block)
            continue;

        if (!line_is_in_block(lines[i])) {
            in_block = FALSE;
            continue;
        }

        /* a new item: none of its refs seen yet */
        if (lines[i][0] == '-') {
            item_path = -1;
            item_copy = FALSE;
            g_free(name);
            name = item_name(lines[i]);
        }

        ref = g_new0(ClawtAttachmentRef, 1);

        /* an agent's file is `clawt:<id>`: the bytes are the daemon's */
        start = strstr(lines[i], "clawt:");

        if (start != NULL) {
            ref->daemon_copy = TRUE;
            ref->value = g_strdup(start + strlen("clawt:"));
        } else {
            start = strchr(lines[i], '/');

            if (start == NULL) {
                g_free(ref);
                continue;
            }

            ref->value = g_strdup(start);
        }

        g_strstrip(ref->value);

        /* the container path is in brackets; the host one is not */
        if (*ref->value == '\0' || g_str_has_suffix(ref->value, ")")) {
            clawt_attachment_ref_free(ref);
            continue;
        }

        /*
         * One ref per item, and the daemon's copy wins: an operator's
         * file is named by its host path (for the agent) and by
         * `clawt:<id>` (for the clients), and the path opens only on the
         * daemon's own machine.
         */
        if (ref->daemon_copy) {
            if (item_copy) {
                clawt_attachment_ref_free(ref);
                continue;
            }

            item_copy = TRUE;

            if (item_path >= 0) {
                g_ptr_array_remove_index(refs, (guint)item_path);
                item_path = -1;
            }
        } else {
            if (item_copy || item_path >= 0) {
                clawt_attachment_ref_free(ref);
                continue;
            }

            item_path = (gint)refs->len;
        }

        /*
         * Named by its item line, or -- for an item with none, which only
         * a hand-written block has -- by the id's carried name or the
         * path's basename.
         */
        if (name != NULL)
            ref->name = g_strdup(name);
        else if (ref->daemon_copy)
            ref->name = clawt_attachment_name(ref->value);

        if (ref->name == NULL)
            ref->name = g_path_get_basename(ref->value);

        g_ptr_array_add(refs, ref);
    }

    return refs;
}

gchar *
clawt_attachment_prose(const gchar *body)
{
    g_auto(GStrv) lines = NULL;
    GString *prose;
    gboolean in_block = FALSE;
    gsize i;

    if (body == NULL)
        return g_strdup("");

    if (strstr(body, CLAWT_ATTACHMENT_MARKER) == NULL)
        return g_strchomp(g_strdup(body));

    lines = g_strsplit(body, "\n", -1);
    prose = g_string_new(NULL);

    for (i = 0; lines[i] != NULL; i++) {
        if (strstr(lines[i], CLAWT_ATTACHMENT_MARKER) != NULL) {
            in_block = TRUE;
            continue;
        }

        if (!in_block) {
            if (prose->len > 0)
                g_string_append_c(prose, '\n');

            g_string_append(prose, lines[i]);
            continue;
        }

        /* The block ends at the margin; what follows is prose again. */
        if (!line_is_in_block(lines[i])) {
            in_block = FALSE;
            g_string_append_c(prose, '\n');
            g_string_append(prose, lines[i]);
        }
    }

    return g_strchomp(g_string_free(prose, FALSE));
}

const gchar *
clawt_attachment_kind(const gchar *name)
{
    static const gchar *const images[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", NULL
    };
    static const gchar *const texts[] = {
        ".txt", ".md", ".org", ".log", ".csv", ".tsv", ".json", ".yaml",
        ".yml", ".toml", ".ini", ".conf", ".cfg", ".xml", ".html", ".c",
        ".h", ".py", ".sh", ".rs", ".go", ".js", ".ts", ".hs", ".el",
        ".diff", ".patch", NULL
    };
    g_autofree gchar *lowered = NULL;
    gsize i;

    if (name == NULL)
        return "other";

    lowered = g_ascii_strdown(name, -1);

    for (i = 0; images[i] != NULL; i++) {
        if (g_str_has_suffix(lowered, images[i]))
            return "image";
    }

    if (g_str_has_suffix(lowered, ".pdf"))
        return "pdf";

    for (i = 0; texts[i] != NULL; i++) {
        if (g_str_has_suffix(lowered, texts[i]))
            return "text";
    }

    return "other";
}

/* ── A conversation's files ─────────────────────────────────────────── */

void
clawt_attachment_item_free(ClawtAttachmentItem *item)
{
    if (item == NULL)
        return;

    g_free(item->id);
    g_free(item->name);
    g_free(item->path);
    g_free(item->message);
    g_free(item->sender);
    g_free(item);
}

/* Whether @path is a regular file, and its size when it is. */
static gboolean
regular_file_size(const gchar *path, gint64 *bytes)
{
    GStatBuf st;

    *bytes = 0;

    if (path == NULL || g_stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return FALSE;

    *bytes = (gint64)st.st_size;

    return TRUE;
}

GPtrArray *
clawt_attachment_list(const gchar *dir, GPtrArray *messages)
{
    GPtrArray *items = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_attachment_item_free);
    guint i;

    if (messages == NULL)
        return items;

    /* Newest first: the history is oldest first. */
    for (i = messages->len; i > 0; i--) {
        ClawtMessage *message = g_ptr_array_index(messages, i - 1);
        g_autoptr(GPtrArray) refs =
            clawt_attachment_refs_parse(clawt_message_get_body(message));
        guint r;

        for (r = 0; r < refs->len; r++) {
            ClawtAttachmentRef *ref = g_ptr_array_index(refs, r);
            ClawtAttachmentItem *item = g_new0(ClawtAttachmentItem, 1);

            item->name = g_strdup(ref->name);
            item->kind = clawt_attachment_kind(ref->name);
            item->message = g_strdup(clawt_message_get_id(message));
            item->sender = g_strdup(clawt_message_get_sender_id(message));
            item->ts = clawt_message_get_timestamp(message);

            /*
             * The daemon's copy is checked through
             * clawt_attachment_path(), so an id in a message body -- which
             * any agent can write -- that is not one of ours is listed as
             * gone rather than resolved to a path of its choosing.
             */
            if (ref->daemon_copy) {
                g_autofree gchar *path = (dir != NULL)
                    ? clawt_attachment_path(dir, ref->value) : NULL;

                item->id = g_strdup(ref->value);
                item->exists = regular_file_size(path, &item->bytes);
            } else {
                item->path = g_strdup(ref->value);
                item->exists = regular_file_size(ref->value, &item->bytes);
            }

            g_ptr_array_add(items, item);
        }
    }

    return items;
}

/* ── Thumbnails ─────────────────────────────────────────────────────── */

guint
clawt_attachment_thumbnail_size(gint64 requested)
{
    if (requested <= 0)
        return CLAWT_ATTACHMENT_THUMB_DEFAULT;

    return (guint)CLAMP(requested, CLAWT_ATTACHMENT_THUMB_MIN,
                        CLAWT_ATTACHMENT_THUMB_MAX);
}

gboolean
clawt_attachment_can_render_pdf(void)
{
#ifdef CLAWT_HAVE_POPPLER
    return TRUE;
#else
    return FALSE;
#endif
}

/* The largest PDF handed to poppler: past this it is not a preview. */
#define THUMB_MAX_INPUT (64 * 1024 * 1024)

/*
 * A PNG's dimensions, from its IHDR -- for a cached thumbnail, which is
 * read back as bytes and never decoded.
 */
static void
png_dimensions(GBytes *png, guint *width, guint *height)
{
    gsize length = 0;
    const guint8 *data = g_bytes_get_data(png, &length);

    if (length < 24 || memcmp(data, "\211PNG\r\n\032\n", 8) != 0)
        return;

    if (width != NULL)
        *width = ((guint)data[16] << 24) | ((guint)data[17] << 16) |
                 ((guint)data[18] << 8) | (guint)data[19];

    if (height != NULL)
        *height = ((guint)data[20] << 24) | ((guint)data[21] << 16) |
                  ((guint)data[22] << 8) | (guint)data[23];
}

#ifdef CLAWT_HAVE_POPPLER
/* cairo's PNG writer, into memory. */
static cairo_status_t
png_to_bytes(void *closure, const unsigned char *data, unsigned int length)
{
    g_byte_array_append((GByteArray *)closure, data, length);

    return CAIRO_STATUS_SUCCESS;
}

/*
 * Page 1, scaled so its longer edge is @size, on white -- a PDF page has
 * no background of its own and a transparent one reads as black on a
 * dark theme.
 */
static GBytes *
render_first_page(const gchar  *path,
                  guint         size,
                  guint        *width,
                  guint        *height,
                  GError      **error)
{
    g_autofree gchar *contents = NULL;
    g_autoptr(GBytes) input = NULL;
    g_autoptr(GByteArray) png = NULL;
    PopplerDocument *document;
    PopplerPage *page;
    cairo_surface_t *surface;
    cairo_t *cr;
    cairo_status_t status;
    gdouble page_width = 0;
    gdouble page_height = 0;
    gdouble scale;
    gint pixel_width;
    gint pixel_height;
    gsize length = 0;

    if (!g_file_get_contents(path, &contents, &length, error))
        return NULL;

    if (length > THUMB_MAX_INPUT) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "that PDF is too large to preview");
        return NULL;
    }

    input = g_bytes_new_take(g_steal_pointer(&contents), length);
    document = poppler_document_new_from_bytes(input, NULL, error);

    if (document == NULL)
        return NULL;

    page = poppler_document_get_page(document, 0);

    if (page == NULL) {
        g_object_unref(document);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "that PDF has no pages");
        return NULL;
    }

    poppler_page_get_size(page, &page_width, &page_height);

    /*
     * A page size is the file's word for it, so it is checked: zero,
     * negative or not-a-number would make a scale of infinity.
     */
    if (!(page_width > 0.5) || !(page_height > 0.5) ||
        page_width > 1e6 || page_height > 1e6) {
        g_object_unref(page);
        g_object_unref(document);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "that PDF's first page has no usable size");
        return NULL;
    }

    /*
     * Rounded, not ceiled: the longer edge times its own scale is @size
     * up to a rounding error, and a ceiling turns 64.000001 into 65.
     */
    scale = (gdouble)size / MAX(page_width, page_height);
    pixel_width = CLAMP((gint)(page_width * scale + 0.5), 1, (gint)size);
    pixel_height = CLAMP((gint)(page_height * scale + 0.5), 1, (gint)size);

    surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixel_width,
                                         pixel_height);
    cr = cairo_create(surface);
    cairo_set_source_rgb(cr, 1.0, 1.0, 1.0);
    cairo_paint(cr);
    cairo_scale(cr, scale, scale);
    poppler_page_render(page, cr);
    status = cairo_status(cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);

    g_object_unref(page);
    g_object_unref(document);

    png = g_byte_array_new();

    if (status == CAIRO_STATUS_SUCCESS)
        status = cairo_surface_write_to_png_stream(surface, png_to_bytes,
                                                   png);

    cairo_surface_destroy(surface);

    if (status != CAIRO_STATUS_SUCCESS) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "could not draw that PDF: %s",
                    cairo_status_to_string(status));
        return NULL;
    }

    if (width != NULL)
        *width = (guint)pixel_width;

    if (height != NULL)
        *height = (guint)pixel_height;

    return g_byte_array_free_to_bytes(g_steal_pointer(&png));
}
#endif

GBytes *
clawt_attachment_thumbnail(const gchar  *dir,
                           const gchar  *id,
                           guint         size,
                           guint        *width,
                           guint        *height,
                           GError      **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *name = NULL;
    g_autofree gchar *thumbs = NULL;
    g_autofree gchar *cached = NULL;
    g_autofree gchar *leaf = NULL;
    gchar *contents = NULL;
    gsize length = 0;

    g_return_val_if_fail(dir != NULL, NULL);

    if (width != NULL)
        *width = 0;

    if (height != NULL)
        *height = 0;

    /* Checked before anything is opened: the id came off the wire. */
    path = clawt_attachment_path(dir, id);

    if (path == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "that is not an attachment id");
        return NULL;
    }

    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no such attachment");
        return NULL;
    }

    name = clawt_attachment_name(id);

    if (g_strcmp0(clawt_attachment_kind(name), "pdf") != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "only a PDF is drawn as a thumbnail");
        return NULL;
    }

    size = clawt_attachment_thumbnail_size((gint64)size);

    /*
     * An id names bytes that never change, so a thumbnail made once is
     * right forever.  The directory starts with a dot, which no id can,
     * so it cannot be asked for as an attachment.
     */
    thumbs = g_build_filename(dir, ".thumbs", NULL);
    leaf = g_strdup_printf("%s.%u.png", id, size);
    cached = g_build_filename(thumbs, leaf, NULL);

    if (g_file_get_contents(cached, &contents, &length, NULL)) {
        GBytes *png = g_bytes_new_take(contents, length);

        png_dimensions(png, width, height);

        return png;
    }

#ifdef CLAWT_HAVE_POPPLER
    {
        GBytes *png = render_first_page(path, size, width, height, error);
        gsize png_length = 0;
        const gchar *png_data;
        g_autoptr(GError) cache_error = NULL;

        if (png == NULL)
            return NULL;

        /* Best effort: a thumbnail that cannot be cached is still drawn. */
        png_data = g_bytes_get_data(png, &png_length);

        if (g_mkdir_with_parents(thumbs, 0700) != 0 ||
            !clawt_write_file_atomic(cached, png_data, (gssize)png_length,
                                     0600, FALSE, &cache_error))
            g_debug("attachment thumbnail for %s not cached: %s", id,
                    cache_error != NULL ? cache_error->message
                                        : "could not make the directory");

        return png;
    }
#else
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                "no PDF renderer in this build");
    return NULL;
#endif
}

gchar *
clawt_attachment_text_head(const gchar  *dir,
                           const gchar  *id,
                           guint         lines,
                           GError      **error)
{
    g_autofree gchar *path = NULL;
    g_autofree gchar *valid = NULL;
    g_auto(GStrv) split = NULL;
    GString *out;
    gchar buffer[8192];
    gsize got;
    FILE *file;
    guint i;

    g_return_val_if_fail(dir != NULL, NULL);

    path = clawt_attachment_path(dir, id);

    if (path == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                    "that is not an attachment id");
        return NULL;
    }

    if (!g_file_test(path, G_FILE_TEST_IS_REGULAR) ||
        (file = g_fopen(path, "rb")) == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "no such attachment");
        return NULL;
    }

    /* A bounded read: a tile shows a dozen lines of a file of any size. */
    got = fread(buffer, 1, sizeof(buffer), file);
    fclose(file);

    /*
     * Valid UTF-8 whatever the file held, with NULs replaced: it goes
     * into a JSON string and onto a page.
     */
    for (i = 0; i < got; i++) {
        if (buffer[i] == '\0')
            buffer[i] = '?';
    }

    valid = g_utf8_make_valid(buffer, (gssize)got);
    split = g_strsplit(valid, "\n", -1);
    out = g_string_new(NULL);

    for (i = 0; split[i] != NULL && i < lines; i++) {
        glong chars = g_utf8_strlen(split[i], -1);

        g_strchomp(split[i]);

        if (i > 0)
            g_string_append_c(out, '\n');

        if (chars > 160) {
            const gchar *end = g_utf8_offset_to_pointer(split[i], 160);

            g_string_append_len(out, split[i], end - split[i]);
            g_string_append(out, "\xe2\x80\xa6");
        } else {
            g_string_append(out, split[i]);
        }
    }

    return g_string_free(out, FALSE);
}
