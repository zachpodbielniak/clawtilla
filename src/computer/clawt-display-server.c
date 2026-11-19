/*
 * clawt-display-server.c - A private display that GUI apps draw on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three ways to draw a screen nobody is sitting at, behind one object:
 *
 * - Xvfb keeps its framebuffer in a file (`-fbdir`), as an XWD dump, so a
 *   picture is a read of that file and a PNG encoded here -- no X
 *   round trip and no image library.  Keys and clicks go through
 *   xdotool.
 * - Xvnc is the same X server with a VNC server inside, on loopback.  It
 *   has no `-fbdir`, so a picture is ImageMagick's `import`; keys and
 *   clicks are xdotool again; and the VNC port is the live view a person
 *   can open to drive it directly.
 * - gowl runs headless in a runtime directory of its own, with its MCP
 *   module on: pictures and input are its MCP tools, the same calls the
 *   desktop add-on makes against the operator's real gowl, and apps are
 *   started through its IPC `spawn` so X ones reach XWayland too.
 *
 * Every child is forked from one long-lived thread
 * (clawt_display_spawn_lasting()) so it dies with the daemon.
 */

#include "clawtilla.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <gio/gunixinputstream.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#if defined(__linux__)
#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#endif

#define START_TIMEOUT_MS   (20000)
#define STOP_WAIT_MS       (3000)
#define CALL_TIMEOUT_S     (15)
#define FAILURE_TAIL_LINES (8)

/* ── Children that die with the daemon ───────────────────────────── */

/* The child setup asked for, run after the death signal is armed. */
typedef struct {
    GSpawnChildSetupFunc  setup;
    gpointer              data;
} ChildSetup;

/* Runs in the child between fork and exec: async-signal-safe only. */
static void
die_with_parent(gpointer user_data)
{
    ChildSetup *extra = user_data;

#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    if (extra != NULL && extra->setup != NULL)
        extra->setup(extra->data);
}

/*
 * Where the children are forked.
 *
 * PR_SET_PDEATHSIG is tied to the *thread* that forked, not the process:
 * the child is signalled when that thread ends.  A display is started by
 * whichever thread first needs it -- an agent's tool call runs on a
 * thread that ends with the call -- so a server forked there would be
 * sent SIGTERM moments after it started.  Every fork happens here
 * instead, on one thread that lives as long as the process.
 */
typedef struct {
    GSubprocessLauncher  *launcher;
    const gchar * const  *argv;
    GSubprocess          *child;
    GError               *error;
    gboolean              done;
    GMutex                lock;
    GCond                 cond;
} SpawnRequest;

static gpointer
spawner_main(gpointer data)
{
    GAsyncQueue *queue = data;

    for (;;) {
        SpawnRequest *request = g_async_queue_pop(queue);
        GSubprocess *child;
        GError *error = NULL;

        child = g_subprocess_launcher_spawnv(request->launcher, request->argv,
                                             &error);

        g_mutex_lock(&request->lock);
        request->child = child;
        request->error = error;
        request->done = TRUE;
        g_cond_signal(&request->cond);
        g_mutex_unlock(&request->lock);
    }
    return NULL;
}

static GSubprocess *
spawn_lasting_with(GSubprocessLauncher  *launcher,
                   const gchar * const  *argv,
                   GSpawnChildSetupFunc  setup,
                   gpointer              setup_data,
                   GError              **error)
{
    static GAsyncQueue *queue = NULL;
    SpawnRequest request = { 0 };
    ChildSetup extra = { setup, setup_data };

    if (g_once_init_enter(&queue)) {
        GAsyncQueue *made = g_async_queue_new();

        /* Never joined: it ends with the process, and that is the point. */
        g_thread_unref(g_thread_new("clawt-display-spawn", spawner_main, made));
        g_once_init_leave(&queue, made);
    }

    /*
     * @extra lives on this stack frame until the spawn returns, which is
     * after the fork: the child has its own copy of the address space by
     * then, so the pointer stays good for it.
     */
    g_subprocess_launcher_set_child_setup(launcher, die_with_parent, &extra,
                                          NULL);

    request.launcher = launcher;
    request.argv = argv;
    g_mutex_init(&request.lock);
    g_cond_init(&request.cond);

    g_mutex_lock(&request.lock);
    g_async_queue_push(queue, &request);
    while (!request.done)
        g_cond_wait(&request.cond, &request.lock);
    g_mutex_unlock(&request.lock);

    g_mutex_clear(&request.lock);
    g_cond_clear(&request.cond);

    /* The launcher must not keep a pointer into this frame. */
    g_subprocess_launcher_set_child_setup(launcher, NULL, NULL, NULL);

    if (request.error != NULL)
        g_propagate_error(error, request.error);
    return request.child;
}

GSubprocess *
clawt_display_spawn_lasting(GSubprocessLauncher *launcher,
                            const gchar * const *argv,
                            GError             **error)
{
    g_return_val_if_fail(G_IS_SUBPROCESS_LAUNCHER(launcher), NULL);
    g_return_val_if_fail(argv != NULL && argv[0] != NULL, NULL);

    return spawn_lasting_with(launcher, argv, NULL, NULL, error);
}

gboolean
clawt_display_child_alive(GSubprocess *child)
{
    /* GSubprocess reaps on GLib's worker: a child that exited has no
     * identifier any more. */
    return child != NULL && g_subprocess_get_identifier(child) != NULL;
}

void
clawt_display_end_child(GSubprocess *child)
{
    gint waited = 0;

    if (!clawt_display_child_alive(child))
        return;

    g_subprocess_send_signal(child, SIGTERM);
    while (clawt_display_child_alive(child) && waited < STOP_WAIT_MS) {
        g_usleep(50 * 1000);
        waited += 50;
    }
    if (clawt_display_child_alive(child))
        g_subprocess_force_exit(child);
}

gint
clawt_display_read_displayfd(GSubprocess *server, guint timeout_ms,
                             GError **error)
{
    GInputStream *out;
    gint fd;
    gchar buffer[32] = { 0 };
    gsize got = 0;
    gint64 deadline;

    g_return_val_if_fail(G_IS_SUBPROCESS(server), -1);

    out = g_subprocess_get_stdout_pipe(server);
    if (out == NULL || !G_IS_UNIX_INPUT_STREAM(out)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                            "the X server was started without a pipe to "
                            "report its display on");
        return -1;
    }

    fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(out));
    deadline = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

    /*
     * Polled with a deadline rather than read: a server that hangs
     * before writing would otherwise hold this thread for ever.
     */
    while (got < sizeof(buffer) - 1) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        gint64 left = (deadline - g_get_monotonic_time()) / 1000;
        gssize n;

        if (left <= 0)
            break;
        if (poll(&pfd, 1, (int)left) <= 0)
            continue;
        n = read(fd, buffer + got, sizeof(buffer) - 1 - got);
        if (n <= 0)
            break;
        got += (gsize)n;
        if (memchr(buffer, '\n', got) != NULL)
            return (gint)g_ascii_strtoll(buffer, NULL, 10);
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                "the X server did not report a display within %u seconds%s",
                timeout_ms / 1000,
                clawt_display_child_alive(server) ? "" : " (it exited)");
    return -1;
}

/* ── PNG, written by hand ────────────────────────────────────────── */

static guint32 crc_table[256];

static void
crc_table_init(void)
{
    static gsize done = 0;
    guint32 n;

    if (!g_once_init_enter(&done))
        return;

    for (n = 0; n < 256; n++) {
        guint32 c = n;
        gint k;

        for (k = 0; k < 8; k++)
            c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    g_once_init_leave(&done, 1);
}

static guint32
crc_update(guint32 crc, const guint8 *data, gsize length)
{
    gsize i;

    for (i = 0; i < length; i++)
        crc = crc_table[(crc ^ data[i]) & 0xff] ^ (crc >> 8);
    return crc;
}

static void
put_be32(GByteArray *out, guint32 value)
{
    guint8 bytes[4];

    bytes[0] = (guint8)(value >> 24);
    bytes[1] = (guint8)(value >> 16);
    bytes[2] = (guint8)(value >> 8);
    bytes[3] = (guint8)value;
    g_byte_array_append(out, bytes, 4);
}

/* One PNG chunk: length, type, data, and the CRC of type and data. */
static void
put_chunk(GByteArray *out, const gchar *type, const guint8 *data,
          gsize length)
{
    guint32 crc = 0xffffffffu;

    put_be32(out, (guint32)length);
    g_byte_array_append(out, (const guint8 *)type, 4);
    if (length > 0)
        g_byte_array_append(out, data, (guint)length);
    crc = crc_update(crc, (const guint8 *)type, 4);
    crc = crc_update(crc, data, length);
    put_be32(out, crc ^ 0xffffffffu);
}

GBytes *
clawt_display_png_encode(guint width, guint height, const guint8 *rgb,
                         GError **error)
{
    static const guint8 signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    g_autoptr(GByteArray) png = NULL;
    g_autoptr(GByteArray) header = NULL;
    g_autoptr(GZlibCompressor) zlib = NULL;
    g_autoptr(GOutputStream) memory = NULL;
    g_autoptr(GOutputStream) deflate = NULL;
    gsize stride;
    guint row;
    guint8 filter = 0;

    g_return_val_if_fail(rgb != NULL || width == 0 || height == 0, NULL);

    if (width == 0 || height == 0) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "an empty picture cannot be a PNG");
        return NULL;
    }

    crc_table_init();
    stride = (gsize)width * 3;

    /*
     * The pixel rows, each led by filter byte 0 (none), through zlib.
     * Filtering would make the file smaller; this is a frame a client
     * shows and throws away, and the time is better not spent.
     */
    zlib = g_zlib_compressor_new(G_ZLIB_COMPRESSOR_FORMAT_ZLIB, 3);
    memory = g_memory_output_stream_new_resizable();
    deflate = g_converter_output_stream_new(memory, G_CONVERTER(zlib));

    for (row = 0; row < height; row++) {
        if (!g_output_stream_write_all(deflate, &filter, 1, NULL, NULL,
                                       error) ||
            !g_output_stream_write_all(deflate, rgb + (gsize)row * stride,
                                       stride, NULL, NULL, error))
            return NULL;
    }
    if (!g_output_stream_close(deflate, NULL, error))
        return NULL;

    png = g_byte_array_new();
    g_byte_array_append(png, signature, sizeof(signature));

    /* IHDR: size, 8 bits, truecolour, deflate, no filter, no interlace. */
    header = g_byte_array_new();
    put_be32(header, width);
    put_be32(header, height);
    {
        static const guint8 rest[5] = { 8, 2, 0, 0, 0 };

        g_byte_array_append(header, rest, sizeof(rest));
    }
    put_chunk(png, "IHDR", header->data, header->len);

    put_chunk(png, "IDAT",
              g_memory_output_stream_get_data(
                  G_MEMORY_OUTPUT_STREAM(memory)),
              g_memory_output_stream_get_data_size(
                  G_MEMORY_OUTPUT_STREAM(memory)));
    put_chunk(png, "IEND", NULL, 0);

    return g_byte_array_free_to_bytes(g_steal_pointer(&png));
}

/* ── XWD, as Xvfb keeps its framebuffer ──────────────────────────── */

#define XWD_FIELDS       (25)
#define XWD_VERSION      (7)
#define XWD_ZPIXMAP      (2)
#define XWD_COLOR_BYTES  (12)

static guint32
read32(const guint8 *p, gboolean big_endian)
{
    return big_endian
        ? ((guint32)p[0] << 24) | ((guint32)p[1] << 16) |
          ((guint32)p[2] << 8) | (guint32)p[3]
        : ((guint32)p[3] << 24) | ((guint32)p[2] << 16) |
          ((guint32)p[1] << 8) | (guint32)p[0];
}

/* The shift and width of a channel mask, so 5-6-5 and 8-8-8 both work. */
static void
mask_shape(guint32 mask, guint *shift, guint *bits)
{
    *shift = 0;
    *bits = 0;
    if (mask == 0)
        return;
    while (((mask >> *shift) & 1) == 0)
        (*shift)++;
    while (((mask >> (*shift + *bits)) & 1) != 0 && *shift + *bits < 32)
        (*bits)++;
}

static guint8
channel(guint32 pixel, guint shift, guint bits)
{
    guint32 value;

    if (bits == 0)
        return 0;
    value = (pixel >> shift) & ((bits >= 32) ? 0xffffffffu
                                             : ((1u << bits) - 1));
    if (bits >= 8)
        return (guint8)(value >> (bits - 8));
    /* widen a short channel to the full 0..255 range */
    return (guint8)((value * 255u) / ((1u << bits) - 1));
}

GBytes *
clawt_display_xwd_to_png(const guint8 *data, gsize length, GError **error)
{
    guint32 field[XWD_FIELDS];
    gboolean big;
    guint32 header_size;
    guint32 width;
    guint32 height;
    guint32 bpp;
    guint32 bytes_per_line;
    guint32 ncolors;
    gboolean pixels_msb;
    guint rshift, rbits, gshift, gbits, bshift, bbits;
    gsize offset;
    g_autofree guint8 *rgb = NULL;
    guint32 x;
    guint32 y;
    gint i;

    g_return_val_if_fail(data != NULL || length == 0, NULL);

    if (length < XWD_FIELDS * 4) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the framebuffer file is %" G_GSIZE_FORMAT " bytes, too "
                    "short to be an XWD dump", length);
        return NULL;
    }

    /*
     * The header is a run of 32-bit fields.  Xvfb writes them most
     * significant byte first, as xwd does; the version is checked both
     * ways rather than trusted, so a server that wrote its native order
     * is read too.
     */
    big = read32(data + 4, TRUE) == XWD_VERSION;
    if (!big && read32(data + 4, FALSE) != XWD_VERSION) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "the framebuffer file is not an XWD dump "
                            "(version 7 in neither byte order)");
        return NULL;
    }
    for (i = 0; i < XWD_FIELDS; i++)
        field[i] = read32(data + (gsize)i * 4, big);

    header_size = field[0];
    width = field[4];
    height = field[5];
    pixels_msb = field[7] == 1;          /* byte_order: 1 MSBFirst */
    bpp = field[11];
    bytes_per_line = field[12];
    ncolors = field[19];

    if (field[2] != XWD_ZPIXMAP || (bpp != 16 && bpp != 24 && bpp != 32)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "the framebuffer is format %u at %u bits per pixel; "
                    "only ZPixmap at 16, 24 or 32 is read",
                    field[2], bpp);
        return NULL;
    }
    if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
        bytes_per_line < width * (bpp / 8)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the framebuffer header is not sensible: %ux%u, %u "
                    "bytes a line", width, height, bytes_per_line);
        return NULL;
    }

    offset = (gsize)header_size + (gsize)ncolors * XWD_COLOR_BYTES;
    if (offset > length ||
        (gsize)bytes_per_line * height > length - offset) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the framebuffer file is shorter than its header says "
                    "(%" G_GSIZE_FORMAT " bytes)", length);
        return NULL;
    }

    mask_shape(field[14], &rshift, &rbits);
    mask_shape(field[15], &gshift, &gbits);
    mask_shape(field[16], &bshift, &bbits);

    rgb = g_malloc((gsize)width * height * 3);

    for (y = 0; y < height; y++) {
        const guint8 *line = data + offset + (gsize)y * bytes_per_line;
        guint8 *dest = rgb + (gsize)y * width * 3;

        for (x = 0; x < width; x++) {
            const guint8 *p = line + (gsize)x * (bpp / 8);
            guint32 pixel = 0;
            guint b;

            /* Assemble the pixel in the byte order the server wrote. */
            for (b = 0; b < bpp / 8; b++) {
                if (pixels_msb)
                    pixel = (pixel << 8) | p[b];
                else
                    pixel |= (guint32)p[b] << (8 * b);
            }

            dest[x * 3] = channel(pixel, rshift, rbits);
            dest[x * 3 + 1] = channel(pixel, gshift, gbits);
            dest[x * 3 + 2] = channel(pixel, bshift, bbits);
        }
    }

    return clawt_display_png_encode(width, height, rgb, error);
}

/* ── argv builders ───────────────────────────────────────────────── */

/*
 * A key combo as xdotool spells it: "Ctrl+Shift+t" -> "ctrl+shift+t".
 * The modifiers are lowercased because xdotool's aliases are; the key
 * itself is left alone, because "Return" and "return" are different
 * keysyms to it and an XKB name is what the caller was told to send.
 */
static gchar *
xdotool_combo(const gchar *combo)
{
    g_auto(GStrv) parts = g_strsplit(combo, "+", -1);
    guint n = g_strv_length(parts);
    guint i;

    for (i = 0; i + 1 < n; i++) {
        gchar *lower = g_ascii_strdown(parts[i], -1);

        g_free(parts[i]);
        parts[i] = lower;
    }
    return g_strjoinv("+", parts);
}

GStrv
clawt_display_xdotool_argv(const gchar *xdotool, ClawtInputEvent *event)
{
    g_autoptr(GStrvBuilder) argv = g_strv_builder_new();

    g_return_val_if_fail(xdotool != NULL, NULL);
    g_return_val_if_fail(event != NULL, NULL);

    g_strv_builder_add(argv, xdotool);

    switch (event->kind) {
    case CLAWT_INPUT_KEY: {
        g_autofree gchar *combo = NULL;

        if (event->text == NULL || *event->text == '\0')
            return NULL;
        combo = xdotool_combo(event->text);
        g_strv_builder_add_many(argv, "key", "--clearmodifiers", combo,
                                NULL);
        break;
    }

    case CLAWT_INPUT_TEXT:
        if (event->text == NULL || *event->text == '\0')
            return NULL;
        /* `--` so text that starts with a dash is typed, not parsed. */
        g_strv_builder_add_many(argv, "type", "--delay", "12", "--",
                                event->text, NULL);
        break;

    case CLAWT_INPUT_CLICK:
    case CLAWT_INPUT_MOVE:
    case CLAWT_INPUT_SCROLL: {
        g_autofree gchar *x = g_strdup_printf("%d", MAX(event->x, 0));
        g_autofree gchar *y = g_strdup_printf("%d", MAX(event->y, 0));

        /* Moved first: a click lands where it was aimed. */
        g_strv_builder_add_many(argv, "mousemove", "--sync", x, y, NULL);

        if (event->kind == CLAWT_INPUT_CLICK) {
            g_autofree gchar *button = g_strdup_printf(
                "%u", (event->button >= 1 && event->button <= 3)
                          ? event->button : 1);

            g_strv_builder_add_many(argv, "click", button, NULL);
        } else if (event->kind == CLAWT_INPUT_SCROLL) {
            /*
             * X scrolls with buttons: 4 up, 5 down, 6 left, 7 right, one
             * press a notch.  The larger axis decides; a scroll of 0 is
             * still one notch, since it was asked for.
             */
            gboolean vertical = ABS(event->dy) >= ABS(event->dx);
            gdouble amount = vertical ? event->dy : event->dx;
            guint notches = (guint)MAX(1.0, ABS(amount) + 0.5);
            const gchar *button = vertical ? (amount < 0 ? "4" : "5")
                                           : (amount < 0 ? "6" : "7");
            g_autofree gchar *repeat = g_strdup_printf("%u", notches);

            g_strv_builder_add_many(argv, "click", "--repeat", repeat,
                                    button, NULL);
        }
        break;
    }
    }

    return g_strv_builder_end(argv);
}

GStrv
clawt_display_import_argv(const gchar *import, const gchar *display)
{
    g_autoptr(GStrvBuilder) argv = g_strv_builder_new();

    g_return_val_if_fail(import != NULL, NULL);
    g_return_val_if_fail(display != NULL, NULL);

    g_strv_builder_add_many(argv, import, "-display", display, "-window",
                            "root", "png:-", NULL);
    return g_strv_builder_end(argv);
}

GStrv
clawt_display_server_argv(ClawtDisplayBackend backend, const gchar *binary,
                          guint width, guint height, guint depth,
                          const gchar *fbdir, guint vnc_port,
                          const gchar *config)
{
    g_autoptr(GStrvBuilder) argv = g_strv_builder_new();
    g_autofree gchar *size = NULL;
    g_autofree gchar *depth_text = g_strdup_printf("%u", depth);

    g_return_val_if_fail(binary != NULL, NULL);

    g_strv_builder_add(argv, binary);

    switch (backend) {
    case CLAWT_DISPLAY_BACKEND_XVFB:
        /*
         * TCP off; the abstract socket left on, because a daemon with a
         * private /tmp does not share /tmp/.X11-unix with what it starts.
         */
        size = g_strdup_printf("%ux%ux%u", width, height, depth);
        g_strv_builder_add_many(argv, "-displayfd", "1", "-screen", "0",
                                size, "-nolisten", "tcp", NULL);
        if (fbdir != NULL)
            g_strv_builder_add_many(argv, "-fbdir", fbdir, NULL);
        break;

    case CLAWT_DISPLAY_BACKEND_XVNC: {
        g_autofree gchar *port = g_strdup_printf("%u", vnc_port);

        size = g_strdup_printf("%ux%u", width, height);
        /*
         * Loopback only and no password: the VNC server is reachable
         * from this machine alone, which is the same trust the daemon's
         * own socket gives -- and a password a person must be told is a
         * password written somewhere.
         */
        g_strv_builder_add_many(argv, "-displayfd", "1", "-geometry", size,
                                "-depth", depth_text, "-rfbport", port,
                                "-localhost", "-SecurityTypes", "None",
                                "-AlwaysShared", "-nolisten", "tcp", NULL);
        break;
    }

    case CLAWT_DISPLAY_BACKEND_GOWL:
        /* No C config: nothing of the operator's own setup is loaded. */
        if (config != NULL)
            g_strv_builder_add_many(argv, "--config", config, NULL);
        g_strv_builder_add(argv, "--no-c-config");
        break;

    case CLAWT_DISPLAY_BACKEND_AUTO:
        break;
    }

    return g_strv_builder_end(argv);
}

gchar *
clawt_display_gowl_config(void)
{
    /*
     * The IPC for `spawn`, the MCP module for pictures and input.
     * stdio transport only: the relay connects to the unix socket, and an
     * HTTP port would be one more thing listening that nobody asked for.
     */
    return g_strdup(
        "# Written by clawtilla for a headless display; rewritten at start.\n"
        "modules:\n"
        "  ipc:\n"
        "    enabled: true\n"
        "  mcp:\n"
        "    enabled: true\n"
        "    transport-stdio: true\n"
        "    transport-http: false\n");
}

GHashTable *
clawt_display_gowl_environment(const gchar *run_dir, guint width,
                               guint height)
{
    GHashTable *env = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            g_free);

    g_return_val_if_fail(run_dir != NULL, env);

    /*
     * Its own runtime directory, so its sockets are not the operator's
     * (a second gowl would otherwise replace gowl.sock under a running
     * session), and wlroots told to draw into memory with the software
     * renderer: there is no GPU to ask on a display nobody looks at.
     */
    g_hash_table_insert(env, g_strdup("XDG_RUNTIME_DIR"), g_strdup(run_dir));
    g_hash_table_insert(env, g_strdup("WLR_BACKENDS"), g_strdup("headless"));
    g_hash_table_insert(env, g_strdup("WLR_HEADLESS_OUTPUTS"), g_strdup("1"));
    g_hash_table_insert(env, g_strdup("WLR_RENDERER"), g_strdup("pixman"));
    g_hash_table_insert(env, g_strdup("WLR_LIBINPUT_NO_DEVICES"),
                        g_strdup("1"));
    g_hash_table_insert(env, g_strdup("GOWL_OUTPUT_SIZE"),
                        g_strdup_printf("%ux%u", width, height));
    g_hash_table_insert(env, g_strdup("GOWL_DISABLE_SYSTEMD"), g_strdup("1"));
    /* Not the operator's session: no inherited display of any kind. */
    g_hash_table_insert(env, g_strdup("WAYLAND_DISPLAY"), g_strdup(""));
    g_hash_table_insert(env, g_strdup("DISPLAY"), g_strdup(""));
    return env;
}

gboolean
clawt_display_parse_resolution(const gchar *text, guint *width,
                               guint *height)
{
    guint64 w;
    guint64 h;
    gchar *end = NULL;

    g_return_val_if_fail(width != NULL && height != NULL, FALSE);

    if (text == NULL)
        return FALSE;
    w = g_ascii_strtoull(text, &end, 10);
    if (end == NULL || (*end != 'x' && *end != 'X'))
        return FALSE;
    h = g_ascii_strtoull(end + 1, &end, 10);
    if (end == NULL || *end != '\0')
        return FALSE;
    if (w < 64 || h < 64 || w > 8192 || h > 8192)
        return FALSE;

    *width = (guint)w;
    *height = (guint)h;
    return TRUE;
}

/* ── Finding a server that works ─────────────────────────────────── */

/* Runs @argv with a deadline; TRUE when it exits 0. */
static gboolean
runs(const gchar * const *argv, gchar **why)
{
    g_autoptr(GSubprocess) child = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *err = NULL;

    child = g_subprocess_newv(argv, G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                    G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    if (child == NULL ||
        !g_subprocess_communicate_utf8(child, NULL, NULL, NULL, &err,
                                       &error)) {
        if (why != NULL)
            *why = g_strdup(error != NULL ? error->message : "did not run");
        return FALSE;
    }
    if (g_subprocess_get_if_exited(child) &&
        g_subprocess_get_exit_status(child) == 0)
        return TRUE;

    if (why != NULL) {
        gchar *line = (err != NULL) ? g_strstrip(err) : NULL;
        gchar *newline = (line != NULL) ? strchr(line, '\n') : NULL;

        if (newline != NULL)
            *newline = '\0';
        *why = g_strdup((line != NULL && *line != '\0') ? line
                                                         : "exited non-zero");
    }
    return FALSE;
}

/*
 * The candidates for @name: every PATH entry, then /usr/bin and
 * /usr/sbin for a daemon started with a thin PATH -- each real file once
 * and only those that exist.  Every one is tried in that order, so a
 * broken build early on PATH costs a line in the notes, not the display.
 */
static GPtrArray *
candidates_for(const gchar *name)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash,
                                                       g_str_equal, g_free,
                                                       NULL);
    g_autoptr(GPtrArray) dirs = g_ptr_array_new_with_free_func(g_free);
    g_auto(GStrv) path = g_strsplit(g_getenv("PATH") != NULL
                                        ? g_getenv("PATH") : "", ":", -1);
    guint i;

    for (i = 0; path[i] != NULL; i++)
        if (*path[i] != '\0')
            g_ptr_array_add(dirs, g_strdup(path[i]));
    g_ptr_array_add(dirs, g_strdup("/usr/bin"));
    g_ptr_array_add(dirs, g_strdup("/usr/sbin"));

    for (i = 0; i < dirs->len; i++) {
        g_autofree gchar *candidate =
            g_build_filename(g_ptr_array_index(dirs, i), name, NULL);
        gchar *resolved;

        if (!g_file_test(candidate, G_FILE_TEST_IS_EXECUTABLE))
            continue;
        resolved = realpath(candidate, NULL);
        if (resolved == NULL)
            continue;
        if (!g_hash_table_contains(seen, resolved)) {
            g_hash_table_add(seen, g_strdup(resolved));
            g_ptr_array_add(out, g_steal_pointer(&candidate));
        }
        free(resolved);
    }
    return out;
}

/*
 * The first @name that works, noting each that did not.  gowl must
 * answer --version, because a gowl that cannot load its wlroots fails
 * there and nowhere earlier.  An X server is not run to be asked: there
 * is no flag every build exits cleanly on, and one that ignores it would
 * sit there being a server -- a start that fails says why soon enough.
 */
static gchar *
first_working(const gchar *name, gboolean strict, GString *notes)
{
    g_autoptr(GPtrArray) found = candidates_for(name);
    guint i;

    for (i = 0; i < found->len; i++) {
        const gchar *candidate = g_ptr_array_index(found, i);
        const gchar *argv[] = { candidate, "--version", NULL };
        g_autofree gchar *why = NULL;

        if (!strict || runs(argv, &why))
            return g_strdup(candidate);

        g_string_append_printf(notes, "%s does not run: %s\n", candidate,
                               why != NULL ? why : "no reason given");
    }
    return NULL;
}

gboolean
clawt_display_resolve(ClawtDisplayBackend backend, const gchar *gowl_binary,
                      ClawtDisplayBackend *chosen, gchar **binary,
                      gchar **notes, GError **error)
{
    static const ClawtDisplayBackend auto_order[] = {
        CLAWT_DISPLAY_BACKEND_GOWL,
        CLAWT_DISPLAY_BACKEND_XVNC,
        CLAWT_DISPLAY_BACKEND_XVFB
    };
    g_autoptr(GString) skipped = g_string_new(NULL);
    gsize i;

    g_return_val_if_fail(chosen != NULL && binary != NULL, FALSE);

    *binary = NULL;
    if (notes != NULL)
        *notes = NULL;

    for (i = 0; i < G_N_ELEMENTS(auto_order); i++) {
        ClawtDisplayBackend candidate = auto_order[i];
        gchar *found = NULL;

        if (backend != CLAWT_DISPLAY_BACKEND_AUTO && backend != candidate)
            continue;

        switch (candidate) {
        case CLAWT_DISPLAY_BACKEND_GOWL:
            if (gowl_binary != NULL && *gowl_binary != '\0') {
                const gchar *argv[] = { gowl_binary, "--version", NULL };
                g_autofree gchar *why = NULL;

                if (runs(argv, &why))
                    found = g_strdup(gowl_binary);
                else
                    g_string_append_printf(skipped,
                                           "%s (computer.display.gowl_binary) "
                                           "does not run: %s\n", gowl_binary,
                                           why != NULL ? why : "no reason");
            } else {
                found = first_working("gowl", TRUE, skipped);
            }
            if (found == NULL)
                g_string_append(skipped, "gowl: none that runs\n");
            break;
        case CLAWT_DISPLAY_BACKEND_XVNC:
            found = first_working("Xvnc", FALSE, skipped);
            if (found == NULL)
                g_string_append(skipped, "Xvnc: not installed "
                                         "(tigervnc-server-minimal)\n");
            break;
        case CLAWT_DISPLAY_BACKEND_XVFB:
            found = first_working("Xvfb", FALSE, skipped);
            if (found == NULL)
                g_string_append(skipped, "Xvfb: not installed "
                                         "(xorg-x11-server-Xvfb)\n");
            break;
        case CLAWT_DISPLAY_BACKEND_AUTO:
            break;
        }

        if (found != NULL) {
            *chosen = candidate;
            *binary = found;
            if (notes != NULL && skipped->len > 0)
                *notes = g_strdup(g_strchomp(skipped->str));
            return TRUE;
        }
    }

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                "no virtual display server could be started for '%s': %s",
                clawt_enum_to_nick(CLAWT_TYPE_DISPLAY_BACKEND, backend),
                skipped->len > 0 ? g_strchomp(skipped->str) : "none found");
    if (notes != NULL && skipped->len > 0)
        *notes = g_strdup(skipped->str);
    return FALSE;
}

/* ── The server ──────────────────────────────────────────────────── */

struct _ClawtDisplayServer {
    GObject              parent_instance;

    ClawtDisplayBackend  asked;
    gchar               *dir;
    guint                width;
    guint                height;
    guint                depth;
    guint                vnc_port_asked;
    gchar               *gowl_binary;

    GMutex               lock;        /* the fields below */
    ClawtDisplayBackend  backend;
    GSubprocess         *server;
    GPtrArray           *apps;        /* GSubprocess*, started by launch */
    gchar               *display;     /* ":N" for X */
    gchar               *wayland;     /* absolute socket path for gowl */
    gchar               *run_dir;     /* gowl's XDG_RUNTIME_DIR */
    gchar               *mcp_socket;
    gchar               *ipc_socket;
    gchar               *fb_file;     /* Xvfb's framebuffer */
    guint                vnc_port;
    gchar               *notes;
    gchar               *log;
};

G_DEFINE_FINAL_TYPE(ClawtDisplayServer, clawt_display_server, G_TYPE_OBJECT)

static void stop_locked(ClawtDisplayServer *self);

static void
clawt_display_server_finalize(GObject *object)
{
    ClawtDisplayServer *self = CLAWT_DISPLAY_SERVER(object);

    g_mutex_lock(&self->lock);
    stop_locked(self);
    g_mutex_unlock(&self->lock);

    g_clear_pointer(&self->apps, g_ptr_array_unref);
    g_free(self->dir);
    g_free(self->gowl_binary);
    g_free(self->notes);
    g_free(self->log);
    g_mutex_clear(&self->lock);

    G_OBJECT_CLASS(clawt_display_server_parent_class)->finalize(object);
}

static void
clawt_display_server_class_init(ClawtDisplayServerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_display_server_finalize;
}

static void
clawt_display_server_init(ClawtDisplayServer *self)
{
    g_mutex_init(&self->lock);
    self->apps = g_ptr_array_new_with_free_func(g_object_unref);
    self->depth = 24;
}

ClawtDisplayServer *
clawt_display_server_new(ClawtDisplayBackend backend, const gchar *dir,
                         guint width, guint height)
{
    ClawtDisplayServer *self;

    g_return_val_if_fail(dir != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_DISPLAY_SERVER, NULL);
    self->asked = backend;
    self->backend = backend;
    self->dir = g_strdup(dir);
    self->width = width > 0 ? width : 1280;
    self->height = height > 0 ? height : 800;
    return self;
}

void
clawt_display_server_set_depth(ClawtDisplayServer *self, guint depth)
{
    g_return_if_fail(CLAWT_IS_DISPLAY_SERVER(self));

    self->depth = (depth == 16 || depth == 24 || depth == 32) ? depth : 24;
}

void
clawt_display_server_set_vnc_port(ClawtDisplayServer *self, guint port)
{
    g_return_if_fail(CLAWT_IS_DISPLAY_SERVER(self));

    self->vnc_port_asked = port;
}

void
clawt_display_server_set_gowl_binary(ClawtDisplayServer *self,
                                     const gchar        *path)
{
    g_return_if_fail(CLAWT_IS_DISPLAY_SERVER(self));

    g_free(self->gowl_binary);
    self->gowl_binary = (path != NULL && *path != '\0') ? g_strdup(path)
                                                        : NULL;
}

/* A free loopback port, for Xvnc when none was asked for. */
static guint
free_loopback_port(GError **error)
{
    g_autoptr(GSocket) socket = NULL;
    g_autoptr(GInetAddress) loopback = NULL;
    g_autoptr(GSocketAddress) any = NULL;
    g_autoptr(GSocketAddress) bound = NULL;

    socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM,
                          G_SOCKET_PROTOCOL_TCP, error);
    if (socket == NULL)
        return 0;
    loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    any = g_inet_socket_address_new(loopback, 0);
    if (!g_socket_bind(socket, any, FALSE, error))
        return 0;
    bound = g_socket_get_local_address(socket, error);
    if (bound == NULL)
        return 0;
    return g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
}

/* The last few lines a dead server wrote, for the reason it died. */
static gchar *
log_tail(const gchar *log)
{
    g_autofree gchar *text = NULL;
    g_auto(GStrv) lines = NULL;
    guint n;
    guint from;

    if (log == NULL || !g_file_get_contents(log, &text, NULL, NULL))
        return NULL;
    lines = g_strsplit(g_strstrip(text), "\n", -1);
    n = g_strv_length(lines);
    if (n == 0)
        return NULL;
    from = n > FAILURE_TAIL_LINES ? n - FAILURE_TAIL_LINES : 0;
    return g_strjoinv("\n", lines + from);
}

/* Waits for @paths to exist while @server lives. */
static gboolean
wait_for_paths(GSubprocess *server, const gchar * const *paths,
               const gchar *what, const gchar *log, GError **error)
{
    gint64 deadline = g_get_monotonic_time() +
                      (gint64)START_TIMEOUT_MS * 1000;

    while (g_get_monotonic_time() < deadline) {
        gboolean all = TRUE;
        guint i;

        for (i = 0; paths[i] != NULL; i++)
            all = all && g_file_test(paths[i], G_FILE_TEST_EXISTS);
        if (all)
            return TRUE;
        if (!clawt_display_child_alive(server))
            break;
        g_usleep(50 * 1000);
    }

    {
        g_autofree gchar *tail = log_tail(log);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "%s %s%s%s", what,
                    clawt_display_child_alive(server)
                        ? "did not open its sockets in time"
                        : "exited while starting",
                    tail != NULL ? ": " : "", tail != NULL ? tail : "");
    }
    return FALSE;
}

/* The wayland socket gowl made in its runtime directory. */
static gchar *
find_wayland_socket(const gchar *run_dir)
{
    g_autoptr(GDir) dir = g_dir_open(run_dir, 0, NULL);
    const gchar *entry;

    while (dir != NULL && (entry = g_dir_read_name(dir)) != NULL) {
        if (g_str_has_prefix(entry, "wayland-") &&
            !g_str_has_suffix(entry, ".lock"))
            return g_build_filename(run_dir, entry, NULL);
    }
    return NULL;
}

/*
 * A log the daemon writes for a child, opened here rather than by name in
 * GLib (0.51.4).  Not through a symlink and only as a regular file: the
 * daemon is unconfined, so a link planted where the log goes would have
 * it write -- with output an agent chooses -- wherever the link points.
 * The display's directory is outside every box now, which closes that
 * on its own; this keeps it closed if a directory is ever shared again.
 */
static gint
open_log(const gchar *path, GError **error)
{
    struct stat st;
    gint fd = g_open(path, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW |
                               O_CLOEXEC | O_NOCTTY, 0600);

    if (fd < 0) {
        gint saved = errno;

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "could not open the log %s: %s%s", path,
                    g_strerror(saved),
                    saved == ELOOP ? " (it is a symlink; refused)" : "");
        return -1;
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "the log %s is not a regular file; refused", path);
        close(fd);
        return -1;
    }
    return fd;
}

static gboolean
start_x(ClawtDisplayServer *self, const gchar *binary, GError **error)
{
    gint log_fd;
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSubprocess) server = NULL;
    g_autoptr(GHashTable) extra = g_hash_table_new(g_str_hash, g_str_equal);
    g_auto(GStrv) environment = NULL;
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *fbdir = NULL;
    gint number;

    if (self->backend == CLAWT_DISPLAY_BACKEND_XVFB) {
        fbdir = g_build_filename(self->dir, "fb", NULL);
        if (g_mkdir_with_parents(fbdir, 0700) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                        "could not create %s: %s", fbdir, g_strerror(errno));
            return FALSE;
        }
    } else {
        self->vnc_port = self->vnc_port_asked > 0 ? self->vnc_port_asked
                                                  : free_loopback_port(error);
        if (self->vnc_port == 0)
            return FALSE;
    }

    argv = clawt_display_server_argv(self->backend, binary, self->width,
                                     self->height, self->depth, fbdir,
                                     self->vnc_port, NULL);

    log_fd = open_log(self->log, error);
    if (log_fd < 0)
        return FALSE;
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    g_subprocess_launcher_take_stderr_fd(launcher, log_fd);
    environment = clawt_build_child_environment(extra);
    g_subprocess_launcher_set_environ(launcher, environment);

    server = clawt_display_spawn_lasting(launcher,
                                         (const gchar * const *)argv, error);
    if (server == NULL)
        return FALSE;

    number = clawt_display_read_displayfd(server, START_TIMEOUT_MS, error);
    if (number < 0) {
        g_autofree gchar *tail = log_tail(self->log);

        clawt_display_end_child(server);
        if (tail != NULL && error != NULL && *error != NULL)
            g_prefix_error(error, "%s: ", tail);
        return FALSE;
    }

    self->display = g_strdup_printf(":%d", number);
    if (fbdir != NULL)
        self->fb_file = g_build_filename(fbdir, "Xvfb_screen0", NULL);
    self->server = g_steal_pointer(&server);
    return TRUE;
}

static gboolean
start_gowl(ClawtDisplayServer *self, const gchar *binary, GError **error)
{
    gint log_fd;
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSubprocess) server = NULL;
    g_autoptr(GHashTable) extra = NULL;
    g_auto(GStrv) environment = NULL;
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *config = g_build_filename(self->dir, "gowl.yaml", NULL);
    g_autofree gchar *yaml = clawt_display_gowl_config();
    const gchar *sockets[3];

    self->run_dir = g_build_filename(self->dir, "run", NULL);
    if (g_mkdir_with_parents(self->run_dir, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "could not create %s: %s", self->run_dir,
                    g_strerror(errno));
        return FALSE;
    }
    if (!g_file_set_contents(config, yaml, -1, error))
        return FALSE;

    self->mcp_socket = g_build_filename(self->run_dir, "gowl-mcp.sock", NULL);
    self->ipc_socket = g_build_filename(self->run_dir, "gowl.sock", NULL);
    if (!clawt_check_socket_path(self->mcp_socket, error))
        return FALSE;

    extra = clawt_display_gowl_environment(self->run_dir, self->width,
                                           self->height);
    environment = clawt_build_child_environment(extra);
    argv = clawt_display_server_argv(CLAWT_DISPLAY_BACKEND_GOWL, binary,
                                     self->width, self->height, self->depth,
                                     NULL, 0, config);

    log_fd = open_log(self->log, error);
    if (log_fd < 0)
        return FALSE;
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE);
    g_subprocess_launcher_take_stderr_fd(launcher, log_fd);
    g_subprocess_launcher_set_environ(launcher, environment);
    /*
     * Its working directory is the one its MCP spawn hands to apps, so
     * the home directory, where an X app starts too -- not the display's
     * own state directory, which is where a terminal opened first.
     */
    g_subprocess_launcher_set_cwd(launcher, g_get_home_dir());

    server = clawt_display_spawn_lasting(launcher,
                                         (const gchar * const *)argv, error);
    if (server == NULL)
        return FALSE;

    sockets[0] = self->mcp_socket;
    sockets[1] = self->ipc_socket;
    sockets[2] = NULL;
    if (!wait_for_paths(server, sockets, "gowl", self->log, error)) {
        clawt_display_end_child(server);
        return FALSE;
    }

    self->wayland = find_wayland_socket(self->run_dir);
    if (self->wayland == NULL) {
        clawt_display_end_child(server);
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                            "gowl started but made no wayland socket");
        return FALSE;
    }

    self->server = g_steal_pointer(&server);
    return TRUE;
}

gboolean
clawt_display_server_start(ClawtDisplayServer *self, GError **error)
{
    g_autofree gchar *binary = NULL;
    g_autofree gchar *notes = NULL;
    ClawtDisplayBackend chosen = CLAWT_DISPLAY_BACKEND_AUTO;
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), FALSE);

    g_mutex_lock(&self->lock);

    if (clawt_display_child_alive(self->server)) {
        g_mutex_unlock(&self->lock);
        return TRUE;
    }
    stop_locked(self);

    if (!clawt_display_resolve(self->asked, self->gowl_binary, &chosen,
                               &binary, &notes, error)) {
        g_free(self->notes);
        self->notes = g_steal_pointer(&notes);
        g_mutex_unlock(&self->lock);
        return FALSE;
    }
    g_free(self->notes);
    self->notes = g_steal_pointer(&notes);
    self->backend = chosen;

    if (g_mkdir_with_parents(self->dir, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "could not create %s: %s", self->dir, g_strerror(errno));
        g_mutex_unlock(&self->lock);
        return FALSE;
    }
    g_free(self->log);
    self->log = g_build_filename(self->dir, "server.log", NULL);

    ok = (chosen == CLAWT_DISPLAY_BACKEND_GOWL)
        ? start_gowl(self, binary, error)
        : start_x(self, binary, error);

    if (!ok)
        stop_locked(self);
    g_mutex_unlock(&self->lock);
    return ok;
}

/* Called with lock held. */
static void
stop_locked(ClawtDisplayServer *self)
{
    guint i;

    /* The apps first: they are clients of the server. */
    for (i = 0; self->apps != NULL && i < self->apps->len; i++)
        clawt_display_end_child(g_ptr_array_index(self->apps, i));
    if (self->apps != NULL)
        g_ptr_array_set_size(self->apps, 0);

    clawt_display_end_child(self->server);
    g_clear_object(&self->server);

    /* What it left: sockets, the framebuffer file, the generated config. */
    if (self->dir != NULL && g_file_test(self->dir, G_FILE_TEST_IS_DIR)) {
        g_autofree gchar *run = g_build_filename(self->dir, "run", NULL);
        g_autofree gchar *fb = g_build_filename(self->dir, "fb", NULL);

        clawt_remove_tree(run, self->dir, NULL);
        clawt_remove_tree(fb, self->dir, NULL);
    }

    g_clear_pointer(&self->display, g_free);
    g_clear_pointer(&self->wayland, g_free);
    g_clear_pointer(&self->run_dir, g_free);
    g_clear_pointer(&self->mcp_socket, g_free);
    g_clear_pointer(&self->ipc_socket, g_free);
    g_clear_pointer(&self->fb_file, g_free);
    self->vnc_port = 0;
}

void
clawt_display_server_stop(ClawtDisplayServer *self)
{
    g_return_if_fail(CLAWT_IS_DISPLAY_SERVER(self));

    g_mutex_lock(&self->lock);
    stop_locked(self);
    g_mutex_unlock(&self->lock);
}

gboolean
clawt_display_server_is_running(ClawtDisplayServer *self)
{
    gboolean alive;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), FALSE);

    g_mutex_lock(&self->lock);
    alive = clawt_display_child_alive(self->server);
    g_mutex_unlock(&self->lock);
    return alive;
}

ClawtDisplayBackend
clawt_display_server_get_backend(ClawtDisplayServer *self)
{
    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self),
                         CLAWT_DISPLAY_BACKEND_AUTO);

    return self->backend;
}

void
clawt_display_server_get_size(ClawtDisplayServer *self, guint *width,
                              guint *height)
{
    g_return_if_fail(CLAWT_IS_DISPLAY_SERVER(self));

    if (width != NULL)
        *width = self->width;
    if (height != NULL)
        *height = self->height;
}

GHashTable *
clawt_display_server_dup_environment(ClawtDisplayServer *self)
{
    GHashTable *env = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                            g_free);

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), env);

    g_mutex_lock(&self->lock);
    if (self->display != NULL) {
        g_hash_table_insert(env, g_strdup("DISPLAY"),
                            g_strdup(self->display));
        /* an X screen: toolkits must not wander to the operator's Wayland */
        g_hash_table_insert(env, g_strdup("WAYLAND_DISPLAY"),
                            g_strdup("no-wayland-here"));
        g_hash_table_insert(env, g_strdup("GDK_BACKEND"), g_strdup("x11"));
        g_hash_table_insert(env, g_strdup("QT_QPA_PLATFORM"),
                            g_strdup("xcb"));
    }
    if (self->wayland != NULL) {
        /*
         * An absolute path: libwayland takes one, and it survives a
         * sandbox that replaces XDG_RUNTIME_DIR with a private one.
         */
        g_hash_table_insert(env, g_strdup("WAYLAND_DISPLAY"),
                            g_strdup(self->wayland));
        g_hash_table_insert(env, g_strdup("GDK_BACKEND"),
                            g_strdup("wayland"));
        g_hash_table_insert(env, g_strdup("QT_QPA_PLATFORM"),
                            g_strdup("wayland"));
        /* gowl's XWayland is started by gowl, for apps it spawns */
        g_hash_table_insert(env, g_strdup("DISPLAY"), g_strdup(""));
    }
    g_mutex_unlock(&self->lock);
    return env;
}

GStrv
clawt_display_server_dup_bind_paths(ClawtDisplayServer *self)
{
    g_autoptr(GStrvBuilder) out = g_strv_builder_new();

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), NULL);

    g_mutex_lock(&self->lock);
    if (self->display != NULL) {
        g_autofree gchar *socket = g_strdup_printf("/tmp/.X11-unix/X%s",
                                                   self->display + 1);

        g_strv_builder_add(out, socket);
    }
    /*
     * gowl's Wayland socket and nothing else of its runtime directory
     * (0.51.4): gowl.sock and gowl-mcp.sock beside it both have a
     * `spawn` that runs a command as the operator, outside any box.
     */
    if (self->wayland != NULL)
        g_strv_builder_add(out, self->wayland);
    g_mutex_unlock(&self->lock);
    return g_strv_builder_end(out);
}

gchar *
clawt_display_server_dup_notes(ClawtDisplayServer *self)
{
    gchar *notes;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), NULL);

    g_mutex_lock(&self->lock);
    notes = g_strdup(self->notes);
    g_mutex_unlock(&self->lock);
    return notes;
}

gchar *
clawt_display_server_dup_failure(ClawtDisplayServer *self)
{
    gchar *tail = NULL;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), NULL);

    g_mutex_lock(&self->lock);
    if (self->server != NULL && !clawt_display_child_alive(self->server))
        tail = log_tail(self->log);
    g_mutex_unlock(&self->lock);
    return tail;
}

const gchar *
clawt_display_server_get_mcp_socket(ClawtDisplayServer *self)
{
    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), NULL);

    return self->mcp_socket;
}

gchar *
clawt_display_server_dup_viewer_uri(ClawtDisplayServer *self)
{
    gchar *uri = NULL;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), NULL);

    g_mutex_lock(&self->lock);
    if (self->backend == CLAWT_DISPLAY_BACKEND_XVNC && self->vnc_port > 0 &&
        clawt_display_child_alive(self->server))
        uri = g_strdup_printf("vnc://127.0.0.1:%u", self->vnc_port);
    g_mutex_unlock(&self->lock);
    return uri;
}

/* Refuses with a sentence when nothing is running. */
static gboolean
check_running(ClawtDisplayServer *self, GError **error)
{
    if (clawt_display_child_alive(self->server))
        return TRUE;

    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                        "the virtual display is not running");
    return FALSE;
}

/* Runs an X tool against this display and returns what it printed. */
static GBytes *
run_x_tool(ClawtDisplayServer *self, const gchar * const *argv,
           GError **error)
{
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSubprocess) child = NULL;
    g_autoptr(GHashTable) extra = g_hash_table_new(g_str_hash, g_str_equal);
    g_auto(GStrv) environment = NULL;
    g_autoptr(GBytes) out = NULL;
    g_autoptr(GBytes) err = NULL;

    g_hash_table_insert(extra, (gpointer)"DISPLAY", self->display);
    environment = clawt_build_child_environment(extra);

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_environ(launcher, environment);
    child = g_subprocess_launcher_spawnv(launcher, argv, error);
    if (child == NULL)
        return NULL;
    if (!g_subprocess_communicate(child, NULL, NULL, &out, &err, error))
        return NULL;

    if (!g_subprocess_get_if_exited(child) ||
        g_subprocess_get_exit_status(child) != 0) {
        g_autofree gchar *text = (err != NULL)
            ? g_strndup(g_bytes_get_data(err, NULL), g_bytes_get_size(err))
            : NULL;

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s failed: %s",
                    argv[0], (text != NULL && *g_strstrip(text) != '\0')
                                 ? text : "no reason given");
        return NULL;
    }
    return g_steal_pointer(&out);
}

/* A tool the X backends need, with the package that has it. */
static gchar *
need_tool(const gchar *name, const gchar *package, const gchar *for_what,
          GError **error)
{
    gchar *path = g_find_program_in_path(name);

    if (path == NULL)
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                    "%s needs %s, which is not installed (Fedora package "
                    "%s)", for_what, name, package);
    return path;
}

GBytes *
clawt_display_server_capture(ClawtDisplayServer *self, GError **error)
{
    GBytes *png = NULL;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), NULL);

    g_mutex_lock(&self->lock);
    if (!check_running(self, error)) {
        g_mutex_unlock(&self->lock);
        return NULL;
    }

    switch (self->backend) {
    case CLAWT_DISPLAY_BACKEND_XVFB: {
        g_autoptr(GMappedFile) mapped =
            g_mapped_file_new(self->fb_file, FALSE, error);

        if (mapped != NULL)
            png = clawt_display_xwd_to_png(
                (const guint8 *)g_mapped_file_get_contents(mapped),
                g_mapped_file_get_length(mapped), error);
        break;
    }

    case CLAWT_DISPLAY_BACKEND_XVNC: {
        g_autofree gchar *import = need_tool("import", "ImageMagick",
                                             "a picture of an Xvnc display",
                                             error);
        g_auto(GStrv) argv = NULL;

        if (import == NULL)
            break;
        argv = clawt_display_import_argv(import, self->display);
        png = run_x_tool(self, (const gchar * const *)argv, error);
        break;
    }

    case CLAWT_DISPLAY_BACKEND_GOWL: {
        g_autoptr(JsonNode) result = clawt_mcp_socket_call(
            self->mcp_socket, "screenshot_monitor", NULL, CALL_TIMEOUT_S,
            error);

        if (result != NULL) {
            png = clawt_mcp_socket_result_image(result);
            if (png == NULL) {
                g_autofree gchar *text = clawt_mcp_socket_result_text(result);

                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "gowl answered with no picture: %s",
                            text != NULL ? text : "nothing at all");
            }
        }
        break;
    }

    case CLAWT_DISPLAY_BACKEND_AUTO:
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "the virtual display has not started");
        break;
    }

    g_mutex_unlock(&self->lock);
    return png;
}

gboolean
clawt_display_server_send_input(ClawtDisplayServer *self,
                                ClawtInputEvent    *event,
                                GError            **error)
{
    gboolean ok = FALSE;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), FALSE);
    g_return_val_if_fail(event != NULL, FALSE);

    g_mutex_lock(&self->lock);
    if (!check_running(self, error)) {
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    if (self->backend == CLAWT_DISPLAY_BACKEND_GOWL) {
        g_autoptr(JsonNode) arguments = NULL;
        g_autoptr(JsonNode) result = NULL;
        const gchar *tool;

        /*
         * A click is a move then a press: gowl's send_mouse acts where
         * the pointer already is, and that is wherever it was left.
         */
        if (event->kind == CLAWT_INPUT_CLICK) {
            g_autoptr(ClawtInputEvent) move =
                clawt_input_event_new(CLAWT_INPUT_MOVE);
            g_autoptr(JsonNode) move_arguments = NULL;
            g_autoptr(JsonNode) moved = NULL;

            move->x = event->x;
            move->y = event->y;
            moved = clawt_mcp_socket_call(
                self->mcp_socket,
                clawt_screen_gowl_input_tool(move, &move_arguments),
                move_arguments, CALL_TIMEOUT_S, error);
            if (moved == NULL) {
                g_mutex_unlock(&self->lock);
                return FALSE;
            }
        }

        tool = clawt_screen_gowl_input_tool(event, &arguments);
        if (tool == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "there is nothing to send: a key or a "
                                "string was asked for and none was given");
        } else {
            result = clawt_mcp_socket_call(self->mcp_socket, tool, arguments,
                                           CALL_TIMEOUT_S, error);
            ok = result != NULL;
        }
    } else {
        g_autofree gchar *xdotool = need_tool("xdotool", "xdotool",
                                              "typing and clicking on an X "
                                              "display", error);
        g_auto(GStrv) argv = NULL;

        if (xdotool != NULL) {
            argv = clawt_display_xdotool_argv(xdotool, event);
            if (argv == NULL) {
                g_set_error_literal(error, CLAWT_ERROR,
                                    CLAWT_ERROR_INVALID_ARGUMENT,
                                    "there is nothing to send: a key or a "
                                    "string was asked for and none was "
                                    "given");
            } else {
                g_autoptr(GBytes) out =
                    run_x_tool(self, (const gchar * const *)argv, error);

                ok = out != NULL;
            }
        }
    }

    g_mutex_unlock(&self->lock);
    return ok;
}

/* One JSON-RPC call to gowl's IPC socket, one line out, one line back. */
static JsonNode *
gowl_ipc_call(const gchar *socket_path, const gchar *method, JsonNode *params,
              GError **error)
{
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketAddress) address =
        g_unix_socket_address_new(socket_path);
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) request = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GDataInputStream) in = NULL;
    g_autofree gchar *line = NULL;
    g_autofree gchar *text = NULL;
    JsonObject *reply;
    gsize length = 0;

    g_socket_client_set_timeout(client, CALL_TIMEOUT_S);
    connection = g_socket_client_connect(client,
                                         G_SOCKET_CONNECTABLE(address), NULL,
                                         error);
    if (connection == NULL)
        return NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    json_builder_set_member_name(builder, "id");
    json_builder_add_int_value(builder, 1);
    json_builder_set_member_name(builder, "method");
    json_builder_add_string_value(builder, method);
    if (params != NULL) {
        json_builder_set_member_name(builder, "params");
        json_builder_add_value(builder, json_node_copy(params));
    }
    json_builder_end_object(builder);
    request = json_builder_get_root(builder);
    text = json_to_string(request, FALSE);

    if (!g_output_stream_write_all(
            g_io_stream_get_output_stream(G_IO_STREAM(connection)), text,
            strlen(text), NULL, NULL, error) ||
        !g_output_stream_write_all(
            g_io_stream_get_output_stream(G_IO_STREAM(connection)), "\n", 1,
            NULL, NULL, error))
        return NULL;

    in = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    line = g_data_input_stream_read_line_utf8(in, &length, NULL, error);
    if (line == NULL) {
        if (error != NULL && *error == NULL)
            g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                                "gowl closed the connection without an "
                                "answer");
        return NULL;
    }
    if (!json_parser_load_from_data(parser, line, (gssize)length, error))
        return NULL;
    if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "gowl's answer was not an object");
        return NULL;
    }

    reply = json_node_get_object(json_parser_get_root(parser));
    if (json_object_has_member(reply, "error")) {
        JsonObject *e = json_object_get_object_member(reply, "error");

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "gowl refused %s: %s",
                    method, e != NULL
                        ? json_object_get_string_member_with_default(
                              e, "message", "no reason given")
                        : "no reason given");
        return NULL;
    }
    if (!json_object_has_member(reply, "result")) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "gowl answered %s with no result", method);
        return NULL;
    }
    return json_node_copy(json_object_get_member(reply, "result"));
}

/* The Landlock scope bwrap cannot apply, in the app's child. */
static void
sandbox_child_setup(gpointer user_data)
{
    clawt_sandbox_enter((ClawtSandbox *)user_data);
}

/*
 * An app started here, not through a compositor: the shell with the
 * screen's variables, inside @sandbox when there is one, kept so it
 * stops with the server.  Called with lock held.
 */
static gboolean
spawn_app(ClawtDisplayServer *self, const gchar *command,
          GHashTable *environment, ClawtSandbox *sandbox, gint *pid_out,
          GError **error)
{
    gint log_fd;
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_autoptr(GSubprocess) child = NULL;
    g_autoptr(GHashTable) extra = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    g_auto(GStrv) env = NULL;
    g_auto(GStrv) argv = NULL;
    g_autofree gchar *log = g_build_filename(self->dir, "apps.log", NULL);
    const gchar *shell[] = { "/bin/sh", "-c", command, NULL };
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    if (environment != NULL) {
        g_hash_table_iter_init(&iter, environment);
        while (g_hash_table_iter_next(&iter, &key, &value))
            g_hash_table_insert(extra, g_strdup(key), g_strdup(value));
    }

    /* The screen's own variables win over anything passed in. */
    if (self->display != NULL) {
        g_hash_table_insert(extra, g_strdup("DISPLAY"),
                            g_strdup(self->display));
        g_hash_table_insert(extra, g_strdup("WAYLAND_DISPLAY"),
                            g_strdup("no-wayland-here"));
        g_hash_table_insert(extra, g_strdup("GDK_BACKEND"), g_strdup("x11"));
        g_hash_table_insert(extra, g_strdup("QT_QPA_PLATFORM"),
                            g_strdup("xcb"));
    } else if (self->wayland != NULL) {
        g_hash_table_insert(extra, g_strdup("WAYLAND_DISPLAY"),
                            g_strdup(self->wayland));
        g_hash_table_insert(extra, g_strdup("GDK_BACKEND"),
                            g_strdup("wayland"));
        g_hash_table_insert(extra, g_strdup("QT_QPA_PLATFORM"),
                            g_strdup("wayland"));
        g_hash_table_insert(extra, g_strdup("DISPLAY"), g_strdup(""));
    }
    env = clawt_build_child_environment(extra);

    /*
     * The same checks and the same box as a command the agent runs: an
     * app is a command that happens to open a window.
     */
    if (sandbox != NULL) {
        g_auto(GStrv) boxed = NULL;

        if (!clawt_sandbox_check_argv(sandbox, shell, error))
            return FALSE;
        argv = clawt_sandbox_wrap_argv(sandbox, shell);
        boxed = clawt_sandbox_wrap_environment(sandbox,
                                               (const gchar * const *)env);
        g_strfreev(env);
        env = g_steal_pointer(&boxed);
    } else {
        argv = g_strdupv((gchar **)shell);
    }

    log_fd = open_log(log, error);
    if (log_fd < 0)
        return FALSE;
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_INHERIT);
    g_subprocess_launcher_set_environ(launcher, env);
    g_subprocess_launcher_take_stderr_fd(launcher, dup(log_fd));
    g_subprocess_launcher_take_stdout_fd(launcher, log_fd);
    g_subprocess_launcher_set_cwd(launcher, g_get_home_dir());

    child = spawn_lasting_with(launcher, (const gchar * const *)argv,
                               sandbox != NULL ? sandbox_child_setup : NULL,
                               sandbox, error);
    if (child == NULL)
        return FALSE;

    /*
     * Started, whatever the pid says.  GLib reaps a child on its worker
     * thread and forgets its identifier then, so an app that is already
     * done -- a startup `echo once >> file` -- has none by the time it is
     * asked for.  Reading "no pid" as "did not start" logged a failure
     * for an app that had run ("did not start: no reason given") and
     * failed /display/computer/commands-and-startup under load.  The pid
     * is 0 then, as it is for gowl's MCP spawn.
     */
    {
        const gchar *id = g_subprocess_get_identifier(child);

        *pid_out = (id != NULL) ? (gint)g_ascii_strtoll(id, NULL, 10) : 0;
        g_ptr_array_add(self->apps, g_steal_pointer(&child));
        return TRUE;
    }
}

gboolean
clawt_display_server_launch(ClawtDisplayServer *self, const gchar *command,
                            GHashTable *environment, ClawtSandbox *sandbox,
                            gint *pid_out, GError **error)
{
    gboolean boxed;
    gboolean ok = FALSE;
    gint pid = 0;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), FALSE);
    g_return_val_if_fail(command != NULL, FALSE);

    if (pid_out != NULL)
        *pid_out = 0;

    if (*command == '\0') {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "launch what? the command is empty");
        return FALSE;
    }

    boxed = sandbox != NULL &&
            clawt_sandbox_get_mode(sandbox) == CLAWT_CONFINE_BWRAP;

    g_mutex_lock(&self->lock);
    if (!check_running(self, error)) {
        g_mutex_unlock(&self->lock);
        return FALSE;
    }

    if (self->backend == CLAWT_DISPLAY_BACKEND_GOWL && !boxed) {
        /*
         * Through gowl's own spawn: the app inherits WAYLAND_DISPLAY and
         * the DISPLAY of gowl's XWayland, which nothing outside gowl
         * knows, so an X app works too.  It dies with gowl.  The argv
         * check still applies, as it does to any command the agent runs.
         */
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(JsonNode) params = NULL;
        g_autoptr(JsonNode) result = NULL;
        g_autoptr(GError) rpc_error = NULL;
        const gchar *shell[] = { "/bin/sh", "-c", command, NULL };

        if (sandbox != NULL &&
            !clawt_sandbox_check_argv(sandbox, shell, error)) {
            g_mutex_unlock(&self->lock);
            return FALSE;
        }

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "command");
        json_builder_add_string_value(builder, command);
        json_builder_set_member_name(builder, "cwd");
        json_builder_add_string_value(builder, g_get_home_dir());
        if (environment != NULL && g_hash_table_size(environment) > 0) {
            GHashTableIter iter;
            gpointer key;
            gpointer value;

            json_builder_set_member_name(builder, "env");
            json_builder_begin_object(builder);
            g_hash_table_iter_init(&iter, environment);
            while (g_hash_table_iter_next(&iter, &key, &value)) {
                json_builder_set_member_name(builder, key);
                json_builder_add_string_value(builder, value);
            }
            json_builder_end_object(builder);
        }
        json_builder_end_object(builder);
        params = json_builder_get_root(builder);

        /*
         * JSON-RPC first, for its pid; a gowl that predates it (0.5.0
         * speaks only the text dialect on gowl.sock) is asked through its
         * MCP spawn tool instead, which starts the app and says nothing
         * of its pid.
         */
        result = gowl_ipc_call(self->ipc_socket, "spawn", params, &rpc_error);
        if (result != NULL) {
            if (JSON_NODE_HOLDS_OBJECT(result))
                pid = (gint)json_object_get_int_member_with_default(
                    json_node_get_object(result), "pid", 0);
            ok = TRUE;
        } else {
            g_autoptr(JsonNode) arguments = json_node_new(JSON_NODE_OBJECT);
            g_autoptr(JsonObject) object = json_object_new();
            g_autoptr(JsonNode) spawned = NULL;
            g_autoptr(GError) mcp_error = NULL;

            json_object_set_string_member(object, "command", command);
            json_node_set_object(arguments, object);
            spawned = clawt_mcp_socket_call(self->mcp_socket, "spawn",
                                            arguments, CALL_TIMEOUT_S,
                                            &mcp_error);
            if (spawned != NULL)
                ok = TRUE;
            else
                g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                            "gowl would not start it: %s (and over its IPC: "
                            "%s)", mcp_error->message, rpc_error->message);
        }
    } else {
        ok = spawn_app(self, command, environment, sandbox, &pid, error);
    }

    g_mutex_unlock(&self->lock);
    if (ok && pid_out != NULL)
        *pid_out = pid;
    return ok;
}

/* gowl's MCP text answer, as JSON when it is JSON. */
static JsonNode *
gowl_tool_json(const gchar *socket_path, const gchar *tool, GError **error)
{
    g_autoptr(JsonNode) result = clawt_mcp_socket_call(socket_path, tool,
                                                       NULL, CALL_TIMEOUT_S,
                                                       error);
    g_autofree gchar *text = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();

    if (result == NULL)
        return NULL;
    text = clawt_mcp_socket_result_text(result);
    if (text != NULL && json_parser_load_from_data(parser, text, -1, NULL))
        return json_node_copy(json_parser_get_root(parser));

    {
        JsonNode *node = json_node_new(JSON_NODE_VALUE);

        json_node_set_string(node, text != NULL ? text : "");
        return node;
    }
}

JsonNode *
clawt_display_server_list_windows(ClawtDisplayServer *self, GError **error)
{
    JsonNode *out = NULL;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), NULL);

    g_mutex_lock(&self->lock);
    if (!check_running(self, error)) {
        g_mutex_unlock(&self->lock);
        return NULL;
    }

    if (self->backend == CLAWT_DISPLAY_BACKEND_GOWL) {
        out = gowl_tool_json(self->mcp_socket, "list_clients", error);
    } else {
        g_autofree gchar *xdotool = need_tool("xdotool", "xdotool",
                                              "listing windows", error);
        g_autoptr(JsonArray) windows = json_array_new();

        if (xdotool != NULL) {
            const gchar *search[] = { xdotool, "search", "--onlyvisible",
                                      "--name", "", NULL };
            g_autoptr(GError) none = NULL;
            g_autoptr(GBytes) ids = run_x_tool(self, search, &none);
            g_autofree gchar *text = NULL;
            g_auto(GStrv) lines = NULL;
            guint i;

            /* No window at all is exit 1 from xdotool, not a failure. */
            text = (ids != NULL) ? g_strndup(g_bytes_get_data(ids, NULL),
                                             g_bytes_get_size(ids))
                                 : g_strdup("");
            lines = g_strsplit(g_strstrip(text), "\n", -1);
            for (i = 0; lines[i] != NULL; i++) {
                const gchar *name_argv[] = { xdotool, "getwindowname",
                                             lines[i], NULL };
                g_autoptr(GBytes) name = NULL;
                g_autofree gchar *title = NULL;
                JsonObject *window;

                if (*lines[i] == '\0')
                    continue;
                name = run_x_tool(self, name_argv, NULL);
                title = (name != NULL)
                    ? g_strndup(g_bytes_get_data(name, NULL),
                                g_bytes_get_size(name))
                    : g_strdup("");
                g_strstrip(title);
                if (*title == '\0')
                    continue;
                window = json_object_new();
                json_object_set_string_member(window, "id", lines[i]);
                json_object_set_string_member(window, "title", title);
                json_array_add_object_element(windows, window);
            }
            out = json_node_new(JSON_NODE_ARRAY);
            json_node_set_array(out, windows);
        }
    }

    g_mutex_unlock(&self->lock);
    return out;
}

gchar *
clawt_display_server_dup_focused_title(ClawtDisplayServer *self)
{
    gchar *title = NULL;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(self), NULL);

    g_mutex_lock(&self->lock);
    if (!clawt_display_child_alive(self->server)) {
        g_mutex_unlock(&self->lock);
        return NULL;
    }

    if (self->backend == CLAWT_DISPLAY_BACKEND_GOWL) {
        g_autoptr(JsonNode) focused =
            gowl_tool_json(self->mcp_socket, "get_focused_client", NULL);

        if (focused != NULL && JSON_NODE_HOLDS_OBJECT(focused))
            title = g_strdup(json_object_get_string_member_with_default(
                json_node_get_object(focused), "title", NULL));
    } else {
        g_autofree gchar *xdotool = g_find_program_in_path("xdotool");

        if (xdotool != NULL) {
            const gchar *argv[] = { xdotool, "getwindowfocus",
                                    "getwindowname", NULL };
            g_autoptr(GBytes) out = run_x_tool(self, argv, NULL);

            if (out != NULL) {
                title = g_strndup(g_bytes_get_data(out, NULL),
                                  g_bytes_get_size(out));
                g_strstrip(title);
            }
        }
    }

    g_mutex_unlock(&self->lock);
    return title;
}
