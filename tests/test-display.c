/*
 * test-display.c - the `display` computer: a virtual screen for GUI apps
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three layers, tested from the bottom:
 *
 * - the pure helpers (the PNG writer, the XWD reader, every argv, the
 *   gowl config and environment, the resolution parser and the binary
 *   resolver), with no display at all;
 * - the server and the computer against a stand-in X server -- a shell
 *   script that reports a display number on -displayfd and writes an XWD
 *   framebuffer where Xvfb would -- so the lifecycle, the environment,
 *   the startup apps and the observable are checked hermetically;
 * - the real thing: Xvfb, Xvnc and a headless gowl, each skipped when it
 *   is not installed, each on a private display and runtime directory
 *   and never the operator's session.
 */

#include <clawtilla.h>
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

/* ── Helpers ─────────────────────────────────────────────────────── */

/* The IDAT data of a PNG, inflated: the filter bytes and the pixels. */
static GBytes *
png_pixels(GBytes *png)
{
    const guint8 *data = g_bytes_get_data(png, NULL);
    gsize length = g_bytes_get_size(png);
    g_autoptr(GByteArray) idat = g_byte_array_new();
    g_autoptr(GZlibDecompressor) zlib = NULL;
    g_autoptr(GInputStream) raw = NULL;
    g_autoptr(GInputStream) inflate = NULL;
    g_autoptr(GOutputStream) out = NULL;
    gsize offset = 8;

    g_assert_cmpuint(length, >, 8);
    g_assert_cmpint(memcmp(data, "\x89PNG\r\n\x1a\n", 8), ==, 0);

    /* Every chunk: length, type, data, CRC.  IDAT gathered, IEND last. */
    while (offset + 12 <= length) {
        guint32 chunk = ((guint32)data[offset] << 24) |
                        ((guint32)data[offset + 1] << 16) |
                        ((guint32)data[offset + 2] << 8) | data[offset + 3];
        const guint8 *type = data + offset + 4;

        if (memcmp(type, "IDAT", 4) == 0)
            g_byte_array_append(idat, data + offset + 8, chunk);
        offset += 12 + chunk;
        if (memcmp(type, "IEND", 4) == 0)
            break;
    }
    g_assert_cmpuint(offset, ==, length);

    zlib = g_zlib_decompressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB);
    raw = g_memory_input_stream_new_from_data(
        g_memdup2(idat->data, idat->len), idat->len, g_free);
    inflate = g_converter_input_stream_new(raw, G_CONVERTER(zlib));
    out = g_memory_output_stream_new_resizable();
    g_assert_cmpint(g_output_stream_splice(out, inflate,
                                           G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
                                           NULL, NULL), >=, 0);
    return g_memory_output_stream_steal_as_bytes(
        G_MEMORY_OUTPUT_STREAM(out));
}

static void
put32(GByteArray *out, guint32 value, gboolean big)
{
    guint8 b[4];

    if (big) {
        b[0] = value >> 24; b[1] = value >> 16; b[2] = value >> 8; b[3] = value;
    } else {
        b[3] = value >> 24; b[2] = value >> 16; b[1] = value >> 8; b[0] = value;
    }
    g_byte_array_append(out, b, 4);
}

/*
 * An XWD dump as Xvfb writes one: the 25 header fields, a window name,
 * no colours, then @rows of pixels.
 */
static GByteArray *
make_xwd(guint width, guint height, guint bpp, gboolean header_big,
         gboolean pixels_msb, guint32 rmask, guint32 gmask, guint32 bmask,
         const guint32 *pixels)
{
    GByteArray *xwd = g_byte_array_new();
    const gchar name[] = "Xvfb main window";
    guint header = 25 * 4 + sizeof(name);
    guint bytes_per_line = width * (bpp / 8);
    guint x;
    guint y;

    put32(xwd, header, header_big);          /* header_size */
    put32(xwd, 7, header_big);               /* file_version */
    put32(xwd, 2, header_big);               /* ZPixmap */
    put32(xwd, bpp == 16 ? 16 : 24, header_big);
    put32(xwd, width, header_big);
    put32(xwd, height, header_big);
    put32(xwd, 0, header_big);               /* xoffset */
    put32(xwd, pixels_msb ? 1 : 0, header_big); /* byte_order */
    put32(xwd, 32, header_big);              /* bitmap_unit */
    put32(xwd, 0, header_big);
    put32(xwd, 32, header_big);              /* bitmap_pad */
    put32(xwd, bpp, header_big);
    put32(xwd, bytes_per_line, header_big);
    put32(xwd, 4, header_big);               /* TrueColor */
    put32(xwd, rmask, header_big);
    put32(xwd, gmask, header_big);
    put32(xwd, bmask, header_big);
    put32(xwd, 8, header_big);
    put32(xwd, 0, header_big);               /* colormap_entries */
    put32(xwd, 0, header_big);               /* ncolors */
    put32(xwd, width, header_big);
    put32(xwd, height, header_big);
    put32(xwd, 0, header_big);
    put32(xwd, 0, header_big);
    put32(xwd, 0, header_big);
    g_byte_array_append(xwd, (const guint8 *)name, sizeof(name));

    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
            guint32 p = pixels[y * width + x];
            guint b;

            for (b = 0; b < bpp / 8; b++) {
                guint shift = pixels_msb ? (bpp - 8 - 8 * b) : 8 * b;
                guint8 byte = (guint8)(p >> shift);

                g_byte_array_append(xwd, &byte, 1);
            }
        }
    return xwd;
}

/* Writes an executable script into @dir. */
static gchar *
write_script(const gchar *dir, const gchar *name, const gchar *body)
{
    gchar *path = g_build_filename(dir, name, NULL);
    g_autofree gchar *text = g_strconcat("#!/bin/sh\n", body, NULL);

    g_assert_true(g_file_set_contents(path, text, -1, NULL));
    g_assert_cmpint(g_chmod(path, 0755), ==, 0);
    return path;
}

static gboolean
wait_for_file(const gchar *path, guint ms)
{
    guint waited;

    for (waited = 0; waited < ms; waited += 20) {
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            return TRUE;
        g_usleep(20 * 1000);
    }
    return FALSE;
}

/* ── The PNG writer ──────────────────────────────────────────────── */

/*
 * Every picture of an X display goes through this writer, so a bug here
 * is a black or garbled Screen tab and a screenshot the agent misreads.
 */
static void
test_png_round_trip(void)
{
    static const guint8 rgb[] = {
        255, 0, 0,    0, 255, 0,    0, 0, 255,
        10, 20, 30,   40, 50, 60,   70, 80, 90
    };
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) png = clawt_display_png_encode(3, 2, rgb, &error);
    g_autoptr(GBytes) pixels = NULL;
    const guint8 *p;
    guint width = 0;
    guint height = 0;

    g_assert_no_error(error);
    g_assert_nonnull(png);
    g_assert_true(clawt_screen_png_size(png, &width, &height));
    g_assert_cmpuint(width, ==, 3);
    g_assert_cmpuint(height, ==, 2);

    pixels = png_pixels(png);
    g_assert_cmpuint(g_bytes_get_size(pixels), ==, 2 * (1 + 9));
    p = g_bytes_get_data(pixels, NULL);
    g_assert_cmpuint(p[0], ==, 0);                       /* filter: none */
    g_assert_cmpint(memcmp(p + 1, rgb, 9), ==, 0);
    g_assert_cmpuint(p[10], ==, 0);
    g_assert_cmpint(memcmp(p + 11, rgb + 9, 9), ==, 0);

    /* nothing to picture is refused, not an empty file */
    g_assert_null(clawt_display_png_encode(0, 2, rgb, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
}

/* ── The XWD reader ──────────────────────────────────────────────── */

/*
 * Xvfb at depth 24 keeps 32-bit pixels, in the server's byte order,
 * under a big-endian header.  Read wrongly, red and blue swap and every
 * screenshot the agent reasons about has the wrong colours.
 */
static void
test_xwd_32bpp(void)
{
    static const guint32 pixels[] = {
        0x00ff0000, 0x0000ff00, 0x000000ff, 0x00102030
    };
    gboolean orders[] = { FALSE, TRUE };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(orders); i++) {
        g_autoptr(GByteArray) xwd = make_xwd(2, 2, 32, TRUE, orders[i],
                                             0xff0000, 0x00ff00, 0x0000ff,
                                             pixels);
        g_autoptr(GError) error = NULL;
        g_autoptr(GBytes) png = clawt_display_xwd_to_png(xwd->data, xwd->len,
                                                         &error);
        g_autoptr(GBytes) out = NULL;
        const guint8 *p;
        static const guint8 want[] = {
            0, 255, 0, 0,  0, 255, 0,
            0, 0, 0, 255,  0x10, 0x20, 0x30
        };

        g_assert_no_error(error);
        out = png_pixels(png);
        p = g_bytes_get_data(out, NULL);
        g_assert_cmpuint(g_bytes_get_size(out), ==, sizeof(want));
        g_assert_cmpint(memcmp(p, want, sizeof(want)), ==, 0);
    }
}

/* A 16-bit 5-6-5 screen, and a header written little-endian. */
static void
test_xwd_16bpp(void)
{
    static const guint32 pixels[] = { 0xf800, 0x07e0, 0x001f, 0xffff };
    g_autoptr(GByteArray) xwd = make_xwd(4, 1, 16, FALSE, FALSE, 0xf800,
                                         0x07e0, 0x001f, pixels);
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) png = clawt_display_xwd_to_png(xwd->data, xwd->len,
                                                     &error);
    g_autoptr(GBytes) out = NULL;
    const guint8 *p;

    g_assert_no_error(error);
    out = png_pixels(png);
    p = g_bytes_get_data(out, NULL);
    g_assert_cmpuint(p[1], ==, 255);   /* red */
    g_assert_cmpuint(p[2], ==, 0);
    g_assert_cmpuint(p[5], ==, 255);   /* green */
    g_assert_cmpuint(p[9], ==, 255);   /* blue */
    g_assert_cmpuint(p[10], ==, 255);  /* white */
    g_assert_cmpuint(p[11], ==, 255);
    g_assert_cmpuint(p[12], ==, 255);
}

/*
 * What is not a framebuffer is refused with what it was, not read past
 * its end: the file is a mapping a dying server may be truncating.
 */
static void
test_xwd_refuses(void)
{
    static const guint32 pixels[] = { 0, 0, 0, 0 };
    g_autoptr(GByteArray) good = make_xwd(2, 2, 32, TRUE, FALSE, 0xff0000,
                                          0x00ff00, 0x0000ff, pixels);
    g_autoptr(GByteArray) xy = make_xwd(2, 2, 32, TRUE, FALSE, 0xff0000,
                                        0x00ff00, 0x0000ff, pixels);
    g_autoptr(GError) error = NULL;

    /* too short */
    g_assert_null(clawt_display_xwd_to_png(good->data, 40, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    /* cut off inside the pixels */
    g_assert_null(clawt_display_xwd_to_png(good->data, good->len - 3, &error));
    g_assert_nonnull(strstr(error->message, "shorter"));
    g_clear_error(&error);

    /* not version 7 */
    good->data[7] = 6;
    g_assert_null(clawt_display_xwd_to_png(good->data, good->len, &error));
    g_assert_nonnull(strstr(error->message, "not an XWD"));
    g_clear_error(&error);

    /* XYPixmap, which nothing here draws */
    xy->data[11] = 1;
    g_assert_null(clawt_display_xwd_to_png(xy->data, xy->len, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
}

/* ── argv ────────────────────────────────────────────────────────── */

static gchar *
joined(GStrv argv)
{
    return argv != NULL ? g_strjoinv(" ", argv) : g_strdup("(null)");
}

/*
 * The keys and clicks an agent and a person send to an X display.  A
 * click that did not move first would land wherever the pointer was
 * left; a modifier xdotool cannot read sends a stray letter instead.
 */
static void
test_xdotool_argv(void)
{
    static const struct {
        ClawtInputKind kind;
        const gchar   *text;
        gint           x, y;
        guint          button;
        gdouble        dx, dy;
        const gchar   *want;
    } cases[] = {
        { CLAWT_INPUT_KEY, "Ctrl+Shift+t", 0, 0, 0, 0, 0,
          "xd key --clearmodifiers ctrl+shift+t" },
        { CLAWT_INPUT_KEY, "Return", 0, 0, 0, 0, 0,
          "xd key --clearmodifiers Return" },
        { CLAWT_INPUT_TEXT, "-rf not an option", 0, 0, 0, 0, 0,
          "xd type --delay 12 -- -rf not an option" },
        { CLAWT_INPUT_CLICK, NULL, 10, 20, 3, 0, 0,
          "xd mousemove --sync 10 20 click 3" },
        { CLAWT_INPUT_CLICK, NULL, 10, 20, 9, 0, 0,
          "xd mousemove --sync 10 20 click 1" },
        { CLAWT_INPUT_MOVE, NULL, -5, 7, 0, 0, 0,
          "xd mousemove --sync 0 7" },
        { CLAWT_INPUT_SCROLL, NULL, 1, 2, 0, 0, 3,
          "xd mousemove --sync 1 2 click --repeat 3 5" },
        { CLAWT_INPUT_SCROLL, NULL, 1, 2, 0, 0, -1,
          "xd mousemove --sync 1 2 click --repeat 1 4" },
        { CLAWT_INPUT_SCROLL, NULL, 1, 2, 0, -2, 0.5,
          "xd mousemove --sync 1 2 click --repeat 2 6" },
        { CLAWT_INPUT_SCROLL, NULL, 1, 2, 0, 0, 0,
          "xd mousemove --sync 1 2 click --repeat 1 5" },
    };
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autoptr(ClawtInputEvent) event =
            clawt_input_event_new(cases[i].kind);
        g_auto(GStrv) argv = NULL;
        g_autofree gchar *line = NULL;

        clawt_input_event_set_text(event, cases[i].text);
        event->x = cases[i].x;
        event->y = cases[i].y;
        event->button = cases[i].button;
        event->dx = cases[i].dx;
        event->dy = cases[i].dy;
        argv = clawt_display_xdotool_argv("xd", event);
        line = joined(argv);
        g_assert_cmpstr(line, ==, cases[i].want);
    }

    /* a key or text with nothing in it sends nothing */
    {
        g_autoptr(ClawtInputEvent) empty =
            clawt_input_event_new(CLAWT_INPUT_TEXT);

        g_assert_null(clawt_display_xdotool_argv("xd", empty));
    }
}

/*
 * The servers' command lines.  The properties that matter are the safe
 * ones: no X server listens on TCP, Xvnc's VNC is loopback only, Xvfb
 * keeps its framebuffer where the reader looks, and gowl loads none of
 * the operator's own C config.
 */
static void
test_server_argv(void)
{
    g_auto(GStrv) xvfb = clawt_display_server_argv(
        CLAWT_DISPLAY_BACKEND_XVFB, "/x/Xvfb", 1280, 800, 24, "/d/fb", 0,
        NULL);
    g_auto(GStrv) xvnc = clawt_display_server_argv(
        CLAWT_DISPLAY_BACKEND_XVNC, "/x/Xvnc", 1024, 768, 16, NULL, 5911,
        NULL);
    g_auto(GStrv) gowl = clawt_display_server_argv(
        CLAWT_DISPLAY_BACKEND_GOWL, "/x/gowl", 1280, 800, 24, NULL, 0,
        "/d/gowl.yaml");
    g_auto(GStrv) import = clawt_display_import_argv("/x/import", ":4");
    g_autofree gchar *a = joined(xvfb);
    g_autofree gchar *b = joined(xvnc);
    g_autofree gchar *c = joined(gowl);
    g_autofree gchar *d = joined(import);

    g_assert_cmpstr(a, ==, "/x/Xvfb -displayfd 1 -screen 0 1280x800x24 "
                           "-nolisten tcp -fbdir /d/fb");
    g_assert_cmpstr(b, ==, "/x/Xvnc -displayfd 1 -geometry 1024x768 -depth 16 "
                           "-rfbport 5911 -localhost -SecurityTypes None "
                           "-AlwaysShared -nolisten tcp");
    g_assert_cmpstr(c, ==, "/x/gowl --config /d/gowl.yaml --no-c-config");
    g_assert_cmpstr(d, ==, "/x/import -display :4 -window root png:-");
}

/*
 * gowl is started apart from the operator's session: its own runtime
 * directory (or it would replace their gowl.sock), headless, software
 * rendered, no systemd -- and with nothing inherited that names a real
 * screen.
 */
static void
test_gowl_config_and_environment(void)
{
    g_autofree gchar *yaml = clawt_display_gowl_config();
    g_autoptr(GHashTable) env = clawt_display_gowl_environment("/r/run",
                                                               800, 600);

    g_assert_nonnull(strstr(yaml, "  ipc:\n    enabled: true"));
    g_assert_nonnull(strstr(yaml, "  mcp:\n    enabled: true"));
    g_assert_nonnull(strstr(yaml, "transport-http: false"));

    g_assert_cmpstr(g_hash_table_lookup(env, "XDG_RUNTIME_DIR"), ==,
                    "/r/run");
    g_assert_cmpstr(g_hash_table_lookup(env, "WLR_BACKENDS"), ==, "headless");
    g_assert_cmpstr(g_hash_table_lookup(env, "WLR_RENDERER"), ==, "pixman");
    g_assert_cmpstr(g_hash_table_lookup(env, "GOWL_OUTPUT_SIZE"), ==,
                    "800x600");
    g_assert_cmpstr(g_hash_table_lookup(env, "GOWL_DISABLE_SYSTEMD"), ==,
                    "1");
    g_assert_cmpstr(g_hash_table_lookup(env, "WAYLAND_DISPLAY"), ==, "");
    g_assert_cmpstr(g_hash_table_lookup(env, "DISPLAY"), ==, "");
}

static void
test_parse_resolution(void)
{
    guint w = 0;
    guint h = 0;

    g_assert_true(clawt_display_parse_resolution("1920x1080", &w, &h));
    g_assert_cmpuint(w, ==, 1920);
    g_assert_cmpuint(h, ==, 1080);
    g_assert_true(clawt_display_parse_resolution("640X480", &w, &h));
    g_assert_false(clawt_display_parse_resolution("1920", &w, &h));
    g_assert_false(clawt_display_parse_resolution("1920x", &w, &h));
    g_assert_false(clawt_display_parse_resolution("10x10", &w, &h));
    g_assert_false(clawt_display_parse_resolution("9000x100", &w, &h));
    g_assert_false(clawt_display_parse_resolution("800x600px", &w, &h));
    g_assert_false(clawt_display_parse_resolution(NULL, &w, &h));
}

/* ── Choosing a server ───────────────────────────────────────────── */

/*
 * The case this resolver exists for, on the machine it was written on:
 * a gowl first on PATH that cannot load its wlroots, and a working one
 * after it.  The broken one is skipped and named; the working one is
 * used.  Taking the first on PATH would be no display at all.
 */
static void
test_resolve_skips_a_broken_gowl(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-display-XXXXXX", NULL);
    g_autofree gchar *broken = g_build_filename(root, "broken", NULL);
    g_autofree gchar *good = g_build_filename(root, "good", NULL);
    g_autofree gchar *b = NULL;
    g_autofree gchar *g = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *saved = g_strdup(g_getenv("PATH"));
    g_autofree gchar *binary = NULL;
    g_autofree gchar *notes = NULL;
    ClawtDisplayBackend chosen = CLAWT_DISPLAY_BACKEND_AUTO;
    g_autoptr(GError) error = NULL;

    g_assert_cmpint(g_mkdir_with_parents(broken, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(good, 0700), ==, 0);
    b = write_script(broken, "gowl",
                     "echo 'gowl: error while loading shared libraries: "
                     "libwlroots-0.19.so' >&2\nexit 127\n");
    g = write_script(good, "gowl", "echo 'gowl 0.5.0'\n");

    path = g_strdup_printf("%s:%s", broken, good);
    g_setenv("PATH", path, TRUE);
    g_assert_true(clawt_display_resolve(CLAWT_DISPLAY_BACKEND_GOWL, NULL,
                                        &chosen, &binary, &notes, &error));
    g_setenv("PATH", saved, TRUE);

    g_assert_no_error(error);
    g_assert_cmpint(chosen, ==, CLAWT_DISPLAY_BACKEND_GOWL);
    g_assert_cmpstr(binary, ==, g);
    g_assert_nonnull(notes);
    g_assert_nonnull(strstr(notes, b));
    g_assert_nonnull(strstr(notes, "libwlroots-0.19.so"));

    clawt_test_remove_tree(root);
}

/*
 * A pinned gowl that does not run is refused with its own words, and a
 * pinned one that does is used whatever PATH holds.
 */
static void
test_resolve_pinned_gowl(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-display-XXXXXX", NULL);
    g_autofree gchar *bad = write_script(root, "bad-gowl",
                                         "echo 'no wlroots here' >&2\n"
                                         "exit 1\n");
    g_autofree gchar *fine = write_script(root, "fine-gowl",
                                          "echo 'gowl 0.5.0'\n");
    g_autofree gchar *binary = NULL;
    g_autofree gchar *notes = NULL;
    ClawtDisplayBackend chosen = CLAWT_DISPLAY_BACKEND_AUTO;
    g_autoptr(GError) error = NULL;

    g_assert_true(clawt_display_resolve(CLAWT_DISPLAY_BACKEND_GOWL, fine,
                                        &chosen, &binary, &notes, &error));
    g_assert_cmpstr(binary, ==, fine);
    g_clear_pointer(&binary, g_free);
    g_clear_pointer(&notes, g_free);

    g_assert_false(clawt_display_resolve(CLAWT_DISPLAY_BACKEND_GOWL, bad,
                                         &chosen, &binary, &notes, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(error->message, "no wlroots here"));
    g_assert_nonnull(strstr(error->message, "gowl_binary"));
    g_assert_null(binary);

    clawt_test_remove_tree(root);
}

/* auto takes gowl, then Xvnc, then Xvfb: the first that is there. */
static void
test_resolve_auto_order(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-display-XXXXXX", NULL);
    g_autofree gchar *x = write_script(root, "Xvnc", "exit 0\n");
    g_autofree gchar *bad = write_script(root, "badgowl", "exit 1\n");
    g_autofree gchar *binary = NULL;
    ClawtDisplayBackend chosen = CLAWT_DISPLAY_BACKEND_AUTO;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *saved = g_strdup(g_getenv("PATH"));

    /*
     * A pinned gowl that fails stands for "no gowl" without depending on
     * whether this machine has one; the first X server on PATH -- the
     * stand-in Xvnc -- comes next.
     */
    g_setenv("PATH", root, TRUE);
    g_assert_true(clawt_display_resolve(CLAWT_DISPLAY_BACKEND_AUTO, bad,
                                        &chosen, &binary, NULL, &error));
    g_setenv("PATH", saved, TRUE);
    g_assert_no_error(error);
    g_assert_cmpint(chosen, ==, CLAWT_DISPLAY_BACKEND_XVNC);
    g_assert_cmpstr(binary, ==, x);

    clawt_test_remove_tree(root);
}

/* ── A stand-in X server ─────────────────────────────────────────── */

/*
 * Behaves as Xvfb does where it matters: reports display 77 on stdout
 * (-displayfd 1), writes its framebuffer into -fbdir, and stays up --
 * or, written with @die, exits after reporting with a reason on stderr.
 * Its settings are in its own text: the server is started with the
 * allowlisted environment, so a variable set here would never reach it.
 */
static gchar *
fake_xvfb_script(const gchar *frame, gboolean die)
{
    return g_strdup_printf(
        "fbdir=''\n"
        "while [ $# -gt 0 ]; do\n"
        "  case \"$1\" in -fbdir) fbdir=\"$2\"; shift;; esac\n"
        "  shift\n"
        "done\n"
        "[ -n \"$fbdir\" ] && cp '%s' \"$fbdir/Xvfb_screen0\"\n"
        "echo 77\n"
        "%s"
        "trap 'exit 0' TERM INT\n"
        "while :; do sleep 1 & wait $!; done\n",
        frame, die ? "echo 'fake X: no screens found' >&2; exit 1\n" : "");
}

typedef struct {
    gchar *root;
    gchar *bin;
    gchar *frame;
    gchar *saved_path;
} FakeX;

static void
fake_x_setup_full(FakeX *fake, gboolean die)
{
    static const guint32 pixels[] = {
        0x00ff0000, 0x0000ff00, 0x000000ff, 0x00ffffff
    };
    g_autoptr(GByteArray) xwd = make_xwd(2, 2, 32, TRUE, FALSE, 0xff0000,
                                         0x00ff00, 0x0000ff, pixels);
    g_autofree gchar *path = NULL;
    g_autofree gchar *script = NULL;

    fake->root = g_dir_make_tmp("clawt-display-XXXXXX", NULL);
    fake->bin = g_build_filename(fake->root, "bin", NULL);
    g_assert_cmpint(g_mkdir_with_parents(fake->bin, 0700), ==, 0);
    fake->frame = g_build_filename(fake->root, "frame.xwd", NULL);
    g_assert_true(g_file_set_contents(fake->frame, (const gchar *)xwd->data,
                                      xwd->len, NULL));
    {
        g_autofree gchar *body = fake_xvfb_script(fake->frame, die);

        script = write_script(fake->bin, "Xvfb", body);
    }

    /* the stand-in first, the system after it for sh, cp and sleep */
    fake->saved_path = g_strdup(g_getenv("PATH"));
    path = g_strdup_printf("%s:%s", fake->bin, fake->saved_path);
    g_setenv("PATH", path, TRUE);
}

static void
fake_x_setup(FakeX *fake)
{
    fake_x_setup_full(fake, FALSE);
}

static void
fake_x_teardown(FakeX *fake)
{
    g_setenv("PATH", fake->saved_path, TRUE);
    clawt_test_remove_tree(fake->root);
    g_free(fake->root);
    g_free(fake->bin);
    g_free(fake->frame);
    g_free(fake->saved_path);
}

/*
 * Start, picture, environment, stop -- the whole life of a display on
 * the stand-in.  After a stop nothing the server made is left: the
 * framebuffer directory is the one an agent's next screen would reuse.
 */
static void
test_server_lifecycle(void)
{
    FakeX fake = { 0 };
    g_autofree gchar *dir = NULL;
    g_autofree gchar *fb = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) png = NULL;
    g_autoptr(GHashTable) env = NULL;
    g_auto(GStrv) binds = NULL;
    guint w = 0;
    guint h = 0;

    fake_x_setup(&fake);
    dir = g_build_filename(fake.root, "display", NULL);
    fb = g_build_filename(dir, "fb", NULL);
    server = clawt_display_server_new(CLAWT_DISPLAY_BACKEND_XVFB, dir, 640,
                                      480);

    g_assert_false(clawt_display_server_is_running(server));
    g_assert_null(clawt_display_server_capture(server, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED);
    g_clear_error(&error);

    g_assert_true(clawt_display_server_start(server, &error));
    g_assert_no_error(error);
    g_assert_true(clawt_display_server_is_running(server));
    g_assert_cmpint(clawt_display_server_get_backend(server), ==,
                    CLAWT_DISPLAY_BACKEND_XVFB);

    /* starting again is not a second server */
    g_assert_true(clawt_display_server_start(server, &error));

    env = clawt_display_server_dup_environment(server);
    g_assert_cmpstr(g_hash_table_lookup(env, "DISPLAY"), ==, ":77");
    g_assert_cmpstr(g_hash_table_lookup(env, "GDK_BACKEND"), ==, "x11");
    binds = clawt_display_server_dup_bind_paths(server);
    /*
     * The socket alone (0.51.4).  The directory would hand a bwrap box
     * every X server on the machine, the operator's own included.
     */
    g_assert_cmpuint(g_strv_length(binds), ==, 1);
    g_assert_cmpstr(binds[0], ==, "/tmp/.X11-unix/X77");

    /* the picture is the framebuffer the server wrote */
    png = clawt_display_server_capture(server, &error);
    g_assert_no_error(error);
    g_assert_true(clawt_screen_png_size(png, &w, &h));
    g_assert_cmpuint(w, ==, 2);
    g_assert_cmpuint(h, ==, 2);

    /* no VNC behind an Xvfb */
    g_assert_null(clawt_display_server_dup_viewer_uri(server));

    clawt_display_server_stop(server);
    g_assert_false(clawt_display_server_is_running(server));
    g_assert_false(g_file_test(fb, G_FILE_TEST_EXISTS));
    g_clear_pointer(&env, g_hash_table_unref);
    env = clawt_display_server_dup_environment(server);
    g_assert_cmpuint(g_hash_table_size(env), ==, 0);

    fake_x_teardown(&fake);
}

/*
 * A server that dies says why: the last thing it wrote reaches the
 * person, rather than "the display is not running" and nothing else.
 */
static void
test_server_death_is_explained(void)
{
    FakeX fake = { 0 };
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *failure = NULL;
    guint waited;

    fake_x_setup_full(&fake, TRUE);
    dir = g_build_filename(fake.root, "display", NULL);
    server = clawt_display_server_new(CLAWT_DISPLAY_BACKEND_XVFB, dir, 640,
                                      480);

    g_assert_true(clawt_display_server_start(server, &error));
    for (waited = 0; waited < 3000 &&
         clawt_display_server_is_running(server); waited += 20)
        g_usleep(20 * 1000);
    g_assert_false(clawt_display_server_is_running(server));

    failure = clawt_display_server_dup_failure(server);
    g_assert_nonnull(failure);
    g_assert_nonnull(strstr(failure, "no screens found"));

    fake_x_teardown(&fake);
}

/*
 * An app launched on the display gets the screen's DISPLAY and none of
 * the daemon's own environment, and stops with the server.
 */
/*
 * The unconfined daemon opens the server's and the apps' logs (0.51.4):
 * a symlink where a log goes is refused, not followed -- following it
 * wrote output an agent chose wherever the link pointed, as the
 * operator.  The target is left exactly as it was.
 */
static void
test_logs_refuse_symlinks(void)
{
    FakeX fake = { 0 };
    g_autofree gchar *dir = NULL;
    g_autofree gchar *target = NULL;
    g_autofree gchar *server_log = NULL;
    g_autofree gchar *apps_log = NULL;
    g_autofree gchar *after = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;
    g_autoptr(GError) error = NULL;

    fake_x_setup(&fake);
    dir = g_build_filename(fake.root, "display", NULL);
    target = g_build_filename(fake.root, "victim", NULL);
    server_log = g_build_filename(dir, "server.log", NULL);
    apps_log = g_build_filename(dir, "apps.log", NULL);
    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(target, "keep\n", -1, NULL));
    server = clawt_display_server_new(CLAWT_DISPLAY_BACKEND_XVFB, dir, 640,
                                      480);

    /* the server's log */
    g_assert_cmpint(symlink(target, server_log), ==, 0);
    g_assert_false(clawt_display_server_start(server, &error));
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "symlink"));
    g_clear_error(&error);
    g_assert_cmpint(unlink(server_log), ==, 0);

    /* an app's log, the one an agent's own command writes into */
    g_assert_true(clawt_display_server_start(server, &error));
    g_assert_no_error(error);
    g_assert_cmpint(symlink(target, apps_log), ==, 0);
    g_assert_false(clawt_display_server_launch(server, "echo overwritten",
                                               NULL, NULL, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);

    g_assert_true(g_file_get_contents(target, &after, NULL, NULL));
    g_assert_cmpstr(after, ==, "keep\n");

    clawt_display_server_stop(server);
    fake_x_teardown(&fake);
}

static void
test_server_launch(void)
{
    FakeX fake = { 0 };
    g_autofree gchar *dir = NULL;
    g_autofree gchar *out = NULL;
    g_autofree gchar *command = NULL;
    g_autofree gchar *seen = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;
    g_autoptr(GError) error = NULL;
    gint pid = 0;

    fake_x_setup(&fake);
    g_setenv("CLAWT_TEST_DAEMON_SECRET", "do-not-leak", TRUE);
    dir = g_build_filename(fake.root, "display", NULL);
    out = g_build_filename(fake.root, "seen", NULL);
    server = clawt_display_server_new(CLAWT_DISPLAY_BACKEND_XVFB, dir, 640,
                                      480);

    /* not running: refused rather than started somewhere else */
    g_assert_false(clawt_display_server_launch(server, "true", NULL, NULL,
                                               NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED);
    g_clear_error(&error);

    g_assert_true(clawt_display_server_start(server, &error));
    command = g_strdup_printf("echo \"$DISPLAY|$CLAWT_TEST_DAEMON_SECRET|\" "
                              "> '%s.tmp' && mv '%s.tmp' '%s'; "
                              "sleep 30", out, out, out);
    g_assert_true(clawt_display_server_launch(server, command, NULL, NULL,
                                              &pid, &error));
    g_assert_no_error(error);
    g_assert_cmpint(pid, >, 0);
    g_assert_true(wait_for_file(out, 5000));
    g_assert_true(g_file_get_contents(out, &seen, NULL, NULL));
    g_assert_cmpstr(seen, ==, ":77||\n");

    /* the app stops with the server, not left drawing on nothing */
    clawt_display_server_stop(server);
    g_assert_cmpint(kill(pid, 0) == 0 ? 1 : 0, ==, 0);

    /* an empty command is refused */
    g_assert_false(clawt_display_server_launch(server, "", NULL, NULL, NULL,
                                               &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);

    g_unsetenv("CLAWT_TEST_DAEMON_SECRET");
    fake_x_teardown(&fake);
}

/* ── The computer ────────────────────────────────────────────────── */

static ClawtComputer *
fake_display_computer(FakeX *fake, ClawtDisplayServer **server_out)
{
    g_autofree gchar *dir = g_build_filename(fake->root, "display", NULL);
    g_autofree gchar *work = g_build_filename(fake->root, "work", NULL);
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) host = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;

    g_assert_cmpint(g_mkdir_with_parents(work, 0700), ==, 0);
    sandbox = clawt_sandbox_new(CLAWT_CONFINE_NONE, work);
    host = clawt_host_computer_new("painter", sandbox);
    server = clawt_display_server_new(CLAWT_DISPLAY_BACKEND_XVFB, dir, 640,
                                      480);
    if (server_out != NULL)
        *server_out = g_object_ref(server);
    return clawt_display_computer_new("painter",
                                      CLAWT_HOST_COMPUTER(host), server);
}

/*
 * Under bwrap the computer's box gets the screen's socket and nothing
 * around it (0.51.4): not /tmp/.X11-unix, which holds the operator's own
 * X server too.  A second start binds it once, not twice, and a stop and
 * start re-takes it rather than keeping the old one beside it.
 */
static void
test_computer_binds_the_socket_alone(void)
{
    FakeX fake = { 0 };
    g_autofree gchar *dir = NULL;
    g_autofree gchar *work = NULL;
    g_autoptr(ClawtSandbox) sandbox = NULL;
    g_autoptr(ClawtComputer) host = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[] = { "true", NULL };
    guint round;

    fake_x_setup(&fake);
    dir = g_build_filename(fake.root, "display", NULL);
    work = g_build_filename(fake.root, "work", NULL);
    g_assert_cmpint(g_mkdir_with_parents(work, 0700), ==, 0);
    sandbox = clawt_sandbox_new(CLAWT_CONFINE_BWRAP, work);
    host = clawt_host_computer_new("painter", sandbox);
    server = clawt_display_server_new(CLAWT_DISPLAY_BACKEND_XVFB, dir, 640,
                                      480);
    computer = clawt_display_computer_new("painter",
                                          CLAWT_HOST_COMPUTER(host), server);

    for (round = 0; round < 2; round++) {
        g_auto(GStrv) wrapped = NULL;
        guint binds = 0;
        guint i;

        g_assert_true(clawt_computer_start(computer, &error));
        g_assert_no_error(error);
        /* a second start of a running computer */
        g_assert_true(clawt_computer_start(computer, &error));
        g_assert_no_error(error);

        wrapped = clawt_sandbox_wrap_argv(sandbox, argv);
        for (i = 0; wrapped[i] != NULL; i++) {
            g_assert_cmpstr(wrapped[i], !=, "/tmp/.X11-unix");
            g_assert_false(g_str_has_prefix(wrapped[i], dir));
            if (g_strcmp0(wrapped[i], "--bind") == 0 &&
                g_strcmp0(wrapped[i + 1], "/tmp/.X11-unix/X77") == 0)
                binds++;
        }
        g_assert_cmpuint(binds, ==, 1);

        g_assert_true(clawt_computer_stop(computer, &error));
        g_assert_no_error(error);
    }

    fake_x_teardown(&fake);
}

/*
 * The computer's commands see the screen, and the apps asked for at
 * start are started once -- not again on a second start of a running
 * computer.
 */
static void
test_computer_commands_and_startup(void)
{
    FakeX fake = { 0 };
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;
    g_autoptr(ClawtExecResult) result = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *marker = NULL;
    g_autofree gchar *startup = NULL;
    g_autofree gchar *count = NULL;
    const gchar *argv[] = { "sh", "-c", "printf '%s' \"$DISPLAY\"", NULL };
    const gchar *apps[2] = { NULL, NULL };

    fake_x_setup(&fake);
    computer = fake_display_computer(&fake, &server);
    marker = g_build_filename(fake.root, "started", NULL);
    startup = g_strdup_printf("echo once >> '%s'", marker);
    apps[0] = startup;
    clawt_display_computer_set_startup(CLAWT_DISPLAY_COMPUTER(computer), apps);

    g_assert_cmpint(clawt_computer_get_computer_type(computer), ==,
                    CLAWT_COMPUTER_DISPLAY);
    g_assert_true(clawt_computer_start(computer, &error));
    g_assert_no_error(error);
    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_RUNNING);

    result = clawt_computer_exec(computer, argv, NULL, 10, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(clawt_exec_result_get_stdout(result), ==, ":77");

    g_assert_true(wait_for_file(marker, 5000));
    g_usleep(200 * 1000);
    g_assert_true(g_file_get_contents(marker, &count, NULL, NULL));
    g_assert_cmpstr(count, ==, "once\n");

    g_assert_true(clawt_computer_stop(computer, &error));
    g_assert_false(clawt_display_server_is_running(server));
    g_assert_cmpint(clawt_computer_get_state(computer), ==,
                    CLAWT_COMPUTER_STATE_STOPPED);

    fake_x_teardown(&fake);
}

/*
 * The Screen tab's contract: a frame, the same frame reported as
 * unchanged (NULL with no error, so nothing is redrawn), and the size.
 */
static void
test_computer_is_observable(void)
{
    FakeX fake = { 0 };
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) first = NULL;
    g_autoptr(GBytes) again = NULL;
    g_autofree gchar *hash = NULL;
    g_autofree gchar *hash2 = NULL;
    gint64 stamp = 0;
    guint w = 0;
    guint h = 0;

    fake_x_setup(&fake);
    computer = fake_display_computer(&fake, NULL);
    g_assert_true(CLAWT_IS_OBSERVABLE(computer));
    g_assert_true(clawt_computer_start(computer, &error));

    g_assert_true(clawt_observable_start(CLAWT_OBSERVABLE(computer), 2,
                                         &error));
    first = clawt_observable_frame(CLAWT_OBSERVABLE(computer), NULL, &stamp,
                                   &hash, &error);
    g_assert_no_error(error);
    g_assert_nonnull(first);
    g_assert_nonnull(hash);
    g_assert_cmpint(stamp, >, 0);

    again = clawt_observable_frame(CLAWT_OBSERVABLE(computer), hash, NULL,
                                   &hash2, &error);
    g_assert_no_error(error);
    g_assert_null(again);
    g_assert_cmpstr(hash2, ==, hash);

    g_assert_true(clawt_observable_geometry(CLAWT_OBSERVABLE(computer), &w,
                                            &h));
    g_assert_cmpuint(w, ==, 640);
    g_assert_cmpuint(h, ==, 480);
    g_assert_true(clawt_observable_can_input(CLAWT_OBSERVABLE(computer)));

    g_assert_true(clawt_computer_stop(computer, &error));
    g_assert_false(clawt_observable_can_input(CLAWT_OBSERVABLE(computer)));

    fake_x_teardown(&fake);
}

/*
 * The shared action used by the agent's tools and computer.act: a
 * screenshot on a computer that starts on first use brings the display
 * up; a bad action and empty text are refused with what was wrong.
 */
static void
test_perform(void)
{
    FakeX fake = { 0 };
    g_autoptr(ClawtComputer) computer = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) png = NULL;
    g_autoptr(JsonNode) shot = NULL;
    g_autoptr(JsonNode) none = NULL;
    g_autoptr(JsonObject) empty = json_object_new();

    fake_x_setup(&fake);
    computer = fake_display_computer(&fake, NULL);
    g_assert_true(clawt_computer_start(computer, &error));

    shot = clawt_display_computer_perform(CLAWT_DISPLAY_COMPUTER(computer),
                                          "screenshot", NULL, &png, &error);
    g_assert_no_error(error);
    g_assert_nonnull(png);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(shot),
                                               "width"), ==, 2);

    none = clawt_display_computer_perform(CLAWT_DISPLAY_COMPUTER(computer),
                                          "dance", NULL, NULL, &error);
    g_assert_null(none);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    g_assert_null(clawt_display_computer_perform(
        CLAWT_DISPLAY_COMPUTER(computer), "type", empty, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    g_assert_null(clawt_display_computer_perform(
        CLAWT_DISPLAY_COMPUTER(computer), "launch", empty, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    g_assert_true(clawt_display_computer_action_is_acting("click"));
    g_assert_true(clawt_display_computer_action_is_acting("launch"));
    g_assert_false(clawt_display_computer_action_is_acting("screenshot"));
    g_assert_false(clawt_display_computer_action_is_acting("windows"));
    g_assert_false(clawt_display_computer_action_is_acting(NULL));

    clawt_computer_stop(computer, NULL);
    fake_x_teardown(&fake);
}

/* What the agent is told, per backend: tools named, the takeover said. */
static void
test_describe(void)
{
    g_autofree gchar *gowl = clawt_display_computer_describe_display(
        CLAWT_DISPLAY_BACKEND_GOWL, 1280, 800, NULL);
    g_autofree gchar *vnc = clawt_display_computer_describe_display(
        CLAWT_DISPLAY_BACKEND_XVNC, 1024, 768, "vnc://127.0.0.1:5900");
    g_autofree gchar *xvfb = clawt_display_computer_describe_display(
        CLAWT_DISPLAY_BACKEND_XVFB, 800, 600, NULL);

    g_assert_nonnull(strstr(gowl, "1280x800"));
    g_assert_nonnull(strstr(gowl, "clawtilla-desktop"));
    g_assert_nonnull(strstr(gowl, "clawtilla_display_screenshot"));
    g_assert_nonnull(strstr(vnc, "VNC viewer"));
    g_assert_null(strstr(xvfb, "clawtilla-desktop"));
    g_assert_nonnull(strstr(xvfb, "take it"));
}

/*
 * The sentence all three clients show under a display: which server,
 * up or not, why it died, what auto skipped -- and nothing at all for a
 * computer that is not a display.
 */
static void
test_status_line(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *running = NULL;
    g_autofree gchar *dead = NULL;
    g_autofree gchar *fresh = NULL;
    g_autofree gchar *host = NULL;
    JsonObject *o;

    g_assert_true(json_parser_load_from_data(parser,
        "{\"display_backend\":\"gowl\",\"display_running\":true,"
        "\"display_notes\":\"/usr/local/bin/gowl does not run: x\\n"
        "Xvnc: not installed\"}", -1, NULL));
    o = json_node_get_object(json_parser_get_root(parser));
    running = clawt_display_status_line(o);
    g_assert_cmpstr(running, ==, "Virtual display: gowl, running. Skipped: "
                    "/usr/local/bin/gowl does not run: x; Xvnc: not "
                    "installed");

    g_assert_true(json_parser_load_from_data(parser,
        "{\"display_backend\":\"xvfb\",\"display_running\":false,"
        "\"display_failure\":\"line one\\nno screens found\"}", -1, NULL));
    o = json_node_get_object(json_parser_get_root(parser));
    dead = clawt_display_status_line(o);
    g_assert_cmpstr(dead, ==, "Virtual display: xvfb, stopped. The server "
                    "exited: line one / no screens found");

    g_assert_true(json_parser_load_from_data(parser,
        "{\"display_backend\":\"auto\",\"display_running\":false}", -1,
        NULL));
    o = json_node_get_object(json_parser_get_root(parser));
    fresh = clawt_display_status_line(o);
    g_assert_cmpstr(fresh, ==, "Virtual display: not started yet.");

    g_assert_true(json_parser_load_from_data(parser,
        "{\"type\":\"host\"}", -1, NULL));
    host = clawt_display_status_line(
        json_node_get_object(json_parser_get_root(parser)));
    g_assert_null(host);
    g_assert_null(clawt_display_status_line(NULL));
}

/* ── The agent's tools ───────────────────────────────────────────── */

typedef struct {
    FakeX              fake;
    ClawtConfig       *config;
    ClawtAgentManager *agents;
    ClawtTaskManager  *tasks;
    ClawtLoopGuard    *guard;
    ClawtMcpTools     *tools;
    ClawtTakeover     *takeover;
    GMainContext      *context;
} ToolsFixture;

static void
tools_setup(ToolsFixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    const gchar *display =
        "    computer:\n      type: display\n"
        "      host:\n        confirm_host_control: true\n";

    fake_x_setup(&fixture->fake);
    yaml = g_strdup_printf(
        "daemon:\n  state_dir: \"%s/state\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n"
        "  - id: painter\n%s"
        "  - id: looker\n%s        \n      display:\n"
        "        allow_input: false\n        allow_spawn: false\n"
        "  - id: plain\n",
        fixture->fake.root, fixture->fake.root, display, display);
    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    fixture->agents = clawt_agent_manager_new(fixture->config);
    clawt_agent_manager_load(fixture->agents, NULL);
    fixture->tasks = clawt_task_manager_new();
    fixture->guard = clawt_loop_guard_new();
    fixture->context = g_main_context_new();
    fixture->tools = clawt_mcp_tools_new(fixture->agents, fixture->tasks,
                                         fixture->guard);
    clawt_mcp_tools_set_main_context(fixture->tools, fixture->context);
    fixture->takeover = clawt_takeover_new();
    clawt_mcp_tools_set_takeover(fixture->tools, fixture->takeover);

    /* painter's computer: the stand-in display, started */
    computer = fake_display_computer(&fixture->fake, NULL);
    g_assert_true(clawt_computer_start(computer, &error));
    clawt_agent_set_computer(clawt_agent_manager_get(fixture->agents,
                                                     "painter"), computer);
}

static void
tools_teardown(ToolsFixture *fixture)
{
    ClawtAgent *painter = clawt_agent_manager_get(fixture->agents, "painter");
    ClawtComputer *computer = clawt_agent_get_computer(painter);

    if (computer != NULL)
        clawt_computer_stop(computer, NULL);
    g_clear_object(&fixture->tools);
    g_clear_object(&fixture->takeover);
    g_clear_object(&fixture->guard);
    g_clear_object(&fixture->tasks);
    g_clear_object(&fixture->agents);
    g_clear_object(&fixture->config);
    g_clear_pointer(&fixture->context, g_main_context_unref);
    fake_x_teardown(&fixture->fake);
}

typedef struct {
    gboolean  arrived;
    JsonNode *response;
} Probe;

static void
on_tool_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Probe *probe = user_data;

    probe->response = clawt_mcp_tools_call_finish(CLAWT_MCP_TOOLS(source),
                                                  result);
    probe->arrived = TRUE;
}

/* One tool through the deferred path, as an agent's call arrives. */
static JsonNode *
run_display_tool(ToolsFixture *fixture, const gchar *agent_id,
                 const gchar *tool, const gchar *arguments)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool,
        arguments);
    Probe probe = { FALSE, NULL };
    gint64 deadline;

    g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
    g_assert_true(clawt_mcp_tools_call_defers(fixture->tools, agent_id,
                                              json_parser_get_root(parser)));
    clawt_mcp_tools_call_async(fixture->tools, agent_id, NULL,
                               json_parser_get_root(parser), on_tool_done,
                               &probe);
    deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (!probe.arrived && g_get_monotonic_time() < deadline)
        g_main_context_iteration(fixture->context, FALSE);
    g_assert_true(probe.arrived);
    return probe.response;
}

/* The result's text and whether it was an error; the image, if any. */
static const gchar *
tool_text(JsonNode *response, gboolean *is_error, gboolean *has_image)
{
    JsonObject *result = json_object_get_object_member(
        json_node_get_object(response), "result");
    JsonArray *content = json_object_get_array_member(result, "content");
    const gchar *text = NULL;
    guint i;

    *is_error = json_object_get_boolean_member_with_default(result,
                                                            "isError", FALSE);
    if (has_image != NULL)
        *has_image = FALSE;
    for (i = 0; i < json_array_get_length(content); i++) {
        JsonObject *item = json_array_get_object_element(content, i);
        const gchar *type = json_object_get_string_member(item, "type");

        if (g_strcmp0(type, "text") == 0)
            text = json_object_get_string_member(item, "text");
        if (g_strcmp0(type, "image") == 0 && has_image != NULL)
            *has_image = TRUE;
    }
    return text;
}

/*
 * Which tools each agent is offered: all of them on a display computer
 * with the default grants; looking only without allow_input and
 * allow_spawn; none at all without a display.  A tool an agent could
 * only be refused is one it would keep calling.
 */
static void
test_tools_follow_the_grants(void)
{
    ToolsFixture fixture = { 0 };
    static const gchar *const acting[] = {
        "clawtilla_display_click", "clawtilla_display_move",
        "clawtilla_display_type", "clawtilla_display_key",
        "clawtilla_display_scroll", NULL
    };
    guint i;

    tools_setup(&fixture);

    g_assert_true(clawt_mcp_tools_is_permitted(
        fixture.tools, "painter", "clawtilla_display_screenshot"));
    g_assert_true(clawt_mcp_tools_is_permitted(
        fixture.tools, "painter", "clawtilla_display_launch"));
    for (i = 0; acting[i] != NULL; i++) {
        g_assert_true(clawt_mcp_tools_is_permitted(fixture.tools, "painter",
                                                   acting[i]));
        g_assert_false(clawt_mcp_tools_is_permitted(fixture.tools, "looker",
                                                    acting[i]));
        g_assert_false(clawt_mcp_tools_is_permitted(fixture.tools, "plain",
                                                    acting[i]));
    }

    g_assert_true(clawt_mcp_tools_is_permitted(
        fixture.tools, "looker", "clawtilla_display_screenshot"));
    g_assert_true(clawt_mcp_tools_is_permitted(
        fixture.tools, "looker", "clawtilla_display_windows"));
    g_assert_false(clawt_mcp_tools_is_permitted(
        fixture.tools, "looker", "clawtilla_display_launch"));
    g_assert_false(clawt_mcp_tools_is_permitted(
        fixture.tools, "plain", "clawtilla_display_screenshot"));

    tools_teardown(&fixture);
}

/*
 * What the agent gets back: a screenshot is a picture with its size in
 * words; a launch says it started and how its env reached it.
 */
static void
test_tools_screenshot_and_launch(void)
{
    ToolsFixture fixture = { 0 };
    g_autoptr(JsonNode) shot = NULL;
    g_autoptr(JsonNode) launched = NULL;
    g_autofree gchar *marker = NULL;
    g_autofree gchar *arguments = NULL;
    g_autofree gchar *seen = NULL;
    gboolean is_error = TRUE;
    gboolean image = FALSE;
    const gchar *text;

    tools_setup(&fixture);

    shot = run_display_tool(&fixture, "painter",
                            "clawtilla_display_screenshot", "{}");
    text = tool_text(shot, &is_error, &image);
    g_assert_false(is_error);
    g_assert_true(image);
    g_assert_nonnull(strstr(text, "2x2 pixels"));

    marker = g_build_filename(fixture.fake.root, "launched", NULL);
    arguments = g_strdup_printf("{\"command\":\"echo $DISPLAY > '%s'\"}",
                                marker);
    launched = run_display_tool(&fixture, "painter",
                                "clawtilla_display_launch", arguments);
    text = tool_text(launched, &is_error, NULL);
    g_assert_false(is_error);
    g_assert_true(g_str_has_prefix(text, "Started"));
    g_assert_true(wait_for_file(marker, 5000));
    g_usleep(100 * 1000);
    g_assert_true(g_file_get_contents(marker, &seen, NULL, NULL));
    g_assert_cmpstr(seen, ==, ":77\n");

    tools_teardown(&fixture);
}

/*
 * A person holding the screen stops the agent's hands with the
 * takeover's own sentence -- and leaves it able to look, so it can wait
 * usefully instead of going blind.
 */
static void
test_tools_refused_while_a_person_holds_the_screen(void)
{
    ToolsFixture fixture = { 0 };
    g_autoptr(JsonNode) click = NULL;
    g_autoptr(JsonNode) shot = NULL;
    gboolean is_error = FALSE;
    gboolean image = FALSE;
    const gchar *text;

    tools_setup(&fixture);
    g_assert_true(clawt_takeover_take(fixture.takeover, "painter", "a test",
                                      60, NULL));

    click = run_display_tool(&fixture, "painter", "clawtilla_display_click",
                             "{\"x\":1,\"y\":1}");
    text = tool_text(click, &is_error, NULL);
    g_assert_true(is_error);
    g_assert_cmpstr(text, ==, clawt_takeover_refusal_text());

    shot = run_display_tool(&fixture, "painter",
                            "clawtilla_display_screenshot", "{}");
    tool_text(shot, &is_error, &image);
    g_assert_false(is_error);
    g_assert_true(image);

    tools_teardown(&fixture);
}

/* ── computer.act and the Screen tab's status, over IPC ─────────── */

typedef struct {
    FakeX         fake;
    GMainContext *context;
    ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_setup(DaemonFixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *yaml = NULL;
    g_autoptr(ClawtComputer) computer = NULL;
    const gchar *display =
        "    computer:\n      type: display\n"
        "      host:\n        confirm_host_control: true\n";

    fake_x_setup(&fixture->fake);
    path = g_build_filename(fixture->fake.root, "config.yaml", NULL);
    yaml = g_strdup_printf(
        "daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
        "  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "agents:\n"
        "  - id: painter\n%s"
        "  - id: looker\n%s      display:\n"
        "        allow_input: false\n        allow_spawn: false\n"
        "  - id: plain\n",
        fixture->fake.root, fixture->fake.root, fixture->fake.root,
        fixture->fake.root, display, display);
    g_assert_true(g_file_set_contents(path, yaml, -1, &error));
    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(path, fixture->context);
    g_assert_true(clawt_daemon_start(fixture->daemon, NULL));

    /* painter's and looker's computers: the stand-in display, started */
    computer = fake_display_computer(&fixture->fake, NULL);
    g_assert_true(clawt_computer_start(computer, &error));
    clawt_agent_set_computer(clawt_agent_manager_get(fixture->daemon->agents,
                                                     "painter"), computer);
    clawt_agent_set_computer(clawt_agent_manager_get(fixture->daemon->agents,
                                                     "looker"), computer);
}

static void
daemon_teardown(DaemonFixture *fixture)
{
    ClawtComputer *computer = clawt_agent_get_computer(
        clawt_agent_manager_get(fixture->daemon->agents, "painter"));

    if (computer != NULL)
        clawt_computer_stop(computer, NULL);
    clawt_daemon_stop(fixture->daemon);
    g_clear_object(&fixture->daemon);
    while (g_main_context_iteration(fixture->context, FALSE))
        ;
    g_clear_pointer(&fixture->context, g_main_context_unref);
    fake_x_teardown(&fixture->fake);
}

static JsonNode *
ipc(DaemonFixture *fixture, const gchar *kind, const gchar *payload_json)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "t1");
    g_autoptr(JsonParser) parser = json_parser_new();

    g_assert_true(json_parser_load_from_data(parser, payload_json, -1, NULL));
    clawt_ipc_frame_set_payload(frame,
                                json_node_copy(json_parser_get_root(parser)));
    return clawt_daemon_handle_request(fixture->daemon, frame);
}

static gint
error_code(JsonNode *reply)
{
    g_assert_nonnull(reply);
    g_assert_true(clawt_ipc_frame_is_error(reply));
    return (gint)json_object_get_int_member(json_node_get_object(reply),
                                            "code");
}

/*
 * computer.act's refusals, each with the reason a client can show: no
 * such agent, no display, no such action, a grant that is off -- and a
 * person holding the screen, which is an answer (allowed: false, with
 * the sentence) rather than an error, so a tool can pass it on.
 */
static void
test_act_refusals(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(JsonNode) none = NULL;
    g_autoptr(JsonNode) plain = NULL;
    g_autoptr(JsonNode) dance = NULL;
    g_autoptr(JsonNode) launch = NULL;
    g_autoptr(JsonNode) click = NULL;
    g_autoptr(JsonNode) taken = NULL;
    g_autoptr(JsonNode) held = NULL;
    JsonObject *answer;

    daemon_setup(&fixture);

    none = ipc(&fixture, "computer.act",
               "{\"agent\":\"nobody\",\"action\":\"screenshot\"}");
    g_assert_cmpint(error_code(none), ==, CLAWT_ERROR_NOT_FOUND);

    plain = ipc(&fixture, "computer.act",
                "{\"agent\":\"plain\",\"action\":\"screenshot\"}");
    g_assert_cmpint(error_code(plain), ==, CLAWT_ERROR_NOT_SUPPORTED);

    dance = ipc(&fixture, "computer.act",
                "{\"agent\":\"painter\",\"action\":\"dance\"}");
    g_assert_cmpint(error_code(dance), ==, CLAWT_ERROR_INVALID_ARGUMENT);

    launch = ipc(&fixture, "computer.act",
                 "{\"agent\":\"looker\",\"action\":\"launch\","
                 "\"command\":\"xterm\"}");
    g_assert_cmpint(error_code(launch), ==, CLAWT_ERROR_PERMISSION_DENIED);

    click = ipc(&fixture, "computer.act",
                "{\"agent\":\"looker\",\"action\":\"click\","
                "\"x\":1,\"y\":1}");
    g_assert_cmpint(error_code(click), ==, CLAWT_ERROR_PERMISSION_DENIED);

    taken = ipc(&fixture, "computer.takeover",
                "{\"agent\":\"painter\",\"holder\":\"a test\"}");
    g_assert_false(clawt_ipc_frame_is_error(taken));
    held = ipc(&fixture, "computer.act",
               "{\"agent\":\"painter\",\"action\":\"type\","
               "\"text\":\"hi\"}");
    g_assert_false(clawt_ipc_frame_is_error(held));
    answer = clawt_ipc_frame_get_payload(held);
    g_assert_false(json_object_get_boolean_member(answer, "allowed"));
    g_assert_cmpstr(json_object_get_string_member(answer, "refusal"), ==,
                    clawt_takeover_refusal_text());

    daemon_teardown(&fixture);
}

/*
 * The Screen tab's status for a display: a screen, observable, and
 * which server is drawing it -- the line a person reads to learn why
 * auto picked what it picked.
 */
static void
test_screen_status_names_the_backend(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(JsonNode) status = NULL;
    JsonObject *screen;

    daemon_setup(&fixture);

    status = ipc(&fixture, "computer.screen", "{\"agent\":\"painter\"}");
    g_assert_false(clawt_ipc_frame_is_error(status));
    screen = clawt_ipc_frame_get_payload(status);
    g_assert_cmpstr(json_object_get_string_member(screen, "type"), ==,
                    "display");
    g_assert_true(json_object_get_boolean_member(screen, "has_screen"));
    g_assert_true(json_object_get_boolean_member(screen, "observable"));
    g_assert_cmpstr(json_object_get_string_member(screen, "display_backend"),
                    ==, "xvfb");
    g_assert_true(json_object_get_boolean_member(screen, "display_running"));

    /* the display's own grant, not the desktop's (off by default) */
    g_assert_true(json_object_get_boolean_member(screen, "allow_input"));
    g_assert_false(json_object_has_member(screen, "viewer"));
    {
        g_autoptr(JsonNode) looker = ipc(&fixture, "computer.screen",
                                         "{\"agent\":\"looker\"}");

        g_assert_false(json_object_get_boolean_member(
            clawt_ipc_frame_get_payload(looker), "allow_input"));
    }

    daemon_teardown(&fixture);
}

/* ── The real servers (each skipped when it is not installed) ────── */

static gchar *
find_real(const gchar *name)
{
    g_autofree gchar *binary = NULL;
    ClawtDisplayBackend chosen = CLAWT_DISPLAY_BACKEND_AUTO;
    ClawtDisplayBackend want = g_strcmp0(name, "gowl") == 0
        ? CLAWT_DISPLAY_BACKEND_GOWL
        : g_strcmp0(name, "Xvnc") == 0 ? CLAWT_DISPLAY_BACKEND_XVNC
                                       : CLAWT_DISPLAY_BACKEND_XVFB;

    if (!clawt_display_resolve(want, NULL, &chosen, &binary, NULL, NULL))
        return NULL;
    return g_steal_pointer(&binary);
}

/*
 * A real Xvfb: a full-size picture, a pointer that goes where it was
 * sent, and an app whose window changes the picture.
 */
static void
test_real_xvfb(void)
{
    g_autofree gchar *xvfb = find_real("Xvfb");
    g_autofree gchar *xdotool = g_find_program_in_path("xdotool");
    g_autofree gchar *xterm = g_find_program_in_path("xterm");
    g_autofree gchar *root = NULL;
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) before = NULL;
    g_autoptr(GBytes) after = NULL;
    g_autofree gchar *h1 = NULL;
    g_autofree gchar *h2 = NULL;
    guint w = 0;
    guint h = 0;

    if (xvfb == NULL || xdotool == NULL) {
        g_test_skip("needs Xvfb and xdotool");
        return;
    }

    root = g_dir_make_tmp("clawt-display-XXXXXX", NULL);
    dir = g_build_filename(root, "display", NULL);
    server = clawt_display_server_new(CLAWT_DISPLAY_BACKEND_XVFB, dir, 800,
                                      600);
    g_assert_true(clawt_display_server_start(server, &error));
    g_assert_no_error(error);

    before = clawt_display_server_capture(server, &error);
    g_assert_no_error(error);
    g_assert_true(clawt_screen_png_size(before, &w, &h));
    g_assert_cmpuint(w, ==, 800);
    g_assert_cmpuint(h, ==, 600);

    {
        g_autoptr(ClawtInputEvent) move =
            clawt_input_event_new(CLAWT_INPUT_MOVE);
        g_autoptr(JsonNode) none = NULL;

        move->x = 123;
        move->y = 234;
        g_assert_true(clawt_display_server_send_input(server, move, &error));
        g_assert_no_error(error);
        none = clawt_display_server_list_windows(server, &error);
        g_assert_no_error(error);
        g_assert_true(JSON_NODE_HOLDS_ARRAY(none));
    }

    if (xterm != NULL) {
        g_autoptr(JsonNode) windows = NULL;
        guint waited;

        g_assert_true(clawt_display_server_launch(
            server, "xterm -geometry 60x10+0+0 -T clawt-test-window", NULL,
            NULL, NULL, &error));
        g_assert_no_error(error);

        /* until its window is up and drawn */
        for (waited = 0; waited < 10000; waited += 100) {
            g_clear_pointer(&windows, json_node_unref);
            windows = clawt_display_server_list_windows(server, NULL);
            if (windows != NULL && JSON_NODE_HOLDS_ARRAY(windows) &&
                json_array_get_length(json_node_get_array(windows)) > 0)
                break;
            g_usleep(100 * 1000);
        }
        g_usleep(500 * 1000);
        g_assert_nonnull(windows);
        g_assert_cmpuint(json_array_get_length(json_node_get_array(windows)),
                         >, 0);

        after = clawt_display_server_capture(server, &error);
        g_assert_no_error(error);
        h1 = clawt_screen_hash_bytes(before);
        h2 = clawt_screen_hash_bytes(after);
        g_assert_cmpstr(h1, !=, h2);

        /* typing reaches the focused window without failing */
        {
            g_autoptr(ClawtInputEvent) text =
                clawt_input_event_new(CLAWT_INPUT_TEXT);

            clawt_input_event_set_text(text, "echo hello");
            g_assert_true(clawt_display_server_send_input(server, text,
                                                          &error));
            g_assert_no_error(error);
        }
    }

    clawt_display_server_stop(server);
    clawt_test_remove_tree(root);
}

/* A real Xvnc: a loopback VNC address that answers, and a picture. */
static void
test_real_xvnc(void)
{
    g_autofree gchar *xvnc = find_real("Xvnc");
    g_autofree gchar *import = g_find_program_in_path("import");
    g_autofree gchar *root = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *uri = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) png = NULL;
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketConnection) connection = NULL;
    guint w = 0;
    guint h = 0;
    guint port = 0;

    if (xvnc == NULL || import == NULL) {
        g_test_skip("needs Xvnc and ImageMagick's import");
        return;
    }

    root = g_dir_make_tmp("clawt-display-XXXXXX", NULL);
    dir = g_build_filename(root, "display", NULL);
    server = clawt_display_server_new(CLAWT_DISPLAY_BACKEND_XVNC, dir, 640,
                                      480);
    g_assert_true(clawt_display_server_start(server, &error));
    g_assert_no_error(error);

    uri = clawt_display_server_dup_viewer_uri(server);
    g_assert_nonnull(uri);
    g_assert_true(g_str_has_prefix(uri, "vnc://127.0.0.1:"));
    port = (guint)g_ascii_strtoull(uri + strlen("vnc://127.0.0.1:"), NULL,
                                   10);
    g_assert_cmpuint(port, >, 0);
    connection = g_socket_client_connect_to_host(client, "127.0.0.1", port,
                                                 NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(connection);

    png = clawt_display_server_capture(server, &error);
    g_assert_no_error(error);
    g_assert_true(clawt_screen_png_size(png, &w, &h));
    g_assert_cmpuint(w, ==, 640);
    g_assert_cmpuint(h, ==, 480);

    clawt_display_server_stop(server);
    g_assert_null(clawt_display_server_dup_viewer_uri(server));
    clawt_test_remove_tree(root);
}

/*
 * A real headless gowl, in a runtime directory of its own: a picture
 * through its MCP socket, an app through its spawn, and the operator's
 * own gowl.sock untouched.
 */
static void
test_real_gowl(void)
{
    g_autofree gchar *gowl = find_real("gowl");
    g_autofree gchar *zenity = g_find_program_in_path("zenity");
    g_autofree gchar *root = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *mine = NULL;
    g_autoptr(ClawtDisplayServer) server = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GBytes) png = NULL;
    g_autoptr(GHashTable) env = NULL;
    struct stat before;
    gboolean had = FALSE;
    guint w = 0;
    guint h = 0;

    if (gowl == NULL) {
        g_test_skip("needs a gowl that runs");
        return;
    }

    /* the operator's own socket, to check it is left alone */
    mine = g_build_filename(g_get_user_runtime_dir(), "gowl.sock", NULL);
    had = g_stat(mine, &before) == 0;

    root = g_dir_make_tmp("clawt-display-XXXXXX", NULL);
    dir = g_build_filename(root, "d", NULL);
    server = clawt_display_server_new(CLAWT_DISPLAY_BACKEND_GOWL, dir, 800,
                                      600);
    if (!clawt_display_server_start(server, &error)) {
        /* a gowl built without its MCP module cannot be driven */
        if (error != NULL && strstr(error->message, "sockets") != NULL) {
            g_test_skip(error->message);
            clawt_test_remove_tree(root);
            return;
        }
    }
    g_assert_no_error(error);

    env = clawt_display_server_dup_environment(server);
    g_assert_true(g_str_has_prefix(g_hash_table_lookup(env,
                                                       "WAYLAND_DISPLAY"),
                                   dir));

    /*
     * Only the Wayland socket is bound into a box (0.51.4): gowl.sock and
     * gowl-mcp.sock beside it both spawn commands outside any box.
     */
    {
        g_auto(GStrv) binds = clawt_display_server_dup_bind_paths(server);

        g_assert_cmpuint(g_strv_length(binds), ==, 1);
        g_assert_cmpstr(binds[0], ==,
                        g_hash_table_lookup(env, "WAYLAND_DISPLAY"));
    }

    png = clawt_display_server_capture(server, &error);
    g_assert_no_error(error);
    g_assert_true(clawt_screen_png_size(png, &w, &h));
    g_assert_cmpuint(w, ==, 800);
    g_assert_cmpuint(h, ==, 600);

    if (zenity != NULL) {
        g_autoptr(JsonNode) windows = NULL;
        guint waited;

        g_assert_true(clawt_display_server_launch(
            server, "zenity --info --text clawt-test", NULL, NULL, NULL,
            &error));
        g_assert_no_error(error);
        for (waited = 0; waited < 10000; waited += 200) {
            g_autofree gchar *text = NULL;

            g_clear_pointer(&windows, json_node_unref);
            windows = clawt_display_server_list_windows(server, NULL);
            if (windows != NULL) {
                g_autoptr(JsonGenerator) gen = json_generator_new();

                json_generator_set_root(gen, windows);
                text = json_generator_to_data(gen, NULL);
                if (strstr(text, "enity") != NULL)
                    break;
            }
            g_usleep(200 * 1000);
        }
        g_assert_cmpuint(waited, <, 10000);
    }

    clawt_display_server_stop(server);

    if (had) {
        struct stat after;

        g_assert_cmpint(g_stat(mine, &after), ==, 0);
        g_assert_cmpuint(after.st_ino, ==, before.st_ino);
    }
    clawt_test_remove_tree(root);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/display/png/round-trip", test_png_round_trip);
    g_test_add_func("/display/xwd/32bpp-both-byte-orders", test_xwd_32bpp);
    g_test_add_func("/display/xwd/16bpp-565", test_xwd_16bpp);
    g_test_add_func("/display/xwd/refuses", test_xwd_refuses);
    g_test_add_func("/display/argv/xdotool", test_xdotool_argv);
    g_test_add_func("/display/argv/servers", test_server_argv);
    g_test_add_func("/display/gowl/config-and-environment",
                    test_gowl_config_and_environment);
    g_test_add_func("/display/resolution", test_parse_resolution);
    g_test_add_func("/display/resolve/skips-a-broken-gowl",
                    test_resolve_skips_a_broken_gowl);
    g_test_add_func("/display/resolve/pinned-gowl", test_resolve_pinned_gowl);
    g_test_add_func("/display/resolve/auto-order", test_resolve_auto_order);
    g_test_add_func("/display/server/lifecycle", test_server_lifecycle);
    g_test_add_func("/display/server/death-is-explained",
                    test_server_death_is_explained);
    g_test_add_func("/display/server/launch", test_server_launch);
    g_test_add_func("/display/server/logs-refuse-symlinks",
                    test_logs_refuse_symlinks);
    g_test_add_func("/display/computer/binds-the-socket-alone",
                    test_computer_binds_the_socket_alone);
    g_test_add_func("/display/computer/commands-and-startup",
                    test_computer_commands_and_startup);
    g_test_add_func("/display/computer/observable",
                    test_computer_is_observable);
    g_test_add_func("/display/computer/perform", test_perform);
    g_test_add_func("/display/computer/describe", test_describe);
    g_test_add_func("/display/status-line", test_status_line);
    g_test_add_func("/display/tools/follow-the-grants",
                    test_tools_follow_the_grants);
    g_test_add_func("/display/tools/screenshot-and-launch",
                    test_tools_screenshot_and_launch);
    g_test_add_func("/display/tools/refused-while-held",
                    test_tools_refused_while_a_person_holds_the_screen);
    g_test_add_func("/display/ipc/act-refusals", test_act_refusals);
    g_test_add_func("/display/ipc/screen-status-names-the-backend",
                    test_screen_status_names_the_backend);
    g_test_add_func("/display/real/xvfb", test_real_xvfb);
    g_test_add_func("/display/real/xvnc", test_real_xvnc);
    g_test_add_func("/display/real/gowl", test_real_gowl);

    return g_test_run();
}
