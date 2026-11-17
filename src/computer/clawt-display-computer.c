/*
 * clawt-display-computer.c - GUI apps on a screen of their own
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A host computer and a ClawtDisplayServer, put together:
 *
 * - commands are the host computer's, unchanged -- its confinement, its
 *   argument checks, its allowlisted environment -- plus the screen's
 *   variables, so a GUI app any command starts appears on the display;
 * - under bwrap the sandbox binds what a client needs to reach the
 *   screen (the X socket directory, or gowl's runtime directory), read
 *   only, which is enough to connect to a socket;
 * - the screen is #ClawtObservable: pictures, keys and clicks go to the
 *   server, so the observer, the takeover and every client's Screen tab
 *   work as they do for a VM's desktop.
 */

#include "clawtilla.h"

struct _ClawtDisplayComputer {
    ClawtComputer        parent_instance;

    ClawtHostComputer   *host;
    ClawtDisplayServer  *server;
    GHashTable          *environment;    /* the agent's own env: */
    GStrv                startup;
    gboolean             bound;          /* sandbox binds added */
};

static void display_observable_init(ClawtObservableInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE(ClawtDisplayComputer, clawt_display_computer,
                              CLAWT_TYPE_COMPUTER,
                              G_IMPLEMENT_INTERFACE(CLAWT_TYPE_OBSERVABLE,
                                                    display_observable_init))

/* ── The paragraph the agent reads ───────────────────────────────── */

gchar *
clawt_display_computer_describe_display(ClawtDisplayBackend backend,
                                        guint width, guint height,
                                        const gchar *viewer)
{
    GString *out = g_string_new(NULL);
    const gchar *what;

    switch (backend) {
    case CLAWT_DISPLAY_BACKEND_GOWL:
        what = "gowl, a Wayland compositor, running headless; Wayland "
               "apps draw on it directly and X apps through XWayland";
        break;
    case CLAWT_DISPLAY_BACKEND_XVNC:
        what = "an X server (Xvnc) with a VNC view a person can open";
        break;
    case CLAWT_DISPLAY_BACKEND_XVFB:
        what = "an X server (Xvfb) drawing into memory";
        break;
    case CLAWT_DISPLAY_BACKEND_AUTO:
    default:
        what = "a virtual screen (gowl, Xvnc or Xvfb, whichever runs here)";
        break;
    }

    g_string_append_printf(out,
        "Your computer has a virtual display of its own: %s, %ux%u. "
        "Nobody else is using it, so it is the place to open and drive GUI "
        "apps. Apps you start -- with clawtilla_display_launch, or any command you "
        "run through clawtilla_computer_exec -- appear there. Look at it "
        "with clawtilla_display_screenshot and act on it with clawtilla_display_click, "
        "clawtilla_display_type, clawtilla_display_key and clawtilla_display_scroll; coordinates are the "
        "screen's own pixels, from the top left. clawtilla_display_windows lists what "
        "is open. Your commands still run on the real machine clawtilla is "
        "on, under the same confinement as any host command. A person can "
        "watch this screen and take it; while they hold it your clicks and "
        "keys are refused, and you should wait for them to give it back.",
        what, width, height);

    if (backend == CLAWT_DISPLAY_BACKEND_GOWL)
        g_string_append(out, " gowl's own tools (window lists, focusing "
                             "and placing windows, reading text on screen) "
                             "are there too, under clawtilla-desktop.");
    if (viewer != NULL)
        g_string_append(out, " A person watching may also have it open in "
                             "a VNC viewer.");

    return g_string_free(out, FALSE);
}

/* ── Lifecycle ───────────────────────────────────────────────────── */

/*
 * What every command gets: the agent's env:, then the screen's own
 * variables on top, so `env: {DISPLAY: ...}` cannot point commands at
 * somebody's real screen.
 */
static void
apply_environment(ClawtDisplayComputer *self)
{
    g_autoptr(GHashTable) merged = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(GHashTable) screen =
        clawt_display_server_dup_environment(self->server);
    GHashTableIter iter;
    gpointer key;
    gpointer value;

    if (self->environment != NULL) {
        g_hash_table_iter_init(&iter, self->environment);
        while (g_hash_table_iter_next(&iter, &key, &value))
            g_hash_table_insert(merged, g_strdup(key), g_strdup(value));
    }
    g_hash_table_iter_init(&iter, screen);
    while (g_hash_table_iter_next(&iter, &key, &value))
        g_hash_table_insert(merged, g_strdup(key), g_strdup(value));

    clawt_host_computer_set_environment(self->host, merged);
}

/*
 * Under bwrap the box has a private /tmp and no XDG runtime of the
 * host's: the screen's socket must be bound in or no app can draw.
 * Writable, as the agent's own box binds the daemon's sockets: connect()
 * on a unix socket counts as a write to some confinement layers.
 * Once per computer: the X socket directory and gowl's runtime directory
 * do not move between starts.
 */
static void
bind_screen(ClawtDisplayComputer *self)
{
    ClawtSandbox *sandbox = clawt_host_computer_get_sandbox(self->host);
    g_auto(GStrv) paths = NULL;
    guint i;

    if (self->bound || sandbox == NULL ||
        clawt_sandbox_get_mode(sandbox) != CLAWT_CONFINE_BWRAP)
        return;

    paths = clawt_display_server_dup_bind_paths(self->server);
    for (i = 0; paths != NULL && paths[i] != NULL; i++) {
        /* the X socket's directory, not the socket: :N changes per start */
        g_autofree gchar *path = g_str_has_prefix(paths[i], "/tmp/.X11-unix/")
            ? g_strdup("/tmp/.X11-unix") : g_strdup(paths[i]);

        clawt_sandbox_add_mount_path(sandbox, path, TRUE);
    }
    self->bound = TRUE;
}

static gboolean
display_provision(ClawtComputer *computer, GError **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(computer);

    /* The host's own check: that the confinement asked for can apply. */
    return clawt_computer_provision(CLAWT_COMPUTER(self->host), error);
}

static gboolean
display_start(ClawtComputer *computer, GError **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(computer);
    guint i;

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STARTING, NULL);

    if (!clawt_computer_start(CLAWT_COMPUTER(self->host), error) ||
        !clawt_display_server_start(self->server, error)) {
        clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_ERROR,
                                 (error != NULL && *error != NULL)
                                     ? (*error)->message : NULL);
        return FALSE;
    }

    apply_environment(self);
    bind_screen(self);

    /*
     * The apps asked for at start, each once.  One that will not start
     * is logged and the rest still are: a screen with three of four apps
     * open is more use than no screen.
     */
    for (i = 0; self->startup != NULL && self->startup[i] != NULL; i++) {
        g_autoptr(GError) failed = NULL;

        if (*self->startup[i] == '\0')
            continue;
        if (clawt_display_server_launch(
                self->server, self->startup[i], NULL,
                clawt_host_computer_get_sandbox(self->host), &failed) <= 0)
            g_warning("display: startup app '%s' did not start: %s",
                      self->startup[i],
                      failed != NULL ? failed->message : "no reason given");
    }

    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_RUNNING, NULL);
    return TRUE;
}

static gboolean
display_stop(ClawtComputer *computer, GError **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(computer);

    clawt_display_server_stop(self->server);
    clawt_computer_stop(CLAWT_COMPUTER(self->host), error);
    clawt_computer_set_state(computer, CLAWT_COMPUTER_STATE_STOPPED, NULL);
    return TRUE;
}

static gboolean
display_teardown(ClawtComputer *computer, GError **error)
{
    /* Nothing outlives a stop: the display's directory is emptied there. */
    return display_stop(computer, error);
}

/* Commands are the host's, with the screen's variables already set. */
static ClawtExecResult *
display_exec(ClawtComputer *computer, const gchar * const *argv,
             const gchar *working_dir, guint timeout_seconds,
             GCancellable *cancellable, GError **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(computer);

    return clawt_computer_exec(CLAWT_COMPUTER(self->host), argv, working_dir,
                               timeout_seconds, cancellable, error);
}

static gboolean
display_put_file(ClawtComputer *computer, const gchar *local_path,
                 const gchar *remote_path, GError **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(computer);

    return clawt_computer_put_file(CLAWT_COMPUTER(self->host), local_path,
                                   remote_path, error);
}

static gboolean
display_get_file(ClawtComputer *computer, const gchar *remote_path,
                 const gchar *local_path, GError **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(computer);

    return clawt_computer_get_file(CLAWT_COMPUTER(self->host), remote_path,
                                   local_path, error);
}

static gchar *
display_describe(ClawtComputer *computer)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(computer);
    g_autofree gchar *host = clawt_computer_describe(
        CLAWT_COMPUTER(self->host));
    g_autofree gchar *viewer =
        clawt_display_server_dup_viewer_uri(self->server);
    g_autofree gchar *screen = NULL;
    guint width = 0;
    guint height = 0;

    clawt_display_server_get_size(self->server, &width, &height);
    screen = clawt_display_computer_describe_display(
        clawt_display_server_get_backend(self->server), width, height,
        viewer);

    return (host != NULL && *host != '\0')
        ? g_strdup_printf("%s\n\n%s", screen, host)
        : g_steal_pointer(&screen);
}

static ClawtComputerType
display_get_computer_type(ClawtComputer *computer)
{
    (void)computer;
    return CLAWT_COMPUTER_DISPLAY;
}

/* ── The screen ──────────────────────────────────────────────────── */

static gboolean
display_observe_start(ClawtObservable *observable, guint fps, GError **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(observable);

    (void)fps;

    /*
     * Watching a display that is not up starts it, the way a first
     * command would: the person opened the tab to see the screen.
     */
    if (clawt_display_server_is_running(self->server))
        return TRUE;
    if (!clawt_computer_ensure_started(CLAWT_COMPUTER(self), error))
        return FALSE;
    if (clawt_display_server_is_running(self->server))
        return TRUE;

    /* Started with the agent and then stopped: a person pressed Stop. */
    g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                        "the virtual display is not running; start the "
                        "computer");
    return FALSE;
}

static GBytes *
display_observe_frame(ClawtObservable *observable,
                      const gchar     *if_changed_from,
                      gint64          *stamp_out,
                      gchar          **hash_out,
                      GError         **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(observable);
    g_autoptr(GBytes) bytes = NULL;
    g_autofree gchar *hash = NULL;

    if (stamp_out != NULL)
        *stamp_out = 0;
    if (hash_out != NULL)
        *hash_out = NULL;

    bytes = clawt_display_server_capture(self->server, error);
    if (bytes == NULL) {
        g_autofree gchar *failure =
            clawt_display_server_dup_failure(self->server);

        /* A server that died says why, not only that it is gone. */
        if (failure != NULL && error != NULL && *error != NULL)
            g_prefix_error(error, "the display server exited (%s): ",
                           failure);
        return NULL;
    }

    hash = clawt_screen_hash_bytes(bytes);
    if (stamp_out != NULL)
        *stamp_out = g_get_real_time();
    if (hash_out != NULL)
        *hash_out = g_strdup(hash);

    /* Unchanged is NULL with no error, as the interface says. */
    if (if_changed_from != NULL && hash != NULL &&
        g_strcmp0(if_changed_from, hash) == 0)
        return NULL;

    return g_steal_pointer(&bytes);
}

static gboolean
display_observe_can_input(ClawtObservable *observable)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(observable);

    return clawt_display_server_is_running(self->server);
}

static gboolean
display_observe_send_input(ClawtObservable *observable,
                           ClawtInputEvent *event, GError **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(observable);

    return clawt_display_server_send_input(self->server, event, error);
}

static gchar *
display_observe_viewer_uri(ClawtObservable *observable)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(observable);

    return clawt_display_server_dup_viewer_uri(self->server);
}

static gboolean
display_observe_geometry(ClawtObservable *observable, guint *width,
                         guint *height)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(observable);

    clawt_display_server_get_size(self->server, width, height);
    return TRUE;
}

static gchar *
display_observe_focused_title(ClawtObservable *observable, GError **error)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(observable);

    (void)error;
    return clawt_display_server_dup_focused_title(self->server);
}

static void
display_observable_init(ClawtObservableInterface *iface)
{
    iface->observe_start = display_observe_start;
    iface->observe_frame = display_observe_frame;
    iface->observe_can_input = display_observe_can_input;
    iface->observe_send_input = display_observe_send_input;
    iface->observe_viewer_uri = display_observe_viewer_uri;
    iface->observe_geometry = display_observe_geometry;
    iface->observe_focused_title = display_observe_focused_title;
}

/* ── GObject ─────────────────────────────────────────────────────── */

static void
clawt_display_computer_dispose(GObject *object)
{
    ClawtDisplayComputer *self = CLAWT_DISPLAY_COMPUTER(object);

    if (self->server != NULL)
        clawt_display_server_stop(self->server);
    g_clear_object(&self->server);
    g_clear_object(&self->host);
    g_clear_pointer(&self->environment, g_hash_table_unref);
    g_clear_pointer(&self->startup, g_strfreev);

    G_OBJECT_CLASS(clawt_display_computer_parent_class)->dispose(object);
}

static void
clawt_display_computer_class_init(ClawtDisplayComputerClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    ClawtComputerClass *computer_class = CLAWT_COMPUTER_CLASS(klass);

    object_class->dispose = clawt_display_computer_dispose;

    computer_class->provision = display_provision;
    computer_class->start = display_start;
    computer_class->stop = display_stop;
    computer_class->teardown = display_teardown;
    computer_class->exec = display_exec;
    computer_class->put_file = display_put_file;
    computer_class->get_file = display_get_file;
    computer_class->describe = display_describe;
    computer_class->get_computer_type = display_get_computer_type;
}

static void
clawt_display_computer_init(ClawtDisplayComputer *self)
{
    (void)self;
}

ClawtComputer *
clawt_display_computer_new(const gchar *agent_id, ClawtHostComputer *host,
                           ClawtDisplayServer *server)
{
    ClawtDisplayComputer *self;

    g_return_val_if_fail(agent_id != NULL, NULL);
    g_return_val_if_fail(CLAWT_IS_HOST_COMPUTER(host), NULL);
    g_return_val_if_fail(CLAWT_IS_DISPLAY_SERVER(server), NULL);

    self = g_object_new(CLAWT_TYPE_DISPLAY_COMPUTER, NULL);
    self->host = g_object_ref(host);
    self->server = g_object_ref(server);
    clawt_computer_bind_agent(CLAWT_COMPUTER(self), agent_id);
    return CLAWT_COMPUTER(self);
}

void
clawt_display_computer_set_environment(ClawtDisplayComputer *self,
                                       GHashTable           *env)
{
    g_return_if_fail(CLAWT_IS_DISPLAY_COMPUTER(self));

    g_clear_pointer(&self->environment, g_hash_table_unref);
    if (env != NULL)
        self->environment = g_hash_table_ref(env);

    /* Before a start the host gets just the agent's, as host would. */
    clawt_host_computer_set_environment(self->host, env);
}

void
clawt_display_computer_set_startup(ClawtDisplayComputer *self,
                                   const gchar * const  *commands)
{
    g_return_if_fail(CLAWT_IS_DISPLAY_COMPUTER(self));

    g_clear_pointer(&self->startup, g_strfreev);
    if (commands != NULL)
        self->startup = g_strdupv((gchar **)commands);
}

ClawtDisplayServer *
clawt_display_computer_get_server(ClawtDisplayComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_DISPLAY_COMPUTER(self), NULL);

    return self->server;
}

ClawtHostComputer *
clawt_display_computer_get_host(ClawtDisplayComputer *self)
{
    g_return_val_if_fail(CLAWT_IS_DISPLAY_COMPUTER(self), NULL);

    return self->host;
}

gint
clawt_display_computer_launch(ClawtDisplayComputer *self,
                              const gchar          *command,
                              GError              **error)
{
    g_return_val_if_fail(CLAWT_IS_DISPLAY_COMPUTER(self), 0);
    g_return_val_if_fail(command != NULL, 0);

    if (!clawt_display_server_is_running(self->server) &&
        !clawt_computer_ensure_started(CLAWT_COMPUTER(self), error))
        return 0;
    if (!clawt_display_server_is_running(self->server)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "the virtual display is not running; start the "
                            "computer");
        return 0;
    }

    return clawt_display_server_launch(
        self->server, command, self->environment,
        clawt_host_computer_get_sandbox(self->host), error);
}

/* ── One action, for the agent's tools and the IPC verb ──────────── */

gboolean
clawt_display_computer_action_is_acting(const gchar *action)
{
    static const gchar *const acting[] = {
        "click", "move", "type", "key", "scroll", "launch", NULL
    };

    return action != NULL && g_strv_contains(acting, action);
}

static gint64
member_int(JsonObject *arguments, const gchar *name, gint64 fallback)
{
    JsonNode *node;

    if (arguments == NULL || !json_object_has_member(arguments, name))
        return fallback;
    node = json_object_get_member(arguments, name);
    if (!JSON_NODE_HOLDS_VALUE(node))
        return fallback;
    if (json_node_get_value_type(node) == G_TYPE_STRING)
        return g_ascii_strtoll(json_node_get_string(node), NULL, 10);
    if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
        return (gint64)json_node_get_double(node);
    return json_node_get_int(node);
}

static const gchar *
member_string(JsonObject *arguments, const gchar *name)
{
    return (arguments != NULL)
        ? json_object_get_string_member_with_default(arguments, name, NULL)
        : NULL;
}

JsonNode *
clawt_display_computer_perform(ClawtDisplayComputer *self,
                               const gchar          *action,
                               JsonObject           *arguments,
                               GBytes              **png,
                               GError              **error)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(ClawtInputEvent) event = NULL;

    g_return_val_if_fail(CLAWT_IS_DISPLAY_COMPUTER(self), NULL);

    if (png != NULL)
        *png = NULL;

    if (action == NULL) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                            "which action?");
        return NULL;
    }

    /*
     * The display comes up on first use, like any computer's first
     * command: an agent asking for a screenshot before anything else is
     * asking for the screen to exist.
     */
    if (!clawt_display_server_is_running(self->server) &&
        !clawt_computer_ensure_started(CLAWT_COMPUTER(self), error))
        return NULL;
    if (!clawt_display_server_is_running(self->server)) {
        g_set_error_literal(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED,
                            "the virtual display is not running; it was "
                            "stopped, and starts again with the computer");
        return NULL;
    }

    json_builder_begin_object(builder);

    if (g_strcmp0(action, "screenshot") == 0) {
        g_autoptr(GBytes) picture =
            clawt_display_server_capture(self->server, error);
        guint width = 0;
        guint height = 0;

        if (picture == NULL)
            return NULL;
        clawt_screen_png_size(picture, &width, &height);
        json_builder_set_member_name(builder, "width");
        json_builder_add_int_value(builder, width);
        json_builder_set_member_name(builder, "height");
        json_builder_add_int_value(builder, height);
        if (png != NULL)
            *png = g_steal_pointer(&picture);
    } else if (g_strcmp0(action, "launch") == 0) {
        const gchar *command = member_string(arguments, "command");
        gint pid;

        if (command == NULL || *command == '\0') {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "command is required: the app to start, as "
                                "a shell command line");
            return NULL;
        }
        pid = clawt_display_computer_launch(self, command, error);
        if (pid <= 0)
            return NULL;
        json_builder_set_member_name(builder, "pid");
        json_builder_add_int_value(builder, pid);
    } else if (g_strcmp0(action, "windows") == 0) {
        JsonNode *windows = clawt_display_server_list_windows(self->server,
                                                              error);

        if (windows == NULL)
            return NULL;
        json_builder_set_member_name(builder, "windows");
        json_builder_add_value(builder, windows);
    } else {
        if (g_strcmp0(action, "click") == 0)
            event = clawt_input_event_new(CLAWT_INPUT_CLICK);
        else if (g_strcmp0(action, "move") == 0)
            event = clawt_input_event_new(CLAWT_INPUT_MOVE);
        else if (g_strcmp0(action, "scroll") == 0)
            event = clawt_input_event_new(CLAWT_INPUT_SCROLL);
        else if (g_strcmp0(action, "type") == 0)
            event = clawt_input_event_new(CLAWT_INPUT_TEXT);
        else if (g_strcmp0(action, "key") == 0)
            event = clawt_input_event_new(CLAWT_INPUT_KEY);

        if (event == NULL) {
            g_set_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT,
                        "'%s' is not an action: screenshot, click, move, "
                        "type, key, scroll, launch or windows", action);
            return NULL;
        }

        event->x = (gint)member_int(arguments, "x", 0);
        event->y = (gint)member_int(arguments, "y", 0);
        event->button = (guint)member_int(arguments, "button", 1);
        event->dx = (gdouble)member_int(arguments, "dx", 0);
        event->dy = (gdouble)member_int(arguments, "dy", 0);
        clawt_input_event_set_text(event, member_string(arguments, "text"));

        if ((event->kind == CLAWT_INPUT_TEXT ||
             event->kind == CLAWT_INPUT_KEY) &&
            (event->text == NULL || *event->text == '\0')) {
            g_set_error_literal(error, CLAWT_ERROR,
                                CLAWT_ERROR_INVALID_ARGUMENT,
                                "text is required: the string to type, or "
                                "the key to press");
            return NULL;
        }

        if (!clawt_display_server_send_input(self->server, event, error))
            return NULL;
        json_builder_set_member_name(builder, "sent");
        json_builder_add_boolean_value(builder, TRUE);
    }

    json_builder_end_object(builder);
    return json_builder_get_root(builder);
}
