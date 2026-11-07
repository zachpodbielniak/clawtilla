/*
 * lrg-computer.c - The Computer page
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Mirrors clients/gtk/gtk-computer.c, plus the machine verbs GTK keeps
 * in the sidebar's Computer submenu (gtk-sidebar.c on_menu_computer())
 * and the rebuild it keeps in the inspector -- one place for everything
 * done *to the machine*, apart from the agent's own Start and Stop.
 *
 * The machine an agent has, in four views walked out of
 * clawt_computer_view_count(): a command run inside it, the screen while
 * somebody is watching, the paths from this machine that appear inside,
 * and the shared drop-box.  Walked rather than listed, because the GTK
 * and web clients draw the same four and a list spelled out three times
 * is a list that drifts.
 *
 * The screen is polled (QUIET) only while it is on screen: a watch held
 * while the window sat elsewhere would cost the agent a frame a second
 * for nothing, and the only symptom would be the agent being slower.
 * The poll is a GLib source on app->context that checks, every tick,
 * whether the page and the view are still the ones showing, and lets go
 * of the watch the first tick they are not.
 */

#include "clawt-lrg.h"
#include "clawt-lrg-agent-model.h"

#include <glib/gstdio.h>
#include <graylib.h>
#include <string.h>
#include <unistd.h>

/* How wide a frame is decoded: big enough to read, small enough to move. */
#define SCREEN_DECODE_WIDTH (1280)

/* How often the screen is re-read while it is showing. */
#define SCREEN_POLL_SECONDS (1)

#define MAX_VIEWS (8)

typedef struct {
    LrgWidget          *page;
    LrgWidget          *state_label;
    LrgWidget          *machine_bar;
    LrgWidget          *tab_bar;
    LrgWidget          *switcher;
    LrgWidget          *tabs[MAX_VIEWS];
    ClawtComputerView   views[MAX_VIEWS];
    guint               n_views;
    ClawtComputerView   view;

    /* shell */
    LrgWidget          *terminal;
    LrgWidget          *exec_entry;

    /* screen */
    LrgWidget          *screen_status;
    LrgWidget          *screen_picture;
    LrgWidget          *screen_take;
    LrgWidget          *screen_release;
    LrgWidget          *screen_viewer;
    LrgWidget          *screen_input;
    LrgWidget          *screen_click;
    LrgWidget          *screen_send;
    GrlTexture         *texture;
    gchar              *screen_agent;
    gchar              *viewer_address;
    gboolean            watching;
    gboolean            held;
    gboolean            status_inflight;
    gboolean            frame_inflight;
    GSource            *poll;

    /* mounts and exchange */
    LrgWidget          *mount_list;
    LrgWidget          *exchange_list;
} ComputerPage;

static ComputerPage cp;

static void refresh(ClawtLrgApp *app);
static void screen_refresh(ClawtLrgApp *app);

/* ── Helpers ──────────────────────────────────────────────────────── */

static JsonNode *
agent_payload_with_remove(const gchar *agent_id)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    /*
     * `remove` is a boolean and has to arrive as one: the daemon refuses
     * a stop that would destroy the machine unless told to go ahead.
     * Sent always, because the confirmation -- where one is needed --
     * has already happened by the time this is built.
     */
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder, agent_id);
    json_builder_set_member_name(builder, "remove");
    json_builder_add_boolean_value(builder, TRUE);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/* Clears a request's in-flight flag whether its reply ran or was dropped. */
static void
clear_flag(gpointer data)
{
    *(gboolean *)data = FALSE;
}

/* ── The machine ──────────────────────────────────────────────────── */

static void
on_status(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    g_autofree gchar *said =
        clawt_lrg_computer_state_text(clawt_lrg_selected_agent(app), reply);

    lrg_label_set_text(LRG_LABEL(cp.state_label), said);
    clawt_lrg_app_wake(app);
}

static void
on_lifecycle_done(ClawtLrgApp *app, JsonNode *reply, const GError *error,
                  gpointer user_data)
{
    const gchar *agent_id = user_data;

    if (reply != NULL) {
        g_autofree gchar *said =
            clawt_lrg_computer_lifecycle_text(agent_id, reply);

        clawt_lrg_toast(app, said);
    }

    /*
     * Refreshed whichever way it went: a stop that failed may still have
     * left the machine somewhere new, and a page disagreeing with a
     * toast is two answers on screen.
     */
    clawt_lrg_refresh_fleet(app);
    refresh(app);
}

/* The kind is chosen from a literal at each branch: `make parity` greps. */
static void
send_lifecycle(ClawtLrgApp *app, const gchar *verb)
{
    if (app->agent_id == NULL)
        return;

    if (g_strcmp0(verb, "start") == 0)
        clawt_lrg_request(app, "computer.start",
                          agent_payload_with_remove(app->agent_id),
                          CLAWT_LRG_REQUEST_NONE, on_lifecycle_done,
                          g_strdup(app->agent_id), g_free);
    else if (g_strcmp0(verb, "stop") == 0)
        clawt_lrg_request(app, "computer.stop",
                          agent_payload_with_remove(app->agent_id),
                          CLAWT_LRG_REQUEST_NONE, on_lifecycle_done,
                          g_strdup(app->agent_id), g_free);
    else
        clawt_lrg_request(app, "computer.restart",
                          agent_payload_with_remove(app->agent_id),
                          CLAWT_LRG_REQUEST_NONE, on_lifecycle_done,
                          g_strdup(app->agent_id), g_free);
}

static void
on_lifecycle_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    send_lifecycle(app, user_data);
}

/*
 * A container that does not keep itself is destroyed by a stop, so Stop
 * there is Stop-and-delete.  Asked first; the daemon also refuses it
 * without `remove`, so a client that did not know to ask is fenced, and
 * this dialog is what stops the fence being an error to decode.
 */
static void
on_lifecycle(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *verb = g_object_get_data(G_OBJECT(button), "clawt-verb");
    JsonObject *agent = clawt_lrg_selected_agent(app);
    g_autofree gchar *body = NULL;

    if (g_strcmp0(verb, "start") == 0 ||
        !clawt_ipc_payload_boolean(agent, "computer_stop_removes", FALSE)) {
        send_lifecycle(app, verb);
        return;
    }

    body = clawt_lrg_computer_stop_warning(app->agent_id);
    clawt_lrg_confirm(app,
                      g_strcmp0(verb, "stop") == 0
                          ? "Stop this machine, and lose it?"
                          : "Restart this machine, and lose it?",
                      body,
                      g_strcmp0(verb, "stop") == 0 ? "Stop" : "Restart",
                      TRUE, on_lifecycle_confirmed, (gpointer)verb, NULL);
}

static void
on_rebuilt(ClawtLrgApp *app, JsonNode *reply, const GError *error,
           gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, "Rebuilt. Its contents are gone, and cloud-init "
                             "runs again on the next start.");

    clawt_lrg_refresh_fleet(app);
    refresh(app);
}

static void
on_rebuild_confirmed(ClawtLrgApp *app, gpointer user_data)
{
    if (app->agent_id == NULL)
        return;

    clawt_lrg_request(app, "computer.rebuild",
                      clawt_ipc_payload_new("agent", app->agent_id, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_rebuilt, NULL, NULL);
}

/*
 * Rebuilding is the only way to apply what cloud-init reads at first
 * boot -- the login, the desktop, the package list -- which are fixed
 * for the life of the overlay, so changing them and restarting looks
 * like it did nothing.
 */
static void
on_rebuild(LrgButton *button, gpointer user_data)
{
    clawt_lrg_confirm(
        user_data, "Rebuild this computer?",
        "Everything in it is destroyed and it is built again from the image."
        "\n\nThis is what applies settings cloud-init only reads at first "
        "boot -- the login, the desktop, the packages -- which are otherwise "
        "fixed for the life of the machine. It is also how to get one back "
        "that was deleted outside clawtilla.\n\nThe agent has to be stopped.",
        "Rebuild", TRUE, on_rebuild_confirmed, NULL, NULL);
}

static LrgWidget *
machine_button(ClawtLrgApp *app, const gchar *label, const gchar *verb)
{
    LrgWidget *button = clawt_lrg_button(label, "flat",
                                         G_CALLBACK(on_lifecycle), app);

    g_object_set_data(G_OBJECT(button), "clawt-verb", (gpointer)verb);
    return button;
}

static void
draw_machine_bar(ClawtLrgApp *app, JsonObject *agent)
{
    gboolean stopped = g_strcmp0(clawt_ipc_payload_string(agent, "state"),
                                 "stopped") == 0;
    LrgWidget *rebuild;

    clawt_lrg_clear(cp.machine_bar);

    if (agent == NULL)
        return;

    /*
     * Only where the library says there is a machine to power: a host
     * agent's machine is this one, and "stopped" about the operator's
     * own workstation is the quiet lie the daemon refuses.  Emptied
     * rather than greyed, so nothing permanently dead is on screen.
     */
    if (clawt_lrg_computer_offers_lifecycle(agent)) {
        clawt_lrg_take(cp.machine_bar,
                       machine_button(app, "Start machine", "start"));
        clawt_lrg_take(cp.machine_bar,
                       machine_button(app, "Stop machine", "stop"));
        clawt_lrg_take(cp.machine_bar,
                       machine_button(app, "Restart machine", "restart"));
    }

    /*
     * Offered whenever a computer is configured, not only when one
     * exists -- the case it is most wanted for is a guest that has gone.
     * Not while the agent runs: destroying the machine underneath it is
     * not a thing to do carefully, it is a thing not to offer.
     */
    if (g_strcmp0(clawt_ipc_payload_string(agent, "computer"), "none") != 0 &&
        clawt_ipc_payload_string(agent, "computer") != NULL) {
        rebuild = clawt_lrg_button("Rebuild computer", "danger",
                                   G_CALLBACK(on_rebuild), app);
        lrg_widget_set_enabled(rebuild, stopped);
        clawt_lrg_take(cp.machine_bar, rebuild);

        if (!stopped)
            clawt_lrg_take(cp.machine_bar,
                           clawt_lrg_label("stop the agent to rebuild",
                                           "caption"));
    }
}

/* ── Shell ────────────────────────────────────────────────────────── */

typedef struct {
    gchar *agent_id;
} ExecJob;

static void
exec_job_free(gpointer data)
{
    ExecJob *job = data;

    g_free(job->agent_id);
    g_free(job);
}

static void
on_exec_done(ClawtLrgApp *app, JsonNode *reply, const GError *error,
             gpointer user_data)
{
    g_autofree gchar *output = NULL;

    /*
     * A failure is written into the console as well as toasted: the
     * console is the record of what was tried, and a command that left
     * no trace there reads as one that never ran.
     */
    if (reply == NULL) {
        g_autofree gchar *line = g_strdup_printf(
            "error: %s\n", error != NULL ? error->message : "no answer");

        lrg_text_editor_append(LRG_TEXT_EDITOR(cp.terminal), line);
        clawt_lrg_app_wake(app);
        return;
    }

    output = clawt_lrg_computer_exec_output(reply);
    lrg_text_editor_append(LRG_TEXT_EDITOR(cp.terminal), output);
    clawt_lrg_app_wake(app);
}

static void
on_exec(GObject *source, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    const gchar *command = clawt_lrg_entry_text(cp.exec_entry);
    g_autofree gchar *echo = NULL;
    g_autofree gchar *refusal = NULL;
    ExecJob *job;

    if (app->agent_id == NULL || command == NULL || *command == '\0')
        return;

    echo = g_strdup_printf("$ %s\n", command);
    lrg_text_editor_append(LRG_TEXT_EDITOR(cp.terminal), echo);

    /*
     * The string form is a command, not a shell line: `>`, `|` and `&&`
     * would arrive as literal text and the program would run and exit 0.
     * The daemon refuses such a line with this same library sentence;
     * asking it here only means the refusal lands in the console, where
     * the command was typed, rather than in a toast above it.
     */
    refusal = clawt_command_shell_syntax_refusal(command);

    if (refusal != NULL) {
        g_autofree gchar *line = g_strdup_printf("refused: %s\n", refusal);

        lrg_text_editor_append(LRG_TEXT_EDITOR(cp.terminal), line);
        clawt_lrg_app_wake(app);
        return;
    }

    job = g_new0(ExecJob, 1);
    job->agent_id = g_strdup(app->agent_id);

    clawt_lrg_request(app, "computer.exec",
                      clawt_ipc_payload_new("agent", app->agent_id,
                                            "command", command, NULL),
                      CLAWT_LRG_QUIET, on_exec_done, job, exec_job_free);

    lrg_text_input_set_text(LRG_TEXT_INPUT(cp.exec_entry), "");
}

static LrgWidget *
build_shell_view(ClawtLrgApp *app)
{
    LrgWidget *box = clawt_lrg_vbox(10.0f);
    LrgWidget *line = clawt_lrg_hbox(8.0f);

    cp.terminal = clawt_lrg_editor(NULL, NULL);
    lrg_text_editor_set_read_only(LRG_TEXT_EDITOR(cp.terminal), TRUE);
    lrg_widget_add_class(cp.terminal, "clawt-terminal");
    lrg_widget_set_min_size(cp.terminal, 0.0f, 360.0f);
    clawt_lrg_add(box, cp.terminal);
    g_object_unref(cp.terminal);

    cp.exec_entry = clawt_lrg_entry(NULL, "Command (an argv, not a shell "
                                          "line: bash -c '…' for pipes)");
    lrg_widget_set_fill_weight(cp.exec_entry, 1.0f);
    g_signal_connect(cp.exec_entry, "submitted", G_CALLBACK(on_exec), app);
    clawt_lrg_add(line, cp.exec_entry);
    g_object_unref(cp.exec_entry);

    clawt_lrg_take(line, clawt_lrg_button("Run", "primary",
                                          G_CALLBACK(on_exec), app));
    clawt_lrg_take(box, line);

    return box;
}

/* ── Screen ───────────────────────────────────────────────────────── */

/*
 * Subscribes, or lets go, so the daemon only grabs while this view is
 * the one somebody is looking at.  Recorded whether or not the request
 * succeeds: a client that remembered only a successful subscribe would
 * try again on every tick against an agent with no screen -- the shape
 * this codebase was bitten by at the event subscription.
 */
static void
screen_set_watching(ClawtLrgApp *app, gboolean want)
{
    if (cp.watching == want)
        return;

    if (cp.screen_agent == NULL && want)
        return;

    if (want)
        clawt_lrg_request(app, "computer.observe",
                          clawt_ipc_payload_new("agent", cp.screen_agent,
                                                "watcher", CLAWT_LRG_BIN_NAME,
                                                NULL),
                          CLAWT_LRG_QUIET, NULL, NULL, NULL);
    else
        clawt_lrg_request(app, "computer.observe_stop",
                          clawt_ipc_payload_new("agent", cp.screen_agent,
                                                "watcher", CLAWT_LRG_BIN_NAME,
                                                NULL),
                          CLAWT_LRG_QUIET, NULL, NULL, NULL);

    cp.watching = want;
}

static void
screen_clear_picture(void)
{
    if (cp.screen_picture != NULL)
        lrg_image_set_texture(LRG_IMAGE(cp.screen_picture), NULL);

    g_clear_object(&cp.texture);
}

/*
 * The agent the screen describes.  The watch on the old one goes
 * *before* the id is replaced -- unsubscribing after would name the new
 * agent, leaving the first grabbed for ever with nobody watching.
 */
static void
screen_follow_agent(ClawtLrgApp *app)
{
    gboolean was_watching = cp.watching;

    if (g_strcmp0(app->agent_id, cp.screen_agent) == 0)
        return;

    screen_set_watching(app, FALSE);
    g_free(cp.screen_agent);
    cp.screen_agent = g_strdup(app->agent_id);
    g_clear_pointer(&cp.viewer_address, g_free);
    cp.held = FALSE;
    screen_clear_picture();

    if (was_watching)
        screen_set_watching(app, TRUE);
}

/*
 * Image bytes to a texture at the drawn width.
 *
 * The approach lrg-util.c takes for avatars: graylib loads by filename
 * and picks its decoder from the extension, so the bytes go through a
 * temporary file named after their *sniffed* type, never anything the
 * daemon claimed.  Decoded at SCREEN_DECODE_WIDTH rather than full size,
 * because a 4K frame arriving once a second is the trap the GTK client
 * records for its picture.
 */
static GrlTexture *
decode_frame(const guchar *data, gsize length)
{
    const gchar *mime = clawt_avatar_sniff_mime_type(data, length);
    g_autofree gchar *template = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GrlImage) image = NULL;
    GrlTexture *texture = NULL;
    gint fd;

    if (mime == NULL)
        return NULL;

    template = g_strdup_printf("clawtilla-frame-XXXXXX%s",
                               clawt_avatar_extension_for_mime_type(mime));
    fd = g_file_open_tmp(template, &path, &error);

    if (fd < 0) {
        g_debug("screen: %s", error->message);
        return NULL;
    }

    close(fd);

    if (g_file_set_contents(path, (const gchar *)data, (gssize)length,
                            &error)) {
        image = grl_image_new_from_file(path);

        if (image != NULL && grl_image_get_width(image) > 0) {
            gint width = grl_image_get_width(image);
            gint height = grl_image_get_height(image);

            /* down only: a small frame is never blown up into a blur */
            if (width > SCREEN_DECODE_WIDTH) {
                height = (gint)((gint64)height * SCREEN_DECODE_WIDTH / width);
                width = SCREEN_DECODE_WIDTH;
                grl_image_resize(image, width, MAX(height, 1));
            }

            texture = grl_texture_new_from_image(image);
        }
    } else {
        g_debug("screen: %s", error->message);
    }

    g_unlink(path);
    return texture;
}

static void
on_frame(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    const gchar *base64 = clawt_ipc_payload_string(
        clawt_ipc_reply_object(reply), "base64");
    g_autofree guchar *raw = NULL;
    GrlTexture *texture;
    gsize length = 0;

    if (base64 == NULL || cp.screen_picture == NULL)
        return;

    raw = g_base64_decode(base64, &length);
    texture = (raw != NULL && length > 0) ? decode_frame(raw, length) : NULL;

    if (texture == NULL)
        return;

    lrg_image_set_texture(LRG_IMAGE(cp.screen_picture), texture);
    lrg_widget_set_max_size(cp.screen_picture,
                            (gfloat)grl_texture_get_width(texture),
                            (gfloat)grl_texture_get_height(texture));
    g_clear_object(&cp.texture);
    cp.texture = texture;
    clawt_lrg_app_wake(app);
}

static void
request_frame(ClawtLrgApp *app)
{
    if (cp.frame_inflight || cp.screen_agent == NULL)
        return;

    cp.frame_inflight = TRUE;
    clawt_lrg_request(app, "computer.frame",
                      clawt_ipc_payload_new("agent", cp.screen_agent, NULL),
                      CLAWT_LRG_QUIET | CLAWT_LRG_GUARDED, on_frame,
                      &cp.frame_inflight, clear_flag);
}

static void
on_screen(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    g_autofree gchar *said =
        clawt_lrg_computer_screen_text(root, g_get_real_time());
    gboolean observable = clawt_ipc_payload_boolean(root, "observable",
                                                    FALSE);
    gboolean can_input;

    lrg_label_set_text(LRG_LABEL(cp.screen_status), said);

    if (root == NULL) {
        clawt_lrg_app_wake(app);
        return;
    }

    cp.held = clawt_ipc_payload_boolean(root, "held", FALSE);
    g_free(cp.viewer_address);
    cp.viewer_address = g_strdup(clawt_ipc_payload_string(root, "viewer"));

    /*
     * Input only while this client holds the screen, and only where the
     * machine can take it: an agent driving the user's screen when it
     * was told to drive its own is the wrong way round to be wrong.
     */
    can_input = cp.held && clawt_ipc_payload_boolean(root, "can_input", TRUE);

    lrg_widget_set_visible(cp.screen_take, !cp.held && observable);
    lrg_widget_set_visible(cp.screen_release, cp.held);
    lrg_widget_set_visible(cp.screen_viewer, cp.viewer_address != NULL);
    lrg_widget_set_enabled(cp.screen_input, can_input);
    lrg_widget_set_enabled(cp.screen_click, can_input);
    lrg_widget_set_enabled(cp.screen_send, can_input);

    if (!observable)
        screen_clear_picture();
    else
        request_frame(app);

    clawt_lrg_app_wake(app);
}

static void
screen_refresh(ClawtLrgApp *app)
{
    if (cp.screen_agent == NULL) {
        lrg_label_set_text(LRG_LABEL(cp.screen_status), "No agent selected.");
        screen_clear_picture();
        return;
    }

    /*
     * The lease is pushed forward on every read, not only when the view
     * opens: a watch is a lease so a client that went away stops costing
     * the agent frames, and a window left on this view longer than the
     * lease would otherwise be dropped by the same rule.
     */
    if (cp.watching)
        clawt_lrg_request(app, "computer.observe",
                          clawt_ipc_payload_new("agent", cp.screen_agent,
                                                "watcher", CLAWT_LRG_BIN_NAME,
                                                NULL),
                          CLAWT_LRG_QUIET, NULL, NULL, NULL);

    if (cp.status_inflight)
        return;

    cp.status_inflight = TRUE;
    clawt_lrg_request(app, "computer.screen",
                      clawt_ipc_payload_new("agent", cp.screen_agent, NULL),
                      CLAWT_LRG_QUIET | CLAWT_LRG_GUARDED, on_screen,
                      &cp.status_inflight, clear_flag);
}

static gboolean
screen_showing(ClawtLrgApp *app)
{
    return app->page == CLAWT_PAGE_COMPUTER &&
           cp.view == CLAWT_COMPUTER_VIEW_SCREEN && app->agent_id != NULL;
}

static gboolean
on_poll(gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    /*
     * The page has no "hidden" callback, so the tick asks: the first one
     * after somebody leaves lets go of the watch and stops the timer.
     */
    if (!screen_showing(app)) {
        screen_set_watching(app, FALSE);
        g_clear_pointer(&cp.poll, g_source_unref);
        return G_SOURCE_REMOVE;
    }

    screen_follow_agent(app);
    screen_set_watching(app, TRUE);
    screen_refresh(app);

    return G_SOURCE_CONTINUE;
}

static void
screen_start_polling(ClawtLrgApp *app)
{
    if (!screen_showing(app))
        return;

    screen_follow_agent(app);
    screen_set_watching(app, TRUE);
    screen_refresh(app);

    if (cp.poll != NULL)
        return;

    /*
     * Attached to app->context by name: the ambient-context APIs reach a
     * loop nobody runs in an embedder (AGENTS.md, "Main contexts").
     */
    cp.poll = g_timeout_source_new_seconds(SCREEN_POLL_SECONDS);
    g_source_set_callback(cp.poll, on_poll, app, NULL);
    clawt_lrg_app_attach(app, cp.poll);
}

static void
on_taken(ClawtLrgApp *app, JsonNode *reply, const GError *error,
         gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, "You have the screen. The agent's clicks are "
                             "refused until you give it back.");

    screen_refresh(app);
}

static void
on_screen_take(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    if (cp.screen_agent == NULL)
        return;

    clawt_lrg_request(app, "computer.takeover",
                      clawt_ipc_payload_new("agent", cp.screen_agent,
                                            "holder", "the libregnum client",
                                            NULL),
                      CLAWT_LRG_REQUEST_NONE, on_taken, NULL, NULL);
}

static void
on_released(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    if (reply != NULL)
        clawt_lrg_toast(app, "The agent has the screen back.");

    screen_refresh(app);
}

static void
on_screen_release(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;

    if (cp.screen_agent == NULL)
        return;

    clawt_lrg_request(app, "computer.release",
                      clawt_ipc_payload_new("agent", cp.screen_agent, NULL),
                      CLAWT_LRG_REQUEST_NONE, on_released, NULL, NULL);
}

/*
 * Opens the VM's own VNC server in a real viewer.  Beside the preview,
 * not instead of it: a frame a second is right for watching and hopeless
 * for using.  remote-viewer is spawned by name and its absence said
 * plainly -- a button that silently does nothing is worse than one that
 * explains what is missing.
 */
static void
on_screen_viewer(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autofree gchar *binary = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *argv[3];

    if (cp.viewer_address == NULL)
        return;

    binary = g_find_program_in_path("remote-viewer");

    if (binary == NULL) {
        clawt_lrg_toast(app, "remote-viewer is not installed (Fedora: "
                             "virt-viewer). The address is on the Screen tab.");
        return;
    }

    argv[0] = binary;
    argv[1] = cp.viewer_address;
    argv[2] = NULL;

    if (!g_spawn_async(NULL, (gchar **)argv, NULL, G_SPAWN_DEFAULT, NULL,
                       NULL, NULL, &error))
        clawt_lrg_toast(app, error->message);
}

static void
on_input_sent(ClawtLrgApp *app, JsonNode *reply, const GError *error,
              gpointer user_data)
{
    if (reply != NULL)
        lrg_text_input_set_text(LRG_TEXT_INPUT(cp.screen_input), "");

    screen_refresh(app);
}

/* One event, from whichever field was filled; the model picks which. */
static void
on_screen_input(GObject *source, gpointer user_data)
{
    ClawtLrgApp *app = user_data;
    g_autofree gchar *refusal = NULL;
    JsonNode *payload;

    if (cp.screen_agent == NULL || !cp.held)
        return;

    payload = clawt_lrg_computer_input_payload(
        cp.screen_agent, clawt_lrg_entry_text(cp.screen_input),
        clawt_lrg_entry_text(cp.screen_click), &refusal);

    if (payload == NULL) {
        clawt_lrg_toast(app, refusal);
        return;
    }

    clawt_lrg_request(app, "computer.input", payload, CLAWT_LRG_REQUEST_NONE,
                      on_input_sent, NULL, NULL);
}

static LrgWidget *
build_screen_view(ClawtLrgApp *app)
{
    LrgWidget *box = clawt_lrg_vbox(10.0f);
    LrgWidget *buttons = clawt_lrg_hbox(8.0f);
    LrgWidget *input = clawt_lrg_hbox(8.0f);
    LrgWidget *frame = clawt_lrg_card(NULL);
    LrgImage *screen_picture = lrg_image_new();

    cp.screen_status = clawt_lrg_paragraph("No agent selected.", "muted");
    clawt_lrg_add(box, cp.screen_status);
    g_object_unref(cp.screen_status);

    /*
     * FIT, and capped at the frame's own size once one arrives: a frame
     * smaller than the panel must not be blown up, and the coordinates a
     * person reads off it stay the picture's own.
     */
    lrg_image_set_scale_mode(screen_picture, LRG_IMAGE_SCALE_MODE_FIT);
    lrg_image_set_corner_radius(screen_picture, 8.0f);
    lrg_widget_set_min_size(LRG_WIDGET(screen_picture), 0.0f, 240.0f);
    cp.screen_picture = LRG_WIDGET(screen_picture);
    clawt_lrg_take(frame, LRG_WIDGET(screen_picture));
    clawt_lrg_take(box, frame);

    cp.screen_take = clawt_lrg_button("Take the screen", NULL,
                                      G_CALLBACK(on_screen_take), app);
    cp.screen_release = clawt_lrg_button("Give the screen back", "primary",
                                         G_CALLBACK(on_screen_release), app);
    cp.screen_viewer = clawt_lrg_button("Open in a viewer", "flat",
                                        G_CALLBACK(on_screen_viewer), app);
    lrg_widget_set_visible(cp.screen_release, FALSE);
    lrg_widget_set_visible(cp.screen_viewer, FALSE);
    clawt_lrg_take(buttons, cp.screen_take);
    clawt_lrg_take(buttons, cp.screen_release);
    clawt_lrg_take(buttons, cp.screen_viewer);
    clawt_lrg_take(box, buttons);

    cp.screen_input = clawt_lrg_entry(NULL, "Type, or @key for a key or combo");
    lrg_widget_set_fill_weight(cp.screen_input, 1.0f);
    g_signal_connect(cp.screen_input, "submitted",
                     G_CALLBACK(on_screen_input), app);
    cp.screen_click = clawt_lrg_entry(NULL, "Click at x,y");
    lrg_widget_set_min_size(cp.screen_click, 140.0f, 0.0f);
    g_signal_connect(cp.screen_click, "submitted",
                     G_CALLBACK(on_screen_input), app);
    cp.screen_send = clawt_lrg_button("Send", "primary",
                                      G_CALLBACK(on_screen_input), app);
    lrg_widget_set_enabled(cp.screen_input, FALSE);
    lrg_widget_set_enabled(cp.screen_click, FALSE);
    lrg_widget_set_enabled(cp.screen_send, FALSE);
    clawt_lrg_take(input, cp.screen_input);
    clawt_lrg_take(input, cp.screen_click);
    clawt_lrg_take(input, cp.screen_send);
    clawt_lrg_take(box, input);

    clawt_lrg_take(box, clawt_lrg_paragraph(
        "Taking the screen refuses the agent's own clicks until you give it "
        "back. Input is sent only while you hold it.", "caption"));

    return box;
}

/* ── Mounts and exchange ──────────────────────────────────────────── */

static void
on_mounts(ClawtLrgApp *app, JsonNode *reply, const GError *error,
          gpointer user_data)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *mounts = (root != NULL && json_object_has_member(root,
                                                                "mounts"))
        ? json_object_get_array_member(root, "mounts") : NULL;
    guint n = (mounts != NULL) ? json_array_get_length(mounts) : 0;
    guint i;

    clawt_lrg_clear(cp.mount_list);

    if (n == 0) {
        clawt_lrg_take(cp.mount_list, clawt_lrg_empty_state(
            "No mounts of its own",
            "Every computer still gets the agent's workspace and the "
            "exchange. Share a folder from the Overview page."));
        clawt_lrg_app_wake(app);
        return;
    }

    for (i = 0; i < n; i++) {
        JsonObject *mount = json_array_get_object_element(mounts, i);
        g_autofree gchar *title = clawt_lrg_computer_mount_title(mount);
        LrgWidget *row = clawt_lrg_row(
            title, clawt_ipc_payload_string(mount, "mode"));

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(cp.mount_list, row);
    }

    clawt_lrg_app_wake(app);
}

static void
on_exchange(ClawtLrgApp *app, JsonNode *reply, const GError *error,
            gpointer user_data)
{
    g_autoptr(GPtrArray) names = clawt_lrg_computer_exchange_names(reply);
    guint i;

    clawt_lrg_clear(cp.exchange_list);

    if (names->len == 0) {
        clawt_lrg_take(cp.exchange_list, clawt_lrg_empty_state(
            "Nothing in the exchange",
            reply == NULL ? "The exchange could not be read."
                          : "Files any agent or you drop here are visible to "
                            "every computer that shares host paths."));
        clawt_lrg_app_wake(app);
        return;
    }

    for (i = 0; i < names->len; i++) {
        LrgWidget *row = clawt_lrg_row(g_ptr_array_index(names, i), NULL);

        clawt_lrg_appear(app, row, i);
        clawt_lrg_take(cp.exchange_list, row);
    }

    clawt_lrg_app_wake(app);
}

/* ── Views ────────────────────────────────────────────────────────── */

static void
refresh_view(ClawtLrgApp *app)
{
    /*
     * A switch with no default: -Wswitch names a fifth view the library
     * adds, rather than it falling through to nothing.
     */
    switch (cp.view) {
    case CLAWT_COMPUTER_VIEW_SHELL:
        break;

    case CLAWT_COMPUTER_VIEW_SCREEN:
        screen_start_polling(app);
        break;

    case CLAWT_COMPUTER_VIEW_MOUNTS:
        if (app->agent_id != NULL)
            clawt_lrg_request(app, "agent.mount.list",
                              clawt_ipc_payload_new("agent", app->agent_id,
                                                    NULL),
                              CLAWT_LRG_GUARDED, on_mounts, NULL, NULL);
        break;

    case CLAWT_COMPUTER_VIEW_EXCHANGE:
        clawt_lrg_request(app, "exchange.list", NULL, CLAWT_LRG_QUIET,
                          on_exchange, NULL, NULL);
        break;
    }

    /* the watch follows the view: nothing grabbed behind another tab */
    if (cp.view != CLAWT_COMPUTER_VIEW_SCREEN)
        screen_set_watching(app, FALSE);
}

static void
show_view(ClawtLrgApp *app, ClawtComputerView view)
{
    guint i;

    cp.view = view;

    for (i = 0; i < cp.n_views; i++) {
        if (cp.views[i] == view) {
            lrg_widget_add_class(cp.tabs[i], "clawt-tab-active");
            lrg_widget_set_state_flags(cp.tabs[i], LRG_WIDGET_STATE_SELECTED);
            lrg_switcher_set_visible_child_name(
                LRG_SWITCHER(cp.switcher),
                clawt_computer_view_nth_nick(i));
        } else {
            lrg_widget_remove_class(cp.tabs[i], "clawt-tab-active");
            lrg_widget_unset_state_flags(cp.tabs[i],
                                         LRG_WIDGET_STATE_SELECTED);
        }
    }

    refresh_view(app);
    clawt_lrg_app_wake(app);
}

static void
on_view_clicked(LrgButton *button, gpointer user_data)
{
    ClawtLrgApp *app = g_object_get_data(G_OBJECT(button), "clawt-app");

    show_view(app, (ClawtComputerView)GPOINTER_TO_UINT(user_data));
}

/*
 * The Screen tab is there only where the daemon says there could be a
 * screen (`computer_screen`, from clawt_computer_type_has_screen()),
 * rather than from a list of types here that would offer it on a backend
 * with none.
 */
static void
sync_tabs(ClawtLrgApp *app, JsonObject *agent)
{
    gboolean has_screen = clawt_lrg_computer_offers_screen(agent);
    guint i;

    for (i = 0; i < cp.n_views; i++) {
        if (cp.views[i] == CLAWT_COMPUTER_VIEW_SCREEN)
            lrg_widget_set_visible(cp.tabs[i], has_screen);
    }

    if (cp.view == CLAWT_COMPUTER_VIEW_SCREEN && !has_screen)
        cp.view = CLAWT_COMPUTER_VIEW_SHELL;
}

static void
refresh(ClawtLrgApp *app)
{
    JsonObject *agent = clawt_lrg_selected_agent(app);
    g_autofree gchar *said = NULL;

    if (cp.page == NULL)
        return;

    /* a different agent: let go of the old one's screen first */
    screen_follow_agent(app);

    said = clawt_lrg_computer_state_text(agent, NULL);
    lrg_label_set_text(LRG_LABEL(cp.state_label), said);
    lrg_widget_set_enabled(cp.exec_entry,
                           agent != NULL && clawt_lrg_computer_present(agent));

    draw_machine_bar(app, agent);
    sync_tabs(app, agent);

    /*
     * The daemon's own state is what says *why*; a stopped agent has no
     * computer object, which on_status() says rather than toasting.
     */
    if (agent != NULL && clawt_lrg_computer_present(agent))
        clawt_lrg_request(app, "computer.status",
                          clawt_ipc_payload_new("agent", app->agent_id, NULL),
                          CLAWT_LRG_QUIET | CLAWT_LRG_GUARDED, on_status,
                          NULL, NULL);

    show_view(app, cp.view);
}

static void
event(ClawtLrgApp *app, ClawtEvent *ev)
{
    const gchar *kind = clawt_event_get_kind(ev);
    const gchar *subject = clawt_event_get_subject(ev);

    if (kind == NULL || app->page != CLAWT_PAGE_COMPUTER)
        return;

    /* A new frame for the agent on screen: fetch it now, not next tick. */
    if (g_strcmp0(kind, "computer.frame") == 0) {
        if (screen_showing(app) && g_strcmp0(subject, cp.screen_agent) == 0)
            request_frame(app);
        return;
    }

    if (g_strcmp0(kind, "computer.takeover") == 0 ||
        g_strcmp0(kind, "computer.frame_failed") == 0) {
        if (screen_showing(app))
            screen_refresh(app);
        return;
    }

    /*
     * The machine came or went, or the agent's config changed under it.
     * A typing frame is a level re-sent every 25 seconds and says nothing
     * about the machine.
     */
    if (g_strcmp0(kind, "computer.power") == 0 ||
        (g_str_has_prefix(kind, "agent.") &&
         g_strcmp0(kind, "agent.typing") != 0)) {
        if (subject == NULL || g_strcmp0(subject, app->agent_id) == 0)
            refresh(app);
    }
}

static LrgWidget *
build_list_view(LrgWidget **out)
{
    LrgWidget *box = clawt_lrg_vbox(8.0f);

    *out = box;
    return box;
}

static LrgWidget *
build(ClawtLrgApp *app)
{
    LrgWidget *page = clawt_lrg_page_new("Computer", NULL);
    LrgWidget *body = clawt_lrg_page_body(page);
    LrgSwitcher *switcher = lrg_switcher_new();
    LrgWidget *header = clawt_lrg_card(NULL);
    LrgWidget *header_box = clawt_lrg_vbox(8.0f);
    guint n = MIN(clawt_computer_view_count(), MAX_VIEWS);
    guint i;

    memset(&cp, 0, sizeof(cp));
    cp.page = page;
    cp.view = CLAWT_COMPUTER_VIEW_SHELL;

    cp.state_label = clawt_lrg_label("No agent selected.", "heading");
    cp.machine_bar = clawt_lrg_hbox(8.0f);
    clawt_lrg_add(header_box, cp.state_label);
    clawt_lrg_add(header_box, cp.machine_bar);
    g_object_unref(cp.state_label);
    g_object_unref(cp.machine_bar);
    clawt_lrg_take(header, header_box);
    clawt_lrg_take(body, header);

    cp.tab_bar = clawt_lrg_hbox(4.0f);
    clawt_lrg_add(body, cp.tab_bar);
    g_object_unref(cp.tab_bar);

    lrg_switcher_set_transition(switcher, LRG_SWITCHER_TRANSITION_CROSSFADE);
    lrg_switcher_set_interpolate_size(switcher, TRUE);
    cp.switcher = LRG_WIDGET(switcher);

    /*
     * The views, walked.  `make parity` compares which enumerations each
     * client walks, so a fifth added to the library appears in all three
     * clients or the check says which one lacks it.
     */
    for (i = 0; i < n; i++) {
        ClawtComputerView view = clawt_computer_view_nth(i);
        LrgWidget *child = NULL;
        LrgWidget *tab;

        switch (view) {
        case CLAWT_COMPUTER_VIEW_SHELL:
            child = build_shell_view(app);
            break;

        case CLAWT_COMPUTER_VIEW_SCREEN:
            child = build_screen_view(app);
            break;

        case CLAWT_COMPUTER_VIEW_MOUNTS:
            child = build_list_view(&cp.mount_list);
            break;

        case CLAWT_COMPUTER_VIEW_EXCHANGE:
            child = build_list_view(&cp.exchange_list);
            break;
        }

        lrg_switcher_add_named(switcher, child,
                               clawt_computer_view_nth_nick(i));
        g_object_unref(child);

        tab = clawt_lrg_button(clawt_computer_view_nth_label(i), "flat",
                               G_CALLBACK(on_view_clicked),
                               GUINT_TO_POINTER(view));
        lrg_widget_add_class(tab, "clawt-tab");
        g_object_set_data(G_OBJECT(tab), "clawt-app", app);
        cp.tabs[cp.n_views] = tab;
        cp.views[cp.n_views] = view;
        cp.n_views++;
        clawt_lrg_take(cp.tab_bar, tab);
    }

    clawt_lrg_take(body, LRG_WIDGET(switcher));

    return page;
}

static const ClawtLrgPage page_impl = {
    CLAWT_PAGE_COMPUTER, build, refresh, event
};

const ClawtLrgPage *
clawt_lrg_computer_page(void)
{
    return &page_impl;
}
