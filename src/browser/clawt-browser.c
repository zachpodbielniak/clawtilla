/*
 * clawt-browser.c - A real web browser an agent can drive
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A session is Xvfb plus gsurf, started together and stopped together.
 * The header says why; this file is the plumbing:
 *
 * - Xvfb picks its own display with `-displayfd`, so two sessions (or a
 *   session and the person's own Xvfb) never race for :99.
 * - gsurf is given a generated config naming its profile, its cache and
 *   its control socket (`control.name`), and runs with `--no-modules`:
 *   the modal module would take an agent's typed `j` as "scroll down".
 * - Its environment is the shared allowlist plus DISPLAY and GDK_BACKEND,
 *   never the daemon's.  WAYLAND_DISPLAY is named as a socket that does
 *   not exist, because GTK finds `wayland-0` in XDG_RUNTIME_DIR on its own
 *   and a "headless" session would open on the person's screen.
 * - Both children die with the daemon (PR_SET_PDEATHSIG), or a killed
 *   daemon leaves browsers holding profiles the next one wants.
 */

#include "clawtilla.h"

#include <errno.h>
#include <stdio.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixsocketaddress.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

/* A reply bigger than this is refused while it is read, not after. */
#define REPLY_MAX_BYTES     (64 * 1024 * 1024)
#define START_TIMEOUT_MS    (20000)
#define STOP_WAIT_MS        (3000)

gboolean
clawt_browser_build_available(void)
{
#ifdef CLAWT_HAVE_GSURF
    return TRUE;
#else
    return FALSE;
#endif
}

/* Where clawtillad is, so a gsurf installed beside it is found first. */
static gchar *
executable_dir(void)
{
    g_autofree gchar *exe = g_file_read_link("/proc/self/exe", NULL);

    return exe != NULL ? g_path_get_dirname(exe) : NULL;
}

/* The oldest gsurf with page.click, page.type, page.key and an inline screenshot. */
#define GSURF_MIN_MAJOR (0)
#define GSURF_MIN_MINOR (5)
#define GSURF_MIN_MICRO (10)

/*
 * Whether @path is a gsurf new enough for the tools, from its --version.
 * A gsurf too old has no page.click and no control.name, so it starts,
 * opens a window, and its control socket never appears under the name
 * asked for -- twenty seconds of "did not answer" for a version problem.
 */
static gboolean
gsurf_new_enough(const gchar *path, gchar **out_version)
{
    const gchar *argv[] = { path, "--version", NULL };
    g_autofree gchar *out = NULL;
    gint status = 0;
    guint major = 0;
    guint minor = 0;
    guint micro = 0;

    if (!g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL,
                      &out, NULL, &status, NULL) || out == NULL)
        return FALSE;
    if (sscanf(out, "gsurf %u.%u.%u", &major, &minor, &micro) != 3)
        return FALSE;
    if (out_version != NULL)
        *out_version = g_strdup_printf("%u.%u.%u", major, minor, micro);
    if (major != GSURF_MIN_MAJOR)
        return major > GSURF_MIN_MAJOR;
    if (minor != GSURF_MIN_MINOR)
        return minor > GSURF_MIN_MINOR;
    return micro >= GSURF_MIN_MICRO;
}

gchar *
clawt_browser_resolve_gsurf(GError **error)
{
    g_autofree gchar *exe_dir = executable_dir();
    g_autoptr(GPtrArray) candidates = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) tried = g_ptr_array_new_with_free_func(g_free);
    g_autofree gchar *joined = NULL;
    gchar *on_path;
    guint i;

    /*
     * Beside the binary, the install location, the GSURF_DIR build this
     * binary was checked against, then PATH.  The build comes before PATH
     * on purpose: a distribution's gsurf on PATH is whatever version the
     * image happened to ship, while GSURF_DIR is the one `make GSURF=1`
     * refused to accept until it was new enough.
     */
    if (exe_dir != NULL)
        g_ptr_array_add(candidates, g_build_filename(exe_dir, "gsurf", NULL));
    g_ptr_array_add(candidates, g_build_filename(CLAWT_DATA_DIR, "..", "..", "bin",
                                                 "gsurf", NULL));
#ifdef CLAWT_GSURF_BUILD_BIN
    g_ptr_array_add(candidates, g_strdup(CLAWT_GSURF_BUILD_BIN));
#endif
    on_path = g_find_program_in_path("gsurf");
    if (on_path != NULL)
        g_ptr_array_add(candidates, on_path);

    for (i = 0; i < candidates->len; i++) {
        const gchar *path = g_ptr_array_index(candidates, i);
        g_autofree gchar *version = NULL;

        if (!g_file_test(path, G_FILE_TEST_IS_EXECUTABLE)) {
            g_ptr_array_add(tried, g_strdup(path));
            continue;
        }
        if (gsurf_new_enough(path, &version))
            return g_strdup(path);
        g_ptr_array_add(tried, g_strdup_printf("%s (%s, too old)", path,
                                               version != NULL ? version : "no version"));
    }
    if (on_path == NULL)
        g_ptr_array_add(tried, g_strdup("PATH"));

    g_ptr_array_add(tried, NULL);
    joined = g_strjoinv(", ", (gchar **)tried->pdata);
    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                "no gsurf %d.%d.%d or newer was found (looked at %s); build "
                "one with `make GSURF=1`, which builds GSURF_DIR, or install it",
                GSURF_MIN_MAJOR, GSURF_MIN_MINOR, GSURF_MIN_MICRO, joined);
    return NULL;
}

/* /usr/bin first: another package manager's Xvfb ahead of it on PATH is a
 * different server with different defaults, found by accident. */
static gchar *
resolve_xvfb(GError **error)
{
    gchar *found;

    if (g_file_test("/usr/bin/Xvfb", G_FILE_TEST_IS_EXECUTABLE))
        return g_strdup("/usr/bin/Xvfb");
    found = g_find_program_in_path("Xvfb");
    if (found != NULL)
        return found;

    g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND,
                "Xvfb was not found (looked in /usr/bin and PATH); a browser "
                "session needs it -- the Fedora package is xorg-x11-server-Xvfb");
    return NULL;
}

/* Runs in the child between fork and exec: async-signal-safe only. */
static void
die_with_parent(gpointer user_data)
{
    (void)user_data;
#if defined(__linux__)
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
}

/* ── The session ──────────────────────────────────────────────────── */

struct _ClawtBrowser {
    GObject       parent_instance;

    gchar        *id;
    gchar        *dir;
    gchar        *control_name;

    /* What the next start uses.  Guarded by lock. */
    gchar        *home;
    gint          width;
    gint          height;
    gint          idle_minutes;

    GMutex        lock;         /* one caller at a time talks to the browser */
    GMutex        state;        /* the fields below, never held across a call */

    GSubprocess  *xvfb;
    GSubprocess  *gsurf;
    gchar        *socket_path;

    gchar        *uri;
    gchar        *title;
    gint64        last_used;
    gchar        *driver;
    gint64        driver_seen;

    ClawtBrowserTransport transport;
    gpointer       transport_data;
    GDestroyNotify transport_destroy;

    gint64        next_id;
};

G_DEFINE_FINAL_TYPE(ClawtBrowser, clawt_browser, G_TYPE_OBJECT)

static void stop_locked(ClawtBrowser *self);

static void
clawt_browser_finalize(GObject *object)
{
    ClawtBrowser *self = CLAWT_BROWSER(object);

    g_mutex_lock(&self->lock);
    stop_locked(self);
    g_mutex_unlock(&self->lock);

    if (self->transport_destroy != NULL)
        self->transport_destroy(self->transport_data);

    g_free(self->id);
    g_free(self->dir);
    g_free(self->control_name);
    g_free(self->home);
    g_free(self->socket_path);
    g_free(self->uri);
    g_free(self->title);
    g_free(self->driver);
    g_mutex_clear(&self->lock);
    g_mutex_clear(&self->state);

    G_OBJECT_CLASS(clawt_browser_parent_class)->finalize(object);
}

static void
clawt_browser_class_init(ClawtBrowserClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_browser_finalize;
}

static void
clawt_browser_init(ClawtBrowser *self)
{
    g_mutex_init(&self->lock);
    g_mutex_init(&self->state);
    self->home = g_strdup("about:blank");
    self->width = 1280;
    self->height = 800;
    self->idle_minutes = 15;
}

ClawtBrowser *
clawt_browser_new(const gchar *id, const gchar *dir)
{
    ClawtBrowser *self;
    g_autofree gchar *hash = NULL;

    g_return_val_if_fail(id != NULL, NULL);
    g_return_val_if_fail(dir != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_BROWSER, NULL);
    self->id = g_strdup(id);
    self->dir = g_strdup(dir);

    /*
     * The socket's name has to be unique per session *and* per daemon:
     * two fleets on one machine both have an agent called `research`.
     * The directory is what is unique, so it is hashed into the name.
     * gsurf allows letters, digits, '-', '_' and '.', which an id is.
     */
    hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, dir, -1);
    hash[10] = '\0';
    self->control_name = g_strdup_printf("clawtilla-%s-%s", id, hash);

    return self;
}

const gchar *
clawt_browser_get_id(ClawtBrowser *self)
{
    g_return_val_if_fail(CLAWT_IS_BROWSER(self), NULL);
    return self->id;
}

const gchar *
clawt_browser_get_dir(ClawtBrowser *self)
{
    g_return_val_if_fail(CLAWT_IS_BROWSER(self), NULL);
    return self->dir;
}

void
clawt_browser_configure(ClawtBrowser *self, const gchar *home, gint width,
                        gint height, gint idle_minutes)
{
    g_return_if_fail(CLAWT_IS_BROWSER(self));

    g_mutex_lock(&self->state);
    g_free(self->home);
    self->home = g_strdup(home != NULL && *home != '\0' ? home : "about:blank");
    self->width = width > 0 ? width : 1280;
    self->height = height > 0 ? height : 800;
    self->idle_minutes = MAX(idle_minutes, 0);
    g_mutex_unlock(&self->state);
}

void
clawt_browser_set_transport(ClawtBrowser *self, ClawtBrowserTransport transport,
                            gpointer user_data, GDestroyNotify destroy)
{
    g_return_if_fail(CLAWT_IS_BROWSER(self));

    if (self->transport_destroy != NULL)
        self->transport_destroy(self->transport_data);
    self->transport = transport;
    self->transport_data = user_data;
    self->transport_destroy = destroy;
}

/* Alive, as the kernel says: GSubprocess reaps on GLib's worker, so a
 * child that exited has no identifier any more. */
static gboolean
child_alive(GSubprocess *child)
{
    return child != NULL && g_subprocess_get_identifier(child) != NULL;
}

gboolean
clawt_browser_is_running(ClawtBrowser *self)
{
    gboolean running;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), FALSE);

    if (self->transport != NULL)
        return TRUE;
    g_mutex_lock(&self->state);
    running = child_alive(self->gsurf) && child_alive(self->xvfb);
    g_mutex_unlock(&self->state);
    return running;
}

/* Sends SIGTERM, waits a little, then SIGKILL.  A wait that never ends is
 * worse than a browser killed rudely. */
static void
end_child(GSubprocess *child)
{
    gint waited = 0;

    if (!child_alive(child))
        return;

    g_subprocess_send_signal(child, SIGTERM);
    while (child_alive(child) && waited < STOP_WAIT_MS) {
        g_usleep(50 * 1000);
        waited += 50;
    }
    if (child_alive(child))
        g_subprocess_force_exit(child);
}

/* Called with lock held. */
static void
stop_locked(ClawtBrowser *self)
{
    GSubprocess *gsurf;
    GSubprocess *xvfb;

    g_mutex_lock(&self->state);
    gsurf = g_steal_pointer(&self->gsurf);
    xvfb = g_steal_pointer(&self->xvfb);
    g_clear_pointer(&self->socket_path, g_free);
    g_clear_pointer(&self->driver, g_free);
    g_mutex_unlock(&self->state);

    /* The browser first: it is a client of the X server. */
    end_child(gsurf);
    end_child(xvfb);
    g_clear_object(&gsurf);
    g_clear_object(&xvfb);
}

void
clawt_browser_stop(ClawtBrowser *self)
{
    g_return_if_fail(CLAWT_IS_BROWSER(self));

    g_mutex_lock(&self->lock);
    stop_locked(self);
    g_mutex_unlock(&self->lock);
}

/*
 * The X server's display number, from its -displayfd pipe.  Polled with a
 * deadline rather than read: an Xvfb that hangs before writing would
 * otherwise hold this thread for ever.
 */
static gint
read_display(GSubprocess *xvfb, GError **error)
{
    GInputStream *out = g_subprocess_get_stdout_pipe(xvfb);
    gint fd = g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(out));
    gchar buffer[32] = { 0 };
    gsize got = 0;
    gint64 deadline = g_get_monotonic_time() + (gint64)START_TIMEOUT_MS * 1000;

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
                "Xvfb did not report a display within %d seconds%s",
                START_TIMEOUT_MS / 1000,
                child_alive(xvfb) ? "" : " (it exited)");
    return -1;
}

/* The gsurf config: its profile, its window and its control socket. */
static gboolean
write_config(ClawtBrowser *self, const gchar *path, GError **error)
{
    g_autofree gchar *data = g_build_filename(self->dir, "data", NULL);
    g_autofree gchar *cache = g_build_filename(self->dir, "cache", NULL);
    g_autofree gchar *downloads = g_build_filename(self->dir, "downloads", NULL);
    g_autofree gchar *cookies = g_build_filename(data, "cookies.sqlite", NULL);
    g_autofree gchar *home = NULL;
    g_autofree gchar *text = NULL;
    gint width;
    gint height;

    g_mutex_lock(&self->state);
    home = g_strescape(self->home, NULL);
    width = self->width;
    height = self->height;
    g_mutex_unlock(&self->state);

    if (g_mkdir_with_parents(data, 0700) != 0 ||
        g_mkdir_with_parents(cache, 0700) != 0 ||
        g_mkdir_with_parents(downloads, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "could not create the browser profile under %s: %s",
                    self->dir, g_strerror(errno));
        return FALSE;
    }

    /* Strings are escaped and quoted: a home page with a colon or a '#'
     * is otherwise a different YAML document. */
    text = g_strdup_printf(
        "# Written by clawtilla for browser session %s; rewritten at every start.\n"
        "browser:\n"
        "  homepage: \"%s\"\n"
        "window:\n"
        "  title: clawtilla-%s\n"
        "  geometry: %dx%d\n"
        "storage:\n"
        "  cache_dir: \"%s\"\n"
        "  data_dir: \"%s\"\n"
        "  download_dir: \"%s\"\n"
        "  cookie_jar: \"%s\"\n"
        "control:\n"
        "  enabled: true\n"
        "  name: %s\n",
        self->id, home, self->id, width, height, cache, data, downloads,
        cookies, self->control_name);

    return g_file_set_contents_full(path, text, -1,
                                    G_FILE_SET_CONTENTS_CONSISTENT, 0600, error);
}

/* Called with lock held. */
static gboolean
start_locked(ClawtBrowser *self, GError **error)
{
    g_autoptr(GSubprocessLauncher) xl = NULL;
    g_autoptr(GSubprocessLauncher) gl = NULL;
    g_autoptr(GSubprocess) xvfb = NULL;
    g_autoptr(GSubprocess) gsurf = NULL;
    g_autoptr(GHashTable) extra = NULL;
    g_auto(GStrv) environment = NULL;
    g_autofree gchar *xvfb_path = NULL;
    g_autofree gchar *gsurf_path = NULL;
    g_autofree gchar *config = NULL;
    g_autofree gchar *log = NULL;
    g_autofree gchar *xlog = NULL;
    g_autofree gchar *screen = NULL;
    g_autofree gchar *display = NULL;
    g_autofree gchar *socket_path = NULL;
    g_autofree gchar *home = NULL;
    g_autofree gchar *runtime = NULL;
    gint number;
    gint waited;

    if (child_alive(self->gsurf) && child_alive(self->xvfb))
        return TRUE;
    stop_locked(self);

    if (!clawt_browser_build_available()) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                    "this clawtilla was built without GSURF=1, so it cannot "
                    "start a browser");
        return FALSE;
    }

    xvfb_path = resolve_xvfb(error);
    if (xvfb_path == NULL)
        return FALSE;
    gsurf_path = clawt_browser_resolve_gsurf(error);
    if (gsurf_path == NULL)
        return FALSE;

    if (g_mkdir_with_parents(self->dir, 0700) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "could not create %s: %s", self->dir, g_strerror(errno));
        return FALSE;
    }

    config = g_build_filename(self->dir, "gsurf.yaml", NULL);
    if (!write_config(self, config, error))
        return FALSE;

    runtime = g_strdup(g_get_user_runtime_dir());
    socket_path = g_strdup_printf("%s/gsurf/%s.sock", runtime, self->control_name);
    if (!clawt_check_socket_path(socket_path, error))
        return FALSE;

    /* The X server: its own display, its stderr kept for the reason it died. */
    g_mutex_lock(&self->state);
    screen = g_strdup_printf("%dx%dx24", self->width, self->height);
    home = g_strdup(self->home);
    g_mutex_unlock(&self->state);

    xlog = g_build_filename(self->dir, "xvfb.log", NULL);
    xl = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE);
    g_subprocess_launcher_set_stderr_file_path(xl, xlog);
    g_subprocess_launcher_set_child_setup(xl, die_with_parent, NULL, NULL);
    /*
     * TCP off; the abstract socket left on.  Without it X is reachable
     * only through /tmp/.X11-unix, which a daemon with a private /tmp
     * (a systemd unit's PrivateTmp, a sandbox) does not share with the
     * browser it starts -- measured: gsurf then never opens a window and
     * its control socket never appears.
     */
    xvfb = g_subprocess_launcher_spawn(xl, error, xvfb_path, "-displayfd", "1",
                                       "-screen", "0", screen, "-nolisten", "tcp", NULL);
    if (xvfb == NULL)
        return FALSE;

    number = read_display(xvfb, error);
    if (number < 0) {
        end_child(xvfb);
        return FALSE;
    }
    display = g_strdup_printf(":%d", number);

    /* The browser. */
    extra = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(extra, g_strdup("DISPLAY"), g_strdup(display));
    g_hash_table_insert(extra, g_strdup("GDK_BACKEND"), g_strdup("x11"));
    g_hash_table_insert(extra, g_strdup("WAYLAND_DISPLAY"), g_strdup("no-wayland-here"));
    g_hash_table_insert(extra, g_strdup("XDG_RUNTIME_DIR"), g_strdup(runtime));
    g_hash_table_insert(extra, g_strdup("XDG_DATA_HOME"),
                        g_build_filename(self->dir, "data", NULL));
    g_hash_table_insert(extra, g_strdup("XDG_CACHE_HOME"),
                        g_build_filename(self->dir, "cache", NULL));
    g_hash_table_insert(extra, g_strdup("XDG_CONFIG_HOME"),
                        g_build_filename(self->dir, "config", NULL));
    environment = clawt_build_child_environment(extra);

    log = g_build_filename(self->dir, "gsurf.log", NULL);
    gl = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_INHERIT);
    g_subprocess_launcher_set_environ(gl, environment);
    g_subprocess_launcher_set_cwd(gl, self->dir);
    g_subprocess_launcher_set_stdout_file_path(gl, log);
    g_subprocess_launcher_set_stderr_file_path(gl, log);
    g_subprocess_launcher_set_child_setup(gl, die_with_parent, NULL, NULL);
    gsurf = g_subprocess_launcher_spawn(gl, error, gsurf_path, "-c", config,
                                        "--no-c-config", "--no-modules", home, NULL);
    if (gsurf == NULL) {
        end_child(xvfb);
        return FALSE;
    }

    /* Its control socket appears once its window is up. */
    for (waited = 0; waited < START_TIMEOUT_MS; waited += 100) {
        if (g_file_test(socket_path, G_FILE_TEST_EXISTS))
            break;
        if (!child_alive(gsurf))
            break;
        g_usleep(100 * 1000);
    }

    if (!g_file_test(socket_path, G_FILE_TEST_EXISTS)) {
        g_autofree gchar *tail = NULL;
        gsize length = 0;

        if (g_file_get_contents(log, &tail, &length, NULL) && length > 400) {
            gchar *start = tail + length - 400;

            memmove(tail, start, 400);
            tail[400] = '\0';
        }
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_RUNTIME_SPAWN,
                    "gsurf %s before its control socket appeared (%s)%s%s",
                    child_alive(gsurf) ? "did not answer within 20 seconds" : "exited",
                    log, tail != NULL ? ": " : "", tail != NULL ? g_strstrip(tail) : "");
        end_child(gsurf);
        end_child(xvfb);
        return FALSE;
    }

    g_mutex_lock(&self->state);
    self->xvfb = g_steal_pointer(&xvfb);
    self->gsurf = g_steal_pointer(&gsurf);
    g_free(self->socket_path);
    self->socket_path = g_steal_pointer(&socket_path);
    self->last_used = g_get_real_time() / G_USEC_PER_SEC;
    g_mutex_unlock(&self->state);

    g_message("browser %s: started gsurf on %s", self->id, display);
    return TRUE;
}

gboolean
clawt_browser_start(ClawtBrowser *self, GError **error)
{
    gboolean ok;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), FALSE);

    if (self->transport != NULL)
        return TRUE;
    g_mutex_lock(&self->lock);
    ok = start_locked(self, error);
    g_mutex_unlock(&self->lock);
    return ok;
}

/*
 * Reads one line, refusing past REPLY_MAX_BYTES as it accumulates.
 * g_data_input_stream_read_line() grows its buffer until it finds a
 * newline, so a cap applied to what it returned would be a cap on memory
 * already spent.
 */
static gchar *
read_reply_line(GInputStream *in, GError **error)
{
    g_autoptr(GByteArray) line = g_byte_array_new();
    guint8 chunk[8192];

    for (;;) {
        gssize n = g_input_stream_read(in, chunk, sizeof(chunk), NULL, error);
        guint8 *newline;

        if (n < 0)
            return NULL;
        if (n == 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                        "the browser closed the connection without answering");
            return NULL;
        }
        newline = memchr(chunk, '\n', (gsize)n);
        g_byte_array_append(line, chunk, newline != NULL ? (guint)(newline - chunk)
                                                         : (guint)n);
        if (line->len > REPLY_MAX_BYTES) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                        "the browser's answer passed %d MiB; refused",
                        REPLY_MAX_BYTES / (1024 * 1024));
            return NULL;
        }
        if (newline != NULL) {
            g_byte_array_append(line, (const guint8 *)"", 1);
            return (gchar *)g_byte_array_free(g_steal_pointer(&line), FALSE);
        }
    }
}

/* One request on a fresh connection; called with lock held. */
static JsonNode *
socket_call(ClawtBrowser *self, const gchar *method, JsonNode *params,
            guint timeout_seconds, GError **error)
{
    g_autoptr(GSocketClient) client = g_socket_client_new();
    g_autoptr(GSocketAddress) address = NULL;
    g_autoptr(GSocketConnection) connection = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonGenerator) generator = json_generator_new();
    g_autoptr(JsonNode) request = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *text = NULL;
    g_autofree gchar *line = NULL;
    g_autofree gchar *path = NULL;
    JsonObject *reply;
    gint64 id;

    g_mutex_lock(&self->state);
    path = g_strdup(self->socket_path);
    id = ++self->next_id;
    g_mutex_unlock(&self->state);

    if (path == NULL) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                    "the browser session %s is not running", self->id);
        return NULL;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "jsonrpc");
    json_builder_add_string_value(builder, "2.0");
    json_builder_set_member_name(builder, "id");
    json_builder_add_int_value(builder, id);
    json_builder_set_member_name(builder, "method");
    json_builder_add_string_value(builder, method);
    if (params != NULL && JSON_NODE_HOLDS_OBJECT(params)) {
        json_builder_set_member_name(builder, "params");
        json_builder_add_value(builder, json_node_copy(params));
    }
    json_builder_end_object(builder);
    request = json_builder_get_root(builder);
    json_generator_set_root(generator, request);
    text = json_generator_to_data(generator, NULL);

    address = g_unix_socket_address_new(path);
    g_socket_client_set_timeout(client, timeout_seconds);
    connection = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address),
                                         NULL, error);
    if (connection == NULL)
        return NULL;
    g_socket_set_timeout(g_socket_connection_get_socket(connection), timeout_seconds);

    if (!g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)),
                                   text, strlen(text), NULL, NULL, error) ||
        !g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)),
                                   "\n", 1, NULL, NULL, error))
        return NULL;

    /* Events would arrive only after events.subscribe, which is never sent;
     * a line with another id is skipped all the same. */
    for (;;) {
        g_clear_pointer(&line, g_free);
        line = read_reply_line(g_io_stream_get_input_stream(G_IO_STREAM(connection)),
                               error);
        if (line == NULL)
            return NULL;
        if (!json_parser_load_from_data(parser, line, -1, error))
            return NULL;
        if (!JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)))
            continue;
        reply = json_node_get_object(json_parser_get_root(parser));
        if (reply == NULL || !json_object_has_member(reply, "id") ||
            json_object_get_int_member(reply, "id") != id)
            continue;
        break;
    }

    if (json_object_has_member(reply, "error")) {
        JsonObject *problem = json_object_get_object_member(reply, "error");
        const gchar *message = problem != NULL
            ? json_object_get_string_member_with_default(problem, "message", "failed")
            : "failed";

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED, "%s: %s", method, message);
        return NULL;
    }

    if (!json_object_has_member(reply, "result"))
        return json_node_new(JSON_NODE_NULL);
    return json_node_copy(json_object_get_member(reply, "result"));
}

JsonNode *
clawt_browser_call(ClawtBrowser *self, const gchar *method, JsonNode *params,
                   guint timeout_seconds, GError **error)
{
    JsonNode *result;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), NULL);
    g_return_val_if_fail(method != NULL, NULL);

    g_mutex_lock(&self->state);
    self->last_used = g_get_real_time() / G_USEC_PER_SEC;
    g_mutex_unlock(&self->state);

    if (self->transport != NULL)
        return self->transport(self, method, params, error, self->transport_data);

    g_mutex_lock(&self->lock);
    if (!start_locked(self, error)) {
        g_mutex_unlock(&self->lock);
        return NULL;
    }
    result = socket_call(self, method, params, timeout_seconds > 0 ? timeout_seconds : 30,
                         error);
    g_mutex_unlock(&self->lock);
    return result;
}

/* A string result, or NULL. */
static gchar *
call_string(ClawtBrowser *self, const gchar *method, JsonNode *params, guint timeout)
{
    g_autoptr(JsonNode) result = clawt_browser_call(self, method, params, timeout, NULL);

    if (result == NULL || !JSON_NODE_HOLDS_VALUE(result) ||
        json_node_get_value_type(result) != G_TYPE_STRING)
        return NULL;
    return g_strdup(json_node_get_string(result));
}

void
clawt_browser_refresh_page(ClawtBrowser *self)
{
    g_autofree gchar *uri = NULL;
    g_autofree gchar *title = NULL;

    g_return_if_fail(CLAWT_IS_BROWSER(self));

    if (!clawt_browser_is_running(self))
        return;
    uri = call_string(self, "page.uri", NULL, 5);
    title = call_string(self, "page.title", NULL, 5);

    g_mutex_lock(&self->state);
    if (uri != NULL) {
        g_free(self->uri);
        self->uri = g_steal_pointer(&uri);
    }
    if (title != NULL) {
        g_free(self->title);
        self->title = g_steal_pointer(&title);
    }
    g_mutex_unlock(&self->state);
}

gchar *
clawt_browser_dup_uri(ClawtBrowser *self)
{
    gchar *out;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), NULL);
    g_mutex_lock(&self->state);
    out = g_strdup(self->uri);
    g_mutex_unlock(&self->state);
    return out;
}

gchar *
clawt_browser_dup_title(ClawtBrowser *self)
{
    gchar *out;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), NULL);
    g_mutex_lock(&self->state);
    out = g_strdup(self->title);
    g_mutex_unlock(&self->state);
    return out;
}

gint64
clawt_browser_get_last_used(ClawtBrowser *self)
{
    gint64 out;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), 0);
    g_mutex_lock(&self->state);
    out = self->last_used;
    g_mutex_unlock(&self->state);
    return out;
}

gboolean
clawt_browser_is_idle(ClawtBrowser *self, gint64 now)
{
    gboolean idle;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), FALSE);

    if (self->transport != NULL || !clawt_browser_is_running(self))
        return FALSE;
    g_mutex_lock(&self->state);
    idle = self->idle_minutes > 0 &&
           now - self->last_used >= (gint64)self->idle_minutes * 60;
    g_mutex_unlock(&self->state);
    return idle;
}

/* Called with state held. */
static gboolean
driver_current(ClawtBrowser *self, gint64 now)
{
    return self->driver != NULL &&
           now - self->driver_seen < CLAWT_BROWSER_DRIVER_LEASE_SECONDS;
}

gboolean
clawt_browser_claim(ClawtBrowser *self, const gchar *agent_id, gint64 now,
                    GError **error)
{
    gboolean ok = TRUE;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), FALSE);
    g_return_val_if_fail(agent_id != NULL, FALSE);

    g_mutex_lock(&self->state);
    if (driver_current(self, now) && g_strcmp0(self->driver, agent_id) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED,
                    "%s is driving the shared browser '%s' right now. Try again "
                    "in a minute, or ask %s to call clawtilla_browser_release when "
                    "it is done; a claim lapses on its own after %d seconds "
                    "without a call.",
                    self->driver, self->id, self->driver,
                    CLAWT_BROWSER_DRIVER_LEASE_SECONDS);
        ok = FALSE;
    } else {
        g_free(self->driver);
        self->driver = g_strdup(agent_id);
        self->driver_seen = now;
    }
    g_mutex_unlock(&self->state);
    return ok;
}

gboolean
clawt_browser_release(ClawtBrowser *self, const gchar *agent_id)
{
    gboolean held;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), FALSE);

    g_mutex_lock(&self->state);
    held = g_strcmp0(self->driver, agent_id) == 0;
    if (held)
        g_clear_pointer(&self->driver, g_free);
    g_mutex_unlock(&self->state);
    return held;
}

gchar *
clawt_browser_dup_driver(ClawtBrowser *self, gint64 now)
{
    gchar *out = NULL;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), NULL);
    g_mutex_lock(&self->state);
    if (driver_current(self, now))
        out = g_strdup(self->driver);
    g_mutex_unlock(&self->state);
    return out;
}

GBytes *
clawt_browser_screenshot(ClawtBrowser *self, GError **error)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) params = NULL;
    g_autoptr(JsonNode) result = NULL;
    const gchar *encoded;
    guchar *decoded;
    gsize length = 0;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), NULL);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "inline");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);
    params = json_builder_get_root(builder);

    result = clawt_browser_call(self, "page.screenshot", params, 30, error);
    if (result == NULL)
        return NULL;

    encoded = JSON_NODE_HOLDS_OBJECT(result)
        ? json_object_get_string_member_with_default(json_node_get_object(result),
                                                     "png_base64", NULL)
        : NULL;
    if (encoded == NULL || *encoded == '\0') {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_PROTOCOL,
                    "the browser answered page.screenshot without a picture "
                    "(gsurf older than 0.5.10?)");
        return NULL;
    }

    decoded = g_base64_decode(encoded, &length);
    return g_bytes_new_take(decoded, length);
}

gboolean
clawt_browser_wait_loaded(ClawtBrowser *self, guint timeout_seconds)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) params = NULL;
    gint64 deadline = g_get_monotonic_time() + (gint64)timeout_seconds * G_USEC_PER_SEC;

    g_return_val_if_fail(CLAWT_IS_BROWSER(self), FALSE);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "script");
    json_builder_add_string_value(builder, "document.readyState");
    json_builder_end_object(builder);
    params = json_builder_get_root(builder);

    /* A beat first: a click that navigates has not started the load yet. */
    g_usleep(250 * 1000);
    while (g_get_monotonic_time() < deadline) {
        g_autofree gchar *state = call_string(self, "page.eval", params, 5);

        if (g_strcmp0(state, "complete") == 0)
            return TRUE;
        g_usleep(250 * 1000);
    }
    return FALSE;
}

/* ── What the clients draw ─────────────────────────────────────────── */

JsonObject *
clawt_browser_list_find(JsonObject *reply, const gchar *agent_id)
{
    JsonArray *sessions;
    guint i;

    if (reply == NULL || agent_id == NULL || !json_object_has_member(reply, "sessions"))
        return NULL;
    sessions = json_object_get_array_member(reply, "sessions");
    for (i = 0; sessions != NULL && i < json_array_get_length(sessions); i++) {
        JsonObject *session = json_array_get_object_element(sessions, i);
        JsonArray *agents = session != NULL && json_object_has_member(session, "agents")
            ? json_object_get_array_member(session, "agents") : NULL;
        guint j;

        for (j = 0; agents != NULL && j < json_array_get_length(agents); j++) {
            if (g_strcmp0(json_array_get_string_element(agents, j), agent_id) == 0)
                return session;
        }
    }
    return NULL;
}

gchar *
clawt_browser_session_line(JsonObject *session)
{
    const gchar *uri;
    const gchar *title;
    const gchar *driver;
    GString *line;

    if (session == NULL || !json_object_get_boolean_member_with_default(session, "running",
                                                                        FALSE))
        return g_strdup("Not running. It starts on the agent's first browser call, "
                        "or when you open a page here.");

    uri = json_object_get_string_member_with_default(session, "uri", "");
    title = json_object_get_string_member_with_default(session, "title", "");
    driver = json_object_get_string_member_with_default(session, "driver", "");

    line = g_string_new("Running");
    if (*title != '\0')
        g_string_append_printf(line, " \xe2\x80\x94 %s", title);
    if (*uri != '\0')
        g_string_append_printf(line, *title != '\0' ? " (%s)" : " \xe2\x80\x94 %s", uri);
    if (*driver != '\0')
        g_string_append_printf(line, " \xe2\x80\x94 driven by %s", driver);
    return g_string_free(line, FALSE);
}

/* ── The manager ──────────────────────────────────────────────────── */

struct _ClawtBrowserManager {
    GObject                parent_instance;

    gchar                 *root;
    GMutex                 lock;
    GHashTable            *sessions;    /* key -> ClawtBrowser */
    ClawtBrowserTransport  transport;
    gpointer               transport_data;
};

G_DEFINE_FINAL_TYPE(ClawtBrowserManager, clawt_browser_manager, G_TYPE_OBJECT)

static void
clawt_browser_manager_finalize(GObject *object)
{
    ClawtBrowserManager *self = CLAWT_BROWSER_MANAGER(object);

    g_hash_table_unref(self->sessions);
    g_free(self->root);
    g_mutex_clear(&self->lock);

    G_OBJECT_CLASS(clawt_browser_manager_parent_class)->finalize(object);
}

static void
clawt_browser_manager_class_init(ClawtBrowserManagerClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_browser_manager_finalize;
}

static void
clawt_browser_manager_init(ClawtBrowserManager *self)
{
    g_mutex_init(&self->lock);
    self->sessions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           g_object_unref);
}

ClawtBrowserManager *
clawt_browser_manager_new(const gchar *root)
{
    ClawtBrowserManager *self;

    g_return_val_if_fail(root != NULL, NULL);

    self = g_object_new(CLAWT_TYPE_BROWSER_MANAGER, NULL);
    self->root = g_strdup(root);
    return self;
}

gchar *
clawt_browser_manager_session_key(ClawtBrowserMode mode, const gchar *agent_id,
                                  const gchar *use)
{
    switch (mode) {
    case CLAWT_BROWSER_NONE:
        return NULL;
    case CLAWT_BROWSER_OWN:
        return agent_id != NULL ? g_strdup_printf("agent:%s", agent_id) : NULL;
    case CLAWT_BROWSER_SHARED:
        return (use != NULL && *use != '\0') ? g_strdup_printf("shared:%s", use) : NULL;
    }
    return NULL;
}

ClawtBrowser *
clawt_browser_manager_obtain(ClawtBrowserManager *self, const gchar *key)
{
    ClawtBrowser *browser;

    g_return_val_if_fail(CLAWT_IS_BROWSER_MANAGER(self), NULL);
    g_return_val_if_fail(key != NULL, NULL);

    g_mutex_lock(&self->lock);
    browser = g_hash_table_lookup(self->sessions, key);
    if (browser == NULL) {
        const gchar *colon = strchr(key, ':');
        const gchar *id = colon != NULL ? colon + 1 : key;
        g_autofree gchar *leaf = NULL;
        g_autofree gchar *dir = NULL;

        /*
         * agent.<id> and shared.<id>: an agent and a shared session may have
         * the same id, and must not end up sharing a profile -- a dot no id
         * can hold keeps the two apart, as computers do.
         */
        leaf = g_strdup_printf("%s.%s", g_str_has_prefix(key, "shared:") ? "shared"
                                                                         : "agent", id);
        dir = g_build_filename(self->root, leaf, NULL);
        browser = clawt_browser_new(id, dir);
        if (self->transport != NULL)
            clawt_browser_set_transport(browser, self->transport,
                                        self->transport_data, NULL);
        g_hash_table_insert(self->sessions, g_strdup(key), browser);
    }
    g_object_ref(browser);
    g_mutex_unlock(&self->lock);

    return browser;
}

ClawtBrowser *
clawt_browser_manager_lookup(ClawtBrowserManager *self, const gchar *key)
{
    ClawtBrowser *browser;

    g_return_val_if_fail(CLAWT_IS_BROWSER_MANAGER(self), NULL);

    if (key == NULL)
        return NULL;
    g_mutex_lock(&self->lock);
    browser = g_hash_table_lookup(self->sessions, key);
    if (browser != NULL)
        g_object_ref(browser);
    g_mutex_unlock(&self->lock);
    return browser;
}

static gint
compare_by_id(gconstpointer a, gconstpointer b)
{
    ClawtBrowser *left = *(ClawtBrowser *const *)a;
    ClawtBrowser *right = *(ClawtBrowser *const *)b;

    return g_strcmp0(left->dir, right->dir);
}

GPtrArray *
clawt_browser_manager_list(ClawtBrowserManager *self)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_object_unref);
    GHashTableIter iter;
    gpointer value;

    g_return_val_if_fail(CLAWT_IS_BROWSER_MANAGER(self), out);

    g_mutex_lock(&self->lock);
    g_hash_table_iter_init(&iter, self->sessions);
    while (g_hash_table_iter_next(&iter, NULL, &value))
        g_ptr_array_add(out, g_object_ref(value));
    g_mutex_unlock(&self->lock);

    g_ptr_array_sort(out, compare_by_id);
    return out;
}

void
clawt_browser_manager_set_transport(ClawtBrowserManager *self,
                                    ClawtBrowserTransport transport,
                                    gpointer user_data)
{
    g_return_if_fail(CLAWT_IS_BROWSER_MANAGER(self));

    self->transport = transport;
    self->transport_data = user_data;
}

void
clawt_browser_manager_stop_all(ClawtBrowserManager *self)
{
    g_autoptr(GPtrArray) all = NULL;
    guint i;

    g_return_if_fail(CLAWT_IS_BROWSER_MANAGER(self));

    all = clawt_browser_manager_list(self);
    for (i = 0; i < all->len; i++)
        clawt_browser_stop(g_ptr_array_index(all, i));
}
