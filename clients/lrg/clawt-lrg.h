/*
 * clawt-lrg.h - Internal API of the libregnum client
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawtilla-lrg is a third graphical client, drawn with libregnum's
 * retained widgets on an OpenGL canvas instead of GTK.  It does the same
 * things as clawtilla-gtk -- `make parity` holds all three to that -- and
 * is shaped the same way: a shell (lrg-window.c) and one file per page,
 * each mirroring the GTK file of the same name.
 *
 * Three rules every file here follows, each the answer to a failure
 * another client has already had:
 *
 *  - **Async only.**  The app owns its loop (lrg-app.c) and drains the
 *    GLib context once per frame.  clawt_client_request() iterates that
 *    context while it waits, which from inside a frame would re-enter the
 *    widget tree being drawn -- so nothing here calls it.  Every request
 *    goes through clawt_lrg_request(), and its reply arrives on a later
 *    frame.
 *
 *  - **Replies are guarded.**  A reply that lands after the person moved
 *    on must not repaint the page they left (the GTK chat once came back
 *    filtered on another agent's room this way).  %CLAWT_LRG_GUARDED drops
 *    a reply whose selection generation has moved on.
 *
 *  - **Decisions live in models.**  A page that has to decide something
 *    -- what a row says, which actions a computer type offers -- asks a
 *    function in clawt-lrg-model.h, tested without libregnum.
 */

#pragma once

#include <clawtilla.h>
#include <libregnum.h>

/*
 * 0.8.5 stops recording widgets wholly outside a clip and measures a
 * markdown label through its layout cache.  Before it, every message in
 * a scrolled transcript was laid out and recorded every frame, and
 * loading one re-wrapped every message per message added: 3 fps and a
 * pegged core on a real fleet.
 *
 * 0.8.6 lets a box short of room take width back from its flexible
 * children.  Before it, one long one-line label -- a chief of staff's
 * description in the header -- ran the island, the alerts and the menu
 * off the right edge, and a transcript laid out wide never came back in
 * after the window shrank.
 *
 * Neither is a new symbol LRG_REQUIRED_SYMBOLS could look for, so the
 * version is the check.
 */
#if !LRG_CHECK_VERSION(0, 8, 6)
#error "clawtilla-lrg needs libregnum 0.8.6 or newer; update the checkout at LRG_DIR"
#endif

/* The smallest window the client allows; every page is tested to fit it. */
#define CLAWT_LRG_MIN_WIDTH  (640)
#define CLAWT_LRG_MIN_HEIGHT (480)

/*
 * Below this the sidebar is never beside the page -- 340 pixels of it
 * left a column too thin to read -- and its toggle opens it as a drawer
 * over the page instead.  GTK collapses at 800; this client's header
 * and tabs are wider, so it gives way sooner.
 */
#define CLAWT_LRG_NARROW_WIDTH (1000)

#include "clawt-lrg-model.h"

G_BEGIN_DECLS

#define CLAWT_LRG_BIN_NAME "clawtilla-lrg"

/* How big an avatar texture is decoded (it is drawn at most this size). */
#define CLAWT_LRG_AVATAR_DECODE_SIZE (128)

typedef struct _ClawtLrgApp ClawtLrgApp;

/**
 * ClawtLrgRequestFlags:
 * @CLAWT_LRG_REQUEST_NONE: toast a failure, deliver the reply whenever it lands
 * @CLAWT_LRG_QUIET: never toast; for polls, where a failure every tick
 *   would stack one toast per second over the page (the computer frame did)
 * @CLAWT_LRG_GUARDED: drop the reply if the selection changed meanwhile
 */
typedef enum {
    CLAWT_LRG_REQUEST_NONE = 0,
    CLAWT_LRG_QUIET = 1 << 0,
    CLAWT_LRG_GUARDED = 1 << 1
} ClawtLrgRequestFlags;

/**
 * ClawtLrgReplyFunc:
 * @app: the app
 * @reply: (nullable): the reply payload, or %NULL if the request failed
 *   (already toasted unless %CLAWT_LRG_QUIET)
 * @error: (nullable): why it failed
 * @user_data: what the caller passed
 */
typedef void (*ClawtLrgReplyFunc)(ClawtLrgApp  *app,
                                  JsonNode     *reply,
                                  const GError *error,
                                  gpointer      user_data);

/**
 * ClawtLrgPage:
 * @page: which #ClawtPage this implements
 * @build: called once; returns the page's root widget (transfer full)
 * @refresh: (nullable): the page was shown, or the selection changed
 *   while it was showing; re-read whatever it draws
 * @event: (nullable): every daemon event, whether or not the page is
 *   showing; a page keeps its own state current and repaints if visible
 *
 * One page of the window.  The shell walks clawt_page_count() and asks
 * clawt_lrg_page_for() for each, so a #ClawtPage added to the library
 * without a page here is a build-time NULL the shell reports, not a tab
 * that silently goes nowhere.
 */
typedef struct {
    ClawtPage    page;
    LrgWidget *(*build)(ClawtLrgApp *app);
    void       (*refresh)(ClawtLrgApp *app);
    void       (*event)(ClawtLrgApp *app, ClawtEvent *event);
} ClawtLrgPage;

/**
 * ClawtLrgSettingsPage:
 * @id: a stable id, also the parity marker for the page
 * @title: the page's name in the settings sidebar
 * @build: returns the page's root widget (transfer full)
 * @refresh: (nullable): re-read from the daemon; called when shown
 *
 * One page of the Settings dialog, in the order GTK draws them.
 */
typedef struct {
    const gchar *id;
    const gchar *title;
    LrgWidget *(*build)(ClawtLrgApp *app);
    void       (*refresh)(ClawtLrgApp *app);
} ClawtLrgSettingsPage;

struct _ClawtLrgApp {
    /* ── the daemon ── */
    ClawtClient     *client;
    ClawtConnection *connection;
    GMainContext    *context;

    /* ── look ── */
    ClawtAppearance *appearance;
    LrgTheme        *theme;
    gdouble          scale;
    gboolean         system_dark;

    /* ── the window ── */
    LrgCanvas       *canvas;
    LrgWidget       *backdrop;     /* animated background, under everything */
    LrgWidget       *root;         /* the shell's layout */
    LrgToastQueue   *toasts;
    gboolean         quit;

    /*
     * Set by anything that changed what is on screen without input --
     * an event, a reply -- so the frame loop draws at full rate for a
     * moment instead of sleeping through it.
     */
    gboolean         dirty;

    /* ── what is selected ── */
    gchar           *agent_id;     /* nullable */
    gchar           *room_id;      /* nullable */
    ClawtPage        page;

    /*
     * A `--open` target not yet opened: held until agent.list and
     * room.list have both answered, so it can be checked against what
     * exists (lrg-window.c).  Normalised; nullable.
     */
    gchar           *open_target;
    gboolean         open_office;     /* --office: the Office page once the fleet loads */
    guint            generation;   /* bumped on every selection change */

    /* ── the fleet, as last read ── */
    JsonNode        *agents;       /* agent.list reply, nullable */
    JsonNode        *rooms;        /* room.list reply, nullable */
    GHashTable      *unread;       /* room id -> GUINT count */
    GHashTable      *muted;        /* agent ids muted (notice.list) */
    GHashTable      *typing;       /* agent id -> set of room ids */
    guint            decisions_open;
    gboolean         held;
    gchar           *daemon_version;  /* control.status, nullable */
    gchar           *daemon_update;   /* an available update, nullable */

    /*
     * Whether the daemon has answered since this client was pointed at
     * it (clawt_daemon_link_state()'s reached_once), whether the client
     * chose it from the last-connection file rather than from a flag
     * (clawt_connection_choose()'s from_last), and what choosing had to
     * say, toasted once the window exists.  The first two decide the
     * banner's Use Local (clawt_connection_offer_local()); a switch
     * clears both, since after one the person chose.
     */
    gboolean         reached_once;
    gboolean         connection_remembered;
    gchar           *start_notice;    /* nullable, toasted once */
    ClawtLrgIslandState island;

    /* ── per-page state, owned by each page file ── */
    gpointer         page_state[16];

    /* ── the shell's widgets, for pages that must reach them ── */
    LrgWidget       *switcher;
    LrgWidget       *island_widget;
    LrgWidget       *banner;
    LrgWidget       *sidebar;

    /* every GSource a page attached, destroyed with the app */
    GPtrArray       *sources;

    /* toast dedup: one sentence once a second, like the GTK client */
    gchar           *last_toast;
    gint64           last_toast_at;
};

/* ── lrg-app.c ─────────────────────────────────────────────────────── */

ClawtLrgApp *clawt_lrg_app_new(ClawtClient     *client,
                               ClawtConnection *connection);
void         clawt_lrg_app_free(ClawtLrgApp *app);

/* Builds the theme, canvas, shell and signal wiring, with no window. */
void         clawt_lrg_app_build(ClawtLrgApp *app);

/* Runs the window until it closes; returns the process exit status. */
gint         clawt_lrg_app_run(ClawtLrgApp *app,
                               const gchar *self_test_page);

/**
 * clawt_lrg_request:
 * @app: the app
 * @kind: the frame kind, written as a literal at the call site so
 *   `make parity` can see it
 * @payload: (transfer full) (nullable): the request body
 * @flags: #ClawtLrgRequestFlags
 * @func: (nullable): called with the reply on a later frame
 * @user_data: for @func
 * @destroy: (nullable): frees @user_data after @func ran or was dropped
 *
 * Sends a request without blocking.  See the header comment.
 */
void clawt_lrg_request(ClawtLrgApp          *app,
                       const gchar          *kind,
                       JsonNode             *payload,
                       ClawtLrgRequestFlags  flags,
                       ClawtLrgReplyFunc     func,
                       gpointer              user_data,
                       GDestroyNotify        destroy);

/* Shows @text as a toast, unless it is the same sentence as a second ago. */
void clawt_lrg_toast(ClawtLrgApp *app, const gchar *text);

/*
 * How long a notice about how this window started stays up.  It is queued
 * before the first frame is drawn, and libregnum's toast clock runs from
 * then: at the default 3.5 s, "The connection you last used ... is no
 * longer saved" was on screen for about a second and a half.
 */
#define CLAWT_LRG_START_NOTICE_SECONDS (10.0)

/* clawt_lrg_toast() for @seconds rather than libregnum's default. */
void clawt_lrg_toast_for(ClawtLrgApp *app, const gchar *text,
                         gdouble seconds);

/* A toast with one action button; @on_action runs if it is pressed. */
void clawt_lrg_toast_action(ClawtLrgApp *app,
                            const gchar *text,
                            const gchar *button,
                            GCallback    on_action,
                            gpointer     user_data);

/*
 * Attaches @source to the app's context and records it, so it is
 * destroyed when the app is: a page's timer captures the app pointer,
 * and one that outlived the app fired into freed memory (ASan found it
 * in the Activity page's tick).  Takes no reference from the caller.
 */
guint clawt_lrg_app_attach(ClawtLrgApp *app, GSource *source);

/* Something changed: draw at full rate for a while. */
void clawt_lrg_app_wake(ClawtLrgApp *app);

/* Re-reads appearance.yaml (or takes @appearance) and restyles the window. */
void clawt_lrg_app_apply_appearance(ClawtLrgApp     *app,
                                    ClawtAppearance *appearance);

/* Switches daemons: reconnects the client to @connection. */
void clawt_lrg_app_switch_connection(ClawtLrgApp     *app,
                                     ClawtConnection *connection);

/* ── lrg-window.c: the shell ───────────────────────────────────────── */

LrgWidget *clawt_lrg_window_build(ClawtLrgApp *app);

/* Caps the page column at the width the window leaves it; every frame. */
void       clawt_lrg_window_resize(ClawtLrgApp *app, gfloat width,
                                   gfloat height);

void clawt_lrg_show_page(ClawtLrgApp *app, ClawtPage page);
void clawt_lrg_select_agent(ClawtLrgApp *app, const gchar *agent_id);
/* Shows or hides the sidebar: saved when wide, a drawer when narrow. */
void     clawt_lrg_window_toggle_sidebar(ClawtLrgApp *app);
/* Whether the sidebar is on screen (beside the page or over it). */
gboolean clawt_lrg_window_sidebar_shown(ClawtLrgApp *app);
void clawt_lrg_select_room(ClawtLrgApp *app, const gchar *room_id);

/* Re-reads agent.list and room.list and redraws the sidebar. */
void clawt_lrg_refresh_fleet(ClawtLrgApp *app);

/*
 * For `--open` (lrg-chat.c): the next time the Chat page settles on the
 * selected agent, it opens on its conversation with @peer instead of
 * the operator's own.
 */
void clawt_lrg_chat_open_conversation(ClawtLrgApp *app, const gchar *peer);

/*
 * For a click on the agent already selected (lrg-sidebar.c): back to its
 * conversation with the operator, and that conversation's count cleared.
 */
void clawt_lrg_chat_reopen(ClawtLrgApp *app);

/*
 * Tells the daemon the operator has read @room_id up to @upto (the
 * newest message when %NULL), so its count clears in every client
 * (lrg-chat.c).  Fire and forget: the daemon's `room.read` event is what
 * zeroes the pill, here as in every other client.
 */
void clawt_lrg_mark_room_read(ClawtLrgApp *app, const gchar *room_id,
                              const gchar *upto);

/* Every conversation read at once, as `clawtilla room read --all`. */
void clawt_lrg_mark_all_read(ClawtLrgApp *app);

/* The currently selected agent's object from the last agent.list. */
JsonObject *clawt_lrg_selected_agent(ClawtLrgApp *app);

/* An agent's object by id from the last agent.list, or NULL. */
JsonObject *clawt_lrg_find_agent(ClawtLrgApp *app, const gchar *agent_id);

/* Sets a page's tab badge; 0 hides it.  @tone as for clawt_lrg_badge(). */
void clawt_lrg_set_page_badge(ClawtLrgApp *app, ClawtPage page,
                              guint count, const gchar *tone);

/* The island and the connection banner, recomputed from app state. */
void clawt_lrg_update_island(ClawtLrgApp *app);
void clawt_lrg_update_banner(ClawtLrgApp *app);

/* Opens the Settings dialog, optionally on the page with this id. */
void clawt_lrg_open_settings(ClawtLrgApp *app, const gchar *page_id);

/* Delivered by lrg-app.c for every event, before the pages see it. */
void clawt_lrg_window_event(ClawtLrgApp *app, ClawtEvent *event);

/* Called on connect and disconnect. */
void clawt_lrg_window_connection_changed(ClawtLrgApp *app);

/* ── The page registry (each implemented in its own file) ─────────── */

const ClawtLrgPage *clawt_lrg_chat_page(void);       /* lrg-chat.c */

/*
 * For tests (lrg-chat.c): puts @text in the composer as if it had been
 * typed, with @skills -- a `skill.commands` reply, or NULL -- standing in
 * for the agent's own commands, and answers how many completion rows are
 * showing.  The daemon a test has is absent, so this is the only way to
 * put a list of realistic size in front of the composer.
 */
guint clawt_lrg_chat_debug_type(ClawtLrgApp *app, const gchar *text, JsonNode *skills);
const ClawtLrgPage *clawt_lrg_agent_page(void);      /* lrg-inspector.c */
const ClawtLrgPage *clawt_lrg_mailbox_page(void);    /* lrg-mailbox.c */
const ClawtLrgPage *clawt_lrg_computer_page(void);   /* lrg-computer.c */
const ClawtLrgPage *clawt_lrg_routines_page(void);   /* lrg-routines.c */
const ClawtLrgPage *clawt_lrg_triggers_page(void);   /* lrg-triggers.c */
const ClawtLrgPage *clawt_lrg_tasks_page(void);      /* lrg-tasks.c */
const ClawtLrgPage *clawt_lrg_decisions_page(void);  /* lrg-decisions.c */
const ClawtLrgPage *clawt_lrg_flow_page(void);       /* lrg-flow.c */
/* lrg-flow.c: opens one conversation on the Flow page -- what a
 * cross-talk summary's "View in Flow" does. */
void clawt_lrg_flow_open(ClawtLrgApp *app, const gchar *room_id);
const ClawtLrgPage *clawt_lrg_activity_page(void);   /* lrg-activity.c */
/* lrg-issues.c: the Activity page's Issues section (the sentinel's report). */
void clawt_lrg_issues_build(ClawtLrgApp *app, LrgWidget *body);
void clawt_lrg_issues_refresh(ClawtLrgApp *app);
/* lrg-rescue.c: the chat's first-reply rescue card, above the composer.
 * The box is the chat's (borrowed); refresh re-reads turn.rescue. */
LrgWidget *clawt_lrg_rescue_build(ClawtLrgApp *app);
void clawt_lrg_rescue_refresh(ClawtLrgApp *app);
/* lrg-steps.c: the Screen view's saved steps, under the screen. */
LrgWidget *clawt_lrg_steps_build(ClawtLrgApp *app);
void clawt_lrg_steps_refresh(ClawtLrgApp *app);
const ClawtLrgPage *clawt_lrg_fleet_page(void);      /* lrg-fleet-page.c */
const ClawtLrgPage *clawt_lrg_grok_page(void);       /* lrg-grok.c */
const ClawtLrgPage *clawt_lrg_office_page(void);     /* lrg-office.c */
/* The drawn office on the Office page: native with OFFICE=1, else a note. */
void clawt_lrg_office_view_add(ClawtLrgApp *app, LrgWidget *parent);
void clawt_lrg_office_view_event(ClawtLrgApp *app, ClawtEvent *event);
const ClawtLrgPage *clawt_lrg_skills_page(void);     /* lrg-skills.c */
/* Draws @skill into one library tile (a fixed-size vbox): name and
 * description each one line, cut short at the tile's edge. */
void clawt_lrg_skill_tile_fill(LrgWidget *tile, JsonObject *skill);
const ClawtLrgPage *clawt_lrg_memory_page(void);     /* lrg-recall.c */

/* Fills the Memory page's recall box with @query and searches (/recall). */
void clawt_lrg_recall_search(ClawtLrgApp *app, const gchar *query);

/* The page for @page, or NULL if this client has none (a bug). */
const ClawtLrgPage *clawt_lrg_page_for(ClawtPage page);

/* Settings pages, in drawing order, NULL-terminated (lrg-settings.c). */
const ClawtLrgSettingsPage *const *clawt_lrg_settings_pages(void);

const ClawtLrgSettingsPage *clawt_lrg_settings_images_page(void);       /* lrg-settings-images.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_teams_page(void);        /* lrg-settings-teams.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_projects_page(void);     /* lrg-settings-projects.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_history_page(void);      /* lrg-settings-history.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_pack_page(void);         /* lrg-settings-pack.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_spending_page(void);     /* lrg-settings-spending.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_orchestration_page(void); /* lrg-settings-orchestration.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_integrations_page(void); /* lrg-settings-integrations.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_connectors_page(void);   /* lrg-settings-connectors.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_folders_page(void);      /* lrg-settings-folders.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_appearance_page(void);   /* lrg-settings-appearance.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_devices_page(void);      /* lrg-settings-devices.c */
const ClawtLrgSettingsPage *clawt_lrg_settings_connections_page(void);  /* lrg-settings-connections.c */

/* The banner's Use Local: Settings → Connections' own switch, to Local. */
void clawt_lrg_settings_connections_use_local(ClawtLrgApp *app);

/* device.* events: the Devices list redraws, a code on screen learns it was used. */
void clawt_lrg_settings_devices_event(ClawtLrgApp *app, ClawtEvent *event);

/* broker.changed: the integration broker's row on Connectors redraws. */
void clawt_lrg_settings_connectors_event(ClawtLrgApp *app, ClawtEvent *event);

/* Dialogs reachable from the header menu and the sidebar (lrg-dialogs.c). */
void clawt_lrg_new_agent_dialog(ClawtLrgApp *app);
void clawt_lrg_import_agent_dialog(ClawtLrgApp *app);
void clawt_lrg_new_room_dialog(ClawtLrgApp *app);

/* The alerts drawer (lrg-alerts.c). */
LrgWidget *build_alerts_panel(ClawtLrgApp *app);
LrgWidget *clawt_lrg_alerts_bell(ClawtLrgApp *app);
void       clawt_lrg_alerts_toggle(ClawtLrgApp *app);
/* Asks the daemon for `notice.list` and redraws the drawer and the bell. */
void       clawt_lrg_alerts_refresh(ClawtLrgApp *app);
/* Mutes or unmutes an agent for every client (notice.mute / notice.unmute). */
void       clawt_lrg_set_agent_muted(ClawtLrgApp *app, const gchar *agent_id,
                                     gboolean muted);
void       clawt_lrg_alerts_reset(void);

/* Ctrl+K "Go to…" (lrg-navigation.c). */
void clawt_lrg_navigation_open(ClawtLrgApp *app);

/* Pause / resume the fleet (lrg-hold.c). */
void clawt_lrg_hold_pause(ClawtLrgApp *app);
void clawt_lrg_hold_resume(ClawtLrgApp *app);
void clawt_lrg_hold_refresh(ClawtLrgApp *app);

/* Agent lifecycle, shared by the sidebar menu and the inspector (lrg-sidebar.c). */
void clawt_lrg_agent_action(ClawtLrgApp *app, const gchar *agent_id,
                            const gchar *kind);
LrgWidget *clawt_lrg_sidebar_build(ClawtLrgApp *app);
void       clawt_lrg_sidebar_refresh(ClawtLrgApp *app);

/* ── lrg-ui.c: the widget kit every page draws with ───────────────── */

/* A label; @style_class is "title", "heading", "muted", "caption",
 * "mono" or NULL.  Text is always plain: nothing here parses markup. */
LrgWidget *clawt_lrg_label(const gchar *text, const gchar *style_class);

/* A wrapping paragraph of plain text. */
LrgWidget *clawt_lrg_paragraph(const gchar *text, const gchar *style_class);

/* One line of plain text, cut short with "…" when it does not fit: for a
 * fixed-size slot (a list row, a tile) a paragraph would wrap out of. */
LrgWidget *clawt_lrg_one_line(const gchar *text, const gchar *style_class);

/* A badge with the sentence explaining it wrapped under it, both at the
 * transcript's gutter unless @from_user: a message's mark (interrupted,
 * steered).  The sentence wraps, so it never runs off the right edge. */
LrgWidget *clawt_lrg_message_mark(const gchar *label, const gchar *tone,
                                  const gchar *caption, gboolean from_user,
                                  gfloat gutter);

/* Rendered markdown (model output): links open only http(s) and file. */
LrgWidget *clawt_lrg_markdown(ClawtLrgApp *app, const gchar *body);

/* A button; @style_class "primary", "danger", "flat" or NULL.  @on_clicked
 * receives (LrgButton *, gpointer user_data). */
LrgWidget *clawt_lrg_button(const gchar *label,
                            const gchar *style_class,
                            GCallback    on_clicked,
                            gpointer     user_data);

/* A lifted card holding @child (which may be NULL; add later). */
LrgWidget *clawt_lrg_card(LrgWidget *child);

/* A small pill: @tone is "good", "warn", "bad", "info" or "neutral". */
LrgWidget *clawt_lrg_badge(const gchar *text, const gchar *tone);

/* An agent's avatar: picture if it has one, else its generated face
 * (clawt_lrg_face_for()), else initials on its colour -- with a glow in
 * that colour when @live.  The face, when drawn, is the overlay's
 * "clawt-face" data, for clawt_lrg_face_track(). */
LrgWidget *clawt_lrg_avatar(ClawtLrgApp *app,
                            const gchar *name,
                            const gchar *agent_id,
                            gboolean     has_avatar,
                            const gchar *color,
                            gfloat       size,
                            gboolean     live);
void       clawt_lrg_avatar_invalidate(const gchar *agent_id);

/* ── Generated faces (lrg-face.c, 0.53.0) ─────────────────────────── */

/* An agent's generated face (src/agent/clawt-face.h): blinks on the
 * shared schedule and looks at the pointer while it is over the face,
 * or over an area given to clawt_lrg_face_track().  @color is checked
 * with clawt_color_ink(); anything else is grey. */
LrgWidget *clawt_lrg_face_new(const gchar *agent_id, gint shape,
                              const gchar *color, gfloat size);
/* Whether @widget is a face made by clawt_lrg_face_new(). */
gboolean   clawt_lrg_is_face(LrgWidget *widget);
void       clawt_lrg_face_set_shape(LrgWidget *face, gint shape);
gint       clawt_lrg_face_get_shape(LrgWidget *face);
void       clawt_lrg_face_set_color(LrgWidget *face, const gchar *color);
void       clawt_lrg_face_set_agent_id(LrgWidget *face, const gchar *agent_id);
/* The eyes follow the pointer while it is over @area too (a sidebar
 * row's catcher, which is on top of the face and gets the events). */
void       clawt_lrg_face_track(LrgWidget *face, LrgWidget *area);
/* What a pointer at (@world_x, @world_y) does; and leaving. */
void       clawt_lrg_face_point_at(LrgWidget *face, gfloat world_x,
                                   gfloat world_y);
void       clawt_lrg_face_look_away(LrgWidget *face);
/* The current and target look, for tests. */
void       clawt_lrg_face_get_pose(LrgWidget *face, gdouble *look_x,
                                   gdouble *look_y, gdouble *target_x,
                                   gdouble *target_y);
/* One frame of easing, as step() runs it; TRUE while still moving. */
gboolean   clawt_lrg_face_step(LrgWidget *face, gdouble dt_ms);
/* The client-wide switch from appearance.yaml's animate_faces. */
void       clawt_lrg_faces_set_animate(gboolean animate);
gboolean   clawt_lrg_faces_get_animate(void);
/* The face the daemon resolved for an agent (agent.list's `face`), so
 * every place that draws the agent agrees; NULL forgets it. */
void       clawt_lrg_face_note(const gchar *agent_id, const gchar *face);
/* A shape index, or CLAWT_FACE_INITIALS; automatic when nothing noted,
 * the disc for a shape this build does not know. */
gint       clawt_lrg_face_for(const gchar *agent_id);

/* Called when a face picker's choice changes: @face is a shape name,
 * "initials" or "" (automatic); @color is #rrggbb or "" (automatic). */
typedef void (*ClawtLrgFacePicked)(gpointer     user_data,
                                   const gchar *face,
                                   const gchar *color);

/* A face picker: a preview that blinks and watches the pointer, every
 * shape (plus automatic and initials) as a tile, and colour swatches.
 * @face and @color are the configured values, NULL or "" for automatic. */
LrgWidget   *clawt_lrg_face_picker_new(const gchar        *agent_id,
                                       const gchar        *face,
                                       const gchar        *color,
                                       ClawtLrgFacePicked  func,
                                       gpointer            user_data);
/* Whose automatic face "automatic" shows (the create dialog's id). */
void         clawt_lrg_face_picker_set_agent_id(LrgWidget   *picker,
                                                const gchar *agent_id);
/* Selects, as a click would, and calls the picked function. */
void         clawt_lrg_face_picker_select(LrgWidget   *picker,
                                          const gchar *face,
                                          const gchar *color);
/* A random shape and palette colour (clawt_face_pick_random()), as the
 * Shuffle button does and New agent does when it opens. */
void         clawt_lrg_face_picker_shuffle(LrgWidget *picker);
const gchar *clawt_lrg_face_picker_get_face(LrgWidget *picker);
const gchar *clawt_lrg_face_picker_get_color(LrgWidget *picker);
LrgWidget   *clawt_lrg_face_picker_get_preview(LrgWidget *picker);

/* A row: title and optional subtitle on the left, suffixes on the right. */
LrgWidget *clawt_lrg_row(const gchar *title, const gchar *subtitle);
/* clawt_lrg_row() for a fixed-height list: title and subtitle are one
 * line each, cut short rather than wrapped over the next row. */
LrgWidget *clawt_lrg_row_one_line(const gchar *title, const gchar *subtitle);
void       clawt_lrg_row_add_prefix(LrgWidget *row, LrgWidget *prefix);
/* Caps the title/subtitle column so the suffixes stay inside a narrow row. */
/* Sets a clawt_lrg_paragraph()'s text, escaped as the constructor does. */
void       clawt_lrg_paragraph_set_text(LrgWidget *paragraph, const gchar *text);
/* The row's wrapping subtitle, or NULL when it was made without one. */
LrgWidget *clawt_lrg_row_subtitle(LrgWidget *row);
void       clawt_lrg_row_add_suffix(LrgWidget *row, LrgWidget *suffix);

/* A page: heading, optional subtitle, a toolbar slot and a scrolling body. */
LrgWidget *clawt_lrg_page_new(const gchar *title, const gchar *subtitle);
LrgWidget *clawt_lrg_page_toolbar(LrgWidget *page);
LrgWidget *clawt_lrg_page_body(LrgWidget *page);
void       clawt_lrg_page_set_subtitle(LrgWidget *page, const gchar *subtitle);

/* Big friendly placeholder for an empty list, saying why it is empty. */
LrgWidget *clawt_lrg_empty_state(const gchar *title, const gchar *body);

/* A heading between groups of rows. */
LrgWidget *clawt_lrg_section(const gchar *title);

/* Removes every child of @container. */
void       clawt_lrg_clear(LrgWidget *container);

/* A vertical / horizontal box with spacing. */
LrgWidget *clawt_lrg_vbox(gfloat spacing);
LrgWidget *clawt_lrg_hbox(gfloat spacing);

/* Adds @child to @parent (consumes nothing: the container takes a ref). */
void       clawt_lrg_add(LrgWidget *parent, LrgWidget *child);

/* Adds and drops the caller's reference: for freshly made children. */
void       clawt_lrg_take(LrgWidget *parent, LrgWidget *child);

/* Stagger an entrance: @index is the child's position, for the delay. */
void       clawt_lrg_appear(ClawtLrgApp *app, LrgWidget *widget, guint index);

/* A form: label + control rows, with an optional hint under each. */
LrgWidget *clawt_lrg_form_new(void);
void       clawt_lrg_form_add(LrgWidget   *form,
                              const gchar *label,
                              LrgWidget   *control,
                              const gchar *hint);

/* An entry with @placeholder and @text. */
LrgWidget *clawt_lrg_entry(const gchar *text, const gchar *placeholder);
const gchar *clawt_lrg_entry_text(LrgWidget *entry);

/* A multi-line editor; @language for highlighting, or NULL. */
LrgWidget *clawt_lrg_editor(const gchar *text, const gchar *language);
gchar     *clawt_lrg_editor_text(LrgWidget *editor);

/* A combo from parallel id/label arrays (NULL-terminated), @selected id. */
LrgWidget *clawt_lrg_combo(const gchar *const *ids,
                           const gchar *const *labels,
                           const gchar        *selected);
const gchar *clawt_lrg_combo_id(LrgWidget *combo);

/* A switch. */
LrgWidget *clawt_lrg_switch(const gchar *label, gboolean active);
gboolean   clawt_lrg_switch_active(LrgWidget *toggle);

/**
 * ClawtLrgConfirmFunc:
 * @app: the app
 * @user_data: what the caller passed
 *
 * The person confirmed.
 */
typedef void (*ClawtLrgConfirmFunc)(ClawtLrgApp *app, gpointer user_data);

/* Asks before doing something; @verb labels the confirm button. */
void clawt_lrg_confirm(ClawtLrgApp         *app,
                       const gchar         *title,
                       const gchar         *body,
                       const gchar         *verb,
                       gboolean             destructive,
                       ClawtLrgConfirmFunc  func,
                       gpointer             user_data,
                       GDestroyNotify       destroy);

/**
 * ClawtLrgDialogFunc:
 * @app: the app
 * @dialog: the dialog, still presented
 * @response: the button's response id
 * @user_data: what the caller passed
 *
 * Returns: %TRUE to close the dialog, %FALSE to keep it open (a form
 *   whose input was refused stays up with its fields intact)
 */
typedef gboolean (*ClawtLrgDialogFunc)(ClawtLrgApp *app,
                                       LrgDialog   *dialog,
                                       gint         response,
                                       gpointer     user_data);

/*
 * A dialog with @content and buttons; the primary is response 1.
 *
 * Returns a new reference (transfer full): the canvas holds its own while
 * the dialog is presented, so a caller that does not keep the dialog
 * drops it at once -- g_object_unref(clawt_lrg_dialog(...)) -- and one
 * that does holds a weak pointer, so closing it frees everything.
 */
LrgDialog *clawt_lrg_dialog(ClawtLrgApp        *app,
                            const gchar        *title,
                            const gchar        *body,
                            LrgWidget          *content,
                            const gchar        *primary,
                            gboolean            destructive,
                            ClawtLrgDialogFunc  func,
                            gpointer            user_data,
                            GDestroyNotify      destroy);

/* Shows text in a read-only, copyable dialog (a secret shown once, a log). */
void clawt_lrg_show_text(ClawtLrgApp *app, const gchar *title,
                         const gchar *body, const gchar *text);

/* A context menu at the pointer, from a GMenuModel whose actions are
 * looked up on @owner's action groups. */
void clawt_lrg_popup_menu(ClawtLrgApp *app, LrgWidget *owner,
                          GMenuModel *model);

/* Opens a path in $VISUAL/$EDITOR, as the GTK client's /edit does. */
gboolean clawt_lrg_open_in_editor(ClawtLrgApp *app, const gchar *path);

/* Opens a URI with the desktop's handler (http, https, file only). */
void clawt_lrg_open_uri(ClawtLrgApp *app, const gchar *uri);

/* Copies @text to the clipboard. */
void clawt_lrg_copy(ClawtLrgApp *app, const gchar *text);

/*
 * Every piece of text on screen, in tree order: labels, buttons, entries,
 * editors, markdown blocks, dialog titles and bodies -- descending only
 * into a switcher's visible page, and into the top popup.  libregnum's
 * inspector snapshot records geometry and looks but no text, so this is
 * what `--self-test` and tests/test-lrgui-widgets.c assert on.
 */
GPtrArray *clawt_lrg_visible_texts(LrgCanvas *canvas);

/* Formats a microsecond timestamp as a relative "2m ago". */
gchar *clawt_lrg_ago(gint64 timestamp_us);

G_END_DECLS
