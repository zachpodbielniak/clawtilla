/*
 * clawt-window-private.h - What the GTK client's pages share
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * #ClawtWindow is one object with one instance struct, and every page of
 * the window reads and writes it.  That struct lived in clawt-window.c
 * while every page was built there too; each page is a file of its own
 * now, so it lives here.
 *
 * Client-internal, on purpose: it is not in PUBLIC_HEADERS, <clawtilla.h>
 * does not name it, and nothing under src/ may include it.  The library
 * must not link GTK, and this header is the line that says so.
 *
 * A function here is one that more than one of those files calls.  A
 * helper only one page uses stays static in that page's file, where the
 * next person to change it can see every caller at once.
 */

#pragma once

#include "clawt-gtk.h"

G_BEGIN_DECLS

/*
 * The provider and model rows, and the free-text row that appears when a
 * model is not in the catalogue.
 *
 * Shared by the create dialog and the agent inspector: the rules for
 * which models a provider runs belong in one place, or the two views
 * drift and one of them starts offering combinations that cannot work.
 */
typedef struct {
    ClawtWindow *window;
    GtkWidget   *provider_row;
    GtkWidget   *model_row;
    GtkWidget   *model_entry;
    JsonNode    *catalog;
} ModelChooser;

/*
 * The container image, as a list plus a way to type one that is not on
 * it.
 *
 * The catalogue is a suggestion, never a restriction -- any reference
 * podman can pull is valid -- so the last entry is always "Other" and
 * selecting it reveals a free-text row.  The daemon merges the user's
 * own images from defaults.container_images into the list before it gets
 * here, so this does not need to know where an entry came from.
 */
typedef struct {
    ClawtWindow *window;
    GtkWidget   *row;
    GtkWidget   *entry;
    JsonNode    *catalog;
} ImageChooser;

/*
 * Every view that rebuilds a list from a daemon reply needs one of these.
 *
 * clawt_window_request() iterates the main context while it waits, and the
 * client delivers events from an idle, so an event handler runs in the
 * middle of the rebuild -- and calls the same refresh again.  The inner
 * call emptied the list and refilled it, then the outer call carried on
 * appending from where it was, which showed the tail of the fleet twice.
 */
typedef enum {
    CLAWT_REFRESH_AGENTS,
    CLAWT_REFRESH_SELECTED,
    CLAWT_REFRESH_MAILBOX,
    CLAWT_REFRESH_TASKS,
    CLAWT_REFRESH_DECISIONS,
    CLAWT_REFRESH_FLOW,
    CLAWT_REFRESH_IMAGES,
    CLAWT_REFRESH_INTEGRATIONS,
    CLAWT_REFRESH_ROUTINES,
    CLAWT_REFRESH_TRIGGERS,
    CLAWT_REFRESH_CONNECTORS,
    CLAWT_REFRESH_TEAMS,
    CLAWT_REFRESH_SPENDING,
    CLAWT_REFRESH_SHARED_FOLDERS,
    CLAWT_N_REFRESH
} ClawtRefreshKind;

struct _ClawtWindow {
    AdwApplicationWindow parent_instance;

    ClawtClient       *client;

    /*
     * Which daemon, and the ones this person has saved.
     *
     * The window holds the profile alongside the client rather than
     * deriving it: a ClawtClient knows a host and a port, not the name
     * somebody gave that machine, and a header bar reading "100.72.0.41"
     * is the address a person saved a name to avoid.
     */
    GPtrArray         *connections;         /* ClawtConnection*, owned */
    ClawtConnection   *active_connection;   /* owned */
    gchar             *local_socket;        /* whatever --socket said */
    GtkWidget         *connection_button;
    GtkWidget         *connection_list;

    /*
     * What the daemon is doing that this window is not.
     *
     * Auto-reconnect is right, and silence while it happens is
     * indistinguishable from a fleet that has gone quiet: the agent list
     * is the last one received, no event arrives, and nothing says why.
     * The banner is the one place that says so -- and the same place
     * says when the daemon is a different version from this client,
     * which is otherwise found out by whichever feature happens to be
     * refused first.
     */
    GtkWidget         *connection_banner;
    gchar             *daemon_version;      /* what control.status said */

    /*
     * Whether this window has ever had a daemon on the other end.
     *
     * The two states read identically from the client -- disconnected,
     * a retry scheduled -- and they need opposite sentences.  "Lost the
     * connection" is wrong and confusing for a window that was opened
     * against a daemon which was never running: nothing was lost, and
     * the useful advice is to start it or to pick another machine,
     * neither of which "trying again" suggests.
     */
    gboolean           connected_once;

    /*
     * What the last probe found for each saved connection, by name.
     *
     * Kept rather than asked for on every redraw: a probe is a network
     * round trip and the menu is rebuilt whenever the editor saves. An
     * entry nobody has probed yet is simply absent, which the row draws
     * as "not checked" -- a client asserting "unreachable" about
     * something it has not asked is worse than saying nothing.
     */
    GHashTable        *connection_status;   /* name -> ClawtConnectionStatus* */

    /*
     * Two toast overlays, because one cannot mean two places.
     *
     * `toasts` wraps the chat page's transcript region, so a toast lands
     * above the composer rather than over the text being typed.
     * `page_toasts` wraps the stack for every other page, where the
     * bottom of the page is the right place and there is nothing to
     * cover.  clawt_window_toast() picks between them.
     */
    /*
     * The two clamps that carry the chat measure.
     *
     * Held because AdwClamp takes a *property*, not a stylesheet: a
     * measure changed in the settings would otherwise reach the file
     * and the CSS and not the widget, so the control would appear to do
     * nothing until the page was rebuilt.  That is the same "saved but
     * nothing rewrote what it produces" gap agent.set had.
     */
    GtkWidget         *transcript_clamp;
    GtkWidget         *composer_clamp;
    GtkWidget         *flow_clamp;

    /*
     * The decisions page reads its column from the same resolver, and
     * so needs the same pair: the clamp to size, and the scroller whose
     * viewport it resolves against.
     */
    GtkWidget         *decision_clamp;
    GtkScrolledWindow *decision_scroll;

    /*
     * The two scrolled windows the measure is a share of.
     *
     * A share is only a number once there is something to take it of,
     * so the column has to be recomputed whenever the page is
     * re-allocated -- and GtkWidget offers no way to hear about that.
     * `notify::width` is the obvious hook and is a trap: measured
     * against real GTK4 across four hand-driven allocations it fired
     * **zero** times, while the scrolled window's own horizontal
     * adjustment fired four for four and reported the allocated width
     * exactly.  Following the adjustment is the same answer the
     * vertical case here already needed, for the same reason.
     *
     * The composer takes the *transcript's* viewport width rather than
     * its own container's, because the two must be one column: sharing
     * a number is what keeps the box you type into standing on the
     * words above it.
     */
    GtkWidget         *chat_scroll;

    AdwToastOverlay   *toasts;
    AdwToastOverlay   *page_toasts;
    AdwOverlaySplitView *split;

    /*
     * Whether each split's sidebar is meant to be showing.
     *
     * AdwOverlaySplitView writes show-sidebar itself when it stops being
     * collapsed, so the widget can only be asked what the toolkit last
     * did -- never what the operator chose.  These remember the choice
     * and the notify::collapsed handlers put it back.
     *
     * `sidebar_transient` covers the one write the client makes for
     * reasons of its own, which is not a choice and must not be recorded
     * as one.
     */
    gboolean           sidebar_open;
    gboolean           alerts_open;
    gboolean           sidebar_transient;
    GtkListBox        *sidebar;
    AdwViewStack      *pages;

    /*
     * Settings, while it is open.
     *
     * The progress bars are held by name so a download event can move the
     * right one without rebuilding the list: a 500 MB image emits a
     * hundred of them, and rebuilding on each would fight whatever the
     * person is doing in that window.
     */
    AdwDialog         *settings;
    /*
     * Appearance, held for the life of the window rather than the life
     * of the settings dialog: the dialog is a live preview, so its
     * controls edit this and every edit is applied and saved at once.
     */
    ClawtAppearance   *appearance;
    GtkWidget         *settings_images;
    GtkWidget         *settings_integrations;
    GtkWidget         *settings_teams;
    GtkWidget         *settings_folders;
    GtkWidget         *settings_spending;
    GtkWidget         *settings_spending_period;
    gint64             settings_spending_since;
    GtkWidget         *settings_connectors;
    GtkListBox        *routine_list;
    GtkListBox        *trigger_list;
    GtkListBox        *delivery_list;
    GtkWidget         *settings_catalog_row;
    GtkWidget         *settings_url_row;
    JsonNode          *settings_catalog;
    GHashTable        *settings_bars;

    /* Chat */
    GtkBox            *transcript;
    GtkScrolledWindow *transcript_scroll;

    /*
     * The two halves of "something arrived while you were reading".
     *
     * They are driven from one place -- set_following() -- because a
     * marker with no pill, or a pill with no marker, is worse than
     * neither: each would be telling the operator something the other
     * contradicts.
     */
    GtkRevealer       *jump_revealer;
    GtkWidget         *unread_marker;    /* borrowed; owned by transcript */

    GtkTextView       *entry;
    GtkWidget         *placeholder;
    GtkLabel          *streaming;

    /* Inspector */
    GtkBox            *inspector;
    GtkWidget         *name_row;
    GtkWidget         *description_row;
    GtkWidget         *effort_row;
    GtkWidget         *computer_row;
    GtkWidget         *restart_row;
    GtkWidget         *autostart_row;
    GtkWidget         *chief_row;
    GtkWidget         *manage_fleet_row;
    GtkWidget         *team_row;
    GtkWidget         *team_role_row;
    GStrv              team_ids;
    gboolean           settings_need_restart;

    /*
     * The rows built straight from the schema, and the key each one
     * sets. Everything else in the inspector is hand-built, because it
     * has copy worth writing by hand; these are the options that had no
     * UI at all until the schema could say what an agent calls them.
     */
    GPtrArray         *schema_rows;         /* SchemaRow*, owned */
    GHashTable        *collapsed_teams;   /* team id -> GINT_TO_POINTER(1) */
    ModelChooser       inspector_models;
    ImageChooser       inspector_image;
    gchar             *inspector_computer;   /* the selected agent's type */
    ImageChooser       inspector_disk;
    GtkWidget         *vm_cpus_row;
    GtkWidget         *vm_memory_row;
    GtkWidget         *vm_disk_row;
    GtkWidget         *vm_resolution_row;
    GtkWidget         *vm_ssh_host_row;
    GtkWidget         *vm_desktop_row;
    GtkWidget         *vm_desktop_input_row;
    GtkWidget         *mount_source_row;
    GtkWidget         *mount_target_row;
    GtkWidget         *mount_mode_row;

    /* Mailbox */
    GtkListBox        *mailbox_list;
    GtkLabel          *mailbox_summary;

    /* Computer */
    GtkEntry          *exec_entry;
    GtkTextView       *exec_output;
    GtkLabel          *computer_state;

    /* Tasks */
    GtkListBox        *task_list;

    /*
     * Choices agents are waiting on somebody for.
     *
     * Its own page rather than a section of the alerts panel: an alert
     * is something that happened and a decision is something that needs
     * you, and one surface meaning both is one badge nobody can act on.
     */
    GtkListBox        *decision_list;
    AdwViewStackPage  *decision_page;

    GtkWidget         *activity_bar;

    /*
     * Stop, beside Send: kills the AI CLI carrying out the current turn
     * and everything it spawned, and leaves the agent up.  Shown only
     * while there is a turn to stop -- a control that is always there
     * and usually does nothing teaches people it does nothing.
     */
    GtkWidget         *stop_turn;

    /*
     * Whether the selected agent declares `interrupt`, read from its own
     * row in the last listing.  Kept rather than re-derived, because the
     * other place that needs it -- a typing frame -- carries no caps,
     * and working them out there from the runtime type would be a second
     * answer to a question the daemon already answers.
     */
    gboolean           selected_can_interrupt;
    GtkSpinner        *activity_spinner;

    GtkWidget         *agent_menu;
    GSimpleActionGroup *agent_actions;

    /*
     * The Team submenu, and the fleet's teams as the sidebar last saw
     * them.
     *
     * Rebuilt on every right-click rather than asked for then: a menu
     * has to appear at once, and the sidebar was drawn from a team.list
     * a moment ago -- the same reason the lifecycle entries decide what
     * to grey out from the row rather than from the daemon.
     */
    GMenu             *agent_menu_teams;

    /*
     * Filled per right-click, like the Team submenu: an agent with no
     * machine gets an empty section, which draws as nothing at all.
     */
    GMenu             *agent_menu_computer;
    gboolean           menu_stop_removes;
    JsonNode          *teams_seen;

    gboolean           refreshing[CLAWT_N_REFRESH];
    gboolean           refresh_again[CLAWT_N_REFRESH];

    gchar             *selected_agent;

    /*
     * Which room the transcript on screen actually is.
     *
     * Not derivable from selected_agent: a chat with an agent is the
     * direct room between the user and it, and the daemon owns how that
     * is named. Matching an incoming message on the sender instead is
     * what put an agent's reply to one of its peers in the user's chat.
     */
    gchar             *selected_room;

    /*
     * Which of the selected agent's conversations is on screen: NULL for
     * the operator's own, or a peer's id for one between two agents that
     * the operator can read but is not in.
     */
    gchar             *selected_conversation;
    GtkWidget         *conversation_bar;
    GtkWidget         *conversation_button;
    GMenu             *conversation_menu;

    /*
     * What arrived while you were looking somewhere else.
     *
     * Keyed by agent id rather than by room, because the client only
     * learns a room id for an agent it has already opened -- and the
     * agent this exists for is precisely the one nobody has opened.
     * `dm_rooms` maps the other way, from the room an event names to the
     * agent whose conversation it is, and is rebuilt from every fleet
     * listing: the daemon reports each agent's `dm_room` so no client
     * has to take "dm:a:b" apart.
     *
     * Session-scoped on purpose.  A count that survived a restart would
     * need a read position per agent that outlives the process, which is
     * a protocol question rather than a display one.  Everything here is
     * driven by one integer per agent, so that answer can change later
     * without any of this changing.
     */
    GHashTable        *unread;
    GHashTable        *dm_rooms;
    AdwViewStackPage  *chat_page;

    /*
     * When this window last connected, in microseconds.
     *
     * A client subscribes from cursor 0 and the daemon replays its
     * recent events, so the first thing a fresh window receives is
     * everything that has just happened -- messages the operator may
     * well have read in the last session.  Counting those would make a
     * window open already showing a number for a conversation nobody
     * has touched, and would make the count depend on whether the
     * replay happened to arrive before or after the first fleet
     * listing.  Replayed events keep their original timestamps, so this
     * is the whole of the test.
     */
    gint64             connected_at;

    /*
     * Where the current run of messages is up to.
     *
     * A run is consecutive messages from one sender, and it gets one
     * header rather than one per message: the header is what does most
     * of the work of making a conversation read as a conversation, and
     * a column of identical faces is noise rather than identity.  A run
     * ends at a different sender or at a day boundary -- not at a time
     * gap, which would be a fourth constant with nothing behind it and
     * would fire constantly on traffic that arrives in bursts.
     */
    gchar             *run_sender;
    gchar             *run_day;

    /*
     * The Flow view's own run state.  It draws through the same row
     * builder as the chat -- two builders for one kind of content is how
     * the two drifted into different renderings of the same messages --
     * but it is a different transcript, so it keeps its own place in the
     * run rather than sharing the chat's.
     */
    gchar             *flow_run_sender;
    gchar             *flow_run_day;

    /* How the selected agent's avatar is drawn, from its own config. */
    gchar             *selected_avatar;
    gchar             *selected_color;

    /*
     * What has happened, for the panel on the right.
     *
     * Toasts answer something the operator just did -- "Saved.", "That
     * is not a port." -- and there are 89 of those.  Exactly two call
     * sites were notifications: a download that failed, and a message
     * the loop guard refused.  Those two arrive on their own, disappear
     * after a few seconds, and leave no trace anywhere if nobody was
     * looking, which is what this is for.
     *
     * The routine stream goes in as well, quietly, so the panel can also
     * answer "what is the fleet doing" -- the client already receives
     * every event and acted on five kinds, dropping the rest.
     *
     * Session-scoped and capped: the daemon's own event log is the
     * durable copy, and `event.list` reads it for anything older.
     */
    GPtrArray            *alerts;
    AdwOverlaySplitView  *alerts_split;
    GtkListBox           *alerts_list;
    GtkWidget            *alerts_badge;
    GtkWidget            *alerts_filter;
    gboolean              alerts_show_all;
    gchar                *alerts_agent;

    /* The flow page: who has been talking to whom, and about what. */
    GtkListBox        *flow_list;
    GtkBox            *flow_transcript;
    GtkScrolledWindow *flow_scroll;
    GtkWidget         *flow_stack;
    GtkWidget         *flow_title;
    GtkWidget         *flow_subtitle;
    GtkWidget         *flow_include_user;
    gchar             *flow_room;

    /*
     * Which messages the transcript already has, by id.
     *
     * The daemon replays recent events to a client that has just
     * connected, so the ones already in the loaded history arrive a
     * second time as events. Without this every message sent before the
     * window opened was drawn twice.
     */
    GHashTable        *shown;

    /*
     * What was typed but not sent, per agent.
     *
     * Clicking another agent to check something and coming back to find
     * your half-written message gone is the sort of small loss that
     * makes a client feel careless.
     */
    GHashTable        *drafts;

    /*
     * Files and images queued to go with the next message, and the
     * strip that shows them. Held client-side until send: an attachment
     * on a message that is never sent should not leave anything in the
     * agent's exchange directory.
     */
    GPtrArray         *pending;      /* Attachment* */
    GtkWidget         *attachments;  /* the strip above the entry */
    /*
     * The command list is a revealer above the entry, not a popover.
     *
     * A popover has to be parented to a widget, which makes it that
     * widget's child; parented to the GtkEntry it belongs to, the
     * window stopped mapping entirely -- it ran, its main loop turned,
     * and nothing ever appeared. A revealer is an ordinary child of the
     * page's box and cannot do that.
     */
    GtkWidget         *command_revealer;
    GtkListBox        *command_list;
    gboolean           following;
};

/*
 * Where a message is being drawn, and where that view is up to.
 *
 * There were two row builders -- the chat's and the Flow tab's -- and
 * they had drifted into visibly different renderings of the same
 * content: one with runs, avatars, day dividers and a measure, the other
 * a flat list of captions.  A reader moving between them saw two
 * conventions for one kind of thing.
 *
 * Fixed by deleting one of them rather than by teaching the second the
 * same tricks, because that is the only version that cannot drift again.
 * What differs between the two views is exactly this struct: which box,
 * whose run state, and how the other party is drawn.
 */
typedef struct {
    GtkBox       *into;
    gchar       **run_sender;   /* the view's own place in the run */
    gchar       **run_day;
    const gchar  *avatar;       /* NULL derives one from the name */
    const gchar  *color;
} TranscriptView;

/*
 * One file queued to go with the next message.
 *
 * The bytes are held here rather than written straight into the agent's
 * exchange directory, because a message that is composed and then
 * abandoned should not leave anything behind for the agent to find.
 */
typedef struct {
    gchar  *name;
    GBytes *bytes;
} Attachment;

/* ── Defined in clawt-window.c: the window itself ────────────────── */

const gchar *
clawt_gtk_answer_of(GtkWidget *row);

GtkWidget *
clawt_gtk_badge(const gchar *text, const gchar *css_class, const gchar *tooltip);

gchar *
clawt_gtk_chooser_model(ModelChooser *chooser);

const gchar *
clawt_gtk_chooser_provider_id(ModelChooser *chooser);

void
clawt_gtk_clear_box(GtkBox *box);

void
clawt_gtk_clear_list(GtkListBox *list);

void
clawt_gtk_disk_chooser_build(ImageChooser *chooser, ClawtWindow *window,
                             GtkWidget *group, const gchar *want);

gchar *
clawt_gtk_disk_chooser_value(ImageChooser *chooser);

void
clawt_gtk_image_chooser_build(ImageChooser *chooser, ClawtWindow *window,
                              GtkWidget *group, const gchar *want);

gchar *
clawt_gtk_image_chooser_value(ImageChooser *chooser);

void
clawt_gtk_model_chooser_build(ModelChooser *chooser, ClawtWindow *window,
                              GtkWidget *group, const gchar *want_provider,
                              const gchar *want_model);

void
clawt_gtk_on_new_agent(GtkButton *button, gpointer user_data);

gboolean
clawt_gtk_refresh_enter(ClawtWindow *self, ClawtRefreshKind kind);

gboolean
clawt_gtk_refresh_repeat(ClawtWindow *self, ClawtRefreshKind kind);

void
clawt_gtk_refresh_selected(ClawtWindow *self);

GtkWidget *
clawt_gtk_row_opens_something(GtkWidget *row);

void
clawt_gtk_select_agent(ClawtWindow *self, const gchar *agent_id);

void
clawt_gtk_set_row_text(GtkWidget *row, const gchar *title, const gchar *subtitle);

/* ── Defined in gtk-chat.c: the chat page ────────────────────────── */

gboolean
clawt_gtk_already_shown(ClawtWindow *self, const gchar *id);

void
clawt_gtk_append_message(ClawtWindow *self, const gchar *sender, const gchar *body,
                         gboolean from_user, gint64 ts);

void
clawt_gtk_append_message_to(ClawtWindow *self, const TranscriptView *view,
                            const gchar *sender, const gchar *body, gboolean from_user,
                            gint64 ts, const gchar *task, gint64 depth);

void
clawt_gtk_attachment_free(Attachment *attachment);

GtkWidget *
clawt_gtk_build_chat_page(ClawtWindow *self);

gint
clawt_gtk_chat_measure(ClawtWindow *self);

void
clawt_gtk_entry_set_text(ClawtWindow *self, const gchar *text);

gchar *
clawt_gtk_entry_text(ClawtWindow *self);

void
clawt_gtk_fill_conversation_menu(ClawtWindow *self);

void
clawt_gtk_follow_viewport_width(ClawtWindow *self, GtkWidget *scroll);

void
clawt_gtk_load_history(ClawtWindow *self);

void
clawt_gtk_note_arrival(ClawtWindow *self);

void
clawt_gtk_on_conversation_chosen(GSimpleAction *action, GVariant *parameter,
                                 gpointer user_data);

void
clawt_gtk_push_chat_measure(ClawtWindow *self);

void
clawt_gtk_queue_scroll(ClawtWindow *self);

void
clawt_gtk_reset_transcript(ClawtWindow *self);

void
clawt_gtk_set_activity(ClawtWindow *self, const gchar *text);

void
clawt_gtk_set_following(ClawtWindow *self, gboolean following);

void
clawt_gtk_sync_stop_turn(ClawtWindow *self, gboolean busy);

/* ── Defined in gtk-computer.c: the computer page ────────────────── */

GtkWidget *
clawt_gtk_build_computer_page(ClawtWindow *self);

void
clawt_gtk_refresh_computer(ClawtWindow *self, JsonObject *agent);

/* ── Defined in gtk-decisions.c: the decisions page ──────────────── */

GtkWidget *
clawt_gtk_build_decision_page(ClawtWindow *self);

void
clawt_gtk_refresh_decisions(ClawtWindow *self);

/* ── Defined in gtk-flow.c: the flow page ────────────────────────── */

GtkWidget *
clawt_gtk_build_flow_page(ClawtWindow *self);

void
clawt_gtk_on_flow_task_clicked(GtkButton *button, gpointer user_data);

void
clawt_gtk_refresh_flow(ClawtWindow *self);

/* ── Defined in gtk-inspector.c: the inspector page ──────────────── */

void
clawt_gtk_agent_action(ClawtWindow *self, const gchar *kind);

gboolean
clawt_gtk_apply_setting(ClawtWindow *self, const gchar *key, const gchar *value);

void
clawt_gtk_build_inspector(ClawtWindow *self, JsonObject *agent, JsonObject *payload);

GtkWidget *
clawt_gtk_build_inspector_page(ClawtWindow *self);

ClawtComputerType
clawt_gtk_computer_type_from_nick(const gchar *nick);

const gchar *
clawt_gtk_computer_type_nick_at(guint selected);

const gchar *const *
clawt_gtk_computer_type_nicks(void);

void
clawt_gtk_delete_agent(ClawtWindow *self);

const gchar *
clawt_gtk_editor_command(void);

void
clawt_gtk_open_path_in_editor(ClawtWindow *self, const gchar *path, const gchar *name);

void
clawt_gtk_schema_row_free(gpointer data);

/* ── Defined in gtk-mailbox.c: the mailbox page ──────────────────── */

GtkWidget *
clawt_gtk_build_mailbox_page(ClawtWindow *self);

void
clawt_gtk_refresh_mailbox(ClawtWindow *self);

/* ── Defined in gtk-prefs-appearance.c: appearance ───────────────── */

void
clawt_gtk_apply_appearance(ClawtAppearance *appearance);

GtkWidget *
clawt_gtk_build_appearance_page(ClawtWindow *self);

void
clawt_gtk_set_label_markdown(GtkLabel *label, const gchar *body);

/* ── Defined in gtk-prefs-connectors.c: the connectors page ──────── */

GtkWidget *
clawt_gtk_build_connectors_page(ClawtWindow *self);

void
clawt_gtk_refresh_settings_connectors(ClawtWindow *self);

/* ── Defined in gtk-prefs-folders.c: the shared folders page ─────── */

void
clawt_gtk_append_warning_rows(GtkListBox *list, JsonNode *reply);

GtkWidget *
clawt_gtk_build_shared_folders_page(ClawtWindow *self);

void
clawt_gtk_refresh_settings_folders(ClawtWindow *self);

/* ── Defined in gtk-prefs-integrations.c: the integrations page ──── */

GtkWidget *
clawt_gtk_add_entry(GtkWidget *group, const gchar *title, const gchar *value);

GtkWidget *
clawt_gtk_build_integrations_page(ClawtWindow *self);

JsonObject *
clawt_gtk_find_integration(JsonNode *reply, const gchar *name);

void
clawt_gtk_refresh_settings_integrations(ClawtWindow *self);

/* ── Defined in gtk-prefs-spending.c: the spending page ──────────── */

GtkWidget *
clawt_gtk_build_spending_page(ClawtWindow *self);

void
clawt_gtk_refresh_settings_spending(ClawtWindow *self);

/* ── Defined in gtk-prefs-teams.c: the teams page ────────────────── */

GtkWidget *
clawt_gtk_build_teams_page(ClawtWindow *self);

void
clawt_gtk_refresh_settings_teams(ClawtWindow *self);

/* ── Defined in gtk-routines.c: the routines page ────────────────── */

GtkWidget *
clawt_gtk_build_routine_page(ClawtWindow *self);

void
clawt_gtk_refresh_routines(ClawtWindow *self);

/* ── Defined in gtk-triggers.c: the triggers page ────────────────── */

GtkWidget *
clawt_gtk_build_trigger_page(ClawtWindow *self);

void
clawt_gtk_refresh_triggers(ClawtWindow *self);

/* ── Defined in gtk-sidebar.c: the sidebar ───────────────────────── */

const gchar *
clawt_gtk_agent_row_data(ClawtWindow *self, const gchar *agent_id, const gchar *key);

void
clawt_gtk_build_agent_menu(ClawtWindow *self);

void
clawt_gtk_note_unread(ClawtWindow *self, ClawtEvent *event, const gchar *from);

void
clawt_gtk_on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);

void
clawt_gtk_refresh_agents(ClawtWindow *self);

GtkStringList *
clawt_gtk_team_choices(ClawtWindow *self, const gchar *current, GStrv *out_ids);

guint
clawt_gtk_team_index_of(GStrv ids, const gchar *current);

void
clawt_gtk_update_unread_tab(ClawtWindow *self);

/* ── Defined in gtk-tasks.c: the tasks page ──────────────────────── */

GtkWidget *
clawt_gtk_build_task_page(ClawtWindow *self);

void
clawt_gtk_refresh_tasks(ClawtWindow *self);

G_END_DECLS
