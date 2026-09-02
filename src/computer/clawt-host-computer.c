/*
 * clawt-host-computer.c - The real machine clawtilla runs on
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "computer/clawt-host-computer.h"
#include "computer/clawt-observable.h"
#include "computer/clawt-screen.h"
#include "mcp/clawt-mcp-socket.h"

#include <glib/gstdio.h>
#include <string.h>
#include <sys/resource.h>

/*
 * How much output one command may return.
 *
 * Unbounded output is a real failure mode rather than a theoretical one: an
 * agent that runs `find /` produces a reply too large to send and too large
 * to reason about, and the turn is wasted either way.
 */

struct _ClawtHostComputer {
    ClawtComputer parent_instance;

    ClawtSandbox *sandbox;
    GHashTable   *environment;
    gint          nice_level;

    /*
     * The desktop this agent was granted, or NULL.
     *
     * Held here rather than looked up, because #ClawtObservable is asked
     * of the *computer* and the desktop is configured beside it -- and a
     * host computer that had to reach back for the agent's config to
     * find out whether it has a screen would be a second answer to a
     * question clawt_computer_factory_create_desktop() already gives.
     */
    ClawtDesktop *desktop;
};

static void host_observable_init(ClawtObservableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(
    ClawtHostComputer, clawt_host_computer, CLAWT_TYPE_COMPUTER,
    G_IMPLEMENT_INTERFACE(CLAWT_TYPE_OBSERVABLE, host_observable_init))

ClawtComputer *
clawt_host_computer_new(const gchar *agent_id, ClawtSandbox *sandbox)
{
    ClawtHostComputer *self;

    g_return_val_if_fail(CLAWT_IS_SANDBOX(sandbox), NULL);

    self = g_object_new(CLAWT_TYPE_HOST_COMPUTER, NULL);
    self->sandbox = g_object_ref(sandbox);

    clawt_computer_bind_agent(CLAWT_COMPUTER(self), agent_id);

    return CLAWT_COMPUTER(self);
}

ClawtSandbox *
clawt_host_computer_get_sandbox(ClawtHostComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_HOST_COMPUTER(self), NULL);

    return self->sandbox;
}

void
clawt_host_computer_set_environment(ClawtHostComputer *self, GHashTable *env)
{
    g_return_if_fail(CLAWT_IS_HOST_COMPUTER(self));

    g_clear_pointer(&self->environment, g_hash_table_unref);

    if (env != NULL)
        self->environment = g_hash_table_ref(env);
}

void
clawt_host_computer_set_nice(ClawtHostComputer *self, gint nice_level)
{
    g_return_if_fail(CLAWT_IS_HOST_COMPUTER(self));

    self->nice_level = nice_level;
}

void
clawt_host_computer_set_desktop(ClawtHostComputer *self,
                                ClawtDesktop      *desktop)
{
    g_return_if_fail(CLAWT_IS_HOST_COMPUTER(self));

    g_clear_object(&self->desktop);

    if (desktop != NULL)
        self->desktop = g_object_ref(desktop);
}

/* ── Watching the machine clawtilla is running on ───────────── */

/*
 * Which of the two host backends is in force.
 *
 * Resolved rather than read: `auto` is the default and it probes, so a
 * host computer asked for a frame before anything else has resolved the
 * desktop would otherwise see AUTO and match neither branch -- which is
 * how clawt_desktop_describe() once told a guest agent it was driving
 * somebody's real screen.
 */
static ClawtDesktopBackend
host_backend(ClawtHostComputer *self)
{
    if (self->desktop == NULL)
        return CLAWT_DESKTOP_BACKEND_AUTO;

    return clawt_desktop_resolve_backend(self->desktop, NULL);
}

static gboolean
host_observe_start(ClawtObservable *observable, guint fps, GError **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(observable);

    (void)fps;

    if (self->desktop == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this agent has no desktop; set "
                            "computer.desktop.enabled to watch the screen "
                            "of the machine clawtilla is running on");
        return FALSE;
    }

    /*
     * Asked before the first grab rather than discovered at it.  A
     * compositor that is not there answers with a connection refused
     * from deep inside a socket call, and somebody watching a blank
     * panel would be reading that in a log rather than on the panel.
     */
    return clawt_desktop_is_available(self->desktop, error);
}

/*
 * The gowl half: an MCP tool call over the compositor's own socket.
 *
 * gowl answers a screenshot as base64 in the reply, so unlike the guest
 * there is no file and no hash from the compositor -- the hash is
 * computed here from the bytes.  That is a strictly worse deal (the
 * whole image crosses the socket even when nothing has changed) and it
 * is the deal gowl offers; the comparison still saves writing the file
 * and waking every client.
 */
static GBytes *
host_frame_gowl(ClawtHostComputer *self, GError **error)
{
    g_autoptr(JsonNode) result = NULL;
    GBytes *bytes;

    result = clawt_mcp_socket_call(
        clawt_desktop_get_socket_path(self->desktop),
        "screenshot_monitor", NULL, 15, error);

    if (result == NULL)
        return NULL;

    bytes = clawt_mcp_socket_result_image(result);

    if (bytes == NULL) {
        g_autofree gchar *text = clawt_mcp_socket_result_text(result);

        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the compositor answered with no picture: %s",
                    (text != NULL && *text != '\0') ? text
                                                    : "nothing at all");
        return NULL;
    }

    return bytes;
}

/*
 * The GNOME half: the same ScreenshotFrame the guest uses, on this
 * machine's own session bus.
 *
 * No ssh and no session wrapper -- the daemon is already inside the
 * session whose screen this is, so DBUS_SESSION_BUS_ADDRESS is in its
 * own environment.  Wrapping it anyway would work and would mean two
 * spellings of the same call, one of which nothing exercises.
 */
static GBytes *
host_frame_gnome(ClawtHostComputer *self,
                 const gchar       *if_changed_from,
                 gint64            *stamp_out,
                 gchar            **hash_out,
                 GError           **error)
{
    g_auto(GStrv) argv = NULL;
    g_autoptr(GSubprocess) process = NULL;
    g_autofree gchar *out = NULL;
    g_autofree gchar *err = NULL;
    g_autofree gchar *contents = NULL;
    ClawtScreenFrameInfo info = { 0 };
    gsize length = 0;

    (void)self;

    argv = clawt_screen_gnome_frame_argv(CLAWT_SCREEN_FRAME_WIDTH, FALSE);
    process = g_subprocess_newv((const gchar * const *)argv,
                                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                G_SUBPROCESS_FLAGS_STDERR_PIPE, error);

    if (process == NULL)
        return NULL;

    if (!g_subprocess_communicate_utf8(process, NULL, NULL, &out, &err,
                                       error))
        return NULL;

    if (g_subprocess_get_exit_status(process) != 0) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                    "the desktop refused: %s",
                    (err != NULL && *g_strstrip(err) != '\0')
                    ? err : "no reason given");
        return NULL;
    }

    if (!clawt_screen_parse_gdbus_frame(out, &info, error))
        return NULL;

    if (stamp_out != NULL)
        *stamp_out = info.stamp;

    if (if_changed_from != NULL && info.hash != NULL &&
        g_strcmp0(if_changed_from, info.hash) == 0) {
        if (hash_out != NULL)
            *hash_out = g_strdup(info.hash);

        clawt_screen_frame_info_clear(&info);

        return NULL;
    }

    if (info.path == NULL ||
        !g_file_get_contents(info.path, &contents, &length, error)) {
        clawt_screen_frame_info_clear(&info);
        return NULL;
    }

    if (hash_out != NULL)
        *hash_out = g_strdup(info.hash);

    clawt_screen_frame_info_clear(&info);

    return g_bytes_new_take(g_steal_pointer(&contents), length);
}

static GBytes *
host_observe_frame(ClawtObservable  *observable,
                   const gchar      *if_changed_from,
                   gint64           *stamp_out,
                   gchar           **hash_out,
                   GError          **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(observable);
    g_autoptr(GBytes) bytes = NULL;
    g_autofree gchar *hash = NULL;

    if (stamp_out != NULL)
        *stamp_out = 0;

    if (hash_out != NULL)
        *hash_out = NULL;

    if (self->desktop == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this agent has no desktop to photograph");
        return NULL;
    }

    if (host_backend(self) == CLAWT_DESKTOP_BACKEND_GNOME)
        return host_frame_gnome(self, if_changed_from, stamp_out, hash_out,
                                error);

    bytes = host_frame_gowl(self, error);

    if (bytes == NULL)
        return NULL;

    hash = clawt_screen_hash_bytes(bytes);

    if (stamp_out != NULL)
        *stamp_out = g_get_real_time();

    if (hash_out != NULL)
        *hash_out = g_strdup(hash);

    if (if_changed_from != NULL && hash != NULL &&
        g_strcmp0(if_changed_from, hash) == 0)
        return NULL;

    return g_steal_pointer(&bytes);
}

static gboolean
host_observe_can_input(ClawtObservable *observable)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(observable);

    return self->desktop != NULL;
}

static gboolean
host_observe_send_input(ClawtObservable  *observable,
                        ClawtInputEvent  *event,
                        GError          **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(observable);

    if (self->desktop == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED,
                            "this agent has no desktop to send anything to");
        return FALSE;
    }

    if (host_backend(self) == CLAWT_DESKTOP_BACKEND_GNOME) {
        g_auto(GStrv) argv = clawt_screen_gnome_input_argv(event);
        g_autoptr(GSubprocess) process = NULL;
        g_autofree gchar *err = NULL;

        if (argv == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "there is nothing to send: a key or a "
                                "string was asked for and none was given");
            return FALSE;
        }

        process = g_subprocess_newv((const gchar * const *)argv,
                                    G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                    G_SUBPROCESS_FLAGS_STDERR_PIPE, error);

        if (process == NULL)
            return FALSE;

        if (!g_subprocess_communicate_utf8(process, NULL, NULL, NULL, &err,
                                           error))
            return FALSE;

        if (g_subprocess_get_exit_status(process) != 0) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_FAILED,
                        "the desktop refused: %s",
                        (err != NULL && *g_strstrip(err) != '\0')
                        ? err : "no reason given");
            return FALSE;
        }

        return TRUE;
    }

    {
        const gchar *socket_path =
            clawt_desktop_get_socket_path(self->desktop);
        g_autoptr(JsonNode) arguments = NULL;
        g_autoptr(JsonNode) result = NULL;
        const gchar *tool;

        /*
         * A click is two calls here and one on GNOME.  gowl's
         * send_mouse presses wherever the pointer already is, so a click
         * that skipped the move would land at the last place anything
         * touched -- which during a takeover is wherever the *agent*
         * left it.
         */
        if (event->kind == CLAWT_INPUT_CLICK) {
            g_autoptr(ClawtInputEvent) move =
                clawt_input_event_new(CLAWT_INPUT_MOVE);
            g_autoptr(JsonNode) move_arguments = NULL;
            g_autoptr(JsonNode) move_result = NULL;
            const gchar *move_tool;

            move->x = event->x;
            move->y = event->y;
            move_tool = clawt_screen_gowl_input_tool(move, &move_arguments);
            move_result = clawt_mcp_socket_call(socket_path, move_tool,
                                                move_arguments, 10, error);

            if (move_result == NULL)
                return FALSE;
        }

        tool = clawt_screen_gowl_input_tool(event, &arguments);

        if (tool == NULL) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "there is nothing to send: a key or a "
                                "string was asked for and none was given");
            return FALSE;
        }

        result = clawt_mcp_socket_call(socket_path, tool, arguments, 10,
                                       error);

        return result != NULL;
    }
}

/*
 * The size of the screen this agent can see, asked once and remembered.
 *
 * Not remembered for ever: a monitor change is real, and a cached size
 * that outlived one would put every click in the wrong place with
 * nothing to say why. It is re-asked whenever a watch starts, which is
 * the only moment a client has to notice.
 *
 * Both backends already offer this among the *observing* tools, so
 * asking costs no new grant.
 */
static gboolean
host_query_geometry(ClawtHostComputer *self, guint *width, guint *height)
{
    g_autofree gchar *json = NULL;
    g_autoptr(JsonParser) parser = NULL;
    JsonArray *monitors;
    JsonObject *first;

    if (self->desktop == NULL)
        return FALSE;

    if (host_backend(self) == CLAWT_DESKTOP_BACKEND_GNOME) {
        g_auto(GStrv) argv = clawt_screen_gnome_monitors_argv();
        g_autoptr(GSubprocess) process = NULL;
        g_autofree gchar *out = NULL;

        process = g_subprocess_newv((const gchar * const *)argv,
                                    G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                    G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);

        if (process == NULL)
            return FALSE;

        if (!g_subprocess_communicate_utf8(process, NULL, NULL, &out, NULL,
                                           NULL) ||
            g_subprocess_get_exit_status(process) != 0)
            return FALSE;

        json = clawt_screen_parse_gdbus_string(out);
    } else {
        g_autoptr(JsonNode) result = NULL;

        result = clawt_mcp_socket_call(
            clawt_desktop_get_socket_path(self->desktop),
            "list_monitors", NULL, 10, NULL);

        if (result == NULL)
            return FALSE;

        json = clawt_mcp_socket_result_text(result);
    }

    if (json == NULL)
        return FALSE;

    parser = json_parser_new();

    if (!json_parser_load_from_data(parser, json, -1, NULL))
        return FALSE;

    if (json_node_get_node_type(json_parser_get_root(parser)) !=
        JSON_NODE_ARRAY)
        return FALSE;

    monitors = json_node_get_array(json_parser_get_root(parser));

    if (json_array_get_length(monitors) == 0)
        return FALSE;

    first = json_array_get_object_element(monitors, 0);

    if (first == NULL)
        return FALSE;

    if (width != NULL)
        *width = (guint)json_object_get_int_member_with_default(first,
                                                                "width", 0);

    if (height != NULL)
        *height = (guint)json_object_get_int_member_with_default(first,
                                                                 "height", 0);

    return TRUE;
}

static gboolean
host_observe_geometry(ClawtObservable *observable, guint *width,
                      guint *height)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(observable);
    guint w = 0;
    guint h = 0;

    if (!host_query_geometry(self, &w, &h) || w == 0 || h == 0)
        return FALSE;

    if (width != NULL)
        *width = w;

    if (height != NULL)
        *height = h;

    return TRUE;
}

static void
host_observable_init(ClawtObservableInterface *iface)
{
    iface->observe_start = host_observe_start;
    iface->observe_frame = host_observe_frame;
    iface->observe_can_input = host_observe_can_input;
    iface->observe_send_input = host_observe_send_input;
    iface->observe_geometry = host_observe_geometry;

    /*
     * observe_viewer_uri is left refusing.  There is no VNC server in
     * front of somebody's own session, and inventing one -- starting
     * wayvnc, say -- would be clawtilla putting a remote-desktop
     * listener on the operator's machine because they opened a tab.
     */
}

/*
 * The host is already there, so provisioning is only a check that the
 * confinement asked for can actually be applied.  Doing it here rather than
 * at the first command means a missing bwrap surfaces when the agent
 * starts, not three turns into a conversation.
 */
static gboolean
host_provision(ClawtComputer *computer, GError **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    GPtrArray *mounts;
    guint i;

    /*
     * A declared mount is a statement that the agent may reach that
     * directory.  On a container it becomes a real mount and the kernel
     * enforces it; on the host there is nothing to enforce it, so the
     * sources are added to what the sandbox allows.  Without this an
     * agent is handed an exchange directory it is then refused access
     * to, which reads as a bug in the confinement rather than as the
     * mount never having been applied.
     */
    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        clawt_sandbox_add_mount_path(
            self->sandbox, clawt_mount_get_source(mount),
            clawt_mount_get_mode(mount) == CLAWT_MOUNT_MODE_RW);
    }

    if (!clawt_sandbox_is_available(self->sandbox, error)) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                 ? (*error)->message : NULL);
        return FALSE;
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING, NULL);

    return TRUE;
}

static gboolean
host_start(ClawtComputer *computer, GError **error)
{
    return host_provision(computer, error);
}

static gboolean
host_stop(ClawtComputer *computer, GError **error)
{
    (void)error;

    /*
     * Nothing to stop.  The host outlives the agent, which is exactly why
     * this backend needs confinement rather than isolation.
     */
    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);

    return TRUE;
}

typedef struct {
    GSubprocess  *process;
    GMainLoop    *loop;
    GCancellable *give_up;
    gboolean      timed_out;
    gchar        *stdout_text;
    gchar        *stderr_text;
    GError       *error;
} ExecWait;

static gboolean
on_exec_timeout(gpointer user_data)
{
    ExecWait *wait = user_data;

    wait->timed_out = TRUE;
    g_subprocess_force_exit(wait->process);

    /*
     * And stop waiting to be read, not just waiting to be exited.
     *
     * g_subprocess_communicate_utf8_async() finishes when the child has
     * gone *and* stdout and stderr have reached EOF, and a grandchild
     * inherits those pipes -- so killing the child releases nothing when
     * the command was `x &`, or a build, or anything with `nohup`.  The
     * flag above was set on time and the loop then ran until whatever
     * held the pipes finished, which handed the give-up time back to
     * the command: exactly what the timeout exists to take away from
     * it.
     */
    g_cancellable_cancel(wait->give_up);

    return G_SOURCE_REMOVE;
}

/*
 * Passes a caller's cancellation on to the one the wait is using.
 *
 * The communicate call can no longer be given the caller's cancellable
 * directly -- it needs one this function can trip at the deadline -- so
 * the caller's is chained onto it rather than dropped.
 */
static void
on_caller_cancelled(GCancellable *caller, gpointer user_data)
{
    (void)caller;

    g_cancellable_cancel(G_CANCELLABLE(user_data));
}

static void
on_exec_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ExecWait *wait = user_data;

    g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result,
                                         &wait->stdout_text,
                                         &wait->stderr_text,
                                         &wait->error);
    g_main_loop_quit(wait->loop);
}

/*
 * Translates an in-computer path to where it really is on this machine.
 *
 * A host computer has no mount namespace, so a mount is not a mount: it is
 * a promise that a path inside the agent's world means a directory
 * outside it.  Nothing enforces that promise for us, so the translation
 * happens here -- otherwise an agent told its exchange is at
 * /mnt/clawtilla/exchange would be refused for using the path it was
 * given, which is a maddening thing to debug.
 *
 * The result still goes through the confinement check: this maps a path,
 * it does not bless one.
 */
static gchar *
translate_mount_path(ClawtComputer *computer, const gchar *path)
{
    GPtrArray *mounts;
    guint i;

    if (path == NULL)
        return NULL;

    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        const gchar *target = clawt_mount_get_target(mount);
        gsize length;

        if (target == NULL || !g_str_has_prefix(path, target))
            continue;

        length = strlen(target);

        /*
         * A prefix match is not enough: "/mnt/clawtillax" starts with
         * "/mnt/clawtilla" and is somewhere else.
         */
        if (path[length] != '\0' && path[length] != G_DIR_SEPARATOR)
            continue;

        return g_build_filename(clawt_mount_get_source(mount),
                                path + length, NULL);
    }

    return clawt_expand_path(path);
}

/*
 * Whether a path falls inside a mount declared read-only.
 */
static gboolean
mount_is_read_only(ClawtComputer *computer, const gchar *path)
{
    GPtrArray *mounts = clawt_computer_get_mounts(computer);
    g_autofree gchar *resolved = clawt_canonicalize_missing(path);
    gboolean read_only = FALSE;
    guint i;

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);
        g_autofree gchar *source =
            clawt_canonicalize_missing(clawt_mount_get_source(mount));

        if (source == NULL || !clawt_path_is_within(resolved, source))
            continue;

        /*
         * The most specific grant wins, so a read-write mount nested
         * inside a read-only one -- which is how the exchange gives an
         * agent its own directory -- still permits writing.
         */
        read_only = (clawt_mount_get_mode(mount) == CLAWT_MOUNT_MODE_RO);
    }

    return read_only;
}

/*
 * Rewrites every argument that names a mount target.
 *
 * Whole arguments only: rewriting inside a longer string would mean
 * guessing at quoting and would change text the agent meant literally.
 */
static GStrv
translate_argv(ClawtComputer *computer, const gchar * const *argv)
{
    g_autoptr(GPtrArray) out = g_ptr_array_new();
    gsize i;

    for (i = 0; argv != NULL && argv[i] != NULL; i++) {
        if (g_path_is_absolute(argv[i]))
            g_ptr_array_add(out, translate_mount_path(computer, argv[i]));
        else
            g_ptr_array_add(out, g_strdup(argv[i]));
    }

    g_ptr_array_add(out, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&out), FALSE);
}

/*
 * Runs in the child between fork and exec.
 *
 * Only async-signal-safe calls belong here; setpriority is one.
 */
static void
apply_nice(gpointer user_data)
{
    gint level = GPOINTER_TO_INT(user_data);

    if (setpriority(PRIO_PROCESS, 0, level) != 0) {
        /* Nothing safe to report from here; the parent cannot be told. */
    }
}

static ClawtExecResult *
host_exec(ClawtComputer        *computer,
          const gchar * const  *argv,
          const gchar          *working_dir,
          guint                 timeout_seconds,
          GCancellable         *cancellable,
          GError              **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    g_autoptr(GSubprocessLauncher) launcher = NULL;
    g_auto(GStrv) wrapped = NULL;
    g_auto(GStrv) translated = NULL;
    g_autoptr(GMainContext) context = NULL;
    g_autoptr(GMainLoop) loop = NULL;
    g_autofree gchar *bounded_stdout = NULL;
    g_autofree gchar *bounded_stderr = NULL;
    ClawtExecResult *result;
    ExecWait wait;
    GSource *timeout_source = NULL;
    gulong cancelled_id = 0;
    gboolean truncated = FALSE;
    gboolean stderr_truncated = FALSE;

    /*
     * Mount targets are rewritten to where they really are before
     * anything is checked or run.  On a container the kernel does this;
     * on the host nothing does, so an agent using the path it was given
     * would be refused for naming its own exchange directory.
     */
    translated = translate_argv(computer, argv);

    /*
     * Checked before anything is spawned.  A command that would reach
     * outside the agent's boundary must never start, not be killed
     * afterwards.
     */
    if (!clawt_sandbox_check_argv(self->sandbox,
                                  (const gchar * const *)translated, error))
        return NULL;

    wrapped = clawt_sandbox_wrap_argv(self->sandbox,
                                      (const gchar * const *)translated);

    /*
     * The command gets the same allowlisted environment a supervised
     * agent process does.  It used to inherit the daemon's, which meant
     * every secret resolved for any agent -- and the operator's
     * SSH_AUTH_SOCK -- was readable by any host command an agent chose to
     * run.  CLAUDE.md forbids exactly this; the rule was applied to the
     * agent process and not to the path agents actually use.
     */
    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                         G_SUBPROCESS_FLAGS_STDERR_PIPE);

    {
        g_auto(GStrv) environment =
            clawt_build_child_environment(self->environment);

        g_subprocess_launcher_set_environ(launcher, environment);
    }

    /*
     * The requested niceness is applied in the child.  It was stored and
     * never used, so an operator asking for nice: 5 got nothing.
     */
    if (self->nice_level != 0)
        g_subprocess_launcher_set_child_setup(launcher, apply_nice,
                                              GINT_TO_POINTER(self->nice_level),
                                              NULL);

    if (working_dir != NULL) {
        g_autofree gchar *expanded = clawt_expand_path(working_dir);

        if (!clawt_sandbox_path_is_allowed(self->sandbox, expanded)) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                        "'%s' is outside what this agent may reach",
                        expanded);
            return NULL;
        }

        g_subprocess_launcher_set_cwd(launcher, expanded);
    }

    memset(&wait, 0, sizeof(wait));

    wait.process = g_subprocess_launcher_spawnv(
        launcher, (const gchar * const *)wrapped, error);

    if (wait.process == NULL) {
        g_prefix_error(error, "running %s: ", argv[0]);
        return NULL;
    }

    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    loop = g_main_loop_new(context, FALSE);
    wait.loop = loop;

    /*
     * A timeout is not optional in practice.  An agent that runs an
     * interactive command by mistake waits for input that never arrives,
     * and without this the turn simply never ends.
     *
     * The source is attached to the context we pushed, not added with
     * g_timeout_add_seconds().  That helper attaches to the DEFAULT main
     * context, which this loop is not running -- so the timeout would never
     * fire and every hanging command would hang for ever.
     */
    if (timeout_seconds > 0) {
        timeout_source = g_timeout_source_new_seconds(timeout_seconds);
        g_source_set_callback(timeout_source, on_exec_timeout, &wait, NULL);
        g_source_attach(timeout_source, context);
    }

    wait.give_up = g_cancellable_new();

    if (cancellable != NULL)
        cancelled_id = g_cancellable_connect(cancellable,
                                             G_CALLBACK(on_caller_cancelled),
                                             wait.give_up, NULL);

    g_subprocess_communicate_utf8_async(wait.process, NULL, wait.give_up,
                                        on_exec_done, &wait);
    g_main_loop_run(loop);

    if (timeout_source != NULL) {
        g_source_destroy(timeout_source);
        g_source_unref(timeout_source);
    }

    if (cancelled_id != 0)
        g_cancellable_disconnect(cancellable, cancelled_id);

    g_clear_object(&wait.give_up);

    g_main_context_pop_thread_default(context);

    /*
     * The deadline is reported before the error, because tripping the
     * cancellable to enforce it *is* how the wait ended: reading the
     * resulting G_IO_ERROR_CANCELLED first would tell the agent its
     * command was cancelled by somebody, rather than that it ran out of
     * time.
     */
    if (wait.timed_out) {
        g_clear_error(&wait.error);
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_TIMEOUT,
                    "'%s' did not finish within %u seconds and was stopped",
                    argv[0], timeout_seconds);
        g_clear_object(&wait.process);
        g_free(wait.stdout_text);
        g_free(wait.stderr_text);
        return NULL;
    }

    if (wait.error != NULL) {
        g_propagate_error(error, wait.error);
        g_clear_object(&wait.process);
        g_free(wait.stdout_text);
        g_free(wait.stderr_text);
        return NULL;
    }

    bounded_stdout = clawt_computer_truncate_output(wait.stdout_text,
                                                    CLAWT_COMPUTER_MAX_OUTPUT_BYTES,
                                                    &truncated);
    bounded_stderr = clawt_computer_truncate_output(wait.stderr_text,
                                                    CLAWT_COMPUTER_MAX_OUTPUT_BYTES,
                                                    &stderr_truncated);

    result = clawt_exec_result_new(g_subprocess_get_exit_status(wait.process),
                                   bounded_stdout, bounded_stderr);
    clawt_exec_result_set_truncated(result, truncated || stderr_truncated);

    g_clear_object(&wait.process);
    g_free(wait.stdout_text);
    g_free(wait.stderr_text);

    return result;
}

/*
 * Copying on the host is a copy, not a transfer -- but it still goes
 * through the confinement check, or an agent could write anywhere simply by
 * calling put_file instead of running cp.
 */
static gboolean
host_put_file(ClawtComputer  *computer,
              const gchar    *local_path,
              const gchar    *remote_path,
              GError        **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    g_autofree gchar *contents = NULL;
    g_autofree gchar *destination = translate_mount_path(computer,
                                                         remote_path);
    gsize length = 0;

    if (!clawt_sandbox_path_is_allowed(self->sandbox, destination)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is outside what this agent may reach", destination);
        return FALSE;
    }

    /*
     * A read-only mount is honoured here.
     *
     * On a container or a VM the kernel enforces `mode: ro`; on a host
     * there is no mount to enforce, so the same configuration was
     * silently writable -- the host being the more permissive backend for
     * an identical config, which is exactly the inconsistency mounts are
     * supposed to avoid.  Argument inspection still cannot stop a program
     * that opens the file itself; this closes the path clawtilla owns.
     */
    if (mount_is_read_only(computer, destination)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is inside a read-only mount", remote_path);
        return FALSE;
    }

    if (!g_file_get_contents(local_path, &contents, &length, error))
        return FALSE;

    return g_file_set_contents(destination, contents, (gssize)length, error);
}

static gboolean
host_get_file(ClawtComputer  *computer,
              const gchar    *remote_path,
              const gchar    *local_path,
              GError        **error)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    g_autofree gchar *contents = NULL;
    g_autofree gchar *source = translate_mount_path(computer, remote_path);
    gsize length = 0;

    if (!clawt_sandbox_path_is_allowed(self->sandbox, source)) {
        g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFINEMENT,
                    "'%s' is outside what this agent may reach", source);
        return FALSE;
    }

    if (!g_file_get_contents(source, &contents, &length, error))
        return FALSE;

    return g_file_set_contents(local_path, contents, (gssize)length, error);
}

static gchar *
host_describe(ClawtComputer *computer)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(computer);
    g_autofree gchar *confinement = clawt_sandbox_describe(self->sandbox);
    g_autoptr(GString) out = g_string_new(NULL);
    GPtrArray *mounts;
    guint i;

    g_string_append_printf(
        out,
        "You can run commands on the machine clawtilla itself is running "
        "on. %s", confinement);

    /*
     * The mounts are listed by name, because an agent told only what it
     * may not reach spends turns discovering what it may.
     */
    mounts = clawt_computer_get_mounts(computer);

    for (i = 0; mounts != NULL && i < mounts->len; i++) {
        ClawtMount *mount = g_ptr_array_index(mounts, i);

        if (i == 0)
            g_string_append(out, "\n\nYou can also reach:");

        g_string_append_printf(out, "\n  %s (%s on this machine)",
                               clawt_mount_get_target(mount),
                               clawt_mount_get_source(mount));
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * The host is not clawtilla's to destroy.
 *
 * Removing an agent that had the run of this machine takes away its
 * access and nothing else: the filesystem it was working in belongs to
 * the person, not to the agent. Spelled out rather than left to the
 * default, which is now a refusal.
 */
static gboolean
host_teardown(ClawtComputer *computer, GError **error)
{
    return TRUE;
}

static ClawtComputerType
host_get_computer_type(ClawtComputer *computer)
{
    (void)computer;
    return CLAWT_COMPUTER_HOST;
}

static void
clawt_host_computer_dispose(GObject *object)
{
    ClawtHostComputer *self = CLAWT_HOST_COMPUTER(object);

    g_clear_pointer(&self->environment, g_hash_table_unref);
    g_clear_object(&self->sandbox);
    g_clear_object(&self->desktop);

    G_OBJECT_CLASS(clawt_host_computer_parent_class)->dispose(object);
}

static void
clawt_host_computer_class_init(ClawtHostComputerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtComputerClass *computer_class = CLAWT_COMPUTER_CLASS(klass);

    object_class->dispose = clawt_host_computer_dispose;

    computer_class->provision = host_provision;
    computer_class->start = host_start;
    computer_class->stop = host_stop;
    computer_class->teardown = host_teardown;
    computer_class->exec = host_exec;
    computer_class->put_file = host_put_file;
    computer_class->get_file = host_get_file;
    computer_class->describe = host_describe;
    computer_class->get_computer_type = host_get_computer_type;
}

static void
clawt_host_computer_init(ClawtHostComputer *self)
{
    self->nice_level = 0;
}
