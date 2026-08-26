/*
 * clawt-window.c - The clawtilla window
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Agents are presented as chats, because that is what they are: a sidebar
 * of who is here, a transcript in the middle, and everything else about
 * an agent behind a tab rather than a settings dialog somewhere else.
 */

#include "clawt-gtk.h"

#include <glib/gstdio.h>
#include <unistd.h>

#include <string.h>

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

/* Defined below; the inspector and the create dialog both use them. */
static JsonObject  *chooser_provider(ModelChooser *chooser);
static const gchar *chooser_provider_id(ModelChooser *chooser);
static gchar       *chooser_model(ModelChooser *chooser);
static void         refresh_flow(ClawtWindow *self);
static void         refresh_settings_images(ClawtWindow *self);
static gchar *      human_size(gint64 bytes);
static void         disk_chooser_build(ImageChooser *chooser,
                                       ClawtWindow  *window,
                                       GtkWidget    *group,
                                       const gchar  *want);
static gchar *      disk_chooser_value(ImageChooser *chooser);
static const gchar *editor_command(void);
static void         on_send(GtkWidget *widget, gpointer user_data);
static void         load_history(ClawtWindow *self);
static gboolean     apply_schema_rows(ClawtWindow *self);
static void         on_new_agent(GtkButton *button,
                                 gpointer   user_data);
static void         on_import_agent(GtkButton *button,
                                    gpointer   user_data);
static void         open_path_in_editor(ClawtWindow *self,
                                        const gchar *path,
                                        const gchar *name);
static void         model_chooser_build(ModelChooser *chooser,
                                        ClawtWindow  *window,
                                        GtkWidget    *group,
                                        const gchar  *want_provider,
                                        const gchar  *want_model);
static void         model_chooser_build_full(ModelChooser *chooser,
                                             ClawtWindow  *window,
                                             GtkWidget    *group,
                                             const gchar  *want_provider,
                                             const gchar  *want_model,
                                             const gchar  *require);
static void         image_chooser_build(ImageChooser *chooser,
                                        ClawtWindow  *window,
                                        GtkWidget    *group,
                                        const gchar  *want);
static gchar *      image_chooser_value(ImageChooser *chooser);
static const gchar *answer_of(GtkWidget *row);
static void         on_image_changed(GObject    *object,
                                     GParamSpec *pspec,
                                     gpointer    user_data);

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
    CLAWT_REFRESH_FLOW,
    CLAWT_REFRESH_IMAGES,
    CLAWT_REFRESH_INTEGRATIONS,
    CLAWT_REFRESH_ROUTINES,
    CLAWT_REFRESH_CONNECTORS,
    CLAWT_REFRESH_TEAMS,
    CLAWT_REFRESH_SPENDING,
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

    AdwToastOverlay   *toasts;
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
    GtkWidget         *settings_spending;
    GtkWidget         *settings_spending_period;
    gint64             settings_spending_since;
    GtkWidget         *settings_connectors;
    GtkListBox        *routine_list;
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

    GtkWidget         *activity_bar;
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

G_DEFINE_FINAL_TYPE(ClawtWindow, clawt_window, ADW_TYPE_APPLICATION_WINDOW)

/*
 * Returns %TRUE when the caller owns the rebuild.  A nested call notes
 * that the data changed under it and returns; the owner runs the body
 * again on the way out, so the last word still belongs to the daemon.
 */
static gboolean
refresh_enter(ClawtWindow *self, ClawtRefreshKind kind)
{
    if (self->refreshing[kind]) {
        self->refresh_again[kind] = TRUE;
        return FALSE;
    }

    self->refreshing[kind] = TRUE;
    return TRUE;
}

/* Returns %TRUE when the body must run once more. */
static gboolean
refresh_repeat(ClawtWindow *self, ClawtRefreshKind kind)
{
    if (!self->refresh_again[kind]) {
        self->refreshing[kind] = FALSE;
        return FALSE;
    }

    self->refresh_again[kind] = FALSE;
    return TRUE;
}

static void refresh_agents(ClawtWindow *self);
static void update_unread_tab(ClawtWindow *self);
static void on_flow_task_clicked(GtkButton *button, gpointer user_data);
static void refresh_selected(ClawtWindow *self);
static JsonObject *find_integration(JsonNode *reply, const gchar *name);
static void refresh_routines(ClawtWindow *self);
static void select_agent(ClawtWindow *self, const gchar *agent_id);

/* ── Talking to the daemon ───────────────────────────────────────── */

void
clawt_window_toast(ClawtWindow *self, const gchar *text)
{
    g_return_if_fail(CLAWT_IS_WINDOW(self));

    adw_toast_overlay_add_toast(self->toasts, adw_toast_new(text));
}

JsonNode *
clawt_window_request(ClawtWindow *self, const gchar *kind, JsonNode *payload)
{
    g_autoptr(GError) error = NULL;
    JsonNode *reply;

    g_return_val_if_fail(CLAWT_IS_WINDOW(self), NULL);

    reply = clawt_client_request(self->client, kind, payload, &error);

    if (reply == NULL) {
        /*
         * Every failure is surfaced.  A request that quietly did nothing
         * is the worst outcome here: the interface would look like it had
         * worked and the fleet would disagree.
         */
        clawt_window_toast(self, error->message);
        return NULL;
    }

    /*
     * A reply can succeed and still leave an agent behind.  Seven daemon
     * handlers re-render the fleet's agent files, and clawtilla refuses to
     * render a `libreclaw:` block that redeclares a section it renders
     * itself -- so a perfectly ordinary edit saves to clawtilla.yaml,
     * reports success, and reaches nothing the agent reads.
     *
     * Checked here rather than at each call site, for the reason the
     * failure toast is: a handler that grows the array later is covered
     * without anybody remembering to look, and one that never carries it
     * costs a member lookup.
     */
    {
        g_autofree gchar *refused =
            clawt_ipc_reply_refusal_text(reply, NULL);

        if (refused != NULL)
            clawt_window_toast(self, refused);
    }

    return reply;
}

/* ── Sidebar ─────────────────────────────────────────────────────── */

/*
 * A coloured dot for the agent's state.
 *
 * Colour alone is never the only signal -- the state is also in the
 * tooltip and in the inspector -- because a dot is exactly the kind of
 * cue that disappears for a colourblind reader.
 */
static GtkWidget *
state_dot(const gchar *state)
{
    GtkWidget *dot = gtk_image_new();
    const gchar *icon = "media-record-symbolic";
    const gchar *css = NULL;

    if (g_strcmp0(state, "running") == 0)
        css = "success";
    else if (g_strcmp0(state, "error") == 0 || g_strcmp0(state, "shadow") == 0)
        css = "error";
    else if (g_strcmp0(state, "degraded") == 0 ||
             g_strcmp0(state, "starting") == 0 ||
             g_strcmp0(state, "stopping") == 0)
        css = "warning";
    else
        css = "dim-label";

    gtk_image_set_from_icon_name(GTK_IMAGE(dot), icon);
    gtk_widget_add_css_class(dot, css);
    gtk_widget_set_tooltip_text(dot, state);

    return dot;
}

/*
 * Sets a row's title and subtitle as plain text.
 *
 * AdwPreferencesRow:use-markup defaults to TRUE, so a row's title and
 * subtitle are parsed as Pango markup unless you say otherwise.  Every
 * string here came from a person or a model -- an agent's description, a
 * message body, a task prompt -- and two things follow from that.  Text
 * containing a bare "<" or "&" silently fails to render at all, which is
 * ordinary English rather than an edge case; and text containing real
 * markup renders as live styled content, including clickable links.
 * Neither is acceptable for something an agent wrote.
 */
static void
set_row_text(GtkWidget *row, const gchar *title, const gchar *subtitle)
{
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  title != NULL ? title : "");

    if (subtitle != NULL)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
}

static GtkWidget *
badge(const gchar *text, const gchar *css_class, const gchar *tooltip)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(label, "caption");
    gtk_widget_add_css_class(label, css_class);
    gtk_widget_set_tooltip_text(label, tooltip);

    return label;
}

/* ── Unread ──────────────────────────────────────────────────────── */

static guint
unread_for(ClawtWindow *self, const gchar *agent_id)
{
    if (agent_id == NULL || self->unread == NULL)
        return 0;

    return GPOINTER_TO_UINT(g_hash_table_lookup(self->unread, agent_id));
}

/*
 * The total on the Chat tab of the page switcher.
 *
 * libadwaita draws this itself and draws it *accent-coloured* only when
 * `needs-attention` is set beside `badge-number` -- its own rule reads
 * the accent token, so a palette recolours it and this client writes no
 * CSS for it at all.
 *
 * Not shown while you are already on the chat page with the sidebar
 * open: the sidebar beside it is saying exactly *which* agent, which is
 * strictly more information, and a badge on the page you are looking at
 * is noise.  It stays when the sidebar is collapsed to a drawer, because
 * then there is nothing else saying so.
 */
static void
update_unread_tab(ClawtWindow *self)
{
    GHashTableIter iter;
    gpointer value;
    guint total = 0;
    gboolean hide;

    if (self->chat_page == NULL || self->unread == NULL)
        return;

    g_hash_table_iter_init(&iter, self->unread);

    while (g_hash_table_iter_next(&iter, NULL, &value))
        total += GPOINTER_TO_UINT(value);

    hide = g_strcmp0(adw_view_stack_get_visible_child_name(self->pages),
                     "chat") == 0 &&
           self->split != NULL &&
           !adw_overlay_split_view_get_collapsed(self->split);

    adw_view_stack_page_set_badge_number(self->chat_page,
                                         hide ? 0 : total);
    adw_view_stack_page_set_needs_attention(self->chat_page,
                                            !hide && total > 0);
}

/*
 * Counts a message that arrived somewhere you are not looking.
 *
 * The rule is the room, not the sender: a room that is on screen never
 * accrues unread whatever the scroll position -- that case belongs to
 * the transcript's own "New messages" pill, which deliberately carries
 * no count -- and a room that is not on screen accrues one.  Only rooms
 * that are somebody's conversation with the operator count, so the
 * fleet's agent-to-agent traffic never does.
 */
static void
note_unread(ClawtWindow *self, ClawtEvent *event, const gchar *from)
{
    const gchar *room_id = clawt_event_get_subject(event);
    const gchar *agent_id;
    guint count;

    /*
     * The rule is clawt_unread_should_count(), in libclawt, because the
     * web client applies the same one -- and two implementations of it
     * would differ exactly once, on the case nobody looked at.
     */
    if (!clawt_unread_should_count(room_id, self->selected_room, from,
                                   clawt_event_get_timestamp(event),
                                   self->connected_at))
        return;

    agent_id = g_hash_table_lookup(self->dm_rooms, room_id);

    if (agent_id == NULL)
        return;

    count = unread_for(self, agent_id) + 1;
    g_hash_table_insert(self->unread, g_strdup(agent_id),
                        GUINT_TO_POINTER(count));
}

/*
 * The count of messages waiting for *you*, as a filled pill.
 *
 * Filled is the whole signal.  Everything else in that row -- HOST,
 * CHIEF, the queue number that used to be here -- is a coloured caption,
 * so a filled pill is unambiguous against all of them without anything
 * moving: filled means for you, text means about the agent.
 *
 * No 99+ cap.  The usual reason is width and it is measurably false
 * here: at the client's caption size "99+" is wider than the three-digit
 * number it would replace.
 */
static GtkWidget *
unread_badge(guint count)
{
    g_autofree gchar *text = g_strdup_printf("%u", count);
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(label, "caption");
    gtk_widget_add_css_class(label, "clawt-unread-badge");
    gtk_widget_set_tooltip_text(
        label, count == 1 ? "1 message you have not read"
                          : "messages you have not read");
    gtk_widget_set_valign(label, GTK_ALIGN_CENTER);

    return label;
}

static GtkWidget *
agent_row(JsonObject *agent, guint unread)
{
    GtkWidget *row = adw_action_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const gchar *state = clawt_json_string(agent, "state", "stopped");
    const gchar *caps = clawt_json_string(agent, "caps", "");
    gint64 depth = json_object_has_member(agent, "mailbox_depth")
                   ? json_object_get_int_member(agent, "mailbox_depth") : 0;

    /*
     * What it is doing, in place of its description while it is doing
     * it.
     *
     * A list of agents that all say "running" answers the wrong
     * question. The one worth answering is whether anything is
     * happening -- and, when it is, who it is happening for: an agent
     * busy for three minutes on a peer's question looks exactly like
     * one busy on yours.
     */
    {
        gboolean busy = json_object_has_member(agent, "busy") &&
                        json_object_get_boolean_member(agent, "busy");
        const gchar *peer = clawt_json_string(agent, "peer", NULL);
        g_autofree gchar *activity = NULL;

        /*
         * The queue moved here in full when the trailing edge became the
         * unread count's.  Two numbers in one 281px row, both about
         * "messages" and meaning opposite things, is not readable -- and
         * the depth was already written out here anyway.
         *
         * Said for a stopped agent too, which the badge never was: mail
         * waiting for an agent that is not running is exactly the case
         * durable mailboxes exist for, and that row had no subtitle at
         * all.
         */
        if (busy && depth > 0)
            activity = g_strdup_printf(
                "working… · %" G_GINT64_FORMAT " waiting", depth);
        else if (busy && peer != NULL && g_strcmp0(peer, "user") != 0)
            activity = g_strdup_printf("working — for %s", peer);
        else if (busy)
            activity = g_strdup("working…");
        else if (depth > 0)
            activity = g_strdup_printf(
                "%" G_GINT64_FORMAT " waiting to be read", depth);

        set_row_text(row,
                     clawt_json_string(agent, "name",
                                       clawt_json_string(agent, "id", "?")),
                     activity != NULL
                         ? activity
                         : clawt_json_string(agent, "description", ""));

        /*
         * A spinner beside the dot, because a colour that means "busy"
         * is a colour somebody has to learn and movement is not.
         */
        if (busy) {
            GtkWidget *spinner = gtk_spinner_new();

            gtk_spinner_set_spinning(GTK_SPINNER(spinner), TRUE);
            gtk_widget_set_tooltip_text(spinner, "taking a turn");
            gtk_box_append(GTK_BOX(box), spinner);
        }
    }

    adw_action_row_add_prefix(ADW_ACTION_ROW(row), state_dot(state));

    /*
     * The unread count, and *not* the mailbox depth.
     *
     * This row drew the depth for a long time, tooltipped "messages
     * waiting", and the event handler's own comment called it "what
     * tells you something happened elsewhere".  The intent was right and
     * it was wired to the wrong number: the depth is the agent's inbound
     * queue -- work waiting for it to read -- which is close to the
     * opposite.  An agent that has just answered you has depth 0 and
     * showed nothing, while one buried in peer traffic showed a large
     * number and had said nothing to you at all.
     */
    if (unread > 0) {
        gtk_box_append(GTK_BOX(box), unread_badge(unread));

        /*
         * ...and the title goes bold.  Colour is never the only signal
         * here -- the same rule the state dot already follows -- and
         * bold survives both a colourblind reader and a glance.
         */
        gtk_widget_add_css_class(row, "clawt-unread");
    }

    /*
     * A HOST badge is not decoration: an agent that can run commands on
     * the real machine should be visibly different from one that cannot,
     * at a glance, in the list.
     */
    if (strstr(caps, "host-control") != NULL)
        gtk_box_append(GTK_BOX(box),
                       badge("HOST", "error",
                             "this agent can run commands on this machine"));

    if (json_object_has_member(agent, "chief_of_staff") &&
        json_object_get_boolean_member(agent, "chief_of_staff"))
        gtk_box_append(GTK_BOX(box),
                       badge("CHIEF", "accent",
                             "hands work to the other agents"));

    adw_action_row_add_suffix(ADW_ACTION_ROW(row), box);

    g_object_set_data_full(G_OBJECT(row), "agent-id",
                           g_strdup(clawt_json_string(agent, "id", "")),
                           g_free);

    /*
     * The context menu decides what to grey out from here rather than
     * asking the daemon again: a right-click has to be instant, and the
     * sidebar was drawn from the same reply a moment ago.
     */
    g_object_set_data_full(G_OBJECT(row), "agent-state", g_strdup(state),
                           g_free);

    /*
     * The team, for the same reason: the context menu's Team submenu
     * ticks the one this agent is already on, and asking the daemon
     * which that is would be a round trip between the click and the
     * menu appearing.
     */
    g_object_set_data_full(G_OBJECT(row), "agent-team",
                           g_strdup(clawt_json_string(agent, "team", "")),
                           g_free);

    /*
     * And how it is drawn, which the transcript needs when a run header
     * is built for it.  Kept on the row rather than fetched with
     * agent.show at selection time: the sidebar was drawn from the same
     * reply a moment ago, and a round trip between clicking an agent and
     * seeing its first message is a round trip nobody asked for.
     */
    g_object_set_data_full(G_OBJECT(row), "agent-avatar",
                           g_strdup(clawt_json_string(agent, "avatar", "")),
                           g_free);
    g_object_set_data_full(G_OBJECT(row), "agent-color",
                           g_strdup(clawt_json_string(agent, "color", "")),
                           g_free);

    return row;
}

/* ── Grouping the fleet by team ──────────────────────────────────── */

/*
 * Whether a team is folded away.
 *
 * Kept in the client rather than in clawtilla.yaml. Which teams somebody
 * has collapsed while working on something else is a view preference,
 * like the fonts -- it belongs to the person at this screen, not to the
 * fleet, and syncing it between machines would be actively wrong.
 */
static gboolean
team_is_collapsed(ClawtWindow *self, const gchar *team_id)
{
    if (self->collapsed_teams == NULL || team_id == NULL)
        return FALSE;

    return g_hash_table_contains(self->collapsed_teams, team_id);
}

static void
on_team_header_toggled(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *team_id = g_object_get_data(G_OBJECT(button), "team-id");

    if (team_id == NULL)
        return;

    if (self->collapsed_teams == NULL)
        self->collapsed_teams = g_hash_table_new_full(g_str_hash,
                                                      g_str_equal,
                                                      g_free, NULL);

    if (g_hash_table_contains(self->collapsed_teams, team_id))
        g_hash_table_remove(self->collapsed_teams, team_id);
    else
        g_hash_table_add(self->collapsed_teams, g_strdup(team_id));

    refresh_agents(self);
}

/*
 * The teams to choose from, as ids, with "" first for none.
 *
 * Whatever the agent already has is included even when the daemon does
 * not declare it, for the same reason the screen-size row includes an
 * unusual resolution: a combo box that cannot represent the current
 * value replaces it the moment somebody saves the page without touching
 * it.
 */
static GtkStringList *
team_choices(ClawtWindow *self, const gchar *current, GStrv *out_ids)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) labels = g_ptr_array_new_with_free_func(g_free);
    GtkStringList *model;
    gboolean known = FALSE;
    guint i;

    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(labels, g_strdup("No team"));

    reply = clawt_window_request(self, "team.list", NULL);

    if (reply != NULL) {
        JsonArray *teams =
            json_object_get_array_member(clawt_payload_of(reply), "teams");

        for (i = 0; i < json_array_get_length(teams); i++) {
            JsonObject *team = json_array_get_object_element(teams, i);
            const gchar *id = clawt_json_string(team, "id", "");

            g_ptr_array_add(ids, g_strdup(id));
            g_ptr_array_add(labels,
                            g_strdup(clawt_json_string(team, "name", id)));

            if (g_strcmp0(id, current) == 0)
                known = TRUE;
        }
    }

    if (current != NULL && *current != '\0' && !known) {
        g_ptr_array_add(ids, g_strdup(current));
        g_ptr_array_add(labels,
                        g_strdup_printf("%s (not declared)", current));
    }

    g_ptr_array_add(ids, NULL);
    g_ptr_array_add(labels, NULL);

    model = gtk_string_list_new((const gchar *const *)labels->pdata);

    if (out_ids != NULL)
        *out_ids = (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);

    return model;
}

/* Which entry in that model matches an agent's current team. */
static guint
team_index_of(GStrv ids, const gchar *current)
{
    guint i;

    for (i = 0; ids != NULL && ids[i] != NULL; i++) {
        if (g_strcmp0(ids[i], current != NULL ? current : "") == 0)
            return i;
    }

    return 0;
}

/* Whether the fleet declares this team, as opposed to an agent naming it. */
static gboolean
team_is_declared(JsonArray *teams, const gchar *team_id)
{
    guint i;

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);

        if (g_strcmp0(clawt_json_string(team, "id", ""), team_id) == 0)
            return TRUE;
    }

    return FALSE;
}

/* The team's display name from the team.list reply, or its id. */
static const gchar *
team_display_name(JsonArray *teams, const gchar *team_id)
{
    guint i;

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);

        if (g_strcmp0(clawt_json_string(team, "id", ""), team_id) == 0)
            return clawt_json_string(team, "name", team_id);
    }

    /*
     * A team the daemon does not declare. The agent still shows, under
     * its own name, rather than vanishing into an "unknown" bucket --
     * the usual cause is a typo, and hiding it is how a typo survives.
     */
    return team_id;
}

static const gchar *
team_description(JsonArray *teams, const gchar *team_id)
{
    guint i;

    if (team_id == NULL)
        return NULL;

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);

        if (g_strcmp0(clawt_json_string(team, "id", ""), team_id) == 0)
            return clawt_json_string(team, "description", NULL);
    }

    return NULL;
}

/*
 * How many of a team are running, counted from the same reply the rows
 * are built from.
 *
 * Not from team.list, even though it answers this: the sidebar would
 * then show a tally from one moment and rows from another, and the two
 * disagreeing by one is exactly the kind of thing somebody notices and
 * cannot explain.
 */
static void
team_tally(JsonArray *agents, const gchar *team_id,
           guint *running, guint *total)
{
    guint i;

    *running = 0;
    *total = 0;

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);

        if (g_strcmp0(clawt_json_string(agent, "team", NULL), team_id) != 0)
            continue;

        (*total)++;

        if (g_strcmp0(clawt_json_string(agent, "state", ""), "running") == 0)
            (*running)++;
    }
}

/* Defined with the rest of the drag handling, below. */
static gboolean on_team_header_drop(GtkDropTarget *target, const GValue *value,
                                    gdouble x, gdouble y, gpointer user_data);
static GdkDragAction on_drop_hover_enter(GtkDropTarget *target, gdouble x,
                                         gdouble y, gpointer user_data);
static void on_drop_hover_leave(GtkDropTarget *target, gpointer user_data);

/*
 * A team's header: its name, how many of it are running, and a twisty.
 *
 * A GtkButton rather than an activatable row, because the sidebar's rows
 * drive selection -- a header that could be "selected" would put an
 * agent's transcript on screen under a team's name, and there is no
 * agent to show.
 */
static GtkWidget *
team_header_row(ClawtWindow *self,
                const gchar *team_id,
                const gchar *name,
                const gchar *description,
                guint        running,
                guint        total)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *button = gtk_button_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *twisty;
    GtkWidget *label = gtk_label_new(name);
    g_autofree gchar *tally = NULL;
    gboolean collapsed = team_is_collapsed(self, team_id);

    twisty = gtk_image_new_from_icon_name(
        collapsed ? "pan-end-symbolic" : "pan-down-symbolic");

    gtk_widget_add_css_class(label, "heading");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_widget_set_hexpand(label, TRUE);

    gtk_box_append(GTK_BOX(box), twisty);
    gtk_box_append(GTK_BOX(box), label);

    /*
     * Running out of total, rather than a single number. "3" could be
     * either, and the question somebody scanning a sidebar is asking is
     * how much of this team is awake.
     */
    tally = g_strdup_printf("%u/%u", running, total);
    gtk_box_append(GTK_BOX(box),
                   badge(tally, running > 0 ? "accent" : "dim",
                         "agents running on this team"));

    gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_widget_add_css_class(button, "flat");

    if (description != NULL && *description != '\0')
        gtk_widget_set_tooltip_text(button, description);

    g_object_set_data_full(G_OBJECT(button), "team-id", g_strdup(team_id),
                           g_free);
    g_signal_connect(button, "clicked",
                     G_CALLBACK(on_team_header_toggled), self);

    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), button);

    /*
     * Not selectable and not activatable: it is a heading with a button
     * in it, and arrow-key navigation should step over it to the next
     * agent rather than landing on something that shows nothing.
     */
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);

    /*
     * And a drop target, so an agent can be dragged onto the team rather
     * than picked from a menu.  On the *row* rather than on the button:
     * the button fills the header, and a target on it would miss the
     * strip of row around it -- which is exactly where somebody aiming
     * at a heading lets go.
     */
    {
        GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING,
                                                  GDK_ACTION_MOVE);

        g_object_set_data(G_OBJECT(row), "window", self);
        g_object_set_data_full(G_OBJECT(row), "team-id", g_strdup(team_id),
                               g_free);
        g_object_set_data_full(G_OBJECT(row), "team-label", g_strdup(name),
                               g_free);

        g_signal_connect(drop, "drop", G_CALLBACK(on_team_header_drop), row);
        g_signal_connect(drop, "enter", G_CALLBACK(on_drop_hover_enter), row);
        g_signal_connect(drop, "leave", G_CALLBACK(on_drop_hover_leave), row);
        gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(drop));
    }

    return row;
}

/* ── Dragging a row to reorder the fleet, or to change its team ──── */

/*
 * Which team an agent is on, from the row the sidebar drew for it.
 *
 * By id rather than by pointer, for the same reason the drag carries an
 * id: a refresh arriving mid-drag rebuilds every row.  A row that is not
 * there returns NULL, which the caller reads as "we cannot tell" and
 * declines to guess -- a collapsed team's rows are absent, though the
 * row being *dragged* is on screen by definition.
 */
static const gchar *
agent_row_data(ClawtWindow *self, const gchar *agent_id, const gchar *key)
{
    GtkWidget *child;

    for (child = gtk_widget_get_first_child(GTK_WIDGET(self->sidebar));
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        const gchar *id;

        if (!GTK_IS_LIST_BOX_ROW(child))
            continue;

        id = g_object_get_data(G_OBJECT(child), "agent-id");

        if (id != NULL && g_strcmp0(id, agent_id) == 0)
            return g_object_get_data(G_OBJECT(child), key);
    }

    return NULL;
}

static const gchar *
team_of_agent_row(ClawtWindow *self, const gchar *agent_id)
{
    return agent_row_data(self, agent_id, "agent-team");
}

/*
 * Puts @agent_id on @team, and says so.
 *
 * Shared by the header drop and the cross-team row drop, because they
 * are the same operation with different precision about where the agent
 * lands -- and two spellings of "move an agent to a team" would be two
 * behaviours, of which the less-used one would be the wrong one.
 *
 * Returns: %TRUE if the daemon accepted it
 */
static gboolean
move_agent_to_team(ClawtWindow *self, const gchar *agent_id,
                   const gchar *team, const gchar *team_label)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *message = NULL;

    reply = clawt_window_request(
        self, "agent.set",
        clawt_build_payload("agent", agent_id, "key", "team",
                            "value", team != NULL ? team : "", NULL));

    if (reply == NULL)
        return FALSE;

    message = (team != NULL && *team != '\0')
              ? g_strdup_printf("%s moved to %s.", agent_id,
                                team_label != NULL ? team_label : team)
              : g_strdup_printf("%s taken off its team.", agent_id);

    clawt_window_toast(self, message);

    return TRUE;
}

/*
 * Dropping an agent on a team's header puts it on that team.
 *
 * The team only -- where it lands inside the team is left alone.  A
 * header names a group rather than a position, and the group may be
 * folded away, in which case there is no row to place it beside and
 * nothing on screen that would show where it went.  Dropping on a *row*
 * is the gesture that says exactly where.
 */
static gboolean
on_team_header_drop(GtkDropTarget *target, const GValue *value, gdouble x,
                    gdouble y, gpointer user_data)
{
    GtkWidget *header = user_data;
    ClawtWindow *self = g_object_get_data(G_OBJECT(header), "window");
    const gchar *dragged = g_value_get_string(value);
    const gchar *team = g_object_get_data(G_OBJECT(header), "team-id");
    const gchar *label = g_object_get_data(G_OBJECT(header), "team-label");
    const gchar *was;

    (void)target;
    (void)x;
    (void)y;

    /*
     * Cleared here as well as on ::leave.  A drop does not promise a
     * leave after it, and a heading left lit under a row that has just
     * moved away reads as the drag still being in flight.
     */
    on_drop_hover_leave(NULL, header);

    if (dragged == NULL || team == NULL || self == NULL)
        return FALSE;

    /*
     * Already there.  Accepted rather than refused: the drag animating
     * back to where it started reads as "that did not work", and it did
     * work -- there was simply nothing to do.
     */
    was = team_of_agent_row(self, dragged);

    if (g_strcmp0(was != NULL ? was : "", team) == 0)
        return TRUE;

    if (!move_agent_to_team(self, dragged, team, label))
        return FALSE;

    refresh_agents(self);

    return TRUE;
}

/*
 * Highlights a row while something is being dragged over it.
 *
 * Dropping an agent onto a team heading is a gesture nothing else on
 * this screen suggests, so the heading has to say it will accept one.
 * Without it the only way to discover the feature is to try it and watch
 * the drag snap back -- which is also what "not a drop target" looks
 * like, so the two are indistinguishable to whoever tried.
 *
 * Through GtkListBox's own API rather than a class of our own.  It is
 * the styling every list in the desktop uses for this, and it carries
 * the theme with it -- a hand-rolled colour would be right in one theme
 * and wrong in the other, and the whole appearance system here exists
 * because people change theirs.
 */
static GdkDragAction
on_drop_hover_enter(GtkDropTarget *target, gdouble x, gdouble y,
                    gpointer user_data)
{
    GtkWidget *row = user_data;
    GtkWidget *box = gtk_widget_get_ancestor(row, GTK_TYPE_LIST_BOX);

    (void)target;
    (void)x;
    (void)y;

    if (box != NULL)
        gtk_list_box_drag_highlight_row(GTK_LIST_BOX(box),
                                        GTK_LIST_BOX_ROW(row));

    return GDK_ACTION_MOVE;
}

static void
on_drop_hover_leave(GtkDropTarget *target, gpointer user_data)
{
    GtkWidget *box = gtk_widget_get_ancestor(GTK_WIDGET(user_data),
                                             GTK_TYPE_LIST_BOX);

    (void)target;

    /*
     * The list box holds at most one highlight, so unhighlighting is
     * per-list rather than per-row -- and it must happen on leave as
     * well as on drop, or a drag abandoned over a heading leaves it lit
     * until the next one passes through.
     */
    if (box != NULL)
        gtk_list_box_drag_unhighlight_row(GTK_LIST_BOX(box));
}

/*
 * The id travels, not the widget.
 *
 * A row is rebuilt from the daemon's reply on every refresh -- and a
 * refresh can arrive mid-drag, because events are delivered from an idle
 * -- so a pointer to the row being dragged is a pointer that may not
 * exist by the time it is dropped. An id survives that.
 */
static GdkContentProvider *
on_row_drag_prepare(GtkDragSource *source, gdouble x, gdouble y,
                    gpointer user_data)
{
    GtkWidget *row = user_data;
    const gchar *agent_id = g_object_get_data(G_OBJECT(row), "agent-id");

    (void)source;
    (void)x;
    (void)y;

    if (agent_id == NULL)
        return NULL;

    return gdk_content_provider_new_typed(G_TYPE_STRING, agent_id);
}

static void
on_row_drag_begin(GtkDragSource *source, GdkDrag *drag, gpointer user_data)
{
    GtkWidget *row = user_data;

    (void)source;
    (void)drag;

    /* Faded, so it is obvious which row is in flight. */
    gtk_widget_set_opacity(row, 0.4);
}

static void
on_row_drag_end(GtkDragSource *source, GdkDrag *drag, gboolean delete,
                gpointer user_data)
{
    (void)source;
    (void)drag;
    (void)delete;

    gtk_widget_set_opacity(GTK_WIDGET(user_data), 1.0);
}

/*
 * Collect the ids in their new order and hand the whole list over.
 *
 * The whole list rather than "move this one here", because the daemon
 * numbers them from what it is given -- so one frame describes the
 * arrangement completely, and a client whose view was a moment stale
 * cannot produce a half-applied reorder.
 */
static gboolean
on_row_drop(GtkDropTarget *target, const GValue *value, gdouble x, gdouble y,
            gpointer user_data)
{
    GtkWidget *onto = user_data;
    ClawtWindow *self = g_object_get_data(G_OBJECT(onto), "window");
    const gchar *dragged = g_value_get_string(value);
    const gchar *landed = g_object_get_data(G_OBJECT(onto), "agent-id");
    const gchar *onto_team = g_object_get_data(G_OBJECT(onto), "agent-team");
    const gchar *from_team;
    g_autoptr(GString) ids = NULL;
    g_autoptr(JsonNode) reply = NULL;
    GtkWidget *child;
    gint onto_index;
    gboolean after;

    (void)target;
    (void)x;

    on_drop_hover_leave(NULL, onto);

    if (dragged == NULL || landed == NULL || self == NULL)
        return FALSE;

    if (g_strcmp0(dragged, landed) == 0)
        return FALSE;

    /*
     * Dropped among another team's agents, so it joins that team.
     *
     * Without this the reorder alone is undone the moment the sidebar
     * redraws: the daemon returns the fleet grouped, so an agent placed
     * below one from another team is sorted straight back under its own
     * heading -- a drop that visibly worked and then reverted, which
     * reads as the sidebar being broken rather than as the drag meaning
     * something narrower than it looked.
     *
     * Read from the row rather than asked for, and before the reorder,
     * so a refusal leaves the arrangement untouched instead of half
     * applied.  The team is set first for the same reason: it is the
     * coarser of the two, and an agent in the right team at the wrong
     * position is a better failure than the reverse.
     */
    from_team = team_of_agent_row(self, dragged);

    if (g_strcmp0(from_team != NULL ? from_team : "",
                  onto_team != NULL ? onto_team : "") != 0) {
        const gchar *label = NULL;

        if (self->teams_seen != NULL && onto_team != NULL)
            label = team_display_name(
                json_object_get_array_member(
                    clawt_payload_of(self->teams_seen), "teams"),
                onto_team);

        if (!move_agent_to_team(self, dragged, onto_team, label))
            return FALSE;
    }

    /*
     * Above or below, decided by which half of the row was dropped on.
     * Without this a row can never be placed last, because every drop
     * would put it before something.
     */
    after = y > (gdouble)(gtk_widget_get_height(onto) / 2);

    onto_index = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(onto));
    ids = g_string_new(NULL);

    for (child = gtk_widget_get_first_child(GTK_WIDGET(self->sidebar));
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        const gchar *agent_id;
        gint index;

        if (!GTK_IS_LIST_BOX_ROW(child))
            continue;

        agent_id = g_object_get_data(G_OBJECT(child), "agent-id");

        if (agent_id == NULL)
            continue;

        index = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(child));

        /* Taken out of where it was... */
        if (g_strcmp0(agent_id, dragged) == 0)
            continue;

        if (index == onto_index && !after) {
            g_string_append_printf(ids, "%s%s", ids->len > 0 ? "," : "",
                                   dragged);
        }

        g_string_append_printf(ids, "%s%s", ids->len > 0 ? "," : "",
                               agent_id);

        /* ...and put back beside the row it was dropped on. */
        if (index == onto_index && after)
            g_string_append_printf(ids, ",%s", dragged);
    }

    reply = clawt_window_request(
        self, "agent.reorder",
        clawt_build_payload("agents", ids->str, NULL));

    if (reply == NULL)
        return FALSE;

    refresh_agents(self);

    return TRUE;
}

/*
 * Both halves on every row: anything can be picked up, and anything can
 * be dropped on.
 */
static void
make_row_draggable(ClawtWindow *self, GtkWidget *row)
{
    GtkDragSource *source = gtk_drag_source_new();
    GtkDropTarget *target = gtk_drop_target_new(G_TYPE_STRING,
                                                GDK_ACTION_MOVE);

    g_object_set_data(G_OBJECT(row), "window", self);

    gtk_drag_source_set_actions(source, GDK_ACTION_MOVE);
    g_signal_connect(source, "prepare", G_CALLBACK(on_row_drag_prepare), row);
    g_signal_connect(source, "drag-begin", G_CALLBACK(on_row_drag_begin), row);
    g_signal_connect(source, "drag-end", G_CALLBACK(on_row_drag_end), row);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(source));

    g_signal_connect(target, "drop", G_CALLBACK(on_row_drop), row);
    g_signal_connect(target, "enter", G_CALLBACK(on_drop_hover_enter), row);
    g_signal_connect(target, "leave", G_CALLBACK(on_drop_hover_leave), row);
    gtk_widget_add_controller(row, GTK_EVENT_CONTROLLER(target));
}

/*
 * Selection, not activation, is what drives the view.
 *
 * AdwActionRow clears GtkListBoxRow:activatable unless an
 * activatable-widget is set, so ::row-activated never fires for these
 * rows at all -- clicking an agent moved the highlight and nothing else,
 * leaving the previous agent's transcript on screen under the new
 * agent's name.  ::row-selected also covers arrow-key navigation, which
 * activation never did.
 */
static void
on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *agent_id;

    (void)box;

    /* Emptying the list to rebuild it unselects; that is not a choice. */
    if (row == NULL)
        return;

    agent_id = g_object_get_data(G_OBJECT(row), "agent-id");

    if (agent_id != NULL)
        select_agent(self, agent_id);
}

static void
clear_list(GtkListBox *list)
{
    GtkWidget *child = gtk_widget_get_first_child(GTK_WIDGET(list));

    /*
     * Rows only, and walked rather than repeatedly taking the first
     * child.  A GtkPopover parented to the list -- the sidebar's context
     * menu is one -- is a child like any other, and gtk_list_box_remove()
     * refuses to take it: the old loop then asked for the same first
     * child forever and hung the window before it ever drew.
     */
    while (child != NULL) {
        GtkWidget *next = gtk_widget_get_next_sibling(child);

        if (GTK_IS_LIST_BOX_ROW(child))
            gtk_list_box_remove(list, child);

        child = next;
    }
}

static void
clear_box(GtkBox *box)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
        gtk_box_remove(box, child);
}

/*
 * One team's heading, remembered so it is never drawn twice.
 */
static void
append_team_header(ClawtWindow *self, JsonArray *teams, JsonArray *agents,
                   const gchar *team_id, GHashTable **emitted)
{
    const gchar *id = (team_id != NULL) ? team_id : "";
    guint running = 0;
    guint total = 0;

    if (g_hash_table_contains(*emitted, id))
        return;

    team_tally(agents, team_id, &running, &total);

    gtk_list_box_append(
        self->sidebar,
        team_header_row(self, id,
                        (*id != '\0') ? team_display_name(teams, id)
                                      : "No team",
                        team_description(teams, team_id),
                        running, total));

    g_hash_table_add(*emitted, g_strdup(id));
}

/*
 * The headings for every group that sorts before @next and has nobody in
 * it, so that an empty team is still somewhere an agent can be dropped.
 *
 * The order is the daemon's, not a second opinion about it: teamless
 * first, then the declared teams in the order team.list gives them,
 * which is the array `group_position()` indexes into.  Passing NULL for
 * @next means "everything that is left", which is how the teams nobody
 * is on reach the bottom of the list.
 *
 * The teamless group is spelled "", never NULL, and the caller has to
 * fold it: `clawt_json_string(agent, "team", NULL)` answers NULL for an
 * agent whose config has never had the key, so passing it straight
 * through read as "flush everything" and drew all four headings in a
 * row above the whole fleet.  Only for an agent that had never been on
 * a team -- one taken *off* one has `team: ""` and looked perfectly
 * correct, which is why the first probe run showed it in one line and
 * not the four below it.
 *
 * A team an agent names that nobody declared is not in that array at
 * all, and gets no heading from here -- it already gets one from the
 * agent standing in it, and inventing a position for something the
 * daemon sorts by a rule of its own is how the two orders drift apart.
 */
static void
emit_empty_headers_before(ClawtWindow *self, JsonArray *teams,
                          JsonArray *agents, const gchar *next,
                          GHashTable **emitted)
{
    const gchar *want = (next != NULL) ? next : "";
    gboolean to_the_end = (next == NULL);
    guint i;

    /*
     * Teamless comes first, and is emitted even when nobody is teamless
     * -- it is the only place to drop an agent that is being taken off
     * a team, and it disappears exactly when every agent has one, which
     * is when it is needed.
     */
    if (to_the_end || g_strcmp0(want, "") != 0)
        append_team_header(self, teams, agents, "", emitted);

    if (!to_the_end && g_strcmp0(want, "") == 0)
        return;

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);
        const gchar *id = clawt_json_string(team, "id", "");

        if (!to_the_end && g_strcmp0(id, want) == 0)
            return;

        append_team_header(self, teams, agents, id, emitted);
    }
}

static void
refresh_agents_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *agents;
    g_autoptr(JsonNode) team_reply = NULL;
    g_autofree gchar *shown_team = NULL;
    JsonArray *teams = NULL;
    g_autoptr(GHashTable) emitted = g_hash_table_new_full(g_str_hash,
                                                          g_str_equal,
                                                          g_free, NULL);
    guint i;

    reply = clawt_window_request(self, "agent.list", NULL);

    if (reply == NULL)
        return;

    /*
     * The teams' names and descriptions, which the agent list does not
     * carry -- it names an agent's team by id and nothing more. A daemon
     * too old to know the frame simply answers nothing and the sidebar
     * falls back to ids, which is a worse label and not a broken window.
     */
    team_reply = clawt_window_request(self, "team.list", NULL);

    if (team_reply != NULL)
        teams = json_object_get_array_member(clawt_payload_of(team_reply),
                                             "teams");

    /*
     * Kept for the context menu, which needs the same list and cannot
     * afford to fetch it.  A daemon that answered nothing leaves the
     * previous list in place rather than emptying it: a menu offering
     * only "No team" reads as a fleet with no teams, which is a
     * different and wrong statement from "we could not ask".
     */
    if (team_reply != NULL) {
        g_clear_pointer(&self->teams_seen, json_node_unref);
        self->teams_seen = json_node_ref(team_reply);
    }

    clear_list(self->sidebar);

    agents = json_object_get_array_member(clawt_payload_of(reply), "agents");

    /*
     * Which room is whose conversation, from the daemon's own answer.
     *
     * Rebuilt on every listing rather than kept, so an agent added,
     * removed or renamed cannot leave a stale entry behind pointing an
     * arriving message at somebody who is not there.
     */
    g_hash_table_remove_all(self->dm_rooms);

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *dm = clawt_json_string(agent, "dm_room", NULL);

        if (dm != NULL)
            g_hash_table_insert(self->dm_rooms, g_strdup(dm),
                                g_strdup(clawt_json_string(agent, "id", "")));
    }

    update_unread_tab(self);

    if (json_array_get_length(agents) == 0) {
        GtkWidget *row = adw_action_row_new();

        set_row_text(row, "No agents yet", "Use the + button to add one");
        gtk_list_box_append(self->sidebar, row);
        return;
    }

    for (i = 0; i < json_array_get_length(agents); i++) {
        JsonObject *agent = json_array_get_object_element(agents, i);
        const gchar *team = clawt_json_string(agent, "team", NULL);
        GtkWidget *row;

        /*
         * A header whenever the team changes, which works because the
         * daemon returns agents grouped by team already. Doing the
         * grouping here as well would be a second answer to what order
         * the fleet is in.
         *
         * The headers a team with no agents would not get are filled in
         * around it, in the daemon's own group order.  They are not
         * decoration: a heading is what an agent is dragged onto, so
         * without them a team somebody has just created cannot be filled
         * -- which is precisely when they would reach for it -- and a
         * fleet where everyone has a team has nothing to drag back out
         * to.  A gesture that works in one direction only reads as
         * broken rather than as narrow.
         */
        if (g_strcmp0(team, shown_team) != 0 || i == 0) {
            emit_empty_headers_before(self, teams, agents,
                                      team != NULL ? team : "", &emitted);
            append_team_header(self, teams, agents, team, &emitted);

            g_free(shown_team);
            shown_team = g_strdup(team);
        }

        /*
         * Folded away, but still counted -- the header's tally is what
         * makes collapsing safe, because it says what is behind it.
         */
        if (team_is_collapsed(self, team != NULL ? team : ""))
            continue;

        row = agent_row(agent, unread_for(self, clawt_json_string(agent, "id",
                                                                  "")));
        gtk_list_box_append(self->sidebar, row);

        /*
         * After the append, because a drop reads the row's index and a
         * row that is not in the list yet does not have one.
         */
        make_row_draggable(self, row);

        /*
         * Keep the current selection across a refresh, and make the very
         * first refresh pick something.  Both go through
         * gtk_list_box_select_row() rather than select_agent() so the
         * highlight and self->selected_agent can never disagree -- they
         * did, and the sidebar showed no selection at all until the
         * second refresh.
         */
        if (g_strcmp0(clawt_json_string(agent, "id", ""),
                      self->selected_agent) == 0 ||
            (self->selected_agent == NULL && i == 0))
            gtk_list_box_select_row(self->sidebar, GTK_LIST_BOX_ROW(row));
    }

    /* Whatever the fleet declares and nobody is on yet, at the bottom. */
    emit_empty_headers_before(self, teams, agents, NULL, &emitted);
}

static void
refresh_agents(ClawtWindow *self)
{
    if (!refresh_enter(self, CLAWT_REFRESH_AGENTS))
        return;

    do {
        refresh_agents_once(self);
    } while (refresh_repeat(self, CLAWT_REFRESH_AGENTS));
}

/* ── Chat ────────────────────────────────────────────────────────── */

/*
 * Shows or hides the activity line.
 *
 * The spinner is stopped when hidden rather than left running: a
 * GtkSpinner that is not visible still drives a frame clock, and there
 * is one per window for the life of the process.
 */
static void
set_activity(ClawtWindow *self, const gchar *text)
{
    if (self->activity_bar == NULL)
        return;

    if (text == NULL) {
        gtk_spinner_stop(self->activity_spinner);
        gtk_widget_set_visible(self->activity_bar, FALSE);
        gtk_label_set_text(self->streaming, "");
        return;
    }

    gtk_label_set_text(self->streaming, text);
    gtk_widget_set_visible(self->activity_bar, TRUE);
    gtk_spinner_start(self->activity_spinner);
}

/*
 * Sets a label from markdown, falling back to plain text.
 *
 * Pango refuses unbalanced markup and a GtkLabel handed something it
 * cannot parse renders *nothing* -- a message that silently disappears
 * is far worse than one that renders without its bold. So the markup is
 * checked before it is used, and anything that fails goes up as the
 * text it came from.
 */
/* ── Appearance ──────────────────────────────────────────────────── */

/*
 * The chosen code font, and the provider carrying the rest.
 *
 * File-scope because both are genuinely per-display rather than per
 * window: a GtkCssProvider is added to the GdkDisplay, and a second
 * window must not add a second copy of the same sheet. The alternative
 * -- threading an appearance pointer down to set_label_markdown() --
 * would put a parameter on a function whose whole job is one label, for
 * a value that cannot differ between two labels.
 */
static GtkCssProvider *appearance_provider = NULL;
static gchar          *appearance_code_font = NULL;

/*
 * Structure this client draws that libadwaita has no widget for.
 *
 * Concatenated ahead of the generated appearance sheet rather than given
 * a provider of its own: a second provider at the same priority would
 * make which sheet wins depend on the order they were added, and the
 * appearance rules already had to be reduced to one provider once for
 * exactly that reason.
 *
 * Every colour is a libadwaita named colour, never a hex value.  That is
 * what makes a palette a palette swap rather than a second pass over
 * every rule here -- the Catppuccin sheet redefines `accent_bg_color`
 * and this follows it for free.
 */
static const gchar CLAWT_STRUCTURE_CSS[] =
    /*
     * The unread pill.  Filled, because everything else in that row is a
     * coloured caption: filled means for you, text means about the
     * agent.
     */
    ".clawt-unread-badge {\n"
    "  background-color: @accent_bg_color;\n"
    "  color: @accent_fg_color;\n"
    "  border-radius: 9px;\n"
    "  min-height: 18px;\n"
    "  min-width: 6px;\n"
    "  padding: 0 6px;\n"
    "  font-weight: bold;\n"
    "}\n"
    /*
     * ...and the name in bold beside it.  Colour is never the only
     * signal in this client; the state dot already holds that rule.
     */
    ".clawt-unread .title {\n"
    "  font-weight: bold;\n"
    "}\n"
    /*
     * The operator's own turns.
     *
     * 12px is libadwaita's card radius, so a bubble matches every other
     * rounded surface in the application rather than inventing one.
     * Named colours throughout, which is what makes a palette a palette
     * swap rather than a second pass over this block.
     */
    ".clawt-bubble {\n"
    "  background-color: @accent_bg_color;\n"
    "  color: @accent_fg_color;\n"
    "  border-radius: 12px;\n"
    "  padding: 8px 12px;\n"
    "}\n"
    /*
     * A run of bubbles reads as one utterance because the second and
     * later ones drop the corner nearest the one above.
     */
    ".clawt-bubble-cont {\n"
    "  border-top-right-radius: 4px;\n"
    "}\n"
    /*
     * Links and inline code inside a bubble.  Without this they render
     * in the accent colour on the accent colour, which is invisible
     * rather than merely low contrast.
     */
    ".clawt-bubble .body {\n"
    "  color: @accent_fg_color;\n"
    "}\n";

/*
 * Applies fonts and colour scheme.
 *
 * Called on startup and on every change in the settings dialog, so the
 * dialog is a live preview rather than something you close and hope
 * about.
 */
static void
apply_appearance(ClawtAppearance *appearance)
{
    GdkDisplay *display = gdk_display_get_default();
    g_autofree gchar *css = NULL;
    AdwStyleManager *style = adw_style_manager_get_default();

    if (appearance == NULL || display == NULL)
        return;

    /*
     * The scheme first, and asked for rather than switched on, so a
     * palette added later needs no case here.  A palette still sets the
     * scheme: its colours only name some of libadwaita's, and the rest
     * come from whichever scheme is underneath -- so Mocha over a light
     * libadwaita is not a lighter Mocha, it is two palettes arguing.
     */
    {
        ClawtTheme theme = clawt_appearance_get_theme(appearance);

        if (theme == CLAWT_THEME_SYSTEM)
            adw_style_manager_set_color_scheme(style,
                                               ADW_COLOR_SCHEME_DEFAULT);
        else if (clawt_appearance_theme_is_dark(theme))
            adw_style_manager_set_color_scheme(style,
                                               ADW_COLOR_SCHEME_FORCE_DARK);
        else
            adw_style_manager_set_color_scheme(style,
                                               ADW_COLOR_SCHEME_FORCE_LIGHT);
    }

    g_free(appearance_code_font);
    appearance_code_font =
        g_strdup(clawt_appearance_get_monospace_font(appearance));

    {
        g_autofree gchar *generated = clawt_appearance_to_css(appearance);

        css = g_strconcat(CLAWT_STRUCTURE_CSS, generated, NULL);
    }

    /*
     * One provider, reloaded, rather than a new one each time.  Adding a
     * provider per change leaves every previous sheet on the display at
     * the same priority, so the fonts stop changing after the first edit
     * -- the oldest rule keeps winning ties.
     */
    if (appearance_provider == NULL) {
        appearance_provider = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(
            display, GTK_STYLE_PROVIDER(appearance_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    gtk_css_provider_load_from_string(appearance_provider, css);
}

static void
set_label_markdown(GtkLabel *label, const gchar *body)
{
    g_autofree gchar *markup =
        clawt_markdown_to_pango_full(body, appearance_code_font);
    g_autoptr(GError) error = NULL;

    if (pango_parse_markup(markup, -1, 0, NULL, NULL, NULL, &error)) {
        gtk_label_set_markup(label, markup);
        return;
    }

    g_warning("markdown produced markup Pango rejected (%s); "
              "showing it plainly", error->message);
    gtk_label_set_text(label, body != NULL ? body : "");
}

/* ── Context menus ───────────────────────────────────────────────── */

/*
 * Attaches a right-click menu to a widget.
 *
 * GtkPopoverMenu wants a GMenuModel and an action group, which is a lot
 * of ceremony for six entries whose targets change per widget. A
 * GtkListBox in a plain popover is less machinery and behaves the same
 * way to the person using it -- and a popover is fine here, unlike one
 * parented to a GtkEntry, because it hangs off a container.
 */
typedef struct {
    const gchar *label;
    const gchar *action;
} MenuEntry;

typedef void (*MenuChosen)(ClawtWindow *self, const gchar *action,
                           gpointer target);

typedef struct {
    ClawtWindow *window;
    GtkWidget   *popover;
    MenuChosen   chosen;
    gpointer     target;      /* borrowed; owned by the widget it hangs off */
} ContextMenu;

static void
context_menu_free(gpointer data)
{
    ContextMenu *menu = data;

    /*
     * The popover is usually already gone by the time this runs.
     *
     * It is a child of the widget this is attached to, and GTK
     * unparents a widget's children while destroying it -- which
     * happens before object data is released. So this ran on a pointer
     * to a finalized widget and asserted once per message, every time a
     * transcript was cleared: fourteen messages, fourteen criticals.
     *
     * The weak pointer is what makes "already gone" tell the truth
     * instead of dangling. The unparent is kept for the other way this
     * can be reached -- the data being replaced on a widget that is
     * still alive.
     */
    if (menu->popover != NULL) {
        g_object_remove_weak_pointer(G_OBJECT(menu->popover),
                                     (gpointer *)&menu->popover);

        if (gtk_widget_get_parent(menu->popover) != NULL)
            gtk_widget_unparent(menu->popover);
    }

    g_free(menu);
}

static void
on_context_chosen(GtkButton *button, gpointer user_data)
{
    ContextMenu *menu = user_data;
    const gchar *action = g_object_get_data(G_OBJECT(button), "action");

    gtk_popover_popdown(GTK_POPOVER(menu->popover));

    if (action != NULL)
        menu->chosen(menu->window, action, menu->target);
}

static void
on_context_pressed(GtkGestureClick *gesture, gint n_press, gdouble x,
                   gdouble y, gpointer user_data)
{
    ContextMenu *menu = user_data;
    GdkRectangle at;

    (void)gesture;
    (void)n_press;

    at.x = (gint)x;
    at.y = (gint)y;
    at.width = 1;
    at.height = 1;

    gtk_popover_set_pointing_to(GTK_POPOVER(menu->popover), &at);
    gtk_popover_popup(GTK_POPOVER(menu->popover));
}

/*
 * Takes the popover off its owner while the owner is still a widget.
 *
 * A popover given a parent by hand is a child that parent knows nothing
 * about, so nothing else will remove it.
 */
static void
on_menu_owner_destroyed(GtkWidget *widget, gpointer user_data)
{
    ContextMenu *menu = user_data;

    (void)widget;

    if (menu->popover != NULL &&
        gtk_widget_get_parent(menu->popover) != NULL)
        gtk_widget_unparent(menu->popover);
}

static void
add_context_menu(ClawtWindow *self, GtkWidget *widget,
                 const MenuEntry *entries, gsize n_entries,
                 MenuChosen chosen, gpointer target)
{
    ContextMenu *menu = g_new0(ContextMenu, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkGesture *gesture;
    gsize i;

    menu->window = self;
    menu->chosen = chosen;
    menu->target = target;

    /*
     * Buttons rather than a GtkListBox.
     *
     * A list box selects a row when it takes focus, and a popover takes
     * focus the moment it opens -- so right-clicking a message ran the
     * first entry immediately and copied it without being asked. A
     * button does nothing until it is clicked, which is the entire
     * behaviour wanted here.
     */
    for (i = 0; i < n_entries; i++) {
        GtkWidget *item;

        /* A NULL label is a separator in the table. */
        if (entries[i].label == NULL) {
            item = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
            gtk_widget_set_margin_top(item, 3);
            gtk_widget_set_margin_bottom(item, 3);
            gtk_box_append(GTK_BOX(box), item);
            continue;
        }

        item = gtk_button_new_with_label(entries[i].label);
        gtk_widget_add_css_class(item, "flat");
        gtk_button_set_has_frame(GTK_BUTTON(item), FALSE);
        gtk_widget_set_halign(item, GTK_ALIGN_FILL);

        /* Left-aligned, like every other menu on the desktop. */
        gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(item))),
                             0.0f);

        g_object_set_data_full(G_OBJECT(item), "action",
                               g_strdup(entries[i].action), g_free);
        g_signal_connect(item, "clicked", G_CALLBACK(on_context_chosen), menu);
        gtk_box_append(GTK_BOX(box), item);
    }

    menu->popover = gtk_popover_new();
    gtk_popover_set_has_arrow(GTK_POPOVER(menu->popover), FALSE);
    gtk_popover_set_child(GTK_POPOVER(menu->popover), box);
    gtk_widget_set_parent(menu->popover, widget);
    g_object_add_weak_pointer(G_OBJECT(menu->popover),
                              (gpointer *)&menu->popover);

    gesture = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(gesture),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(gesture, "pressed", G_CALLBACK(on_context_pressed), menu);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(gesture));

    /*
     * Unparented from ::destroy, not from the object data's notify.
     *
     * qdata is cleared in finalize, and gtk_widget_finalize() checks for
     * leftover children *before* chaining up to it -- so the notify ran
     * too late and every chip in a cleared transcript warned "Finalizing
     * GtkButton, but it still has children left: GtkPopover". ::destroy
     * is emitted from dispose, which is early enough for the child to be
     * gone before anything counts them.
     */
    g_signal_connect(widget, "destroy", G_CALLBACK(on_menu_owner_destroyed),
                     menu);

    g_object_set_data_full(G_OBJECT(widget), "context-menu", menu,
                           context_menu_free);
}

/* ── What the menus do ───────────────────────────────────────────── */

static void
copy_text(ClawtWindow *self, const gchar *text, const gchar *what)
{
    g_autofree gchar *message = NULL;

    gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(self)),
                           text != NULL ? text : "");

    message = g_strdup_printf("%s copied.", what);
    clawt_window_toast(self, message);
}

/*
 * Converts and copies, saying so when the format is not available.
 *
 * Silently handing back markdown under an org label would be the worst
 * of the three possible answers.
 */
static void
copy_as(ClawtWindow *self, const gchar *markdown, ClawtExportFormat format,
        const gchar *what)
{
    g_autofree gchar *converted = NULL;
    g_autoptr(GError) error = NULL;

    converted = clawt_export_convert(markdown, format, &error);

    if (converted == NULL) {
        clawt_window_toast(self, error->message);
        return;
    }

    copy_text(self, converted, what);
}

/*
 * The whole conversation as a markdown document.
 *
 * Read back from the daemon rather than scraped off the widgets: the
 * transcript on screen is the last two hundred messages, and an export
 * that quietly stopped there would be a export of the window rather
 * than of the conversation.
 */
static gchar *
conversation_markdown(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) messages = NULL;
    JsonArray *array;
    guint i;

    if (self->selected_agent == NULL)
        return NULL;

    reply = clawt_window_request(
        self, "room.history",
        clawt_build_payload("room", self->selected_agent, "as", "user",
                            "limit", "5000", NULL));

    if (reply == NULL)
        return NULL;

    array = json_object_get_array_member(clawt_payload_of(reply), "messages");
    messages = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_message_free);

    for (i = 0; i < json_array_get_length(array); i++) {
        JsonObject *one = json_array_get_object_element(array, i);
        ClawtMessage *message = clawt_message_new(
            self->selected_room, clawt_json_string(one, "sender", "?"),
            clawt_json_string(one, "body", ""));

        clawt_message_set_timestamp(message, clawt_json_int(one, "ts", 0));
        g_ptr_array_add(messages, message);
    }

    return clawt_export_transcript(self->selected_agent, messages,
                                   CLAWT_EXPORT_MARKDOWN, NULL);
}

static ClawtExportFormat
format_from_action(const gchar *action)
{
    if (g_str_has_suffix(action, "org"))
        return CLAWT_EXPORT_ORG;

    if (g_str_has_suffix(action, "text"))
        return CLAWT_EXPORT_PLAIN;

    return CLAWT_EXPORT_MARKDOWN;
}

static void
on_conversation_saved(GObject *source, GAsyncResult *result, gpointer data)
{
    ClawtWindow *self = g_object_get_data(source, "window");
    g_autoptr(GFile) file = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *contents = data;

    file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result,
                                       &error);

    if (file == NULL) {
        if (error != NULL &&
            !g_error_matches(error, GTK_DIALOG_ERROR,
                             GTK_DIALOG_ERROR_DISMISSED))
            clawt_window_toast(self, error->message);

        g_object_unref(self);
        return;
    }

    if (!g_file_replace_contents(file, contents, strlen(contents), NULL,
                                 FALSE, G_FILE_CREATE_NONE, NULL, NULL,
                                 &error))
        clawt_window_toast(self, error->message);
    else
        clawt_window_toast(self, "Saved.");

    g_object_unref(self);
}

static void
save_document(ClawtWindow *self, const gchar *contents, const gchar *basename,
              ClawtExportFormat format)
{
    GtkFileDialog *dialog = gtk_file_dialog_new();
    g_autofree gchar *suggested = g_strconcat(
        basename, clawt_export_format_extension(format), NULL);

    gtk_file_dialog_set_title(dialog, "Save");
    gtk_file_dialog_set_initial_name(dialog, suggested);

    /*
     * The contents travel with the dialog rather than being re-derived
     * in the callback: by the time somebody has picked a filename the
     * conversation may have moved on, and saving something other than
     * what they asked for is worse than not saving.
     */
    g_object_set_data_full(G_OBJECT(dialog), "window", g_object_ref(self),
                           g_object_unref);
    gtk_file_dialog_save(dialog, GTK_WINDOW(self), NULL, on_conversation_saved,
                         g_strdup(contents));
    g_object_unref(dialog);
}

static void
open_document_in_editor(ClawtWindow *self, const gchar *contents,
                        const gchar *basename, ClawtExportFormat format)
{
    g_autofree gchar *template = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    gint fd;

    template = g_strdup_printf("%s-XXXXXX%s", basename,
                               clawt_export_format_extension(format));
    fd = g_file_open_tmp(template, &path, &error);

    if (fd < 0) {
        clawt_window_toast(self, error->message);
        return;
    }

    close(fd);

    if (!g_file_set_contents(path, contents, -1, &error)) {
        clawt_window_toast(self, error->message);
        g_unlink(path);
        return;
    }

    /*
     * Not deleted afterwards, unlike the message composer's scratch
     * file: this is somebody taking a conversation away to keep, and
     * removing it the moment their editor exits would throw away what
     * they went to get.
     */
    open_path_in_editor(self, path, basename);
}

static void
on_conversation_action(ClawtWindow *self, const gchar *action, gpointer target)
{
    g_autofree gchar *markdown = NULL;
    g_autofree gchar *converted = NULL;
    g_autoptr(GError) error = NULL;
    ClawtExportFormat format = format_from_action(action);

    (void)target;

    markdown = conversation_markdown(self);

    if (markdown == NULL) {
        clawt_window_toast(self, "there is no conversation to export");
        return;
    }

    converted = clawt_export_convert(markdown, format, &error);

    if (converted == NULL) {
        clawt_window_toast(self, error->message);
        return;
    }

    if (g_str_has_prefix(action, "copy"))
        copy_text(self, converted, "Conversation");
    else if (g_str_has_prefix(action, "edit"))
        open_document_in_editor(self, converted, self->selected_agent, format);
    else if (g_str_has_prefix(action, "save"))
        save_document(self, converted, self->selected_agent, format);
}

static void
on_message_action(ClawtWindow *self, const gchar *action, gpointer target)
{
    const gchar *body = target;

    if (g_strcmp0(action, "copy-markdown") == 0) {
        copy_text(self, body, "Message");
        return;
    }

    copy_as(self, body, format_from_action(action), "Message");
}


/* ── Attachment previews in the transcript ───────────────────────── */

/*
 * The marker body_with_attachments() writes.
 *
 * The transcript is rebuilt from what the daemon stored, so the only
 * way back to "this message had a picture on it" is to recognise the
 * line we wrote. Matched on the prefix rather than the whole sentence,
 * so rewording the guidance does not silently turn previews off.
 */
#define ATTACHMENT_MARKER CLAWT_ATTACHMENT_MARKER

/*
 * Brings an agent-sent file to this machine, once.
 *
 * An attachment the *operator* sent is a path on this host, because the
 * client put it there.  One an *agent* sent is a `clawt:<id>` naming a
 * copy the daemon took at send time -- and the daemon may be on another
 * machine, which is the whole reason the bytes travel rather than the
 * path.  Cached under the user's cache directory so a transcript
 * redrawn on every fleet event does not re-fetch every picture in it.
 *
 * Returns: (transfer full) (nullable): a local path, or %NULL
 */
static gchar *
fetch_attachment(ClawtWindow *self, const gchar *id)
{
    g_autofree gchar *dir = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *encoded;
    guchar *bytes;
    gsize length = 0;

    if (id == NULL || *id == '\0')
        return NULL;

    dir = g_build_filename(g_get_user_cache_dir(), "clawtilla",
                           "attachments", NULL);
    path = g_build_filename(dir, id, NULL);

    if (g_file_test(path, G_FILE_TEST_EXISTS))
        return g_steal_pointer(&path);

    reply = clawt_window_request(self, "attachment.get",
                                 clawt_build_payload("id", id, NULL));

    if (reply == NULL)
        return NULL;

    encoded = clawt_json_string(clawt_payload_of(reply), "base64", NULL);

    if (encoded == NULL)
        return NULL;

    if (g_mkdir_with_parents(dir, 0700) != 0)
        return NULL;

    bytes = g_base64_decode(encoded, &length);

    if (!g_file_set_contents(path, (const gchar *)bytes, (gssize)length,
                             NULL)) {
        g_free(bytes);
        return NULL;
    }

    g_free(bytes);

    return g_steal_pointer(&path);
}

static gboolean
looks_like_an_image(const gchar *path)
{
    static const gchar *extensions[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", NULL
    };
    g_autofree gchar *lowered = g_ascii_strdown(path, -1);
    gsize i;

    for (i = 0; extensions[i] != NULL; i++) {
        if (g_str_has_suffix(lowered, extensions[i]))
            return TRUE;
    }

    return FALSE;
}

static gboolean
on_preview_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
               GdkModifierType state, gpointer user_data)
{
    (void)controller;
    (void)keycode;
    (void)state;

    if (keyval != GDK_KEY_Escape)
        return GDK_EVENT_PROPAGATE;

    gtk_window_destroy(GTK_WINDOW(user_data));
    return GDK_EVENT_STOP;
}

/*
 * Hands a file to the desktop -- xdg-open's job.
 *
 * GtkFileLauncher rather than a spawn, so this goes through the
 * portal when there is one and through the mime handler when there is
 * not, which is what a person means by "open it".
 */
static void
open_with_desktop(ClawtWindow *self, const gchar *path)
{
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);

    gtk_file_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL, NULL);
}

static void
on_attachment_saved(GObject *source, GAsyncResult *result, gpointer data)
{
    ClawtWindow *self = g_object_get_data(source, "window");
    g_autoptr(GFile) target = NULL;
    g_autoptr(GFile) from = g_file_new_for_path(data);
    g_autoptr(GError) error = NULL;

    g_free(data);

    target = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result,
                                         &error);

    if (target == NULL) {
        if (error != NULL &&
            !g_error_matches(error, GTK_DIALOG_ERROR,
                             GTK_DIALOG_ERROR_DISMISSED))
            clawt_window_toast(self, error->message);

        g_object_unref(self);
        return;
    }

    if (!g_file_copy(from, target, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL,
                     &error))
        clawt_window_toast(self, error->message);
    else
        clawt_window_toast(self, "Saved.");

    g_object_unref(self);
}

static void
on_attachment_delete_confirmed(AdwAlertDialog *dialog, const gchar *response,
                               gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *path = g_object_get_data(G_OBJECT(dialog), "path");
    g_autofree gchar *name = NULL;
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "delete") != 0)
        return;

    name = g_path_get_basename(path);

    /*
     * Through the daemon, which owns the exchange directory and checks
     * the name, rather than unlinking from here -- a client that
     * deletes by path is a client that can be asked to delete any path.
     */
    reply = clawt_window_request(
        self, "attachment.remove",
        clawt_build_payload("agent", self->selected_agent, "name", name,
                            NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Deleted. The message still names it.");
    load_history(self);
}

static void
on_attachment_action(ClawtWindow *self, const gchar *action, gpointer target)
{
    const gchar *path = target;

    if (g_strcmp0(action, "open") == 0) {
        open_with_desktop(self, path);
        return;
    }

    if (g_strcmp0(action, "copy-path") == 0) {
        copy_text(self, path, "Path");
        return;
    }

    if (g_strcmp0(action, "save") == 0) {
        GtkFileDialog *dialog = gtk_file_dialog_new();
        g_autofree gchar *name = g_path_get_basename(path);

        gtk_file_dialog_set_title(dialog, "Save a copy");
        gtk_file_dialog_set_initial_name(dialog, name);
        g_object_set_data_full(G_OBJECT(dialog), "window",
                               g_object_ref(self), g_object_unref);
        gtk_file_dialog_save(dialog, GTK_WINDOW(self), NULL,
                             on_attachment_saved, g_strdup(path));
        g_object_unref(dialog);
        return;
    }

    if (g_strcmp0(action, "delete") == 0) {
        AdwAlertDialog *dialog;
        g_autofree gchar *name = g_path_get_basename(path);
        g_autofree gchar *body = g_strdup_printf(
            "\xe2\x80\x9c%s\xe2\x80\x9d will be deleted from %s's exchange "
            "directory. This cannot be undone, and the message will still "
            "name the file.", name, self->selected_agent);

        dialog = ADW_ALERT_DIALOG(
            adw_alert_dialog_new("Delete this file?", body));
        adw_alert_dialog_add_responses(dialog, "cancel", "Cancel",
                                       "delete", "Delete", NULL);
        adw_alert_dialog_set_response_appearance(dialog, "delete",
                                                 ADW_RESPONSE_DESTRUCTIVE);
        adw_alert_dialog_set_default_response(dialog, "cancel");
        g_object_set_data_full(G_OBJECT(dialog), "path", g_strdup(path),
                               g_free);
        g_signal_connect(dialog, "response",
                         G_CALLBACK(on_attachment_delete_confirmed), self);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
    }
}

static const MenuEntry attachment_menu[] = {
    { "Open",              "open" },
    { "Save a copy\xe2\x80\xa6", "save" },
    { "Copy path",         "copy-path" },
    { NULL,                NULL },
    { "Delete permanently", "delete" }
};

/*
 * Opens one attachment full size.
 *
 * A window rather than a dialog: this is something to look at beside
 * the conversation, and a modal one would stop you reading the message
 * it came with.
 */
static void
on_preview_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *path = g_object_get_data(G_OBJECT(button), "path");
    GtkWidget *window;
    GtkWidget *scroll;
    GtkWidget *picture;
    GtkEventController *keys;
    g_autofree gchar *title = NULL;

    if (path == NULL)
        return;

    title = g_path_get_basename(path);

    window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(window), title);
    gtk_window_set_transient_for(GTK_WINDOW(window), GTK_WINDOW(self));
    gtk_window_set_default_size(GTK_WINDOW(window), 900, 700);

    picture = gtk_picture_new_for_filename(path);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);

    /*
     * SCALE_DOWN, not CONTAIN: a big screenshot shrinks to fit, and a
     * small image is shown at its own size rather than blown up to fill
     * the window, which is what CONTAIN does and it looks like a
     * mistake.
     */
    gtk_picture_set_content_fit(GTK_PICTURE(picture),
                                GTK_CONTENT_FIT_SCALE_DOWN);

    scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), picture);
    gtk_window_set_child(GTK_WINDOW(window), scroll);

    keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(on_preview_key), window);
    gtk_widget_add_controller(window, keys);

    gtk_window_present(GTK_WINDOW(window));
}

/*
 * A file that is not an image: a name, a size, and the same menu.
 *
 * Clicking opens it with the desktop's handler, which is what makes a
 * PDF in a conversation something you can read rather than a path you
 * have to go and find.
 */
static void
on_file_chip_clicked(GtkButton *button, gpointer user_data)
{
    open_with_desktop(user_data, g_object_get_data(G_OBJECT(button), "path"));
}

static void
append_file_chip(ClawtWindow *self, GtkWidget *row, const gchar *path,
                 gboolean from_user)
{
    g_autoptr(GFile) file = g_file_new_for_path(path);
    g_autoptr(GFileInfo) info = NULL;
    g_autofree gchar *name = g_path_get_basename(path);
    g_autofree gchar *label = NULL;
    GtkWidget *button;
    GtkWidget *box;
    GtkWidget *icon;

    info = g_file_query_info(file,
                             G_FILE_ATTRIBUTE_STANDARD_SIZE ","
                             G_FILE_ATTRIBUTE_STANDARD_ICON,
                             G_FILE_QUERY_INFO_NONE, NULL, NULL);

    if (info != NULL) {
        g_autofree gchar *size = g_format_size(
            (guint64)g_file_info_get_size(info));

        label = g_strdup_printf("%s  \xc2\xb7  %s", name, size);
    } else {
        label = g_strdup(name);
    }

    box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    /*
     * The icon the desktop would use for this type, so a PDF looks like
     * a PDF rather than like every other attachment.
     */
    icon = (info != NULL && g_file_info_get_icon(info) != NULL)
           ? gtk_image_new_from_gicon(g_file_info_get_icon(info))
           : gtk_image_new_from_icon_name("text-x-generic-symbolic");

    gtk_box_append(GTK_BOX(box), icon);
    gtk_box_append(GTK_BOX(box), gtk_label_new(label));

    button = gtk_button_new();
    gtk_button_set_child(GTK_BUTTON(button), box);
    gtk_widget_add_css_class(button, "card");
    gtk_widget_set_halign(button,
                          from_user ? GTK_ALIGN_END : GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(button, path);
    g_object_set_data_full(G_OBJECT(button), "path", g_strdup(path), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_file_chip_clicked),
                     self);

    add_context_menu(self, button, attachment_menu,
                     G_N_ELEMENTS(attachment_menu), on_attachment_action,
                     g_object_get_data(G_OBJECT(button), "path"));

    gtk_box_append(GTK_BOX(row), button);
}

/*
 * A thumbnail under any message that carried a picture.
 *
 * So the conversation still shows what was sent when you scroll back to
 * it -- a path in a body is a thing nobody remembers the content of ten
 * minutes later.
 */
static void
append_attachment_previews(ClawtWindow *self, GtkWidget *row,
                           const gchar *body, gboolean from_user)
{
    g_auto(GStrv) lines = NULL;
    gboolean in_block = FALSE;
    gsize i;

    if (body == NULL || strstr(body, ATTACHMENT_MARKER) == NULL)
        return;

    lines = g_strsplit(body, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        g_autofree gchar *candidate = NULL;
        const gchar *start;
        GtkWidget *button;
        GtkWidget *picture;

        if (strstr(lines[i], ATTACHMENT_MARKER) != NULL) {
            in_block = TRUE;
            continue;
        }

        if (!in_block)
            continue;

        /*
         * The block is its list items and their indented continuation
         * lines. Anything at the left margin ends it, so a path an
         * agent quotes further down the message does not sprout a
         * preview of its own.
         *
         * Leaving on "no slash on this line" was wrong and cost the
         * first preview: every entry starts with a name line, which has
         * no slash, and that ended the block before the path beneath it
         * was ever looked at.
         */
        if (lines[i][0] != '\0' && lines[i][0] != ' ' &&
            lines[i][0] != '-' && lines[i][0] != '\t') {
            in_block = FALSE;
            continue;
        }

        /*
         * An agent's attachment is `clawt:<id>`, not a path: the file
         * lives in the daemon's keeping and may be on another machine.
         * Fetched to a local cache and then treated exactly like one the
         * operator attached, so there is one preview path rather than
         * two to disagree.
         */
        start = strstr(lines[i], "clawt:");

        if (start != NULL) {
            g_autofree gchar *id = g_strdup(start + strlen("clawt:"));

            g_strchomp(id);
            candidate = fetch_attachment(self, id);

            if (candidate == NULL)
                continue;
        } else {
            start = strchr(lines[i], '/');

            if (start == NULL)
                continue;

            candidate = g_strdup(start);
            g_strchomp(candidate);
        }

        /* The container path is in brackets; the host one is not. */
        if (g_str_has_suffix(candidate, ")"))
            continue;

        if (!g_file_test(candidate, G_FILE_TEST_EXISTS))
            continue;

        /*
         * Anything that is not an image gets a chip instead of a
         * thumbnail: a name, a size and the same menu. A PDF in a
         * conversation was previously a path and nothing else.
         */
        if (!looks_like_an_image(candidate)) {
            append_file_chip(self, row, candidate, from_user);
            continue;
        }

        /*
         * Scaled on load rather than shrunk in the widget.
         *
         * A size request is a *minimum*: asking for 160 high and
         * handing GtkPicture a 2000-pixel screenshot gets a 2000-pixel
         * screenshot. Decoding straight to thumbnail size also means a
         * transcript full of images does not hold every one of them in
         * memory at full resolution.
         */
        /*
         * Scaled during the decode, not by the widget.
         *
         * GTK has no maximum-size property: a size request is a
         * *minimum*, and a GtkPicture takes its natural size from the
         * paintable, so handing it a full screenshot gives a full-size
         * thumbnail however the widget is configured. Decoding straight
         * to thumbnail size settles it, and a transcript full of images
         * then does not hold every one at full resolution either.
         */
        {
            g_autoptr(GdkPixbuf) scaled = NULL;
            g_autoptr(GdkTexture) texture = NULL;
            g_autoptr(GBytes) pixels = NULL;
            g_autoptr(GError) error = NULL;

            /*
             * Big enough to actually see, the way a chat client shows
             * one. A hundred and sixty pixels was a postage stamp.
             */
            scaled = gdk_pixbuf_new_from_file_at_scale(candidate, 460, 320,
                                                        TRUE, &error);

            if (scaled == NULL) {
                /* Not an image after all, or one we cannot decode. */
                g_info("no preview for %s: %s", candidate, error->message);
                continue;
            }

            /*
             * A memory texture from the pixbuf's own pixels.
             * gdk_texture_new_for_pixbuf() would say this in one line
             * and is deprecated.
             */
            pixels = g_bytes_new(gdk_pixbuf_get_pixels(scaled),
                                 gdk_pixbuf_get_byte_length(scaled));
            texture = gdk_memory_texture_new(
                gdk_pixbuf_get_width(scaled),
                gdk_pixbuf_get_height(scaled),
                gdk_pixbuf_get_has_alpha(scaled)
                    ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8,
                pixels, (gsize)gdk_pixbuf_get_rowstride(scaled));

            picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
        }

        /*
         * can-shrink off.
         *
         * It defaults on, which makes a GtkPicture's *minimum* width
         * zero -- so anything in the ancestry doing a height-for-width
         * pass is free to squeeze it to nothing, and it did: the
         * thumbnail rendered a few dozen pixels wide. The texture is
         * already exactly the size it should be drawn at, so shrinking
         * it is never the right answer.
         */
        gtk_picture_set_can_shrink(GTK_PICTURE(picture), FALSE);
        gtk_widget_set_valign(picture, GTK_ALIGN_START);

        gtk_widget_set_halign(picture,
                              from_user ? GTK_ALIGN_END : GTK_ALIGN_START);

        button = gtk_button_new();
        gtk_button_set_child(GTK_BUTTON(button), picture);
        gtk_widget_add_css_class(button, "flat");
        gtk_widget_set_halign(button,
                              from_user ? GTK_ALIGN_END : GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(button, "Click to see it full size");
        g_object_set_data_full(G_OBJECT(button), "path",
                               g_strdup(candidate), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(on_preview_clicked),
                         self);

        add_context_menu(self, button, attachment_menu,
                         G_N_ELEMENTS(attachment_menu),
                         on_attachment_action,
                         g_object_get_data(G_OBJECT(button), "path"));

        gtk_box_append(GTK_BOX(row), button);
    }
}


/*
 * The chat column's geometry, in one place.
 *
 * Every row in the transcript is inset from the clamp by CHAT_ROW_MARGIN
 * on both sides, and an agent's body is indented past its avatar by a
 * further CHAT_GUTTER.  CHAT_BODY_INSET is where a body therefore
 * starts, and the composer below uses it so that the entry's frame and
 * the text above it stand on the same line.
 *
 * They are constants rather than three literals because the composer is
 * the only thing here that has to agree with a number it does not draw:
 * a literal 56 would be a number nobody could trace back to the two it
 * came from, and it would go stale the first time either changed.
 */
#define CHAT_ROW_MARGIN  12
#define CHAT_GUTTER      44
#define CHAT_BODY_INSET  (CHAT_ROW_MARGIN + CHAT_GUTTER)

/*
 * "Today", "Yesterday", or "Wednesday 25 August".
 *
 * A date change is a bigger break than a speaker change, so it gets more
 * room than the gap it sits among: 24 above, and the run header below it
 * drops to 6 so the divider belongs to the block it labels.
 */
static GtkWidget *
day_divider(GDateTime *when)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *left = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *right = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *label;
    g_autofree gchar *text = clawt_chat_day_label(when, NULL);

    label = gtk_label_new(text);
    gtk_widget_add_css_class(label, "caption");
    gtk_widget_add_css_class(label, "dim-label");

    gtk_widget_set_valign(left, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_widget_set_hexpand(right, TRUE);

    gtk_box_append(GTK_BOX(row), left);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), right);

    gtk_widget_set_margin_start(row, CHAT_ROW_MARGIN);
    gtk_widget_set_margin_end(row, CHAT_ROW_MARGIN);
    gtk_widget_set_margin_top(row, 24);

    return row;
}

/*
 * The face beside a run's first message.
 *
 * AdwAvatar derives both the initials and a colour from the text it is
 * given, so identity costs one widget, no palette and no hashing scheme
 * of ours -- which is why an avatar could ship before either of the two
 * config keys was wired up, and why neither is a prerequisite.
 *
 * `agents.avatar` wins when it loads, then `agents.color`, then the
 * derived colour.  A file that is missing or unreadable falls through
 * rather than producing an empty circle: the fallback is already a
 * complete answer, so there is nothing to report.
 */
/*
 * A style class painting an avatar in one configured colour.
 *
 * One provider on the display carrying a rule per colour, rather than a
 * provider per widget: gtk_style_context_add_provider() is deprecated,
 * and adding one provider per avatar would leave a sheet on the display
 * for every message ever drawn.  The class name is derived from the
 * colour, so two agents sharing one produce one rule and a colour that
 * has already been seen costs a hash lookup.
 *
 * @color reached clawt_color_ink() before this, which is what makes it
 * safe to splice: nothing but `#rgb` and `#rrggbb` gets this far.
 */
static GHashTable     *avatar_tints = NULL;
static GtkCssProvider *avatar_tint_provider = NULL;

static gchar *
tint_class(const gchar *color, const gchar *ink)
{
    gchar *name = g_strdup_printf("clawt-tint-%s", color + 1);

    if (avatar_tints == NULL)
        avatar_tints = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, g_free);

    if (!g_hash_table_contains(avatar_tints, name)) {
        g_autoptr(GString) sheet = g_string_new(NULL);
        GHashTableIter iter;
        gpointer key;
        gpointer value;

        g_hash_table_insert(avatar_tints, g_strdup(name),
                            g_strdup_printf("%s %s", color, ink));

        g_hash_table_iter_init(&iter, avatar_tints);

        while (g_hash_table_iter_next(&iter, &key, &value)) {
            g_auto(GStrv) pair = g_strsplit(value, " ", 2);

            g_string_append_printf(
                sheet,
                "avatar.%s { background-image: none; background-color: %s; "
                "color: %s; }\n",
                (const gchar *)key, pair[0], pair[1]);
        }

        if (avatar_tint_provider == NULL) {
            avatar_tint_provider = gtk_css_provider_new();
            gtk_style_context_add_provider_for_display(
                gdk_display_get_default(),
                GTK_STYLE_PROVIDER(avatar_tint_provider),
                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        }

        gtk_css_provider_load_from_string(avatar_tint_provider, sheet->str);
    }

    return name;
}

static GtkWidget *
run_avatar(const gchar *name, const gchar *image_path, const gchar *color)
{
    GtkWidget *avatar = adw_avatar_new(32, name, TRUE);
    const gchar *ink;

    if (image_path != NULL && *image_path != '\0') {
        g_autoptr(GdkTexture) texture =
            gdk_texture_new_from_filename(image_path, NULL);

        if (texture != NULL) {
            adw_avatar_set_custom_image(ADW_AVATAR(avatar),
                                        GDK_PAINTABLE(texture));
            return avatar;
        }
    }

    /*
     * A colour somebody typed into a YAML file, so it is checked before
     * it is spliced into a stylesheet -- clawt_color_ink() refuses
     * anything that is not #rgb or #rrggbb, and answers which of black
     * or white is legible on it.  Nothing else validates this key.
     */
    ink = clawt_color_ink(color);

    if (ink != NULL) {
        g_autofree gchar *class_name = tint_class(color, ink);

        gtk_widget_add_css_class(avatar, class_name);
    }

    return avatar;
}

/*
 * The task and the hop count, which belong to a message rather than to a
 * run.
 *
 * The Flow tab has had both since it was written and the chat has had
 * neither, which is backwards: a delegated reply arriving in your own
 * chat is exactly the one you want to know was delegated, and a hop
 * count climbing towards max_hops is the only thing on screen that
 * distinguishes a loop from a conversation.  The web client already
 * drew both in its chat, so this also closes an asymmetry between the
 * two clients that `make parity` cannot see.
 */
static void
append_message_chips(ClawtWindow *self, GtkWidget *into, const gchar *task,
                     gint64 depth)
{
    if (task != NULL) {
        g_autofree gchar *chip = g_strdup_printf("task %.8s", task);
        GtkWidget *button = gtk_button_new_with_label(chip);

        gtk_widget_add_css_class(button, "flat");
        gtk_widget_add_css_class(button, "caption");
        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(button, task);
        g_signal_connect(button, "clicked", G_CALLBACK(on_flow_task_clicked),
                         self);
        gtk_box_append(GTK_BOX(into), button);
    }

    /*
     * From the second hop on.  Every ordinary message is one hop, and a
     * "hop 1" on all of them would make the number stop being read.
     */
    if (depth > 1) {
        g_autofree gchar *hops = g_strdup_printf("hop %" G_GINT64_FORMAT,
                                                 depth);
        GtkWidget *chip = gtk_label_new(hops);

        gtk_widget_add_css_class(chip, "caption");
        gtk_widget_add_css_class(chip, "dim-label");
        gtk_widget_set_tooltip_text(
            chip,
            "How far this is from the request that started it. "
            "A count that keeps climbing is a loop; "
            "orchestration.max_hops is where it stops.");
        gtk_box_append(GTK_BOX(into), chip);
    }
}

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

static void
append_message_to(ClawtWindow *self, const TranscriptView *view,
                  const gchar *sender, const gchar *body, gboolean from_user,
                  gint64 ts, const gchar *task, gint64 depth)
{
    static const MenuEntry message_menu[] = {
        { "Copy",             "copy-markdown" },
        { "Copy as text",     "copy-text" },
        { "Copy as org",      "copy-org" }
    };
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *text = gtk_label_new(NULL);
    g_autoptr(GDateTime) when = (ts > 0)
        ? g_date_time_new_from_unix_local(ts)
        : g_date_time_new_now_local();
    g_autofree gchar *day = g_date_time_format(when, "%Y-%m-%d");
    g_autofree gchar *stamp = g_date_time_format(when, "%H:%M");
    gboolean new_day;
    gboolean run_start = clawt_chat_run_is_start(*view->run_sender,
                                                 *view->run_day, sender, day,
                                                 &new_day);

    if (new_day)
        gtk_box_append(view->into, day_divider(when));

    g_free(*view->run_day);
    *view->run_day = g_steal_pointer(&day);
    g_free(*view->run_sender);
    *view->run_sender = g_strdup(sender);

    /*
     * Rendered from markdown, never *as* markup.
     *
     * clawt_markdown_to_pango() emits markup only for the structure
     * cmark identified and escapes every literal on the way out, so an
     * agent writing "<span foreground=...>" gets those characters on
     * screen rather than a message that repaints the interface around
     * it.
     */
    set_label_markdown(GTK_LABEL(text), body);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_widget_set_halign(text, GTK_ALIGN_START);

    /*
     * Full contrast for both speakers.
     *
     * `accent` on the operator's body measured 5.82:1 against the
     * background where the agent's `body` measured 12.22:1 -- so the
     * operator's own words were rendered at less than half the contrast
     * of everything they were reading, all day.  It clears WCAG AA, so
     * this is not an accessibility failure; it is simply the wrong text
     * to make harder to read.
     */
    gtk_widget_add_css_class(text, "body");

    if (from_user) {
        /*
         * The operator's turns are bubbles, and only the operator's.
         *
         * An agent's turn runs to dozens of lines with headings, lists
         * and code blocks.  A container that long stops reading as a
         * message and starts reading as a panel, and a bubble wide
         * enough to read as a bubble is too wide to have a measure --
         * the two constraints pull opposite ways and only one side of
         * this conversation is short enough to satisfy both.  So the
         * bubble goes where it works, and the asymmetry is the thing
         * that says who is speaking at a glance.
         *
         * This is also why an earlier attempt at right alignment did
         * nothing: a bare wrapping label is allocated its natural width,
         * so GTK_ALIGN_END moved no body long enough to fill the column.
         * A bubble has a width of its own to be aligned.
         */
        GtkWidget *bubble = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

        gtk_label_set_max_width_chars(GTK_LABEL(text), 59);
        gtk_box_append(GTK_BOX(bubble), text);
        gtk_widget_add_css_class(bubble, "clawt-bubble");
        gtk_widget_set_halign(bubble, GTK_ALIGN_END);

        /*
         * Within a run the second and later bubbles drop their top-right
         * corner, which is what makes a run read as one utterance rather
         * than a stack.
         */
        gtk_widget_add_css_class(bubble,
                                 run_start ? "clawt-bubble-start"
                                           : "clawt-bubble-cont");

        if (run_start) {
            /*
             * The time once, above the first bubble.  No name and no
             * avatar: it is always the same person, the alignment
             * already said so, and a face on every one of your own
             * messages carries no information while costing the side
             * that can least afford it.
             */
            GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
            GtkWidget *at = gtk_label_new(stamp);

            gtk_widget_add_css_class(at, "caption");
            gtk_widget_add_css_class(at, "dim-label");
            gtk_widget_set_halign(line, GTK_ALIGN_END);
            gtk_widget_set_margin_bottom(line, 2);
            append_message_chips(self, line, task, depth);
            gtk_box_append(GTK_BOX(line), at);
            gtk_box_append(GTK_BOX(row), line);
        }

        gtk_box_append(GTK_BOX(row), bubble);
    } else {
        /*
         * The agent's side: one header per run, and every body in the
         * run indented to the same 44px so the left edge of the text is
         * unbroken down it.
         */
        GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        GtkWidget *gutter = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);

        if (run_start) {
            GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
            GtkWidget *who = gtk_label_new(sender);
            GtkWidget *at = gtk_label_new(stamp);
            GtkWidget *avatar = run_avatar(sender, view->avatar,
                                           view->color);

            /*
             * `heading` rather than `caption-heading`: shrinking the
             * name to caption size makes every turn look like metadata
             * about a message rather than a person saying something.
             */
            gtk_widget_add_css_class(who, "heading");
            gtk_widget_set_margin_start(who, 12);

            gtk_widget_add_css_class(at, "caption");
            gtk_widget_add_css_class(at, "dim-label");
            gtk_widget_set_margin_start(at, 8);

            gtk_widget_set_valign(avatar, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(header), avatar);
            gtk_box_append(GTK_BOX(header), who);
            gtk_box_append(GTK_BOX(header), at);
            append_message_chips(self, header, task, depth);
            gtk_widget_set_margin_bottom(header, 2);
            gtk_box_append(GTK_BOX(row), header);
        }

        /*
         * A real 44px slot rather than a margin on the label.  Avatar
         * plus its 12px gap; a widget is something a narrow layout could
         * collapse, and a margin set from C is not.
         */
        gtk_widget_set_size_request(gutter, CHAT_GUTTER, -1);
        gtk_box_append(GTK_BOX(line), gutter);
        gtk_box_append(GTK_BOX(line), text);
        gtk_widget_set_hexpand(text, TRUE);
        gtk_box_append(GTK_BOX(row), line);
    }

    append_attachment_previews(self, row, body, from_user);

    /*
     * The body travels with the row, so the menu can copy what was
     * actually said rather than what the label happens to render.
     */
    g_object_set_data_full(G_OBJECT(row), "body", g_strdup(body), g_free);
    add_context_menu(self, row, message_menu, G_N_ELEMENTS(message_menu),
                     on_message_action,
                     g_object_get_data(G_OBJECT(row), "body"));
    gtk_widget_set_margin_start(row, CHAT_ROW_MARGIN);
    gtk_widget_set_margin_end(row, CHAT_ROW_MARGIN);

    /*
     * Turns have to be further apart than the paragraphs inside one.
     *
     * They were not, and that -- not the 6px -- is why the transcript
     * read as a single wall.  A message's own paragraph break is a
     * literal blank line: clawt_markdown_to_pango() separates blocks
     * with \n\n (src/chat/clawt-markdown.c), and a blank line costs a
     * whole line height.  Measured off a rendered window at 1280px, ink
     * to ink: 11px between lines of one paragraph, 27px between two
     * paragraphs of one message, and 21px between two speakers.
     * Proximity was inverted -- one person's paragraphs sat further
     * apart than two different people's turns did.
     *
     * 30 is the smallest step on the HIG's 6px grid that beats that
     * 27px, and it was measured rather than derived: 18 was tried first,
     * because it is the HIG's own step for separating groups, and it
     * still lost.  The run redesign specified 18 again for run-to-run;
     * the measurement has not changed and neither has the paragraph gap,
     * so the measured number stands.
     *
     * Inside a run it is 6, and after a day divider 6 as well -- the
     * divider already carries 24 above it, and a run header adding 30 to
     * that would put the date adrift between two blocks instead of
     * belonging to the one beneath it.
     */
    gtk_widget_set_margin_top(row, !run_start ? 6 : (new_day ? 6 : 30));

    gtk_box_append(view->into, row);
}

/*
 * The chat transcript, which is the common case.
 */
static void
append_message(ClawtWindow *self, const gchar *sender, const gchar *body,
               gboolean from_user, gint64 ts)
{
    TranscriptView view = { self->transcript, &self->run_sender,
                            &self->run_day, self->selected_avatar,
                            self->selected_color };

    append_message_to(self, &view, sender, body, from_user, ts, NULL, 0);
}

/*
 * Schedules a scroll that cannot outlive the window.
 *
 * A plain g_idle_add(self) runs after the window has been destroyed if
 * the user closes it in the same turn a message arrives, and the
 * callback then reads freed memory.  Holding a reference for the life of
 * the idle costs nothing and removes the race.
 */
static gboolean scroll_to_bottom(gpointer user_data);

static void
queue_scroll(ClawtWindow *self)
{
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, scroll_to_bottom,
                    g_object_ref(self), g_object_unref);
}

/*
 * Scrolls to the bottom, but only when the reader was already there.
 *
 * Yanking somebody down mid-read because a message arrived is the single
 * most annoying thing a chat window can do.
 */
static gboolean
scroll_to_bottom(gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkAdjustment *adjustment;

    if (!self->following || self->transcript_scroll == NULL)
        return G_SOURCE_REMOVE;

    adjustment = gtk_scrolled_window_get_vadjustment(self->transcript_scroll);
    gtk_adjustment_set_value(adjustment,
                             gtk_adjustment_get_upper(adjustment) -
                             gtk_adjustment_get_page_size(adjustment));

    return G_SOURCE_REMOVE;
}

/*
 * Pins the view to the bottom when the transcript actually grows.
 *
 * The queued idle is not enough on its own: it reads `upper` before
 * GTK's frame clock has laid the new message out, so it scrolls to where
 * the bottom *was* and leaves the newest message just below the fold --
 * which is why a reply, or even typing, needed a scroll by hand. The
 * adjustment says when it genuinely knows how tall the content is; that
 * is the moment to follow it.
 *
 * page-size as well as upper, because typing grows the composer and
 * shrinks the transcript above it, which moves the bottom without adding
 * anything.
 */
static void
on_transcript_grew(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkAdjustment *adjustment = GTK_ADJUSTMENT(object);
    gdouble bottom;

    (void)pspec;

    if (!self->following)
        return;

    bottom = gtk_adjustment_get_upper(adjustment) -
             gtk_adjustment_get_page_size(adjustment);

    /*
     * Only when it is not already there. Setting the same value is a
     * no-op to GTK but this runs on every layout pass, and doing nothing
     * is cheaper than asking it to.
     */
    if (gtk_adjustment_get_value(adjustment) < bottom)
        gtk_adjustment_set_value(adjustment, bottom);
}

/*
 * The only place `following` changes, and the reason the two unread
 * affordances cannot disagree.
 *
 * `following` false means the reader is deliberately somewhere above the
 * live edge, and the client already refuses to move the view for them --
 * see scroll_to_bottom().  That refusal is right; saying nothing about
 * it was not.  Two things say it: a pill floating over the transcript,
 * and a rule drawn in the transcript at the point reading stopped.
 *
 * Both are cleared by the same false -> true edge, so every path that
 * already re-arms following clears them with no new cases: reaching the
 * bottom by hand, sending a message, switching agent, /clear, and the
 * pill's own click.
 */
static void
set_following(ClawtWindow *self, gboolean following)
{
    self->following = following;

    if (!following)
        return;

    if (self->unread_marker != NULL) {
        gtk_box_remove(self->transcript, self->unread_marker);
        self->unread_marker = NULL;
    }

    if (self->jump_revealer != NULL) {
        gtk_revealer_set_reveal_child(self->jump_revealer, FALSE);

        /*
         * A GtkRevealer keeps its allocation while its child is hidden,
         * so an overlay child that is merely not revealed can still take
         * the clicks meant for the transcript underneath it.  Dropping
         * can-target with the reveal removes that without having to
         * reason about which transition does what.
         */
        gtk_widget_set_can_target(GTK_WIDGET(self->jump_revealer), FALSE);
    }
}

/*
 * The rule drawn where reading stopped.
 *
 *   ---------------------  New messages  ---------------------
 *
 * It stores nothing: no read receipt, no per-agent position, nothing on
 * disk.  It is exactly as durable as `following` itself, which is what
 * lets it promise something the client can always keep -- "these arrived
 * while you were up there" -- rather than "this is where you left off",
 * which would need state across restarts to be true.
 *
 * `accent` on the label rather than a colour, so the palette work
 * redefines it for free; the separators keep the platform colour,
 * because the label is the message and the rules are only the ruling.
 */
static GtkWidget *
unread_marker_new(void)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *before = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *label = gtk_label_new("New messages");
    GtkWidget *after = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

    gtk_widget_set_hexpand(before, TRUE);
    gtk_widget_set_valign(before, GTK_ALIGN_CENTER);
    gtk_widget_set_hexpand(after, TRUE);
    gtk_widget_set_valign(after, GTK_ALIGN_CENTER);

    gtk_widget_add_css_class(label, "caption-heading");
    gtk_widget_add_css_class(label, "accent");

    gtk_box_append(GTK_BOX(row), before);
    gtk_box_append(GTK_BOX(row), label);
    gtk_box_append(GTK_BOX(row), after);

    gtk_widget_set_margin_start(row, CHAT_ROW_MARGIN);
    gtk_widget_set_margin_end(row, CHAT_ROW_MARGIN);

    /*
     * Closer to the message below than to the one above, on purpose: the
     * marker labels the block underneath it, so it should belong to it.
     */
    gtk_widget_set_margin_top(row, 18);

    return row;
}

/*
 * Something arrived that the reader is not looking at.
 *
 * Called only from the event path, so replayed history and the client's
 * own local output can never trip it -- neither of those is an arrival.
 * At most one marker is ever alive, because the second arrival in a run
 * belongs under the same rule as the first.
 */
static void
note_arrival(ClawtWindow *self)
{
    if (self->following || self->unread_marker != NULL)
        return;

    self->unread_marker = unread_marker_new();
    gtk_box_append(self->transcript, self->unread_marker);

    if (self->jump_revealer != NULL) {
        gtk_widget_set_can_target(GTK_WIDGET(self->jump_revealer), TRUE);
        gtk_revealer_set_reveal_child(self->jump_revealer, TRUE);
    }
}

/*
 * Emptying the transcript, rather than clear_box() on its own.
 *
 * The marker is a borrowed pointer into that box, so clearing the box
 * behind its back leaves it dangling and the next false -> true edge
 * removes a widget that is already gone.  Re-arming here is also what
 * keeps load_history()'s replay from drawing a "New messages" rule at
 * the top of a freshly loaded transcript.
 */
static void
reset_transcript(ClawtWindow *self)
{
    self->unread_marker = NULL;
    g_clear_pointer(&self->run_sender, g_free);
    g_clear_pointer(&self->run_day, g_free);
    clear_box(self->transcript);
    set_following(self, TRUE);
}

static void
on_jump_to_latest(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)button;

    /*
     * Jump, do not animate.  GTK4 has no adjustment animation, and a
     * scroll through several screens of text disorients rather than
     * orients.  set_following() removes the marker, which shrinks the
     * content, so the scroll is queued rather than computed here --
     * on_transcript_grew() lands it on the real bottom afterwards.
     */
    set_following(self, TRUE);
    queue_scroll(self);
}

static void
on_scrolled(GtkAdjustment *adjustment, gpointer user_data)
{
    ClawtWindow *self = user_data;

    /*
     * The predicate is clawt_transcript_is_at_bottom(), in libclawt, so
     * the tolerance can be exercised on both sides and at its boundary
     * without a window -- which is the one part of the follow machinery
     * a test could not otherwise reach.
     */
    set_following(self, clawt_transcript_is_at_bottom(
                            gtk_adjustment_get_value(adjustment),
                            gtk_adjustment_get_upper(adjustment),
                            gtk_adjustment_get_page_size(adjustment)));
}

/*
 * Whether this message is already on screen, remembering it if not.
 *
 * Answers the replay case: a client that has just connected is sent the
 * recent events as well as loading the history they are already in.
 */
static gboolean
already_shown(ClawtWindow *self, const gchar *id)
{
    if (id == NULL)
        return FALSE;

    if (g_hash_table_contains(self->shown, id))
        return TRUE;

    g_hash_table_add(self->shown, g_strdup(id));
    return FALSE;
}

/*
 * The message being composed.
 *
 * A GtkTextView rather than a GtkEntry, because Ctrl+G hands the box
 * whatever came back from $EDITOR and that is usually several
 * paragraphs -- an entry is single-line and draws every newline as a
 * control picture, so the one feature that exists to write something
 * long made it unreadable.
 */
static gchar *
entry_text(ClawtWindow *self)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->entry);
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);

    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void
entry_set_text(ClawtWindow *self, const gchar *text)
{
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(self->entry),
                             text != NULL ? text : "", -1);
}

static void
entry_focus_end(ClawtWindow *self)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(self->entry);
    GtkTextIter end;

    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_place_cursor(buffer, &end);
    gtk_widget_grab_focus(GTK_WIDGET(self->entry));
}

/* ── Attachments ─────────────────────────────────────────────────── */

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

static void
attachment_free(Attachment *attachment)
{
    if (attachment == NULL)
        return;

    g_free(attachment->name);
    g_clear_pointer(&attachment->bytes, g_bytes_unref);
    g_free(attachment);
}

static void refresh_attachment_strip(ClawtWindow *self);

static void
on_drop_attachment(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    Attachment *attachment = g_object_get_data(G_OBJECT(button), "attachment");

    g_ptr_array_remove(self->pending, attachment);
    refresh_attachment_strip(self);
}

/* A row of chips above the entry, one per queued file. */
static void
refresh_attachment_strip(ClawtWindow *self)
{
    GtkWidget *child;
    guint i;

    while ((child = gtk_widget_get_first_child(self->attachments)) != NULL)
        gtk_box_remove(GTK_BOX(self->attachments), child);

    gtk_widget_set_visible(self->attachments, self->pending->len > 0);

    for (i = 0; i < self->pending->len; i++) {
        Attachment *attachment = g_ptr_array_index(self->pending, i);
        GtkWidget *chip = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *label;
        GtkWidget *drop;
        g_autofree gchar *text = g_strdup_printf(
            "%s (%" G_GSIZE_FORMAT " KB)", attachment->name,
            (g_bytes_get_size(attachment->bytes) + 1023) / 1024);

        label = gtk_label_new(text);
        gtk_widget_add_css_class(label, "caption");
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(label), 28);

        drop = gtk_button_new_from_icon_name("window-close-symbolic");
        gtk_widget_add_css_class(drop, "flat");
        gtk_widget_add_css_class(drop, "circular");
        gtk_widget_set_tooltip_text(drop, "Do not send this one");
        g_object_set_data(G_OBJECT(drop), "attachment", attachment);
        g_signal_connect(drop, "clicked", G_CALLBACK(on_drop_attachment),
                         self);

        gtk_widget_add_css_class(chip, "card");
        gtk_box_append(GTK_BOX(chip), label);
        gtk_box_append(GTK_BOX(chip), drop);
        gtk_box_append(GTK_BOX(self->attachments), chip);
    }
}

static void
queue_attachment(ClawtWindow *self, const gchar *name, GBytes *bytes)
{
    Attachment *attachment;

    if (bytes == NULL || g_bytes_get_size(bytes) == 0)
        return;

    /*
     * Bounded, because this crosses the IPC socket base64-encoded and a
     * client that queues a 400 MB video would block the daemon's main
     * context for as long as it takes to decode.
     */
    if (g_bytes_get_size(bytes) > 32u * 1024u * 1024u) {
        clawt_window_toast(self, "that file is over 32 MB; put it in a "
                                 "shared folder instead");
        return;
    }

    attachment = g_new0(Attachment, 1);
    attachment->name = g_strdup(name);
    attachment->bytes = g_bytes_ref(bytes);

    g_ptr_array_add(self->pending, attachment);
    refresh_attachment_strip(self);
}

static void
on_texture_pasted(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GdkTexture) texture = NULL;
    g_autoptr(GBytes) png = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *name = NULL;

    texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source), result,
                                                &error);

    if (texture == NULL) {
        if (error != NULL &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            clawt_window_toast(self, error->message);

        return;
    }

    /*
     * PNG, because a pasted screenshot is a texture with no file behind
     * it and every model that reads images reads PNG.
     */
    png = gdk_texture_save_to_png_bytes(texture);

    name = g_strdup_printf("pasted-%" G_GINT64_FORMAT ".png",
                           g_get_real_time() / G_USEC_PER_SEC);

    queue_attachment(self, name, png);
    g_object_unref(self);
}

/*
 * Whether the clipboard has an image, and if so take it.
 *
 * Returns %TRUE when the paste was handled here, so the entry does not
 * also paste whatever text representation the source offered -- an
 * image copied from a browser usually carries a URL alongside it, and
 * getting both is worse than getting either.
 */
static gboolean
paste_image(ClawtWindow *self)
{
    GdkClipboard *clipboard = gtk_widget_get_clipboard(GTK_WIDGET(self));
    GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);

    if (!gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE))
        return FALSE;

    gdk_clipboard_read_texture_async(clipboard, NULL, on_texture_pasted,
                                     g_object_ref(self));
    return TRUE;
}

static void
on_files_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GListModel) files = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    files = gtk_file_dialog_open_multiple_finish(GTK_FILE_DIALOG(source),
                                                 result, &error);

    if (files == NULL) {
        /* Dismissing the dialog is not a failure worth a toast. */
        if (error != NULL &&
            !g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
            clawt_window_toast(self, error->message);

        g_object_unref(self);
        return;
    }

    for (i = 0; i < g_list_model_get_n_items(files); i++) {
        g_autoptr(GFile) file = g_list_model_get_item(files, i);
        g_autofree gchar *name = g_file_get_basename(file);
        g_autofree gchar *contents = NULL;
        g_autoptr(GError) read_error = NULL;
        gsize length = 0;

        if (!g_file_load_contents(file, NULL, &contents, &length, NULL,
                                  &read_error)) {
            clawt_window_toast(self, read_error->message);
            continue;
        }

        {
            g_autoptr(GBytes) bytes = g_bytes_new(contents, length);

            queue_attachment(self, name, bytes);
        }
    }

    g_object_unref(self);
}

static void
on_attach_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();

    (void)button;

    gtk_file_dialog_set_title(dialog, "Send with this message");
    gtk_file_dialog_open_multiple(dialog, GTK_WINDOW(self), NULL,
                                  on_files_chosen, g_object_ref(self));
}

/*
 * Hands the queued files to the daemon and describes them in the body.
 *
 * The daemon writes them into the agent's exchange directory and says
 * what path the *agent* should use, which is not the host path when the
 * agent lives in a container. Naming them in the message is what makes
 * them reachable: an agent reads files with its own tools, and one it
 * has not been told about is one it will not open.
 *
 * Returns: (transfer full): the body to actually send
 */
static gchar *
body_with_attachments(ClawtWindow *self, const gchar *body)
{
    g_autoptr(GString) out = NULL;
    guint i;

    if (self->pending->len == 0)
        return g_strdup(body);

    out = g_string_new(body);

    if (out->len > 0)
        g_string_append(out, "\n\n");

    g_string_append(out,
                    "[clawtilla] Files sent with this message. Open them "
                    "with your own read tool at the host path below; it "
                    "runs on the host even when your shell does not.\n");

    for (i = 0; i < self->pending->len; i++) {
        Attachment *attachment = g_ptr_array_index(self->pending, i);
        g_autoptr(JsonNode) reply = NULL;
        g_autofree gchar *encoded = NULL;
        gsize length = 0;
        const guchar *data = g_bytes_get_data(attachment->bytes, &length);

        encoded = g_base64_encode(data, length);

        reply = clawt_window_request(
            self, "attachment.put",
            clawt_build_payload("agent", self->selected_agent,
                                "name", attachment->name,
                                "data", encoded, NULL));

        if (reply == NULL) {
            g_string_append_printf(out, "- %s (could not be saved)\n",
                                   attachment->name);
            continue;
        }

        {
            JsonObject *result = clawt_payload_of(reply);
            const gchar *host = clawt_json_string(result, "host_path", "");
            const gchar *guest = clawt_json_string(result, "path", "");

            g_string_append_printf(out, "- %s\n  %s\n",
                                   attachment->name, host);

            /*
             * Both paths, and the host one first, because they are for
             * different tools.
             *
             * An agent's own read tool runs on the host even when its
             * shell runs in a container -- so given only the container
             * path it could stat the file with clawtilla_computer_exec
             * and never open it, which is exactly what happened to the
             * first image anybody sent.
             */
            if (g_strcmp0(host, guest) != 0)
                g_string_append_printf(
                    out, "  (inside your container: %s)\n", guest);
        }
    }

    g_ptr_array_set_size(self->pending, 0);
    refresh_attachment_strip(self);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/* ── Composing in $EDITOR ────────────────────────────────────────── */

/*
 * Ctrl+G: hand what is typed to $EDITOR, take back whatever comes out.
 *
 * A one-line GtkEntry is a bad place to write six paragraphs, and the
 * editor is where the person already knows how to write. The file is
 * seeded with the current text so this extends a draft rather than
 * replacing it.
 */
static void
on_compose_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autofree gchar *path = g_object_get_data(G_OBJECT(source), "path") != NULL
        ? g_strdup(g_object_get_data(G_OBJECT(source), "path")) : NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *written = NULL;

    if (!g_subprocess_wait_check_finish(G_SUBPROCESS(source), result,
                                        &error)) {
        clawt_window_toast(self, error->message);
        g_object_unref(self);
        return;
    }

    if (path != NULL && g_file_get_contents(path, &written, NULL, NULL)) {
        /*
         * Trailing newline stripped: every editor adds one, and it would
         * otherwise arrive as an empty line at the end of every message
         * composed this way.
         */
        g_strchomp(written);
        entry_set_text(self, written);
    }

    if (path != NULL)
        g_unlink(path);

    entry_focus_end(self);
    g_object_unref(self);
}

static void
compose_in_editor(ClawtWindow *self)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) parts = NULL;
    g_autofree gchar *path = NULL;
    const gchar *editor = editor_command();
    g_autofree gchar *current = NULL;
    gint fd;
    gint i;

    if (editor == NULL) {
        clawt_window_toast(self, "no editor found; set $EDITOR or $VISUAL");
        return;
    }

    /*
     * .md, so an editor that picks a mode by extension gives you one
     * suited to prose rather than to nothing at all.
     */
    fd = g_file_open_tmp("clawtilla-message-XXXXXX.md", &path, &error);

    if (fd < 0) {
        clawt_window_toast(self, error->message);
        return;
    }

    close(fd);

    current = entry_text(self);

    if (!g_file_set_contents(path, current != NULL ? current : "", -1,
                             &error)) {
        clawt_window_toast(self, error->message);
        g_unlink(path);
        return;
    }

    if (!g_shell_parse_argv(editor, NULL, &parts, &error)) {
        clawt_window_toast(self, error->message);
        g_unlink(path);
        return;
    }

    for (i = 0; parts[i] != NULL; i++)
        g_ptr_array_add(argv, g_strdup(parts[i]));

    g_ptr_array_add(argv, g_strdup(path));
    g_ptr_array_add(argv, NULL);

    process = g_subprocess_newv((const gchar *const *)argv->pdata,
                                G_SUBPROCESS_FLAGS_NONE, &error);

    if (process == NULL) {
        clawt_window_toast(self, error->message);
        g_unlink(path);
        return;
    }

    g_object_set_data_full(G_OBJECT(process), "path", g_steal_pointer(&path),
                           g_free);
    g_subprocess_wait_check_async(process, NULL, on_compose_finished,
                                  g_object_ref(self));
}


/* ── Slash commands ──────────────────────────────────────────────── */

/*
 * What "/" offers.
 *
 * A chat entry is the one place a person always is, so the things they
 * do most often should be reachable without going and finding a tab.
 * Everything here is a shortcut to something that already exists --
 * none of it is a second way to do anything.
 */
typedef struct {
    const gchar *name;
    const gchar *argument;   /* (nullable) what to type after it */
    const gchar *summary;
} SlashCommand;

static const SlashCommand slash_commands[] = {
    { "/help",    NULL,      "list these commands" },
    { "/start",   NULL,      "start this agent" },
    { "/stop",    NULL,      "stop this agent" },
    { "/restart", NULL,      "restart this agent" },
    { "/attach",  NULL,      "pick files to send with the next message" },
    { "/compose", NULL,      "write the message in $EDITOR (same as Ctrl+G)" },
    { "/edit",    "[file]",  "open a workspace file in $EDITOR" },
    { "/files",   NULL,      "list this agent's workspace files" },
    { "/memory",  "<query>", "search what this agent has remembered" },
    { "/agents",  NULL,      "who is in the fleet" },
    { "/flow",    NULL,      "go to the conversations between agents" },
    { "/tasks",   NULL,      "go to the task board" },
    { "/reset",   NULL,      "start the agent's AI session again, from nothing" },
    { "/retry",   NULL,      "send your last message again" },
    { "/export",  "[org]",   "save the conversation: text, markdown or org" },
    { "/copy",    "[org]",   "copy the conversation: text, markdown or org" },
    { "/clear",   NULL,      "clear this transcript on screen only" },
    { "/new",     NULL,      "create an agent" }
};

/* A reply that came from the client, not from the agent. */
static void
append_local(ClawtWindow *self, const gchar *text)
{
    /*
     * Follow again, because the operator asked for this.
     *
     * scroll_to_bottom() and on_transcript_grew() both refuse to move
     * the view while `following` is false, which is right for a message
     * arriving on its own and wrong for output the operator just asked
     * to see.  on_send() re-arms following at its end, but it returns
     * early when the text was a slash command -- so with the view
     * scrolled up, /help, /export, /copy and /clear appended their
     * output below the fold and the queued scroll bailed on the first
     * line.  The command looked like it had done nothing.
     *
     * Re-arming here rather than in on_send() covers every caller: the
     * point is not which path ran, it is that the operator's own action
     * put this on screen and they should be looking at it.
     */
    set_following(self, TRUE);

    append_message(self, "clawtilla", text, FALSE, 0);
    queue_scroll(self);
}

static void
show_command_help(ClawtWindow *self)
{
    g_autoptr(GString) out = g_string_new(NULL);
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(slash_commands); i++) {
        g_string_append_printf(out, "%s%s%s\n    %s\n",
                               slash_commands[i].name,
                               slash_commands[i].argument != NULL ? " " : "",
                               slash_commands[i].argument != NULL
                                   ? slash_commands[i].argument : "",
                               slash_commands[i].summary);
    }

    g_string_append(out,
                    "\nCtrl+G writes the message in $EDITOR. Paste an image "
                    "or use /attach to send files.");

    append_local(self, out->str);
}

/*
 * Runs a slash command.
 *
 * Returns %TRUE when the text was a command and has been dealt with, so
 * the caller does not also send it to the agent -- a mistyped command
 * reaching the model as a message is how a person learns to distrust
 * the feature.
 */
static gboolean
run_slash_command(ClawtWindow *self, const gchar *text)
{
    g_auto(GStrv) parts = NULL;
    const gchar *name;
    const gchar *rest;
    gsize i;

    if (text == NULL || text[0] != '/')
        return FALSE;

    parts = g_strsplit(text, " ", 2);
    name = parts[0];
    rest = (parts[1] != NULL) ? g_strstrip(parts[1]) : NULL;

    for (i = 0; i < G_N_ELEMENTS(slash_commands); i++) {
        if (g_strcmp0(slash_commands[i].name, name) == 0)
            break;
    }

    if (i == G_N_ELEMENTS(slash_commands)) {
        g_autofree gchar *message = g_strdup_printf(
            "There is no %s. Type /help for the list.", name);

        append_local(self, message);
        return TRUE;
    }

    if (g_strcmp0(name, "/help") == 0) {
        show_command_help(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/clear") == 0) {
        reset_transcript(self);
        g_hash_table_remove_all(self->shown);
        append_local(self, "Cleared on screen. The transcript on disk is "
                           "untouched -- reopen this agent to see it again.");
        return TRUE;
    }

    if (g_strcmp0(name, "/compose") == 0) {
        entry_set_text(self, "");
        compose_in_editor(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/attach") == 0) {
        on_attach_clicked(NULL, self);
        return TRUE;
    }

    if (g_strcmp0(name, "/flow") == 0) {
        adw_view_stack_set_visible_child_name(self->pages, "flow");
        return TRUE;
    }

    if (g_strcmp0(name, "/tasks") == 0) {
        adw_view_stack_set_visible_child_name(self->pages, "tasks");
        return TRUE;
    }

    if (g_strcmp0(name, "/new") == 0) {
        on_new_agent(NULL, self);
        return TRUE;
    }

    if (self->selected_agent == NULL) {
        append_local(self, "Pick an agent first.");
        return TRUE;
    }

    if (g_strcmp0(name, "/start") == 0 || g_strcmp0(name, "/stop") == 0 ||
        g_strcmp0(name, "/restart") == 0) {
        g_autofree gchar *verb = g_strdup_printf("agent.%s", name + 1);
        g_autoptr(JsonNode) reply = NULL;

        reply = clawt_window_request(
            self, verb,
            clawt_build_payload("agent", self->selected_agent, NULL));

        if (reply != NULL) {
            g_autofree gchar *message = g_strdup_printf(
                "%s: %s requested.", self->selected_agent, name + 1);

            append_local(self, message);
        }

        refresh_agents(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/reset") == 0) {
        g_autoptr(JsonNode) reply = clawt_window_request(
            self, "agent.reset",
            clawt_build_payload("agent", self->selected_agent, NULL));

        if (reply != NULL) {
            JsonObject *result = clawt_payload_of(reply);
            g_autofree gchar *message = g_strdup_printf(
                "Session reset: %" G_GINT64_FORMAT " stored session%s "
                "cleared%s. The next thing you say starts a new one.",
                clawt_json_int(result, "sessions_cleared", 0),
                clawt_json_int(result, "sessions_cleared", 0) == 1 ? "" : "s",
                clawt_json_int(result, "restarted", 0) ? " and the agent "
                                                          "restarted" : "");

            append_local(self, message);
        }

        refresh_agents(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/retry") == 0) {
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *messages;
        const gchar *last = NULL;
        guint j;

        reply = clawt_window_request(
            self, "room.history",
            clawt_build_payload("room", self->selected_agent, "as", "user",
                                NULL));

        if (reply == NULL)
            return TRUE;

        messages = json_object_get_array_member(clawt_payload_of(reply),
                                                "messages");

        for (j = 0; j < json_array_get_length(messages); j++) {
            JsonObject *one = json_array_get_object_element(messages, j);

            if (g_strcmp0(clawt_json_string(one, "sender", ""), "user") == 0)
                last = clawt_json_string(one, "body", NULL);
        }

        if (last == NULL) {
            append_local(self, "You have not said anything to resend.");
            return TRUE;
        }

        /*
         * Put in the box rather than sent. Retry usually means "that
         * did not go how I wanted", and the chance to change a word
         * before it goes again is the point.
         */
        entry_set_text(self, last);
        entry_focus_end(self);
        return TRUE;
    }

    if (g_strcmp0(name, "/export") == 0 || g_strcmp0(name, "/copy") == 0) {
        g_autofree gchar *what = g_strdup_printf(
            "%s-%s", g_strcmp0(name, "/copy") == 0 ? "copy" : "save",
            (rest != NULL && rest[0] != '\0') ? rest : "markdown");

        on_conversation_action(self, what, NULL);
        return TRUE;
    }

    if (g_strcmp0(name, "/agents") == 0) {
        g_autoptr(JsonNode) reply = clawt_window_request(self, "agent.list",
                                                          NULL);
        g_autoptr(GString) out = g_string_new(NULL);
        JsonArray *agents;
        guint j;

        if (reply == NULL)
            return TRUE;

        agents = json_object_get_array_member(clawt_payload_of(reply),
                                              "agents");

        for (j = 0; j < json_array_get_length(agents); j++) {
            JsonObject *one = json_array_get_object_element(agents, j);

            g_string_append_printf(out, "%-20s %-10s %s\n",
                                   clawt_json_string(one, "id", "?"),
                                   clawt_json_string(one, "state", "?"),
                                   clawt_json_string(one, "description", ""));
        }

        append_local(self, out->str);
        return TRUE;
    }

    if (g_strcmp0(name, "/files") == 0 || g_strcmp0(name, "/edit") == 0) {
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *files;
        guint j;

        reply = clawt_window_request(
            self, "agent.files",
            clawt_build_payload("agent", self->selected_agent, NULL));

        if (reply == NULL)
            return TRUE;

        files = json_object_get_array_member(clawt_payload_of(reply),
                                             "files");

        if (g_strcmp0(name, "/files") == 0 || rest == NULL) {
            g_autoptr(GString) out = g_string_new(NULL);

            for (j = 0; j < json_array_get_length(files); j++) {
                JsonObject *file = json_array_get_object_element(files, j);

                g_string_append_printf(out, "%-18s %s\n",
                                       clawt_json_string(file, "name", "?"),
                                       clawt_json_string(file, "title", ""));
            }

            g_string_append(out, "\n/edit <name> opens one in $EDITOR.");
            append_local(self, out->str);
            return TRUE;
        }

        for (j = 0; j < json_array_get_length(files); j++) {
            JsonObject *file = json_array_get_object_element(files, j);

            if (g_strcmp0(clawt_json_string(file, "name", ""), rest) != 0)
                continue;

            /*
             * The daemon's path, never one built here: it owns where a
             * workspace is, and a client that constructs the path is a
             * client that can be pointed at somebody else's.
             */
            open_path_in_editor(self, clawt_json_string(file, "path", ""),
                                rest);
            return TRUE;
        }

        {
            g_autofree gchar *message = g_strdup_printf(
                "%s has no file called '%s'. /files lists them.",
                self->selected_agent, rest);

            append_local(self, message);
        }

        return TRUE;
    }

    if (g_strcmp0(name, "/memory") == 0) {
        g_autoptr(JsonNode) reply = NULL;
        g_autoptr(GString) out = g_string_new(NULL);
        JsonArray *memories;
        guint j;

        reply = clawt_window_request(
            self, rest != NULL ? "memory.search" : "memory.list",
            rest != NULL
                ? clawt_build_payload("agent", self->selected_agent,
                                      "query", rest, NULL)
                : clawt_build_payload("agent", self->selected_agent, NULL));

        if (reply == NULL)
            return TRUE;

        memories = json_object_get_array_member(clawt_payload_of(reply),
                                                "memories");

        if (json_array_get_length(memories) == 0) {
            append_local(self, "Nothing remembered matches that.");
            return TRUE;
        }

        for (j = 0; j < json_array_get_length(memories); j++) {
            JsonObject *memory = json_array_get_object_element(memories, j);
            const gchar *summary = clawt_json_string(memory, "summary", NULL);

            g_string_append_printf(out, "%s [%s]\n  %s\n",
                                   clawt_json_string(memory, "id", "?"),
                                   clawt_json_string(memory, "category", "?"),
                                   summary != NULL
                                       ? summary
                                       : clawt_json_string(memory, "content",
                                                           ""));
        }

        append_local(self, out->str);
        return TRUE;
    }

    return TRUE;
}

static void
on_command_row_selected(GtkListBox *list, GtkListBoxRow *row,
                        gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name;
    g_autofree gchar *filled = NULL;

    (void)list;

    if (row == NULL)
        return;

    name = g_object_get_data(G_OBJECT(row), "command");

    if (name == NULL)
        return;

    /*
     * A trailing space for a command that takes an argument, so the
     * next keystroke is the argument rather than a correction.
     */
    filled = g_strconcat(name,
                         g_object_get_data(G_OBJECT(row), "takes-argument")
                             != NULL ? " " : "", NULL);

    gtk_revealer_set_reveal_child(GTK_REVEALER(self->command_revealer),
                                  FALSE);
    entry_set_text(self, filled);
    entry_focus_end(self);
}

/*
 * Shows the matching commands as the person types "/".
 *
 * Discoverability, not completion: the list is there to be read, and
 * clicking one fills it in. Anybody who already knows the command just
 * keeps typing and never looks at it.
 */
static void
on_entry_changed(GtkTextBuffer *buffer, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autofree gchar *text = entry_text(self);
    guint matches = 0;
    gsize i;

    (void)buffer;

    /* GtkTextView has no placeholder of its own. */
    gtk_widget_set_visible(self->placeholder,
                           text == NULL || text[0] == '\0');

    if (text == NULL || text[0] != '/' || strchr(text, ' ') != NULL) {
        gtk_revealer_set_reveal_child(GTK_REVEALER(self->command_revealer),
                                      FALSE);
        return;
    }

    clear_list(self->command_list);

    for (i = 0; i < G_N_ELEMENTS(slash_commands); i++) {
        GtkWidget *row;
        g_autofree gchar *label = NULL;

        if (!g_str_has_prefix(slash_commands[i].name, text))
            continue;

        label = g_strdup_printf("%s%s%s", slash_commands[i].name,
                                slash_commands[i].argument != NULL ? " " : "",
                                slash_commands[i].argument != NULL
                                    ? slash_commands[i].argument : "");

        row = adw_action_row_new();
        set_row_text(row, label, slash_commands[i].summary);
        g_object_set_data_full(G_OBJECT(row), "command",
                               g_strdup(slash_commands[i].name), g_free);

        if (slash_commands[i].argument != NULL)
            g_object_set_data(G_OBJECT(row), "takes-argument",
                              GINT_TO_POINTER(1));

        gtk_list_box_append(self->command_list, row);
        matches++;
    }

    gtk_revealer_set_reveal_child(GTK_REVEALER(self->command_revealer),
                                  matches > 0);
}

static gboolean
on_entry_key(GtkEventControllerKey *controller, guint keyval, guint keycode,
             GdkModifierType state, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)controller;
    (void)keycode;

    /*
     * Enter sends; Shift+Enter is a newline.  A multi-line box needs
     * both, and a chat window where Enter inserts a newline is a chat
     * window nobody can send a message from.
     */
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if ((state & GDK_SHIFT_MASK) != 0)
            return GDK_EVENT_PROPAGATE;

        on_send(NULL, self);
        return GDK_EVENT_STOP;
    }

    if ((state & GDK_CONTROL_MASK) == 0)
        return GDK_EVENT_PROPAGATE;

    if (keyval == GDK_KEY_g || keyval == GDK_KEY_G) {
        /*
         * Shift takes the whole conversation to $EDITOR as org; plain
         * Ctrl+G takes the message being written. Same gesture, and the
         * difference is how much of it you meant.
         */
        if ((state & GDK_SHIFT_MASK) != 0)
            on_conversation_action(self, "edit-org", NULL);
        else
            compose_in_editor(self);

        return GDK_EVENT_STOP;
    }

    /*
     * Ctrl+V is intercepted only when there is actually an image on the
     * clipboard; ordinary text paste is left to the entry, which
     * already does it correctly.
     */
    if (keyval == GDK_KEY_v || keyval == GDK_KEY_V)
        return paste_image(self) ? GDK_EVENT_STOP : GDK_EVENT_PROPAGATE;

    return GDK_EVENT_PROPAGATE;
}


static void
load_history(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *messages;
    guint i;

    reset_transcript(self);
    set_activity(self, NULL);
    g_clear_pointer(&self->selected_room, g_free);
    g_hash_table_remove_all(self->shown);

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "room.history",
        clawt_build_payload("room", self->selected_agent, "as", "user",
                            NULL));

    if (reply == NULL)
        return;

    /*
     * The daemon says which room it resolved the agent to, and that is
     * what later messages are matched against.
     */
    self->selected_room = g_strdup(
        clawt_json_string(clawt_payload_of(reply), "room", NULL));

    messages = json_object_get_array_member(clawt_payload_of(reply),
                                            "messages");

    for (i = 0; i < json_array_get_length(messages); i++) {
        JsonObject *message = json_array_get_object_element(messages, i);
        const gchar *sender = clawt_json_string(message, "sender", "?");
        const gchar *id = clawt_json_string(message, "id", NULL);

        if (id != NULL)
            g_hash_table_add(self->shown, g_strdup(id));

        append_message(self, sender, clawt_json_string(message, "body", ""),
                       g_strcmp0(sender, "user") == 0,
                       clawt_json_int(message, "ts", 0));
    }

    set_following(self, TRUE);
    queue_scroll(self);
}

static void
on_send(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *full = NULL;
    g_autofree gchar *body = NULL;

    (void)widget;

    if (self->selected_agent == NULL)
        return;

    body = entry_text(self);

    if (body == NULL || *body == '\0')
        return;

    /*
     * A command never reaches the agent.  A mistyped one arriving as a
     * message is how somebody learns not to trust the feature.
     */
    if (run_slash_command(self, body)) {
        entry_set_text(self, "");
        return;
    }

    full = body_with_attachments(self, body);

    reply = clawt_window_request(
        self, "msg.send",
        clawt_build_payload("target", self->selected_agent, "body", full,
                            "from", "user", NULL));

    if (reply == NULL)
        return;

    /*
     * Not drawn here.  The daemon publishes an event for every message
     * it routes, this one included, so letting the send path draw its
     * own would put it on screen twice -- and the event is the version
     * that carries the room, which is what the transcript is filtered
     * on.
     */
    entry_set_text(self, "");
    g_hash_table_remove(self->drafts, self->selected_agent);

    /*
     * A stopped agent accepts the message -- that is what a durable
     * mailbox is for -- but it will not answer it, and a spinner that
     * spins forever is a worse lie than no spinner at all.  The daemon
     * reports the target's state so this does not have to be inferred
     * from a sidebar that may be a refresh behind.
     */
    {
        const gchar *state = clawt_json_string(clawt_payload_of(reply),
                                               "target_state", NULL);

        if (state != NULL && g_strcmp0(state, "running") != 0) {
            g_autofree gchar *warning = g_strdup_printf(
                "%s is %s -- held in its mailbox until it starts",
                self->selected_agent, state);

            set_activity(self, NULL);
            clawt_window_toast(self, warning);
        } else {
            set_activity(self, "delivered -- waiting for a reply");
        }
    }

    set_following(self, TRUE);
    queue_scroll(self);
}

/* ── Inspector ───────────────────────────────────────────────────── */

static GtkWidget *
info_row(const gchar *title, const gchar *value)
{
    GtkWidget *row = adw_action_row_new();

    set_row_text(row, title, value != NULL ? value : "-");

    return row;
}

/* Shared by the inspector's buttons and the sidebar's context menu. */
static void
agent_action(ClawtWindow *self, const gchar *kind)
{
    g_autoptr(JsonNode) reply = NULL;

    if (self->selected_agent == NULL || kind == NULL)
        return;

    reply = clawt_window_request(
        self, kind, clawt_build_payload("agent", self->selected_agent, NULL));

    (void)reply;

    /*
     * Refreshed whether or not it worked.  A start that fails still moves
     * the agent to ERROR, and skipping the refresh on failure left the
     * sidebar showing "stopped" next to a toast explaining why it could
     * not start -- two contradictory answers on screen at once.
     */
    refresh_agents(self);
    refresh_selected(self);
}

static void
on_agent_action(GtkButton *button, gpointer user_data)
{
    agent_action(user_data, g_object_get_data(G_OBJECT(button), "kind"));
}

static GtkWidget *
action_button(ClawtWindow *self, const gchar *label, const gchar *kind,
              gboolean sensitive, const gchar *why_not)
{
    GtkWidget *button = gtk_button_new_with_label(label);

    g_object_set_data_full(G_OBJECT(button), "kind", g_strdup(kind), g_free);
    g_signal_connect(button, "clicked", G_CALLBACK(on_agent_action), self);

    gtk_widget_set_sensitive(button, sensitive);

    /*
     * A disabled control says why.  A greyed-out button with no
     * explanation is the interface equivalent of a shrug.
     */
    if (!sensitive && why_not != NULL)
        gtk_widget_set_tooltip_text(button, why_not);

    return button;
}

/*
 * Sends one setting, and reports it if the daemon refuses.
 *
 * Returns: %TRUE if it was accepted
 */
static gboolean
apply_setting(ClawtWindow *self, const gchar *key, const gchar *value)
{
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(
        self, "agent.set",
        clawt_build_payload("agent", self->selected_agent, "key", key,
                            "value", value, NULL));

    if (reply == NULL)
        return FALSE;

    /*
     * An AI CLI lists its tools once, when its session starts, so a
     * permission changed under a running agent reaches its files and not
     * its session. Remembered rather than toasted here, because a save
     * applies a dozen settings and a dozen toasts is not an answer.
     */
    if (clawt_json_boolean(clawt_payload_of(reply), "restart_required",
                           FALSE))
        self->settings_need_restart = TRUE;

    return TRUE;
}

static void
on_save_agent(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    /* Per save, not per session. */
    self->settings_need_restart = FALSE;

    g_autofree gchar *model = NULL;
    static const gchar *const computers[] = { "none", "host", "container",
                                              "vm" };
    static const gchar *const efforts[] = { "low", "medium", "high",
                                            "xhigh", "max" };
    static const gchar *const restarts[] = { "never", "on-failure",
                                             "always" };
    gboolean ok = TRUE;

    (void)button;

    if (self->selected_agent == NULL)
        return;

    ok &= apply_schema_rows(self);
    ok &= apply_setting(self, "name",
                        gtk_editable_get_text(GTK_EDITABLE(self->name_row)));
    ok &= apply_setting(self, "description",
                        gtk_editable_get_text(
                            GTK_EDITABLE(self->description_row)));

    if (chooser_provider_id(&self->inspector_models) != NULL)
        ok &= apply_setting(self, "model.provider",
                            chooser_provider_id(&self->inspector_models));

    model = chooser_model(&self->inspector_models);

    if (model != NULL)
        ok &= apply_setting(self, "model.model", model);

    ok &= apply_setting(self, "model.effort",
                        efforts[MIN(adw_combo_row_get_selected(
                                        ADW_COMBO_ROW(self->effort_row)), 4)]);

    ok &= apply_setting(self, "computer.type",
                        computers[MIN(adw_combo_row_get_selected(
                                          ADW_COMBO_ROW(self->computer_row)),
                                      3)]);

    if (self->vm_cpus_row != NULL) {
        g_autofree gchar *image = disk_chooser_value(&self->inspector_disk);
        const gchar *cpus = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_cpus_row));
        const gchar *memory = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_memory_row));
        const gchar *disk = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_disk_row));
        const gchar *ssh_host = gtk_editable_get_text(
            GTK_EDITABLE(self->vm_ssh_host_row));

        if (image != NULL && *image != '\0')
            ok &= apply_setting(self, "computer.vm.image", image);

        if (cpus != NULL && *cpus != '\0')
            ok &= apply_setting(self, "computer.vm.cpus", cpus);

        if (memory != NULL && *memory != '\0')
            ok &= apply_setting(self, "computer.vm.memory_mb", memory);

        if (disk != NULL && *disk != '\0')
            ok &= apply_setting(self, "computer.vm.disk_gb", disk);

        /*
         * Read out of the model rather than from a fixed table, because
         * the list is built per agent: one whose size is not among the
         * common ones has it added, so saving cannot silently replace
         * it with the first entry.
         */
        if (self->vm_resolution_row != NULL) {
            GtkStringList *sizes = GTK_STRING_LIST(
                adw_combo_row_get_model(
                    ADW_COMBO_ROW(self->vm_resolution_row)));
            const gchar *resolution = gtk_string_list_get_string(
                sizes,
                adw_combo_row_get_selected(
                    ADW_COMBO_ROW(self->vm_resolution_row)));

            if (resolution != NULL && *resolution != '\0')
                ok &= apply_setting(self, "computer.vm.resolution",
                                    resolution);
        }

        if (ssh_host != NULL && *ssh_host != '\0')
            ok &= apply_setting(self, "computer.vm.ssh_host", ssh_host);

        /*
         * Written unconditionally, unlike the entries above.  A switch
         * turned *off* is a value of its own, and skipping an empty one
         * the way an empty text field is skipped would make the desktop
         * impossible to turn back off from here.
         */
        ok &= apply_setting(
            self, "computer.desktop.enabled",
            adw_switch_row_get_active(
                ADW_SWITCH_ROW(self->vm_desktop_row)) ? "true" : "false");
        ok &= apply_setting(
            self, "computer.desktop.allow_input",
            adw_switch_row_get_active(
                ADW_SWITCH_ROW(self->vm_desktop_input_row)) ? "true"
                                                            : "false");
    }

    if (self->inspector_image.row != NULL) {
        g_autofree gchar *image = image_chooser_value(&self->inspector_image);

        if (image != NULL)
            ok &= apply_setting(self, "computer.container.image", image);
    }

    ok &= apply_setting(self, "runtime.restart",
                        restarts[MIN(adw_combo_row_get_selected(
                                         ADW_COMBO_ROW(self->restart_row)),
                                     2)]);

    ok &= apply_setting(self, "runtime.autostart",
                        adw_switch_row_get_active(
                            ADW_SWITCH_ROW(self->autostart_row))
                            ? "true" : "false");

    if (self->team_row != NULL && self->team_ids != NULL) {
        guint chosen = adw_combo_row_get_selected(
            ADW_COMBO_ROW(self->team_row));
        guint count = g_strv_length(self->team_ids);

        /*
         * Written even when empty -- that is how an agent is taken off a
         * team, and skipping a blank would make "No team" the one choice
         * in this dialog that does nothing.
         */
        if (chosen < count)
            ok &= apply_setting(self, "team", self->team_ids[chosen]);
    }

    if (self->team_role_row != NULL) {
        static const gchar *const roles[] = { "member", "lead" };

        ok &= apply_setting(self, "team_role",
                            roles[MIN(adw_combo_row_get_selected(
                                          ADW_COMBO_ROW(self->team_role_row)),
                                      1)]);
    }

    ok &= apply_setting(self, "chief_of_staff",
                        adw_switch_row_get_active(
                            ADW_SWITCH_ROW(self->chief_row))
                            ? "true" : "false");

    ok &= apply_setting(self, "tools.manage_fleet",
                        adw_switch_row_get_active(
                            ADW_SWITCH_ROW(self->manage_fleet_row))
                            ? "true" : "false");

    if (!ok)
        return;

    /*
     * Said plainly, because most of these only take effect on the next
     * start -- the model and the computer especially.  A reload does not
     * restart running agents on purpose, and an interface that implied
     * otherwise would have people wondering why nothing changed.
     */
    /*
     * The tool list is named separately when it changed, because it is
     * the one where "restart it" is not general advice but the whole
     * answer: an AI CLI lists its tools once, at session start, so a
     * permission granted under a running agent reaches its files and not
     * its session -- and the agent then says, accurately, that it does
     * not have the tool.
     */
    if (self->settings_need_restart) {
        clawt_window_toast(self,
                           "Saved. Restart the agent -- it lists its tools "
                           "when it starts, so it cannot see the change "
                           "until then.");
    } else {
        clawt_window_toast(self,
                           "Saved. Restart the agent for the model or "
                           "computer to take effect.");
    }

    refresh_agents(self);
}

/* ── Deleting ────────────────────────────────────────────────────── */

static void
on_delete_confirmed_twice(AdwAlertDialog *dialog, gchar *response,
                          gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *check = g_object_get_data(G_OBJECT(dialog), "remove-computer");
    GtkWidget *purge_check = g_object_get_data(G_OBJECT(dialog),
                                               "remove-files");
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *agent_id = NULL;
    gboolean with_computer;
    gboolean purge;

    if (g_strcmp0(response, "delete") != 0)
        return;

    agent_id = g_strdup(self->selected_agent);
    with_computer = check != NULL &&
                    gtk_check_button_get_active(GTK_CHECK_BUTTON(check));
    purge = purge_check != NULL &&
            gtk_check_button_get_active(GTK_CHECK_BUTTON(purge_check));

    reply = clawt_window_request(
        self, "agent.remove",
        clawt_build_payload("agent", agent_id,
                            "remove_computer", with_computer ? "true" : NULL,
                            "remove_files", purge ? "true" : NULL,
                            NULL));

    if (reply == NULL)
        return;

    g_clear_pointer(&self->selected_agent, g_free);
    g_clear_pointer(&self->selected_room, g_free);
    clear_box(self->inspector);
    reset_transcript(self);

    {
        const gchar *computer = clawt_json_string(clawt_payload_of(reply),
                                                   "computer", NULL);
        g_autofree gchar *message = NULL;

        /*
         * The computer's fate is reported rather than assumed: a
         * teardown that failed is not fatal to the removal, and a toast
         * saying "is gone" over a container still running would be a
         * lie.
         */
        if (computer != NULL && g_strcmp0(computer, "removed") == 0)
            message = g_strdup_printf("%s and its computer are gone.",
                                      agent_id);
        else if (computer != NULL)
            message = g_strdup_printf("%s is gone; its computer was not "
                                      "removed: %s", agent_id, computer);
        else
            message = g_strdup_printf("%s is gone.", agent_id);

        clawt_window_toast(self, message);
    }

    refresh_agents(self);
}

static void
on_delete_confirmed_once(AdwAlertDialog *dialog, gchar *response,
                         gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *second;

    (void)dialog;

    if (g_strcmp0(response, "delete") != 0)
        return;

    /*
     * A second, deliberately louder confirmation.  Removing an agent
     * takes its running process and its place in every room with it, and
     * a single click between "browsing" and "gone" is too few.
     */
    second = ADW_ALERT_DIALOG(
        adw_alert_dialog_new("ARE YOU REALLY SURE!?", NULL));

    adw_alert_dialog_set_body(
        second,
        "This removes the agent from the fleet and stops it if it is "
        "running.\n\n"
        "Its mailbox and transcripts stay on disk -- removing an agent is "
        "reversible, deleting its history is not.");

    /*
     * The container or VM is a separate decision.
     *
     * Removing the agent used to leave it running under a name derived
     * from an agent that no longer existed, so the only way to find it
     * again was to remember what it had been called. Off by default,
     * because a container may hold work that was never anywhere else.
     */
    {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        GtkWidget *purge;

        gtk_widget_set_margin_top(box, 12);

        if (g_strcmp0(self->inspector_computer, "container") == 0 ||
            g_strcmp0(self->inspector_computer, "vm") == 0) {
            GtkWidget *check;
            g_autofree gchar *label = g_strdup_printf(
                "Also delete its %s, clawt-%s",
                g_strcmp0(self->inspector_computer, "vm") == 0
                    ? "virtual machine" : "container",
                self->selected_agent);

            check = gtk_check_button_new_with_label(label);
            gtk_box_append(GTK_BOX(box), check);

            /*
             * Kept on the dialog rather than in the window: the dialog
             * is what the response handler is given, and a second delete
             * started before the first finished would otherwise read the
             * wrong checkbox.
             */
            g_object_set_data(G_OBJECT(second), "remove-computer", check);
        }

        /*
         * And the files, which is the half with no undo.
         *
         * Off by default and worded without euphemism: removing an agent
         * from the fleet is reversible and deleting what it wrote is
         * not. It is here because a throwaway agent made to test
         * something should be throwable away, and leaving seven
         * abandoned workspaces behind is its own kind of mess.
         */
        purge = gtk_check_button_new_with_label(
            "Also delete everything it owns: its persona, notes, "
            "mailbox, memories, transcripts and credentials");
        gtk_label_set_wrap(
            GTK_LABEL(gtk_widget_get_last_child(purge)), TRUE);
        gtk_box_append(GTK_BOX(box), purge);
        g_object_set_data(G_OBJECT(second), "remove-files", purge);

        adw_alert_dialog_set_extra_child(second, box);
    }

    adw_alert_dialog_add_response(second, "cancel", "Keep it");
    adw_alert_dialog_add_response(second, "delete", "Delete it for good");
    adw_alert_dialog_set_response_appearance(second, "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(second, "cancel");
    adw_alert_dialog_set_close_response(second, "cancel");

    g_signal_connect(second, "response",
                     G_CALLBACK(on_delete_confirmed_twice), self);

    adw_dialog_present(ADW_DIALOG(second), GTK_WIDGET(self));
}

static void
delete_agent(ClawtWindow *self)
{
    AdwAlertDialog *first;
    g_autofree gchar *heading = NULL;

    if (self->selected_agent == NULL)
        return;

    heading = g_strdup_printf("Delete %s?", self->selected_agent);

    first = ADW_ALERT_DIALOG(adw_alert_dialog_new("Are you sure?", NULL));

    adw_alert_dialog_set_body(first, heading);
    adw_alert_dialog_add_response(first, "cancel", "Cancel");
    adw_alert_dialog_add_response(first, "delete", "Delete");
    adw_alert_dialog_set_response_appearance(first, "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(first, "cancel");
    adw_alert_dialog_set_close_response(first, "cancel");

    g_signal_connect(first, "response",
                     G_CALLBACK(on_delete_confirmed_once), self);

    adw_dialog_present(ADW_DIALOG(first), GTK_WIDGET(self));
}

static void
on_delete_agent(GtkButton *button, gpointer user_data)
{
    (void)button;

    delete_agent(user_data);
}

/* ── The inspector ───────────────────────────────────────────────── */

static guint
index_of(const gchar *const *values, const gchar *wanted)
{
    guint i;

    for (i = 0; values[i] != NULL; i++) {
        if (g_strcmp0(values[i], wanted) == 0)
            return i;
    }

    return 0;
}

static GtkWidget *
combo_row(const gchar *title, const gchar *const *values,
          const gchar *selected)
{
    GtkWidget *row = adw_combo_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    adw_combo_row_set_model(ADW_COMBO_ROW(row),
                            G_LIST_MODEL(gtk_string_list_new(values)));
    adw_combo_row_set_selected(ADW_COMBO_ROW(row),
                               index_of(values, selected));

    return row;
}

/*
 * The screen sizes worth offering, plus whatever this agent already has.
 *
 * The second half matters: a resolution set by hand or by another client
 * that is not in this list would otherwise be shown as the first entry,
 * and saving the page -- without touching this row -- would quietly
 * change it. A combo box has no way to say "something else", so the
 * something else joins the list.
 */
static GtkWidget *
resolution_row(const gchar *current)
{
    static const gchar *const common[] = {
        "1280x800", "1280x1024", "1440x900", "1600x900", "1680x1050",
        "1920x1080", "1920x1200", "2560x1440", "3840x2160", NULL
    };
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    gboolean known = FALSE;
    gsize i;

    for (i = 0; common[i] != NULL; i++) {
        if (g_strcmp0(common[i], current) == 0)
            known = TRUE;

        g_ptr_array_add(values, (gpointer)common[i]);
    }

    if (current != NULL && *current != '\0' && !known)
        g_ptr_array_insert(values, 0, (gpointer)current);

    g_ptr_array_add(values, NULL);

    return combo_row("Screen size", (const gchar *const *)values->pdata,
                     current);
}

/*
 * Make a row respond to a click, and look like it will.
 *
 * libadwaita clears GtkListBoxRow:activatable unless an *activatable
 * widget* is set, so a plain AdwActionRow never emits ::row-activated:
 * the row highlights and nothing happens, which is what the integrations
 * list did for its whole life.
 *
 * Setting the row as its own activatable widget makes it activatable and
 * then recurses until the stack runs out -- measured at 2502 GTK
 * criticals and a segfault. It has to be a *different* widget, and the
 * chevron is the natural one: it is also what tells somebody the row
 * goes somewhere.
 *
 * One function because there are three such lists and they had three
 * answers between them, two of which were wrong in different ways.
 *
 * Returns: (transfer none): @row, so it can be used inline
 */
static GtkWidget *
row_opens_something(GtkWidget *row)
{
    GtkWidget *chevron = gtk_image_new_from_icon_name("go-next-symbolic");

    adw_action_row_add_suffix(ADW_ACTION_ROW(row), chevron);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), chevron);

    return row;
}

/*
 * One row built from a schema entry, and the key it writes.
 */
typedef struct {
    GtkWidget       *row;
    gchar           *key;
    ClawtSchemaType  type;
    GStrv            choices;               /* for an enum */
} SchemaRow;

static void
schema_row_free(gpointer data)
{
    SchemaRow *row = data;

    g_free(row->key);
    g_strfreev(row->choices);
    g_free(row);
}


static GtkWidget *
entry_row(const gchar *title, const gchar *value)
{
    GtkWidget *row = adw_entry_row_new();

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
    gtk_editable_set_text(GTK_EDITABLE(row), value != NULL ? value : "");

    return row;
}

static GtkWidget *
switch_row(const gchar *title, const gchar *subtitle, gboolean active)
{
    GtkWidget *row = adw_switch_row_new();

    set_row_text(row, title, subtitle);
    adw_switch_row_set_active(ADW_SWITCH_ROW(row), active);

    return row;
}

/*
 * The nicknames an enum option accepts, from its own GType.
 */
static GStrv
schema_enum_choices(const ClawtSchemaEntry *entry)
{
    g_autoptr(GPtrArray) values = g_ptr_array_new();
    g_autoptr(GEnumClass) klass = NULL;
    guint i;

    if (entry->enum_type == NULL || !G_TYPE_IS_ENUM(entry->enum_type()))
        return NULL;

    klass = g_type_class_ref(entry->enum_type());

    for (i = 0; i < klass->n_values; i++)
        g_ptr_array_add(values, g_strdup(klass->values[i].value_nick));

    g_ptr_array_add(values, NULL);

    return (GStrv)g_ptr_array_free(g_steal_pointer(&values), FALSE);
}

/*
 * Every option an agent can set that has no hand-built row of its own.
 *
 * Walked from the schema rather than listed, which is what makes these
 * appear at all: the six mailbox overrides and three memories ones are
 * not `agents.*` keys, so nothing here knew their names until
 * clawt_config_schema_agent_name() could say. They were settable in a
 * config file and absent from both clients for the whole life of the
 * feature.
 *
 * The rows are generic on purpose. The hand-built ones above earn their
 * copy -- "May create agents" explains which of two similarly-named
 * settings actually grants the tool -- and a generated row cannot do
 * that. These get the schema's own first line instead, which is better
 * than not existing.
 */
static void
build_schema_rows(ClawtWindow *self, JsonObject *settings)
{
    GtkWidget *group = adw_preferences_group_new();
    const ClawtSchemaEntry *schema;
    gsize n_entries = 0;
    gsize i;
    guint added = 0;

    g_ptr_array_set_size(self->schema_rows, 0);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Mailbox and memories");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Fleet settings this agent may override. Left as they are, each "
        "follows the fleet.");

    schema = clawt_config_schema_get(&n_entries);

    for (i = 0; i < n_entries; i++) {
        const ClawtSchemaEntry *entry = &schema[i];
        const gchar *key = clawt_config_schema_agent_name(entry);
        const gchar *value;
        SchemaRow *record;

        /*
         * Only the ones with no row of their own. An `agents.*` option
         * is already on screen above, written by hand.
         */
        if (key == NULL || g_str_has_prefix(entry->key, "agents."))
            continue;

        value = (settings != NULL && json_object_has_member(settings, key))
                ? json_object_get_string_member(settings, key) : NULL;

        record = g_new0(SchemaRow, 1);
        record->key = g_strdup(key);
        record->type = entry->type;

        switch (entry->type) {
        case CLAWT_SCHEMA_BOOLEAN:
            record->row = switch_row(key, entry->doc,
                                     g_strcmp0(value, "true") == 0);
            break;

        case CLAWT_SCHEMA_ENUM:
            record->choices = schema_enum_choices(entry);

            if (record->choices != NULL) {
                record->row = combo_row(
                    key, (const gchar *const *)record->choices, value);
                break;
            }

            record->row = entry_row(key, value);
            record->type = CLAWT_SCHEMA_STRING;
            break;

        default:
            record->row = entry_row(key, value);
            break;
        }

        g_ptr_array_add(self->schema_rows, record);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), record->row);
        added++;
    }

    if (added == 0) {
        gtk_widget_unparent(group);
        return;
    }

    gtk_box_append(self->inspector, group);
}

/*
 * Writes back whatever the generic rows hold.
 */
static gboolean
apply_schema_rows(ClawtWindow *self)
{
    gboolean ok = TRUE;
    guint i;

    for (i = 0; i < self->schema_rows->len; i++) {
        SchemaRow *record = g_ptr_array_index(self->schema_rows, i);
        g_autofree gchar *value = NULL;

        switch (record->type) {
        case CLAWT_SCHEMA_BOOLEAN:
            value = g_strdup(
                adw_switch_row_get_active(ADW_SWITCH_ROW(record->row))
                ? "true" : "false");
            break;

        case CLAWT_SCHEMA_ENUM: {
            guint selected =
                adw_combo_row_get_selected(ADW_COMBO_ROW(record->row));

            if (record->choices == NULL ||
                selected >= g_strv_length(record->choices))
                continue;

            value = g_strdup(record->choices[selected]);
            break;
        }

        default:
            value = g_strdup(
                gtk_editable_get_text(GTK_EDITABLE(record->row)));
            break;
        }

        ok &= apply_setting(self, record->key, value);
    }

    return ok;
}

/*
 * Shares a host folder with the agent's computer.
 *
 * The mount list has always been read and applied -- bind mounts for a
 * container, virtiofs devices for a VM -- and no client could write one,
 * so the only way to share a folder was to edit the YAML by hand.
 */
static void
on_add_mount(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    static const gchar *const modes[] = { "ro", "rw" };
    const gchar *source;
    const gchar *target;
    guint mode;

    (void)button;

    if (self->selected_agent == NULL || self->mount_source_row == NULL)
        return;

    source = gtk_editable_get_text(GTK_EDITABLE(self->mount_source_row));
    target = gtk_editable_get_text(GTK_EDITABLE(self->mount_target_row));

    if (source == NULL || *source == '\0' || target == NULL ||
        *target == '\0') {
        clawt_window_toast(self, "A share needs a folder and a path inside.");
        return;
    }

    mode = adw_combo_row_get_selected(ADW_COMBO_ROW(self->mount_mode_row));

    reply = clawt_window_request(
        self, "agent.mount.add",
        clawt_build_payload("agent", self->selected_agent,
                            "source", source, "target", target,
                            "mode", modes[MIN(mode, 1)], NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Shared. It appears when the agent restarts.");
    refresh_selected(self);
}

static void
on_remove_mount(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *target = g_object_get_data(G_OBJECT(button), "target");
    g_autoptr(JsonNode) reply = NULL;

    if (self->selected_agent == NULL || target == NULL)
        return;

    reply = clawt_window_request(
        self, "agent.mount.remove",
        clawt_build_payload("agent", self->selected_agent, "target", target,
                            NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Removed. Takes effect on the next restart.");
    refresh_selected(self);
}

/*
 * The shared folders group, for a computer that can have them.
 *
 * A host agent is left out: its computer is the machine itself, so there
 * is nothing to mount into -- what limits it there is confinement, not
 * a mount list.
 */
static void
build_mounts(ClawtWindow *self, const gchar *computer_type)
{
    static const gchar *const modes[] = { "ro", "rw", NULL };
    g_autoptr(JsonNode) reply = NULL;
    GtkWidget *group;
    GtkWidget *add;
    JsonArray *mounts;
    guint i;

    self->mount_source_row = NULL;
    self->mount_target_row = NULL;
    self->mount_mode_row = NULL;

    if (g_strcmp0(computer_type, "container") != 0 &&
        g_strcmp0(computer_type, "vm") != 0)
        return;

    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Shared folders");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        g_strcmp0(computer_type, "vm") == 0
            ? "Passed into the VM over virtiofs. Changes apply when it "
              "next starts."
            : "Bind-mounted into the container, SELinux-relabelled so "
              "they are actually readable. Changes apply when it next "
              "starts.");

    reply = clawt_window_request(
        self, "agent.mount.list",
        clawt_build_payload("agent", self->selected_agent, NULL));

    mounts = (reply != NULL)
             ? json_object_get_array_member(clawt_payload_of(reply), "mounts")
             : NULL;

    for (i = 0; mounts != NULL && i < json_array_get_length(mounts); i++) {
        JsonObject *mount = json_array_get_object_element(mounts, i);
        const gchar *target = clawt_json_string(mount, "target", "?");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *remove;
        g_autofree gchar *subtitle = g_strdup_printf(
            "%s  (%s)", clawt_json_string(mount, "source", "-"),
            clawt_json_string(mount, "mode", "ro"));

        set_row_text(row, target, subtitle);

        remove = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(remove, "flat");
        gtk_widget_set_tooltip_text(remove, "Stop sharing this folder");
        g_object_set_data_full(G_OBJECT(remove), "target", g_strdup(target),
                               g_free);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_mount),
                         self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    self->mount_source_row = entry_row("Folder on this machine", NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->mount_source_row);

    self->mount_target_row = entry_row("Where the agent sees it", NULL);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->mount_target_row);

    self->mount_mode_row = combo_row("Access", modes, "ro");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->mount_mode_row);

    add = gtk_button_new_with_label("Share this folder");
    gtk_widget_set_margin_top(add, 6);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_mount), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add);

    gtk_box_append(self->inspector, group);
}

/* ── Opening a workspace file ────────────────────────────────────── */

/*
 * One attempt at running an editor, so a failed one can be retried
 * differently.
 *
 * Holds a reference to the window: this outlives the click that started
 * it, and an editor the user leaves open for an hour would otherwise be
 * writing its result into freed memory when they finally quit it.
 */
typedef struct {
    ClawtWindow *window;
    gchar       *path;
    gchar       *name;
    gboolean     in_terminal;
    gint64       started;
} EditorLaunch;

static void editor_launch(EditorLaunch *launch);

static void
editor_launch_free(EditorLaunch *launch)
{
    g_clear_object(&launch->window);
    g_free(launch->path);
    g_free(launch->name);
    g_free(launch);
}

/*
 * $CLAWT_EDITOR first, so this can be pointed somewhere else without
 * changing what every other program on the machine does.
 */
static const gchar *
editor_command(void)
{
    const gchar *editor = g_getenv("CLAWT_EDITOR");

    if (editor == NULL || editor[0] == '\0')
        editor = g_getenv("VISUAL");

    if (editor == NULL || editor[0] == '\0')
        editor = g_getenv("EDITOR");

    return (editor != NULL && editor[0] != '\0') ? editor : NULL;
}

/*
 * A terminal to run a terminal-only editor inside.
 *
 * There is no way to ask an editor whether it needs one, so this is
 * only reached after running it bare has already failed -- which is
 * exactly what a curses editor does when it is started without a
 * terminal, and quickly. The flag differs per terminal: some take the
 * command straight after the binary, some want a separator first.
 */
static gboolean
terminal_prefix(GPtrArray *argv)
{
    static const struct {
        const gchar *binary;
        const gchar *flag;
    } terminals[] = {
        { "xdg-terminal-exec", NULL },
        { "gst",               "-e" },
        { "foot",              NULL },
        { "kitty",             NULL },
        { "wezterm",           "start" },
        { "alacritty",         "-e" },
        { "gnome-terminal",    "--" },
        { "xterm",             "-e" }
    };
    const gchar *configured = g_getenv("TERMINAL");
    gsize i;

    if (configured != NULL && configured[0] != '\0') {
        g_ptr_array_add(argv, g_strdup(configured));
        g_ptr_array_add(argv, g_strdup("-e"));
        return TRUE;
    }

    for (i = 0; i < G_N_ELEMENTS(terminals); i++) {
        g_autofree gchar *found = g_find_program_in_path(terminals[i].binary);

        if (found == NULL)
            continue;

        g_ptr_array_add(argv, g_steal_pointer(&found));

        if (terminals[i].flag != NULL)
            g_ptr_array_add(argv, g_strdup(terminals[i].flag));

        return TRUE;
    }

    return FALSE;
}

static void
on_editor_finished(GObject *source, GAsyncResult *result, gpointer user_data)
{
    EditorLaunch *launch = user_data;
    g_autoptr(GError) error = NULL;

    if (g_subprocess_wait_check_finish(G_SUBPROCESS(source), result, &error)) {
        editor_launch_free(launch);
        return;
    }

    /*
     * A terminal editor started without a terminal fails immediately,
     * and there is no way to know in advance which kind $EDITOR is --
     * emacsclient opens a window here and vi does not. So the bare run
     * is the test, and this is the second attempt.
     *
     * Only when it failed straight away, though: an editor the user
     * spent ten minutes in and then quit with a non-zero status has
     * already done its job, and popping a terminal open at that point
     * would be baffling.
     */
    if (!launch->in_terminal &&
        g_get_monotonic_time() - launch->started < 2 * G_USEC_PER_SEC) {
        launch->in_terminal = TRUE;
        editor_launch(launch);
        return;
    }

    {
        g_autofree gchar *message =
            g_strdup_printf("could not open %s: %s", launch->name,
                            error->message);

        clawt_window_toast(launch->window, message);
    }

    editor_launch_free(launch);
}

static void
editor_launch(EditorLaunch *launch)
{
    g_autoptr(GPtrArray) argv = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GSubprocess) process = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) parts = NULL;
    const gchar *editor = editor_command();
    gint i;

    if (launch->in_terminal && !terminal_prefix(argv)) {
        clawt_window_toast(launch->window,
                           "no terminal found to run $EDITOR in");
        editor_launch_free(launch);
        return;
    }

    /*
     * $EDITOR is a command line, not a program name: "emacsclient -nw"
     * and "code --wait" are both ordinary settings, so it is parsed
     * rather than exec'd whole.
     */
    if (editor == NULL || !g_shell_parse_argv(editor, NULL, &parts, &error)) {
        g_autofree gchar *message =
            g_strdup_printf("$EDITOR (%s) could not be parsed: %s",
                            editor != NULL ? editor : "unset",
                            error != NULL ? error->message : "it is not set");

        clawt_window_toast(launch->window, message);
        editor_launch_free(launch);
        return;
    }

    for (i = 0; parts[i] != NULL; i++)
        g_ptr_array_add(argv, g_strdup(parts[i]));

    g_ptr_array_add(argv, g_strdup(launch->path));
    g_ptr_array_add(argv, NULL);

    process = g_subprocess_newv((const gchar *const *)argv->pdata,
                                G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                                &error);

    if (process == NULL) {
        g_autofree gchar *message =
            g_strdup_printf("could not run %s: %s", editor, error->message);

        clawt_window_toast(launch->window, message);
        editor_launch_free(launch);
        return;
    }

    launch->started = g_get_monotonic_time();
    g_subprocess_wait_check_async(process, NULL, on_editor_finished, launch);
}

static void
open_path_in_editor(ClawtWindow *self, const gchar *path, const gchar *name)
{
    EditorLaunch *launch;

    if (path == NULL || path[0] == '\0')
        return;

    if (editor_command() == NULL) {
        g_autofree gchar *message =
            g_strdup_printf("no editor set; try `clawtilla agent edit %s %s`",
                            self->selected_agent != NULL
                                ? self->selected_agent : "<agent>",
                            name != NULL ? name : "<file>");

        clawt_window_toast(self, message);
        return;
    }

    launch = g_new0(EditorLaunch, 1);
    launch->window = g_object_ref(self);
    launch->path = g_strdup(path);
    launch->name = g_strdup(name);

    editor_launch(launch);
}

static void
on_open_file(GtkButton *button, gpointer user_data)
{
    open_path_in_editor(user_data,
                        g_object_get_data(G_OBJECT(button), "path"),
                        g_object_get_data(G_OBJECT(button), "name"));
}

/*
 * Which integrations this agent has, and a switch for each shared one.
 *
 * The switch edits the instance's `agents` list rather than writing
 * anything into the agent, because that is where the scope lives -- and
 * the alternative, an inline copy per agent, is the duplication the
 * shared instances exist to end.
 */
static void
on_agent_integration_toggled(GObject *row, GParamSpec *spec,
                             gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name = g_object_get_data(row, "integration");
    g_autoptr(JsonNode) list = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    gboolean wanted = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
    JsonObject *integration;
    JsonArray *agents;
    gboolean present = FALSE;
    guint i;

    (void)spec;

    if (self->selected_agent == NULL || name == NULL)
        return;

    list = clawt_window_request(self, "integration.list", NULL);
    integration = find_integration(list, name);

    if (integration == NULL)
        return;

    /*
     * `all` is left alone.  Switching one agent off an integration
     * everybody has is a decision about the whole fleet, and quietly
     * turning it into a list of everyone-but-this-one is not what the
     * switch appears to say.
     */
    if (g_strcmp0(clawt_json_string(integration, "scope", ""), "all") == 0) {
        clawt_window_toast(self,
                           "That one is set to every agent. Change its "
                           "scope in Settings to pick agents.");
        return;
    }

    agents = json_object_get_array_member(integration, "agents");

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "scope");
    json_builder_add_string_value(builder, "selected");
    json_builder_set_member_name(builder, "agents");
    json_builder_begin_array(builder);

    for (i = 0; i < json_array_get_length(agents); i++) {
        const gchar *id = json_array_get_string_element(agents, i);

        if (g_strcmp0(id, self->selected_agent) == 0) {
            present = TRUE;

            if (!wanted)
                continue;
        }

        json_builder_add_string_value(builder, id);
    }

    if (wanted && !present)
        json_builder_add_string_value(builder, self->selected_agent);

    json_builder_end_array(builder);
    json_builder_end_object(builder);

    reply = clawt_window_request(self, "integration.update",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    refresh_selected(self);
}

static void
build_agent_integrations(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    GtkWidget *group;
    JsonObject *root;
    JsonArray *integrations;
    JsonArray *bindings;
    guint i;

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "integration.list",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    root = clawt_payload_of(reply);
    integrations = json_object_get_array_member(root, "integrations");
    bindings = json_object_has_member(root, "bindings")
        ? json_object_get_array_member(root, "bindings") : NULL;

    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Integrations");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "What reaches this agent from outside the fleet. Add and configure "
        "them in Settings; here you choose which ones this agent gets.");

    for (i = 0; i < json_array_get_length(integrations); i++) {
        JsonObject *integration =
            json_array_get_object_element(integrations, i);
        const gchar *name = clawt_json_string(integration, "name", "?");
        gboolean covers = json_object_has_member(integration, "covers") &&
                          json_object_get_boolean_member(integration,
                                                         "covers");
        GtkWidget *row = adw_switch_row_new();
        g_autofree gchar *subtitle = NULL;

        subtitle = g_strdup_printf(
            "%s%s", clawt_json_string(integration, "type", "?"),
            g_strcmp0(clawt_json_string(integration, "scope", ""),
                      "all") == 0 ? " \342\200\224 every agent" : "");

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
        adw_switch_row_set_active(ADW_SWITCH_ROW(row), covers);

        g_object_set_data_full(G_OBJECT(row), "integration", g_strdup(name),
                               g_free);

        /*
         * Connected after the initial state is set, so building the
         * inspector does not look like somebody flipping every switch.
         */
        g_signal_connect(row, "notify::active",
                         G_CALLBACK(on_agent_integration_toggled), self);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    /*
     * The agent's own inline blocks are not instances and have no switch:
     * they were written inside this agent and belong to it. Shown anyway,
     * because an agent with a Matrix block of its own would otherwise
     * look like it had no Matrix at all.
     */
    for (i = 0; bindings != NULL && i < json_array_get_length(bindings); i++) {
        JsonObject *binding = json_array_get_object_element(bindings, i);
        g_autofree gchar *subtitle = NULL;

        if (json_object_get_boolean_member(binding, "shared"))
            continue;

        subtitle = g_strdup_printf(
            "%s \342\200\224 configured inside this agent%s%s",
            clawt_json_string(binding, "type", "?"),
            json_object_get_boolean_member(binding, "valid") ? "" : ": ",
            json_object_get_boolean_member(binding, "valid")
                ? "" : clawt_json_string(binding, "problem", ""));

        adw_preferences_group_add(
            ADW_PREFERENCES_GROUP(group),
            info_row(clawt_json_string(binding, "name", "?"), subtitle));
    }

    if (json_array_get_length(integrations) == 0 &&
        (bindings == NULL || json_array_get_length(bindings) == 0))
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(group),
            "None yet. Settings \342\206\222 Integrations adds one -- a "
            "Matrix account to talk to this agent from your phone, or an "
            "MCP server to give it tools.");

    gtk_box_append(self->inspector, group);
}

/*
 * The agent's workspace files, each openable in $EDITOR.
 *
 * These are the files the agent reads as its system prompt, plus the
 * .mcp.json that decides which MCP servers it can call -- so this is
 * where an agent is actually configured, as opposed to merely wired up.
 */
static void
build_files(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    GtkWidget *group;
    JsonArray *files;
    guint i;

    reply = clawt_window_request(
        self, "agent.files",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    files = json_object_get_array_member(clawt_payload_of(reply), "files");

    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Files");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Opened in $EDITOR. The ones marked \"prompt\" are read into every "
        "turn; .mcp.json is the list of MCP servers this agent may call, "
        "and clawtilla only ever rewrites its own entry in it.");

    for (i = 0; files != NULL && i < json_array_get_length(files); i++) {
        JsonObject *file = json_array_get_object_element(files, i);
        const gchar *name = clawt_json_string(file, "name", "?");
        const gchar *path = clawt_json_string(file, "path", "");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *open;
        g_autofree gchar *subtitle = NULL;

        subtitle = json_object_get_boolean_member(file, "identity")
                   ? g_strdup_printf("prompt \xc2\xb7 %s",
                                     clawt_json_string(file, "title", ""))
                   : g_strdup(clawt_json_string(file, "title", ""));

        set_row_text(row, name, subtitle);
        gtk_widget_set_tooltip_text(row, path);

        /*
         * An explicit button rather than an activatable row: libadwaita
         * clears GtkListBoxRow:activatable on an AdwActionRow unless it
         * has an activatable-widget, so ::row-activated would never
         * fire and clicking would appear to do nothing.
         */
        open = gtk_button_new_from_icon_name("document-edit-symbolic");
        gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(open, "flat");
        gtk_widget_set_tooltip_text(open, "Open in $EDITOR");
        g_object_set_data_full(G_OBJECT(open), "path", g_strdup(path),
                               g_free);
        g_object_set_data_full(G_OBJECT(open), "name", g_strdup(name),
                               g_free);
        g_signal_connect(open, "clicked", G_CALLBACK(on_open_file), self);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), open);

        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    gtk_box_append(self->inspector, group);
}

/*
 * Rebuilding is the only way to apply anything cloud-init reads at first
 * boot -- the login, the desktop, the package list.  Those are fixed for
 * the life of the overlay, so changing them in the config and restarting
 * looks like it did nothing.
 */
static void
on_rebuild_confirmed(AdwAlertDialog *dialog, const gchar *response,
                     gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "rebuild") != 0)
        return;

    reply = clawt_window_request(
        self, "computer.rebuild",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Rebuilt. Its contents are gone, and "
                             "cloud-init runs again on the next start.");
    refresh_selected(self);
}

static void
on_rebuild_computer(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *ask;

    (void)button;

    ask = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "Rebuild this computer?",
        "Everything in it is destroyed and it is built again from the "
        "image.\n\n"
        "This is what applies settings cloud-init only reads at first "
        "boot -- the login, the desktop, the packages -- which are "
        "otherwise fixed for the life of the machine. It is also how to "
        "get one back that was deleted outside clawtilla.\n\n"
        "The agent has to be stopped."));

    adw_alert_dialog_add_response(ask, "cancel", "Cancel");
    adw_alert_dialog_add_response(ask, "rebuild", "Rebuild");
    adw_alert_dialog_set_response_appearance(ask, "rebuild",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ask, "cancel");

    g_signal_connect(ask, "response", G_CALLBACK(on_rebuild_confirmed), self);
    adw_dialog_present(ADW_DIALOG(ask), GTK_WIDGET(self));
}

static void
build_inspector(ClawtWindow *self, JsonObject *agent, JsonObject *payload)
{
    static const gchar *const computers[] = { "none", "host", "container",
                                              "vm", NULL };
    static const gchar *const efforts[] = { "low", "medium", "high",
                                            "xhigh", "max", NULL };
    static const gchar *const restarts[] = { "never", "on-failure",
                                             "always", NULL };
    GtkWidget *group;
    GtkWidget *actions;
    GtkWidget *save;
    GtkWidget *danger;
    GtkWidget *delete_button;
    const gchar *state = clawt_json_string(agent, "state", "stopped");
    const gchar *caps = clawt_json_string(agent, "caps", "");
    gboolean is_shadow = g_strcmp0(state, "shadow") == 0;

    clear_box(self->inspector);
    g_clear_pointer(&self->inspector_models.catalog, json_node_unref);

    /*
     * The image chooser is rebuilt with the inspector, so its catalogue
     * and its widget pointers are cleared together -- a stale row
     * pointer here is one the save handler would write through after
     * the widget had been destroyed.
     */
    g_clear_pointer(&self->inspector_image.catalog, json_node_unref);
    self->inspector_image.row = NULL;
    self->inspector_image.entry = NULL;

    /* ── What it is, and what it is doing ── */
    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Agent");

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              info_row("Id", clawt_json_string(agent, "id",
                                                               "?")));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              info_row("State", state));

    if (clawt_json_string(agent, "detail", NULL) != NULL)
        adw_preferences_group_add(
            ADW_PREFERENCES_GROUP(group),
            info_row("Why", clawt_json_string(agent, "detail", NULL)));

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              info_row("Can do", caps));

    gtk_box_append(self->inspector, group);

    /* ── Editable ── */
    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), "Settings");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The model and the computer take effect when the agent next "
        "starts.");

    self->name_row = entry_row("Name", clawt_json_string(agent, "name", ""));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->name_row);

    self->description_row = entry_row(
        "Description", clawt_json_string(agent, "description", ""));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->description_row);

    model_chooser_build(&self->inspector_models, self, group,
                        clawt_json_string(agent, "provider", NULL),
                        clawt_json_string(agent, "model", NULL));

    self->effort_row = combo_row("Effort", efforts,
                                 clawt_json_string(agent, "effort",
                                                   "medium"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->effort_row);

    self->computer_row = combo_row("Computer", computers,
                                   clawt_json_string(agent, "computer",
                                                     "none"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->computer_row);

    /*
     * The image, for a container agent.  Built only when there is one:
     * an agent that is not a container has nothing to show, and the
     * inspector is rebuilt whenever the selection changes, so switching
     * the computer type and saving brings the row in on the next
     * refresh.
     */
    g_free(self->inspector_computer);
    self->inspector_computer =
        g_strdup(clawt_json_string(agent, "computer", "none"));

    if (g_strcmp0(self->inspector_computer, "container") == 0)
        image_chooser_build(&self->inspector_image, self, group,
                            clawt_json_string(agent, "image", NULL));
    else
        self->inspector_image.row = NULL;

    /*
     * The VM's disk, size and address.  Only shown for a VM: no other
     * backend reads these, and a row that quietly does nothing is worse
     * than no row.
     */
    self->inspector_disk.row = NULL;
    self->vm_cpus_row = NULL;
    self->vm_memory_row = NULL;
    self->vm_disk_row = NULL;
    self->vm_resolution_row = NULL;
    self->vm_ssh_host_row = NULL;
    self->vm_desktop_row = NULL;
    self->vm_desktop_input_row = NULL;

    if (g_strcmp0(self->inspector_computer, "vm") == 0) {
        disk_chooser_build(&self->inspector_disk, self, group,
                           clawt_json_string(agent, "vm_image", NULL));

        self->vm_cpus_row = entry_row(
            "Cores", clawt_json_string(agent, "vm_cpus", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_cpus_row);

        self->vm_memory_row = entry_row(
            "Memory (MB)", clawt_json_string(agent, "vm_memory_mb", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_memory_row);

        self->vm_disk_row = entry_row(
            "Disk (GB)", clawt_json_string(agent, "vm_disk_gb", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_disk_row);

        /*
         * Beside the other things the machine is made of, because that is
         * what it is -- and unlike almost everything else about a VM it
         * is not baked into the cloud-init seed, so it applies at the
         * guest's next boot rather than needing the machine rebuilt.
         */
        self->vm_resolution_row = resolution_row(
            clawt_json_string(agent, "vm_resolution", "1280x800"));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_resolution_row);

        /*
         * A cloud image has no desktop at all, so this installs one --
         * GNOME, GDM and an autologin -- on the guest's first boot. It
         * takes a while and it wants the memory and disk above.
         */
        self->vm_desktop_row = switch_row(
            "Desktop in the VM",
            "Installs GNOME and logs it in, then lets this agent see it. "
            "Built on first boot, so give it time.",
            json_object_has_member(agent, "desktop_enabled")
                ? json_object_get_boolean_member(agent, "desktop_enabled")
                : FALSE);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_desktop_row);

        /*
         * Separate from seeing it, and off by default.  An agent that can
         * screenshot but not click is a genuinely useful amount of access
         * and a much smaller grant.
         */
        self->vm_desktop_input_row = switch_row(
            "Let it click and type",
            "Without this the agent can take screenshots and list windows "
            "but cannot act.",
            json_object_has_member(agent, "desktop_input")
                ? json_object_get_boolean_member(agent, "desktop_input")
                : FALSE);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_desktop_input_row);

        /*
         * Left empty, clawtilla forwards a port to the guest itself.  It
         * is here for the VM that lives somewhere clawtilla did not put
         * it.
         */
        self->vm_ssh_host_row = entry_row(
            "SSH address (optional)",
            clawt_json_string(agent, "vm_ssh_host", ""));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->vm_ssh_host_row);
    }

    self->restart_row = combo_row("Restart", restarts,
                                  clawt_json_string(agent, "restart",
                                                    "on-failure"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->restart_row);

    self->autostart_row = switch_row(
        "Start with the daemon", NULL,
        json_object_has_member(agent, "autostart")
            ? json_object_get_boolean_member(agent, "autostart") : TRUE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->autostart_row);

    /*
     * Which team, and what standing on it. Beside the chief switch
     * because the three answer one question between them: who this
     * agent may hand work to.
     */
    {
        GtkStringList *choices;

        g_clear_pointer(&self->team_ids, g_strfreev);
        choices = team_choices(self, clawt_json_string(agent, "team", ""),
                               &self->team_ids);

        self->team_row = adw_combo_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->team_row),
                                      "Team");
        adw_combo_row_set_model(ADW_COMBO_ROW(self->team_row),
                                G_LIST_MODEL(choices));
        adw_combo_row_set_selected(
            ADW_COMBO_ROW(self->team_row),
            team_index_of(self->team_ids,
                          clawt_json_string(agent, "team", "")));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->team_row);
    }

    {
        static const gchar *const roles[] = { "member", "lead", NULL };

        self->team_role_row = combo_row(
            "Role on that team", roles,
            clawt_json_string(agent, "team_role", "member"));
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(self->team_role_row),
            "A lead assigns work inside its own team. A member talks to "
            "anyone and assigns to nobody.");
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                                  self->team_role_row);
    }

    self->chief_row = switch_row(
        "Chief of staff", "Hands work to the other agents",
        json_object_has_member(agent, "chief_of_staff") &&
        json_object_get_boolean_member(agent, "chief_of_staff"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), self->chief_row);

    /*
     * Beside the chief-of-staff switch, because that is where somebody
     * looking for it goes.
     *
     * Being the chief of staff and being allowed to create agents are
     * two settings, and the first is the one with the obvious name -- a
     * person enabled it, asked their chief to make an agent, and was
     * told it had no such tool. Which was true: the tool is gated on
     * this one, and nothing on screen said so.
     */
    self->manage_fleet_row = switch_row(
        "May create agents",
        "Adds clawtilla_create_agent. It can give a new agent a container "
        "or a VM, which is a machine that runs code.",
        json_object_has_member(agent, "manage_fleet") &&
        json_object_get_boolean_member(agent, "manage_fleet"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->manage_fleet_row);

    save = gtk_button_new_with_label("Save changes");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_margin_top(save, 12);
    g_signal_connect(save, "clicked", G_CALLBACK(on_save_agent), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), save);

    gtk_box_append(self->inspector, group);

    /* Credentials: named, never valued. */
    if (json_object_has_member(agent, "credentials")) {
        JsonObject *credentials = json_object_get_object_member(agent,
                                                                "credentials");
        g_autoptr(GList) names = json_object_get_members(credentials);
        GtkWidget *creds = adw_preferences_group_new();
        GList *l;

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(creds),
                                        "Credentials");
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(creds),
            "Shown by reference. clawtilla never sends the values to a "
            "client, so nothing secret can end up in a screenshot.");

        for (l = names; l != NULL; l = l->next)
            adw_preferences_group_add(
                ADW_PREFERENCES_GROUP(creds),
                info_row(l->data,
                         json_object_get_string_member(credentials,
                                                       l->data)));

        if (names != NULL)
            gtk_box_append(self->inspector, creds);
    }

    build_agent_integrations(self);

    if (clawt_json_string(payload, "computer_detail", NULL) != NULL) {
        GtkWidget *detail = adw_preferences_group_new();
        GtkWidget *label = gtk_label_new(
            clawt_json_string(payload, "computer_detail", ""));

        adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(detail),
                                        "Computer");
        gtk_label_set_wrap(GTK_LABEL(label), TRUE);
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_selectable(GTK_LABEL(label), TRUE);
        gtk_widget_set_margin_top(label, 6);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(detail), label);

        gtk_box_append(self->inspector, detail);
    }

    /*
     * Offered whenever there is a computer configured, not only when one
     * currently exists -- the case it is most wanted for is a guest that
     * has gone, where there is nothing left to report on above.
     */
    if (g_strcmp0(clawt_json_string(agent, "computer", "none"),
                  "none") != 0) {
        GtkWidget *rebuild_group = adw_preferences_group_new();
        GtkWidget *rebuild = gtk_button_new_with_label("Rebuild computer");

        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(rebuild_group),
            "Destroys it and builds it again from the image. The only way "
            "to apply the login, the desktop or the package list, which "
            "are read once at first boot.");

        gtk_widget_add_css_class(rebuild, "destructive-action");
        gtk_widget_set_halign(rebuild, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(rebuild, 6);

        /* Destroying the machine underneath a running agent is not a
         * thing to do carefully; it is a thing not to offer. */
        gtk_widget_set_sensitive(
            rebuild, g_strcmp0(clawt_json_string(agent, "state", ""),
                               "stopped") == 0);

        g_signal_connect(rebuild, "clicked",
                         G_CALLBACK(on_rebuild_computer), self);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(rebuild_group),
                                  rebuild);

        gtk_box_append(self->inspector, rebuild_group);
    }

    /* ── Running it ── */
    actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(GTK_WIDGET(actions), 12);
    gtk_widget_set_halign(actions, GTK_ALIGN_CENTER);

    /*
     * Every control's sensitivity comes from what the agent can actually
     * do, so nothing here offers something that would fail.
     */
    gtk_box_append(GTK_BOX(actions),
                   action_button(self, "Start", "agent.start",
                                 !is_shadow &&
                                 g_strcmp0(state, "running") != 0,
                                 is_shadow
                                     ? "this agent's configuration could "
                                       "not be understood"
                                     : "already running"));
    gtk_box_append(GTK_BOX(actions),
                   action_button(self, "Stop", "agent.stop",
                                 g_strcmp0(state, "stopped") != 0 &&
                                 !is_shadow, "not running"));
    gtk_box_append(GTK_BOX(actions),
                   action_button(self, "Restart", "agent.restart",
                                 !is_shadow,
                                 "this agent cannot start"));

    gtk_box_append(self->inspector, actions);

    build_mounts(self, self->inspector_computer);
    build_files(self);

    /* ── Removing it ── */
    /*
     * Built here rather than in the "Settings" group above, so the
     * generated rows sit apart from the hand-written ones and nobody
     * has to wonder which is which.
     */
    build_schema_rows(self,
                      (payload != NULL &&
                       json_object_has_member(payload, "settings"))
                      ? json_object_get_object_member(payload, "settings")
                      : NULL);

    danger = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(danger),
                                    "Danger zone");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(danger),
        "Removing an agent stops it and takes it out of every room. Its "
        "mailbox and transcripts stay on disk.");

    delete_button = gtk_button_new_with_label("Delete this agent");
    gtk_widget_add_css_class(delete_button, "destructive-action");
    gtk_widget_set_margin_top(delete_button, 6);
    g_signal_connect(delete_button, "clicked", G_CALLBACK(on_delete_agent),
                     self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(danger), delete_button);

    gtk_box_append(self->inspector, danger);
}

/* ── Mailbox ─────────────────────────────────────────────────────── */

static void
on_mailbox_item_action(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *item_id = g_object_get_data(G_OBJECT(button), "item-id");
    const gchar *kind = g_object_get_data(G_OBJECT(button), "kind");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(
        self, kind,
        clawt_build_payload("agent", self->selected_agent, "item", item_id,
                            NULL));

    if (reply != NULL)
        refresh_selected(self);
}

/*
 * Every expired item, across every mailbox.
 *
 * Fleet-wide rather than per-agent because that is what the daemon
 * offers -- a sweep is a sweep -- so the button says so.
 */
static void
on_mailbox_purge(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    /*
     * Per-agent: the daemon resolves a mailbox from the payload and
     * refuses without one. A fleet-wide sweep is not a thing it offers.
     */
    reply = clawt_window_request(
        self, "mailbox.purge",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    {
        g_autofree gchar *said = g_strdup_printf(
            "Purged %" G_GINT64_FORMAT " expired item(s).",
            json_object_get_int_member(clawt_payload_of(reply), "purged"));

        clawt_window_toast(self, said);
    }

    refresh_selected(self);
}

/*
 * Adds one mailbox row.
 *
 * Shared by the waiting list and the dead-letter list, which differ only
 * in whether requeueing is offered -- an item that has not run out of
 * attempts has nothing to be put back into.
 */
static void
add_mailbox_row(ClawtWindow *self, GtkListBox *list, JsonObject *item,
                gboolean dead)
{
    GtkWidget *row = adw_action_row_new();
    GtkWidget *ack = gtk_button_new_with_label("Ack");
    g_autofree gchar *title = NULL;

    title = g_strdup_printf("from %s", clawt_json_string(item, "from", "?"));
    set_row_text(row, title, clawt_json_string(item, "body", ""));

    if (clawt_json_string(item, "last_error", NULL) != NULL) {
        GtkWidget *warn = badge("failed", "error",
                                clawt_json_string(item, "last_error", ""));

        adw_action_row_add_prefix(ADW_ACTION_ROW(row), warn);
    }

    if (dead) {
        GtkWidget *requeue = gtk_button_new_with_label("Requeue");

        g_object_set_data_full(G_OBJECT(requeue), "item-id",
                               g_strdup(clawt_json_string(item, "id", "")),
                               g_free);
        g_object_set_data(G_OBJECT(requeue), "kind", "mailbox.requeue");
        g_signal_connect(requeue, "clicked",
                         G_CALLBACK(on_mailbox_item_action), self);
        gtk_widget_set_valign(requeue, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), requeue);
    }

    g_object_set_data_full(G_OBJECT(ack), "item-id",
                           g_strdup(clawt_json_string(item, "id", "")),
                           g_free);
    g_object_set_data(G_OBJECT(ack), "kind", "mailbox.ack");
    g_signal_connect(ack, "clicked", G_CALLBACK(on_mailbox_item_action),
                     self);
    gtk_widget_set_valign(ack, GTK_ALIGN_CENTER);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), ack);

    gtk_list_box_append(list, row);
}

static void
refresh_mailbox_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *items;
    guint i;

    clear_list(self->mailbox_list);

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "mailbox.list",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    items = json_object_get_array_member(clawt_payload_of(reply), "items");

    {
        gint64 depth = json_object_get_int_member(clawt_payload_of(reply),
                                                  "depth");
        g_autofree gchar *summary = g_strdup_printf(
            "%" G_GINT64_FORMAT " message(s) waiting", depth);

        gtk_label_set_text(self->mailbox_summary, summary);
    }

    for (i = 0; i < json_array_get_length(items); i++)
        add_mailbox_row(self, self->mailbox_list,
                        json_array_get_object_element(items, i), FALSE);

    /*
     * Dead letters in the same list, after the waiting ones.  Nothing is
     * dropped silently, so an item that ran out of attempts has to be
     * somewhere a person can see it and put it back.
     */
    {
        g_autoptr(JsonNode) dead = clawt_window_request(
            self, "mailbox.dead",
            clawt_build_payload("agent", self->selected_agent, NULL));

        if (dead != NULL) {
            JsonArray *letters = json_object_get_array_member(
                clawt_payload_of(dead), "items");

            for (i = 0; i < json_array_get_length(letters); i++)
                add_mailbox_row(self, self->mailbox_list,
                                json_array_get_object_element(letters, i),
                                TRUE);
        }
    }
}

static void
refresh_mailbox(ClawtWindow *self)
{
    if (!refresh_enter(self, CLAWT_REFRESH_MAILBOX))
        return;

    do {
        refresh_mailbox_once(self);
    } while (refresh_repeat(self, CLAWT_REFRESH_MAILBOX));
}

/* ── Computer ────────────────────────────────────────────────────── */

static void
on_exec(GtkWidget *widget, gpointer user_data)
{
    ClawtWindow *self = user_data;
    g_autoptr(JsonNode) reply = NULL;
    GtkTextBuffer *buffer;
    GtkTextIter end;
    const gchar *command;

    (void)widget;

    if (self->selected_agent == NULL)
        return;

    command = gtk_editable_get_text(GTK_EDITABLE(self->exec_entry));

    if (command == NULL || *command == '\0')
        return;

    buffer = gtk_text_view_get_buffer(self->exec_output);
    gtk_text_buffer_get_end_iter(buffer, &end);

    {
        g_autofree gchar *echo = g_strdup_printf("$ %s\n", command);

        gtk_text_buffer_insert(buffer, &end, echo, -1);
    }

    reply = clawt_window_request(
        self, "computer.exec",
        clawt_build_payload("agent", self->selected_agent, "command",
                            command, NULL));

    if (reply == NULL)
        return;

    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end,
                           clawt_json_string(clawt_payload_of(reply),
                                             "stdout", ""), -1);

    /*
     * stderr is shown too.  A console that swallowed it would leave a
     * failing command looking like one that produced nothing.
     */
    gtk_text_buffer_get_end_iter(buffer, &end);
    gtk_text_buffer_insert(buffer, &end,
                           clawt_json_string(clawt_payload_of(reply),
                                             "stderr", ""), -1);

    gtk_editable_set_text(GTK_EDITABLE(self->exec_entry), "");
}

static void
refresh_computer(ClawtWindow *self, JsonObject *agent)
{
    const gchar *caps = clawt_json_string(agent, "caps", "");
    gboolean has_computer = strstr(caps, "computer") != NULL;

    gtk_widget_set_sensitive(GTK_WIDGET(self->exec_entry), has_computer);

    gtk_label_set_text(self->computer_state,
                       has_computer
                           ? clawt_json_string(agent, "computer", "none")
                           : "This agent has no computer.");
}

/* ── Tasks ───────────────────────────────────────────────────────── */

/*
 * Cancels a task that is still going.
 *
 * Offered only while it is, because cancelling a finished one is not a
 * refusal the daemon needs to explain -- it is a button that should not
 * have been there.
 */
static void         refresh_tasks(ClawtWindow *self);

static void
on_task_cancel(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *task_id = g_object_get_data(G_OBJECT(button), "task-id");
    g_autoptr(JsonNode) reply = NULL;

    reply = clawt_window_request(self, "task.cancel",
                                 clawt_build_payload("task", task_id, NULL));

    if (reply != NULL)
        refresh_tasks(self);
}

static void
refresh_tasks_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *tasks;
    guint i;

    clear_list(self->task_list);

    reply = clawt_window_request(self, "task.list", NULL);

    if (reply == NULL)
        return;

    tasks = json_object_get_array_member(clawt_payload_of(reply), "tasks");

    for (i = 0; i < json_array_get_length(tasks); i++) {
        JsonObject *task = json_array_get_object_element(tasks, i);
        GtkWidget *row = adw_action_row_new();
        g_autofree gchar *title = NULL;

        title = g_strdup_printf("%s → %s",
                                clawt_json_string(task, "origin", "?"),
                                clawt_json_string(task, "assignee", "?"));

        set_row_text(row, title, clawt_json_string(task, "prompt", ""));
        adw_action_row_add_suffix(
            ADW_ACTION_ROW(row),
            badge(clawt_json_string(task, "state", "?"), "dim-label",
                  clawt_json_string(task, "reason", "")));

        {
            const gchar *state = clawt_json_string(task, "state", "");

            if (g_strcmp0(state, "running") == 0 ||
                g_strcmp0(state, "pending") == 0) {
                GtkWidget *cancel = gtk_button_new_with_label("Cancel");

                g_object_set_data_full(
                    G_OBJECT(cancel), "task-id",
                    g_strdup(clawt_json_string(task, "id", "")), g_free);
                g_signal_connect(cancel, "clicked",
                                 G_CALLBACK(on_task_cancel), self);
                gtk_widget_set_valign(cancel, GTK_ALIGN_CENTER);
                adw_action_row_add_suffix(ADW_ACTION_ROW(row), cancel);
            }
        }

        gtk_list_box_append(self->task_list, row);
    }
}

static void
refresh_tasks(ClawtWindow *self)
{
    if (!refresh_enter(self, CLAWT_REFRESH_TASKS))
        return;

    do {
        refresh_tasks_once(self);
    } while (refresh_repeat(self, CLAWT_REFRESH_TASKS));
}

/* ── Selection ───────────────────────────────────────────────────── */

static void
refresh_selected_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *agent;

    if (self->selected_agent == NULL)
        return;

    reply = clawt_window_request(
        self, "agent.show",
        clawt_build_payload("agent", self->selected_agent, NULL));

    if (reply == NULL)
        return;

    agent = json_object_get_object_member(clawt_payload_of(reply), "agent");

    build_inspector(self, agent, clawt_payload_of(reply));
    refresh_computer(self, agent);
    refresh_mailbox(self);
    refresh_tasks(self);
    refresh_routines(self);
    refresh_flow(self);
}

static void
refresh_selected(ClawtWindow *self)
{
    if (!refresh_enter(self, CLAWT_REFRESH_SELECTED))
        return;

    do {
        refresh_selected_once(self);
    } while (refresh_repeat(self, CLAWT_REFRESH_SELECTED));
}

static void
select_agent(ClawtWindow *self, const gchar *agent_id)
{
    if (agent_id == NULL || *agent_id == '\0')
        return;

    /*
     * Re-selecting what is already shown has to be a no-op, and not by
     * luck: refresh_agents() rebuilds the sidebar on every daemon event
     * and restores the selection, which emits ::row-selected again.
     * Reloading there would drop the transcript each time an event
     * arrived, and load_history() iterates the main context, so it would
     * re-enter this function while the first call was still running.
     */
    if (g_strcmp0(agent_id, self->selected_agent) == 0)
        return;

    /* Keep what was being written to the agent we are leaving. */
    if (self->selected_agent != NULL) {
        g_autofree gchar *draft = entry_text(self);

        if (draft != NULL && draft[0] != '\0')
            g_hash_table_insert(self->drafts,
                                g_strdup(self->selected_agent),
                                g_steal_pointer(&draft));
        else
            g_hash_table_remove(self->drafts, self->selected_agent);
    }

    g_free(self->selected_agent);
    self->selected_agent = g_strdup(agent_id);

    /*
     * Opening a conversation is the only thing that clears its count.
     *
     * Not scrolling, not the window gaining focus, not time passing: a
     * counter that decays on its own is a counter you stop trusting.
     */
    if (g_hash_table_remove(self->unread, agent_id))
        update_unread_tab(self);

    g_free(self->selected_avatar);
    self->selected_avatar = g_strdup(agent_row_data(self, agent_id,
                                                    "agent-avatar"));
    g_free(self->selected_color);
    self->selected_color = g_strdup(agent_row_data(self, agent_id,
                                                   "agent-color"));

    entry_set_text(self, g_hash_table_lookup(self->drafts, agent_id));

    adw_window_title_set_title(
        ADW_WINDOW_TITLE(g_object_get_data(G_OBJECT(self), "title")),
        agent_id);

    load_history(self);
    refresh_selected(self);

    /*
     * On a narrow window the sidebar is a drawer, so close it.
     *
     * That is the client dismissing a drawer that is in the way, not the
     * operator saying they want no agent list, so it deliberately does
     * not touch sidebar_open: widen the window again and the list comes
     * back.  Recording it would make an affordance into a preference.
     */
    if (adw_overlay_split_view_get_collapsed(self->split)) {
        self->sidebar_transient = TRUE;
        adw_overlay_split_view_set_show_sidebar(self->split, FALSE);
        self->sidebar_transient = FALSE;
    }
}

/* ── The sidebar's context menu ───────────────────────────────────── */

/*
 * Start, stop and restart without leaving the conversation.
 *
 * These live on the Agent tab too, but reaching for them meant leaving
 * the chat, acting, and coming back -- three navigations for the thing
 * you do most often to an agent.
 */
static void
on_menu_action(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));

    (void)parameter;

    if (g_strcmp0(name, "delete") == 0) {
        delete_agent(self);
        return;
    }

    {
        g_autofree gchar *kind = g_strconcat("agent.", name, NULL);

        agent_action(self, kind);
    }
}

static void
enable_agent_action(ClawtWindow *self, const gchar *name, gboolean enabled)
{
    GAction *action = g_action_map_lookup_action(
        G_ACTION_MAP(self->agent_actions), name);

    if (action != NULL)
        g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

/*
 * A menu that offers what would fail is worse than one that greys it
 * out, so the same rules the inspector's buttons use apply here.  A
 * shadow agent cannot run at all: its configuration did not parse, and
 * every lifecycle verb would be refused by the daemon.
 */
static void
set_agent_action_states(ClawtWindow *self, const gchar *state)
{
    gboolean shadow = g_strcmp0(state, "shadow") == 0;

    enable_agent_action(self, "start",
                        !shadow && g_strcmp0(state, "running") != 0);
    enable_agent_action(self, "stop",
                        !shadow && g_strcmp0(state, "stopped") != 0);
    enable_agent_action(self, "restart", !shadow);
    enable_agent_action(self, "delete", TRUE);
}

/*
 * Fills the Team submenu, ticking the team this agent is already on.
 *
 * The entries are radio items -- a stateful action taking the team id,
 * whose state is the current team -- so the menu answers "which team is
 * this on" as well as offering to change it.  A plain list of teams
 * would make somebody open the inspector to find out where they were
 * before deciding where to go.
 */
static void
fill_team_menu(ClawtWindow *self, const gchar *current)
{
    GAction *action;
    JsonArray *teams = NULL;
    guint i;

    if (self->agent_menu_teams == NULL)
        return;

    g_menu_remove_all(self->agent_menu_teams);

    if (self->teams_seen != NULL)
        teams = json_object_get_array_member(
            clawt_payload_of(self->teams_seen), "teams");

    /*
     * "No team" first, and always present.  It is how an agent comes off
     * a team, and it is where the chief of staff lives -- the same
     * reason the sidebar sorts the teamless agents above every team.
     */
    {
        g_autoptr(GMenuItem) item = g_menu_item_new("No team", NULL);

        g_menu_item_set_action_and_target_value(item, "agent.team",
                                                g_variant_new_string(""));
        g_menu_append_item(self->agent_menu_teams, item);
    }

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        JsonObject *team = json_array_get_object_element(teams, i);
        const gchar *id = clawt_json_string(team, "id", NULL);
        g_autoptr(GMenuItem) item = NULL;

        if (id == NULL || *id == '\0')
            continue;

        item = g_menu_item_new(clawt_json_string(team, "name", id), NULL);
        g_menu_item_set_action_and_target_value(item, "agent.team",
                                                g_variant_new_string(id));
        g_menu_append_item(self->agent_menu_teams, item);
    }

    /*
     * A team nobody declared still gets an entry, under the id the agent
     * named, so that the menu can show where this agent actually is.
     * Without it the tick lands on "No team" and moving the agent away
     * looks like it did nothing -- the typo in `agents.team` being
     * exactly what somebody opened this menu to fix.
     */
    if (current != NULL && *current != '\0' &&
        !team_is_declared(teams, current)) {
        g_autoptr(GMenuItem) item = g_menu_item_new(current, NULL);

        g_menu_item_set_action_and_target_value(item, "agent.team",
                                                g_variant_new_string(current));
        g_menu_append_item(self->agent_menu_teams, item);
    }

    action = g_action_map_lookup_action(G_ACTION_MAP(self->agent_actions),
                                        "team");

    if (action != NULL)
        g_simple_action_set_state(
            G_SIMPLE_ACTION(action),
            g_variant_new_string(current != NULL ? current : ""));
}

/*
 * Moves the right-clicked agent onto a team.
 */
static void
on_menu_team(GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *team = g_variant_get_string(parameter, NULL);
    g_autoptr(GVariant) state = g_action_get_state(G_ACTION(action));
    g_autofree gchar *message = NULL;

    if (self->selected_agent == NULL)
        return;

    /*
     * Activating the item already ticked is an ordinary thing to do with
     * a radio menu, and writing the value back would render every agent,
     * emit a change and toast about a move that did not happen.
     */
    if (state != NULL &&
        g_strcmp0(g_variant_get_string(state, NULL), team) == 0)
        return;

    if (!apply_setting(self, "team", team))
        return;

    g_simple_action_set_state(action, g_variant_new_string(team));

    message = (*team != '\0')
              ? g_strdup_printf("%s moved to %s.", self->selected_agent, team)
              : g_strdup_printf("%s taken off its team.",
                                self->selected_agent);

    clawt_window_toast(self, message);

    /* The sidebar groups by team, so the row belongs somewhere else now. */
    refresh_agents(self);
}

static void
popup_agent_menu(ClawtWindow *self, gdouble x, gdouble y)
{
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(self->sidebar, (gint)y);
    g_autofree gchar *state = NULL;
    g_autofree gchar *team = NULL;
    const gchar *agent_id;
    GdkRectangle rect;

    if (row == NULL || self->agent_menu == NULL)
        return;

    agent_id = g_object_get_data(G_OBJECT(row), "agent-id");

    /* The "No agents yet" placeholder is a row like any other. */
    if (agent_id == NULL)
        return;

    /*
     * Copied off the row before anything else, because selecting it
     * loads the transcript, which iterates the main context, which lets
     * a refresh rebuild the sidebar and free this row underneath us.
     */
    state = g_strdup(g_object_get_data(G_OBJECT(row), "agent-state"));
    team = g_strdup(g_object_get_data(G_OBJECT(row), "agent-team"));

    /* Act on what was right-clicked, not on what happened to be selected. */
    gtk_list_box_select_row(self->sidebar, row);

    set_agent_action_states(self, state);
    fill_team_menu(self, team);

    rect.x = (gint)x;
    rect.y = (gint)y;
    rect.width = 1;
    rect.height = 1;

    gtk_popover_set_pointing_to(GTK_POPOVER(self->agent_menu), &rect);
    gtk_popover_popup(GTK_POPOVER(self->agent_menu));
}

static void
on_sidebar_secondary_click(GtkGestureClick *gesture, gint n_press,
                           gdouble x, gdouble y, gpointer user_data)
{
    (void)gesture;
    (void)n_press;

    popup_agent_menu(user_data, x, y);
}

/* Touch has no second button; a long press is the same gesture there. */
static void
on_sidebar_long_press(GtkGestureLongPress *gesture, gdouble x, gdouble y,
                      gpointer user_data)
{
    (void)gesture;

    popup_agent_menu(user_data, x, y);
}

static void
build_agent_menu(ClawtWindow *self)
{
    static const gchar *const names[] = { "start", "stop", "restart",
                                          "delete" };
    g_autoptr(GMenu) menu = g_menu_new();
    g_autoptr(GMenu) lifecycle = g_menu_new();
    g_autoptr(GMenu) grouping = g_menu_new();
    g_autoptr(GMenu) danger = g_menu_new();
    GtkGesture *click;
    GtkGesture *press;
    guint i;

    self->agent_actions = g_simple_action_group_new();

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(names[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_menu_action), self);
        g_action_map_add_action(G_ACTION_MAP(self->agent_actions),
                                G_ACTION(action));
    }

    /*
     * Stateful, and taking the team id as its parameter, so GTK draws
     * the entries as radio items with the current team ticked.
     */
    {
        g_autoptr(GSimpleAction) action = g_simple_action_new_stateful(
            "team", G_VARIANT_TYPE_STRING, g_variant_new_string(""));

        g_signal_connect(action, "activate", G_CALLBACK(on_menu_team), self);
        g_action_map_add_action(G_ACTION_MAP(self->agent_actions),
                                G_ACTION(action));
    }

    g_menu_append(lifecycle, "Start", "agent.start");
    g_menu_append(lifecycle, "Stop", "agent.stop");
    g_menu_append(lifecycle, "Restart", "agent.restart");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(lifecycle));

    /*
     * Its own section between the lifecycle verbs and Delete.  The
     * entries are filled in per right-click, from the fleet the sidebar
     * last saw -- an empty model here is what an unopened menu looks
     * like, not a fleet with no teams.
     */
    self->agent_menu_teams = g_menu_new();
    g_menu_append_submenu(grouping, "Team",
                          G_MENU_MODEL(self->agent_menu_teams));
    g_menu_append_section(menu, NULL, G_MENU_MODEL(grouping));

    /* Its own section, so Delete is never the neighbour of Restart. */
    g_menu_append(danger, "Delete\342\200\246", "agent.delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(danger));

    gtk_widget_insert_action_group(GTK_WIDGET(self), "agent",
                                   G_ACTION_GROUP(self->agent_actions));

    self->agent_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(self->agent_menu), FALSE);
    gtk_widget_set_halign(self->agent_menu, GTK_ALIGN_START);
    gtk_widget_set_parent(self->agent_menu, GTK_WIDGET(self->sidebar));

    click = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click),
                                  GDK_BUTTON_SECONDARY);
    g_signal_connect(click, "pressed",
                     G_CALLBACK(on_sidebar_secondary_click), self);
    gtk_widget_add_controller(GTK_WIDGET(self->sidebar),
                              GTK_EVENT_CONTROLLER(click));

    press = gtk_gesture_long_press_new();
    gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(press), TRUE);
    g_signal_connect(press, "pressed",
                     G_CALLBACK(on_sidebar_long_press), self);
    gtk_widget_add_controller(GTK_WIDGET(self->sidebar),
                              GTK_EVENT_CONTROLLER(press));
}

/* ── Alerts ──────────────────────────────────────────────────────── */

/*
 * An alert is an event that demanded attention; a routine entry is one
 * that did not.  Same stream, different weight -- so one list with a
 * filter rather than two tabs: deciding at write time which tab a thing
 * belongs in means guessing before the interesting case, and a filter
 * lets you widen after it.
 */
typedef struct {
    ClawtAlertTier tier;
    gchar     *text;
    gchar     *source;   /* the event kind, so a row says where to look */
    gchar     *agent;    /* the event's subject */
    gint64     ts;
    gboolean   read;
} Alert;

/*
 * The most recent 200 are held, matching the limit `agent.logs` already
 * defaults to so the two surfaces agree on what "recent" means.  Older
 * than that is the event log's, through `event.list`.
 */
#define ALERTS_KEPT 200

static void refresh_alerts(ClawtWindow *self);
static void update_alert_badge(ClawtWindow *self);
static guint unread_alerts(ClawtWindow *self);

static void
alert_free(gpointer data)
{
    Alert *alert = data;

    g_free(alert->text);
    g_free(alert->source);
    g_free(alert->agent);
    g_free(alert);
}

static void
clawt_window_alert(ClawtWindow *self, ClawtAlertTier tier, const gchar *source,
                   const gchar *agent, const gchar *text)
{
    Alert *alert;

    if (self->alerts == NULL || text == NULL)
        return;

    alert = g_new0(Alert, 1);
    alert->tier = tier;
    alert->text = g_strdup(text);
    alert->source = g_strdup(source != NULL ? source : "");
    alert->agent = g_strdup(agent != NULL ? agent : "");
    alert->ts = g_get_real_time();

    /*
     * An alert that lands while the panel is open has been seen as it
     * arrived, so it arrives read.
     *
     * on_alerts_shown() only fires on a show *transition*, so without
     * this the badge counts something the operator is looking straight
     * at, and keeps counting it until they close the panel and open it
     * again.  This is the same rule that already decides whether the
     * list is worth rebuilding a few lines below, applied one step
     * earlier.
     */
    alert->read = (self->alerts_split != NULL &&
                   adw_overlay_split_view_get_show_sidebar(self->alerts_split));

    /* Newest first: the list is read from the top and never scrolled to
     * the bottom, which is the opposite of a transcript. */
    g_ptr_array_insert(self->alerts, 0, alert);

    while (self->alerts->len > ALERTS_KEPT)
        g_ptr_array_remove_index(self->alerts, self->alerts->len - 1);

    update_alert_badge(self);

    /*
     * The list is only rebuilt when somebody can see it.  Every event in
     * the fleet lands here, and rebuilding two hundred rows per message
     * would be work done for nobody.
     */
    if (self->alerts_split != NULL &&
        adw_overlay_split_view_get_show_sidebar(self->alerts_split))
        refresh_alerts(self);
}

/*
 * How many *alerts* are unread -- never routine entries.
 *
 * A badge that counted the routine stream would be permanently non-zero
 * and would stop being read, and widening the filter is a thing the
 * operator chooses rather than something that should change the number
 * on the bell.
 */
static guint
unread_alerts(ClawtWindow *self)
{
    guint count = 0;
    guint i;

    for (i = 0; self->alerts != NULL && i < self->alerts->len; i++) {
        Alert *alert = g_ptr_array_index(self->alerts, i);

        if (!alert->read && alert->tier != CLAWT_ALERT_ROUTINE)
            count++;
    }

    return count;
}

static void
update_alert_badge(ClawtWindow *self)
{
    guint count;

    if (self->alerts_badge == NULL)
        return;

    count = unread_alerts(self);

    if (count == 0) {
        gtk_widget_set_visible(self->alerts_badge, FALSE);
        return;
    }

    {
        /*
         * Capped at 9+ here, unlike the sidebar's unread pill, because
         * this one sits on a 34px round button rather than at the end of
         * a row: three digits would be wider than the control.
         */
        g_autofree gchar *text = (count > 9)
            ? g_strdup("9+") : g_strdup_printf("%u", count);

        gtk_label_set_text(GTK_LABEL(self->alerts_badge), text);
        gtk_widget_set_visible(self->alerts_badge, TRUE);
    }
}

/* "4 minutes ago", roughly, because a wall-clock time in a list of
 * things that just happened is a number you have to subtract. */
static gchar *
alert_when(gint64 ts)
{
    gint64 seconds = (g_get_real_time() - ts) / G_USEC_PER_SEC;

    if (seconds < 60)
        return g_strdup("just now");

    if (seconds < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m ago", seconds / 60);

    if (seconds < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h ago", seconds / 3600);

    return g_strdup_printf("%" G_GINT64_FORMAT "d ago", seconds / 86400);
}

static void
on_alert_dismissed(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    Alert *alert = g_object_get_data(G_OBJECT(button), "alert");

    if (alert != NULL)
        g_ptr_array_remove(self->alerts, alert);

    update_alert_badge(self);
    refresh_alerts(self);
}

static void
on_alerts_cleared(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)button;

    g_ptr_array_set_size(self->alerts, 0);
    update_alert_badge(self);
    refresh_alerts(self);
}

static void
on_alert_scope_cleared(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)button;

    g_clear_pointer(&self->alerts_agent, g_free);
    refresh_alerts(self);
}

static void
on_alert_scoped(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *agent = g_object_get_data(G_OBJECT(button), "agent");

    g_free(self->alerts_agent);
    self->alerts_agent = g_strdup(agent);
    refresh_alerts(self);
}

static void
on_alert_filter_changed(AdwToggleGroup *group, GParamSpec *pspec,
                        gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)pspec;

    self->alerts_show_all = (adw_toggle_group_get_active(group) == 1);
    refresh_alerts(self);
}

/*
 * Reads the event log, once, when the expander is opened.
 *
 * On open rather than on every refresh: it is a round trip to the daemon
 * and every event in the fleet triggers a refresh, so filling it eagerly
 * would ask for fifty lines nobody had asked to see, repeatedly.
 */
static void
on_alert_history_expanded(GObject *object, GParamSpec *pspec,
                          gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkExpander *expander = GTK_EXPANDER(object);
    GtkWidget *body = g_object_get_data(object, "body");
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *events;
    guint i;

    (void)pspec;

    if (!gtk_expander_get_expanded(expander) || body == NULL)
        return;

    if (g_object_get_data(object, "filled") != NULL)
        return;

    reply = clawt_window_request(
        self, "event.list",
        clawt_build_payload("limit", "50",
                            self->alerts_agent != NULL ? "subject" : NULL,
                            self->alerts_agent, NULL));

    if (reply == NULL)
        return;

    g_object_set_data(object, "filled", GINT_TO_POINTER(1));

    events = json_object_get_array_member(clawt_payload_of(reply), "events");

    /* Newest first, like the live list above it. */
    for (i = json_array_get_length(events); i > 0; i--) {
        JsonObject *one = json_array_get_object_element(events, i - 1);
        g_autofree gchar *text = g_strdup_printf(
            "%s \302\267 %s", clawt_json_string(one, "kind", "?"),
            clawt_json_string(one, "subject", "the fleet"));
        GtkWidget *label = gtk_label_new(text);

        gtk_widget_add_css_class(label, "caption");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(body), label);
    }
}

/*
 * A row.
 *
 * Error and Notice get a card and a tinted disc; Routine gets neither --
 * a dim dot, caption text, one line.  If everything carries a colour,
 * colour stops meaning anything, and routine entries are the majority
 * the moment the filter widens.  Making them quiet is what keeps the
 * loud ones loud, and severity is carried by weight and container as
 * much as by hue, so it survives a colourblind reader and a Catppuccin
 * palette alike.
 */
static GtkWidget *
alert_row(ClawtWindow *self, Alert *alert)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *middle = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *text = gtk_label_new(alert->text);
    GtkWidget *meta;
    g_autofree gchar *when = alert_when(alert->ts);
    g_autofree gchar *line = NULL;

    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_label_set_xalign(GTK_LABEL(text), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(text), TRUE);
    gtk_label_set_selectable(GTK_LABEL(text), TRUE);

    if (alert->tier == CLAWT_ALERT_ROUTINE) {
        GtkWidget *dot = gtk_label_new("\342\200\242");

        gtk_widget_add_css_class(dot, "dim-label");
        gtk_widget_set_valign(dot, GTK_ALIGN_START);
        gtk_widget_add_css_class(text, "caption");
        gtk_widget_add_css_class(text, "dim-label");
        gtk_label_set_lines(GTK_LABEL(text), 2);
        gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
        gtk_box_append(GTK_BOX(box), dot);
    } else {
        GtkWidget *icon = gtk_image_new_from_icon_name(
            alert->tier == CLAWT_ALERT_ERROR ? "dialog-error-symbolic"
                                       : "dialog-information-symbolic");

        gtk_widget_add_css_class(icon, alert->tier == CLAWT_ALERT_ERROR
                                           ? "error" : "accent");
        gtk_widget_set_valign(icon, GTK_ALIGN_START);
        gtk_widget_add_css_class(row, "clawt-alert-card");

        if (!alert->read)
            gtk_widget_add_css_class(row, "clawt-alert-unread");

        gtk_box_append(GTK_BOX(box), icon);
    }

    gtk_box_append(GTK_BOX(middle), text);

    /*
     * Relative time and the source, so a row says where to go and look
     * without a second click.  The agent's name is a button: clicking it
     * narrows the panel to that agent, which is the one control the
     * scope needs -- a dropdown would be a second answer to the same
     * question.
     */
    meta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    if (alert->agent != NULL && *alert->agent != '\0') {
        GtkWidget *who = gtk_button_new_with_label(alert->agent);

        gtk_widget_add_css_class(who, "flat");
        gtk_widget_add_css_class(who, "caption");
        gtk_widget_set_tooltip_text(who, "Show only this agent");
        g_object_set_data_full(G_OBJECT(who), "agent",
                               g_strdup(alert->agent), g_free);
        g_signal_connect(who, "clicked", G_CALLBACK(on_alert_scoped), self);
        gtk_box_append(GTK_BOX(meta), who);
    }

    line = g_strdup_printf("%s \302\267 %s", when, alert->source);
    {
        GtkWidget *label = gtk_label_new(line);

        gtk_widget_add_css_class(label, "caption");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
        gtk_box_append(GTK_BOX(meta), label);
    }

    gtk_box_append(GTK_BOX(middle), meta);
    gtk_widget_set_hexpand(middle, TRUE);
    gtk_box_append(GTK_BOX(box), middle);

    {
        /*
         * Always visible rather than on hover: a control that is
         * invisible until you already know it exists is a control nobody
         * finds.
         */
        GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");

        gtk_widget_add_css_class(close, "flat");
        gtk_widget_add_css_class(close, "circular");
        gtk_widget_set_valign(close, GTK_ALIGN_START);
        gtk_widget_set_tooltip_text(close, "Dismiss");
        g_object_set_data(G_OBJECT(close), "alert", alert);
        g_signal_connect(close, "clicked", G_CALLBACK(on_alert_dismissed),
                         self);
        gtk_box_append(GTK_BOX(box), close);
    }

    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 8);
    gtk_widget_set_margin_bottom(box, 8);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);

    return row;
}

static void
refresh_alerts(ClawtWindow *self)
{
    guint shown = 0;
    guint i;

    if (self->alerts_list == NULL)
        return;

    clear_list(self->alerts_list);

    /*
     * The scope chip, when the panel has been narrowed to one agent.
     * Dismissing it is how the scope is cleared -- there is one control
     * for it and it is the one that set it.
     */
    if (self->alerts_agent != NULL) {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *chip = gtk_button_new();
        g_autofree gchar *label = g_strdup_printf("Only %s  \303\227",
                                                  self->alerts_agent);

        gtk_button_set_label(GTK_BUTTON(chip), label);
        gtk_widget_add_css_class(chip, "flat");
        gtk_widget_add_css_class(chip, "caption");
        gtk_widget_set_tooltip_text(chip, "Show the whole fleet again");
        g_signal_connect(chip, "clicked", G_CALLBACK(on_alert_scope_cleared),
                         self);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), chip);
        gtk_list_box_append(self->alerts_list, row);
    }

    for (i = 0; self->alerts != NULL && i < self->alerts->len; i++) {
        Alert *alert = g_ptr_array_index(self->alerts, i);

        if (!self->alerts_show_all && alert->tier == CLAWT_ALERT_ROUTINE)
            continue;

        if (self->alerts_agent != NULL &&
            g_strcmp0(self->alerts_agent, alert->agent) != 0)
            continue;

        gtk_list_box_append(self->alerts_list, alert_row(self, alert));
        shown++;
    }

    /*
     * And the durable copy, from the daemon's own event log.
     *
     * ClawtEventLog has recorded every published event since the daemon
     * was written, sweeps on `daemon.event_log_days`, and was read back
     * by nobody -- which is why diagnosing a message loop meant running
     * sqlite3 and grep on the host.  The panel holds the recent ones in
     * memory and reads the rest through `event.list`, so it stays a view
     * onto a store that already exists and already has a retention
     * policy rather than becoming one.
     *
     * Behind an expander: it is a round trip to the daemon and a reader
     * looking at what just happened does not want fifty older lines
     * above it.
     */
    {
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *expander = gtk_expander_new("Earlier, from the daemon\342\200\231s log");
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);

        gtk_widget_add_css_class(gtk_expander_get_label_widget(
                                     GTK_EXPANDER(expander)), "caption");
        g_object_set_data(G_OBJECT(expander), "window", self);
        g_signal_connect(expander, "notify::expanded",
                         G_CALLBACK(on_alert_history_expanded), self);
        gtk_expander_set_child(GTK_EXPANDER(expander), box);
        g_object_set_data(G_OBJECT(expander), "body", box);
        gtk_widget_set_margin_start(expander, 12);
        gtk_widget_set_margin_end(expander, 12);
        gtk_widget_set_margin_top(expander, 12);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), expander);
        gtk_list_box_append(self->alerts_list, row);
    }

    if (shown == 0) {
        /*
         * Not an AdwStatusPage: in a 320px column that is more furniture
         * than the message needs.
         */
        GtkWidget *row = gtk_list_box_row_new();
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
        GtkWidget *bell = gtk_image_new_from_icon_name(
            "preferences-system-notifications-symbolic");
        GtkWidget *label = gtk_label_new(
            self->alerts_show_all ? "Nothing has happened."
                                  : "Nothing has needed you.");

        gtk_image_set_pixel_size(GTK_IMAGE(bell), 32);
        gtk_widget_add_css_class(bell, "dim-label");
        gtk_widget_add_css_class(label, "caption");
        gtk_widget_add_css_class(label, "dim-label");
        gtk_widget_set_margin_top(box, 48);
        gtk_box_append(GTK_BOX(box), bell);
        gtk_box_append(GTK_BOX(box), label);
        gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(self->alerts_list, row);
    }
}

/*
 * A split view that stops being collapsed opens its sidebar, whether or
 * not anybody asked it to.
 *
 * adw_breakpoint_add_setters() restores the previous value when its
 * breakpoint stops matching, so every upward crossing of the widths set
 * below drives collapsed TRUE -> FALSE, and that transition sets
 * show-sidebar TRUE inside libadwaita.  Measured on 1.8.7: a split with
 * show-sidebar FALSE that is collapsed and then uncollapsed comes back
 * showing, while one that was never collapsed does not.  So an operator
 * who closed the panel finds it open again after a resize they made for
 * an unrelated reason.
 *
 * These put the remembered choice back.  They are on notify::collapsed
 * rather than notify::show-sidebar because libadwaita emits the collapsed
 * notify first and the show notify after -- so the correction is already
 * made by the time anything downstream reads the property, and nothing
 * has to tell the toolkit's write apart from a person's.  Move this to
 * the other signal and it will still compile, still look right, and
 * quietly do nothing.
 */
static void
on_alerts_collapsed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)pspec;

    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(object),
                                            self->alerts_open);
}

static void
on_sidebar_collapsed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)pspec;

    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(object),
                                            self->sidebar_open);
}

/* The agent list's half of the same memory. */
static void
on_sidebar_shown(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)pspec;

    if (self->sidebar_transient)
        return;

    self->sidebar_open = adw_overlay_split_view_get_show_sidebar(
        ADW_OVERLAY_SPLIT_VIEW(object));
}

/*
 * Opening the panel marks everything read.
 *
 * The badge is "how much have you not seen", and you have now seen it.
 * Rows stay until they are dismissed, so nothing is lost by the count
 * going to zero.
 */
static void
on_alerts_shown(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    guint i;

    (void)pspec;

    /*
     * Whatever the panel has settled at is what the operator wants, and
     * this runs after the notify::collapsed handler below has already
     * put their choice back -- so what is recorded here is the choice,
     * not libadwaita's overwrite of it.
     */
    self->alerts_open = adw_overlay_split_view_get_show_sidebar(
        ADW_OVERLAY_SPLIT_VIEW(object));

    if (!self->alerts_open)
        return;

    for (i = 0; self->alerts != NULL && i < self->alerts->len; i++)
        ((Alert *)g_ptr_array_index(self->alerts, i))->read = TRUE;

    update_alert_badge(self);
    refresh_alerts(self);
}

static GtkWidget *
build_alerts_panel(ClawtWindow *self)
{
    GtkWidget *view = adw_toolbar_view_new();
    GtkWidget *header = adw_header_bar_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *filter = adw_toggle_group_new();
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header),
                                                FALSE);
    adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header),
                                    gtk_label_new("Alerts"));

    {
        GtkWidget *clear = gtk_button_new_with_label("Clear all");

        gtk_widget_add_css_class(clear, "flat");
        g_signal_connect(clear, "clicked", G_CALLBACK(on_alerts_cleared),
                         self);
        adw_header_bar_pack_end(ADW_HEADER_BAR(header), clear);
    }

    /*
     * Two options, not five.  A panel that opens onto every routine
     * event is noise; one that opens onto only errors hides what you
     * came for.
     */
    {
        AdwToggle *alerts_only = adw_toggle_new();
        AdwToggle *everything = adw_toggle_new();

        adw_toggle_set_label(alerts_only, "Alerts");
        adw_toggle_set_label(everything, "Everything");
        adw_toggle_group_add(ADW_TOGGLE_GROUP(filter), alerts_only);
        adw_toggle_group_add(ADW_TOGGLE_GROUP(filter), everything);
    }
    adw_toggle_group_set_active(ADW_TOGGLE_GROUP(filter), 0);
    gtk_widget_set_margin_start(filter, 12);
    gtk_widget_set_margin_end(filter, 12);
    gtk_widget_set_margin_top(filter, 8);
    gtk_widget_set_margin_bottom(filter, 4);
    g_signal_connect(filter, "notify::active",
                     G_CALLBACK(on_alert_filter_changed), self);
    self->alerts_filter = filter;

    self->alerts_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->alerts_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->alerts_list), "navigation-sidebar");

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->alerts_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_box_append(GTK_BOX(body), filter);
    gtk_box_append(GTK_BOX(body), scroll);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), header);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), body);

    refresh_alerts(self);

    return view;
}

/* ── Events ──────────────────────────────────────────────────────── */

static void
on_daemon_event(ClawtClient *client, ClawtEvent *event, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *kind = clawt_event_get_kind(event);

    (void)client;

    /*
     * The routine stream.
     *
     * The client already receives every event the daemon publishes and
     * acted on five kinds, dropping the rest -- so an agent stopping, or
     * erroring, produced nothing anywhere.  Recorded here, quietly, so
     * the panel can answer "what has the fleet been doing" as well as
     * "what needed me".  It is one branch rather than a new surface,
     * which is the argument for a panel over relocating a toast.
     *
     * `image.progress` is excluded: a download emits one per percent and
     * would fill the whole list with one file.
     */
    if (self->alerts != NULL) {
        ClawtAlertTier tier = clawt_alert_tier_for_event(event);

        /*
         * The two loud kinds have branches of their own further down,
         * which say what happened in words rather than naming the kind.
         * Everything the classifier calls routine or a notice is
         * recorded here, quietly.
         */
        if (tier == CLAWT_ALERT_ROUTINE || tier == CLAWT_ALERT_NOTICE) {
            const gchar *subject = clawt_event_get_subject(event);
            g_autofree gchar *line = NULL;

            if (g_strcmp0(kind, "agent.state") == 0) {
                const gchar *state = clawt_event_get_detail(event, "state");

                line = g_strdup_printf("%s is %s",
                                       subject != NULL ? subject : "?",
                                       state != NULL ? state : "?");
            } else {
                line = g_strdup_printf("%s \302\267 %s",
                                       kind != NULL ? kind : "?",
                                       subject != NULL ? subject
                                                       : "the fleet");
            }

            clawt_window_alert(self, tier, kind, subject, line);
        }
    }

    if (g_strcmp0(kind, "message") == 0) {
        const gchar *from = clawt_event_get_detail(event, "from");
        const gchar *body = clawt_event_get_detail(event, "body");

        /*
         * Matched on the room, and only on the room.
         *
         * This used to append anything whose sender *or* subject was the
         * selected agent, which meant a reply from that agent to one of
         * its peers was drawn in the user's own chat with it -- the
         * message had been routed correctly the whole time, the
         * transcript was showing a conversation it was not part of. The
         * rest change the sidebar's queue badge, which is what tells you
         * something happened elsewhere.
         */
        if (self->selected_room != NULL &&
            g_strcmp0(clawt_event_get_subject(event),
                      self->selected_room) == 0 &&
            !already_shown(self, clawt_event_get_detail(event, "id"))) {
            /*
             * Before the message, so the rule sits above the first
             * thing the reader has not seen rather than below it.
             */
            note_arrival(self);

            append_message(self, from != NULL ? from : "?",
                           body != NULL ? body : "",
                           g_strcmp0(from, "user") == 0, 0);

            /*
             * The reply is the end of the turn.  libreclaw drops the
             * typing indicator too, but the message overtakes it often
             * enough that relying on the indicator alone leaves a
             * spinner running under an answer that has already arrived.
             */
            if (g_strcmp0(from, self->selected_agent) == 0)
                set_activity(self, NULL);

            queue_scroll(self);
        }

        /*
         * Anything that was not for the room on screen counts.  Done
         * before the refresh below, because that is what draws the pill.
         */
        note_unread(self, event, from);

        /*
         * The flow page is refreshed for every message, not only the one
         * on screen: it is a list of what the fleet has been doing, and
         * a conversation that does not move when the agents talk is the
         * one thing it must not be.
         */
        refresh_flow(self);
        refresh_agents(self);
        return;
    }

    /*
     * A download's progress moves the bar it belongs to and nothing else.
     * Rebuilding the list on every one of a hundred events would fight
     * whoever is using that window.
     */
    if (g_strcmp0(kind, "image.progress") == 0) {
        GtkWidget *bar = g_hash_table_lookup(self->settings_bars,
                                             clawt_event_get_subject(event));
        gint64 total = clawt_event_get_detail_int(event, "total");

        /*
         * No bar means this download was begun somewhere else -- the CLI,
         * or another window -- so there is no row for it yet.  Building
         * the list once gives it one, and every later event for it finds
         * the bar and moves it.
         */
        if (bar == NULL) {
            if (self->settings_images != NULL)
                refresh_settings_images(self);

            return;
        }

        if (total > 0) {
            gint64 done = clawt_event_get_detail_int(event, "done");
            GtkWidget *row = gtk_widget_get_ancestor(bar,
                                                     ADW_TYPE_ACTION_ROW);

            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar),
                                          (gdouble)done / total);

            /*
             * The subtitle moves with the bar.  Left as it was written,
             * it says the size the download had reached when the row was
             * built -- a bar at a fifth beside "16 kB of 557 MB", which
             * reads as a broken bar rather than a stale label.
             */
            if (row != NULL) {
                g_autofree gchar *done_text = human_size(done);
                g_autofree gchar *total_text = human_size(total);
                g_autofree gchar *subtitle =
                    g_strdup_printf("%s of %s", done_text, total_text);

                adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
            }
        }

        return;
    }

    if (g_strcmp0(kind, "image.finished") == 0) {
        const gchar *failure = clawt_event_get_detail(event, "error");

        if (failure != NULL) {
            g_autofree gchar *message = g_strdup_printf(
                "%s could not be downloaded: %s",
                clawt_event_get_subject(event), failure);

            /*
             * Into the panel rather than a toast.  A download runs in the
             * daemon and outlives the window that started it, so the
             * failure arrives on its own -- and a toast about something
             * that arrived on its own vanishes after a few seconds and
             * leaves no trace anywhere if nobody was looking.
             */
            clawt_window_alert(self, CLAWT_ALERT_ERROR, "download",
                               clawt_event_get_subject(event), message);
        }

        refresh_settings_images(self);
        return;
    }

    if (g_strcmp0(kind, "message.refused") == 0) {
        const gchar *from = clawt_event_get_detail(event, "from");
        const gchar *to = clawt_event_get_detail(event, "to");
        const gchar *reason = clawt_event_get_detail(event, "reason");
        g_autofree gchar *message = g_strdup_printf(
            "%s to %s was stopped: %s", from != NULL ? from : "?",
            to != NULL ? to : "?", reason != NULL ? reason : "a limit");

        /*
         * The other of the two.  A refusal on the link path is invisible
         * otherwise: the two agents just stop, and the conversation
         * appears to trail off for no reason.  It is also the one an
         * operator most wants to find again an hour later, which a toast
         * cannot offer.
         */
        clawt_window_alert(self, CLAWT_ALERT_ERROR, "message", from, message);
        refresh_flow(self);
        return;
    }

    if (g_strcmp0(kind, "agent.typing") == 0) {
        const gchar *typing = clawt_event_get_detail(event, "typing");
        const gchar *peer = clawt_event_get_detail(event, "peer");

        if (g_strcmp0(clawt_event_get_subject(event),
                      self->selected_agent) == 0) {
            g_autofree gchar *what = NULL;

            /*
             * Say who it is for when it is not for you. The activity
             * line under a chat that reads "thinking" while the agent
             * is actually answering a peer is the same lie the sidebar
             * used to tell.
             */
            if (g_strcmp0(typing, "true") == 0 && peer != NULL &&
                g_strcmp0(peer, "user") != 0)
                what = g_strdup_printf("working for %s", peer);
            else if (g_strcmp0(typing, "true") == 0)
                what = g_strdup("thinking");

            set_activity(self, what);
        }

        /*
         * And the sidebar, which is where somebody looks to see whether
         * anything anywhere is happening.
         */
        refresh_agents(self);
        return;
    }

    if (g_str_has_prefix(kind, "agent.") || g_str_has_prefix(kind, "mailbox.")) {
        refresh_agents(self);
        refresh_mailbox(self);
        return;
    }

    if (g_str_has_prefix(kind, "task."))
        refresh_tasks(self);

    /*
     * `routine.ran` is published when one starts, so the list shows the
     * new "last run" without anybody reopening the page.
     */
    if (g_str_has_prefix(kind, "routine."))
        refresh_routines(self);
}

/* ── New agent ───────────────────────────────────────────────────── */

typedef struct {
    ClawtWindow  *window;
    AdwDialog    *dialog;
    GtkWidget    *id_entry;
    GtkWidget    *name_entry;
    GtkWidget    *description_entry;
    GtkWidget    *computer_row;
    GtkWidget    *team_row;
    GStrv         team_ids;
    GtkWidget    *describe_entry;   /* purpose: the one required answer */
    GtkWidget    *boundaries_entry;
    GtkWidget    *needs_entry;
    GtkWidget    *personality_entry;
    GtkWidget    *projects_entry;
    GtkWidget    *notes_entry;
    ModelChooser  models;           /* the model the agent will run */
    ModelChooser  designer;         /* the model that drafts it */
    ImageChooser  image;
    ImageChooser  disk;             /* the VM's disk, when it is a VM */
} NewAgentDialog;

static void
new_agent_dialog_free(gpointer data)
{
    NewAgentDialog *dialog = data;

    g_clear_pointer(&dialog->models.catalog, json_node_unref);
    g_clear_pointer(&dialog->designer.catalog, json_node_unref);
    g_clear_pointer(&dialog->image.catalog, json_node_unref);
    g_clear_pointer(&dialog->disk.catalog, json_node_unref);
    g_free(dialog);
}

/*
 * The computer the form describes.
 *
 * Shared deliberately, because this was two code paths disagreeing about
 * one form: `agent.create` sent the disk image and `design.agent` did
 * not, so designing a VM agent produced one that refused to provision --
 * naming a setting that was filled in on screen at the time. A single
 * reader cannot drift from itself.
 *
 * Each backend is sent only its own image key. Setting a container
 * reference on a VM, or the other way round, writes a key that backend
 * never reads and then looks like a setting being ignored.
 */
/*
 * The team the dialog is offering, or NULL for none.
 *
 * Read through one function for the same reason the computer is: the
 * manual path and the designer path both build a payload from these
 * widgets, and the last time there were two readers one of them silently
 * dropped a field.
 */
static const gchar *
dialog_team(NewAgentDialog *dialog)
{
    guint chosen;

    if (dialog->team_row == NULL || dialog->team_ids == NULL)
        return NULL;

    chosen = adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->team_row));

    if (chosen >= g_strv_length(dialog->team_ids))
        return NULL;

    /* "" is the "No team" entry, and means send nothing. */
    return (*dialog->team_ids[chosen] != '\0') ? dialog->team_ids[chosen]
                                                : NULL;
}

static const gchar *
dialog_computer(NewAgentDialog *dialog, gchar **out_image, gchar **out_disk)
{
    static const gchar *const computers[] = { "none", "host", "container",
                                              "vm" };
    guint selected =
        adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->computer_row));
    const gchar *type = computers[MIN(selected, 3)];

    *out_image = NULL;
    *out_disk = NULL;

    if (g_strcmp0(type, "container") == 0)
        *out_image = image_chooser_value(&dialog->image);
    else if (g_strcmp0(type, "vm") == 0)
        *out_disk = disk_chooser_value(&dialog->disk);

    return type;
}

static JsonObject *
chooser_provider(ModelChooser *chooser)
{
    JsonArray *providers;
    guint selected;

    if (chooser->catalog == NULL)
        return NULL;

    providers = json_object_get_array_member(
        json_node_get_object(chooser->catalog), "providers");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->provider_row));

    if (selected >= json_array_get_length(providers))
        return NULL;

    return json_array_get_object_element(providers, selected);
}

/*
 * Rebuilds the model list for whichever provider is selected.
 *
 * The models a provider runs are not interchangeable -- "sonnet" means
 * nothing to Ollama -- so one flat list would offer combinations that
 * cannot work.
 */
static void
on_provider_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ModelChooser *chooser = user_data;
    JsonObject *provider = chooser_provider(chooser);
    g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
    JsonArray *models;
    guint i;

    (void)object;
    (void)pspec;

    if (provider == NULL)
        return;

    models = json_object_get_array_member(provider, "models");

    for (i = 0; i < json_array_get_length(models); i++) {
        JsonObject *model = json_array_get_object_element(models, i);
        const gchar *note = clawt_json_string(model, "note", NULL);
        g_autofree gchar *label = NULL;

        label = (note != NULL)
                ? g_strdup_printf("%s - %s",
                                  clawt_json_string(model, "label", "?"), note)
                : g_strdup(clawt_json_string(model, "label", "?"));

        gtk_string_list_append(names, label);
    }

    /*
     * The catalogue is curated and goes stale, so every provider also
     * takes a name typed by hand.  Offering it as the last entry keeps
     * the common case one click.
     */
    gtk_string_list_append(names, "Something else…");

    adw_combo_row_set_model(ADW_COMBO_ROW(chooser->model_row),
                            G_LIST_MODEL(g_steal_pointer(&names)));
    adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->model_row), 0);

    if (clawt_json_string(provider, "note", NULL) != NULL) {
        adw_preferences_row_set_use_markup(
            ADW_PREFERENCES_ROW(chooser->provider_row), FALSE);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(chooser->provider_row),
                                    clawt_json_string(provider, "note", ""));
    }
}

/* The free-text row appears only when "Something else…" is chosen. */
static void
on_model_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ModelChooser *chooser = user_data;
    JsonObject *provider = chooser_provider(chooser);
    JsonArray *models;
    guint selected;

    (void)object;
    (void)pspec;

    if (provider == NULL)
        return;

    models = json_object_get_array_member(provider, "models");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->model_row));

    gtk_widget_set_visible(chooser->model_entry,
                           selected >= json_array_get_length(models));
}

/*
 * Returns: (transfer full) (nullable): the chosen model id
 */
static gchar *
chooser_model(ModelChooser *chooser)
{
    JsonObject *provider = chooser_provider(chooser);
    JsonArray *models;
    guint selected;

    if (provider == NULL)
        return NULL;

    models = json_object_get_array_member(provider, "models");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->model_row));

    if (selected < json_array_get_length(models))
        return g_strdup(clawt_json_string(
            json_array_get_object_element(models, selected), "id", NULL));

    {
        const gchar *typed = gtk_editable_get_text(
            GTK_EDITABLE(chooser->model_entry));

        return (typed != NULL && *typed != '\0') ? g_strdup(typed) : NULL;
    }
}

/*
 * Returns: (transfer none) (nullable): the chosen provider id
 */
static const gchar *
chooser_provider_id(ModelChooser *chooser)
{
    JsonObject *provider = chooser_provider(chooser);

    return (provider != NULL) ? clawt_json_string(provider, "id", NULL)
                              : NULL;
}

/* The reference currently chosen, from the list or from the entry. */
static gchar *
image_chooser_value(ImageChooser *chooser)
{
    JsonArray *images;
    guint selected;

    if (chooser->catalog == NULL)
        return NULL;

    images = json_object_get_array_member(
        json_node_get_object(chooser->catalog), "images");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->row));

    if (selected < json_array_get_length(images))
        return g_strdup(clawt_json_string(
            json_array_get_object_element(images, selected), "reference",
            NULL));

    {
        const gchar *typed =
            gtk_editable_get_text(GTK_EDITABLE(chooser->entry));

        return (typed != NULL && *typed != '\0') ? g_strdup(typed) : NULL;
    }
}

static void
on_image_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ImageChooser *chooser = user_data;
    JsonArray *images;
    guint selected;

    (void)object;
    (void)pspec;

    if (chooser->catalog == NULL)
        return;

    images = json_object_get_array_member(
        json_node_get_object(chooser->catalog), "images");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->row));

    /* "Other" is the one entry past the end of the catalogue. */
    gtk_widget_set_visible(chooser->entry,
                           selected >= json_array_get_length(images));

    if (selected < json_array_get_length(images)) {
        JsonObject *image = json_array_get_object_element(images, selected);
        const gchar *note = clawt_json_string(image, "note", NULL);
        const gchar *reference = clawt_json_string(image, "reference", "");

        /*
         * The subtitle carries the full reference, because a label like
         * "Fedora 44" does not say which registry it comes from -- and
         * that is exactly what goes wrong when two machines disagree
         * about an unqualified name.
         */
        if (note != NULL) {
            g_autofree gchar *subtitle =
                g_strdup_printf("%s -- %s", reference, note);

            adw_action_row_set_subtitle(ADW_ACTION_ROW(chooser->row),
                                        subtitle);
        } else {
            adw_action_row_set_subtitle(ADW_ACTION_ROW(chooser->row),
                                        reference);
        }
    } else {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(chooser->row),
                                    "any reference podman can pull");
    }
}

/*
 * Adds the image rows to a group, selecting @want if it is on the list
 * and putting it in the entry if it is not.
 */
static void
image_chooser_build(ImageChooser *chooser, ClawtWindow *window,
                    GtkWidget *group, const gchar *want)
{
    GtkStringList *labels = gtk_string_list_new(NULL);
    JsonArray *images = NULL;
    const gchar *fallback = NULL;
    gboolean matched = FALSE;
    guint chosen = 0;
    guint i;

    chooser->window = window;

    chooser->row = adw_combo_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(chooser->row),
                                       FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->row), "Image");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), chooser->row);

    chooser->entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(chooser->entry),
                                       FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->entry),
                                  "Image reference");
    gtk_widget_set_visible(chooser->entry, FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), chooser->entry);

    chooser->catalog = clawt_window_request(window, "image.list", NULL);

    if (chooser->catalog != NULL) {
        JsonObject *root = json_node_get_object(chooser->catalog);

        images = json_object_get_array_member(root, "images");
        fallback = clawt_json_string(root, "default", NULL);

        for (i = 0; i < json_array_get_length(images); i++) {
            JsonObject *image = json_array_get_object_element(images, i);
            const gchar *reference = clawt_json_string(image, "reference", "");
            /*
             * The label alone, not the group.  AdwComboRow takes a flat
             * GListModel and sizes its popup to the row, so a
             * "Enterprise Linux / CentOS Stream 10" prefix truncates to
             * the part every entry in the group shares -- which is the
             * half that carries no information. The catalogue's labels
             * are already distinct, the order groups them, and the full
             * reference is on the subtitle once one is picked.
             */
            gtk_string_list_append(labels,
                                   clawt_json_string(image, "label",
                                                     reference));

            if (want != NULL && g_strcmp0(reference, want) == 0) {
                chosen = i;
                matched = TRUE;
            }
        }
    }

    gtk_string_list_append(labels, "Other\342\200\246");
    adw_combo_row_set_model(ADW_COMBO_ROW(chooser->row),
                            G_LIST_MODEL(labels));

    /*
     * A reference that is not on the list is not an error: it goes into
     * the entry, selected as "Other", so an agent configured by hand
     * keeps what it was given instead of being silently retargeted at
     * whatever happened to be first.
     */
    if (want != NULL && *want != '\0' && !matched) {
        gtk_editable_set_text(GTK_EDITABLE(chooser->entry), want);
        chosen = (images != NULL) ? json_array_get_length(images) : 0;
    } else if (want == NULL && fallback != NULL && images != NULL) {
        for (i = 0; i < json_array_get_length(images); i++) {
            if (g_strcmp0(clawt_json_string(
                    json_array_get_object_element(images, i),
                    "reference", ""), fallback) == 0) {
                chosen = i;
                break;
            }
        }
    }

    adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->row), chosen);

    g_signal_connect(chooser->row, "notify::selected",
                     G_CALLBACK(on_image_changed), chooser);
    on_image_changed(NULL, NULL, chooser);
}


/* ── The VM's disk ───────────────────────────────────────────────── */

/*
 * The same shape as the container image chooser, over a different list:
 * cached cloud images rather than registry references, and a path rather
 * than a reference.
 *
 * A VM without a disk is defined, started, and never boots -- so an agent
 * with nothing chosen has to look wrong here, which is why the empty case
 * says where to get one instead of showing a bare text field.
 */
static void
on_disk_changed(GObject *object, GParamSpec *pspec, gpointer user_data)
{
    ImageChooser *chooser = user_data;
    JsonArray *images;
    guint selected;

    (void)object;
    (void)pspec;

    if (chooser->catalog == NULL)
        return;

    images = json_object_get_array_member(
        json_node_get_object(chooser->catalog), "images");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->row));

    gtk_widget_set_visible(chooser->entry,
                           selected >= json_array_get_length(images));

    if (selected < json_array_get_length(images)) {
        JsonObject *image = json_array_get_object_element(images, selected);

        adw_action_row_set_subtitle(ADW_ACTION_ROW(chooser->row),
                                    clawt_json_string(image, "path", ""));
        return;
    }

    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(chooser->row),
        json_array_get_length(images) == 0
            ? "No cloud images yet \342\200\224 fetch one from Settings"
            : "A path to a qcow2 on this machine");
}

static gchar *
disk_chooser_value(ImageChooser *chooser)
{
    JsonArray *images;
    guint selected;

    if (chooser->row == NULL || chooser->catalog == NULL)
        return NULL;

    images = json_object_get_array_member(
        json_node_get_object(chooser->catalog), "images");
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(chooser->row));

    if (selected < json_array_get_length(images))
        return g_strdup(clawt_json_string(
            json_array_get_object_element(images, selected), "path", NULL));

    {
        const gchar *typed =
            gtk_editable_get_text(GTK_EDITABLE(chooser->entry));

        return (typed != NULL && *typed != '\0') ? g_strdup(typed) : NULL;
    }
}

static void
disk_chooser_build(ImageChooser *chooser, ClawtWindow *window,
                   GtkWidget *group, const gchar *want)
{
    GtkStringList *labels = gtk_string_list_new(NULL);
    JsonArray *images = NULL;
    gboolean matched = FALSE;
    guint chosen = 0;
    guint i;

    chooser->window = window;

    chooser->row = adw_combo_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(chooser->row),
                                       FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->row),
                                  "Disk image");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), chooser->row);

    chooser->entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(chooser->entry),
                                       FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->entry),
                                  "Path to a qcow2");
    gtk_widget_set_visible(chooser->entry, FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), chooser->entry);

    chooser->catalog = clawt_window_request(window, "image.vm_list", NULL);

    if (chooser->catalog != NULL) {
        images = json_object_get_array_member(
            json_node_get_object(chooser->catalog), "images");

        for (i = 0; i < json_array_get_length(images); i++) {
            JsonObject *image = json_array_get_object_element(images, i);

            /*
             * One still downloading is listed and selectable: it will be
             * there by the time the agent is started, and hiding it makes
             * the list appear to lose the thing just asked for.
             */
            gtk_string_list_append(labels,
                                   clawt_json_string(image, "name", "?"));

            if (want != NULL &&
                g_strcmp0(clawt_json_string(image, "path", ""), want) == 0) {
                chosen = i;
                matched = TRUE;
            }
        }
    }

    gtk_string_list_append(labels, "Other\342\200\246");
    adw_combo_row_set_model(ADW_COMBO_ROW(chooser->row),
                            G_LIST_MODEL(labels));

    /*
     * A path that is not one of ours keeps what it was given rather than
     * being retargeted at whatever happens to be first.
     */
    if (want != NULL && *want != '\0' && !matched) {
        gtk_editable_set_text(GTK_EDITABLE(chooser->entry), want);
        chosen = (images != NULL) ? json_array_get_length(images) : 0;
    } else if (!matched) {
        chosen = (images != NULL) ? json_array_get_length(images) : 0;
    }

    adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->row), chosen);

    g_signal_connect(chooser->row, "notify::selected",
                     G_CALLBACK(on_disk_changed), chooser);
    on_disk_changed(NULL, NULL, chooser);
}

/*
 * Adds the provider and model rows to a group, populated from the daemon
 * so every view agrees on what exists.
 */
static void
model_chooser_build(ModelChooser *chooser, ClawtWindow *window,
                    GtkWidget *group, const gchar *want_provider,
                    const gchar *want_model)
{
    model_chooser_build_full(chooser, window, group, want_provider,
                             want_model, "agent");
}

/*
 * @require names the flag a provider must carry to be offered, or is
 * %NULL to offer all of them.
 *
 * Two views ask this for two different jobs and want two different
 * lists. "agent" is what libreclaw can drive: its provider table is
 * command-line only, and it rewrites anything it does not recognise to
 * claude-code, so offering the HTTP providers as an agent's backend let
 * a person pick OpenAI and silently get Claude Code with "gpt-4o" in
 * the model field. "tools" is what can be given tool definitions, which
 * the designer is built entirely out of and ai-glib's CLI clients drop;
 * offering those let a person fill in the whole form, press Design, and
 * only then be told the provider cannot do this.
 *
 * The list is filtered rather than the entries greyed out because every
 * remaining index is used to look a provider back up, and a hole in the
 * middle of that array is a bug waiting for somebody to add a provider.
 *
 * @want_provider survives the filter whatever it is. An agent already
 * configured for a provider this view would not offer must still show
 * the one it has -- dropping it would leave the combo pointing at the
 * first entry, and saving the page would then change the agent's
 * provider to something nobody chose.
 */
static void
model_chooser_build_full(ModelChooser *chooser, ClawtWindow *window,
                         GtkWidget *group, const gchar *want_provider,
                         const gchar *want_model, const gchar *require)
{
    g_autoptr(GtkStringList) provider_names = gtk_string_list_new(NULL);
    JsonArray *providers = NULL;
    guint chosen = 0;
    guint i;

    chooser->window = window;

    chooser->provider_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->provider_row),
                                  "Provider");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              chooser->provider_row);

    chooser->model_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->model_row),
                                  "Model");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              chooser->model_row);

    chooser->model_entry = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(chooser->model_entry),
                                  "Model name");
    gtk_widget_set_visible(chooser->model_entry, FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              chooser->model_entry);

    /*
     * refresh: the provider's own list where we can get it.  The
     * hardcoded table goes stale, and a person picking a model should
     * see what the provider actually runs today. The daemon falls back
     * to the table when there is no key or no network, so this never
     * produces an empty chooser.
     */
    chooser->catalog = clawt_window_request(
        window, "model.list", clawt_build_payload("refresh", "true", NULL));

    if (chooser->catalog != NULL && require != NULL) {
        JsonArray *all = json_object_get_array_member(
            json_node_get_object(chooser->catalog), "providers");
        g_autoptr(JsonArray) kept = json_array_new();

        for (i = 0; i < json_array_get_length(all); i++) {
            JsonObject *provider = json_array_get_object_element(all, i);

            /*
             * A missing member means an older daemon that does not
             * report this flag -- keep the provider rather than drop
             * it. Filtering on absence emptied the whole list and left
             * two blank rows, which is a worse failure than offering one
             * provider that will refuse.
             */
            if (!json_object_has_member(provider, require) ||
                json_object_get_boolean_member(provider, require) ||
                (want_provider != NULL &&
                 g_strcmp0(clawt_json_string(provider, "id", ""),
                           want_provider) == 0))
                json_array_add_object_element(kept,
                                              json_object_ref(provider));
        }

        json_object_set_array_member(json_node_get_object(chooser->catalog),
                                     "providers",
                                     json_array_ref(kept));
    }

    if (chooser->catalog != NULL) {
        providers = json_object_get_array_member(
            json_node_get_object(chooser->catalog), "providers");

        for (i = 0; i < json_array_get_length(providers); i++) {
            JsonObject *provider = json_array_get_object_element(providers, i);

            gtk_string_list_append(provider_names,
                                   clawt_json_string(provider, "label", "?"));

            if (want_provider != NULL &&
                g_strcmp0(clawt_json_string(provider, "id", ""),
                          want_provider) == 0)
                chosen = i;
        }
    }

    adw_combo_row_set_model(ADW_COMBO_ROW(chooser->provider_row),
                            G_LIST_MODEL(g_steal_pointer(&provider_names)));
    adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->provider_row), chosen);

    g_signal_connect(chooser->provider_row, "notify::selected",
                     G_CALLBACK(on_provider_changed), chooser);
    g_signal_connect(chooser->model_row, "notify::selected",
                     G_CALLBACK(on_model_changed), chooser);

    on_provider_changed(NULL, NULL, chooser);

    /* Select the agent's current model, or offer it as typed text. */
    if (want_model != NULL && providers != NULL) {
        JsonObject *provider = chooser_provider(chooser);
        JsonArray *models = (provider != NULL)
                            ? json_object_get_array_member(provider, "models")
                            : NULL;
        gboolean found = FALSE;

        for (i = 0; models != NULL && i < json_array_get_length(models); i++) {
            if (g_strcmp0(clawt_json_string(
                              json_array_get_object_element(models, i),
                              "id", ""), want_model) != 0)
                continue;

            adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->model_row), i);
            found = TRUE;
            break;
        }

        if (!found && models != NULL) {
            adw_combo_row_set_selected(ADW_COMBO_ROW(chooser->model_row),
                                       json_array_get_length(models));
            gtk_editable_set_text(GTK_EDITABLE(chooser->model_entry),
                                  want_model);
        }
    }
}

/*
 * What actually happened, rather than a fixed sentence.
 *
 * Creating an agent used to toast "Agent created." whatever followed,
 * and the daemon built its computer only at *start* -- so a VM agent
 * created here left a config file and no machine, and nothing on screen
 * suggested a step was missing. The CLI had known all along and printed
 * "Start it with: ..." as its third line, which is the sort of divergence
 * that leaves one client's users convinced a feature is broken.
 */
static void
report_created(ClawtWindow *self, JsonNode *reply, const gchar *agent_id)
{
    JsonObject *payload = clawt_payload_of(reply);
    const gchar *failure = clawt_json_string(payload, "start_error", NULL);

    if (failure != NULL) {
        g_autofree gchar *message =
            g_strdup_printf("%s was created but did not start: %s",
                            agent_id, failure);

        clawt_window_toast(self, message);
        return;
    }

    if (clawt_json_boolean(payload, "started", FALSE)) {
        g_autofree gchar *message =
            g_strdup_printf("%s created and started.", agent_id);

        clawt_window_toast(self, message);
        return;
    }

    /*
     * A daemon older than this client sends neither member.  Saying
     * "created" and nothing more is right there: it is what happened.
     */
    clawt_window_toast(self, "Agent created.");
}

static void
on_create_manually(GtkButton *button, gpointer user_data)
{
    NewAgentDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    JsonObject *provider = chooser_provider(&dialog->models);
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *model = NULL;
    g_autofree gchar *image = NULL;
    g_autofree gchar *disk = NULL;
    const gchar *computer;
    const gchar *agent_id;

    (void)button;

    agent_id = gtk_editable_get_text(GTK_EDITABLE(dialog->id_entry));

    if (agent_id == NULL || *agent_id == '\0') {
        clawt_window_toast(self, "An agent needs an id.");
        return;
    }

    model = chooser_model(&dialog->models);
    computer = dialog_computer(dialog, &image, &disk);

    reply = clawt_window_request(
        self, "agent.create",
        clawt_build_payload(
            "id", agent_id,
            "name", answer_of(dialog->name_entry),
            "description", answer_of(dialog->description_entry),
            "provider", provider != NULL
                        ? clawt_json_string(provider, "id", NULL) : NULL,
            "model", model,
            "computer", computer,
            "image", image,
            "vm_image", disk,
            "team", dialog_team(dialog),
            NULL));

    /*
     * The dialog stays open on failure, with the toast explaining why, so
     * whatever was typed is still there to correct.
     */
    if (reply == NULL)
        return;

    report_created(self, reply, agent_id);
    refresh_agents(self);

    /*
     * adw_dialog_close, not gtk_window_close.  An AdwDialog is a widget
     * inside the window rather than a window of its own, so asking for
     * its GtkWindow ancestor finds the main window -- which either does
     * nothing useful or closes the application.
     */
    adw_dialog_close(dialog->dialog);
}

static void
on_preview_response(AdwAlertDialog *dialog, gchar *response,
                    gpointer user_data)
{
    NewAgentDialog *new_agent = user_data;
    ClawtWindow *self = new_agent->window;
    const gchar *draft = g_object_get_data(G_OBJECT(dialog), "draft");
    g_autoptr(JsonNode) reply = NULL;

    if (draft == NULL)
        return;

    /*
     * Cancelling drops the draft rather than leaving it on the daemon.
     * It holds a whole designer, and a person who says no is done with
     * it.
     */
    if (g_strcmp0(response, "create") != 0) {
        g_autoptr(JsonNode) discarded = clawt_window_request(
            self, "design.discard", clawt_build_payload("draft", draft, NULL));

        (void)discarded;
        return;
    }

    /*
     * Commits the draft that was reviewed.  Re-running the design would
     * be a fresh conversation producing something else, which makes the
     * preview a demonstration rather than a decision.
     */
    reply = clawt_window_request(self, "design.commit",
                                 clawt_build_payload("draft", draft, NULL));

    if (reply == NULL)
        return;

    report_created(self, reply,
                   clawt_json_string(clawt_payload_of(reply), "id",
                                     "The agent"));
    refresh_agents(self);

    /*
     * The outer dialog closes here too.  Only the manual path used to,
     * so designing an agent left the New agent dialog sitting open over
     * a fleet that already contained it.
     */
    adw_dialog_close(new_agent->dialog);
}

/* An answer, or NULL when the row was left empty. */
static const gchar *
answer_of(GtkWidget *row)
{
    const gchar *text = gtk_editable_get_text(GTK_EDITABLE(row));

    return (text != NULL && *text != '\0') ? text : NULL;
}

static void
on_design_with_ai(GtkButton *button, gpointer user_data)
{
    NewAgentDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *designer_model = NULL;
    g_autofree gchar *image = NULL;
    g_autofree gchar *disk = NULL;
    const gchar *computer;
    const gchar *purpose;

    purpose = answer_of(dialog->describe_entry);

    if (purpose == NULL) {
        clawt_window_toast(self, "Say what the agent should do first.");
        return;
    }

    /*
     * Disabled while the model works.  A design takes tens of seconds
     * and a second click starts a second one, which is billed and
     * discarded.
     */
    gtk_widget_set_sensitive(GTK_WIDGET(button), FALSE);
    gtk_button_set_label(GTK_BUTTON(button), "Designing...");

    designer_model = chooser_model(&dialog->designer);
    computer = dialog_computer(dialog, &image, &disk);

    reply = clawt_window_request(
        self, "design.agent",
        clawt_build_payload(
            /*
             * Whatever was typed above is pinned, so the model fills in
             * what is blank rather than replacing what is not. Left out,
             * these arrived as NULL and the model named the agent
             * itself -- renaming one the person had already named, which
             * is the single most irritating thing it can do.
             */
            "id", answer_of(dialog->id_entry),
            "name", answer_of(dialog->name_entry),
            "purpose", purpose,
            "boundaries", answer_of(dialog->boundaries_entry),
            "needs", answer_of(dialog->needs_entry),
            "personality", answer_of(dialog->personality_entry),
            "projects", answer_of(dialog->projects_entry),
            "notes", answer_of(dialog->notes_entry),
            "provider", chooser_provider_id(&dialog->designer),
            "model", designer_model,
            /*
             * And the computer chosen above, which used to be left out.
             * The designer cannot name a disk image -- the images that
             * exist are the ones somebody fetched -- so a VM it picked
             * on its own never provisioned, refusing with a message
             * about computer.vm.image while the image sat filled in on
             * screen a few rows up.
             */
            "computer", g_strcmp0(computer, "none") != 0 ? computer : NULL,
            "image", image,
            "vm_image", disk,
            /*
             * The team too, for the same reason: it is a choice the
             * person made on this form, and the model has no way to
             * know which teams exist.
             */
            "team", dialog_team(dialog),
            NULL));

    gtk_button_set_label(GTK_BUTTON(button), "Design it");
    gtk_widget_set_sensitive(GTK_WIDGET(button), TRUE);

    if (reply == NULL)
        return;

    /*
     * The design is shown before anything is written.  A model's
     * proposal becoming an agent without a person reading it first is
     * exactly the kind of convenience nobody asks for twice.
     */
    {
        JsonObject *result = clawt_payload_of(reply);
        JsonArray *files = json_object_has_member(result, "files")
                           ? json_object_get_array_member(result, "files")
                           : NULL;
        g_autoptr(GString) body = g_string_new(
            clawt_json_string(result, "yaml", ""));
        AdwAlertDialog *preview;
        guint i;

        /*
         * The org files are listed rather than shown in full: they run
         * to hundreds of lines each, and a dialog nobody can read is not
         * a review.  They are on disk after Create, and
         * `clawtilla agent edit` opens them.
         */
        if (files != NULL && json_array_get_length(files) > 0) {
            g_string_append(body, "\n\nIt also wrote:\n");

            for (i = 0; i < json_array_get_length(files); i++) {
                JsonObject *file = json_array_get_object_element(files, i);
                const gchar *content = clawt_json_string(file, "content", "");
                gsize lines = 0;
                const gchar *p;

                for (p = content; *p != '\0'; p++) {
                    if (*p == '\n')
                        lines++;
                }

                g_string_append_printf(body, "  %s  (%" G_GSIZE_FORMAT
                                             " lines)\n",
                                       clawt_json_string(file, "name", "?"),
                                       lines);
            }

            g_string_append(body,
                            "\nEdit them after with: clawtilla agent edit ");
            g_string_append(body, clawt_json_string(result, "id", ""));
        }

        preview = ADW_ALERT_DIALOG(
            adw_alert_dialog_new("Proposed agent", NULL));

        adw_alert_dialog_set_body(preview, body->str);
        adw_alert_dialog_add_response(preview, "cancel", "Cancel");
        adw_alert_dialog_add_response(preview, "create", "Create");
        adw_alert_dialog_set_response_appearance(preview, "create",
                                                 ADW_RESPONSE_SUGGESTED);

        g_object_set_data_full(
            G_OBJECT(preview), "draft",
            g_strdup(clawt_json_string(result, "draft", NULL)), g_free);
        g_signal_connect(preview, "response",
                         G_CALLBACK(on_preview_response), dialog);

        adw_dialog_present(ADW_DIALOG(preview), GTK_WIDGET(self));
    }
}

/*
 * Shows the image rows only for a container.
 *
 * Sensitivity would be the softer choice, but a greyed-out Image row on
 * a host agent still reads as "this agent has an image, you just cannot
 * change it", which is not true.
 */
static void
on_computer_type_changed(GObject *object, GParamSpec *pspec,
                         gpointer user_data)
{
    NewAgentDialog *dialog = user_data;
    guint selected;
    gboolean is_container;

    (void)object;
    (void)pspec;

    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->computer_row));
    is_container = (selected == 2);   /* none, host, container, vm */

    gtk_widget_set_visible(dialog->image.row, is_container);

    if (!is_container)
        gtk_widget_set_visible(dialog->image.entry, FALSE);
    else
        on_image_changed(NULL, NULL, &dialog->image);

    /*
     * A VM gets the same treatment, over the cached cloud images.  It
     * matters more here than for a container: podman pulls what it is
     * given, while a VM with no disk defines, starts and never boots.
     */
    gtk_widget_set_visible(dialog->disk.row, selected == 3);

    if (selected != 3)
        gtk_widget_set_visible(dialog->disk.entry, FALSE);
    else
        on_disk_changed(NULL, NULL, &dialog->disk);
}

static void
on_new_agent(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    NewAgentDialog *dialog = g_new0(NewAgentDialog, 1);
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *manual = adw_preferences_group_new();
    GtkWidget *ai = adw_preferences_group_new();
    GtkWidget *create;
    GtkWidget *design;
    static const gchar *const computers[] = { "none", "host", "container",
                                              "vm", NULL };

    (void)button;

    dialog->window = self;
    dialog->dialog = window;

    adw_dialog_set_title(window, "New agent");
    adw_dialog_set_content_width(window, 520);

    /* ── By hand ── */
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(manual),
                                    "By hand");

    dialog->id_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(dialog->id_entry),
                                        FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->id_entry),
                                  "Id");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->id_entry);

    /*
     * Name and description, which only the inspector offered before --
     * so every agent was created with its id as its name and no
     * description, and the description is what other agents read when
     * deciding who to delegate to.
     */
    dialog->name_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(dialog->name_entry), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->name_entry),
                                  "Name");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->name_entry);

    dialog->description_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(dialog->description_entry), FALSE);
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(dialog->description_entry),
        "What it is for");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->description_entry);

    /*
     * Which team, chosen here rather than only afterwards. An agent made
     * for a team and then left out of it is one the team's lead cannot
     * hand anything to, which reads as the lead being broken.
     */
    {
        GtkStringList *choices = team_choices(self, NULL, &dialog->team_ids);

        dialog->team_row = adw_combo_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->team_row),
                                      "Team");
        adw_combo_row_set_model(ADW_COMBO_ROW(dialog->team_row),
                                G_LIST_MODEL(choices));
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                                  dialog->team_row);
    }

    /*
     * Provider first, then model: the model list depends on it, and
     * asking for a model before knowing the provider is how you end up
     * offering "sonnet" for Ollama.
     */
    model_chooser_build(&dialog->models, self, manual, NULL, NULL);

    dialog->computer_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->computer_row),
                                  "Computer");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->computer_row),
                            G_LIST_MODEL(gtk_string_list_new(computers)));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->computer_row);

    /*
     * The image, shown only while "container" is the chosen computer.
     * The other backends do not read it, and a row that quietly does
     * nothing is worse than no row.
     */
    image_chooser_build(&dialog->image, self, manual, NULL);
    disk_chooser_build(&dialog->disk, self, manual, NULL);
    g_signal_connect(dialog->computer_row, "notify::selected",
                     G_CALLBACK(on_computer_type_changed), dialog);
    on_computer_type_changed(NULL, NULL, dialog);

    create = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_margin_top(create, 12);
    g_signal_connect(create, "clicked", G_CALLBACK(on_create_manually),
                     dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual), create);

    /* ── By description ── */
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(ai),
                                    "By description");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(ai),
        "Answer what you can and clawtilla drafts the configuration and "
        "the agent's org files. Only the first is required; a blank "
        "answer becomes a heading to fill in rather than an invention. "
        "You see everything before anything is created.");

    /*
     * Named questions rather than one box.
     *
     * "Describe what you want" asked the person to write a paragraph
     * that happened to contain everything the model needed, and a
     * paragraph that leaves out the boundaries produces an agent with
     * none. Each question asks for one thing once.
     */
    {
        static const struct {
            const gchar *title;
            gsize        offset;
        } questions[] = {
            { "What should it do?",
              G_STRUCT_OFFSET(NewAgentDialog, describe_entry) },
            { "What should it never do?",
              G_STRUCT_OFFSET(NewAgentDialog, boundaries_entry) },
            { "What does it need to work on?",
              G_STRUCT_OFFSET(NewAgentDialog, needs_entry) },
            { "How should it come across?",
              G_STRUCT_OFFSET(NewAgentDialog, personality_entry) },
            { "What is it working on, and where?",
              G_STRUCT_OFFSET(NewAgentDialog, projects_entry) },
            { "Anything else it should know?",
              G_STRUCT_OFFSET(NewAgentDialog, notes_entry) },
            { NULL, 0 }
        };
        gsize i;

        for (i = 0; questions[i].title != NULL; i++) {
            GtkWidget *row = adw_entry_row_new();

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                                FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          questions[i].title);
            adw_preferences_group_add(ADW_PREFERENCES_GROUP(ai), row);

            G_STRUCT_MEMBER(GtkWidget *, dialog, questions[i].offset) = row;
        }
    }

    /*
     * Which model does the designing -- separate from the model the
     * agent will run on. There is no reason for them to be the same, and
     * a person will often want their best model to draft an agent that
     * then runs on a cheap one.
     */
    model_chooser_build_full(&dialog->designer, self, ai, NULL, NULL,
                             "tools");
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(dialog->designer.provider_row),
        "Designed by");
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(dialog->designer.model_row),
        "Designer's model");

    design = gtk_button_new_with_label("Design it");
    gtk_widget_set_margin_top(design, 12);
    g_signal_connect(design, "clicked", G_CALLBACK(on_design_with_ai),
                     dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(ai), design);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(manual));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(ai));

    g_object_set_data_full(G_OBJECT(window), "dialog", dialog,
                           new_agent_dialog_free);
    /*
     * A header bar, for the close button.
     *
     * An AdwDialog with a bare child has no visible way out: Escape
     * works, but a modal whose only exit is a keystroke nobody
     * advertised is one people report as stuck -- and they were right
     * to.
     */
    {
        GtkWidget *toolbar = adw_toolbar_view_new();
        GtkWidget *header = adw_header_bar_new();
        GtkWidget *cancel = gtk_button_new_with_label("Cancel");

        g_signal_connect_swapped(cancel, "clicked",
                                 G_CALLBACK(adw_dialog_close), window);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);

        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);

        adw_dialog_set_child(window, toolbar);
    }

    adw_dialog_present(window, GTK_WIDGET(self));
}

/*
 * Importing an agent, rather than creating one.
 *
 * Two different things wear the name, and the dialog shows both because
 * which one a person wants depends on where the agent already is.
 *
 * Adopting is the common one and the one nothing surfaced before: a
 * workspace already under clawtilla's own directories -- an agent removed
 * from the config, a design that was never committed, a config restored
 * from a backup -- needs no copying at all, only a config entry.  Those
 * accumulate silently, and `agent discover` was the only way to find out
 * they were there.
 *
 * Copying is the other: somebody else's standalone libreclaw workspace,
 * which is read from wherever it sits and written into a workspace
 * clawtilla chose.
 */
typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    GtkWidget   *found_group;
    GtkWidget   *id_entry;
    GtkWidget   *from_row;
    GtkWidget   *keep_git_row;
    gchar       *from_path;
} ImportAgentDialog;

static void
import_agent_dialog_free(gpointer data)
{
    ImportAgentDialog *dialog = data;

    g_free(dialog->from_path);
    g_free(dialog);
}

/*
 * Adopts a workspace that is already where clawtilla keeps them.
 *
 * This is agent.create with nothing but an id, which is the whole of it:
 * the workspace and the mailbox are already at the paths the daemon
 * looks for, so there is deliberately no second code path that could
 * treat an imported agent differently from a created one.
 */
static void
on_adopt_found(GtkButton *button, gpointer user_data)
{
    ImportAgentDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    const gchar *agent_id = g_object_get_data(G_OBJECT(button), "agent-id");
    g_autoptr(JsonNode) reply = NULL;

    if (agent_id == NULL)
        return;

    reply = clawt_window_request(self, "agent.create",
                                 clawt_build_payload("id", agent_id, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Imported. Check it over before starting it.");
    refresh_agents(self);
    adw_dialog_close(dialog->dialog);
}

/*
 * What is on disk and not in the config.
 *
 * Each row says what the directory holds, because "there is a directory
 * called researcher" is not enough to decide with: a workspace with a
 * mailbox and a memory database is an agent somebody ran, and one with
 * nothing but a config.yaml is a design that was abandoned halfway.
 */
static void
import_dialog_fill_found(ImportAgentDialog *dialog)
{
    ClawtWindow *self = dialog->window;
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *found;
    guint i;

    reply = clawt_window_request(self, "agent.discover", NULL);

    if (reply == NULL)
        return;

    found = json_object_get_array_member(json_node_get_object(reply),
                                          "found");

    if (found == NULL || json_array_get_length(found) == 0) {
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "Nothing unclaimed on disk");
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(row),
            "Every workspace clawtilla can see is already an agent.");
        gtk_widget_set_sensitive(row, FALSE);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(dialog->found_group),
                                  row);
        return;
    }

    for (i = 0; i < json_array_get_length(found); i++) {
        JsonObject *entry = json_array_get_object_element(found, i);
        const gchar *agent_id = clawt_json_string(entry, "id", NULL);
        const gchar *holds = clawt_json_string(entry, "holds", NULL);
        const gchar *path = clawt_json_string(entry, "path", NULL);
        GtkWidget *row = adw_action_row_new();
        GtkWidget *button = gtk_button_new_with_label("Import");

        if (agent_id == NULL)
            continue;

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), agent_id);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                    holds != NULL && *holds != '\0'
                                        ? holds : path);

        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        g_object_set_data_full(G_OBJECT(button), "agent-id",
                               g_strdup(agent_id), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(on_adopt_found),
                         dialog);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(dialog->found_group),
                                  row);
    }
}

static void
on_import_folder_chosen(GObject *source, GAsyncResult *result,
                        gpointer user_data)
{
    ImportAgentDialog *dialog = user_data;
    g_autoptr(GFile) folder = NULL;
    g_autoptr(GError) error = NULL;

    folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source),
                                                   result, &error);

    /* Dismissing the chooser is not a failure worth a toast. */
    if (folder == NULL)
        return;

    g_free(dialog->from_path);
    dialog->from_path = g_file_get_path(folder);

    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->from_row),
                                dialog->from_path);

    /*
     * The id defaults to the directory's own name, which is right often
     * enough to be worth filling in and is still editable when it is
     * not.
     */
    if (*gtk_editable_get_text(GTK_EDITABLE(dialog->id_entry)) == '\0') {
        g_autofree gchar *base = g_file_get_basename(folder);

        gtk_editable_set_text(GTK_EDITABLE(dialog->id_entry), base);
    }
}

static void
on_import_choose_folder(GtkButton *button, gpointer user_data)
{
    ImportAgentDialog *dialog = user_data;
    GtkFileDialog *chooser = gtk_file_dialog_new();

    (void)button;

    gtk_file_dialog_set_title(chooser, "The agent's workspace");
    gtk_file_dialog_select_folder(chooser,
                                  GTK_WINDOW(dialog->window), NULL,
                                  on_import_folder_chosen, dialog);
    g_object_unref(chooser);
}

static void
on_import_from_directory(GtkButton *button, gpointer user_data)
{
    ImportAgentDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    const gchar *agent_id;
    g_autoptr(JsonNode) reply = NULL;
    gboolean keep_git;

    (void)button;

    agent_id = gtk_editable_get_text(GTK_EDITABLE(dialog->id_entry));

    if (agent_id == NULL || *agent_id == '\0') {
        clawt_window_toast(self, "An imported agent needs an id.");
        return;
    }

    if (dialog->from_path == NULL) {
        clawt_window_toast(self, "Choose the directory to import from.");
        return;
    }

    keep_git = adw_switch_row_get_active(
        ADW_SWITCH_ROW(dialog->keep_git_row));

    reply = clawt_window_request(
        self, "agent.import",
        clawt_build_payload("id", agent_id, "from", dialog->from_path,
                            "keep_git", keep_git ? "true" : "false", NULL));

    /* Left open on failure, so the path and id are still there to fix. */
    if (reply == NULL)
        return;

    clawt_window_toast(self, "Imported. Check it over before starting it.");
    refresh_agents(self);
    adw_dialog_close(dialog->dialog);
}

static void
on_import_agent(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    ImportAgentDialog *dialog = g_new0(ImportAgentDialog, 1);
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *from_group = adw_preferences_group_new();
    GtkWidget *choose;
    GtkWidget *import;

    (void)button;

    dialog->window = self;
    dialog->dialog = window;

    adw_dialog_set_title(window, "Import an agent");
    adw_dialog_set_content_width(window, 520);

    /* ── Already on disk ── */
    dialog->found_group = adw_preferences_group_new();
    adw_preferences_group_set_title(
        ADW_PREFERENCES_GROUP(dialog->found_group), "Already on disk");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(dialog->found_group),
        "Workspaces clawtilla can see that no agent claims. Importing one "
        "adds the config entry it is missing; nothing is copied or moved.");
    import_dialog_fill_found(dialog);

    /* ── From a directory ── */
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(from_group),
                                    "From somewhere else");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(from_group),
        "Copies a standalone libreclaw workspace in. Its provider and "
        "model come with it, so an import does not quietly move the agent "
        "onto the fleet defaults.");

    dialog->id_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(dialog->id_entry),
                                        FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->id_entry),
                                  "Id");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(from_group),
                              dialog->id_entry);

    dialog->from_row = adw_action_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->from_row),
                                  "Workspace");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->from_row),
                                "Nothing chosen");
    choose = gtk_button_new_with_label("Choose\342\200\246");
    gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
    g_signal_connect(choose, "clicked", G_CALLBACK(on_import_choose_folder),
                     dialog);
    adw_action_row_add_suffix(ADW_ACTION_ROW(dialog->from_row), choose);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(from_group),
                              dialog->from_row);

    dialog->keep_git_row = adw_switch_row_new();
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(dialog->keep_git_row), "Keep git history");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->keep_git_row),
        "Copies .git too. Off by default: the new workspace is usually "
        "not the same repository.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(from_group),
                              dialog->keep_git_row);

    import = gtk_button_new_with_label("Import");
    gtk_widget_add_css_class(import, "suggested-action");
    gtk_widget_set_margin_top(import, 12);
    g_signal_connect(import, "clicked", G_CALLBACK(on_import_from_directory),
                     dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(from_group), import);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(dialog->found_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(from_group));

    g_object_set_data_full(G_OBJECT(window), "dialog", dialog,
                           import_agent_dialog_free);

    {
        GtkWidget *toolbar = adw_toolbar_view_new();
        GtkWidget *header = adw_header_bar_new();
        GtkWidget *cancel = gtk_button_new_with_label("Cancel");

        g_signal_connect_swapped(cancel, "clicked",
                                 G_CALLBACK(adw_dialog_close), window);
        adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);

        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);

        adw_dialog_set_child(window, toolbar);
    }

    adw_dialog_present(window, GTK_WIDGET(self));
}

/* ── Connections ─────────────────────────────────────────────────── */

/*
 * Which daemon this window is looking at.
 *
 * The daemon owns every agent process, credential and socket, and a
 * client is a fold over one event stream -- which is precisely what makes
 * pointing the same client at a daemon on another machine a change of one
 * object rather than a second program.  clawtillad binds its tailnet
 * address by default, so the far end usually needs nothing set up.
 *
 * Switching replaces self->client wholesale.  Everything on screen came
 * from the old daemon and none of it is true of the new one: agent ids
 * are per-fleet, so a stale selection would show one daemon's transcript
 * under another daemon's agent.
 */

static void switch_connection(ClawtWindow *self, ClawtConnection *connection);
static void rebuild_connection_menu(ClawtWindow *self);
static void on_manage_connections(GtkButton *button, gpointer user_data);

/*
 * The name shown on the button, and in the window subtitle.
 *
 * Worth being loud about.  Every destructive verb in the client -- remove
 * an agent, tear down its VM, reset its session -- acts on whichever
 * daemon is connected, and a person who has forgotten they are pointed at
 * another machine will not find out until afterwards.
 */
static void
update_connection_label(ClawtWindow *self)
{
    GtkWidget *title = g_object_get_data(G_OBJECT(self), "title");
    const gchar *name = self->active_connection != NULL
                        ? clawt_connection_get_name(self->active_connection)
                        : "Local";
    gboolean remote = self->active_connection != NULL &&
                      !clawt_connection_is_local(self->active_connection);

    if (self->connection_button != NULL) {
        adw_button_content_set_label(
            ADW_BUTTON_CONTENT(
                gtk_menu_button_get_child(
                    GTK_MENU_BUTTON(self->connection_button))),
            name);

        /*
         * A remote connection is styled, not merely labelled.  The label
         * alone is a word in a header bar that stops being read after the
         * first day.
         */
        if (remote)
            gtk_widget_add_css_class(self->connection_button, "accent");
        else
            gtk_widget_remove_css_class(self->connection_button, "accent");
    }

    if (title != NULL)
        adw_window_title_set_subtitle(ADW_WINDOW_TITLE(title),
                                      remote ? name : NULL);
}

static void
on_connection_chosen(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    ClawtConnection *connection = g_object_get_data(G_OBJECT(button),
                                                     "connection");

    gtk_menu_button_popdown(GTK_MENU_BUTTON(self->connection_button));

    if (connection != NULL)
        switch_connection(self, connection);
}

/*
 * Everything the previous daemon told us.
 *
 * Not an optimisation -- a correctness step.  Agent ids, room ids and
 * message ids are all per-daemon, so keeping any of them across a switch
 * means the deduplication set silently swallows the new daemon's
 * messages whenever an id happens to collide, and the selected agent
 * names one that may not exist.
 */
static void
forget_daemon_state(ClawtWindow *self)
{
    g_clear_pointer(&self->selected_agent, g_free);
    g_clear_pointer(&self->selected_room, g_free);
    g_clear_pointer(&self->flow_room, g_free);

    g_hash_table_remove_all(self->shown);
    g_hash_table_remove_all(self->drafts);
    g_hash_table_remove_all(self->unread);
    g_hash_table_remove_all(self->dm_rooms);

    /* Whatever the next daemon replays belongs to before we got here. */
    self->connected_at = g_get_real_time();

    /*
     * Alerts belong to the daemon that raised them: an agent id in one
     * of them names something the next daemon may not have.
     */
    if (self->alerts != NULL)
        g_ptr_array_set_size(self->alerts, 0);

    g_clear_pointer(&self->alerts_agent, g_free);
    update_alert_badge(self);
    refresh_alerts(self);

    /*
     * Teams are per-daemon too.  The refresh below repopulates this, but
     * only if the new daemon answers team.list -- and offering another
     * machine's teams in the context menu is how an agent gets moved
     * onto one this fleet has never declared.
     */
    g_clear_pointer(&self->teams_seen, json_node_unref);

    if (self->collapsed_teams != NULL)
        g_hash_table_remove_all(self->collapsed_teams);

    reset_transcript(self);
    clear_list(self->sidebar);
    set_activity(self, NULL);
}

static void
switch_connection(ClawtWindow *self, ClawtConnection *connection)
{
    g_autoptr(ClawtClient) client = NULL;
    g_autoptr(GError) error = NULL;

    g_return_if_fail(connection != NULL);

    /* Already there.  Reconnecting would be a visible no-op. */
    if (self->active_connection != NULL &&
        g_strcmp0(clawt_connection_get_name(self->active_connection),
                  clawt_connection_get_name(connection)) == 0 &&
        clawt_client_is_connected(self->client))
        return;

    client = clawt_connection_create_client(connection);

    /*
     * The new client is connected *before* the old one is let go.  A
     * remote daemon that is not running, or a token that is wrong, is
     * the ordinary case here -- and dropping the working connection
     * first would leave the window showing nothing, connected to
     * nowhere, because of a typo in a port.
     */
    if (!clawt_client_connect(client, &error)) {
        g_autofree gchar *where = clawt_connection_describe(connection);
        g_autofree gchar *message =
            g_strdup_printf("%s (%s): %s", clawt_connection_get_name(connection),
                            where, error->message);

        clawt_window_toast(self, message);
        return;
    }

    clawt_client_set_auto_reconnect(client, TRUE);

    if (self->client != NULL) {
        g_signal_handlers_disconnect_by_data(self->client, self);
        clawt_client_disconnect(self->client);
        g_object_unref(self->client);
    }

    self->client = g_steal_pointer(&client);
    g_signal_connect(self->client, "event", G_CALLBACK(on_daemon_event),
                     self);

    /*
     * From 0, not from the cursor the previous daemon had reached.  A
     * cursor is a position in one daemon's event stream and means
     * nothing in another's; asking a fresh daemon to resume from it
     * either replays the wrong events or reports it cannot resume.
     */
    clawt_client_subscribe(self->client, 0, NULL, NULL);

    if (self->active_connection != NULL)
        clawt_connection_free(self->active_connection);

    self->active_connection = clawt_connection_copy(connection);

    forget_daemon_state(self);
    update_connection_label(self);
    rebuild_connection_menu(self);

    refresh_agents(self);
    refresh_tasks(self);
    refresh_routines(self);

    {
        g_autofree gchar *where = clawt_connection_describe(connection);
        g_autofree gchar *message =
            g_strdup_printf("Connected to %s (%s).",
                            clawt_connection_get_name(connection), where);

        clawt_window_toast(self, message);
    }
}

/*
 * Rebuilt rather than updated, because the set changes from the editor
 * beside it and a menu that disagrees with the file is worse than one
 * rebuilt more often than it needs to be.
 *
 * Plain GtkButtons in a box, not a GtkListBox: a list box selects a row
 * when it takes focus and a popover takes focus as it opens, so the first
 * entry would connect itself the moment the menu appeared.
 */
static void
rebuild_connection_menu(ClawtWindow *self)
{
    GtkWidget *box = self->connection_list;
    GtkWidget *child;
    GtkWidget *manage;
    guint i;

    if (box == NULL)
        return;

    while ((child = gtk_widget_get_first_child(box)) != NULL)
        gtk_box_remove(GTK_BOX(box), child);

    for (i = 0; i < self->connections->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(self->connections, i);
        g_autofree gchar *where = clawt_connection_describe(connection);
        GtkWidget *button = gtk_button_new();
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *labels = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *name = gtk_label_new(clawt_connection_get_name(connection));
        GtkWidget *detail = gtk_label_new(where);
        gboolean current =
            self->active_connection != NULL &&
            g_strcmp0(clawt_connection_get_name(self->active_connection),
                      clawt_connection_get_name(connection)) == 0;

        gtk_widget_set_halign(name, GTK_ALIGN_START);
        gtk_widget_set_halign(detail, GTK_ALIGN_START);
        gtk_widget_add_css_class(detail, "dim-label");
        gtk_widget_add_css_class(detail, "caption");
        gtk_label_set_ellipsize(GTK_LABEL(detail), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(detail), 28);

        gtk_box_append(GTK_BOX(labels), name);
        gtk_box_append(GTK_BOX(labels), detail);
        gtk_widget_set_hexpand(labels, TRUE);
        gtk_box_append(GTK_BOX(row), labels);

        if (current) {
            GtkWidget *tick =
                gtk_image_new_from_icon_name("object-select-symbolic");

            gtk_widget_set_valign(tick, GTK_ALIGN_CENTER);
            gtk_box_append(GTK_BOX(row), tick);
        }

        gtk_button_set_child(GTK_BUTTON(button), row);
        gtk_widget_add_css_class(button, "flat");

        /*
         * The profile is copied onto the button rather than referenced.
         * The list behind it is rebuilt whenever the editor saves, and a
         * button outliving that rebuild by one click would hand
         * switch_connection() a freed profile.
         */
        g_object_set_data_full(G_OBJECT(button), "connection",
                               clawt_connection_copy(connection),
                               (GDestroyNotify)clawt_connection_free);
        g_signal_connect(button, "clicked", G_CALLBACK(on_connection_chosen),
                         self);

        gtk_box_append(GTK_BOX(box), button);
    }

    gtk_box_append(GTK_BOX(box),
                   gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    manage = gtk_button_new_with_label("Manage connections\342\200\246");
    gtk_widget_add_css_class(manage, "flat");
    g_signal_connect(manage, "clicked", G_CALLBACK(on_manage_connections),
                     self);
    gtk_box_append(GTK_BOX(box), manage);
}

/*
 * The saved profiles, with the local daemon always first.
 *
 * Local is synthesised rather than stored: it is where the client points
 * with no configuration at all, and a person who has never opened this
 * menu should still find their own machine in it.  It also carries
 * whatever --socket was given, so a second local daemon on a different
 * socket stays reachable after a trip to a remote one.
 */
static void
reload_connections(ClawtWindow *self)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) saved = NULL;
    guint i;

    saved = clawt_connection_list_load(NULL, &error);

    if (saved == NULL) {
        /*
         * A connections file that cannot be read is worth saying out
         * loud, once.  Failing silently would leave a person looking at
         * a menu with their remote hosts missing and no reason given.
         */
        clawt_window_toast(self, error->message);
        saved = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_connection_free);
    }

    g_ptr_array_set_size(self->connections, 0);
    g_ptr_array_add(self->connections,
                    clawt_connection_new_local("Local", self->local_socket));

    for (i = 0; i < saved->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(saved, i);

        /* The synthesised Local already covers a file's own local entry. */
        if (clawt_connection_is_local(connection))
            continue;

        g_ptr_array_add(self->connections,
                        clawt_connection_copy(connection));
    }

    rebuild_connection_menu(self);
}

/* ── The connection editor ───────────────────────────────────────── */

typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    GtkWidget   *list_group;
    /*
     * The rows we put in the group, so they can be taken back out.
     * AdwPreferencesGroup wraps every added row in a list-box row of its
     * own and offers no way to enumerate them, so walking its children
     * hands back its internal boxes rather than anything
     * adw_preferences_group_remove() will accept.
     */
    GPtrArray   *rows;
    GtkWidget   *name_entry;
    GtkWidget   *host_entry;
    GtkWidget   *port_entry;
    GtkWidget   *token_entry;
    GtkWidget   *tls_row;
    GtkWidget   *insecure_row;
} ConnectionDialog;

static void connection_dialog_fill(ConnectionDialog *dialog);

/*
 * Saves the remote profiles only.
 *
 * Local is synthesised at load, so writing it back would add an entry
 * that reload_connections() then skips -- a line in the file that does
 * nothing, which is how a file starts collecting explanations.
 */
static gboolean
save_connections(ClawtWindow *self)
{
    g_autoptr(GPtrArray) remotes = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_connection_free);
    g_autoptr(GError) error = NULL;
    guint i;

    for (i = 0; i < self->connections->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(self->connections, i);

        if (clawt_connection_is_local(connection))
            continue;

        g_ptr_array_add(remotes, clawt_connection_copy(connection));
    }

    if (!clawt_connection_list_save(NULL, remotes, &error)) {
        clawt_window_toast(self, error->message);
        return FALSE;
    }

    return TRUE;
}

static void
on_connection_removed(GtkButton *button, gpointer user_data)
{
    ConnectionDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    const gchar *name = g_object_get_data(G_OBJECT(button), "connection-name");
    guint i;

    if (name == NULL)
        return;

    for (i = 0; i < self->connections->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(self->connections, i);

        if (clawt_connection_is_local(connection))
            continue;

        if (g_strcmp0(clawt_connection_get_name(connection), name) != 0)
            continue;

        g_ptr_array_remove_index(self->connections, i);
        break;
    }

    if (!save_connections(self))
        return;

    rebuild_connection_menu(self);
    connection_dialog_fill(dialog);
    clawt_window_toast(self, "Removed.");
}

static void
on_connection_added(GtkButton *button, gpointer user_data)
{
    ConnectionDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    ClawtConnection *connection;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry));
    const gchar *host = gtk_editable_get_text(GTK_EDITABLE(dialog->host_entry));
    const gchar *port_text =
        gtk_editable_get_text(GTK_EDITABLE(dialog->port_entry));
    const gchar *token =
        gtk_editable_get_text(GTK_EDITABLE(dialog->token_entry));
    gint64 port;

    (void)button;

    if (host == NULL || *host == '\0') {
        clawt_window_toast(self, "A connection needs a host.");
        return;
    }

    port = (port_text != NULL && *port_text != '\0')
           ? g_ascii_strtoll(port_text, NULL, 10)
           : CLAWT_DEFAULT_TCP_PORT;

    if (port <= 0 || port > G_MAXUINT16) {
        clawt_window_toast(self, "That is not a port.");
        return;
    }

    if (name == NULL || *name == '\0')
        name = host;

    /*
     * A name is how the menu and the CLI's --profile refer to a
     * connection, so two of them cannot share one: the second would be
     * unreachable by name and would look like the first had simply not
     * saved.
     */
    if (clawt_connection_list_find(self->connections, name) != NULL) {
        clawt_window_toast(self, "There is already a connection with that "
                                 "name.");
        return;
    }

    connection = clawt_connection_new_remote(name, host, (guint16)port,
                                             (token != NULL && *token != '\0')
                                                 ? token : NULL);
    clawt_connection_set_tls(
        connection, adw_switch_row_get_active(ADW_SWITCH_ROW(dialog->tls_row)),
        adw_switch_row_get_active(ADW_SWITCH_ROW(dialog->insecure_row)));

    g_ptr_array_add(self->connections, connection);

    if (!save_connections(self)) {
        g_ptr_array_remove(self->connections, connection);
        return;
    }

    gtk_editable_set_text(GTK_EDITABLE(dialog->name_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(dialog->host_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(dialog->token_entry), "");

    rebuild_connection_menu(self);
    connection_dialog_fill(dialog);
    clawt_window_toast(self, "Saved.");
}

static void
connection_dialog_fill(ConnectionDialog *dialog)
{
    ClawtWindow *self = dialog->window;
    guint i;
    guint shown = 0;

    for (i = 0; i < dialog->rows->len; i++)
        adw_preferences_group_remove(ADW_PREFERENCES_GROUP(dialog->list_group),
                                     g_ptr_array_index(dialog->rows, i));

    g_ptr_array_set_size(dialog->rows, 0);

    for (i = 0; i < self->connections->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(self->connections, i);
        g_autofree gchar *where = NULL;
        GtkWidget *row;
        GtkWidget *remove;

        if (clawt_connection_is_local(connection))
            continue;

        where = clawt_connection_describe(connection);
        row = adw_action_row_new();

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(
            ADW_PREFERENCES_ROW(row),
            clawt_connection_get_name(connection));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), where);

        remove = gtk_button_new_from_icon_name("user-trash-symbolic");
        gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(remove, "flat");
        g_object_set_data_full(
            G_OBJECT(remove), "connection-name",
            g_strdup(clawt_connection_get_name(connection)), g_free);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_connection_removed),
                         dialog);

        adw_action_row_add_suffix(ADW_ACTION_ROW(row), remove);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(dialog->list_group),
                                  row);
        g_ptr_array_add(dialog->rows, row);
        shown++;
    }

    if (shown == 0) {
        GtkWidget *row = adw_action_row_new();

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      "No remote daemons yet");
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(row),
            "clawtillad listens on its tailnet address by default, so a "
            "machine on your tailnet usually needs nothing set up.");
        gtk_widget_set_sensitive(row, FALSE);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(dialog->list_group),
                                  row);
        g_ptr_array_add(dialog->rows, row);
    }
}

static void
connection_dialog_free(gpointer data)
{
    ConnectionDialog *dialog = data;

    g_ptr_array_unref(dialog->rows);
    g_free(dialog);
}

static void
on_manage_connections(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    ConnectionDialog *dialog = g_new0(ConnectionDialog, 1);
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *add_group = adw_preferences_group_new();
    GtkWidget *add;
    g_autofree gchar *port_default = NULL;

    (void)button;

    gtk_menu_button_popdown(GTK_MENU_BUTTON(self->connection_button));

    dialog->window = self;
    dialog->dialog = window;
    dialog->rows = g_ptr_array_new();

    adw_dialog_set_title(window, "Connections");
    adw_dialog_set_content_width(window, 520);

    dialog->list_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(dialog->list_group),
                                    "Saved");
    connection_dialog_fill(dialog);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(add_group),
                                    "Add a daemon");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(add_group),
        "The address the daemon reports at startup, and the token from "
        "`clawtilla daemon token` on that machine.");

    dialog->name_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(dialog->name_entry), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->name_entry),
                                  "Name");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group),
                              dialog->name_entry);

    dialog->host_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(dialog->host_entry), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->host_entry),
                                  "Host");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group),
                              dialog->host_entry);

    dialog->port_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(dialog->port_entry), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->port_entry),
                                  "Port");
    port_default = g_strdup_printf("%d", CLAWT_DEFAULT_TCP_PORT);
    gtk_editable_set_text(GTK_EDITABLE(dialog->port_entry), port_default);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group),
                              dialog->port_entry);

    dialog->token_entry = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->token_entry),
                                  "Token");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group),
                              dialog->token_entry);

    dialog->tls_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->tls_row),
                                  "TLS");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->tls_row),
        "Only if that daemon has a certificate. Without one the token "
        "crosses the network in the clear -- inside a tailnet, WireGuard "
        "is already encrypting it.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group),
                              dialog->tls_row);

    dialog->insecure_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->insecure_row),
                                  "Accept an unknown certificate");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->insecure_row),
        "For a self-signed certificate on a machine you control. It turns "
        "off the check that would notice somebody else answering.");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group),
                              dialog->insecure_row);

    add = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_set_margin_top(add, 12);
    g_signal_connect(add, "clicked", G_CALLBACK(on_connection_added), dialog);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group), add);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(dialog->list_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(add_group));

    g_object_set_data_full(G_OBJECT(window), "dialog", dialog,
                           connection_dialog_free);

    {
        GtkWidget *toolbar = adw_toolbar_view_new();
        GtkWidget *header = adw_header_bar_new();

        adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
        adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);

        adw_dialog_set_child(window, toolbar);
    }

    adw_dialog_present(window, GTK_WIDGET(self));
}

/*
 * The menu entries beside the + button.
 *
 * A GAction hands its callback the action and a parameter; the button's
 * own ::clicked hands it the button.  Two thin wrappers rather than one
 * signature bent to fit both, because the alternative is a cast that is
 * wrong on one of the two paths.
 */
static void
on_new_agent_activate(GSimpleAction *action, GVariant *parameter,
                      gpointer user_data)
{
    (void)action;
    (void)parameter;

    on_new_agent(NULL, user_data);
}

static void
on_import_agent_activate(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data)
{
    (void)action;
    (void)parameter;

    on_import_agent(NULL, user_data);
}

/* ── Construction ────────────────────────────────────────────────── */

static GtkWidget *
build_chat_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *attach;
    GtkWidget *send;

    self->transcript = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));

    /*
     * The last turn should not sit against the composer.  18px because
     * this is a container edge meeting a different control, not a gap
     * between two turns.
     */
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->transcript), 18);

    /*
     * A measure limit, so widening the window widens the margins rather
     * than the lines.
     *
     * Nothing capped the column, so a body label's natural width was
     * whatever the window was: a 1280px window measured 877px of text on
     * its longest line, about 137 characters, and this scales with the
     * display -- a wider one is worse rather than equal.  The clamp
     * bounds it instead.
     *
     * Continuous prose reads comfortably at
     * roughly 45 to 90 characters; past that the eye has to cross the
     * full width and then hunt back for the start of the next line,
     * which is the failure measure exists to prevent.
     *
     * The clamp goes inside the scrolled window rather than around it,
     * so the scrollbar and the wheel target stay at the window edge
     * where they are reachable.
     *
     * AdwClamp's own defaults are the right numbers and are deliberately
     * left alone: maximum-size is 600 and tightening-threshold is 400,
     * which is what stops the column snapping in a narrow window.  A
     * default left unset is a number the platform can revise; a
     * hardcoded one is a number somebody has to maintain.
     *
     * What 600 leaves for words is 600 less CHAT_BODY_INSET at the start
     * and CHAT_ROW_MARGIN at the end: 532px, about 82 characters in the
     * default font.  An earlier reading of this said 576 and 90, which
     * forgot that an agent's body is indented past its avatar as well as
     * inset from the clamp -- 44px of it, seven characters' worth, on
     * every line.
     *
     * The body does not fill even that.  A wrapping GtkLabel set
     * GTK_ALIGN_START is allocated its natural width rather than the
     * column's, and measured here that is 437px against the 532px
     * offered -- about 66 characters.  So 82 is what the clamp permits
     * and 66 is what a reader sees; the two differ by the label's
     * alignment, not by anything decided here.
     */
    {
        GtkWidget *clamp = adw_clamp_new();

        adw_clamp_set_child(ADW_CLAMP(clamp), GTK_WIDGET(self->transcript));
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), clamp);
    }

    /*
     * Right-clicking the conversation itself, rather than one message.
     * Attached to the scrolled window so the empty space below the last
     * message counts too -- that is where somebody actually right
     * clicks.
     */
    {
        static const MenuEntry conversation_menu[] = {
            { "Copy conversation as text",     "copy-text" },
            { "Copy conversation as markdown", "copy-markdown" },
            { "Copy conversation as org",      "copy-org" },
            { NULL, NULL },
            { "Open in $EDITOR as text",       "edit-text" },
            { "Open in $EDITOR as markdown",   "edit-markdown" },
            { "Open in $EDITOR as org",        "edit-org" },
            { NULL, NULL },
            { "Save as text\xe2\x80\xa6",      "save-text" },
            { "Save as markdown\xe2\x80\xa6",  "save-markdown" },
            { "Save as org\xe2\x80\xa6",       "save-org" }
        };

        add_context_menu(self, scroll, conversation_menu,
                         G_N_ELEMENTS(conversation_menu),
                         on_conversation_action, NULL);
    }
    gtk_widget_set_vexpand(scroll, TRUE);
    self->transcript_scroll = GTK_SCROLLED_WINDOW(scroll);

    /*
     * Following is maintained from three places: the reader scrolling
     * (below), and the content growing (either of these two), because
     * "am I at the bottom" changes for both reasons.
     */
    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(scroll)),
                     "notify::upper", G_CALLBACK(on_transcript_grew), self);
    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(scroll)),
                     "notify::page-size",
                     G_CALLBACK(on_transcript_grew), self);

    g_signal_connect(gtk_scrolled_window_get_vadjustment(
                         GTK_SCROLLED_WINDOW(scroll)),
                     "value-changed", G_CALLBACK(on_scrolled), self);

    /*
     * The activity line.  A chat window that shows nothing between the
     * question and the answer is indistinguishable from a broken one,
     * and an agent turn can easily run for minutes.
     */
    self->streaming = GTK_LABEL(gtk_label_new(NULL));
    gtk_widget_add_css_class(GTK_WIDGET(self->streaming), "dim-label");
    gtk_label_set_wrap(self->streaming, TRUE);
    gtk_label_set_xalign(self->streaming, 0.0f);
    gtk_label_set_ellipsize(self->streaming, PANGO_ELLIPSIZE_END);

    self->activity_spinner = GTK_SPINNER(gtk_spinner_new());
    self->activity_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(self->activity_bar),
                   GTK_WIDGET(self->activity_spinner));
    gtk_box_append(GTK_BOX(self->activity_bar), GTK_WIDGET(self->streaming));
    gtk_widget_set_visible(self->activity_bar, FALSE);

    /* The queued files, hidden until there are some. */
    self->attachments = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_top(self->attachments, 6);
    gtk_widget_set_visible(self->attachments, FALSE);

    self->entry = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(self->entry, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(self->entry, 8);
    gtk_text_view_set_bottom_margin(self->entry, 8);
    gtk_text_view_set_left_margin(self->entry, 8);
    gtk_text_view_set_right_margin(self->entry, 8);
    gtk_widget_set_hexpand(GTK_WIDGET(self->entry), TRUE);
    g_signal_connect(gtk_text_view_get_buffer(self->entry), "changed",
                     G_CALLBACK(on_entry_changed), self);

    /*
     * The placeholder is a label under the view rather than a property,
     * because GtkTextView has none.  Hidden as soon as anything is
     * typed, and it must not eat clicks meant for the text.
     */
    self->placeholder = gtk_label_new(
        "Message  \xe2\x80\x94  / for commands, Ctrl+G to write it in $EDITOR");
    gtk_widget_add_css_class(self->placeholder, "dim-label");
    gtk_widget_set_halign(self->placeholder, GTK_ALIGN_START);
    gtk_widget_set_valign(self->placeholder, GTK_ALIGN_START);
    gtk_widget_set_margin_start(self->placeholder, 10);
    gtk_widget_set_margin_top(self->placeholder, 8);
    gtk_widget_set_can_target(self->placeholder, FALSE);

    {
        GtkEventController *keys = gtk_event_controller_key_new();

        /*
         * The capture phase, so Ctrl+V is seen before the entry's own
         * paste handler consumes it.
         */
        gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
        g_signal_connect(keys, "key-pressed", G_CALLBACK(on_entry_key), self);
        gtk_widget_add_controller(GTK_WIDGET(self->entry), keys);
    }

    /*
     * The command list, parented to the entry it belongs to.  Note that
     * this makes it a *child* of the entry, so anything that walks the
     * entry's children has to expect it.
     */
    self->command_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->command_list, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(self->command_list),
                             "navigation-sidebar");
    g_signal_connect(self->command_list, "row-selected",
                     G_CALLBACK(on_command_row_selected), self);

    {
        GtkWidget *command_scroll = gtk_scrolled_window_new();

        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(command_scroll),
                                      GTK_WIDGET(self->command_list));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(command_scroll),
                                       GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(command_scroll), 220);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(command_scroll), TRUE);

        self->command_revealer = gtk_revealer_new();
        gtk_revealer_set_transition_type(
            GTK_REVEALER(self->command_revealer),
            GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
        gtk_revealer_set_child(GTK_REVEALER(self->command_revealer),
                               command_scroll);
    }

    attach = gtk_button_new_from_icon_name("mail-attachment-symbolic");
    gtk_widget_set_tooltip_text(attach,
                                "Send files with this message. You can also "
                                "paste an image.");
    g_signal_connect(attach, "clicked", G_CALLBACK(on_attach_clicked), self);

    send = gtk_button_new_from_icon_name("document-send-symbolic");
    g_signal_connect(send, "clicked", G_CALLBACK(on_send), self);

    {
        GtkWidget *overlay = gtk_overlay_new();
        GtkWidget *entry_scroll = gtk_scrolled_window_new();

        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(entry_scroll),
                                      GTK_WIDGET(self->entry));
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(entry_scroll),
                                       GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);

        /*
         * Grows with the message and then stops: a pasted essay should
         * not push the transcript off the top of the window.
         */
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(entry_scroll), 200);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(entry_scroll), TRUE);
        gtk_widget_add_css_class(entry_scroll, "card");
        gtk_widget_set_hexpand(entry_scroll, TRUE);

        gtk_overlay_set_child(GTK_OVERLAY(overlay), entry_scroll);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->placeholder);

        gtk_box_append(GTK_BOX(entry_box), overlay);
    }

    gtk_widget_set_valign(attach, GTK_ALIGN_END);
    gtk_widget_set_valign(send, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(entry_box), attach);
    gtk_box_append(GTK_BOX(entry_box), send);
    gtk_widget_set_margin_top(entry_box, 6);
    gtk_widget_set_margin_bottom(entry_box, 12);

    /*
     * The pill that says something arrived while you were reading.
     *
     * It floats over the transcript rather than sitting in the column,
     * because it is about the transcript rather than part of it, and it
     * carries the words -- a bare arrow says "go down", which the
     * scrollbar already says.  What was missing was "something is down
     * there", and that needs saying once, not counting: a message here
     * is a whole turn, so "3" could be three lines or three screens.
     *
     * It appears only when a message has arrived while `following` is
     * false, not merely because the reader has scrolled up.  A control
     * that is always there while you read carries no information; one
     * whose appearance is the signal carries exactly the bit that is
     * missing.
     */
    {
        GtkWidget *overlay = gtk_overlay_new();
        GtkWidget *revealer = gtk_revealer_new();
        GtkWidget *pill = gtk_button_new();
        GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        gtk_box_append(GTK_BOX(content),
                       gtk_image_new_from_icon_name("go-bottom-symbolic"));
        gtk_box_append(GTK_BOX(content), gtk_label_new("New messages"));

        gtk_button_set_child(GTK_BUTTON(pill), content);
        gtk_widget_add_css_class(pill, "osd");
        gtk_widget_add_css_class(pill, "pill");
        gtk_widget_set_tooltip_text(pill, "Jump to latest");
        gtk_accessible_update_property(GTK_ACCESSIBLE(pill),
                                       GTK_ACCESSIBLE_PROPERTY_LABEL,
                                       "Jump to latest", -1);
        g_signal_connect(pill, "clicked", G_CALLBACK(on_jump_to_latest),
                         self);

        gtk_revealer_set_child(GTK_REVEALER(revealer), pill);
        gtk_revealer_set_transition_type(GTK_REVEALER(revealer),
                                         GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);

        /*
         * Centred over the transcript, 12px clear of the composer.  Not
         * anchored to the end: the reader this is for is scrolled up and
         * therefore the one most likely to be holding the scrollbar.
         */
        gtk_widget_set_halign(revealer, GTK_ALIGN_CENTER);
        gtk_widget_set_valign(revealer, GTK_ALIGN_END);
        gtk_widget_set_margin_bottom(revealer, 12);
        gtk_widget_set_can_target(revealer, FALSE);

        self->jump_revealer = GTK_REVEALER(revealer);

        gtk_widget_set_vexpand(overlay, TRUE);
        gtk_overlay_set_child(GTK_OVERLAY(overlay), scroll);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), revealer);

        gtk_box_append(GTK_BOX(box), overlay);
    }

    /*
     * The composer follows the transcript's column.
     *
     * It becomes visible the moment the transcript is clamped: a
     * full-width entry under a narrow column of text looks like a
     * rendering fault rather than a layout.  The thing you read and the
     * thing you type into should be the same column, so the same clamp
     * wraps the whole composer cluster -- the activity line, the
     * slash-command list, the staged attachments and the entry.
     */
    {
        GtkWidget *composer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        GtkWidget *clamp = adw_clamp_new();

        /*
         * And it stands on the same line as the words above it.
         *
         * The same clamp is not the same column: a row spends
         * CHAT_ROW_MARGIN plus CHAT_GUTTER of its 600 before a body
         * starts, and the composer spent only CHAT_ROW_MARGIN -- so the
         * entry's frame, the strongest vertical in the whole page, stood
         * CHAT_GUTTER left of every line of text and inside the one
         * column deliberately left empty.  Insetting by CHAT_BODY_INSET
         * puts it back under the text; CHAT_ROW_MARGIN on the trailing
         * edge is what a row already ends at, so both ends agree.
         *
         * This aligns the frame, not the text inside it.  GtkText keeps
         * an inset of its own, so the caret sits a little inside the
         * rail -- which is right, because a bordered control reads as a
         * box and the box's edge is the line the eye follows down.
         */
        gtk_widget_set_margin_start(composer, CHAT_BODY_INSET);
        gtk_widget_set_margin_end(composer, CHAT_ROW_MARGIN);

        gtk_box_append(GTK_BOX(composer), self->activity_bar);
        gtk_box_append(GTK_BOX(composer), self->command_revealer);
        gtk_box_append(GTK_BOX(composer), self->attachments);
        gtk_box_append(GTK_BOX(composer), entry_box);

        adw_clamp_set_child(ADW_CLAMP(clamp), composer);
        gtk_box_append(GTK_BOX(box), clamp);
    }

    return box;
}

static GtkWidget *
build_inspector_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->inspector = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 12));
    gtk_widget_set_margin_top(GTK_WIDGET(self->inspector), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->inspector), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->inspector), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->inspector), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->inspector));

    return scroll;
}

static GtkWidget *
build_mailbox_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->mailbox_summary = GTK_LABEL(gtk_label_new("No agent selected."));
    gtk_widget_set_margin_top(GTK_WIDGET(self->mailbox_summary), 12);

    {
        GtkWidget *purge = gtk_button_new_with_label("Purge expired items");
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

        gtk_widget_set_tooltip_text(
            purge, "Removes every item past its time-to-live from this "
                   "agent's mailbox.");
        g_signal_connect(purge, "clicked", G_CALLBACK(on_mailbox_purge),
                         self);
        gtk_widget_set_halign(row, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(row, 6);
        gtk_box_append(GTK_BOX(row), purge);
        gtk_box_append(GTK_BOX(box), row);
    }

    self->mailbox_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->mailbox_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->mailbox_list), "boxed-list");
    gtk_widget_set_margin_start(GTK_WIDGET(self->mailbox_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->mailbox_list), 12);
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->mailbox_list), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->mailbox_list));
    gtk_widget_set_vexpand(scroll, TRUE);

    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->mailbox_summary));
    gtk_box_append(GTK_BOX(box), scroll);

    return box;
}

static GtkWidget *
build_computer_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *run;
    GtkWidget *entry_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    self->computer_state = GTK_LABEL(gtk_label_new("No agent selected."));
    gtk_widget_set_margin_top(GTK_WIDGET(self->computer_state), 12);

    self->exec_output = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_editable(self->exec_output, FALSE);
    gtk_text_view_set_monospace(self->exec_output, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->exec_output));
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_widget_set_margin_start(scroll, 12);
    gtk_widget_set_margin_end(scroll, 12);

    self->exec_entry = GTK_ENTRY(gtk_entry_new());
    gtk_entry_set_placeholder_text(self->exec_entry, "Command");
    gtk_widget_set_hexpand(GTK_WIDGET(self->exec_entry), TRUE);
    g_signal_connect(self->exec_entry, "activate", G_CALLBACK(on_exec), self);

    run = gtk_button_new_with_label("Run");
    g_signal_connect(run, "clicked", G_CALLBACK(on_exec), self);

    gtk_box_append(GTK_BOX(entry_box), GTK_WIDGET(self->exec_entry));
    gtk_box_append(GTK_BOX(entry_box), run);
    gtk_widget_set_margin_start(entry_box, 12);
    gtk_widget_set_margin_end(entry_box, 12);
    gtk_widget_set_margin_bottom(entry_box, 12);

    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->computer_state));
    gtk_box_append(GTK_BOX(box), scroll);
    gtk_box_append(GTK_BOX(box), entry_box);

    return box;
}

/* ── The flow page ───────────────────────────────────────────────── */

/*
 * "4 minutes ago" rather than a timestamp.
 *
 * A conversation list is read to find the recent one, and working out
 * which of two wall-clock times is nearer to now is work the reader
 * should not be doing.
 */
static gchar *
relative_time(gint64 ts)
{
    gint64 delta = (g_get_real_time() / G_USEC_PER_SEC) - ts;

    if (ts <= 0)
        return g_strdup("");

    if (delta < 60)
        return g_strdup("just now");

    if (delta < 3600)
        return g_strdup_printf("%" G_GINT64_FORMAT "m ago", delta / 60);

    if (delta < 86400)
        return g_strdup_printf("%" G_GINT64_FORMAT "h ago", delta / 3600);

    return g_strdup_printf("%" G_GINT64_FORMAT "d ago", delta / 86400);
}

/*
 * One line of a message, trimmed, for a list subtitle.
 *
 * Cut on a character boundary rather than a byte one: an agent's reply
 * is as likely to contain an em dash as not, and half a UTF-8 sequence
 * renders as a replacement glyph for the rest of the row.
 */
static gchar *
one_line(const gchar *body, glong limit)
{
    g_autofree gchar *flat = NULL;
    const gchar *newline;

    if (body == NULL)
        return g_strdup("");

    newline = strchr(body, '\n');
    flat = (newline != NULL) ? g_strndup(body, (gsize)(newline - body))
                             : g_strdup(body);
    g_strstrip(flat);

    if (!g_utf8_validate(flat, -1, NULL))
        return g_strdup("");

    if (g_utf8_strlen(flat, -1) <= limit)
        return g_steal_pointer(&flat);

    {
        const gchar *end = g_utf8_offset_to_pointer(flat, limit);
        g_autofree gchar *cut = g_strndup(flat, (gsize)(end - flat));

        return g_strconcat(cut, "\xe2\x80\xa6", NULL);
    }
}

/*
 * "alpha and beta", from the room's member list.
 *
 * Built from the members rather than by taking the id apart, because
 * how a direct room is named is the daemon's business -- a client that
 * parses "dm:a:b" is a client that breaks when that changes.
 */
static gchar *
room_label(JsonArray *members)
{
    g_autoptr(GString) out = g_string_new(NULL);
    guint i;

    for (i = 0; members != NULL && i < json_array_get_length(members); i++) {
        const gchar *member = json_array_get_string_element(members, i);

        if (out->len > 0)
            g_string_append(out, i + 1 == json_array_get_length(members)
                                 ? " and " : ", ");

        g_string_append(out, g_strcmp0(member, "user") == 0 ? "you" : member);
    }

    return g_string_free(g_steal_pointer(&out), FALSE);
}

static gboolean
room_involves_user(JsonArray *members)
{
    guint i;

    for (i = 0; members != NULL && i < json_array_get_length(members); i++) {
        if (g_strcmp0(json_array_get_string_element(members, i),
                      "user") == 0)
            return TRUE;
    }

    return FALSE;
}

static void
on_flow_task_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)button;

    /*
     * The task board is where the rest of the story is -- who asked for
     * it, what state it is in, what it returned.
     */
    adw_view_stack_set_visible_child_name(self->pages, "tasks");
}

/*
 * Loads one conversation into the right-hand pane.
 */
static void
show_flow_room(ClawtWindow *self, const gchar *room_id, const gchar *label)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *messages;
    guint i;

    /*
     * The request first, and only then the clear.
     *
     * clawt_window_request() iterates the main context while it waits,
     * so an event arriving mid-flight re-enters this function: the inner
     * call emptied the box and filled it, the outer one carried on
     * appending from where it was, and the conversation appeared twice.
     * Emptying after the answer is back means a nested call finishes
     * completely and the outer one then replaces its work rather than
     * adding to it.
     */
    reply = clawt_window_request(
        self, "room.history",
        clawt_build_payload("room", room_id, "limit", "200", NULL));

    if (reply == NULL)
        return;

    clear_box(self->flow_transcript);

    g_free(self->flow_room);
    self->flow_room = g_strdup(room_id);

    gtk_label_set_text(GTK_LABEL(self->flow_title),
                       label != NULL ? label : room_id);

    messages = json_object_get_array_member(clawt_payload_of(reply),
                                            "messages");

    {
        g_autofree gchar *count = g_strdup_printf(
            "%u message%s", json_array_get_length(messages),
            json_array_get_length(messages) == 1 ? "" : "s");

        gtk_label_set_text(GTK_LABEL(self->flow_subtitle), count);
    }

    /*
     * Drawn through the chat's own row builder.
     *
     * This used to be a second builder, and the two had drifted into
     * visibly different renderings of the same messages: one with runs,
     * avatars, day dividers and a measure, the other a flat list of
     * captions.  A reader moving between a conversation and the flow of
     * one saw two conventions for one kind of content.
     *
     * Its avatars are derived from each sender's name rather than from
     * one agent's configured image, because a room here has several
     * participants -- which is what the NULLs in the view say.
     */
    g_clear_pointer(&self->flow_run_sender, g_free);
    g_clear_pointer(&self->flow_run_day, g_free);

    for (i = 0; i < json_array_get_length(messages); i++) {
        JsonObject *message = json_array_get_object_element(messages, i);
        const gchar *sender = clawt_json_string(message, "sender", "?");
        TranscriptView view = { self->flow_transcript,
                                &self->flow_run_sender,
                                &self->flow_run_day, NULL, NULL };

        append_message_to(self, &view, sender,
                          clawt_json_string(message, "body", ""),
                          g_strcmp0(sender, "user") == 0,
                          clawt_json_int(message, "ts", 0),
                          clawt_json_string(message, "task", NULL),
                          clawt_json_int(message, "depth", 0));
    }

    gtk_stack_set_visible_child_name(GTK_STACK(self->flow_stack), "room");
}

static void
on_flow_row_selected(GtkListBox *list, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *room_id;
    const gchar *label;

    (void)list;

    if (row == NULL)
        return;

    room_id = g_object_get_data(G_OBJECT(row), "room");
    label = g_object_get_data(G_OBJECT(row), "label");

    if (room_id != NULL)
        show_flow_room(self, room_id, label);
}

/*
 * Sorts the conversation list, most recently active first.
 */
static gint
compare_by_last(gconstpointer a, gconstpointer b)
{
    JsonObject *left = *(JsonObject **)a;
    JsonObject *right = *(JsonObject **)b;
    gint64 left_ts = clawt_json_int(left, "last_ts", 0);
    gint64 right_ts = clawt_json_int(right, "last_ts", 0);

    if (left_ts == right_ts)
        return 0;

    return (left_ts > right_ts) ? -1 : 1;
}

static void
refresh_flow_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) ordered = NULL;
    gboolean include_user = gtk_check_button_get_active(
        GTK_CHECK_BUTTON(self->flow_include_user));
    JsonArray *rooms;
    guint i;

    clear_list(self->flow_list);

    reply = clawt_window_request(self, "room.list", NULL);

    if (reply == NULL)
        return;

    rooms = json_object_get_array_member(clawt_payload_of(reply), "rooms");
    ordered = g_ptr_array_new();

    for (i = 0; i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);

        /*
         * An empty room is one the daemon made because somebody could
         * have talked, not one where anybody did. A fleet accumulates a
         * direct room per pair and listing them all buries the few that
         * matter.
         */
        if (clawt_json_int(room, "messages", 0) == 0)
            continue;

        if (!include_user &&
            room_involves_user(json_object_get_array_member(room, "members")))
            continue;

        g_ptr_array_add(ordered, room);
    }

    g_ptr_array_sort(ordered, compare_by_last);

    for (i = 0; i < ordered->len; i++) {
        JsonObject *room = g_ptr_array_index(ordered, i);
        const gchar *room_id = clawt_json_string(room, "id", "");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *count;
        g_autofree gchar *label =
            room_label(json_object_get_array_member(room, "members"));
        g_autofree gchar *snippet =
            one_line(clawt_json_string(room, "last_body", ""), 44);
        g_autofree gchar *when =
            relative_time(clawt_json_int(room, "last_ts", 0));
        g_autofree gchar *subtitle = g_strdup_printf(
            "%s \xc2\xb7 %s: %s", when,
            clawt_json_string(room, "last_sender", "?"), snippet);
        g_autofree gchar *badge = g_strdup_printf(
            "%" G_GINT64_FORMAT, clawt_json_int(room, "messages", 0));

        set_row_text(row, label, subtitle);

        count = gtk_label_new(badge);
        gtk_widget_add_css_class(count, "caption");
        gtk_widget_add_css_class(count, "dim-label");
        gtk_widget_set_valign(count, GTK_ALIGN_CENTER);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), count);

        g_object_set_data_full(G_OBJECT(row), "room", g_strdup(room_id),
                               g_free);
        g_object_set_data_full(G_OBJECT(row), "label",
                               g_strdup(label), g_free);

        gtk_list_box_append(self->flow_list, row);

        /*
         * The conversation already open stays open across a refresh,
         * which arrives on every message -- reselecting the first row
         * would drag the reader away from what they were reading every
         * time anything anywhere said something.
         */
        if (g_strcmp0(room_id, self->flow_room) == 0)
            gtk_list_box_select_row(
                self->flow_list,
                GTK_LIST_BOX_ROW(gtk_widget_get_last_child(
                    GTK_WIDGET(self->flow_list))));
    }
}

/* ── Settings: the cloud image manager ───────────────────────────── */

static gchar *
human_size(gint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024)
        return g_strdup_printf("%.1f GB", bytes / (1024.0 * 1024.0 * 1024.0));

    if (bytes >= 1024 * 1024)
        return g_strdup_printf("%.0f MB", bytes / (1024.0 * 1024.0));

    if (bytes >= 1024)
        return g_strdup_printf("%.0f kB", bytes / 1024.0);

    return g_strdup_printf("%" G_GINT64_FORMAT " B", bytes);
}

static void
image_action(ClawtWindow *self, const gchar *kind, const gchar *name)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_end_object(builder);

    reply = clawt_window_request(self, kind, json_builder_get_root(builder));

    if (reply != NULL)
        refresh_settings_images(self);
}

static void
on_image_remove(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    image_action(self, "image.vm_remove",
                 g_object_get_data(G_OBJECT(button), "clawt-image"));
}

static void
on_image_cancel(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;

    image_action(self, "image.vm_cancel",
                 g_object_get_data(G_OBJECT(button), "clawt-image"));
}

static void
start_download(ClawtWindow *self, const gchar *url, const gchar *name)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;

    if (url == NULL || *url == '\0')
        return;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "url");
    json_builder_add_string_value(builder, url);

    if (name != NULL) {
        json_builder_set_member_name(builder, "name");
        json_builder_add_string_value(builder, name);
    }

    json_builder_end_object(builder);

    reply = clawt_window_request(self, "image.vm_download",
                                 json_builder_get_root(builder));

    if (reply != NULL)
        refresh_settings_images(self);
}

static void
on_download_from_catalog(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    JsonArray *sources;
    guint selected;

    (void)button;

    if (self->settings_catalog == NULL)
        return;

    sources = json_object_get_array_member(
        json_node_get_object(self->settings_catalog), "sources");
    selected = adw_combo_row_get_selected(
        ADW_COMBO_ROW(self->settings_catalog_row));

    if (selected >= json_array_get_length(sources))
        return;

    /*
     * The catalog id is sent rather than the URL behind it, so the daemon
     * resolves the newest compose itself.  Sending a URL resolved here
     * would pin whatever this client happened to see.
     */
    start_download(self,
                   clawt_json_string(
                       json_array_get_object_element(sources, selected),
                       "id", NULL), NULL);
}

static void
on_download_from_url(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *url = gtk_editable_get_text(
        GTK_EDITABLE(self->settings_url_row));

    (void)button;

    start_download(self, url, NULL);
    gtk_editable_set_text(GTK_EDITABLE(self->settings_url_row), "");
}

/*
 * Rebuilds the list of images.
 *
 * Called when something changes it, never on progress: the bars are moved
 * in place instead, because a hundred rebuilds during one download would
 * take the scroll position and the focus with them each time.
 */
static void refresh_settings_images_once(ClawtWindow *self);

static void
refresh_settings_images(ClawtWindow *self)
{
    if (self->settings_images == NULL)
        return;

    /*
     * Guarded like every other list here: clawt_window_request() iterates
     * the main context while it waits, and events arrive from an idle, so
     * a download event lands in the middle of a rebuild and starts
     * another one.
     */
    if (!refresh_enter(self, CLAWT_REFRESH_IMAGES))
        return;

    do
        refresh_settings_images_once(self);
    while (refresh_repeat(self, CLAWT_REFRESH_IMAGES));
}

static void
refresh_settings_images_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *images;
    guint i;

    /*
     * clear_list(), not a loop unparenting children.  GtkListBox wraps an
     * appended widget in a row of its own and keeps its own record of
     * them, so unparenting behind its back leaves a list that accepts
     * appends and draws none of them -- an empty box where the images
     * were, which is exactly what it did.
     */
    clear_list(GTK_LIST_BOX(self->settings_images));

    g_hash_table_remove_all(self->settings_bars);

    reply = clawt_window_request(self, "image.vm_list", NULL);

    if (reply == NULL)
        return;

    images = json_object_get_array_member(json_node_get_object(reply),
                                          "images");

    if (json_array_get_length(images) == 0) {
        GtkWidget *empty = adw_action_row_new();

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(empty),
                                      "No images yet");
        adw_action_row_set_subtitle(
            ADW_ACTION_ROW(empty),
            "A VM needs one. Fedora is the safe pick; anything with "
            "cloud-init works.");
        gtk_list_box_append(GTK_LIST_BOX(self->settings_images), empty);
        return;
    }

    for (i = 0; i < json_array_get_length(images); i++) {
        JsonObject *image = json_array_get_object_element(images, i);
        const gchar *name = clawt_json_string(image, "name", "?");
        gboolean downloading =
            json_object_has_member(image, "downloading") &&
            json_object_get_boolean_member(image, "downloading");
        gint64 bytes = json_object_get_int_member(image, "bytes");
        gint64 total = json_object_get_int_member(image, "total");
        GtkWidget *row = adw_action_row_new();
        GtkWidget *button;

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);

        if (downloading) {
            GtkWidget *bar = gtk_progress_bar_new();
            g_autofree gchar *done_text = human_size(bytes);
            g_autofree gchar *total_text = human_size(total);
            g_autofree gchar *subtitle =
                g_strdup_printf("%s of %s", done_text, total_text);

            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            gtk_widget_set_valign(bar, GTK_ALIGN_CENTER);
            gtk_widget_set_size_request(bar, 160, -1);

            if (total > 0)
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar),
                                              (gdouble)bytes / total);

            adw_action_row_add_suffix(ADW_ACTION_ROW(row), bar);
            g_hash_table_insert(self->settings_bars, g_strdup(name),
                                g_object_ref_sink(bar));

            button = gtk_button_new_from_icon_name("process-stop-symbolic");
            gtk_widget_set_tooltip_text(button, "Stop this download");
            g_signal_connect(button, "clicked", G_CALLBACK(on_image_cancel),
                             self);
        } else {
            g_autofree gchar *size = human_size(bytes);
            const gchar *url = clawt_json_string(image, "url", NULL);
            g_autofree gchar *subtitle =
                url != NULL ? g_strdup_printf("%s  \342\200\224  %s", size, url)
                            : g_strdup(size);

            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            button = gtk_button_new_from_icon_name("user-trash-symbolic");
            gtk_widget_set_tooltip_text(button, "Delete this image");
            g_signal_connect(button, "clicked", G_CALLBACK(on_image_remove),
                             self);
        }

        gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
        gtk_widget_add_css_class(button, "flat");
        g_object_set_data_full(G_OBJECT(button), "clawt-image",
                               g_strdup(name), g_free);
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);

        gtk_list_box_append(GTK_LIST_BOX(self->settings_images), row);
    }
}

static void
on_settings_closed(AdwDialog *dialog, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)dialog;

    /*
     * Forgotten together.  A bar held past its window is a finalized
     * widget the next progress event would write to.
     */
    self->settings = NULL;
    self->settings_images = NULL;
    self->settings_integrations = NULL;
    self->settings_teams = NULL;
    self->settings_spending = NULL;
    self->settings_spending_period = NULL;
    self->settings_connectors = NULL;
    self->settings_catalog_row = NULL;
    self->settings_url_row = NULL;
    g_clear_pointer(&self->settings_catalog, json_node_unref);
    g_hash_table_remove_all(self->settings_bars);
}

/* ── The Appearance page ─────────────────────────────────────────── */

/*
 * Saved on every change rather than behind an Apply button.
 *
 * The page is a live preview -- the font changes under the dialog as you
 * pick it -- and a preview you can see but which is not yet saved is the
 * worst of both: it looks applied, and closing the window loses it.
 */
static void
appearance_changed(ClawtWindow *self)
{
    g_autoptr(GError) error = NULL;

    apply_appearance(self->appearance);

    if (!clawt_appearance_save(self->appearance, NULL, &error))
        clawt_window_toast(self, error->message);
}

static void
on_theme_selected(GObject *row, GParamSpec *spec, gpointer user_data)
{
    ClawtWindow *self = user_data;
    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(row));

    (void)spec;

    /*
     * An index into the library's own list rather than into a copy of
     * it. The copy here named four schemes while the web client's named
     * three, so the palette added to clawt-appearance.c reached one
     * client and not the other -- and nothing said so, because a colour
     * scheme sends no IPC frame and is no slash command.
     */
    clawt_appearance_set_theme(
        self->appearance,
        clawt_appearance_theme_nth(
            MIN(selected, clawt_appearance_theme_count() - 1)));
    appearance_changed(self);
}

static void
on_font_size_changed(GtkSpinButton *spin, gpointer user_data)
{
    ClawtWindow *self = user_data;
    gboolean monospace =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(spin), "monospace"));
    gdouble points = gtk_spin_button_get_value(spin);

    if (monospace)
        clawt_appearance_set_monospace_size(self->appearance, points);
    else
        clawt_appearance_set_font_size(self->appearance, points);

    appearance_changed(self);
}

/*
 * The label under a font row: the chosen family, or what the desktop is
 * using when nothing is chosen.
 *
 * Naming the system font rather than saying "Default" matters, because
 * the two states look identical on screen and only one of them keeps
 * following the desktop when it changes.
 */
static void
update_font_row(ClawtWindow *self, GtkWidget *row, gboolean monospace)
{
    const gchar *chosen =
        monospace ? clawt_appearance_get_monospace_font(self->appearance)
                  : clawt_appearance_get_font(self->appearance);

    if (chosen != NULL) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), chosen);
        return;
    }

    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(row),
        monospace ? "Whatever the desktop uses for monospace"
                  : "Whatever the desktop uses");
}

static void
on_font_chosen(GObject *source, GAsyncResult *result, gpointer user_data)
{
    GtkWidget *row = user_data;
    ClawtWindow *self = g_object_get_data(G_OBJECT(row), "window");
    gboolean monospace =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "monospace"));
    g_autoptr(PangoFontDescription) description = NULL;
    g_autoptr(GError) error = NULL;

    description = gtk_font_dialog_choose_font_finish(
        GTK_FONT_DIALOG(source), result, &error);

    /* Dismissing the chooser is not a failure worth a toast. */
    if (description == NULL)
        return;

    if (monospace)
        clawt_appearance_set_monospace_font(
            self->appearance, pango_font_description_get_family(description));
    else
        clawt_appearance_set_font(
            self->appearance, pango_font_description_get_family(description));

    /*
     * The size comes with the family from a font chooser, and ignoring
     * it would mean picking "Cantarell 14" and getting Cantarell at
     * whatever size was already set -- which reads as the size control
     * being broken.
     */
    if (pango_font_description_get_size(description) > 0) {
        gdouble points =
            (gdouble)pango_font_description_get_size(description) / PANGO_SCALE;
        GtkWidget *spin = g_object_get_data(G_OBJECT(row), "spin");

        if (monospace)
            clawt_appearance_set_monospace_size(self->appearance, points);
        else
            clawt_appearance_set_font_size(self->appearance, points);

        /*
         * Set on the widget too, so the spin button agrees with what was
         * just chosen.  ::value-changed then fires and saves, which is
         * why this happens before the explicit save below rather than
         * after it.
         */
        if (spin != NULL)
            gtk_spin_button_set_value(
                GTK_SPIN_BUTTON(spin),
                monospace
                    ? clawt_appearance_get_monospace_size(self->appearance)
                    : clawt_appearance_get_font_size(self->appearance));
    }

    update_font_row(self, row, monospace);
    appearance_changed(self);
}

static void
on_choose_font(GtkButton *button, gpointer user_data)
{
    GtkWidget *row = user_data;
    ClawtWindow *self = g_object_get_data(G_OBJECT(row), "window");
    GtkFontDialog *chooser = gtk_font_dialog_new();
    g_autoptr(PangoFontDescription) current = NULL;
    gboolean monospace =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "monospace"));
    const gchar *family =
        monospace ? clawt_appearance_get_monospace_font(self->appearance)
                  : clawt_appearance_get_font(self->appearance);

    (void)button;

    gtk_font_dialog_set_title(chooser, monospace ? "Code font"
                                                 : "Interface font");

    if (family != NULL)
        current = pango_font_description_from_string(family);

    gtk_font_dialog_choose_font(chooser, GTK_WINDOW(self), current, NULL,
                                on_font_chosen, row);
    g_object_unref(chooser);
}

/*
 * Back to the desktop's own font.
 *
 * Worth its own button: clearing a font chooser is not something a font
 * chooser offers, so without this a person who tried a font could never
 * get back to following their desktop -- only to naming whatever it
 * happens to use today, which is a different and worse thing.
 */
static void
on_clear_font(GtkButton *button, gpointer user_data)
{
    GtkWidget *row = user_data;
    ClawtWindow *self = g_object_get_data(G_OBJECT(row), "window");
    gboolean monospace =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "monospace"));
    GtkWidget *spin = g_object_get_data(G_OBJECT(row), "spin");

    (void)button;

    if (monospace) {
        clawt_appearance_set_monospace_font(self->appearance, NULL);
        clawt_appearance_set_monospace_size(self->appearance, 0);
    } else {
        clawt_appearance_set_font(self->appearance, NULL);
        clawt_appearance_set_font_size(self->appearance, 0);
    }

    if (spin != NULL)
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), 0);

    update_font_row(self, row, monospace);
    appearance_changed(self);
}

/*
 * One font row: a family with a chooser, and a size beside it.
 *
 * The size is a separate control from the chooser's own because 0 has to
 * be reachable, and 0 means "the desktop's size" -- which no font chooser
 * has a way to express.
 */
static GtkWidget *
build_font_group(ClawtWindow *self, const gchar *title,
                 const gchar *description, gboolean monospace)
{
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *row = adw_action_row_new();
    GtkWidget *size_row = adw_action_row_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *choose = gtk_button_new_with_label("Choose\342\200\246");
    GtkWidget *clear = gtk_button_new_from_icon_name("edit-clear-symbolic");
    /*
     * Half-point steps, one decimal shown.
     *
     * Whole points looked tidier and could not express a size people
     * actually run: Emacs states a pixel size, and 18px lands on 13.6pt
     * -- which a whole-number control silently rounds to 14, so the file
     * and the dialog disagree about what is set.
     */
    GtkWidget *spin = gtk_spin_button_new_with_range(0, 48, 0.5);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
    adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group),
                                          description);

    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Font");

    g_object_set_data(G_OBJECT(row), "window", self);
    g_object_set_data(G_OBJECT(row), "monospace",
                      GINT_TO_POINTER(monospace));
    g_object_set_data(G_OBJECT(row), "spin", spin);

    gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(clear, "flat");
    gtk_widget_set_tooltip_text(clear, "Follow the desktop again");

    g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_font), row);
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_font), row);

    gtk_box_append(GTK_BOX(buttons), choose);
    gtk_box_append(GTK_BOX(buttons), clear);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), buttons);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(size_row), "Size");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(size_row),
                                "0 follows the desktop");

    gtk_widget_set_valign(spin, GTK_ALIGN_CENTER);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(spin), 1);
    gtk_spin_button_set_value(
        GTK_SPIN_BUTTON(spin),
        monospace ? clawt_appearance_get_monospace_size(self->appearance)
                  : clawt_appearance_get_font_size(self->appearance));
    g_object_set_data(G_OBJECT(spin), "monospace",
                      GINT_TO_POINTER(monospace));
    g_signal_connect(spin, "value-changed",
                     G_CALLBACK(on_font_size_changed), self);

    adw_action_row_add_suffix(ADW_ACTION_ROW(size_row), spin);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), size_row);

    update_font_row(self, row, monospace);

    return group;
}

static GtkWidget *
build_appearance_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *theme_group = adw_preferences_group_new();
    GtkWidget *theme_row = adw_combo_row_new();
    g_autoptr(GtkStringList) theme_names = gtk_string_list_new(NULL);
    guint selected = 0;
    guint t;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Appearance");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "applications-graphics-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(theme_group),
                                    "Theme");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(theme_group),
        "Kept on this machine rather than in clawtilla.yaml. The client "
        "can switch between daemons while it runs, and fonts that came "
        "from a daemon's config would change when you connected to "
        "another one.");

    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(theme_row),
                                  "Colour scheme");
    /*
     * Built from the library's list, so a palette added there appears
     * here without this file being touched -- and cannot appear here and
     * not in the web client, which builds its own select the same way.
     */
    for (t = 0; t < clawt_appearance_theme_count(); t++) {
        ClawtTheme theme = clawt_appearance_theme_nth(t);

        gtk_string_list_append(theme_names,
                               clawt_appearance_theme_label(theme));

        if (theme == clawt_appearance_get_theme(self->appearance))
            selected = t;
    }

    adw_combo_row_set_model(ADW_COMBO_ROW(theme_row),
                            G_LIST_MODEL(g_object_ref(theme_names)));

    adw_combo_row_set_selected(ADW_COMBO_ROW(theme_row), selected);

    /*
     * Connected after the initial selection is set, or setting it would
     * fire the handler and save the file on every open.
     */
    g_signal_connect(theme_row, "notify::selected",
                     G_CALLBACK(on_theme_selected), self);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(theme_group), theme_row);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(theme_group));

    adw_preferences_page_add(
        ADW_PREFERENCES_PAGE(page),
        ADW_PREFERENCES_GROUP(build_font_group(
            self, "Interface",
            "Everything but code: the agent list, messages, dialogs.",
            FALSE)));

    adw_preferences_page_add(
        ADW_PREFERENCES_PAGE(page),
        ADW_PREFERENCES_GROUP(build_font_group(
            self, "Code",
            "Code blocks and inline code in a conversation, and the "
            "output of the exec console.",
            TRUE)));

    return page;
}

/* ── Integrations ────────────────────────────────────────────────── */

/*
 * One integration being edited.
 *
 * The widgets are held rather than looked up because which of them exist
 * depends on the type: a Matrix instance has a homeserver and a room
 * picker, an MCP one has a command line, and building both and hiding
 * half is how a dialog ends up saving a field nobody could see.
 */
typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    gchar       *name;
    gchar       *type_id;

    GtkWidget   *enabled_row;
    GtkWidget   *description_row;
    GtkWidget   *scope_row;
    GtkWidget   *agents_group;
    GPtrArray   *agent_rows;     /* AdwSwitchRow*, unowned */

    GtkWidget   *homeserver_row;
    GtkWidget   *user_row;
    GtkWidget   *rooms_row;
    GtkWidget   *mention_row;

    GtkWidget   *imap_host_row;
    GtkWidget   *imap_port_row;
    GtkWidget   *smtp_host_row;
    GtkWidget   *smtp_port_row;
    GtkWidget   *username_row;
    GtkWidget   *secret_row;

    GtkWidget   *port_row;
    GtkWidget   *command_row;
    GtkWidget   *args_row;
    GtkWidget   *url_row;

    GtkWidget   *backend_row;
    GtkWidget   *quiet_row;
    GtkWidget   *notify_title_row;
    GtkWidget   *priority_row;
    GPtrArray   *event_rows;     /* AdwSwitchRow*, unowned */

    GStrv        rooms;          /* what the picker last agreed on */
} IntegrationDialog;

static void refresh_settings_integrations(ClawtWindow *self);
static void open_integration_editor(ClawtWindow *self, const gchar *name,
                                    const gchar *type_id);

static void
integration_dialog_free(gpointer data)
{
    IntegrationDialog *dialog = data;

    g_free(dialog->name);
    g_free(dialog->type_id);
    g_strfreev(dialog->rooms);
    g_clear_pointer(&dialog->agent_rows, g_ptr_array_unref);
    g_clear_pointer(&dialog->event_rows, g_ptr_array_unref);
    g_free(dialog);
}

/*
 * The instance as the daemon currently has it.
 *
 * Refetched rather than cached on the dialog, because signing in changes
 * it behind the dialog's back: the daemon writes the user id and the
 * token reference itself, and a dialog showing what it had before would
 * save the old values back over them.
 */
static JsonObject *
find_integration(JsonNode *reply, const gchar *name)
{
    JsonArray *integrations;
    guint i;

    if (reply == NULL)
        return NULL;

    integrations = json_object_get_array_member(json_node_get_object(reply),
                                                "integrations");

    for (i = 0; i < json_array_get_length(integrations); i++) {
        JsonObject *integration = json_array_get_object_element(integrations,
                                                                i);

        if (g_strcmp0(clawt_json_string(integration, "name", ""), name) == 0)
            return integration;
    }

    return NULL;
}

static gchar *
join_strings(JsonObject *object, const gchar *member, const gchar *separator)
{
    GString *out = g_string_new(NULL);
    JsonArray *array;
    guint i;

    if (object == NULL || !json_object_has_member(object, member))
        return g_string_free(out, FALSE);

    array = json_object_get_array_member(object, member);

    for (i = 0; i < json_array_get_length(array); i++) {
        if (i > 0)
            g_string_append(out, separator);

        g_string_append(out, json_array_get_string_element(array, i));
    }

    return g_string_free(out, FALSE);
}

/*
 * Adds a comma-separated entry as a JSON array.
 *
 * The separator is a comma because these are ids and room addresses,
 * which never contain one, and a person editing three rooms in a text
 * field should not have to think about quoting.
 */
static void
add_list_member(JsonBuilder *builder, const gchar *member, const gchar *text)
{
    g_auto(GStrv) parts = NULL;
    guint i;

    json_builder_set_member_name(builder, member);
    json_builder_begin_array(builder);

    if (text != NULL && *text != '\0') {
        parts = g_strsplit(text, ",", -1);

        for (i = 0; parts[i] != NULL; i++) {
            g_strstrip(parts[i]);

            if (*parts[i] != '\0')
                json_builder_add_string_value(builder, parts[i]);
        }
    }

    json_builder_end_array(builder);
}

static void
add_string_member(JsonBuilder *builder, const gchar *member, GtkWidget *row)
{
    if (row == NULL)
        return;

    json_builder_set_member_name(builder, member);
    json_builder_add_string_value(builder,
                                  gtk_editable_get_text(GTK_EDITABLE(row)));
}

static void
add_int_member(JsonBuilder *builder, const gchar *member, GtkWidget *row)
{
    const gchar *text;

    if (row == NULL)
        return;

    text = gtk_editable_get_text(GTK_EDITABLE(row));

    json_builder_set_member_name(builder, member);
    json_builder_add_int_value(builder, g_ascii_strtoll(text, NULL, 10));
}

static void
on_integration_saved(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *dialog = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    guint i;

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, dialog->name);

    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(ADW_SWITCH_ROW(dialog->enabled_row)));

    add_string_member(builder, "description", dialog->description_row);

    {
        static const gchar *const scopes[] = { "all", "selected", "none" };
        guint selected = adw_combo_row_get_selected(
            ADW_COMBO_ROW(dialog->scope_row));

        json_builder_set_member_name(builder, "scope");
        json_builder_add_string_value(builder, scopes[MIN(selected, 2)]);
    }

    /*
     * The ticked agents go up whatever the scope is, so switching to
     * `all` and back does not lose the selection somebody made.
     */
    json_builder_set_member_name(builder, "agents");
    json_builder_begin_array(builder);

    for (i = 0; i < dialog->agent_rows->len; i++) {
        GtkWidget *row = g_ptr_array_index(dialog->agent_rows, i);

        if (adw_switch_row_get_active(ADW_SWITCH_ROW(row)))
            json_builder_add_string_value(
                builder, g_object_get_data(G_OBJECT(row), "agent"));
    }

    json_builder_end_array(builder);

    if (g_strcmp0(dialog->type_id, "matrix") == 0) {
        add_string_member(builder, "homeserver", dialog->homeserver_row);
        add_string_member(builder, "user_id", dialog->user_row);
        add_list_member(builder, "rooms",
                        gtk_editable_get_text(
                            GTK_EDITABLE(dialog->rooms_row)));
        json_builder_set_member_name(builder, "require_mention");
        json_builder_add_boolean_value(
            builder,
            adw_switch_row_get_active(ADW_SWITCH_ROW(dialog->mention_row)));
    } else if (g_strcmp0(dialog->type_id, "email") == 0) {
        add_string_member(builder, "imap_host", dialog->imap_host_row);
        add_int_member(builder, "imap_port", dialog->imap_port_row);
        add_string_member(builder, "smtp_host", dialog->smtp_host_row);
        add_int_member(builder, "smtp_port", dialog->smtp_port_row);
        add_string_member(builder, "username", dialog->username_row);
    } else if (g_strcmp0(dialog->type_id, "webhook") == 0) {
        add_int_member(builder, "port", dialog->port_row);
    } else if (g_strcmp0(dialog->type_id, "mcp") == 0) {
        add_string_member(builder, "command", dialog->command_row);
        add_list_member(builder, "args",
                        gtk_editable_get_text(GTK_EDITABLE(dialog->args_row)));
        add_string_member(builder, "url", dialog->url_row);
    } else if (g_strcmp0(dialog->type_id, "notify") == 0) {
        static const gchar *const backend_ids[] = {
            "desktop", "ntfy", "gotify", "matrix", "command"
        };
        static const gchar *const priorities[] = {
            "low", "normal", "high", "urgent"
        };
        guint backend = adw_combo_row_get_selected(
            ADW_COMBO_ROW(dialog->backend_row));
        guint priority = adw_combo_row_get_selected(
            ADW_COMBO_ROW(dialog->priority_row));

        json_builder_set_member_name(builder, "backend");
        json_builder_add_string_value(builder, backend_ids[MIN(backend, 4)]);
        json_builder_set_member_name(builder, "priority");
        json_builder_add_string_value(builder, priorities[MIN(priority, 3)]);

        add_string_member(builder, "url", dialog->url_row);
        add_string_member(builder, "homeserver", dialog->homeserver_row);
        add_string_member(builder, "room", dialog->rooms_row);
        add_string_member(builder, "command", dialog->command_row);
        add_list_member(builder, "args",
                        gtk_editable_get_text(GTK_EDITABLE(dialog->args_row)));
        add_string_member(builder, "title", dialog->notify_title_row);
        add_string_member(builder, "quiet_hours", dialog->quiet_row);

        json_builder_set_member_name(builder, "events");
        json_builder_begin_array(builder);

        for (i = 0; i < dialog->event_rows->len; i++) {
            GtkWidget *row = g_ptr_array_index(dialog->event_rows, i);

            if (adw_switch_row_get_active(ADW_SWITCH_ROW(row)))
                json_builder_add_string_value(
                    builder, g_object_get_data(G_OBJECT(row), "event"));
        }

        json_builder_end_array(builder);
    }

    /*
     * A secret is sent as a reference -- `env:NAME`, `file:PATH` --
     * because there is no way to put a secret's value into clawtilla.yaml
     * and this dialog is not going to be the first.  Matrix has its own
     * sign-in button instead, which is the only place a password is ever
     * typed.
     */
    if (dialog->secret_row != NULL) {
        const gchar *text =
            gtk_editable_get_text(GTK_EDITABLE(dialog->secret_row));

        if (text != NULL && *text != '\0') {
            g_auto(GStrv) parts = g_strsplit(text, ":", 2);

            if (parts[1] == NULL) {
                clawt_window_toast(dialog->window,
                                   "A secret is env:NAME, file:PATH or "
                                   "command:...");
                return;
            }

            json_builder_set_member_name(builder, "secret_key");
            json_builder_add_string_value(
                builder,
                g_strcmp0(dialog->type_id, "email") == 0 ? "password"
                    : (g_strcmp0(dialog->type_id, "notify") == 0 ? "token"
                                                                 : "access_token"));
            json_builder_set_member_name(builder, "secret_backend");
            json_builder_add_string_value(builder, parts[0]);
            json_builder_set_member_name(builder, "secret_locator");
            json_builder_add_string_value(builder, parts[1]);
        }
    }

    json_builder_end_object(builder);

    reply = clawt_window_request(dialog->window, "integration.update",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    refresh_settings_integrations(dialog->window);
    refresh_selected(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_integration_removed(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, dialog->name);
    json_builder_end_object(builder);

    reply = clawt_window_request(dialog->window, "integration.remove",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window,
                       "Removed. Any credential file it wrote is still on "
                       "disk.");
    refresh_settings_integrations(dialog->window);
    refresh_selected(dialog->window);
    adw_dialog_close(dialog->dialog);
}

/*
 * Checks one integration against the first agent that has it.
 *
 * A health check belongs to a binding rather than to an instance -- an
 * instance shared by four agents may be reachable for one of them and
 * not another, if they differ in per_agent -- so it needs an agent to
 * check as, and the first in scope is the one that will notice first.
 */
static void
on_integration_checked(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *dialog = user_data;
    g_autoptr(JsonNode) list = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    JsonObject *integration;
    JsonArray *effective;
    JsonArray *checks;
    guint i;

    (void)button;

    /*
     * A notifier has nothing to connect to and check: the only honest
     * test is to send one and see whether it arrives.  It ignores the
     * event list and the quiet hours, because a button that did nothing
     * at half past eleven would be indistinguishable from a broken one.
     */
    if (g_strcmp0(dialog->type_id, "notify") == 0) {
        reply = clawt_window_request(
            dialog->window, "integration.notify_test",
            clawt_build_payload("integration", dialog->name, NULL));

        if (reply != NULL)
            clawt_window_toast(dialog->window,
                               "Sent. If nothing arrived, it is not "
                               "reaching you.");

        return;
    }

    list = clawt_window_request(dialog->window, "integration.list", NULL);
    integration = find_integration(list, dialog->name);

    if (integration == NULL)
        return;

    effective = json_object_get_array_member(integration,
                                             "effective_agents");

    if (json_array_get_length(effective) == 0) {
        clawt_window_toast(dialog->window,
                           "No agent has this yet, so there is nothing to "
                           "check it as.");
        return;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(builder,
                                  json_array_get_string_element(effective, 0));
    json_builder_set_member_name(builder, "integration");
    json_builder_add_string_value(builder, dialog->name);
    json_builder_end_object(builder);

    reply = clawt_window_request(dialog->window, "integration.health",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    checks = json_object_get_array_member(json_node_get_object(reply),
                                          "checks");

    for (i = 0; i < json_array_get_length(checks); i++) {
        JsonObject *check = json_array_get_object_element(checks, i);

        if (json_object_get_boolean_member(check, "ok"))
            clawt_window_toast(dialog->window, "It answered.");
        else
            clawt_window_toast(dialog->window,
                               clawt_json_string(check, "error",
                                                 "it did not answer"));
    }
}

/* ── Matrix sign-in ──────────────────────────────────────────────── */

typedef struct {
    IntegrationDialog *editor;
    AdwDialog         *dialog;
    GtkWidget         *user_row;
    GtkWidget         *password_row;
} MatrixSignIn;

static void
on_matrix_signed_in(GtkButton *button, gpointer user_data)
{
    MatrixSignIn *sign_in = user_data;
    IntegrationDialog *editor = sign_in->editor;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *password =
        gtk_editable_get_text(GTK_EDITABLE(sign_in->password_row));

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "integration");
    json_builder_add_string_value(builder, editor->name);
    json_builder_set_member_name(builder, "homeserver");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(editor->homeserver_row)));
    json_builder_set_member_name(builder, "user");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(sign_in->user_row)));
    json_builder_set_member_name(builder, "password");
    json_builder_add_string_value(builder, password);
    json_builder_end_object(builder);

    reply = clawt_window_request(editor->window, "integration.matrix_login",
                                 json_builder_get_root(builder));

    /*
     * Cleared whatever happened.  A wrong password leaves the dialog open
     * to be retyped, and leaving the old one in the box means the next
     * attempt sends it again by accident.
     */
    gtk_editable_set_text(GTK_EDITABLE(sign_in->password_row), "");

    if (reply == NULL)
        return;

    {
        JsonObject *root = json_node_get_object(reply);

        gtk_editable_set_text(GTK_EDITABLE(editor->user_row),
                              clawt_json_string(root, "user_id", ""));
    }

    clawt_window_toast(editor->window,
                       "Signed in. The token is on the daemon's disk, not "
                       "here.");
    adw_dialog_close(sign_in->dialog);
}

static void
on_matrix_sign_in(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *editor = user_data;
    MatrixSignIn *sign_in = g_new0(MatrixSignIn, 1);
    AdwDialog *dialog = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *sign_in_button;

    (void)button;

    sign_in->editor = editor;
    sign_in->dialog = dialog;

    adw_dialog_set_title(dialog, "Sign in to Matrix");
    adw_dialog_set_content_width(dialog, 460);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Matrix account");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The password is used once, by the daemon, and is never stored. "
        "What comes back is an access token, written to a file only the "
        "daemon can read. It appears on your account's device list as "
        "\"clawtilla\", which is where you revoke it.");

    sign_in->user_row = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(sign_in->user_row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sign_in->user_row),
                                  "User");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              sign_in->user_row);

    sign_in->password_row = adw_password_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(sign_in->password_row),
                                  "Password");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              sign_in->password_row);

    sign_in_button = gtk_button_new_with_label("Sign in");
    gtk_widget_add_css_class(sign_in_button, "suggested-action");
    gtk_widget_set_margin_top(sign_in_button, 12);
    g_signal_connect(sign_in_button, "clicked",
                     G_CALLBACK(on_matrix_signed_in), sign_in);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), sign_in_button);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(dialog, toolbar);

    g_object_set_data_full(G_OBJECT(dialog), "sign-in", sign_in, g_free);
    adw_dialog_present(dialog, GTK_WIDGET(editor->window));
}

/* ── Choosing rooms ──────────────────────────────────────────────── */

static void
on_rooms_chosen(GtkButton *button, gpointer user_data)
{
    GPtrArray *rows = g_object_get_data(G_OBJECT(button), "rows");
    IntegrationDialog *editor = user_data;
    GString *chosen = g_string_new(NULL);
    g_autofree gchar *text = NULL;
    guint i;

    for (i = 0; i < rows->len; i++) {
        GtkWidget *row = g_ptr_array_index(rows, i);

        if (!adw_switch_row_get_active(ADW_SWITCH_ROW(row)))
            continue;

        if (chosen->len > 0)
            g_string_append(chosen, ", ");

        g_string_append(chosen, g_object_get_data(G_OBJECT(row), "room"));
    }

    text = g_string_free(chosen, FALSE);
    gtk_editable_set_text(GTK_EDITABLE(editor->rooms_row), text);

    adw_dialog_close(ADW_DIALOG(g_object_get_data(G_OBJECT(button),
                                                  "dialog")));
}

static void
on_choose_rooms(GtkButton *button, gpointer user_data)
{
    IntegrationDialog *editor = user_data;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_auto(GStrv) current = NULL;
    AdwDialog *dialog;
    GtkWidget *page;
    GtkWidget *group;
    GtkWidget *toolbar;
    GtkWidget *done;
    GPtrArray *rows;
    JsonArray *rooms;
    guint i;

    (void)button;

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "integration");
    json_builder_add_string_value(builder, editor->name);
    json_builder_end_object(builder);

    reply = clawt_window_request(editor->window, "integration.matrix_rooms",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    rooms = json_object_get_array_member(json_node_get_object(reply),
                                         "rooms");

    dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, "Rooms");
    adw_dialog_set_content_width(dialog, 520);
    adw_dialog_set_content_height(dialog, 560);

    page = adw_preferences_page_new();
    group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Listen in");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Choose none to listen in every room this account is in, including "
        "ones it is invited to later.");

    current = g_strsplit(gtk_editable_get_text(GTK_EDITABLE(editor->rooms_row)),
                         ",", -1);
    rows = g_ptr_array_new();

    for (i = 0; i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        const gchar *id = clawt_json_string(room, "id", "");
        GtkWidget *row = adw_switch_row_new();
        guint k;

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      clawt_json_string(room, "label", id));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), id);

        for (k = 0; current[k] != NULL; k++) {
            if (g_strcmp0(g_strstrip(current[k]), id) == 0)
                adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
        }

        g_object_set_data_full(G_OBJECT(row), "room", g_strdup(id), g_free);
        g_ptr_array_add(rows, row);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }

    if (json_array_get_length(rooms) == 0)
        adw_preferences_group_set_description(
            ADW_PREFERENCES_GROUP(group),
            "That account is not in any rooms yet. Invite it from your "
            "Matrix client and look again.");

    done = gtk_button_new_with_label("Use these");
    gtk_widget_add_css_class(done, "suggested-action");
    gtk_widget_set_margin_top(done, 12);
    g_object_set_data_full(G_OBJECT(done), "rows", rows,
                           (GDestroyNotify)g_ptr_array_unref);
    g_object_set_data(G_OBJECT(done), "dialog", dialog);
    g_signal_connect(done, "clicked", G_CALLBACK(on_rooms_chosen), editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), done);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(dialog, toolbar);

    adw_dialog_present(dialog, GTK_WIDGET(editor->window));
}

/* ── The editor ──────────────────────────────────────────────────── */

static GtkWidget *
add_entry(GtkWidget *group, const gchar *title, const gchar *value)
{
    GtkWidget *row = adw_entry_row_new();

    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);

    if (value != NULL)
        gtk_editable_set_text(GTK_EDITABLE(row), value);

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);

    return row;
}

static GtkWidget *
add_int_entry(GtkWidget *group, const gchar *title, JsonObject *object,
              const gchar *member)
{
    g_autofree gchar *text = NULL;

    if (object != NULL && json_object_has_member(object, member))
        text = g_strdup_printf("%" G_GINT64_FORMAT,
                               json_object_get_int_member(object, member));

    return add_entry(group, title, text);
}

/*
 * The agent list, one switch each.
 *
 * Switches rather than a multi-select list because the question is per
 * agent -- "does the researcher get this" -- and a selection model makes
 * that a drag gesture with a hidden state instead of a yes or a no.
 */
static void
build_agent_group(IntegrationDialog *dialog, JsonObject *integration)
{
    g_autoptr(JsonNode) agents = NULL;
    g_autofree gchar *chosen = NULL;
    JsonArray *array;
    guint i;

    dialog->agents_group = adw_preferences_group_new();
    adw_preferences_group_set_title(
        ADW_PREFERENCES_GROUP(dialog->agents_group), "Which agents");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(dialog->agents_group),
        "Used when the scope above is \"These agents\". Kept either way, so "
        "switching to everyone and back does not lose the choice.");

    dialog->agent_rows = g_ptr_array_new();
    chosen = join_strings(integration, "agents", ",");
    agents = clawt_window_request(dialog->window, "agent.list", NULL);

    if (agents == NULL)
        return;

    array = json_object_get_array_member(json_node_get_object(agents),
                                         "agents");

    for (i = 0; i < json_array_get_length(array); i++) {
        JsonObject *agent = json_array_get_object_element(array, i);
        const gchar *id = clawt_json_string(agent, "id", "");
        GtkWidget *row = adw_switch_row_new();
        g_auto(GStrv) parts = g_strsplit(chosen, ",", -1);
        guint k;

        adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      clawt_json_string(agent, "name", id));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), id);

        for (k = 0; parts[k] != NULL; k++) {
            if (g_strcmp0(g_strstrip(parts[k]), id) == 0)
                adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
        }

        g_object_set_data_full(G_OBJECT(row), "agent", g_strdup(id), g_free);
        g_ptr_array_add(dialog->agent_rows, row);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(dialog->agents_group),
                                  row);
    }
}

static void
build_matrix_rows(IntegrationDialog *dialog, GtkWidget *group,
                  JsonObject *integration)
{
    GtkWidget *sign_in;
    GtkWidget *choose;

    dialog->homeserver_row = add_entry(
        group, "Homeserver",
        clawt_json_string(integration, "homeserver", ""));

    sign_in = gtk_button_new_with_label("Sign in\342\200\246");
    gtk_widget_set_valign(sign_in, GTK_ALIGN_CENTER);
    g_signal_connect(sign_in, "clicked", G_CALLBACK(on_matrix_sign_in),
                     dialog);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(dialog->homeserver_row), sign_in);

    dialog->user_row = add_entry(group, "User id",
                                 clawt_json_string(integration, "user_id",
                                                   ""));
    adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(dialog->user_row),
                                        FALSE);

    dialog->rooms_row = add_entry(group, "Rooms",
                                  NULL);
    {
        g_autofree gchar *rooms = join_strings(integration, "rooms", ", ");

        gtk_editable_set_text(GTK_EDITABLE(dialog->rooms_row), rooms);
    }

    choose = gtk_button_new_with_label("Choose\342\200\246");
    gtk_widget_set_valign(choose, GTK_ALIGN_CENTER);
    g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_rooms), dialog);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(dialog->rooms_row), choose);

    dialog->mention_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->mention_row),
                                  "Only when mentioned");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->mention_row),
        "Off means a turn for every message in these rooms, including ones "
        "between two other people.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->mention_row),
        integration == NULL ||
        !json_object_has_member(integration, "require_mention") ||
        json_object_get_boolean_member(integration, "require_mention"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              dialog->mention_row);
}


/*
 * A notifier's rows.
 *
 * Which of them make sense depends on the backend, and they are all
 * built rather than swapped as it changes: a dialog that rebuilds itself
 * under somebody's cursor loses whatever they were typing, and an unused
 * entry left empty costs nothing.
 */
static void
build_notify_rows(IntegrationDialog *dialog, GtkWidget *group,
                  JsonObject *integration)
{
    static const gchar *const backends[] = {
        "Desktop notification", "ntfy", "Gotify", "Matrix room",
        "Run a command", NULL
    };
    static const gchar *const backend_ids[] = {
        "desktop", "ntfy", "gotify", "matrix", "command"
    };
    static const gchar *const priorities[] = {
        "Low", "Normal", "High", "Urgent", NULL
    };
    static const struct {
        const gchar *id;
        const gchar *title;
        const gchar *subtitle;
    } events[] = {
        { "question", "Blocked on you",
          "An agent said something and is waiting" },
        { "error",    "Broken",
          "An agent stopped in a way nobody asked for" },
        { "done",     "Finished a task",
          "Off by default: a fleet that works finishes tasks all day" },
        { "routine",  "A routine failed",
          "A scheduled run that could not be started" }
    };
    g_autofree gchar *chosen = join_strings(integration, "events", ",");
    const gchar *backend = clawt_json_string(integration, "backend",
                                             "desktop");
    const gchar *priority = clawt_json_string(integration, "priority",
                                              "normal");
    gsize i;

    dialog->backend_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->backend_row),
                                  "How it reaches you");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->backend_row),
                            G_LIST_MODEL(gtk_string_list_new(backends)));

    for (i = 0; i < G_N_ELEMENTS(backend_ids); i++) {
        if (g_strcmp0(backend_ids[i], backend) == 0)
            adw_combo_row_set_selected(ADW_COMBO_ROW(dialog->backend_row),
                                       (guint)i);
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              dialog->backend_row);

    dialog->url_row = add_entry(group, "URL",
                                clawt_json_string(integration, "url", ""));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->url_row),
        "ntfy: the topic, https://ntfy.sh/your-topic. Gotify: the server.");

    dialog->homeserver_row = add_entry(
        group, "Homeserver",
        clawt_json_string(integration, "homeserver", ""));
    dialog->rooms_row = add_entry(group, "Room",
                                  clawt_json_string(integration, "room", ""));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->rooms_row),
                                "Matrix only. A room with nobody else in it "
                                "works well.");

    dialog->secret_row = add_entry(group, "Token",
                                   clawt_json_string(integration, "token",
                                                     ""));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->secret_row),
                                "A reference: env:NAME, file:PATH or "
                                "command:...");

    dialog->command_row = add_entry(
        group, "Command", clawt_json_string(integration, "command", ""));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->command_row),
        "Gets the title and the body as two arguments, or wherever you "
        "write {{title}} and {{body}}.");

    {
        g_autofree gchar *args = join_strings(integration, "args", ", ");

        dialog->args_row = add_entry(group, "Arguments", args);
    }

    dialog->priority_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->priority_row),
                                  "Priority");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->priority_row),
                            G_LIST_MODEL(gtk_string_list_new(priorities)));
    adw_combo_row_set_selected(
        ADW_COMBO_ROW(dialog->priority_row),
        g_strcmp0(priority, "low") == 0 ? 0
            : (g_strcmp0(priority, "high") == 0 ? 2
                : (g_strcmp0(priority, "urgent") == 0 ? 3 : 1)));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              dialog->priority_row);

    dialog->notify_title_row = add_entry(
        group, "Say it is from",
        clawt_json_string(integration, "title", ""));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->notify_title_row),
                                "Worth setting when several fleets notify "
                                "the same phone");

    dialog->quiet_row = add_entry(
        group, "Quiet hours",
        clawt_json_string(integration, "quiet_hours", ""));
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->quiet_row),
        "Such as 23:00-07:00. Silences this one completely -- to be woken "
        "only for a broken agent, make a second notifier without it.");

    dialog->event_rows = g_ptr_array_new();

    for (i = 0; i < G_N_ELEMENTS(events); i++) {
        GtkWidget *row = adw_switch_row_new();
        g_auto(GStrv) parts = g_strsplit(chosen, ",", -1);
        guint k;

        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                      events[i].title);
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), events[i].subtitle);

        /*
         * An instance with no `events` of its own is on the schema
         * default, which is question and error -- so an empty list here
         * would show two switches off that the daemon has on.
         */
        if (*chosen == '\0') {
            adw_switch_row_set_active(
                ADW_SWITCH_ROW(row),
                g_strcmp0(events[i].id, "question") == 0 ||
                g_strcmp0(events[i].id, "error") == 0);
        } else {
            for (k = 0; parts[k] != NULL; k++) {
                if (g_strcmp0(g_strstrip(parts[k]), events[i].id) == 0)
                    adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
            }
        }

        g_object_set_data_full(G_OBJECT(row), "event",
                               g_strdup(events[i].id), g_free);
        g_ptr_array_add(dialog->event_rows, row);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
    }
}

static void
open_integration_editor(ClawtWindow *self, const gchar *name,
                        const gchar *type_id)
{
    IntegrationDialog *dialog = g_new0(IntegrationDialog, 1);
    g_autoptr(JsonNode) list = NULL;
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *actions = adw_preferences_group_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *save;
    GtkWidget *check;
    GtkWidget *remove;
    JsonObject *integration;
    static const gchar *const scopes[] = {
        "Every agent", "These agents", "Nobody", NULL
    };

    list = clawt_window_request(self, "integration.list", NULL);
    integration = find_integration(list, name);

    dialog->window = self;
    dialog->dialog = window;
    dialog->name = g_strdup(name);
    dialog->type_id = g_strdup(
        integration != NULL ? clawt_json_string(integration, "type", type_id)
                            : type_id);

    adw_dialog_set_title(window, name);
    adw_dialog_set_content_width(window, 560);
    adw_dialog_set_content_height(window, 680);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    dialog->type_id);
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        integration != NULL ? clawt_json_string(integration, "summary", "")
                            : "");

    dialog->enabled_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->enabled_row),
                                  "Enabled");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->enabled_row),
        integration == NULL ||
        json_object_get_boolean_member(integration, "enabled"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              dialog->enabled_row);

    dialog->description_row = add_entry(
        group, "What it is for",
        clawt_json_string(integration, "description", ""));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->description_row),
                                "Written into every agent's TOOLS.org");

    dialog->scope_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->scope_row),
                                  "Who gets it");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->scope_row),
                            G_LIST_MODEL(gtk_string_list_new(scopes)));

    {
        const gchar *scope = clawt_json_string(integration, "scope",
                                               "selected");

        adw_combo_row_set_selected(
            ADW_COMBO_ROW(dialog->scope_row),
            g_strcmp0(scope, "all") == 0 ? 0
                : (g_strcmp0(scope, "none") == 0 ? 2 : 1));
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), dialog->scope_row);

    if (g_strcmp0(dialog->type_id, "matrix") == 0) {
        build_matrix_rows(dialog, group, integration);
    } else if (g_strcmp0(dialog->type_id, "email") == 0) {
        dialog->imap_host_row = add_entry(
            group, "IMAP host", clawt_json_string(integration, "imap_host",
                                                  ""));
        dialog->imap_port_row = add_int_entry(group, "IMAP port", integration,
                                              "imap_port");
        dialog->smtp_host_row = add_entry(
            group, "SMTP host", clawt_json_string(integration, "smtp_host",
                                                  ""));
        dialog->smtp_port_row = add_int_entry(group, "SMTP port", integration,
                                              "smtp_port");
        dialog->username_row = add_entry(
            group, "Mailbox", clawt_json_string(integration, "username", ""));
        dialog->secret_row = add_entry(
            group, "Password",
            clawt_json_string(integration, "password", ""));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->secret_row),
                                    "A reference: env:NAME, file:PATH or "
                                    "command:...");
    } else if (g_strcmp0(dialog->type_id, "webhook") == 0) {
        dialog->port_row = add_int_entry(group, "Port", integration, "port");
        adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->port_row),
                                    "Must differ per agent -- two cannot "
                                    "bind the same one");
    } else if (g_strcmp0(dialog->type_id, "mcp") == 0) {
        g_autofree gchar *args = join_strings(integration, "args", ", ");

        dialog->command_row = add_entry(
            group, "Command", clawt_json_string(integration, "command", ""));
        dialog->args_row = add_entry(group, "Arguments", args);
        dialog->url_row = add_entry(
            group, "Or a URL", clawt_json_string(integration, "url", ""));
        adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->url_row),
                                    "One or the other, never both");
    } else if (g_strcmp0(dialog->type_id, "notify") == 0) {
        build_notify_rows(dialog, group, integration);
    }

    build_agent_group(dialog, integration);

    save = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_hexpand(save, TRUE);
    g_signal_connect(save, "clicked", G_CALLBACK(on_integration_saved),
                     dialog);

    check = gtk_button_new_with_label(
        g_strcmp0(dialog->type_id, "notify") == 0 ? "Send a test" : "Check");
    gtk_widget_set_hexpand(check, TRUE);
    g_signal_connect(check, "clicked", G_CALLBACK(on_integration_checked),
                     dialog);

    remove = gtk_button_new_with_label("Remove");
    gtk_widget_add_css_class(remove, "destructive-action");
    gtk_widget_set_hexpand(remove, TRUE);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_integration_removed),
                     dialog);

    gtk_box_append(GTK_BOX(buttons), save);
    gtk_box_append(GTK_BOX(buttons), check);
    gtk_box_append(GTK_BOX(buttons), remove);
    gtk_widget_set_margin_top(buttons, 12);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), buttons);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(dialog->agents_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(actions));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(window, toolbar);

    g_object_set_data_full(G_OBJECT(window), "dialog", dialog,
                           integration_dialog_free);
    adw_dialog_present(window, GTK_WIDGET(self));
}

/* ── Adding one ──────────────────────────────────────────────────── */

typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    GtkWidget   *name_row;
    GtkWidget   *type_row;
    GStrv        types;
} AddIntegration;

static void
add_integration_free(gpointer data)
{
    AddIntegration *add = data;

    g_strfreev(add->types);
    g_free(add);
}

static void
on_integration_added(GtkButton *button, gpointer user_data)
{
    AddIntegration *add = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(add->name_row));
    guint selected = adw_combo_row_get_selected(ADW_COMBO_ROW(add->type_row));
    const gchar *type_id;

    (void)button;

    if (name == NULL || *name == '\0') {
        clawt_window_toast(add->window, "It needs a name.");
        return;
    }

    if (add->types == NULL || selected >= g_strv_length(add->types))
        return;

    type_id = add->types[selected];

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, type_id);
    json_builder_end_object(builder);

    reply = clawt_window_request(add->window, "integration.add",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    refresh_settings_integrations(add->window);
    adw_dialog_close(add->dialog);

    /*
     * Straight into the editor.  An integration with nothing but a name
     * and a type reaches nobody and does nothing, so stopping here would
     * be stopping halfway.
     */
    open_integration_editor(add->window, name, type_id);
}

static void
on_add_integration(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AddIntegration *add = g_new0(AddIntegration, 1);
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(GPtrArray) ids = NULL;
    AdwDialog *dialog = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkStringList *labels = gtk_string_list_new(NULL);
    GtkWidget *create;
    JsonArray *types;
    guint i;

    (void)button;

    add->window = self;
    add->dialog = dialog;

    reply = clawt_window_request(self, "integration.types", NULL);

    if (reply == NULL)
        return;

    types = json_object_get_array_member(json_node_get_object(reply),
                                         "types");
    ids = g_ptr_array_new();

    for (i = 0; i < json_array_get_length(types); i++) {
        JsonObject *type = json_array_get_object_element(types, i);
        const gchar *id = clawt_json_string(type, "id", "");
        g_autofree gchar *label = g_strdup_printf(
            "%s \342\200\224 %s", id, clawt_json_string(type, "summary", ""));

        gtk_string_list_append(labels, label);
        g_ptr_array_add(ids, g_strdup(id));
    }

    g_ptr_array_add(ids, NULL);
    add->types = (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);

    adw_dialog_set_title(dialog, "Add an integration");
    adw_dialog_set_content_width(dialog, 560);

    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "One connection, pointed at whichever agents should have it. The "
        "name is how you refer to it later, and for an MCP server it is "
        "also the key it gets in every agent's .mcp.json.");

    add->name_row = adw_entry_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(add->name_row),
                                       FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->name_row), "Name");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->name_row);

    add->type_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->type_row), "Kind");
    adw_combo_row_set_model(ADW_COMBO_ROW(add->type_row),
                            G_LIST_MODEL(labels));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->type_row);

    create = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_margin_top(create, 12);
    g_signal_connect(create, "clicked", G_CALLBACK(on_integration_added),
                     add);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(dialog, toolbar);

    g_object_set_data_full(G_OBJECT(dialog), "add", add,
                           add_integration_free);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* ── The settings list ───────────────────────────────────────────── */

static void
on_integration_activated(GtkListBox *box, GtkListBoxRow *row,
                         gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name;

    (void)box;

    if (row == NULL)
        return;

    name = g_object_get_data(G_OBJECT(row), "integration");

    if (name == NULL)
        return;

    open_integration_editor(self, name, NULL);
}

static void
refresh_settings_integrations(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *integrations;
    JsonArray *warnings;
    guint i;

    if (self->settings_integrations == NULL)
        return;

    if (!refresh_enter(self, CLAWT_REFRESH_INTEGRATIONS))
        return;

    do {
        clear_list(GTK_LIST_BOX(self->settings_integrations));

        reply = clawt_window_request(self, "integration.list", NULL);

        if (reply == NULL)
            continue;

        integrations = json_object_get_array_member(
            json_node_get_object(reply), "integrations");

        for (i = 0; i < json_array_get_length(integrations); i++) {
            JsonObject *integration =
                json_array_get_object_element(integrations, i);
            const gchar *name = clawt_json_string(integration, "name", "?");
            JsonArray *effective =
                json_object_get_array_member(integration, "effective_agents");
            const gchar *scope = clawt_json_string(integration, "scope",
                                                   "selected");
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *subtitle = NULL;
            g_autofree gchar *reach = NULL;

            if (g_strcmp0(scope, "all") == 0)
                reach = g_strdup_printf("every agent (%u)",
                                        json_array_get_length(effective));
            else if (json_array_get_length(effective) == 0)
                reach = g_strdup("nobody yet");
            else
                reach = join_strings(integration, "effective_agents", ", ");

            subtitle = g_strdup_printf(
                "%s \342\200\224 %s%s",
                clawt_json_string(integration, "type", "?"), reach,
                json_object_get_boolean_member(integration, "enabled")
                    ? "" : " (off)");

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
            row_opens_something(row);

            g_object_set_data_full(G_OBJECT(row), "integration",
                                   g_strdup(name), g_free);
            gtk_list_box_append(GTK_LIST_BOX(self->settings_integrations),
                                row);
        }

        /*
         * A collision between two agents sharing one account is worth
         * showing here rather than only in the daemon's log: it is the
         * failure that looks like the fleet misbehaving rather than like
         * a config mistake.
         */
        warnings = json_object_get_array_member(json_node_get_object(reply),
                                                "warnings");

        for (i = 0; i < json_array_get_length(warnings); i++) {
            GtkWidget *row = adw_action_row_new();

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(row),
                json_array_get_string_element(warnings, i));
            adw_action_row_add_prefix(
                ADW_ACTION_ROW(row),
                gtk_image_new_from_icon_name("dialog-warning-symbolic"));
            gtk_widget_set_sensitive(row, FALSE);
            gtk_list_box_append(GTK_LIST_BOX(self->settings_integrations),
                                row);
        }
    } while (refresh_repeat(self, CLAWT_REFRESH_INTEGRATIONS));
}

/* ── Connectors ──────────────────────────────────────────────────── */

/*
 * Authorising takes as long as a person takes.
 *
 * The request iterates the window's own main context while it waits, so
 * the dialog showing the code stays painted and the rest of the window
 * keeps working -- but the default two-minute timeout would give up
 * while somebody was still unlocking their phone.
 */
static JsonNode *
connector_request_slow(ClawtWindow *self, const gchar *kind, JsonNode *payload,
                       gint seconds)
{
    g_autoptr(GError) error = NULL;
    JsonNode *reply;

    reply = clawt_client_request_full(self->client, kind,
                                      payload, seconds, &error);

    if (reply == NULL) {
        clawt_window_toast(self, error->message);
        return NULL;
    }

    return reply;
}

static void refresh_settings_connectors(ClawtWindow *self);

/*
 * Shows the code, then waits.
 *
 * The code is the whole of the interaction for a device flow, so it gets
 * the dialog's heading at full size rather than being a line of body
 * text: it is about to be read off this screen and typed into another
 * device, quite possibly across a room.
 */
static void
connector_run_flow(ClawtWindow *self, const gchar *name)
{
    g_autoptr(JsonNode) begun = NULL;
    g_autoptr(JsonNode) done = NULL;
    JsonObject *root;
    AdwAlertDialog *dialog;
    const gchar *method;
    const gchar *flow;
    g_autofree gchar *body = NULL;

    begun = clawt_window_request(self, "connector.begin",
                                 clawt_build_payload("name", name, NULL));

    if (begun == NULL)
        return;

    root = json_node_get_object(begun);
    method = json_object_get_string_member_with_default(root, "method", "");
    flow = json_object_get_string_member_with_default(root, "flow", NULL);

    if (g_strcmp0(method, "device") == 0) {
        const gchar *uri =
            json_object_get_string_member_with_default(root,
                                                       "verification_uri",
                                                       "the provider's page");
        const gchar *code =
            json_object_get_string_member_with_default(root, "user_code", "?");

        body = g_strdup_printf("Enter this code at\n%s", uri);
        dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(code, body));
    } else {
        const gchar *url =
            json_object_get_string_member_with_default(root, "authorize_url",
                                                       NULL);

        body = g_strdup_printf("Open this to approve:\n\n%s",
                               url != NULL ? url : "(no URL)");
        dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new("Waiting for you",
                                                        body));

        if (url != NULL) {
            g_autoptr(GtkUriLauncher) launcher = gtk_uri_launcher_new(url);

            /*
             * Opened for them, and still shown above: a browser that
             * declines to open -- or opens somewhere they are not
             * looking -- would otherwise leave a dialog saying "approve
             * this" with nothing to approve.
             */
            gtk_uri_launcher_launch(launcher, GTK_WINDOW(self), NULL, NULL,
                                    NULL);
        }
    }

    adw_alert_dialog_add_response(dialog, "close", "Close");
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));

    /*
     * Fifteen minutes, which is how long a device code is usually good
     * for. Giving up sooner would report a failure for a flow that was
     * about to succeed -- and leave the daemon holding a credential
     * nobody had been told about.
     */
    done = connector_request_slow(self, "connector.await",
                                  clawt_build_payload("flow", flow, NULL),
                                  900);

    adw_dialog_close(ADW_DIALOG(dialog));

    if (done != NULL)
        clawt_window_toast(self, "Connected.");

    refresh_settings_connectors(self);
}

typedef struct {
    ClawtWindow *window;
    gchar       *name;
    AdwDialog   *dialog;
    GtkWidget   *entry;
} ConnectorAction;

static void
connector_action_free(gpointer data, GClosure *closure)
{
    ConnectorAction *action = data;

    g_free(action->name);
    g_free(action);
}

static void
on_connector_connect(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;

    adw_dialog_close(action->dialog);
    connector_run_flow(action->window, action->name);
}

static void
on_connector_key_entered(AdwAlertDialog *dialog, const gchar *response,
                         gpointer user_data)
{
    ConnectorAction *action = user_data;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *key;

    if (g_strcmp0(response, "save") != 0)
        return;

    key = gtk_editable_get_text(GTK_EDITABLE(action->entry));

    if (key == NULL || *key == '\0')
        return;

    reply = clawt_window_request(action->window, "connector.key",
                                 clawt_build_payload("name", action->name,
                                                      "key", key, NULL));

    /*
     * Cleared whether or not it was accepted.  A rejected token is still
     * a live one, and leaving it in a widget puts it in the accessibility
     * tree and in whatever the toolkit last rendered.
     */
    gtk_editable_set_text(GTK_EDITABLE(action->entry), "");

    if (reply != NULL)
        clawt_window_toast(action->window, "Token stored.");

    refresh_settings_connectors(action->window);
}

static void
on_connector_key(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;
    AdwAlertDialog *ask;
    GtkWidget *entry;

    adw_dialog_close(action->dialog);

    ask = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "Paste a token",
        "A personal access token works as well as an authorization, and "
        "needs no application registered with the provider. It is stored "
        "0600 and never reaches the agent."));

    entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(entry), TRUE);
    adw_alert_dialog_set_extra_child(ask, entry);

    adw_alert_dialog_add_response(ask, "cancel", "Cancel");
    adw_alert_dialog_add_response(ask, "save", "Store");
    adw_alert_dialog_set_response_appearance(ask, "save",
                                             ADW_RESPONSE_SUGGESTED);

    action->entry = entry;

    g_signal_connect_data(ask, "response",
                          G_CALLBACK(on_connector_key_entered), action,
                          connector_action_free, 0);

    adw_dialog_present(ADW_DIALOG(ask), GTK_WIDGET(action->window));
}

static void
on_connector_refresh(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;
    g_autoptr(JsonNode) reply = NULL;

    adw_dialog_close(action->dialog);

    reply = clawt_window_request(action->window, "connector.refresh",
                                 clawt_build_payload("name", action->name,
                                                      NULL));

    if (reply != NULL)
        clawt_window_toast(action->window, "Renewed.");

    refresh_settings_connectors(action->window);
}

static void
on_connector_revoke(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;
    g_autoptr(JsonNode) reply = NULL;

    adw_dialog_close(action->dialog);

    reply = clawt_window_request(action->window, "connector.revoke",
                                 clawt_build_payload("name", action->name,
                                                      NULL));

    if (reply != NULL) {
        JsonObject *root = json_node_get_object(reply);

        /*
         * Says which of the two things happened.  Somebody who believes
         * a token is dead and finds it working months later has been
         * misled by this message, and the remaining step is on a page
         * only they can reach.
         */
        if (json_object_get_boolean_member_with_default(root, "told_provider",
                                                         FALSE))
            clawt_window_toast(action->window,
                               "Revoked, and the provider was told.");
        else
            clawt_window_toast(action->window,
                               "Forgotten here -- withdraw it in the "
                               "provider's settings to finish.");
    }

    refresh_settings_connectors(action->window);
}

static void
on_connector_remove(GtkButton *button, gpointer user_data)
{
    ConnectorAction *action = user_data;
    g_autoptr(JsonNode) revoked = NULL;
    g_autoptr(JsonNode) reply = NULL;

    adw_dialog_close(action->dialog);

    /*
     * The credential first. Removing the integration and leaving the
     * token behind would strand a live credential under a name nothing
     * refers to any more.
     */
    revoked = clawt_window_request(action->window, "connector.revoke",
                                   clawt_build_payload("name", action->name,
                                                        NULL));

    reply = clawt_window_request(action->window, "integration.remove",
                                 clawt_build_payload("name", action->name,
                                                      NULL));

    if (reply != NULL)
        clawt_window_toast(action->window, "Removed.");

    refresh_settings_connectors(action->window);
}

/*
 * Plain buttons in a box rather than a list.
 *
 * A GtkListBox selects a row when it takes focus and a popover takes
 * focus as it opens, so a menu built from ::row-selected runs its first
 * entry before anybody has chosen anything -- and the first entry here
 * would be Revoke.
 */
static void
on_connector_activated(GtkListBox *list, GtkListBoxRow *row,
                       gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name = g_object_get_data(G_OBJECT(row), "clawt-connector");
    const gchar *provider = g_object_get_data(G_OBJECT(row),
                                              "clawt-connector-provider");
    AdwDialog *dialog;
    GtkWidget *toolbar;
    GtkWidget *box;
    GtkWidget *button;
    ConnectorAction *action;
    struct {
        const gchar *label;
        const gchar *css;
        GCallback    handler;
    } actions[] = {
        { "Authorize again", NULL,      G_CALLBACK(on_connector_connect) },
        { "Paste a token",   NULL,      G_CALLBACK(on_connector_key) },
        { "Renew now",       NULL,      G_CALLBACK(on_connector_refresh) },
        { "Revoke",          "destructive-action",
          G_CALLBACK(on_connector_revoke) },
        { "Remove",          "destructive-action",
          G_CALLBACK(on_connector_remove) }
    };
    gsize i;

    if (name == NULL)
        return;

    dialog = adw_dialog_new();
    adw_dialog_set_title(dialog, name);
    adw_dialog_set_content_width(dialog, 380);

    toolbar = adw_toolbar_view_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());

    box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 18);
    gtk_widget_set_margin_end(box, 18);

    if (provider != NULL) {
        GtkWidget *label = gtk_label_new(provider);

        gtk_widget_add_css_class(label, "dim-label");
        gtk_box_append(GTK_BOX(box), label);
    }

    for (i = 0; i < G_N_ELEMENTS(actions); i++) {
        action = g_new0(ConnectorAction, 1);
        action->window = self;
        action->name = g_strdup(name);
        action->dialog = dialog;

        button = gtk_button_new_with_label(actions[i].label);

        if (actions[i].css != NULL)
            gtk_widget_add_css_class(button, actions[i].css);

        g_signal_connect_data(button, "clicked", actions[i].handler, action,
                              connector_action_free, 0);
        gtk_box_append(GTK_BOX(box), button);
    }

    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);
    adw_dialog_set_child(dialog, toolbar);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

static void
refresh_settings_connectors(ClawtWindow *self)
{
    if (self->settings_connectors == NULL)
        return;

    if (!refresh_enter(self, CLAWT_REFRESH_CONNECTORS))
        return;

    do {
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *connectors;
        guint i;

        clear_list(GTK_LIST_BOX(self->settings_connectors));

        reply = clawt_window_request(self, "connector.list", NULL);

        if (reply == NULL)
            continue;

        connectors = json_object_get_array_member(json_node_get_object(reply),
                                                  "connectors");

        if (json_array_get_length(connectors) == 0) {
            GtkWidget *row = adw_action_row_new();

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          "No connected accounts");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(row),
                "Connect one to give agents a service's tools without "
                "giving them the credential");
            gtk_widget_set_sensitive(row, FALSE);
            gtk_list_box_append(GTK_LIST_BOX(self->settings_connectors), row);
            continue;
        }

        for (i = 0; i < json_array_get_length(connectors); i++) {
            JsonObject *entry =
                json_array_get_object_element(connectors, i);
            const gchar *name =
                json_object_get_string_member_with_default(entry, "name", "?");
            const gchar *provider =
                json_object_get_string_member_with_default(entry, "provider",
                                                           "");
            const gchar *account =
                json_object_get_string_member_with_default(entry, "account",
                                                           NULL);
            gboolean connected =
                json_object_get_boolean_member_with_default(entry,
                                                            "connected",
                                                            FALSE);
            gint64 expires =
                json_object_get_int_member_with_default(entry, "expires_at",
                                                        0);
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *subtitle = NULL;
            const gchar *icon;

            if (account != NULL && *account != '\0')
                subtitle = g_strdup_printf("%s -- %s", provider, account);
            else
                subtitle = g_strdup(provider);

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            /*
             * Three states, not two.  An expired credential that can
             * renew itself is working normally, and showing it as broken
             * would light up the list every hour and teach somebody to
             * ignore the one that genuinely is.
             */
            if (!connected)
                icon = "channel-insecure-symbolic";
            else if (expires > 0 &&
                     expires <= g_get_real_time() / G_USEC_PER_SEC &&
                     !json_object_get_boolean_member_with_default(
                         entry, "renewable", FALSE))
                icon = "dialog-warning-symbolic";
            else
                icon = "emblem-ok-symbolic";

            adw_action_row_add_suffix(ADW_ACTION_ROW(row),
                                      gtk_image_new_from_icon_name(icon));

            g_object_set_data_full(G_OBJECT(row), "clawt-connector",
                                   g_strdup(name), g_free);
            g_object_set_data_full(G_OBJECT(row), "clawt-connector-provider",
                                   g_strdup(provider), g_free);

            /*
             * This used to pass the row as its own activatable widget,
             * which made it activatable and crashed the client on the
             * first click. See row_opens_something().
             */
            row_opens_something(row);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_connectors), row);
        }
    } while (refresh_repeat(self, CLAWT_REFRESH_CONNECTORS));
}

typedef struct {
    ClawtWindow   *window;
    AdwDialog     *dialog;
    GtkWidget     *provider;
    GtkWidget     *name;
    GtkWidget     *account;
    GtkWidget     *client_id;
    GtkWidget     *instance;
    GtkWidget     *scope;
    GtkWidget     *agents;
    GtkStringList *provider_ids;
} AddConnector;

static void
add_connector_free(gpointer data, GClosure *closure)
{
    AddConnector *add = data;

    g_clear_object(&add->provider_ids);
    g_free(add);
}

static void
on_create_connector(GtkButton *button, gpointer user_data)
{
    AddConnector *add = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(add->name));
    const gchar *client_id =
        gtk_editable_get_text(GTK_EDITABLE(add->client_id));
    const gchar *instance = gtk_editable_get_text(GTK_EDITABLE(add->instance));
    const gchar *account = gtk_editable_get_text(GTK_EDITABLE(add->account));
    const gchar *agents = gtk_editable_get_text(GTK_EDITABLE(add->agents));
    const gchar *provider;
    guint selected;
    g_autofree gchar *chosen = g_strdup(name);

    if (name == NULL || *name == '\0') {
        clawt_window_toast(add->window, "It needs a name.");
        return;
    }

    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(add->provider));
    provider = gtk_string_list_get_string(add->provider_ids, selected);

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "connector");
    json_builder_set_member_name(builder, "provider");
    json_builder_add_string_value(builder, provider);
    json_builder_set_member_name(builder, "scope");
    json_builder_add_string_value(
        builder, adw_combo_row_get_selected(ADW_COMBO_ROW(add->scope)) == 0
                 ? "all" : "selected");

    if (account != NULL && *account != '\0') {
        json_builder_set_member_name(builder, "account");
        json_builder_add_string_value(builder, account);
    }

    if (client_id != NULL && *client_id != '\0') {
        json_builder_set_member_name(builder, "client_id");
        json_builder_add_string_value(builder, client_id);
    }

    if (instance != NULL && *instance != '\0') {
        json_builder_set_member_name(builder, "instance");
        json_builder_add_string_value(builder, instance);
    }

    if (agents != NULL && *agents != '\0') {
        g_auto(GStrv) ids = g_strsplit(agents, ",", -1);
        gsize i;

        json_builder_set_member_name(builder, "agents");
        json_builder_begin_array(builder);

        for (i = 0; ids[i] != NULL; i++)
            json_builder_add_string_value(builder, g_strstrip(ids[i]));

        json_builder_end_array(builder);
    }

    json_builder_end_object(builder);

    reply = clawt_window_request(add->window, "integration.add",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    adw_dialog_close(add->dialog);
    refresh_settings_connectors(add->window);

    /*
     * Straight into the flow only when there is an application to run
     * it with. Without a client id the provider has nothing to identify
     * the request, and starting something that cannot succeed is worse
     * than leaving the person on a list where the next step is visible.
     */
    if (client_id != NULL && *client_id != '\0')
        connector_run_flow(add->window, chosen);
    else
        clawt_window_toast(add->window,
                           "Added. Open it to paste a token, or set a "
                           "client id to authorize.");
}

static void
on_add_connector(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AddConnector *add = g_new0(AddConnector, 1);
    g_autoptr(JsonNode) reply = NULL;
    AdwDialog *dialog = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkStringList *labels = gtk_string_list_new(NULL);
    GtkWidget *create;
    GtkStringList *scopes;
    JsonArray *connectors;
    guint i;

    (void)button;

    add->window = self;
    add->dialog = dialog;
    add->provider_ids = gtk_string_list_new(NULL);

    reply = clawt_window_request(self, "connector.catalog", NULL);

    if (reply == NULL) {
        g_object_unref(labels);
        add_connector_free(add, NULL);
        adw_dialog_close(dialog);
        return;
    }

    connectors = json_object_get_array_member(json_node_get_object(reply),
                                              "connectors");

    for (i = 0; i < json_array_get_length(connectors); i++) {
        JsonObject *entry = json_array_get_object_element(connectors, i);
        const gchar *id =
            json_object_get_string_member_with_default(entry, "id", "?");
        const gchar *label =
            json_object_get_string_member_with_default(entry, "name", id);
        g_autofree gchar *shown = NULL;

        /*
         * The auth kind is on the label because it decides what happens
         * next: a device connector needs a client id, an api_key one
         * needs a token pasted, and being told after pressing Add is too
         * late to be useful.
         */
        shown = g_strdup_printf("%s (%s)", label,
                                json_object_get_string_member_with_default(
                                    entry, "auth", ""));

        gtk_string_list_append(labels, shown);
        gtk_string_list_append(add->provider_ids, id);
    }

    adw_dialog_set_title(dialog, "Add a connector");
    adw_dialog_set_content_width(dialog, 460);
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());

    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "clawtilla holds the credential and hands it to the tool server. "
        "Agents get the tools; they never get the token.");

    add->provider = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->provider),
                                  "Service");
    adw_combo_row_set_model(ADW_COMBO_ROW(add->provider),
                            G_LIST_MODEL(labels));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->provider);

    add->name = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->name), "Name");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->name);

    add->account = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->account),
                                  "Account (work, personal)");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->account);

    add->client_id = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->client_id),
                                  "Client ID (leave empty to paste a token)");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->client_id);

    add->instance = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->instance),
                                  "Instance (if you host it yourself)");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->instance);

    scopes = gtk_string_list_new(NULL);
    gtk_string_list_append(scopes, "Every agent");
    gtk_string_list_append(scopes, "Only the agents I name");

    add->scope = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->scope), "Who gets it");
    adw_combo_row_set_model(ADW_COMBO_ROW(add->scope), G_LIST_MODEL(scopes));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->scope);

    add->agents = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(add->agents),
                                  "Agents, comma separated");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), add->agents);

    create = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_margin_top(create, 12);
    g_signal_connect_data(create, "clicked",
                          G_CALLBACK(on_create_connector), add,
                          add_connector_free, 0);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), create);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(dialog, toolbar);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

static GtkWidget *
build_connectors_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *add;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Connectors");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "changes-prevent-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Connected accounts");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "An account clawtilla holds the credential for. Agents get its "
        "tools; the token stays here, is renewed here, and can be "
        "withdrawn here.");

    add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add, "Connect an account");
    gtk_widget_add_css_class(add, "flat");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_connector), self);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group), add);

    self->settings_connectors = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_connectors),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_connectors, "boxed-list");
    g_signal_connect(self->settings_connectors, "row-activated",
                     G_CALLBACK(on_connector_activated), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_connectors);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}

static void refresh_settings_teams(ClawtWindow *self);

/* Edit one team: its name, what it is for, and removing it. */
typedef struct {
    ClawtWindow *window;
    gchar       *team_id;
    GtkWidget   *name_entry;
    GtkWidget   *description_entry;
} TeamDialog;

static void
team_dialog_free(gpointer data)
{
    TeamDialog *editor = data;

    g_free(editor->team_id);
    g_free(editor);
}

static void
on_team_removed(AdwAlertDialog *dialog, gchar *response, gpointer user_data)
{
    TeamDialog *editor = user_data;
    g_autoptr(JsonNode) reply = NULL;

    if (g_strcmp0(response, "delete") != 0)
        return;

    reply = clawt_window_request(
        editor->window, "team.remove",
        clawt_build_payload("team", editor->team_id, NULL));

    if (reply == NULL)
        return;

    {
        gint64 orphaned = clawt_json_int(clawt_payload_of(reply),
                                         "orphaned", 0);
        g_autofree gchar *message = NULL;

        /*
         * Said rather than left to be discovered. The agents are fine and
         * still running; they are simply on a team that is no longer
         * declared, which is where hand-offs quietly stop working.
         */
        message = (orphaned > 0)
            ? g_strdup_printf("Team removed. %" G_GINT64_FORMAT " agent%s "
                              "still name it -- put them on another team.",
                              orphaned, orphaned == 1 ? "" : "s")
            : g_strdup("Team removed.");

        clawt_window_toast(editor->window, message);
    }

    refresh_settings_teams(editor->window);
    refresh_agents(editor->window);
}

static void
on_team_saved(GtkButton *button, gpointer user_data)
{
    TeamDialog *editor = user_data;
    g_autoptr(JsonNode) named = NULL;
    g_autoptr(JsonNode) described = NULL;

    named = clawt_window_request(
        editor->window, "team.set",
        clawt_build_payload("team", editor->team_id, "key", "name",
                            "value", answer_of(editor->name_entry), NULL));

    described = clawt_window_request(
        editor->window, "team.set",
        clawt_build_payload("team", editor->team_id, "key", "description",
                            "value", answer_of(editor->description_entry),
                            NULL));

    if (named == NULL || described == NULL)
        return;

    clawt_window_toast(editor->window, "Saved.");
    refresh_settings_teams(editor->window);
    refresh_agents(editor->window);
}

static void
on_team_delete_clicked(GtkButton *button, gpointer user_data)
{
    TeamDialog *editor = user_data;
    AdwAlertDialog *confirm;
    g_autofree gchar *heading = NULL;

    heading = g_strdup_printf("Remove the %s team?", editor->team_id);

    confirm = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        heading,
        "The agents on it keep running and keep their settings. They "
        "simply stop being on a team, so its lead can no longer assign "
        "to them."));

    adw_alert_dialog_add_response(confirm, "cancel", "Keep it");
    adw_alert_dialog_add_response(confirm, "delete", "Remove it");
    adw_alert_dialog_set_response_appearance(confirm, "delete",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(confirm, "cancel");
    adw_alert_dialog_set_close_response(confirm, "cancel");

    g_signal_connect(confirm, "response", G_CALLBACK(on_team_removed),
                     editor);

    adw_dialog_present(ADW_DIALOG(confirm), GTK_WIDGET(editor->window));
}

static void
on_team_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *team_id = g_object_get_data(G_OBJECT(row), "clawt-team");
    TeamDialog *editor;
    AdwDialog *dialog;
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *save;
    GtkWidget *remove;

    if (team_id == NULL)
        return;

    editor = g_new0(TeamDialog, 1);
    editor->window = self;
    editor->team_id = g_strdup(team_id);

    dialog = ADW_DIALOG(adw_preferences_dialog_new());
    adw_dialog_set_title(dialog, team_id);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), team_id);

    editor->name_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(editor->name_entry), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(editor->name_entry),
                                  "Name");
    gtk_editable_set_text(
        GTK_EDITABLE(editor->name_entry),
        (const gchar *)g_object_get_data(G_OBJECT(row), "clawt-team-name"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              editor->name_entry);

    editor->description_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(editor->description_entry), FALSE);
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(editor->description_entry), "What it handles");
    gtk_editable_set_text(
        GTK_EDITABLE(editor->description_entry),
        (const gchar *)g_object_get_data(G_OBJECT(row), "clawt-team-desc"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              editor->description_entry);

    /*
     * Said here rather than left implicit. This description is not a
     * label: the chief of staff reads it to decide what belongs to this
     * team, so a blank one means work never gets sent here.
     */
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The chief of staff reads the description to decide which team a "
        "piece of work belongs to. Say what this team handles and what it "
        "does not -- the names on it do not say that.");

    save = gtk_button_new_with_label("Save changes");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_margin_top(save, 12);
    g_signal_connect(save, "clicked", G_CALLBACK(on_team_saved), editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), save);

    remove = gtk_button_new_with_label("Remove this team");
    gtk_widget_add_css_class(remove, "destructive-action");
    gtk_widget_set_margin_top(remove, 6);
    g_signal_connect(remove, "clicked", G_CALLBACK(on_team_delete_clicked),
                     editor);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), remove);

    g_object_set_data_full(G_OBJECT(dialog), "clawt-team-editor", editor,
                           team_dialog_free);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(page));
    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

static void
on_team_add_response(AdwAlertDialog *dialog, gchar *response,
                     gpointer user_data)
{
    ClawtWindow *self = user_data;
    GtkWidget *entry = g_object_get_data(G_OBJECT(dialog), "clawt-team-id");
    g_autoptr(JsonNode) reply = NULL;
    const gchar *team_id;

    if (g_strcmp0(response, "create") != 0 || entry == NULL)
        return;

    team_id = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (team_id == NULL || *team_id == '\0') {
        clawt_window_toast(self, "A team needs an id.");
        return;
    }

    reply = clawt_window_request(self, "team.create",
                                 clawt_build_payload("id", team_id, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(self, "Team created. Give it a description so the "
                             "chief of staff knows what belongs here.");
    refresh_settings_teams(self);
    refresh_agents(self);
}

static void
on_team_add_clicked(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwAlertDialog *dialog;
    GtkWidget *entry = gtk_entry_new();

    dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "New team",
        "An id, in lowercase with hyphens. Agents name their team by it "
        "and it cannot be changed afterwards."));

    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "research");
    adw_alert_dialog_set_extra_child(dialog, entry);

    adw_alert_dialog_add_response(dialog, "cancel", "Cancel");
    adw_alert_dialog_add_response(dialog, "create", "Create");
    adw_alert_dialog_set_response_appearance(dialog, "create",
                                             ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response(dialog, "create");
    adw_alert_dialog_set_close_response(dialog, "cancel");

    g_object_set_data(G_OBJECT(dialog), "clawt-team-id", entry);
    g_signal_connect(dialog, "response", G_CALLBACK(on_team_add_response),
                     self);

    adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(self));
}

static void
refresh_settings_teams(ClawtWindow *self)
{
    if (self->settings_teams == NULL)
        return;

    if (!refresh_enter(self, CLAWT_REFRESH_TEAMS))
        return;

    do {
        g_autoptr(JsonNode) reply = NULL;
        JsonArray *teams;
        JsonArray *warnings;
        guint i;

        clear_list(GTK_LIST_BOX(self->settings_teams));

        reply = clawt_window_request(self, "team.list", NULL);

        if (reply == NULL)
            continue;

        teams = json_object_get_array_member(clawt_payload_of(reply),
                                             "teams");

        if (json_array_get_length(teams) == 0) {
            GtkWidget *row = adw_action_row_new();

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          "No teams yet");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(row),
                "A fleet works without them. They start earning their "
                "keep once there are more agents than you can hold in "
                "your head.");
            gtk_widget_set_sensitive(row, FALSE);
            gtk_list_box_append(GTK_LIST_BOX(self->settings_teams), row);
        }

        for (i = 0; i < json_array_get_length(teams); i++) {
            JsonObject *team = json_array_get_object_element(teams, i);
            const gchar *id = clawt_json_string(team, "id", "?");
            const gchar *lead = clawt_json_string(team, "lead", NULL);
            const gchar *description =
                clawt_json_string(team, "description", NULL);
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *subtitle = NULL;

            subtitle = g_strdup_printf(
                "%s \342\200\224 %" G_GINT64_FORMAT " of %"
                G_GINT64_FORMAT " running, lead: %s",
                (description != NULL && *description != '\0')
                    ? description
                    : "no description, so nothing will be sent here",
                clawt_json_int(team, "running", 0),
                clawt_json_int(team, "total", 0),
                lead != NULL ? lead : "nobody");

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(row),
                clawt_json_string(team, "name", id));
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
            row_opens_something(row);

            g_object_set_data_full(G_OBJECT(row), "clawt-team",
                                   g_strdup(id), g_free);
            g_object_set_data_full(
                G_OBJECT(row), "clawt-team-name",
                g_strdup(clawt_json_string(team, "name", id)), g_free);
            g_object_set_data_full(
                G_OBJECT(row), "clawt-team-desc",
                g_strdup(description != NULL ? description : ""), g_free);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_teams), row);
        }

        /*
         * Two leads on one team, or an agent on a team nobody declared.
         * Shown here rather than only in the daemon's log: it is the
         * failure where work quietly goes nowhere.
         */
        warnings = json_object_get_array_member(clawt_payload_of(reply),
                                                "warnings");

        for (i = 0; i < json_array_get_length(warnings); i++) {
            GtkWidget *row = adw_action_row_new();

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                          "Worth fixing");
            adw_action_row_set_subtitle(
                ADW_ACTION_ROW(row),
                json_array_get_string_element(warnings, i));
            adw_action_row_add_prefix(
                ADW_ACTION_ROW(row),
                gtk_image_new_from_icon_name("dialog-warning-symbolic"));
            gtk_list_box_append(GTK_LIST_BOX(self->settings_teams), row);
        }
    } while (refresh_repeat(self, CLAWT_REFRESH_TEAMS));
}

/* ── Spending ────────────────────────────────────────────────────── */

static void refresh_settings_spending(ClawtWindow *self);

/*
 * The windows offered, and what each means in seconds.
 *
 * "All time" is first and is the default, because the first question is
 * how much this fleet has cost at all -- and because a narrower window
 * that happened to be empty would look exactly like a feature that does
 * not work.
 */
typedef struct {
    const gchar *label;
    gint         days;      /* 0 = everything, -1 = since local midnight */
} SpendingPeriod;

static const SpendingPeriod spending_periods[] = {
    { "All time", 0 },
    { "Today", -1 },
    { "Last 7 days", 7 },
    { "Last 30 days", 30 }
};

static gint64
spending_since_for(guint index)
{
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;

    if (index >= G_N_ELEMENTS(spending_periods))
        return 0;

    if (spending_periods[index].days == 0)
        return 0;

    if (spending_periods[index].days < 0) {
        g_autoptr(GDateTime) now_dt = g_date_time_new_now_local();
        g_autoptr(GDateTime) midnight = g_date_time_new_local(
            g_date_time_get_year(now_dt), g_date_time_get_month(now_dt),
            g_date_time_get_day_of_month(now_dt), 0, 0, 0.0);

        return g_date_time_to_unix(midnight);
    }

    return now - (gint64)spending_periods[index].days * 86400;
}

static void
on_spending_period_changed(GObject *object, GParamSpec *pspec,
                           gpointer user_data)
{
    ClawtWindow *self = user_data;
    guint index = adw_combo_row_get_selected(ADW_COMBO_ROW(object));

    (void)pspec;

    self->settings_spending_since = spending_since_for(index);
    refresh_settings_spending(self);
}

/*
 * One row per agent, cheapest to read at a glance: the cost is the
 * title's suffix, the turns are the subtitle.
 */
static void
refresh_settings_spending(ClawtWindow *self)
{
    if (self->settings_spending == NULL)
        return;

    if (!refresh_enter(self, CLAWT_REFRESH_SPENDING))
        return;

    do {
        g_autoptr(JsonNode) reply = NULL;
        JsonNode *payload;
        JsonArray *agents;
        JsonObject *total;
        JsonObject *root;
        g_autoptr(JsonBuilder) builder = json_builder_new();
        guint i;

        clear_list(GTK_LIST_BOX(self->settings_spending));

        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "since");
        json_builder_add_int_value(builder, self->settings_spending_since);
        json_builder_end_object(builder);
        payload = json_builder_get_root(builder);

        reply = clawt_window_request(self, "usage.summary", payload);

        if (reply == NULL)
            continue;

        root = clawt_payload_of(reply);
        agents = json_object_get_array_member(root, "agents");
        total = json_object_get_object_member(root, "total");

        for (i = 0; i < json_array_get_length(agents); i++) {
            JsonObject *a = json_array_get_object_element(agents, i);
            gint64 turns = json_object_get_int_member_with_default(a, "turns",
                                                                   0);
            g_autofree gchar *cost = clawt_usage_format_cost(
                json_object_get_int_member_with_default(a, "cost_micros", 0));
            g_autofree gchar *subtitle = NULL;
            GtkWidget *row = adw_action_row_new();
            GtkWidget *value = gtk_label_new(cost);

            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(row),
                clawt_json_string(a, "name", clawt_json_string(a, "id", "?")));

            subtitle = (turns == 0)
                ? g_strdup("nothing recorded in this period")
                : g_strdup_printf("%" G_GINT64_FORMAT " turn%s, "
                                  "%" G_GINT64_FORMAT " tokens out",
                                  turns, turns == 1 ? "" : "s",
                                  json_object_get_int_member_with_default(
                                      a, "output_tokens", 0));

            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            gtk_widget_add_css_class(value, "numeric");
            gtk_widget_add_css_class(value, "dim-label");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), value);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_spending), row);
        }

        {
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *cost = clawt_usage_format_cost(
                json_object_get_int_member_with_default(total, "cost_micros",
                                                        0));
            GtkWidget *value = gtk_label_new(cost);
            g_autofree gchar *subtitle = g_strdup_printf(
                "%" G_GINT64_FORMAT " turns across the fleet",
                json_object_get_int_member_with_default(total, "turns", 0));

            adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "Total");
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);

            gtk_widget_add_css_class(value, "numeric");
            gtk_widget_add_css_class(value, "heading");
            adw_action_row_add_suffix(ADW_ACTION_ROW(row), value);

            gtk_list_box_append(GTK_LIST_BOX(self->settings_spending), row);
        }
    } while (refresh_repeat(self, CLAWT_REFRESH_SPENDING));
}

static GtkWidget *
build_spending_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkStringList *periods = gtk_string_list_new(NULL);
    guint i;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Spending");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "emblem-documents-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "What the fleet has cost");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "The figure each provider reported for each turn, summed per "
        "agent. Token counts cover new input and output only -- cached "
        "context is billed but is not reported as tokens, so the cost "
        "is larger than the tokens beside it suggest.");

    for (i = 0; i < G_N_ELEMENTS(spending_periods); i++)
        gtk_string_list_append(periods, spending_periods[i].label);

    self->settings_spending_period = adw_combo_row_new();
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(self->settings_spending_period), "Period");
    adw_combo_row_set_model(ADW_COMBO_ROW(self->settings_spending_period),
                            G_LIST_MODEL(periods));
    g_signal_connect(self->settings_spending_period, "notify::selected",
                     G_CALLBACK(on_spending_period_changed), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_spending_period);

    self->settings_spending = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_spending),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_spending, "boxed-list");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_spending);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}


static GtkWidget *
build_teams_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *add;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), "Teams");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "system-users-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "How the fleet is divided");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "A team has a lead who assigns work inside it, and members who "
        "talk to anyone and assign to nobody. The chief of staff sits "
        "above all of them and hands work to the leads -- it picks which "
        "team by reading these descriptions, so they are worth writing "
        "properly.");

    add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_add_css_class(add, "flat");
    gtk_widget_set_tooltip_text(add, "Add a team");
    g_signal_connect(add, "clicked", G_CALLBACK(on_team_add_clicked), self);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group),
                                            add);

    self->settings_teams = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_teams),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_teams, "boxed-list");
    g_signal_connect(self->settings_teams, "row-activated",
                     G_CALLBACK(on_team_activated), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_teams);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}

static GtkWidget *
build_integrations_page(ClawtWindow *self)
{
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *add;

    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page),
                                   "Integrations");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "network-transmit-receive-symbolic");

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group),
                                    "Connections to the outside");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(group),
        "Configured once here and pointed at one agent, some agents or the "
        "whole fleet. A channel puts a person on the other end of an "
        "agent's reply; an MCP server gives it tools.");

    add = gtk_button_new_from_icon_name("list-add-symbolic");
    gtk_widget_set_tooltip_text(add, "Add an integration");
    gtk_widget_add_css_class(add, "flat");
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_integration), self);
    adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(group),
                                            add);

    /*
     * A list box of the group's own, for the same reason the image list
     * has one: refreshing means emptying one container rather than
     * working out which of a preferences group's children were ours.
     */
    self->settings_integrations = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_integrations),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_integrations, "boxed-list");
    g_signal_connect(self->settings_integrations, "row-activated",
                     G_CALLBACK(on_integration_activated), self);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              self->settings_integrations);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));

    return page;
}

static void
on_settings_activate(GSimpleAction *action, GVariant *parameter,
                     gpointer user_data)
{
    ClawtWindow *self = user_data;
    AdwDialog *dialog;
    GtkWidget *page;
    GtkWidget *cached_group;
    GtkWidget *add_group;
    GtkWidget *catalog_button;
    GtkWidget *url_button;
    GtkStringList *labels;
    JsonArray *sources = NULL;
    guint i;

    (void)action;
    (void)parameter;

    if (self->settings != NULL) {
        adw_dialog_present(self->settings, GTK_WIDGET(self));
        return;
    }

    dialog = adw_preferences_dialog_new();
    adw_dialog_set_title(dialog, "Settings");
    adw_dialog_set_content_width(dialog, 720);
    adw_dialog_set_content_height(dialog, 600);

    page = adw_preferences_page_new();
    adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page),
                                   "Cloud images");
    adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page),
                                       "drive-harddisk-symbolic");

    cached_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(cached_group),
                                    "On this machine");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(cached_group),
        "One copy serves every agent: a VM writes to an overlay backed by "
        "the image, never to the image itself.");

    /*
     * The rows go in a list box of the group's own rather than straight
     * into the group, so refreshing means emptying one container instead
     * of tracking which children were ours.
     */
    self->settings_images = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(self->settings_images),
                                    GTK_SELECTION_NONE);
    gtk_widget_add_css_class(self->settings_images, "boxed-list");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(cached_group),
                              self->settings_images);

    add_group = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(add_group),
                                    "Fetch another");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(add_group),
        "These are suggestions, not a restriction. Any https URL to a "
        "qcow2 works, and an image with cloud-init needs no preparing.");

    labels = gtk_string_list_new(NULL);
    self->settings_catalog = clawt_window_request(self, "image.vm_catalog",
                                                  NULL);

    if (self->settings_catalog != NULL) {
        sources = json_object_get_array_member(
            json_node_get_object(self->settings_catalog), "sources");

        for (i = 0; i < json_array_get_length(sources); i++) {
            gtk_string_list_append(
                labels, clawt_json_string(
                    json_array_get_object_element(sources, i), "name", "?"));
        }
    }

    self->settings_catalog_row = adw_combo_row_new();
    adw_preferences_row_set_title(
        ADW_PREFERENCES_ROW(self->settings_catalog_row), "Suggested");
    adw_combo_row_set_model(ADW_COMBO_ROW(self->settings_catalog_row),
                            G_LIST_MODEL(labels));

    catalog_button = gtk_button_new_with_label("Download");
    gtk_widget_set_valign(catalog_button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class(catalog_button, "suggested-action");
    g_signal_connect(catalog_button, "clicked",
                     G_CALLBACK(on_download_from_catalog), self);
    adw_action_row_add_suffix(ADW_ACTION_ROW(self->settings_catalog_row),
                              catalog_button);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group),
                              self->settings_catalog_row);

    self->settings_url_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->settings_url_row),
                                  "Or a URL of your own");
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(self->settings_url_row), FALSE);

    url_button = gtk_button_new_with_label("Download");
    gtk_widget_set_valign(url_button, GTK_ALIGN_CENTER);
    g_signal_connect(url_button, "clicked", G_CALLBACK(on_download_from_url),
                     self);
    adw_entry_row_add_suffix(ADW_ENTRY_ROW(self->settings_url_row),
                             url_button);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(add_group),
                              self->settings_url_row);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(cached_group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(add_group));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(page));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   build_teams_page(self)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   build_spending_page(self)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   build_integrations_page(self)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   build_connectors_page(self)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   build_appearance_page(self)));

    self->settings = dialog;
    g_signal_connect(dialog, "closed", G_CALLBACK(on_settings_closed), self);

    refresh_settings_images(self);
    refresh_settings_teams(self);
    refresh_settings_spending(self);
    refresh_settings_integrations(self);
    refresh_settings_connectors(self);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

static void
refresh_flow(ClawtWindow *self)
{
    if (!refresh_enter(self, CLAWT_REFRESH_FLOW))
        return;

    do {
        refresh_flow_once(self);
    } while (refresh_repeat(self, CLAWT_REFRESH_FLOW));
}

static void
on_flow_filter_toggled(GtkCheckButton *button, gpointer user_data)
{
    (void)button;

    refresh_flow(user_data);
}

static GtkWidget *
build_flow_page(ClawtWindow *self)
{
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget *list_scroll = gtk_scrolled_window_new();
    GtkWidget *empty = adw_status_page_new();

    /* ── who has been talking ── */
    self->flow_include_user = gtk_check_button_new_with_label(
        "Include your own chats");
    gtk_widget_set_margin_start(self->flow_include_user, 12);
    gtk_widget_set_margin_end(self->flow_include_user, 12);
    gtk_widget_set_margin_top(self->flow_include_user, 12);
    gtk_widget_set_margin_bottom(self->flow_include_user, 6);
    gtk_widget_set_tooltip_text(
        self->flow_include_user,
        "Off by default: this page is for what the agents did without you");
    g_signal_connect(self->flow_include_user, "toggled",
                     G_CALLBACK(on_flow_filter_toggled), self);

    self->flow_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->flow_list, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(self->flow_list), "navigation-sidebar");

    /*
     * ::row-selected, not ::row-activated: libadwaita clears
     * GtkListBoxRow:activatable on an AdwActionRow with no
     * activatable-widget, so the activate signal never arrives. Selection
     * also covers arrow-key navigation, which activation does not.
     */
    g_signal_connect(self->flow_list, "row-selected",
                     G_CALLBACK(on_flow_row_selected), self);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(list_scroll),
                                  GTK_WIDGET(self->flow_list));
    gtk_widget_set_vexpand(list_scroll, TRUE);

    gtk_box_append(GTK_BOX(left), self->flow_include_user);
    gtk_box_append(GTK_BOX(left), list_scroll);
    gtk_widget_set_size_request(left, 280, -1);

    /* ── and what they said ── */
    self->flow_title = gtk_label_new(NULL);
    gtk_widget_add_css_class(self->flow_title, "title-4");
    gtk_label_set_xalign(GTK_LABEL(self->flow_title), 0.0f);

    self->flow_subtitle = gtk_label_new(NULL);
    gtk_widget_add_css_class(self->flow_subtitle, "caption");
    gtk_widget_add_css_class(self->flow_subtitle, "dim-label");
    gtk_label_set_xalign(GTK_LABEL(self->flow_subtitle), 0.0f);

    gtk_widget_set_margin_start(header, 12);
    gtk_widget_set_margin_top(header, 12);
    gtk_widget_set_margin_bottom(header, 6);
    gtk_box_append(GTK_BOX(header), self->flow_title);
    gtk_box_append(GTK_BOX(header), self->flow_subtitle);

    self->flow_transcript = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
    self->flow_scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());

    /*
     * The same measure the chat has, and for the same reason: without a
     * clamp a body label's natural width is whatever the window is, and
     * the line runs past the point where the eye can find the start of
     * the next one.  Inside the scrolled window rather than around it,
     * so the scrollbar stays at the window edge.
     */
    {
        GtkWidget *clamp = adw_clamp_new();

        adw_clamp_set_child(ADW_CLAMP(clamp),
                            GTK_WIDGET(self->flow_transcript));
        gtk_scrolled_window_set_child(self->flow_scroll, clamp);
    }

    gtk_widget_set_margin_bottom(GTK_WIDGET(self->flow_transcript), 18);
    gtk_widget_set_vexpand(GTK_WIDGET(self->flow_scroll), TRUE);

    gtk_box_append(GTK_BOX(right), header);
    gtk_box_append(GTK_BOX(right), GTK_WIDGET(self->flow_scroll));

    adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty),
                                  "system-users-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(empty), "Nothing yet");
    adw_status_page_set_description(
        ADW_STATUS_PAGE(empty),
        "When one agent messages another it appears here, newest first. "
        "Pick a conversation to read it.");

    self->flow_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(self->flow_stack), empty, "empty");
    gtk_stack_add_named(GTK_STACK(self->flow_stack), right, "room");

    gtk_paned_set_start_child(GTK_PANED(paned), left);
    gtk_paned_set_end_child(GTK_PANED(paned), self->flow_stack);
    gtk_paned_set_position(GTK_PANED(paned), 300);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);

    return paned;
}

/* ── Routines ────────────────────────────────────────────────────── */

/*
 * One routine being written.
 *
 * The schedule is a segmented control over the presets with a cron field
 * behind Custom, because those are two different questions: most people
 * want "weekdays at nine" and should never meet an expression, and the
 * ones who want every six hours should not have to bend it into a preset
 * that cannot hold it.
 *
 * (An expression with a step in it cannot be written in a C comment
 * without ending it, which is its own small argument for the field.)
 */
typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    gchar       *id;
    gboolean     creating;

    GtkWidget   *id_row;
    GtkWidget   *description_row;
    GtkTextView *instructions;
    GtkWidget   *agent_row;
    GStrv        agent_ids;
    GtkWidget   *directory_row;
    GtkWidget   *worktree_row;
    GtkWidget   *enabled_row;
    GtkWidget   *catch_up_row;

    GtkWidget   *schedule_row;
    GtkWidget   *at_row;
    GtkWidget   *weekday_row;
    GtkWidget   *cron_row;
    GtkWidget   *preview;
} RoutineDialog;

static void refresh_routines(ClawtWindow *self);

static void
routine_dialog_free(gpointer data)
{
    RoutineDialog *dialog = data;

    g_free(dialog->id);
    g_strfreev(dialog->agent_ids);
    g_free(dialog);
}

static const gchar *const schedule_ids[] = {
    "manual", "hourly", "daily", "weekdays", "weekly", "custom"
};

static const gchar *const weekday_ids[] = {
    "sunday", "monday", "tuesday", "wednesday", "thursday", "friday",
    "saturday"
};

/*
 * Shows only the fields the chosen schedule uses, and says when it will
 * next run.
 *
 * The preview is the part that earns its place: an expression is not
 * something a person can check by reading it, and "next: Mon 25 Aug
 * 09:00" is.
 */
static void
update_schedule_rows(RoutineDialog *dialog)
{
    guint selected = adw_combo_row_get_selected(
        ADW_COMBO_ROW(dialog->schedule_row));
    const gchar *preset = schedule_ids[MIN(selected,
                                           G_N_ELEMENTS(schedule_ids) - 1)];
    gboolean timed = g_strcmp0(preset, "daily") == 0 ||
                     g_strcmp0(preset, "weekdays") == 0 ||
                     g_strcmp0(preset, "weekly") == 0 ||
                     g_strcmp0(preset, "hourly") == 0;
    gboolean custom = g_strcmp0(preset, "custom") == 0;

    gtk_widget_set_visible(dialog->at_row, timed);
    gtk_widget_set_visible(dialog->weekday_row,
                           g_strcmp0(preset, "weekly") == 0);
    gtk_widget_set_visible(dialog->cron_row, custom);

    if (g_strcmp0(preset, "manual") == 0) {
        gtk_label_set_text(GTK_LABEL(dialog->preview),
                           "Only when you ask.");
        return;
    }

    gtk_label_set_text(GTK_LABEL(dialog->preview),
                       custom ? "minute hour day-of-month month day-of-week"
                              : "");
}

static void
on_schedule_changed(GObject *row, GParamSpec *spec, gpointer user_data)
{
    (void)row;
    (void)spec;

    update_schedule_rows(user_data);
}

static gchar *
instructions_text(RoutineDialog *dialog)
{
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(dialog->instructions);
    GtkTextIter start;
    GtkTextIter end;

    gtk_text_buffer_get_bounds(buffer, &start, &end);

    return gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
}

static void
on_routine_saved(GtkButton *button, gpointer user_data)
{
    RoutineDialog *dialog = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *instructions = instructions_text(dialog);
    const gchar *id = dialog->creating
        ? gtk_editable_get_text(GTK_EDITABLE(dialog->id_row)) : dialog->id;
    guint schedule = adw_combo_row_get_selected(
        ADW_COMBO_ROW(dialog->schedule_row));
    guint weekday = adw_combo_row_get_selected(
        ADW_COMBO_ROW(dialog->weekday_row));
    guint agent = adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->agent_row));

    (void)button;

    if (id == NULL || *id == '\0') {
        clawt_window_toast(dialog->window, "It needs a name.");
        return;
    }

    if (*instructions == '\0') {
        /*
         * Refused rather than saved empty: a routine with no
         * instructions fires on schedule and asks an agent for nothing,
         * which costs a turn and produces a puzzled reply.
         */
        clawt_window_toast(dialog->window,
                           "It needs instructions -- that is the whole of "
                           "what the agent is asked.");
        return;
    }

    if (dialog->agent_ids == NULL || dialog->agent_ids[0] == NULL) {
        clawt_window_toast(dialog->window,
                           "There is no agent to run it.");
        return;
    }

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, id);
    json_builder_set_member_name(builder, "agent");
    json_builder_add_string_value(
        builder, dialog->agent_ids[MIN(agent,
                                       g_strv_length(dialog->agent_ids) - 1)]);
    json_builder_set_member_name(builder, "description");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->description_row)));
    json_builder_set_member_name(builder, "instructions");
    json_builder_add_string_value(builder, instructions);
    json_builder_set_member_name(builder, "schedule");
    json_builder_add_string_value(
        builder, schedule_ids[MIN(schedule,
                                  G_N_ELEMENTS(schedule_ids) - 1)]);
    json_builder_set_member_name(builder, "at");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->at_row)));
    json_builder_set_member_name(builder, "weekday");
    json_builder_add_string_value(
        builder, weekday_ids[MIN(weekday, G_N_ELEMENTS(weekday_ids) - 1)]);
    json_builder_set_member_name(builder, "cron");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->cron_row)));
    json_builder_set_member_name(builder, "directory");
    json_builder_add_string_value(
        builder, gtk_editable_get_text(GTK_EDITABLE(dialog->directory_row)));
    json_builder_set_member_name(builder, "worktree");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(
                     ADW_SWITCH_ROW(dialog->worktree_row)));
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(
                     ADW_SWITCH_ROW(dialog->enabled_row)));
    json_builder_set_member_name(builder, "catch_up");
    json_builder_add_boolean_value(
        builder, adw_switch_row_get_active(
                     ADW_SWITCH_ROW(dialog->catch_up_row)));
    json_builder_end_object(builder);

    reply = clawt_window_request(dialog->window,
                                 dialog->creating ? "routine.add"
                                                  : "routine.update",
                                 json_builder_get_root(builder));

    if (reply == NULL)
        return;

    refresh_routines(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_routine_removed(GtkButton *button, gpointer user_data)
{
    RoutineDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    reply = clawt_window_request(dialog->window, "routine.remove",
                                 clawt_build_payload("id", dialog->id, NULL));

    if (reply == NULL)
        return;

    refresh_routines(dialog->window);
    adw_dialog_close(dialog->dialog);
}

static void
on_routine_run(GtkButton *button, gpointer user_data)
{
    RoutineDialog *dialog = user_data;
    g_autoptr(JsonNode) reply = NULL;

    (void)button;

    reply = clawt_window_request(dialog->window, "routine.run",
                                 clawt_build_payload("id", dialog->id, NULL));

    if (reply == NULL)
        return;

    clawt_window_toast(dialog->window, "Started. It is in Tasks.");
    refresh_routines(dialog->window);
}

static void
open_routine_editor(ClawtWindow *self, JsonObject *existing)
{
    RoutineDialog *dialog = g_new0(RoutineDialog, 1);
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *what = adw_preferences_group_new();
    GtkWidget *where = adw_preferences_group_new();
    GtkWidget *when = adw_preferences_group_new();
    GtkWidget *actions = adw_preferences_group_new();
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *toolbar = adw_toolbar_view_new();
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkStringList *agent_labels = gtk_string_list_new(NULL);
    g_autoptr(GPtrArray) ids = g_ptr_array_new();
    g_autoptr(JsonNode) agents = NULL;
    GtkWidget *save;
    static const gchar *const schedules[] = {
        "Manual", "Hourly", "Daily", "Weekdays", "Weekly", "Custom", NULL
    };
    static const gchar *const weekdays[] = {
        "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
        "Saturday", NULL
    };
    guint i;

    dialog->window = self;
    dialog->dialog = window;
    dialog->creating = existing == NULL;
    dialog->id = g_strdup(existing != NULL
                          ? clawt_json_string(existing, "id", "") : "");

    adw_dialog_set_title(window, dialog->creating ? "New routine"
                                                  : dialog->id);
    adw_dialog_set_content_width(window, 620);
    adw_dialog_set_content_height(window, 760);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(what),
                                    "What it does");
    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(what),
        "Routines only run while the daemon is running. A machine that "
        "was asleep at nine o'clock has missed nine o'clock.");

    dialog->id_row = add_entry(what, "Name", dialog->id);
    gtk_widget_set_sensitive(dialog->id_row, dialog->creating);

    dialog->description_row = add_entry(
        what, "Description",
        existing != NULL ? clawt_json_string(existing, "description", "")
                         : "");

    /*
     * A text view rather than an entry: the instructions are the whole
     * of the prompt, and a single line teaches people to write a
     * reminder where an instruction is needed.
     */
    dialog->instructions = GTK_TEXT_VIEW(gtk_text_view_new());
    gtk_text_view_set_wrap_mode(dialog->instructions, GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_top_margin(dialog->instructions, 8);
    gtk_text_view_set_bottom_margin(dialog->instructions, 8);
    gtk_text_view_set_left_margin(dialog->instructions, 8);
    gtk_text_view_set_right_margin(dialog->instructions, 8);
    gtk_widget_set_size_request(GTK_WIDGET(dialog->instructions), -1, 160);

    if (existing != NULL)
        gtk_text_buffer_set_text(
            gtk_text_view_get_buffer(dialog->instructions),
            clawt_json_string(existing, "instructions", ""), -1);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(dialog->instructions));
    gtk_widget_add_css_class(scroll, "card");
    gtk_widget_set_margin_top(scroll, 6);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(what), scroll);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(where),
                                    "Who and where");

    agents = clawt_window_request(self, "agent.list", NULL);

    if (agents != NULL) {
        JsonArray *array = json_object_get_array_member(
            clawt_payload_of(agents), "agents");

        for (i = 0; i < json_array_get_length(array); i++) {
            JsonObject *agent = json_array_get_object_element(array, i);
            const gchar *id = clawt_json_string(agent, "id", "");

            gtk_string_list_append(agent_labels,
                                   clawt_json_string(agent, "name", id));
            g_ptr_array_add(ids, g_strdup(id));
        }
    }

    g_ptr_array_add(ids, NULL);
    dialog->agent_ids = (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);

    dialog->agent_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->agent_row),
                                  "Agent");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->agent_row),
                            G_LIST_MODEL(agent_labels));

    if (existing != NULL) {
        const gchar *agent = clawt_json_string(existing, "agent", "");

        for (i = 0; dialog->agent_ids[i] != NULL; i++) {
            if (g_strcmp0(dialog->agent_ids[i], agent) == 0)
                adw_combo_row_set_selected(ADW_COMBO_ROW(dialog->agent_row),
                                           i);
        }
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(where),
                              dialog->agent_row);

    dialog->directory_row = add_entry(
        where, "Folder",
        existing != NULL ? clawt_json_string(existing, "directory", "") : "");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->directory_row),
        "On the agent's computer. Empty means its workspace.");

    dialog->worktree_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->worktree_row),
                                  "Use a git worktree");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->worktree_row),
        "For a routine that changes files: keeps a scheduled run off "
        "whatever you had checked out at the time.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->worktree_row),
        existing != NULL &&
        json_object_get_boolean_member(existing, "worktree"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(where),
                              dialog->worktree_row);

    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(when), "When");

    dialog->schedule_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->schedule_row),
                                  "Schedule");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->schedule_row),
                            G_LIST_MODEL(gtk_string_list_new(schedules)));

    if (existing != NULL) {
        const gchar *schedule = clawt_json_string(existing, "schedule",
                                                  "daily");

        for (i = 0; i < G_N_ELEMENTS(schedule_ids); i++) {
            if (g_strcmp0(schedule_ids[i], schedule) == 0)
                adw_combo_row_set_selected(
                    ADW_COMBO_ROW(dialog->schedule_row), i);
        }
    } else {
        adw_combo_row_set_selected(ADW_COMBO_ROW(dialog->schedule_row), 2);
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when),
                              dialog->schedule_row);

    dialog->at_row = add_entry(
        when, "At",
        existing != NULL ? clawt_json_string(existing, "at", "09:00")
                         : "09:00");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(dialog->at_row),
                                "Local time, such as 09:00");

    dialog->weekday_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->weekday_row),
                                  "Day");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->weekday_row),
                            G_LIST_MODEL(gtk_string_list_new(weekdays)));

    if (existing != NULL) {
        const gchar *day = clawt_json_string(existing, "weekday", "monday");

        for (i = 0; i < G_N_ELEMENTS(weekday_ids); i++) {
            if (g_strcmp0(weekday_ids[i], day) == 0)
                adw_combo_row_set_selected(
                    ADW_COMBO_ROW(dialog->weekday_row), i);
        }
    } else {
        adw_combo_row_set_selected(ADW_COMBO_ROW(dialog->weekday_row), 1);
    }

    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when),
                              dialog->weekday_row);

    dialog->cron_row = add_entry(
        when, "Cron expression",
        existing != NULL ? clawt_json_string(existing, "cron", "") : "");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->cron_row),
        "Note: with both day fields set, it is day-of-month OR "
        "day-of-week.");

    dialog->preview = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(dialog->preview), TRUE);
    gtk_label_set_xalign(GTK_LABEL(dialog->preview), 0.0f);
    gtk_widget_add_css_class(dialog->preview, "dim-label");
    gtk_widget_set_margin_top(dialog->preview, 6);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when), dialog->preview);

    dialog->enabled_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->enabled_row),
                                  "Enabled");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->enabled_row),
        "A disabled routine can still be run by hand, which is how you "
        "try one before trusting it.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->enabled_row),
        existing == NULL ||
        json_object_get_boolean_member(existing, "enabled"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when),
                              dialog->enabled_row);

    dialog->catch_up_row = adw_switch_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->catch_up_row),
                                  "Run a missed one at startup");
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(dialog->catch_up_row),
        "Once, however many were missed.");
    adw_switch_row_set_active(
        ADW_SWITCH_ROW(dialog->catch_up_row),
        existing != NULL &&
        json_object_get_boolean_member(existing, "catch_up"));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(when),
                              dialog->catch_up_row);

    g_signal_connect(dialog->schedule_row, "notify::selected",
                     G_CALLBACK(on_schedule_changed), dialog);

    save = gtk_button_new_with_label(dialog->creating ? "Create" : "Save");
    gtk_widget_add_css_class(save, "suggested-action");
    gtk_widget_set_hexpand(save, TRUE);
    g_signal_connect(save, "clicked", G_CALLBACK(on_routine_saved), dialog);
    gtk_box_append(GTK_BOX(buttons), save);

    if (!dialog->creating) {
        GtkWidget *run = gtk_button_new_with_label("Run now");
        GtkWidget *remove = gtk_button_new_with_label("Remove");

        gtk_widget_set_hexpand(run, TRUE);
        g_signal_connect(run, "clicked", G_CALLBACK(on_routine_run), dialog);
        gtk_box_append(GTK_BOX(buttons), run);

        gtk_widget_add_css_class(remove, "destructive-action");
        gtk_widget_set_hexpand(remove, TRUE);
        g_signal_connect(remove, "clicked", G_CALLBACK(on_routine_removed),
                         dialog);
        gtk_box_append(GTK_BOX(buttons), remove);
    }

    gtk_widget_set_margin_top(buttons, 12);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(actions), buttons);

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(what));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(where));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(when));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(actions));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar),
                                 adw_header_bar_new());
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
    adw_dialog_set_child(window, toolbar);

    g_object_set_data_full(G_OBJECT(window), "dialog", dialog,
                           routine_dialog_free);

    update_schedule_rows(dialog);
    adw_dialog_present(window, GTK_WIDGET(self));
}

static void
on_add_routine(GtkButton *button, gpointer user_data)
{
    (void)button;

    open_routine_editor(user_data, NULL);
}

static void
on_routine_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    JsonObject *routine;

    (void)box;

    if (row == NULL)
        return;

    routine = g_object_get_data(G_OBJECT(row), "routine");

    if (routine == NULL)
        return;

    open_routine_editor(self, routine);
}

static void
refresh_routines(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *routines;
    guint i;

    if (self->routine_list == NULL)
        return;

    if (!refresh_enter(self, CLAWT_REFRESH_ROUTINES))
        return;

    do {
        clear_list(self->routine_list);

        reply = clawt_window_request(self, "routine.list", NULL);

        if (reply == NULL)
            continue;

        routines = json_object_get_array_member(clawt_payload_of(reply),
                                                "routines");

        for (i = 0; i < json_array_get_length(routines); i++) {
            JsonObject *routine = json_array_get_object_element(routines, i);
            const gchar *next = clawt_json_string(routine, "next_run", NULL);
            GtkWidget *row = adw_action_row_new();
            g_autofree gchar *subtitle = NULL;
            g_autofree gchar *when = NULL;

            if (next != NULL) {
                g_autoptr(GDateTime) parsed =
                    g_date_time_new_from_iso8601(next, NULL);

                when = (parsed != NULL)
                    ? g_date_time_format(parsed, "next %a %d %b at %H:%M")
                    : g_strdup(next);
            } else if (json_object_get_boolean_member(routine, "enabled")) {
                when = g_strdup("only when you ask");
            } else {
                when = g_strdup("off");
            }

            subtitle = g_strdup_printf(
                "%s \342\200\224 %s%s%s",
                clawt_json_string(routine, "agent", "?"), when,
                g_strcmp0(clawt_json_string(routine, "last_state", "never"),
                          "never") == 0 ? "" : ", last run ",
                g_strcmp0(clawt_json_string(routine, "last_state", "never"),
                          "never") == 0
                    ? "" : clawt_json_string(routine, "last_state", ""));

            adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row),
                                               FALSE);
            adw_preferences_row_set_title(
                ADW_PREFERENCES_ROW(row),
                clawt_json_string(routine, "description",
                                  clawt_json_string(routine, "id", "?")));
            adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
            row_opens_something(row);

            g_object_set_data_full(G_OBJECT(row), "routine",
                                   json_object_ref(routine),
                                   (GDestroyNotify)json_object_unref);
            gtk_list_box_append(self->routine_list, row);
        }
    } while (refresh_repeat(self, CLAWT_REFRESH_ROUTINES));
}

static GtkWidget *
build_routine_page(ClawtWindow *self)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *scroll = gtk_scrolled_window_new();
    GtkWidget *add = gtk_button_new_with_label("New routine");
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

    self->routine_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->routine_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->routine_list), "boxed-list");
    g_signal_connect(self->routine_list, "row-activated",
                     G_CALLBACK(on_routine_activated), self);

    gtk_widget_add_css_class(add, "suggested-action");
    gtk_widget_set_halign(add, GTK_ALIGN_END);
    g_signal_connect(add, "clicked", G_CALLBACK(on_add_routine), self);
    gtk_widget_set_hexpand(bar, TRUE);
    gtk_box_append(GTK_BOX(bar), add);

    gtk_box_append(GTK_BOX(box), bar);
    gtk_box_append(GTK_BOX(box), GTK_WIDGET(self->routine_list));

    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);

    return scroll;
}

static GtkWidget *
build_task_page(ClawtWindow *self)
{
    GtkWidget *scroll = gtk_scrolled_window_new();

    self->task_list = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->task_list, GTK_SELECTION_NONE);
    gtk_widget_add_css_class(GTK_WIDGET(self->task_list), "boxed-list");
    gtk_widget_set_margin_top(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_start(GTK_WIDGET(self->task_list), 12);
    gtk_widget_set_margin_end(GTK_WIDGET(self->task_list), 12);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                  GTK_WIDGET(self->task_list));

    return scroll;
}

ClawtWindow *
clawt_window_new(AdwApplication *app, ClawtClient *client,
                 ClawtConnection *connection)
{
    ClawtWindow *self;
    GtkWidget *sidebar_box;
    GtkWidget *sidebar_scroll;
    GtkWidget *sidebar_header;
    GtkWidget *content;
    GtkWidget *header;
    GtkWidget *switcher;
    GtkWidget *title;
    GtkWidget *new_button;
    GtkWidget *sidebar_button;
    AdwBreakpoint *breakpoint;

    self = g_object_new(CLAWT_TYPE_WINDOW, "application", app, NULL);
    self->client = g_object_ref(client);

    /*
     * The profile is passed in beside the client rather than guessed at
     * from it, because the two have to agree and only the caller knows
     * both.  A window that worked out for itself what its client was
     * would be a second answer to that question, and the ones this
     * codebase has grown are all bugs.
     */
    self->active_connection =
        connection != NULL ? clawt_connection_copy(connection)
                           : clawt_connection_new_local("Local", NULL);
    self->local_socket =
        g_strdup(clawt_connection_get_socket_path(self->active_connection));

    gtk_window_set_title(GTK_WINDOW(self), "clawtilla");
    gtk_window_set_default_size(GTK_WINDOW(self), 1100, 720);

    /* ── Sidebar ── */
    self->sidebar = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->sidebar, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(self->sidebar), "navigation-sidebar");
    g_signal_connect(self->sidebar, "row-selected",
                     G_CALLBACK(on_row_selected), self);
    build_agent_menu(self);

    sidebar_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                  GTK_WIDGET(self->sidebar));
    gtk_widget_set_vexpand(sidebar_scroll, TRUE);

    sidebar_header = adw_header_bar_new();
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(sidebar_header),
                                    adw_window_title_new("Agents", NULL));

    /*
     * Packed before the + button, and therefore drawn to the right of it:
     * pack_end stacks inward from the edge, so the first one packed is
     * the outermost.
     */
    {
        GMenu *menu = g_menu_new();
        GtkWidget *menu_button = gtk_menu_button_new();
        GSimpleAction *settings_action = g_simple_action_new("settings",
                                                             NULL);

        g_menu_append(menu, "Settings\342\200\246", "win.settings");

        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu_button),
                                      "open-menu-symbolic");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(menu_button),
                                       G_MENU_MODEL(menu));
        gtk_widget_set_tooltip_text(menu_button, "Settings");
        adw_header_bar_pack_end(ADW_HEADER_BAR(sidebar_header), menu_button);

        g_signal_connect(settings_action, "activate",
                         G_CALLBACK(on_settings_activate), self);
        g_action_map_add_action(G_ACTION_MAP(self),
                                G_ACTION(settings_action));
        g_object_unref(settings_action);
        g_object_unref(menu);
    }

    /*
     * A split button rather than a plain one: pressing + creates an
     * agent, which is what it has always done and what people reach for,
     * and the arrow beside it offers importing one.
     *
     * Import needed somewhere to live.  Adopting a workspace already on
     * disk was reachable only from `clawtilla agent discover` followed by
     * `agent import`, so from the client those directories were invisible
     * -- and they are exactly what accumulates: an agent removed from the
     * config keeps its state, and a design that was never committed
     * leaves a whole workspace behind.
     */
    {
        GMenu *menu = g_menu_new();
        GSimpleAction *create_action = g_simple_action_new("agent-create",
                                                            NULL);
        GSimpleAction *import_action = g_simple_action_new("agent-import",
                                                            NULL);

        g_menu_append(menu, "Create an agent\342\200\246",
                      "win.agent-create");
        g_menu_append(menu, "Import an agent\342\200\246",
                      "win.agent-import");

        new_button = adw_split_button_new();
        adw_split_button_set_icon_name(ADW_SPLIT_BUTTON(new_button),
                                       "list-add-symbolic");
        adw_split_button_set_menu_model(ADW_SPLIT_BUTTON(new_button),
                                        G_MENU_MODEL(menu));
        gtk_widget_set_tooltip_text(new_button, "Add an agent");

        g_signal_connect(new_button, "clicked", G_CALLBACK(on_new_agent),
                         self);

        g_signal_connect(create_action, "activate",
                         G_CALLBACK(on_new_agent_activate), self);
        g_signal_connect(import_action, "activate",
                         G_CALLBACK(on_import_agent_activate), self);
        g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(create_action));
        g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(import_action));

        g_object_unref(create_action);
        g_object_unref(import_action);
        g_object_unref(menu);
    }

    adw_header_bar_pack_end(ADW_HEADER_BAR(sidebar_header), new_button);

    sidebar_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_header);
    gtk_box_append(GTK_BOX(sidebar_box), sidebar_scroll);

    /* ── Content ── */
    self->pages = ADW_VIEW_STACK(adw_view_stack_new());

    /*
     * The chat page is kept, not discarded, because the total unread is
     * drawn on it -- adw_view_stack_add_titled_with_icon() returns the
     * page and every other call here throws it away.
     */
    self->chat_page =
        adw_view_stack_add_titled_with_icon(self->pages,
                                            build_chat_page(self),
                                            "chat", "Chat",
                                            "user-available-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages,
                                        build_inspector_page(self),
                                        "agent", "Agent",
                                        "emblem-system-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages, build_mailbox_page(self),
                                        "mailbox", "Mailbox",
                                        "mail-unread-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages,
                                        build_computer_page(self),
                                        "computer", "Computer",
                                        "utilities-terminal-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages,
                                        build_routine_page(self),
                                        "routines", "Routines",
                                        "alarm-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages, build_task_page(self),
                                        "tasks", "Tasks",
                                        "view-list-symbolic");
    adw_view_stack_add_titled_with_icon(self->pages, build_flow_page(self),
                                        "flow", "Flow",
                                        "system-users-symbolic");

    header = adw_header_bar_new();
    title = adw_window_title_new("clawtilla", NULL);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), title);
    g_object_set_data(G_OBJECT(self), "title", title);

    sidebar_button = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(sidebar_button),
                             "sidebar-show-symbolic");
    adw_header_bar_pack_start(ADW_HEADER_BAR(header), sidebar_button);

    /*
     * Which daemon, to the left of the title.
     *
     * A popover holding plain buttons rather than a GMenu, because the
     * entries are the contents of a file that changes while the app is
     * running -- and rather than a GtkListBox, which selects a row when
     * it takes focus and would therefore connect to the first daemon in
     * the list the moment the menu opened.
     */
    {
        GtkWidget *label = adw_button_content_new();
        GtkWidget *popover = gtk_popover_new();
        GtkWidget *scroll = gtk_scrolled_window_new();

        adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(label),
                                         "network-server-symbolic");
        adw_button_content_set_label(ADW_BUTTON_CONTENT(label), "Local");

        self->connection_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_size_request(self->connection_list, 240, -1);

        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll),
                                      self->connection_list);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                       GTK_POLICY_NEVER,
                                       GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height(
            GTK_SCROLLED_WINDOW(scroll), 400);
        gtk_scrolled_window_set_propagate_natural_height(
            GTK_SCROLLED_WINDOW(scroll), TRUE);

        gtk_popover_set_child(GTK_POPOVER(popover), scroll);

        self->connection_button = gtk_menu_button_new();
        gtk_menu_button_set_child(GTK_MENU_BUTTON(self->connection_button),
                                  label);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(self->connection_button),
                                    popover);
        gtk_widget_set_tooltip_text(self->connection_button,
                                    "Which daemon this window is showing");
        adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                                  self->connection_button);
    }

    /*
     * Alerts take the right-hand side, inside the existing split's
     * content rather than wrapping it.
     *
     * Opening alerts must not hide the agent list: that is navigation,
     * and losing your place while reading a notice about something else
     * is the same failure as a toast covering the composer.  Right
     * rather than a second pane in the left sidebar, because one
     * collapsible surface holding two unrelated things means opening
     * either hides the other.
     *
     * 320px is the conventional libadwaita sidebar width and is
     * load-bearing: 281 (agent list) + 600 (transcript clamp) + 320
     * comes to 1201, so all three fit side by side on an ordinary window
     * and the transcript does not move when the panel opens.  That is
     * the whole point of pushing rather than overlaying, and it is where
     * the breakpoint below comes from.
     */
    self->alerts_split = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar_position(self->alerts_split,
                                                GTK_PACK_END);
    adw_overlay_split_view_set_sidebar(self->alerts_split,
                                       build_alerts_panel(self));
    adw_overlay_split_view_set_content(self->alerts_split,
                                       GTK_WIDGET(self->pages));
    adw_overlay_split_view_set_show_sidebar(self->alerts_split, FALSE);
    adw_overlay_split_view_set_sidebar_width_fraction(self->alerts_split,
                                                      0.26);

    switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), self->pages);
    adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher),
                                 ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_pack_end(ADW_HEADER_BAR(header), switcher);

    /*
     * The bell, at the header's end, bound to the alerts panel exactly
     * as the sidebar button is bound to the agent list.
     *
     * The count is a GtkOverlay carrying a small rounded label, because
     * libadwaita has no badge widget -- the same pill the sidebar's
     * unread count uses, so the two read as the same kind of number.
     */
    {
        GtkWidget *bell = gtk_toggle_button_new();
        GtkWidget *overlay = gtk_overlay_new();

        gtk_button_set_icon_name(
            GTK_BUTTON(bell), "preferences-system-notifications-symbolic");
        gtk_widget_set_tooltip_text(bell, "What has happened");

        self->alerts_badge = gtk_label_new("");
        gtk_widget_add_css_class(self->alerts_badge, "caption");
        gtk_widget_add_css_class(self->alerts_badge, "clawt-unread-badge");
        gtk_widget_set_halign(self->alerts_badge, GTK_ALIGN_END);
        gtk_widget_set_valign(self->alerts_badge, GTK_ALIGN_START);
        gtk_widget_set_can_target(self->alerts_badge, FALSE);
        gtk_widget_set_visible(self->alerts_badge, FALSE);

        gtk_overlay_set_child(GTK_OVERLAY(overlay), bell);
        gtk_overlay_add_overlay(GTK_OVERLAY(overlay), self->alerts_badge);

        g_object_bind_property(bell, "active", self->alerts_split,
                               "show-sidebar",
                               G_BINDING_BIDIRECTIONAL |
                               G_BINDING_SYNC_CREATE);

        adw_header_bar_pack_end(ADW_HEADER_BAR(header), overlay);
    }

    content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(content), header);
    gtk_box_append(GTK_BOX(content), GTK_WIDGET(self->alerts_split));
    gtk_widget_set_vexpand(GTK_WIDGET(self->alerts_split), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self->pages), TRUE);

    self->split = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar(self->split, sidebar_box);
    adw_overlay_split_view_set_content(self->split, content);

    g_signal_connect(self->alerts_split, "notify::show-sidebar",
                     G_CALLBACK(on_alerts_shown), self);
    g_signal_connect(self->alerts_split, "notify::collapsed",
                     G_CALLBACK(on_alerts_collapsed), self);
    g_signal_connect(self->split, "notify::show-sidebar",
                     G_CALLBACK(on_sidebar_shown), self);
    g_signal_connect(self->split, "notify::collapsed",
                     G_CALLBACK(on_sidebar_collapsed), self);

    g_object_bind_property(sidebar_button, "active", self->split,
                           "show-sidebar",
                           G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);

    /*
     * Whether the Chat tab carries its badge depends on which page is up
     * and whether the sidebar is a drawer, so both have to be watched --
     * neither changes the counts themselves, and without this the badge
     * is right only until somebody switches page.
     */
    g_signal_connect_swapped(self->pages, "notify::visible-child-name",
                             G_CALLBACK(update_unread_tab), self);
    g_signal_connect_swapped(self->split, "notify::collapsed",
                             G_CALLBACK(update_unread_tab), self);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sidebar_button), TRUE);

    /*
     * Below 800px the sidebar becomes a drawer.  Without this the agent
     * list eats a phone-sized window and the conversation has nowhere to
     * go.
     */
    breakpoint = adw_breakpoint_new(
        adw_breakpoint_condition_parse("max-width: 800px"));
    adw_breakpoint_add_setters(breakpoint, G_OBJECT(self->split),
                               "collapsed", TRUE, NULL);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(self),
                                          breakpoint);

    /*
     * And below 1200px the alerts panel overlays rather than pushes.
     *
     * The number is derived rather than chosen: 281 for the agent list,
     * 600 for the transcript's clamp and 320 for the panel come to 1201,
     * so above it all three fit and the transcript does not move when
     * the panel opens.  Below it something has to give, and the reader's
     * place in the conversation is the wrong thing to take.
     */
    breakpoint = adw_breakpoint_new(
        adw_breakpoint_condition_parse("max-width: 1200px"));
    adw_breakpoint_add_setters(breakpoint, G_OBJECT(self->alerts_split),
                               "collapsed", TRUE, NULL);
    adw_application_window_add_breakpoint(ADW_APPLICATION_WINDOW(self),
                                          breakpoint);

    /*
     * Both remembered choices start from what the widgets have actually
     * been left showing, rather than from a constant repeated here.
     *
     * Taken deliberately at the end of construction rather than left to
     * the notifies above, which only give the right answer while the
     * connects sit above the bind and the toggle's set_active() below
     * it.  Reorder those three and sidebar_open stays FALSE while the
     * list is visibly shown -- after which the first crossing of 800px
     * makes the agent list disappear, with nothing in the diff that
     * looks wrong.
     */
    self->sidebar_open = adw_overlay_split_view_get_show_sidebar(self->split);
    self->alerts_open = adw_overlay_split_view_get_show_sidebar(
        self->alerts_split);

    self->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->toasts, GTK_WIDGET(self->split));

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(self->toasts));

    g_signal_connect(client, "event", G_CALLBACK(on_daemon_event), self);

    /*
     * Applied before anything is shown, so the window is drawn once at
     * the right size rather than redrawn a frame later.
     */
    {
        g_autoptr(GError) appearance_error = NULL;

        self->appearance = clawt_appearance_load(NULL, &appearance_error);

        if (self->appearance == NULL) {
            g_warning("appearance: %s", appearance_error->message);
            self->appearance = clawt_appearance_new();
        }

        apply_appearance(self->appearance);
    }

    reload_connections(self);
    update_connection_label(self);

    refresh_agents(self);
    refresh_routines(self);

    return self;
}

static void
clawt_window_dispose(GObject *object)
{
    ClawtWindow *self = CLAWT_WINDOW(object);

    if (self->client != NULL)
        g_signal_handlers_disconnect_by_data(self->client, self);

    g_clear_object(&self->client);

    /*
     * A popover parented to a widget outlives that widget's own dispose
     * and GTK complains about it at teardown; unparenting is the
     * documented way to hand it back.
     */
    g_clear_pointer(&self->agent_menu, gtk_widget_unparent);
    g_clear_object(&self->agent_actions);
    g_clear_object(&self->agent_menu_teams);
    g_clear_pointer(&self->teams_seen, json_node_unref);

    g_clear_pointer(&self->appearance, clawt_appearance_free);
    g_clear_pointer(&self->connections, g_ptr_array_unref);
    g_clear_pointer(&self->active_connection, clawt_connection_free);
    g_clear_pointer(&self->local_socket, g_free);
    g_clear_pointer(&self->team_ids, g_strfreev);
    g_clear_pointer(&self->collapsed_teams, g_hash_table_unref);

    g_clear_pointer(&self->selected_agent, g_free);
    g_clear_pointer(&self->selected_room, g_free);
    g_clear_pointer(&self->flow_room, g_free);
    g_clear_pointer(&self->run_sender, g_free);
    g_clear_pointer(&self->run_day, g_free);
    g_clear_pointer(&self->flow_run_sender, g_free);
    g_clear_pointer(&self->flow_run_day, g_free);
    g_clear_pointer(&self->selected_avatar, g_free);
    g_clear_pointer(&self->selected_color, g_free);
    g_clear_pointer(&self->shown, g_hash_table_unref);
    g_clear_pointer(&self->drafts, g_hash_table_unref);
    g_clear_pointer(&self->unread, g_hash_table_unref);
    g_clear_pointer(&self->dm_rooms, g_hash_table_unref);
    g_clear_pointer(&self->alerts, g_ptr_array_unref);
    g_clear_pointer(&self->alerts_agent, g_free);
    g_clear_pointer(&self->pending, g_ptr_array_unref);


    G_OBJECT_CLASS(clawt_window_parent_class)->dispose(object);
}

static void
clawt_window_finalize(GObject *object)
{
    ClawtWindow *self = CLAWT_WINDOW(object);

    g_free(self->selected_agent);
    g_free(self->inspector_computer);
    g_clear_pointer(&self->settings_bars, g_hash_table_unref);
    g_clear_pointer(&self->settings_catalog, json_node_unref);

    /*
     * The records only, not the widgets: those belong to the group they
     * were added to, which GTK has already taken apart.
     */
    g_clear_pointer(&self->schema_rows, g_ptr_array_unref);

    G_OBJECT_CLASS(clawt_window_parent_class)->finalize(object);
}

static void
clawt_window_class_init(ClawtWindowClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = clawt_window_dispose;
    object_class->finalize = clawt_window_finalize;
}

static void
clawt_window_init(ClawtWindow *self)
{
    set_following(self, TRUE);
    self->connections = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_connection_free);
    self->schema_rows = g_ptr_array_new_with_free_func(schema_row_free);
    self->shown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        NULL);
    self->drafts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         g_free);
    self->unread = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         NULL);
    self->connected_at = g_get_real_time();
    self->alerts = g_ptr_array_new_with_free_func(alert_free);
    self->dm_rooms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           g_free);
    self->pending = g_ptr_array_new_with_free_func(
        (GDestroyNotify)attachment_free);

    /*
     * Holds a reference to each bar, so a progress event arriving while
     * the list is being rebuilt finds a live widget rather than one the
     * container has already dropped.
     */
    self->settings_bars = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_object_unref);
}
