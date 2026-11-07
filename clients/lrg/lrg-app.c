/*
 * lrg-app.c - The libregnum client's window, frame loop and plumbing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * libregnum has no application object: a program opens a raylib window
 * and runs its own loop.  This file is that loop, plus the three things
 * every page needs from it -- requests, toasts and events.
 *
 * The loop does four things a frame:
 *
 *  1. drains the GLib main context, which is where the daemon's socket
 *     is read and every async reply and event is dispatched;
 *  2. polls input and hands it to the canvas;
 *  3. lays out and draws;
 *  4. decides how long to wait before the next frame.
 *
 * The fourth is what keeps an idle client from costing a core.  A frame
 * with no input, no GLib work, nothing animating and no animated
 * background is followed by a bounded wait *inside the GLib context*
 * (g_main_context_iteration() blocking, with a timeout source as the
 * ceiling) rather than a sleep, so a daemon event arriving mid-wait wakes
 * the window at once.  Input is the one thing the wait cannot see --
 * X11 events are not a GLib source here -- so the ceiling is short
 * enough (100 ms) that the first keypress after a pause is not felt.
 */

#include "clawt-lrg.h"

#include <graylib.h>
#include <raylib.h>
#include <stdlib.h>
#include <string.h>

/* How long an idle frame may wait for GLib work, by background. */
#define IDLE_WAIT_MS          (100)
#define ANIMATED_IDLE_WAIT_MS (33)

/* How many GLib dispatches one frame may do before it draws. */
#define DISPATCH_LIMIT (64)

/* How long a wake keeps the loop at full rate. */
#define WAKE_SECONDS (0.75)

/* ── Requests ─────────────────────────────────────────────────────── */

typedef struct {
    ClawtLrgApp          *app;
    gchar                *kind;
    ClawtLrgRequestFlags  flags;
    guint                 generation;
    ClawtLrgReplyFunc     func;
    gpointer              user_data;
    GDestroyNotify        destroy;
} Pending;

static void
pending_free(Pending *pending)
{
    if (pending->destroy != NULL)
        pending->destroy(pending->user_data);

    g_free(pending->kind);
    g_free(pending);
}

static void
on_request_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    Pending *pending = user_data;
    ClawtLrgApp *app = pending->app;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_client_request_finish(CLAWT_CLIENT(source), result, &error);

    /*
     * A reply to a selection that has moved on is dropped whole -- not
     * toasted either, because "agent not found" about an agent somebody
     * already clicked away from describes nothing they are looking at.
     */
    if (!clawt_lrg_reply_is_current((pending->flags & CLAWT_LRG_GUARDED) != 0,
                                    pending->generation, app->generation)) {
        pending_free(pending);
        return;
    }

    if (!(pending->flags & CLAWT_LRG_QUIET)) {
        g_autofree gchar *text = clawt_ipc_reply_toast_text(error, reply);

        if (text != NULL)
            clawt_lrg_toast(app, text);
    }

    if (pending->func != NULL)
        pending->func(app, reply, error, pending->user_data);

    clawt_lrg_app_wake(app);
    pending_free(pending);
}

void
clawt_lrg_request(ClawtLrgApp *app, const gchar *kind, JsonNode *payload,
                  ClawtLrgRequestFlags flags, ClawtLrgReplyFunc func,
                  gpointer user_data, GDestroyNotify destroy)
{
    Pending *pending;

    g_return_if_fail(app != NULL);
    g_return_if_fail(kind != NULL);

    pending = g_new0(Pending, 1);
    pending->app = app;
    pending->kind = g_strdup(kind);
    pending->flags = flags;
    pending->generation = app->generation;
    pending->func = func;
    pending->user_data = user_data;
    pending->destroy = destroy;

    /*
     * The thread-default is pushed around the call so the GTask inside
     * clawt_client_request_async() attaches to our context: an async API
     * takes the *ambient* context (AGENTS.md, "Main contexts"), and this
     * process has no other loop running it.
     */
    g_main_context_push_thread_default(app->context);
    clawt_client_request_async(app->client, kind, payload, NULL,
                               on_request_finished, pending);
    g_main_context_pop_thread_default(app->context);
}

/* ── Toasts ───────────────────────────────────────────────────────── */

void
clawt_lrg_toast(ClawtLrgApp *app, const gchar *text)
{
    gint64 now = g_get_monotonic_time();
    LrgToast *toast;

    g_return_if_fail(app != NULL);

    if (text == NULL || *text == '\0')
        return;

    /*
     * The same sentence within a second is one toast: a refresh that
     * fails on every tick must not stack a bar per tick over the page
     * (AGENTS.md, "A polled request that fails is a toast per refresh").
     */
    if (!clawt_toast_should_show(app->last_toast, app->last_toast_at, text,
                                 now))
        return;

    g_free(app->last_toast);
    app->last_toast = g_strdup(text);
    app->last_toast_at = now;

    toast = lrg_toast_new(NULL, text);
    lrg_toast_queue_show(app->toasts, toast);
    g_object_unref(toast);
    clawt_lrg_app_wake(app);
}

typedef struct {
    GCallback callback;
    gpointer  user_data;
} ToastAction;

static void
on_toast_button(LrgToast *toast, guint index, const gchar *action,
                gpointer user_data)
{
    ToastAction *a = user_data;

    ((void (*)(gpointer))a->callback)(a->user_data);
}

void
clawt_lrg_toast_action(ClawtLrgApp *app, const gchar *text,
                       const gchar *button, GCallback on_action,
                       gpointer user_data)
{
    LrgToast *toast;
    ToastAction *a;

    g_return_if_fail(app != NULL);

    toast = lrg_toast_new(NULL, text);
    a = g_new0(ToastAction, 1);
    a->callback = on_action;
    a->user_data = user_data;

    lrg_toast_add_button(toast, button, NULL);
    g_signal_connect_data(toast, "button-clicked", G_CALLBACK(on_toast_button),
                          a, (GClosureNotify)(void (*)(void))g_free, 0);
    lrg_toast_queue_show(app->toasts, toast);
    g_object_unref(toast);
    clawt_lrg_app_wake(app);
}

guint
clawt_lrg_app_attach(ClawtLrgApp *app, GSource *source)
{
    guint i;

    g_return_val_if_fail(app != NULL, 0);
    g_return_val_if_fail(source != NULL, 0);

    /* drop the ones that have finished, so the list does not grow */
    for (i = app->sources->len; i > 0; i--) {
        if (g_source_is_destroyed(g_ptr_array_index(app->sources, i - 1)))
            g_ptr_array_remove_index_fast(app->sources, i - 1);
    }

    g_ptr_array_add(app->sources, g_source_ref(source));
    return g_source_attach(source, app->context);
}

/* ── Waking the loop ──────────────────────────────────────────────── */

static gdouble wake_until = 0.0;

void
clawt_lrg_app_wake(ClawtLrgApp *app)
{
    if (app != NULL)
        app->dirty = TRUE;

    if (IsWindowReady())
        wake_until = GetTime() + WAKE_SECONDS;
}

/* ── Appearance ───────────────────────────────────────────────────── */

void
clawt_lrg_app_apply_appearance(ClawtLrgApp *app, ClawtAppearance *appearance)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;
    LrgTheme *theme;
    gdouble monitor_scale = 1.0;

    g_return_if_fail(app != NULL);

    if (appearance != NULL && appearance != app->appearance) {
        g_clear_pointer(&app->appearance, clawt_appearance_free);
        app->appearance = clawt_appearance_copy(appearance);
    } else if (app->appearance == NULL) {
        app->appearance = clawt_appearance_load(NULL, &error);

        if (app->appearance == NULL) {
            g_warning("%s: appearance.yaml: %s", CLAWT_LRG_BIN_NAME,
                      error->message);
            app->appearance = clawt_appearance_new();
        }

        g_clear_error(&error);
    }

    if (IsWindowReady()) {
        Vector2 dpi = GetWindowScaleDPI();

        monitor_scale = (dpi.x > 0.0f) ? (gdouble)dpi.x : 1.0;
    }

    app->scale = clawt_lrg_effective_scale(app->appearance, monitor_scale);
    yaml = clawt_lrg_theme_yaml(app->appearance, app->system_dark, app->scale);
    theme = lrg_theme_new_from_data(yaml, -1, &error);

    if (theme == NULL) {
        /*
         * A theme document this client wrote that libregnum refuses is a
         * bug here, not in somebody's file -- say so and keep drawing in
         * a preset, rather than a window with no theme at all.
         */
        g_warning("%s: the generated theme was refused (%s); using a preset",
                  CLAWT_LRG_BIN_NAME, error->message);
        theme = lrg_theme_new_preset(
            clawt_lrg_appearance_is_dark(app->appearance, app->system_dark)
                ? "midnight" : "daylight");
    }

    /*
     * Palettes on disk are written for GTK's eyes and checked by nobody
     * for contrast; nudge whatever fails WCAG AA here rather than drawing
     * unreadable text in somebody's colours.
     */
    lrg_theme_fix_contrast(theme, 4.5f);

    lrg_motion_set_reduced(clawt_appearance_get_lrg_motion(app->appearance) ==
                           CLAWT_LRG_MOTION_NONE);

    lrg_theme_set_default(theme);
    g_clear_object(&app->theme);
    app->theme = theme;

    if (app->canvas != NULL)
        lrg_widget_queue_layout(LRG_WIDGET(app->canvas));

    clawt_lrg_app_wake(app);
}

/* ── Events and connection ────────────────────────────────────────── */

static void
on_client_event(ClawtClient *client, ClawtEvent *event, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    guint n = clawt_page_count();
    guint i;

    clawt_lrg_window_event(app, event);

    /*
     * Indexed by value: the page enum is dense from 0 and
     * clawt_page_count() bounds it (tests/test-sections.c pins both).
     */
    for (i = 0; i < n; i++) {
        const ClawtLrgPage *page = clawt_lrg_page_for((ClawtPage)i);

        if (page != NULL && page->event != NULL)
            page->event(app, event);
    }

    clawt_lrg_app_wake(app);
}

static void
on_client_connected(ClawtClient *client, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    clawt_lrg_window_connection_changed(app);
    clawt_lrg_app_wake(app);
}

static void
on_client_disconnected(ClawtClient *client, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    clawt_lrg_window_connection_changed(app);
    clawt_lrg_app_wake(app);
}

static void
on_client_resync(ClawtClient *client, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    /*
     * The cursor fell off the daemon's replay ring: whatever the pages
     * hold has a hole in it, so they re-read rather than carry it.
     */
    clawt_lrg_refresh_fleet(app);
    clawt_lrg_show_page(app, app->page);
}

static void
connect_signals(ClawtLrgApp *app)
{
    g_signal_connect(app->client, "event", G_CALLBACK(on_client_event), app);
    g_signal_connect(app->client, "connected",
                     G_CALLBACK(on_client_connected), app);
    g_signal_connect(app->client, "disconnected",
                     G_CALLBACK(on_client_disconnected), app);
    g_signal_connect(app->client, "resync", G_CALLBACK(on_client_resync), app);
}

static void
on_connected(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autoptr(GError) error = NULL;

    if (!clawt_client_connect_finish(CLAWT_CLIENT(source), result, &error)) {
        /*
         * Not a reason to close.  The window says so in its banner,
         * offers the connection list, and keeps trying -- the same
         * decision the GTK client made, for the same reason: the local
         * daemon is the connection least likely to matter to a client
         * whose point is reaching daemons elsewhere.
         */
        g_printerr("%s: %s\n", CLAWT_LRG_BIN_NAME, error->message);
        clawt_client_start_reconnecting(app->client);
    }

    clawt_lrg_window_connection_changed(app);
}

static void
start_connecting(ClawtLrgApp *app)
{
    clawt_client_set_auto_reconnect(app->client, TRUE);

    g_main_context_push_thread_default(app->context);

    /*
     * A subscription is an intent the client remembers: asked for now, it
     * is sent the moment a connection exists, so a window that came up
     * before its daemon still hears everything once the daemon appears.
     */
    clawt_client_subscribe_async(app->client, 0, NULL, NULL, NULL);
    clawt_client_connect_async(app->client, NULL, on_connected, app);

    g_main_context_pop_thread_default(app->context);
}

void
clawt_lrg_app_switch_connection(ClawtLrgApp *app, ClawtConnection *connection)
{
    g_autoptr(ClawtClient) client = NULL;

    g_return_if_fail(app != NULL);
    g_return_if_fail(connection != NULL);

    g_signal_handlers_disconnect_by_data(app->client, app);
    clawt_client_disconnect(app->client);
    g_clear_object(&app->client);

    g_clear_pointer(&app->connection, clawt_connection_free);
    app->connection = clawt_connection_copy(connection);

    client = clawt_connection_create_client(connection);
    app->client = g_object_ref(client);

    /* everything the window holds describes the old daemon */
    g_clear_pointer(&app->agents, json_node_unref);
    g_clear_pointer(&app->rooms, json_node_unref);
    g_hash_table_remove_all(app->unread);
    g_hash_table_remove_all(app->typing);
    g_clear_pointer(&app->agent_id, g_free);
    g_clear_pointer(&app->room_id, g_free);
    app->generation++;

    connect_signals(app);
    start_connecting(app);
    clawt_lrg_window_connection_changed(app);
    clawt_lrg_show_page(app, CLAWT_PAGE_CHAT);
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

ClawtLrgApp *
clawt_lrg_app_new(ClawtClient *client, ClawtConnection *connection)
{
    ClawtLrgApp *app = g_new0(ClawtLrgApp, 1);

    app->client = g_object_ref(client);
    app->connection = clawt_connection_copy(connection);
    app->context = g_main_context_ref(g_main_context_default());
    app->unread = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    app->typing = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        (GDestroyNotify)g_hash_table_unref);
    app->page = CLAWT_PAGE_CHAT;
    app->island.spend_micros = -1;
    app->scale = 1.0;
    app->sources = g_ptr_array_new_with_free_func(
        (GDestroyNotify)g_source_unref);

    /*
     * There is no portable way to ask the desktop for its colour scheme
     * from raylib; GNOME's answer is in GSettings, and it is only read if
     * the schema is installed (a missing schema aborts the process).
     */
    {
        GSettingsSchemaSource *source = g_settings_schema_source_get_default();
        g_autoptr(GSettingsSchema) schema = (source != NULL)
            ? g_settings_schema_source_lookup(source,
                                              "org.gnome.desktop.interface",
                                              TRUE)
            : NULL;

        app->system_dark = TRUE;

        if (schema != NULL && g_settings_schema_has_key(schema,
                                                        "color-scheme")) {
            g_autoptr(GSettings) settings =
                g_settings_new("org.gnome.desktop.interface");
            g_autofree gchar *scheme =
                g_settings_get_string(settings, "color-scheme");

            app->system_dark = g_strcmp0(scheme, "prefer-light") != 0 &&
                               g_strcmp0(scheme, "default") != 0;
        }
    }

    return app;
}

void
clawt_lrg_app_free(ClawtLrgApp *app)
{
    if (app == NULL)
        return;

    if (app->client != NULL)
        g_signal_handlers_disconnect_by_data(app->client, app);

    /*
     * First, before anything they point into is freed: a source whose
     * callback holds the app must not survive it.  Destroying one that
     * already finished is harmless.
     */
    if (app->sources != NULL) {
        guint i;

        for (i = 0; i < app->sources->len; i++)
            g_source_destroy(g_ptr_array_index(app->sources, i));

        g_clear_pointer(&app->sources, g_ptr_array_unref);
    }

    g_clear_object(&app->canvas);
    g_clear_object(&app->toasts);
    g_clear_object(&app->theme);
    g_clear_object(&app->client);
    g_clear_pointer(&app->connection, clawt_connection_free);
    g_clear_pointer(&app->appearance, clawt_appearance_free);
    g_clear_pointer(&app->agents, json_node_unref);
    g_clear_pointer(&app->rooms, json_node_unref);
    g_clear_pointer(&app->unread, g_hash_table_unref);
    g_clear_pointer(&app->typing, g_hash_table_unref);
    g_clear_pointer(&app->context, g_main_context_unref);
    g_free(app->agent_id);
    g_free(app->room_id);
    g_free(app->last_toast);
    g_free(app->daemon_version);
    g_free(app->daemon_update);
    g_free(app);
}

static gboolean
on_idle_ceiling(gpointer user_data)
{
    gboolean *fired = user_data;

    *fired = TRUE;
    return G_SOURCE_REMOVE;
}

/*
 * Waits up to @ms for GLib work, returning as soon as any is dispatched.
 * The timeout source is the ceiling; any socket readiness or reply
 * dispatched first ends the wait early.
 */
static void
idle_wait(ClawtLrgApp *app, guint ms)
{
    gboolean fired = FALSE;
    GSource *ceiling = g_timeout_source_new(ms);

    g_source_set_callback(ceiling, on_idle_ceiling, &fired, NULL);
    g_source_attach(ceiling, app->context);

    g_main_context_iteration(app->context, TRUE);

    if (!fired)
        g_source_destroy(ceiling);

    g_source_unref(ceiling);
}

static gboolean
input_is_active(const LrgCanvasInput *input, gfloat last_x, gfloat last_y)
{
    return input->n_keys > 0 || input->n_chars > 0 ||
           input->buttons_down != 0 || input->buttons_pressed != 0 ||
           input->buttons_released != 0 || input->wheel_x != 0.0f ||
           input->wheel_y != 0.0f || input->n_touches > 0 ||
           input->pointer_x != last_x || input->pointer_y != last_y;
}

/* Lets replies land: drains the context for about @ms milliseconds. */
static void
settle(ClawtLrgApp *app, guint ms)
{
    guint waited;

    for (waited = 0; waited < ms; waited += 10) {
        while (g_main_context_iteration(app->context, FALSE))
            ;
        g_usleep(10000);
    }
}

/*
 * `--self-test PAGE[:TARGET]`: build the window without opening one, show
 * PAGE (a page nick, or "shell"), optionally select TARGET (an agent id,
 * else a room id), lay out at 1280x800 and print libregnum's inspector
 * snapshot as JSON.  Tests assert on structure this way rather than on
 * pixels (tests/test-lrgui-widgets.c, tests/test-lrg-rooms.sh).
 */
/*
 * PAGE[:TARGET]: select TARGET (an agent id, else a room id) if given,
 * then show PAGE (a page nick; "shell" leaves the page alone).
 *
 * Returns: %FALSE if PAGE names no page
 */
static gboolean
select_spec(ClawtLrgApp *app, const gchar *spec)
{
    g_auto(GStrv) parts = g_strsplit(spec, ":", 2);
    const gchar *target = parts[1];
    guint n = clawt_page_count();
    guint i;

    if (target != NULL && *target != '\0') {
        if (clawt_lrg_find_agent(app, target) != NULL)
            clawt_lrg_select_agent(app, target);
        else
            clawt_lrg_select_room(app, target);
    }

    if (g_strcmp0(parts[0], "shell") == 0)
        return TRUE;

    for (i = 0; i < n; i++) {
        if (g_strcmp0(clawt_page_nick((ClawtPage)i), parts[0]) == 0) {
            clawt_lrg_show_page(app, (ClawtPage)i);
            return TRUE;
        }
    }

    return FALSE;
}

static gint
run_self_test(ClawtLrgApp *app, const gchar *spec)
{
    /* the fleet first, so a target can be found in it */
    settle(app, 1200);

    if (!select_spec(app, spec)) {
        g_printerr("%s: --self-test: no page in '%s'\n", CLAWT_LRG_BIN_NAME,
                   spec);
        return 2;
    }

    settle(app, 1200);

    lrg_widget_set_size(LRG_WIDGET(app->canvas), 1280.0f, 800.0f);
    lrg_widget_set_size(app->backdrop, 1280.0f, 800.0f);
    lrg_widget_set_size(app->root, 1280.0f, 800.0f);
    clawt_lrg_window_resize(app, 1280.0f, 800.0f);
    lrg_widget_layout(LRG_WIDGET(app->canvas));

    /*
     * {"page", "texts": [...], "tree": <inspector snapshot>}.  The
     * snapshot has the geometry and looks and no text at all, so the text
     * on screen is walked separately (clawt_lrg_visible_texts()).
     */
    {
        g_autoptr(LrgUIInspector) inspector = lrg_ui_inspector_new();
        g_autoptr(GPtrArray) texts = clawt_lrg_visible_texts(app->canvas);
        g_autoptr(JsonBuilder) builder = json_builder_new();
        g_autoptr(JsonParser) parser = json_parser_new();
        g_autoptr(JsonGenerator) generator = json_generator_new();
        g_autoptr(JsonNode) root = NULL;
        g_autofree gchar *dump = NULL;
        g_autofree gchar *out = NULL;
        guint t;

        lrg_ui_inspector_set_canvas(inspector, app->canvas);
        dump = lrg_ui_inspector_snapshot_json(inspector, FALSE);

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "page");
        json_builder_add_string_value(builder, clawt_page_nick(app->page));
        json_builder_set_member_name(builder, "texts");
        json_builder_begin_array(builder);

        for (t = 0; t < texts->len; t++)
            json_builder_add_string_value(builder,
                                          g_ptr_array_index(texts, t));

        json_builder_end_array(builder);

        if (dump != NULL && json_parser_load_from_data(parser, dump, -1,
                                                        NULL)) {
            json_builder_set_member_name(builder, "tree");
            json_builder_add_value(builder,
                                   json_node_copy(json_parser_get_root(parser)));
        }

        json_builder_end_object(builder);

        root = json_builder_get_root(builder);
        json_generator_set_root(generator, root);
        json_generator_set_pretty(generator, TRUE);
        out = json_generator_to_data(generator, NULL);
        g_print("%s\n", out);
    }

    return 0;
}

/*
 * Everything but the window and the loop: the theme, the canvas, the
 * shell and the pages' first build, connected to the client's signals.
 * Separate so tests/test-lrgui-widgets.c can build the real client on a
 * canvas with no window and lay it out.
 */
void
clawt_lrg_app_build(ClawtLrgApp *app)
{
    g_return_if_fail(app != NULL);
    g_return_if_fail(app->canvas == NULL);

    connect_signals(app);
    clawt_lrg_app_apply_appearance(app, NULL);

    app->canvas = lrg_canvas_new();
    lrg_canvas_set_paint_background(app->canvas, TRUE);

    /* the animated background is the canvas's first child: under everything */
    app->backdrop = LRG_WIDGET(lrg_panel_new());
    lrg_widget_add_class(app->backdrop, "clawt-backdrop");
    lrg_widget_set_pointer_transparent(app->backdrop, TRUE);
    clawt_lrg_take(LRG_WIDGET(app->canvas), app->backdrop);

    app->root = clawt_lrg_window_build(app);
    clawt_lrg_take(LRG_WIDGET(app->canvas), app->root);

    app->toasts = lrg_toast_queue_new();
    lrg_toast_queue_set_toast_halign(app->toasts, LRG_ALIGN_CENTER);
    lrg_toast_queue_set_toast_valign(app->toasts, LRG_ALIGN_END);
    clawt_lrg_add(LRG_WIDGET(app->canvas), LRG_WIDGET(app->toasts));

    lrg_canvas_add_shortcut(app->canvas, "<Control>k", "win.navigate", NULL);
    lrg_canvas_add_shortcut(app->canvas, "<Control>comma", "win.settings",
                            NULL);
    lrg_canvas_add_shortcut(app->canvas, "<Control>q", "win.quit", NULL);
}

gint
clawt_lrg_app_run(ClawtLrgApp *app, const gchar *self_test_page)
{
    g_autoptr(GrlWindow) window = NULL;
    LrgCanvasInput input;
    gfloat last_x = -1.0f;
    gfloat last_y = -1.0f;
    const gchar *screenshot_path = NULL;
    gboolean report_fps = FALSE;
    gint64 report_since = 0;
    gint64 report_busy = 0;
    gint64 report_worst = 0;
    gint64 frame_start = 0;
    guint report_frames = 0;
    guint screenshot_after = 0;
    guint frame_count = 0;

    g_return_val_if_fail(app != NULL, 1);

    if (self_test_page == NULL) {
        SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT |
                       FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT);
        SetTraceLogLevel(LOG_WARNING);
        window = grl_window_new(1360, 860, "clawtilla");
        SetWindowMinSize(720, 480);
        SetExitKey(KEY_NULL);
        grl_window_set_target_fps(window, 0);
    }

    clawt_lrg_app_build(app);
    start_connecting(app);

    if (self_test_page != NULL)
        return run_self_test(app, self_test_page);

    /*
     * CLAWT_LRG_SCREENSHOT=<file.png>: draw for CLAWT_LRG_SCREENSHOT_FRAMES
     * frames (default 150), save the window, exit.  How the screenshots in
     * docs/images/lrg/ are made, and how a test can look at the real
     * rendering rather than the widget tree.
     */
    {
        const gchar *frames = g_getenv("CLAWT_LRG_SCREENSHOT_FRAMES");

        screenshot_path = g_getenv("CLAWT_LRG_SCREENSHOT");
        screenshot_after = (frames != NULL) ? (guint)atoi(frames) : 150;
    }

    /*
     * CLAWT_LRG_FPS=1: every two seconds, print frames drawn and the mean
     * and worst frame time to stderr.  A frame rate is a measurement, not
     * a feeling; this is what the docs' performance notes are taken with.
     */
    report_fps = g_getenv("CLAWT_LRG_FPS") != NULL;
    report_since = g_get_monotonic_time();

    while (!app->quit && !grl_window_should_close(window)) {
        gfloat dt = GetFrameTime();
        gfloat w = (gfloat)GetScreenWidth();
        gfloat h = (gfloat)GetScreenHeight();
        gboolean active;
        gboolean animating;
        guint dispatched = 0;

        app->dirty = FALSE;

        /* 1. GLib: the daemon's socket, replies, events */
        while (dispatched < DISPATCH_LIMIT &&
               g_main_context_iteration(app->context, FALSE))
            dispatched++;

        /* 2. input */
        lrg_widget_set_size(LRG_WIDGET(app->canvas), w, h);
        lrg_widget_set_size(app->backdrop, w, h);
        lrg_widget_set_size(app->root, w, h);
        clawt_lrg_window_resize(app, w, h);

        lrg_canvas_input_init(&input);
        lrg_canvas_input_poll(&input, dt);
        active = input_is_active(&input, last_x, last_y);
        last_x = input.pointer_x;
        last_y = input.pointer_y;
        lrg_canvas_process_input(app->canvas, &input);

        /* 3. draw */
        frame_start = g_get_monotonic_time();
        grl_window_begin_drawing(window);
        lrg_canvas_render(app->canvas);
        grl_window_end_drawing(window);

        if (report_fps) {
            gint64 now = g_get_monotonic_time();
            gint64 took = now - frame_start;

            report_frames++;
            report_busy += took;
            report_worst = MAX(report_worst, took);

            if (now - report_since >= 2 * G_USEC_PER_SEC) {
                g_printerr("%s: %.1f fps, frame %.1f ms mean, %.1f ms worst\n",
                           CLAWT_LRG_BIN_NAME,
                           (gdouble)report_frames * G_USEC_PER_SEC /
                               (gdouble)(now - report_since),
                           (gdouble)report_busy / report_frames / 1000.0,
                           (gdouble)report_worst / 1000.0);
                report_since = now;
                report_frames = 0;
                report_busy = 0;
                report_worst = 0;
            }
        }

        /*
         * CLAWT_LRG_SCREENSHOT_PAGE=PAGE[:TARGET]: a third of the way in,
         * once the fleet has loaded, select TARGET and show PAGE -- the
         * same spelling --self-test takes.
         */
        if (screenshot_path != NULL && frame_count == screenshot_after / 3) {
            const gchar *spec = g_getenv("CLAWT_LRG_SCREENSHOT_PAGE");

            if (spec != NULL)
                select_spec(app, spec);
        }

        if (screenshot_path != NULL && ++frame_count >= screenshot_after) {
            Image shot = LoadImageFromScreen();

            ExportImage(shot, screenshot_path);
            UnloadImage(shot);
            break;
        }

        /* 4. pace */
        animating = lrg_canvas_update(app->canvas, 0.0f);

        if (active || animating || dispatched > 0 || app->dirty ||
            screenshot_path != NULL || GetTime() < wake_until) {
            if (active || dispatched > 0)
                wake_until = GetTime() + WAKE_SECONDS;
            continue;
        }

        {
            ClawtLrgBackground background =
                clawt_appearance_get_lrg_background(app->appearance);
            gboolean moving =
                background != CLAWT_LRG_BACKGROUND_NONE &&
                clawt_appearance_get_lrg_motion(app->appearance) !=
                    CLAWT_LRG_MOTION_NONE;

            idle_wait(app, moving ? ANIMATED_IDLE_WAIT_MS : IDLE_WAIT_MS);
        }
    }

    return 0;
}
