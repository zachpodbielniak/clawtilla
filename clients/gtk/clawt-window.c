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

#include "clawt-window-private.h"

/* Defined below; the inspector and the create dialog both use them. */
static JsonObject  *chooser_provider(ModelChooser *chooser);
static void         refresh_settings_images(ClawtWindow *self);
static gchar *      human_size(gint64 bytes);
static void         on_import_agent(GtkButton *button,
                                    gpointer   user_data);
static void         model_chooser_build_full(ModelChooser *chooser,
                                             ClawtWindow  *window,
                                             GtkWidget    *group,
                                             const gchar  *want_provider,
                                             const gchar  *want_model,
                                             const gchar  *require);
static void         on_image_changed(GObject    *object,
                                     GParamSpec *pspec,
                                     gpointer    user_data);

G_DEFINE_FINAL_TYPE(ClawtWindow, clawt_window, ADW_TYPE_APPLICATION_WINDOW)

/*
 * Returns %TRUE when the caller owns the rebuild.  A nested call notes
 * that the data changed under it and returns; the owner runs the body
 * again on the way out, so the last word still belongs to the daemon.
 */
gboolean
clawt_gtk_refresh_enter(ClawtWindow *self, ClawtRefreshKind kind)
{
    if (self->refreshing[kind]) {
        self->refresh_again[kind] = TRUE;
        return FALSE;
    }

    self->refreshing[kind] = TRUE;
    return TRUE;
}

/* Returns %TRUE when the body must run once more. */
gboolean
clawt_gtk_refresh_repeat(ClawtWindow *self, ClawtRefreshKind kind)
{
    if (!self->refresh_again[kind]) {
        self->refreshing[kind] = FALSE;
        return FALSE;
    }

    self->refresh_again[kind] = FALSE;
    return TRUE;
}

/* ── Talking to the daemon ───────────────────────────────────────── */

void
clawt_window_toast(ClawtWindow *self, const gchar *text)
{
    AdwToastOverlay *where;

    g_return_if_fail(CLAWT_IS_WINDOW(self));

    /*
     * Which overlay depends on what is on screen, so all 87 callers stay
     * as they are.  Chat is the only page with something a toast must
     * not cover: everything else it hides can be scrolled back to, while
     * the composer holds text that has not been sent anywhere yet.
     */
    where = (clawt_gtk_current_page(self) == CLAWT_PAGE_CHAT &&
             self->toasts != NULL)
                ? self->toasts : self->page_toasts;

    if (where == NULL)
        return;

    /*
     * The same sentence twice while the first is still up is one
     * sentence.  A polled request that keeps failing raises a toast per
     * refresh -- the screen panel did, once a second for as long as a VM
     * took to boot -- and a stack of identical bars covers the controls
     * underneath while telling nobody anything the first did not.
     *
     * Checked here rather than at the 87 call sites, because which of
     * them will be reached in a loop next is not knowable from any one
     * of them.
     */
    {
        gint64 now = g_get_monotonic_time();

        if (!clawt_toast_should_show(self->last_toast, self->last_toast_at,
                                     text, now))
            return;

        g_free(self->last_toast);
        self->last_toast = g_strdup(text);
        self->last_toast_at = now;
    }

    adw_toast_overlay_add_toast(where, adw_toast_new(text));
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
         * Every failure is surfaced -- except the one the banner is
         * already holding open.  A request that quietly did nothing is
         * the worst outcome here: the interface would look like it had
         * worked and the fleet would disagree.
         *
         * "Not connected" is the exception because it is not an answer
         * to anything somebody just did: it is a condition the window is
         * *in*, said once and permanently by the banner.  A window
         * opened against a daemon that is not running makes a dozen
         * requests before it has drawn anything, so toasting each would
         * stack a dozen copies of one sentence over the one control
         * that leads out of it.  Same split as the alerts panel: arrived
         * on its own goes to the persistent surface, answers what you
         * just did stays a toast.
         */
        if (!g_error_matches(error, CLAWT_ERROR, CLAWT_ERROR_NOT_CONNECTED))
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
void
clawt_gtk_set_row_text(GtkWidget *row, const gchar *title, const gchar *subtitle)
{
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  title != NULL ? title : "");

    if (subtitle != NULL)
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
}

/*
 * One explanatory line under a row, whichever kind of row it is.
 *
 * ADW_ACTION_ROW() is a runtime cast, so calling an AdwActionRow method
 * on a row that is not one compiles, logs two criticals and sets
 * nothing.  AdwSwitchRow and AdwComboRow derive from AdwActionRow;
 * AdwEntryRow and AdwExpanderRow derive from AdwPreferencesRow and do
 * not -- and the four are used side by side in the same dialogs.
 *
 * Twenty of these had accumulated across the integration, routine and
 * trigger editors, each one a sentence that had never been on screen.
 * The one that cost most said a credential field takes a *reference*
 * (`env:NAME`, `file:PATH`) rather than a literal token, so an operator
 * reading no hint pastes the token in plain text.
 *
 * An AdwEntryRow has no subtitle slot at all -- there is no
 * adw_entry_row_set_subtitle() to reach for -- so its hint becomes a
 * tooltip.  That is less than a subtitle and more than the nothing it
 * was, and it is in one place, so a better slot changes one function.
 */
void
clawt_gtk_set_row_hint(GtkWidget *row, const gchar *hint)
{
    g_return_if_fail(GTK_IS_WIDGET(row));

    if (hint == NULL)
        return;

    if (ADW_IS_ACTION_ROW(row)) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(row), hint);
        return;
    }

    if (ADW_IS_EXPANDER_ROW(row)) {
        adw_expander_row_set_subtitle(ADW_EXPANDER_ROW(row), hint);
        return;
    }

    gtk_widget_set_tooltip_text(row, hint);
}

GtkWidget *
clawt_gtk_badge(const gchar *text, const gchar *css_class, const gchar *tooltip)
{
    GtkWidget *label = gtk_label_new(text);

    gtk_widget_add_css_class(label, "caption");
    /*
     * `clawt-badge` is what makes it bold, and the rule's own comment
     * says why that is not a style choice: at this size the horizontal
     * stems of a T or an F cover less than a pixel and fade out of a
     * coloured caption entirely.
     */
    gtk_widget_add_css_class(label, "clawt-badge");
    gtk_widget_add_css_class(label, css_class);
    gtk_widget_set_tooltip_text(label, tooltip);

    return label;
}

const gchar *
clawt_gtk_tone_class(const gchar *tone)
{
    /*
     * Presentation, so it lives here: the *decision* about what a state
     * means is clawt_task_state_tone()'s, in the library, where both
     * clients read it and a test can cover it without a window. This end
     * only says what libadwaita calls each tone.
     *
     * An unknown tone is dim rather than an assertion. It can only come
     * from a library newer than this build, and a badge in the default
     * grey is a better answer there than a critical.
     */
    if (g_strcmp0(tone, "good") == 0)
        return "success";
    if (g_strcmp0(tone, "warn") == 0)
        return "warning";
    if (g_strcmp0(tone, "bad") == 0)
        return "error";
    if (g_strcmp0(tone, "info") == 0)
        return "accent";

    return "dim-label";
}

void
clawt_gtk_clear_list(GtkListBox *list)
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

void
clawt_gtk_clear_box(GtkBox *box)
{
    GtkWidget *child;

    while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
        gtk_box_remove(box, child);
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
GtkWidget *
clawt_gtk_row_opens_something(GtkWidget *row)
{
    GtkWidget *chevron = gtk_image_new_from_icon_name("go-next-symbolic");

    adw_action_row_add_suffix(ADW_ACTION_ROW(row), chevron);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), chevron);

    return row;
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

    clawt_gtk_build_inspector(self, agent, clawt_payload_of(reply));
    clawt_gtk_refresh_computer(self, agent);
    clawt_gtk_refresh_mailbox(self);
    clawt_gtk_refresh_tasks(self);
    clawt_gtk_refresh_decisions(self);
    clawt_gtk_refresh_routines(self);
    clawt_gtk_refresh_flow(self);
}

void
clawt_gtk_refresh_selected(ClawtWindow *self)
{
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_SELECTED))
        return;

    do {
        refresh_selected_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_SELECTED));
}

/*
 * Writes one composer's contents to the client's own config, so a
 * half-typed message survives the client being closed.
 *
 * Keyed through clawt_draft_key(), which puts the connection profile in
 * front of the agent id: this client switches daemons at runtime, and
 * two fleets can each hold an agent called `chief`.
 *
 * Errors are dropped rather than reported.  A draft is a convenience,
 * and a dialog about a config file somebody has never heard of, raised
 * while they are clicking between agents, is worse than losing the text.
 */
void
clawt_gtk_persist_draft(ClawtWindow *self, const gchar *agent_id,
                        const gchar *text)
{
    g_autofree gchar *key = NULL;

    if (agent_id == NULL)
        return;

    key = clawt_draft_key(
        self->active_connection != NULL
            ? clawt_connection_get_name(self->active_connection) : NULL,
        agent_id);

    clawt_draft_store_set(NULL, key, text, NULL);
}

/*
 * And what was left there last time.
 *
 * Returns: (transfer full) (nullable): the held text
 */
gchar *
clawt_gtk_stored_draft(ClawtWindow *self, const gchar *agent_id)
{
    g_autofree gchar *key = NULL;

    if (agent_id == NULL)
        return NULL;

    key = clawt_draft_key(
        self->active_connection != NULL
            ? clawt_connection_get_name(self->active_connection) : NULL,
        agent_id);

    return clawt_draft_store_get(NULL, key);
}

void
clawt_gtk_select_room(ClawtWindow *self, const gchar *room_id)
{
    if (room_id == NULL || *room_id == '\0')
        return;

    if (g_strcmp0(room_id, self->selected_room_entry) == 0) {
        /*
         * The same stuck-view retry clawt_gtk_select_agent() does, for
         * the same reason: a transcript with no room lost its history
         * load, and a re-click is what a person actually tries.
         */
        if (self->selected_room == NULL && self->history_inflight == 0)
            clawt_gtk_load_history(self);
        return;
    }

    /* Keep what was being written wherever we are leaving. */
    if (self->selected_agent != NULL || self->selected_room_entry != NULL) {
        const gchar *leaving = (self->selected_room_entry != NULL)
            ? self->selected_room_entry : self->selected_agent;
        g_autofree gchar *draft = clawt_gtk_entry_text(self);

        clawt_gtk_persist_draft(self, leaving, draft);

        if (draft != NULL && draft[0] != '\0')
            g_hash_table_insert(self->drafts, g_strdup(leaving),
                                g_steal_pointer(&draft));
        else
            g_hash_table_remove(self->drafts, leaving);
    }

    /*
     * A room is not an agent, and the two selections are exclusive.
     *
     * Clearing the agent is what makes every agent-scoped page -- the
     * computer, the mailbox, its memories -- stop describing somebody
     * who is not on screen.  A room has none of those, so it shows the
     * chat and nothing else.
     */
    g_clear_pointer(&self->selected_agent, g_free);
    g_clear_pointer(&self->selected_conversation, g_free);

    g_free(self->selected_room_entry);
    self->selected_room_entry = g_strdup(room_id);

    self->selected_has_avatar = FALSE;
    g_clear_pointer(&self->selected_color, g_free);
    self->selected_can_interrupt = FALSE;
    clawt_gtk_sync_stop_turn(self, FALSE);

    if (g_hash_table_remove(self->unread, room_id))
        clawt_gtk_update_unread_tab(self);

    {
        const gchar *held = g_hash_table_lookup(self->drafts, room_id);
        g_autofree gchar *stored = NULL;

        if (held == NULL) {
            stored = clawt_gtk_stored_draft(self, room_id);
            held = stored;
        }

        clawt_gtk_entry_set_text(self, held);
    }

    adw_window_title_set_title(
        ADW_WINDOW_TITLE(g_object_get_data(G_OBJECT(self), "title")),
        room_id);

    clawt_gtk_load_history(self);
    clawt_gtk_fill_conversation_menu(self);
    clawt_gtk_refresh_selected(self);

    if (adw_overlay_split_view_get_collapsed(self->split)) {
        self->sidebar_transient = TRUE;
        adw_overlay_split_view_set_show_sidebar(self->split, FALSE);
        self->sidebar_transient = FALSE;
    }
}

void
clawt_gtk_select_agent(ClawtWindow *self, const gchar *agent_id)
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
     *
     * One exception: a transcript with no room lost its history load (a
     * failure, or a superseded call), and a re-click is the gesture a
     * person actually tries against a stuck view.  Retrying here is safe
     * where a blanket reload is not, because it stops the moment a load
     * succeeds, and the inflight guard keeps the sidebar-rebuild path
     * from nesting a second request inside the first one's wait.
     */
    if (g_strcmp0(agent_id, self->selected_agent) == 0) {
        if (self->selected_agent != NULL && self->selected_room == NULL &&
            self->history_inflight == 0)
            clawt_gtk_load_history(self);
        return;
    }

    /* Keep what was being written to the agent we are leaving. */
    if (self->selected_agent != NULL) {
        g_autofree gchar *draft = clawt_gtk_entry_text(self);

        clawt_gtk_persist_draft(self, self->selected_agent, draft);

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
     * And no longer a room.  The two selections are exclusive, and a
     * stale room entry would keep the sidebar highlighting a row
     * nobody is looking at.
     */
    g_clear_pointer(&self->selected_room_entry, g_free);

    /*
     * A different agent opens on its operator conversation. Keeping the
     * peer would mean clicking an agent and landing in a conversation
     * between two others -- and "with gnuisaince" means a different room
     * for every agent it is shown under.
     */
    g_clear_pointer(&self->selected_conversation, g_free);

    /*
     * Opening a conversation is the only thing that clears its count.
     *
     * Not scrolling, not the window gaining focus, not time passing: a
     * counter that decays on its own is a counter you stop trusting.
     */
    {
        /*
         * By the room, because that is what the count is against now --
         * a group has no agent to key on, and every row has a room.
         */
        g_autofree gchar *room = clawt_room_manager_direct_id("user",
                                                              agent_id);

        if (g_hash_table_remove(self->unread, room))
            clawt_gtk_update_unread_tab(self);
    }

    {
        const gchar *has_avatar =
            clawt_gtk_agent_row_data(self, agent_id, "agent-has-avatar");

        self->selected_has_avatar = (has_avatar != NULL && *has_avatar != '\0');
    }
    g_free(self->selected_color);
    self->selected_color = g_strdup(clawt_gtk_agent_row_data(self, agent_id,
                                                             "agent-color"));

    {
        const gchar *held = g_hash_table_lookup(self->drafts, agent_id);
        g_autofree gchar *stored = NULL;

        /*
         * Memory first, then the file.  Within one run the table is
         * authoritative -- it is what a draft cleared by sending was
         * removed from -- and the file is what makes the first visit
         * after a restart find the text still there.
         */
        if (held == NULL) {
            stored = clawt_gtk_stored_draft(self, agent_id);
            held = stored;
        }

        clawt_gtk_entry_set_text(self, held);
    }

    adw_window_title_set_title(
        ADW_WINDOW_TITLE(g_object_get_data(G_OBJECT(self), "title")),
        agent_id);

    clawt_gtk_load_history(self);
    clawt_gtk_fill_conversation_menu(self);
    clawt_gtk_refresh_selected(self);

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
    alert->read = clawt_alert_arrives_read(
        self->alerts_split != NULL &&
            adw_overlay_split_view_get_show_sidebar(self->alerts_split),
        alert->tier);

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

static gchar *
alert_when(gint64 ts)
{
    return clawt_time_ago_label(ts, g_get_real_time());
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

    clawt_gtk_clear_list(self->alerts_list);

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

    gtk_widget_set_name(view, "clawt-alerts-panel");

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
            !clawt_gtk_already_shown(self, clawt_event_get_detail(event, "id"))) {
            /*
             * Before the message, so the rule sits above the first
             * thing the reader has not seen rather than below it.
             */
            clawt_gtk_note_arrival(self);

            clawt_gtk_append_message(self, from != NULL ? from : "?",
                                     body != NULL ? body : "",
                                     g_strcmp0(from, "user") == 0, 0);

            /*
             * The reply is the end of the turn.  libreclaw drops the
             * typing indicator too, but the message overtakes it often
             * enough that relying on the indicator alone leaves a
             * spinner running under an answer that has already arrived.
             */
            if (g_strcmp0(from, self->selected_agent) == 0)
                clawt_gtk_set_activity(self, NULL);

            clawt_gtk_queue_scroll(self);
        } else if (self->history_inflight == 0 &&
                   self->selected_conversation == NULL &&
                   self->selected_agent != NULL &&
                   clawt_event_get_subject(event) != NULL &&
                   g_strcmp0(clawt_event_get_subject(event),
                             self->selected_room) != 0 &&
                   g_strcmp0(g_hash_table_lookup(
                                 self->dm_rooms,
                                 clawt_event_get_subject(event)),
                             self->selected_agent) == 0) {
            /*
             * The daemon says this room is the selected agent's own
             * conversation -- the one on screen -- and the transcript's
             * room disagrees.  That is the transcript being wrong, not
             * the message: a failed or superseded history load leaves
             * selected_room unset or stale, and nothing else corrects
             * it.  Reloading resolves the room afresh and the history
             * it returns already contains this message, so nothing is
             * lost and the replay below deduplicates.  Ordered before
             * note_unread so a message now on screen is not also
             * counted as waiting.
             */
            clawt_gtk_load_history(self);
        }

        /*
         * Anything that was not for the room on screen counts.  Done
         * before the refresh below, because that is what draws the pill.
         */
        clawt_gtk_note_unread(self, event, from);

        /*
         * The flow page is refreshed for every message, not only the one
         * on screen: it is a list of what the fleet has been doing, and
         * a conversation that does not move when the agents talk is the
         * one thing it must not be.
         */
        clawt_gtk_refresh_flow(self);
        clawt_gtk_refresh_agents(self);
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
        clawt_gtk_refresh_flow(self);
        return;
    }

    if (g_strcmp0(kind, "turn.step") == 0) {
        g_autoptr(ClawtTurnStep) step = clawt_turn_step_new_from_event(event);

        /*
         * Only the conversation on screen.  An agent runs a turn per
         * room and can be mid-turn in three at once, so the room is
         * checked rather than the agent -- drawing a peer exchange's
         * tool calls under the operator's own chat would read as the
         * agent doing something nobody asked it to.
         */
        if (step != NULL && self->selected_room != NULL &&
            g_strcmp0(clawt_turn_step_get_room_id(step),
                      self->selected_room) == 0)
            clawt_gtk_steps_add(self, step);

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

            clawt_gtk_set_activity(self, what);

            /*
             * And the steps of the turn that just ended are sealed
             * where they are, not removed.
             *
             * They used to be removed, so a turn's tool calls vanished
             * the moment its answer appeared -- which took away the
             * working and left the conclusion, and read as text being
             * deleted from a conversation somebody was reading,
             * because it was.  The next turn starts a block of its own
             * underneath.
             */
            if (g_strcmp0(typing, "true") != 0)
                clawt_gtk_steps_seal(self);

            /*
             * And the button that ends it.  The caps come from the
             * listing rather than the event, so this asks the row we
             * already drew rather than inventing a second answer.
             */
            clawt_gtk_sync_stop_turn(self, g_strcmp0(typing, "true") == 0);
        }

        /*
         * And the sidebar, which is where somebody looks to see whether
         * anything anywhere is happening.
         */
        clawt_gtk_refresh_agents(self);
        return;
    }

    if (g_str_has_prefix(kind, "agent.") || g_str_has_prefix(kind, "mailbox.")) {
        /*
         * `agent.changed` is what agent.avatar_set and agent.avatar_clear
         * publish, among other things -- so a cached "here is its face"
         * or "it has none" answer might now be wrong. A NULL subject
         * (agent.reorder touches the whole fleet) drops every entry
         * rather than guessing which agent moved.
         */
        if (g_strcmp0(kind, "agent.changed") == 0)
            clawt_gtk_avatar_invalidate(clawt_event_get_subject(event));

        clawt_gtk_refresh_agents(self);
        clawt_gtk_refresh_mailbox(self);
        return;
    }

    /*
     * A room was made, renamed, moved or had its members changed.
     *
     * The sidebar draws rooms beside the agents, so it has to redraw --
     * and this is the one that arrives from *another* client, or from
     * the CLI, which is exactly when nothing else on screen would have
     * prompted it.  `room.created` and `room.changed` were published
     * from the day rooms existed and nothing had ever subscribed.
     */
    if (g_str_has_prefix(kind, "room.")) {
        clawt_gtk_refresh_agents(self);
        return;
    }

    /*
     * A new frame, or a screen that changed hands. Both redraw the
     * Screen tab; neither touches anything else, so this is deliberately
     * not folded into the agent refresh above.
     */
    if (g_str_has_prefix(kind, "computer.")) {
        clawt_gtk_refresh_screen(self);
        return;
    }

    if (g_str_has_prefix(kind, "task."))
        clawt_gtk_refresh_tasks(self);

    /*
     * An agent filing one has to reach a window that is already open,
     * or the badge only appears to somebody who happened to switch
     * pages -- which is the whole difference between a surface and a
     * page nobody looks at.
     */
    if (g_str_has_prefix(kind, "decision."))
        clawt_gtk_refresh_decisions(self);

    /*
     * `routine.ran` is published when one starts, so the list shows the
     * new "last run" without anybody reopening the page.
     */
    if (g_str_has_prefix(kind, "routine."))
        clawt_gtk_refresh_routines(self);

    /*
     * One prefix match covers trigger.fired, .changed, .refused and
     * .verified -- and the last two are the ones somebody is waiting on
     * while they point a forge at a new endpoint.
     */
    if (g_str_has_prefix(kind, "trigger."))
        clawt_gtk_refresh_triggers(self);

    /*
     * A skill enabled, imported or removed changes both the page and
     * what `/` offers, and the cached command list is the half that
     * would otherwise stay right until the window was reopened.
     */
    if (g_str_has_prefix(kind, "skill.")) {
        clawt_gtk_skill_commands_forget(self);
        clawt_gtk_refresh_skills(self);
    }
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
    guint selected =
        adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->computer_row));
    const gchar *type = clawt_gtk_computer_type_nick_at(selected);

    *out_image = NULL;
    *out_disk = NULL;

    if (clawt_computer_type_takes_image(clawt_gtk_computer_type_from_nick(type)))
        *out_image = clawt_gtk_image_chooser_value(&dialog->image);
    else if (g_strcmp0(type, "vm") == 0)
        *out_disk = clawt_gtk_disk_chooser_value(&dialog->disk);

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
gchar *
clawt_gtk_chooser_model(ModelChooser *chooser)
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
const gchar *
clawt_gtk_chooser_provider_id(ModelChooser *chooser)
{
    JsonObject *provider = chooser_provider(chooser);

    return (provider != NULL) ? clawt_json_string(provider, "id", NULL)
                              : NULL;
}

/* The reference currently chosen, from the list or from the entry. */
gchar *
clawt_gtk_image_chooser_value(ImageChooser *chooser)
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
void
clawt_gtk_image_chooser_build(ImageChooser *chooser, ClawtWindow *window,
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

gchar *
clawt_gtk_disk_chooser_value(ImageChooser *chooser)
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

void
clawt_gtk_disk_chooser_build(ImageChooser *chooser, ClawtWindow *window,
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
void
clawt_gtk_model_chooser_build(ModelChooser *chooser, ClawtWindow *window,
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

    model = clawt_gtk_chooser_model(&dialog->models);
    computer = dialog_computer(dialog, &image, &disk);

    reply = clawt_window_request(
        self, "agent.create",
        clawt_build_payload(
            "id", agent_id,
            "name", clawt_gtk_answer_of(dialog->name_entry),
            "description", clawt_gtk_answer_of(dialog->description_entry),
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
    clawt_gtk_refresh_agents(self);

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
    clawt_gtk_refresh_agents(self);

    /*
     * The outer dialog closes here too.  Only the manual path used to,
     * so designing an agent left the New agent dialog sitting open over
     * a fleet that already contained it.
     */
    adw_dialog_close(new_agent->dialog);
}

/* An answer, or NULL when the row was left empty. */
const gchar *
clawt_gtk_answer_of(GtkWidget *row)
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

    purpose = clawt_gtk_answer_of(dialog->describe_entry);

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

    designer_model = clawt_gtk_chooser_model(&dialog->designer);
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
            "id", clawt_gtk_answer_of(dialog->id_entry),
            "name", clawt_gtk_answer_of(dialog->name_entry),
            "purpose", purpose,
            "boundaries", clawt_gtk_answer_of(dialog->boundaries_entry),
            "needs", clawt_gtk_answer_of(dialog->needs_entry),
            "personality", clawt_gtk_answer_of(dialog->personality_entry),
            "projects", clawt_gtk_answer_of(dialog->projects_entry),
            "notes", clawt_gtk_answer_of(dialog->notes_entry),
            "provider", clawt_gtk_chooser_provider_id(&dialog->designer),
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

    /*
     * Asked, not counted.  This was `selected == 2` with a comment
     * naming the order it assumed -- so reordering the list, or adding
     * a type before that index, silently pointed it at a different
     * backend and the image row appeared for the wrong one.
     */
    selected = adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->computer_row));
    is_container = clawt_computer_type_takes_image(
        clawt_gtk_computer_type_from_nick(clawt_gtk_computer_type_nick_at(selected)));

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

void
clawt_gtk_on_new_agent(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    NewAgentDialog *dialog = g_new0(NewAgentDialog, 1);
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *manual = adw_preferences_group_new();
    GtkWidget *ai = adw_preferences_group_new();
    GtkWidget *create;
    GtkWidget *design;

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
        GtkStringList *choices = clawt_gtk_team_choices(self, NULL, &dialog->team_ids);

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
    clawt_gtk_model_chooser_build(&dialog->models, self, manual, NULL, NULL);

    dialog->computer_row = adw_combo_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->computer_row),
                                  "Computer");
    adw_combo_row_set_model(ADW_COMBO_ROW(dialog->computer_row),
                            G_LIST_MODEL(gtk_string_list_new(
                                clawt_gtk_computer_type_nicks())));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(manual),
                              dialog->computer_row);

    /*
     * The image, shown only while "container" is the chosen computer.
     * The other backends do not read it, and a row that quietly does
     * nothing is worse than no row.
     */
    clawt_gtk_image_chooser_build(&dialog->image, self, manual, NULL);
    clawt_gtk_disk_chooser_build(&dialog->disk, self, manual, NULL);
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
    GtkWidget   *mode_row;
    GtkWidget   *url_row;
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

    /*
     * The daemon's own sentence. Whether a git import became a
     * submodule depends on the machine, so the side that found out is
     * the side that says -- a toast reading "Imported." would be true
     * of both and useful for neither.
     */
    {
        JsonObject *object = json_node_get_object(reply);
        const gchar *detail = clawt_json_string(object, "detail", NULL);

        clawt_window_toast(self, detail != NULL ? detail
                                 : "Imported. Check it over before "
                                   "starting it.");
    }
    clawt_gtk_refresh_agents(self);
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

/*
 * A git import names a URL; the other two name a directory.
 *
 * Asked of the library rather than compared against "git" here, so this
 * client cannot end up offering a folder chooser for a mode that needs
 * a URL -- which is the shape the computer types were in until the type
 * predicates replaced five copies of the same comparison.
 */
static void
on_import_mode_changed(GObject *row, GParamSpec *spec, gpointer user_data)
{
    ImportAgentDialog *dialog = user_data;
    ClawtImportMode mode;
    gboolean wants_url;

    (void)spec;

    mode = clawt_import_mode_nth(
        adw_combo_row_get_selected(ADW_COMBO_ROW(row)));
    wants_url = clawt_import_mode_takes_url(mode);

    gtk_widget_set_sensitive(dialog->url_row, wants_url);
    gtk_widget_set_sensitive(dialog->from_row, !wants_url);

    /*
     * Only a copy reads the source's .git, so the switch is meaningless
     * for the other two -- a link keeps whatever is there and a clone
     * *is* a repository.
     */
    gtk_widget_set_sensitive(dialog->keep_git_row,
                             mode == CLAWT_IMPORT_COPY);
}

static void
on_import_from_directory(GtkButton *button, gpointer user_data)
{
    ImportAgentDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    const gchar *agent_id;
    g_autoptr(JsonNode) reply = NULL;
    const gchar *source;
    ClawtImportMode mode;
    gboolean keep_git;

    (void)button;

    agent_id = gtk_editable_get_text(GTK_EDITABLE(dialog->id_entry));

    if (agent_id == NULL || *agent_id == '\0') {
        clawt_window_toast(self, "An imported agent needs an id.");
        return;
    }

    mode = clawt_import_mode_nth(
        adw_combo_row_get_selected(ADW_COMBO_ROW(dialog->mode_row)));

    if (clawt_import_mode_takes_url(mode)) {
        source = gtk_editable_get_text(GTK_EDITABLE(dialog->url_row));

        if (source == NULL || *source == '\0') {
            clawt_window_toast(self, "A git import needs a repository URL.");
            return;
        }
    } else {
        source = dialog->from_path;

        if (source == NULL) {
            clawt_window_toast(self,
                               "Choose the directory to import from.");
            return;
        }
    }

    keep_git = adw_switch_row_get_active(
        ADW_SWITCH_ROW(dialog->keep_git_row));

    reply = clawt_window_request(
        self, "agent.import",
        clawt_build_payload("id", agent_id, "from", source,
                            "mode", clawt_import_mode_nth_nick(mode),
                            "keep_git", keep_git ? "true" : "false", NULL));

    /* Left open on failure, so the path and id are still there to fix. */
    if (reply == NULL)
        return;

    clawt_window_toast(self, "Imported. Check it over before starting it.");
    clawt_gtk_refresh_agents(self);
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

    /*
     * How, and the URL the git mode needs.
     *
     * Both rows always exist and the one that does not apply is
     * insensitive rather than hidden -- a row that vanishes takes the
     * reader's place on the page with it, and the dialog is short
     * enough that there is nothing to gain by hiding it.
     */
    {
        g_autoptr(GtkStringList) labels = gtk_string_list_new(NULL);
        guint i;

        for (i = 0; i < clawt_import_mode_count(); i++)
            gtk_string_list_append(labels, clawt_import_mode_nth_label(i));

        dialog->mode_row = adw_combo_row_new();
        adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->mode_row),
                                      "How");
        adw_combo_row_set_model(ADW_COMBO_ROW(dialog->mode_row),
                                G_LIST_MODEL(labels));
        adw_combo_row_set_selected(ADW_COMBO_ROW(dialog->mode_row), 0);
        adw_preferences_group_add(ADW_PREFERENCES_GROUP(from_group),
                                  dialog->mode_row);
    }

    dialog->url_row = adw_entry_row_new();
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->url_row),
                                  "Git URL");
    gtk_widget_set_sensitive(dialog->url_row, FALSE);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(from_group),
                              dialog->url_row);

    g_signal_connect(dialog->mode_row, "notify::selected",
                     G_CALLBACK(on_import_mode_changed), dialog);

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
/*
 * Draws whatever clawt_connection_notice_text() says, or hides the
 * banner.
 *
 * The sentence itself is in libclawt, because the web client shows the
 * same one and two spellings of "is this connection lost or was it never
 * made" would differ exactly once -- on the case nobody looked at.  What
 * is left here is the widget: which state gets a button, and clearing
 * that button again.
 *
 * The button belongs to the never-connected state alone, and it is the
 * whole fix.  Every other state leaves the window usable, so the
 * connection menu in the header is route enough; this one has an empty
 * fleet behind it and nothing on screen that looks like a way out, which
 * is precisely how somebody with two saved workstations in
 * connections.yaml concludes the client cannot reach them.
 */
static void
update_connection_banner(ClawtWindow *self)
{
    ClawtDaemonLink link;
    g_autofree gchar *text = NULL;

    if (self->connection_banner == NULL)
        return;

    link = clawt_daemon_link_state(self->client, self->connected_once);
    text = clawt_connection_notice_text(link, self->active_connection,
                                        self->daemon_version,
                                        self->daemon_update);

    /*
     * Cleared unless this state wants it.  AdwBanner keeps a button
     * label until it is taken away, so a banner that grew one while
     * disconnected would still be offering it under a version-mismatch
     * message about a daemon that is right there.
     */
    adw_banner_set_button_label(
        ADW_BANNER(self->connection_banner),
        (link == CLAWT_DAEMON_LINK_NEVER) ? "Connections" : NULL);

    if (text == NULL) {
        adw_banner_set_revealed(ADW_BANNER(self->connection_banner), FALSE);
        return;
    }

    adw_banner_set_title(ADW_BANNER(self->connection_banner), text);
    adw_banner_set_revealed(ADW_BANNER(self->connection_banner), TRUE);
}

/*
 * Asks the daemon what it is, once, at connect.
 *
 * `control.status` has reported the version since the frame existed and
 * had exactly one caller in the tree -- the CLI.  Neither graphical
 * client ever sent it, so a client talking to an older or newer daemon
 * found out by a frame kind being refused, in whichever feature the
 * person happened to open, with a message about that feature.
 *
 * Cheap enough to do inline: the daemon answers it from what it already
 * holds and never leaves the machine to do so.
 */
static void
note_daemon_version(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;

    g_clear_pointer(&self->daemon_version, g_free);
    g_clear_pointer(&self->daemon_update, g_free);

    if (self->client == NULL)
        return;

    reply = clawt_window_request(self, "control.status", NULL);

    if (reply != NULL) {
        JsonObject *payload = clawt_payload_of(reply);
        JsonObject *update = NULL;

        self->daemon_version =
            g_strdup(clawt_json_string(payload, "version", NULL));

        if (json_object_has_member(payload, "update"))
            update = json_object_get_object_member(payload, "update");

        /*
         * Only when the daemon says one is available.  Reading `latest`
         * on its own would draw a banner about the version already
         * running: the comparison belongs to the daemon, where it is
         * written once, rather than to each of three clients.
         */
        if (update != NULL &&
            json_object_has_member(update, "available") &&
            json_object_get_boolean_member(update, "available"))
            self->daemon_update =
                g_strdup(clawt_json_string(update, "latest", NULL));
    }

    update_connection_banner(self);
}

/*
 * The banner's way out: open the connection menu in the header.
 *
 * Opened rather than replicated.  A second list of saved connections
 * built into a banner would be a second answer to which daemons exist,
 * and every one of those in this codebase turned out to be a bug.
 */
static void
on_banner_button(AdwBanner *banner, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)banner;

    if (self->connection_button != NULL)
        gtk_menu_button_popup(GTK_MENU_BUTTON(self->connection_button));
}

/*
 * Re-reads a window whose daemon has just appeared.
 *
 * From an idle rather than inline in ::connected, and both halves of
 * that matter.  The signal is emitted from inside clawt_client_connect(),
 * which on the retry path is called from the reconnect timeout *before*
 * it re-subscribes -- so reading here would take a snapshot and then
 * open the event stream, losing anything that happened in between.  And
 * these are synchronous requests that iterate the context, which is a
 * poor thing to do from inside a source dispatch that has more to do
 * after it.
 *
 * A reference for the life of the idle, like queue_scroll(): a window
 * closed in the same turn the daemon came back would otherwise be read
 * after it was freed.
 */
static gboolean
reload_after_connect(gpointer user_data)
{
    ClawtWindow *self = user_data;

    note_daemon_version(self);
    clawt_gtk_refresh_agents(self);
    clawt_gtk_refresh_selected(self);
    clawt_gtk_refresh_tasks(self);
    clawt_gtk_refresh_decisions(self);
    clawt_gtk_refresh_routines(self);
    clawt_gtk_refresh_skills(self);
    clawt_gtk_load_history(self);

    return G_SOURCE_REMOVE;
}

static void
on_client_connected(ClawtClient *client, gpointer user_data)
{
    ClawtWindow *self = user_data;
    gboolean first = !self->connected_once;

    (void)client;

    self->connected_once = TRUE;
    update_connection_banner(self);

    /*
     * A window that came up before its daemon has nothing on it -- no
     * agents, no version, no history -- and no event will ever fill
     * that in, because events describe what changes rather than what is.
     * So the arrival of a connection is itself the thing that has to
     * trigger the first read.
     *
     * Only the first.  An ordinary reconnect is covered by the daemon's
     * replay, and ::resync already re-reads the case where the replay
     * could not reach back far enough; re-reading every reconnect would
     * throw away the reader's place in the transcript over a blip.
     */
    if (first)
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, reload_after_connect,
                        g_object_ref(self), g_object_unref);
}

static void
on_client_disconnected(ClawtClient *client, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)client;

    update_connection_banner(self);
}

/*
 * The daemon could not replay from where this client had got to, so
 * everything on screen may be stale in ways no event will correct.
 *
 * Re-read rather than wait.  A toast rather than the banner: by the time
 * this returns the view is correct again, and a persistent notice about
 * a hole that has just been filled is a notice about nothing.
 */
static void
on_client_resync(ClawtClient *client, gpointer user_data)
{
    ClawtWindow *self = user_data;

    (void)client;

    clawt_gtk_refresh_agents(self);
    clawt_gtk_refresh_selected(self);
    clawt_gtk_load_history(self);

    clawt_window_toast(self,
                       "Reconnected after a long gap. Some events were "
                       "missed, so this has been reloaded.");
}

/* Connects the three things a window wants to know about its client. */
static void
watch_the_connection(ClawtWindow *self)
{
    if (self->client == NULL)
        return;

    g_signal_connect(self->client, "connected",
                     G_CALLBACK(on_client_connected), self);
    g_signal_connect(self->client, "disconnected",
                     G_CALLBACK(on_client_disconnected), self);
    g_signal_connect(self->client, "resync",
                     G_CALLBACK(on_client_resync), self);
}

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
    g_hash_table_remove_all(self->flow_shown);
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

    clawt_gtk_reset_transcript(self);
    clawt_gtk_clear_list(self->sidebar);
    clawt_gtk_set_activity(self, NULL);
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

    /* Connected above, before watch_the_connection() could hear it. */
    self->connected_once = clawt_client_is_connected(self->client);

    g_signal_connect(self->client, "event", G_CALLBACK(on_daemon_event),
                     self);
    watch_the_connection(self);

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

    /*
     * The version belongs to the daemon, not to the window, so it is
     * asked again here.  A banner kept from the machine somebody just
     * switched away from is worse than no banner.
     */
    note_daemon_version(self);

    clawt_gtk_refresh_agents(self);
    clawt_gtk_refresh_tasks(self);
    clawt_gtk_refresh_decisions(self);
    clawt_gtk_refresh_routines(self);

    {
        g_autofree gchar *where = clawt_connection_describe(connection);
        g_autofree gchar *message =
            g_strdup_printf("Connected to %s (%s).",
                            clawt_connection_get_name(connection), where);

        clawt_window_toast(self, message);
    }
}

/*
 * One saved connection, asked whether it is up.
 *
 * On a thread, because clawt_connection_probe() blocks until the far end
 * answers and the far end may be a laptop that is asleep -- which fails
 * only when its route times out. Answering that on the main context
 * would freeze the window for the length of every unreachable host in
 * the list, which is the failure this feature exists to avoid making
 * worse.
 */
typedef struct {
    ClawtWindow     *window;
    gchar           *name;
    ClawtConnection *connection;
} ProbeJob;

static void
probe_job_free(gpointer data)
{
    ProbeJob *job = data;

    g_clear_pointer(&job->connection, clawt_connection_free);
    g_free(job->name);
    g_free(job);
}

static void
probe_worker(GTask *task, gpointer source, gpointer data,
             GCancellable *cancellable)
{
    ProbeJob *job = data;

    (void)source;
    (void)cancellable;

    g_task_return_pointer(task, clawt_connection_probe(job->connection),
                          (GDestroyNotify)clawt_connection_status_free);
}

static void
on_connection_probed(GObject *source, GAsyncResult *result, gpointer data)
{
    ClawtWindow *self = CLAWT_WINDOW(source);
    ProbeJob *job = g_task_get_task_data(G_TASK(result));
    ClawtConnectionStatus *status =
        g_task_propagate_pointer(G_TASK(result), NULL);

    (void)data;

    if (status == NULL)
        return;

    g_hash_table_replace(self->connection_status, g_strdup(job->name),
                         status);

    /*
     * The menu is redrawn rather than the one row patched. The rows are
     * plain buttons rebuilt from the profile list every time anything
     * changes, so there is no row to hold on to -- and a pointer kept
     * across a rebuild is how a click on a stale button hands
     * switch_connection() a freed profile, which the code below already
     * takes care to avoid.
     */
    rebuild_connection_menu(self);
}

/*
 * Ask every saved connection, once, when the menu is opened.
 *
 * Here rather than on a timer: a probe nobody is looking at is a network
 * call nobody asked for, and the answer is only interesting at the
 * moment somebody is deciding where to go.
 */
static void
probe_saved_connections(ClawtWindow *self)
{
    guint i;

    for (i = 0; i < self->connections->len; i++) {
        ClawtConnection *connection = g_ptr_array_index(self->connections, i);
        ProbeJob *job = g_new0(ProbeJob, 1);
        g_autoptr(GTask) task = NULL;

        job->window = self;
        job->name = g_strdup(clawt_connection_get_name(connection));

        /*
         * Its own copy, for the same reason the row's button takes one:
         * the list behind it is rebuilt whenever the editor saves, and a
         * probe outliving that by one round trip would be reading a
         * freed profile on a thread.
         */
        job->connection = clawt_connection_copy(connection);

        task = g_task_new(self, NULL, on_connection_probed, NULL);
        g_task_set_task_data(task, job, probe_job_free);
        g_task_run_in_thread(task, probe_worker);
    }
}

static void
on_connection_menu_shown(GtkWidget *popover, gpointer user_data)
{
    (void)popover;

    probe_saved_connections(CLAWT_WINDOW(user_data));
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

        /*
         * Whether that machine is up, as far as anybody has asked.
         *
         * Every entry used to be drawn identically whether the daemon
         * behind it was running, stopped, unreachable or holding a token
         * that no longer matched -- and the only way to find out was to
         * switch to it and fail.
         *
         * "Refused" and "unreachable" are deliberately different words:
         * one is a credential problem and the other is a network one,
         * and sending somebody to check the wrong one costs far more
         * than the probe does.
         */
        {
            ClawtConnectionStatus *status = g_hash_table_lookup(
                self->connection_status,
                clawt_connection_get_name(connection));
            ClawtReachability reach = (status != NULL)
                                      ? status->reach : CLAWT_REACH_UNKNOWN;
            GtkWidget *verdict = gtk_label_new(
                clawt_reachability_word(reach));

            gtk_widget_add_css_class(verdict, "caption");
            gtk_widget_add_css_class(
                verdict, (reach == CLAWT_REACH_REACHABLE) ? "success"
                       : (reach == CLAWT_REACH_UNKNOWN) ? "dim-label"
                       : "error");
            gtk_widget_set_valign(verdict, GTK_ALIGN_CENTER);

            /*
             * The version and the agent count come back on the same
             * reply, so the tooltip costs nothing extra -- and the
             * version is the one thing nobody was comparing across the
             * link at all.
             */
            if (status != NULL && status->reach == CLAWT_REACH_REACHABLE) {
                g_autofree gchar *tip = g_strdup_printf(
                    "clawtillad %s, %u agent%s",
                    status->version != NULL ? status->version : "?",
                    status->agents, status->agents == 1 ? "" : "s");

                gtk_widget_set_tooltip_text(verdict, tip);
            } else if (status != NULL && status->detail != NULL) {
                gtk_widget_set_tooltip_text(verdict, status->detail);
            }

            gtk_box_append(GTK_BOX(row), verdict);
        }

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
/*
 * The New room item on the + button's menu.
 *
 * A room is made the same way an agent is -- from the one control that
 * adds things to the sidebar -- rather than from a separate corner, so
 * somebody looking for "how do I make one of these" finds both in the
 * same place.
 */
static void
on_new_room_activate(GSimpleAction *action, GVariant *parameter,
                     gpointer user_data)
{
    (void)action;
    (void)parameter;

    clawt_gtk_on_new_room(NULL, user_data);
}

static void
on_new_agent_activate(GSimpleAction *action, GVariant *parameter,
                      gpointer user_data)
{
    (void)action;
    (void)parameter;

    clawt_gtk_on_new_agent(NULL, user_data);
}

static void
on_import_agent_activate(GSimpleAction *action, GVariant *parameter,
                         gpointer user_data)
{
    (void)action;
    (void)parameter;

    on_import_agent(NULL, user_data);
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
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_IMAGES))
        return;

    do
        refresh_settings_images_once(self);
    while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_IMAGES));
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
    clawt_gtk_clear_list(GTK_LIST_BOX(self->settings_images));

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

/*
 * A page that has to read something reads it when it is opened.
 *
 * The memory page costs two round trips to fill, and filling it on every
 * connect would pay for both whether or not anybody looks -- which is
 * the same mistake as warming a provider cache in clawt_daemon_start().
 */
static void
on_page_shown(ClawtWindow *self)
{
    if (clawt_gtk_current_page(self) != CLAWT_PAGE_MEMORY)
        return;

    clawt_gtk_refresh_operator(self);
    clawt_gtk_refresh_recall(self);
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
                                   clawt_gtk_build_teams_page(self)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   clawt_gtk_build_spending_page(self)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   clawt_gtk_build_integrations_page(self)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   clawt_gtk_build_connectors_page(self)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   clawt_gtk_build_shared_folders_page(self)));
    adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog),
                               ADW_PREFERENCES_PAGE(
                                   clawt_gtk_build_appearance_page(self)));

    self->settings = dialog;
    g_signal_connect(dialog, "closed", G_CALLBACK(on_settings_closed), self);

    refresh_settings_images(self);
    clawt_gtk_refresh_settings_teams(self);
    clawt_gtk_refresh_settings_folders(self);
    clawt_gtk_refresh_settings_spending(self);
    clawt_gtk_refresh_settings_integrations(self);
    clawt_gtk_refresh_settings_connectors(self);
    adw_dialog_present(dialog, GTK_WIDGET(self));
}

/* ── Construction ────────────────────────────────────────────────── */

/*
 * Which builder fills a page.
 *
 * A `switch` with no `default:`, so -Wswitch names a page added to the
 * library that nothing here draws.  The permissive alternative is a
 * stack with a hole in it: a tab that opens onto an empty box, which
 * reads as a page that failed to load rather than as one nobody wrote.
 */
static GtkWidget *
build_page(ClawtWindow *self, ClawtPage page)
{
    switch (page) {
    case CLAWT_PAGE_CHAT:      return clawt_gtk_build_chat_page(self);
    case CLAWT_PAGE_AGENT:     return clawt_gtk_build_inspector_page(self);
    case CLAWT_PAGE_MAILBOX:   return clawt_gtk_build_mailbox_page(self);
    case CLAWT_PAGE_COMPUTER:  return clawt_gtk_build_computer_page(self);
    case CLAWT_PAGE_ROUTINES:  return clawt_gtk_build_routine_page(self);
    case CLAWT_PAGE_TRIGGERS:  return clawt_gtk_build_trigger_page(self);
    case CLAWT_PAGE_TASKS:     return clawt_gtk_build_task_page(self);
    case CLAWT_PAGE_DECISIONS: return clawt_gtk_build_decision_page(self);
    case CLAWT_PAGE_FLOW:      return clawt_gtk_build_flow_page(self);
    case CLAWT_PAGE_SKILLS:    return clawt_gtk_build_skill_page(self);
    case CLAWT_PAGE_MEMORY:    return clawt_gtk_build_recall_page(self);
    }

    g_return_val_if_reached(NULL);
}

/*
 * The icon on a section's tab.
 *
 * Here rather than as a column in the library's table: an icon name is
 * GNOME's vocabulary, libclawt must never link GTK, and the web client
 * draws no icons at all, so a column would be a GTK string carried by
 * everything that links the library and read by one caller.  A `switch`
 * with no `default:` gives the same guarantee a column would -- a
 * section added to the table does not build here until somebody has
 * chosen an icon for it.
 */
static const gchar *
section_icon(ClawtSection section)
{
    switch (section) {
    case CLAWT_SECTION_CHAT:       return "user-available-symbolic";
    case CLAWT_SECTION_AGENT:      return "emblem-system-symbolic";
    case CLAWT_SECTION_COMPUTER:   return "utilities-terminal-symbolic";
    case CLAWT_SECTION_AUTOMATION: return "alarm-symbolic";
    case CLAWT_SECTION_WORK:       return "view-list-symbolic";
    case CLAWT_SECTION_LIBRARY:    return "accessories-text-editor-symbolic";
    }

    g_return_val_if_reached("application-x-executable-symbolic");
}

/*
 * The switcher, two deep.
 *
 * Eleven tabs did not fit a header bar: below about 1500 logical pixels
 * an AdwViewSwitcher pinned to POLICY_WIDE is simply clipped -- no
 * ellipsis, no overflow menu, nothing in the log -- so which pages a
 * person could reach depended on the monitor, and on the machine where
 * it was written they all fit.
 *
 * Grouped into six, walked from clawt_section_count() rather than
 * listed, so the browser and the window cannot come to disagree about
 * which group a page is in.
 */
static void
build_page_stack(ClawtWindow *self)
{
    guint n_sections = clawt_section_count();
    guint n_pages = clawt_page_count();
    guint i;

    self->pages = ADW_VIEW_STACK(adw_view_stack_new());

    self->section_stacks = g_new0(AdwViewStack *, n_sections);
    self->section_tabs = g_new0(AdwViewStackPage *, n_sections);
    self->page_tabs = g_new0(AdwViewStackPage *, n_pages);
    self->page_badges = g_new0(guint, n_pages);
    self->page_attention = g_new0(gboolean, n_pages);

    for (i = 0; i < n_sections; i++) {
        ClawtSection section = clawt_section_nth(i);
        guint n_children = clawt_section_page_count(section);
        GtkWidget *child;

        if (n_children == 1) {
            /*
             * A section that is one page holds it directly.  A switcher
             * over a stack of one draws a row with a single tab in it,
             * which reads as a control that does nothing -- and costs a
             * click and a strip of vertical space to say so.
             */
            child = build_page(self, clawt_section_page_nth(section, 0));
        } else {
            GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
            GtkWidget *stack = adw_view_stack_new();
            GtkWidget *switcher = adw_view_switcher_new();
            guint j;

            for (j = 0; j < n_children; j++) {
                ClawtPage page = clawt_section_page_nth(section, j);

                self->page_tabs[page] = adw_view_stack_add_titled(
                    ADW_VIEW_STACK(stack), build_page(self, page),
                    clawt_page_nick(page), clawt_page_label(page));
            }

            adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher),
                                        ADW_VIEW_STACK(stack));
            adw_view_switcher_set_policy(ADW_VIEW_SWITCHER(switcher),
                                         ADW_VIEW_SWITCHER_POLICY_WIDE);
            gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
            gtk_widget_set_margin_top(switcher, 6);
            gtk_widget_add_css_class(switcher, "clawt-section-switcher");
            gtk_widget_set_vexpand(stack, TRUE);

            gtk_box_append(GTK_BOX(box), switcher);
            gtk_box_append(GTK_BOX(box), stack);

            self->section_stacks[section] = ADW_VIEW_STACK(stack);
            child = box;
        }

        self->section_tabs[section] = adw_view_stack_add_titled_with_icon(
            self->pages, child, clawt_section_nth_nick(i),
            clawt_section_nth_label(i), section_icon(section));
    }
}

void
clawt_gtk_show_page(ClawtWindow *self, ClawtPage page)
{
    ClawtSection section;

    g_return_if_fail(CLAWT_IS_WINDOW(self));

    if (self->pages == NULL)
        return;

    section = clawt_page_section(page);

    /*
     * The inner stack first.  Selecting the section emits
     * notify::visible-child-name on `pages`, and both handlers that
     * fires ask which page is now up -- so the inner answer has to
     * already be this one, or opening Library from a menu refreshes
     * Skills and leaves Memory blank.
     */
    if (self->section_stacks[section] != NULL)
        adw_view_stack_set_visible_child_name(self->section_stacks[section],
                                              clawt_page_nick(page));

    adw_view_stack_set_visible_child_name(self->pages,
                                          clawt_section_nick(section));
}

ClawtPage
clawt_gtk_current_page(ClawtWindow *self)
{
    ClawtSection section;
    const gchar *name;

    g_return_val_if_fail(CLAWT_IS_WINDOW(self), CLAWT_PAGE_CHAT);

    if (self->pages == NULL || self->section_stacks == NULL)
        return CLAWT_PAGE_CHAT;

    section = clawt_section_from_nick(
        adw_view_stack_get_visible_child_name(self->pages));

    if (self->section_stacks[section] == NULL)
        return clawt_section_default_page(section);

    name = adw_view_stack_get_visible_child_name(
        self->section_stacks[section]);

    /*
     * NULL is not "the first page" by accident: a stack answers NULL
     * only before it has a visible child, and clawt_page_from_nick()
     * would turn that into Chat -- a page in a different section, which
     * is a wrong answer rather than an approximate one.
     */
    if (name == NULL)
        return clawt_section_default_page(section);

    return clawt_page_from_nick(name);
}

void
clawt_gtk_set_page_badge(ClawtWindow *self, ClawtPage page, guint count,
                         gboolean attention)
{
    ClawtSection section;
    guint total = 0;
    gboolean urgent = FALSE;
    guint n;
    guint i;

    g_return_if_fail(CLAWT_IS_WINDOW(self));

    if (self->page_badges == NULL || self->section_tabs == NULL)
        return;

    /*
     * These arrays are sized by clawt_page_count() and indexed by the
     * value, so a page added to the enumeration and left out of the
     * library's table would write past the end.
     *
     * The compiler catches that first -- build_page() here and
     * clawt_web_view_body() in the other client are both a `switch` with
     * no `default:`, so a new value is two -Wswitch warnings, and the
     * zero-warning rule is what stops somebody.  This is the backstop
     * for the case where it did not, because a silent out-of-bounds
     * write is a much worse way to find out than a blank tab.
     */
    if ((guint)page >= clawt_page_count())
        return;

    self->page_badges[page] = count;
    self->page_attention[page] = attention;

    if (self->page_tabs[page] != NULL) {
        adw_view_stack_page_set_badge_number(self->page_tabs[page], count);
        adw_view_stack_page_set_needs_attention(self->page_tabs[page],
                                                attention);
    }

    /*
     * And the section's own tab carries the sum, because a badge one
     * level down is invisible until somebody opens the section -- which
     * for Decisions would defeat the point of having one.
     */
    section = clawt_page_section(page);
    n = clawt_section_page_count(section);

    for (i = 0; i < n; i++) {
        ClawtPage sibling = clawt_section_page_nth(section, i);

        total += self->page_badges[sibling];
        urgent = urgent || self->page_attention[sibling];
    }

    if (self->section_tabs[section] != NULL) {
        adw_view_stack_page_set_badge_number(self->section_tabs[section],
                                             total);
        adw_view_stack_page_set_needs_attention(self->section_tabs[section],
                                                urgent);
    }
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
     * Asked, not assumed.  main() connects before this window exists, so
     * ::connected has already been emitted for a client that came up
     * normally and no handler of ours saw it -- a window that took the
     * signal as its only evidence would draw "not connected" over a
     * perfectly good fleet.
     */
    self->connected_once = clawt_client_is_connected(client);

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

    /*
     * Wide enough that the alerts panel pushes rather than overlays on
     * first launch.
     *
     * 1100 was below the breakpoint above it, so the side-by-side layout
     * existed and nobody arrived at it -- a window had to be resized
     * before the design the panel was drawn for appeared at all.  1280
     * clears the 1150 threshold with room, and 1280x720 still fits a
     * 1366x768 laptop with its panels, which 1280x800 does not.
     *
     * The overlay is not lost by this: it is what the breakpoint still
     * gives anyone who makes the window narrower, which is now a choice
     * rather than the only state.
     */
    gtk_window_set_default_size(GTK_WINDOW(self), 1280, 720);

    /* ── Sidebar ── */
    self->sidebar = GTK_LIST_BOX(gtk_list_box_new());
    gtk_list_box_set_selection_mode(self->sidebar, GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(GTK_WIDGET(self->sidebar), "navigation-sidebar");
    g_signal_connect(self->sidebar, "row-selected",
                     G_CALLBACK(clawt_gtk_on_row_selected), self);

    sidebar_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll),
                                  GTK_WIDGET(self->sidebar));
    gtk_widget_set_vexpand(sidebar_scroll, TRUE);
    self->sidebar_scroll = sidebar_scroll;

    /*
     * After the scroller exists, because the menu is parented to it
     * rather than to the list -- see clawt_gtk_build_agent_menu().
     */
    clawt_gtk_build_agent_menu(self);

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
        /*
         * Stateful and taking the peer id, so the menu draws as radios
         * with the conversation on screen ticked -- a switcher that
         * cannot say where you are makes you open one to find out.
         */
        {
            g_autoptr(GSimpleAction) conversation =
                g_simple_action_new_stateful("conversation",
                                             G_VARIANT_TYPE_STRING,
                                             g_variant_new_string(""));

            g_signal_connect(conversation, "activate",
                             G_CALLBACK(clawt_gtk_on_conversation_chosen), self);
            g_action_map_add_action(G_ACTION_MAP(self),
                                    G_ACTION(conversation));
        }

        GSimpleAction *settings_action = g_simple_action_new("settings",
                                                             NULL);

        /*
         * Holding the fleet lives here rather than beside an agent,
         * because it is about all of them: it stops delivery everywhere
         * and lets the turns that are running finish, which is what
         * makes a restart a bounded operation instead of an unbounded
         * loss.  Two entries rather than one toggle -- a menu item that
         * says "Pause" while the fleet is already held is a control
         * whose label is the opposite of the truth, and the banner is
         * what says which state you are in.
         */
        {
            g_autoptr(GSimpleAction) pause =
                g_simple_action_new("pause-fleet", NULL);
            g_autoptr(GSimpleAction) resume =
                g_simple_action_new("resume-fleet", NULL);

            g_signal_connect(pause, "activate",
                             G_CALLBACK(clawt_gtk_on_pause_fleet), self);
            g_signal_connect(resume, "activate",
                             G_CALLBACK(clawt_gtk_on_resume_fleet), self);
            g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(pause));
            g_action_map_add_action(G_ACTION_MAP(self), G_ACTION(resume));
        }

        g_menu_append(menu, "Hold the fleet", "win.pause-fleet");
        g_menu_append(menu, "Resume the fleet", "win.resume-fleet");
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
        g_menu_append(menu, "New room\342\200\246", "win.room-create");

        new_button = adw_split_button_new();
        adw_split_button_set_icon_name(ADW_SPLIT_BUTTON(new_button),
                                       "list-add-symbolic");
        adw_split_button_set_menu_model(ADW_SPLIT_BUTTON(new_button),
                                        G_MENU_MODEL(menu));
        gtk_widget_set_tooltip_text(new_button, "Add an agent");

        g_signal_connect(new_button, "clicked", G_CALLBACK(clawt_gtk_on_new_agent),
                         self);

        {
            GSimpleAction *room_action =
                g_simple_action_new("room-create", NULL);

            g_signal_connect(room_action, "activate",
                             G_CALLBACK(on_new_room_activate), self);
            g_action_map_add_action(G_ACTION_MAP(self),
                                    G_ACTION(room_action));
            g_object_unref(room_action);
        }

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
    gtk_widget_set_name(sidebar_box, "clawt-sidebar");

    /* ── Content ── */
    build_page_stack(self);

    header = adw_header_bar_new();
    gtk_widget_set_name(header, "clawt-headerbar");
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

        self->connection_status = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free,
            (GDestroyNotify)clawt_connection_status_free);

        g_signal_connect(popover, "show",
                         G_CALLBACK(on_connection_menu_shown), self);

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
     * The panel is a fraction of what is left, not a width.
     * AdwOverlaySplitView has no sidebar-width property: it takes
     * sidebar-width-fraction, clamped by min-sidebar-width and
     * max-sidebar-width, which default to 180 and 280.  So with the
     * 0.26 set below the panel measures 259px beside a 1000px content
     * area, against 0.26 x 1000 predicted, and 179px once the fraction
     * would fall under the 180 floor.  Both are the measured allocation
     * rather than the model, which is the point: they are a pixel under
     * because that is what the widget was actually given.  Anything in
     * here that names a fixed panel width
     * is describing a widget that does not exist.
     *
     * What the breakpoint below is derived from is therefore the
     * transcript's side of that split, and the derivation is in the
     * comment on it.
     */
    self->alerts_split = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    adw_overlay_split_view_set_sidebar_position(self->alerts_split,
                                                GTK_PACK_END);

    /*
     * A peer of the agent sidebar, not a sidebar *of* it.
     *
     * libadwaita styles panes by where they sit in the widget tree: a
     * `.sidebar-pane` inside a `.content-pane` is a nested sidebar, so it
     * is painted from `--secondary-sidebar-bg-color` -- deliberately a
     * different shade, because a sidebar within a sidebar's content
     * should not read as the same surface.  This one is inside the outer
     * split view's content purely so that opening alerts does not hide
     * the agent list, which is navigation.  That is a layout decision,
     * not a statement of hierarchy, and libadwaita cannot tell the two
     * apart.
     *
     * So the two panels on the two edges of the window were drawn in two
     * different greys.  Measured against real GTK 4.22 and libadwaita
     * 1.9.3 before this line existed: the agent sidebar `#181825` and
     * this one `#28282c`, which is not in the Catppuccin palette at all
     * -- and on stock GNOME dark, `#2e2e32` against `#28282c`, so the
     * mismatch was there under every theme rather than only under a
     * palette.  `.isolated` is libadwaita's own way of saying this split
     * view is not a pane of the one around it; it now takes
     * `--sidebar-bg-color` and the two edges match.
     */
    gtk_widget_add_css_class(GTK_WIDGET(self->alerts_split), "isolated");
    adw_overlay_split_view_set_sidebar(self->alerts_split,
                                       build_alerts_panel(self));
    self->page_toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    adw_toast_overlay_set_child(self->page_toasts, GTK_WIDGET(self->pages));

    adw_overlay_split_view_set_content(self->alerts_split,
                                       GTK_WIDGET(self->page_toasts));
    adw_overlay_split_view_set_show_sidebar(self->alerts_split, FALSE);
    adw_overlay_split_view_set_sidebar_width_fraction(
        self->alerts_split, CLAWT_ALERTS_PANEL_FRACTION);

    switcher = adw_view_switcher_new();
    adw_view_switcher_set_stack(ADW_VIEW_SWITCHER(switcher), self->pages);
    gtk_widget_set_name(switcher, "clawt-page-switcher");
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
        gtk_widget_set_name(bell, "clawt-alerts-bell");
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

    /*
     * Under the header and over the content, which is where libadwaita
     * puts a banner and where a person looks for one.  In the *content*
     * rather than above the split, so it does not push the agent list
     * down -- that list is navigation and losing a connection is not a
     * reason to move it.
     */
    self->connection_banner = adw_banner_new("");
    adw_banner_set_revealed(ADW_BANNER(self->connection_banner), FALSE);
    g_signal_connect(self->connection_banner, "button-clicked",
                     G_CALLBACK(on_banner_button), self);
    gtk_box_append(GTK_BOX(content), self->connection_banner);

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
                             G_CALLBACK(clawt_gtk_update_unread_tab), self);

    /*
     * And the memory page reads itself when it is opened.
     *
     * Not on connect: the operator profile is one IPC round trip and the
     * recall query is another, and paying for both on every reconnect to
     * fill a page nobody has looked at is exactly the shape of thing the
     * daemon's own start had to stop doing.
     */
    g_signal_connect_swapped(self->pages, "notify::visible-child-name",
                             G_CALLBACK(on_page_shown), self);

    /*
     * And on every section's own stack, because a move inside a section
     * never reaches `pages`.
     *
     * Skills and Memory share the Library section, so switching between
     * them changes which page is up while the top-level child name does
     * not move at all -- without this the memory page is filled once, on
     * whichever entry to Library happened to come from a menu, and never
     * again.
     */
    {
        guint i;

        for (i = 0; i < clawt_section_count(); i++) {
            AdwViewStack *inner = self->section_stacks[clawt_section_nth(i)];

            if (inner == NULL)
                continue;

            g_signal_connect_swapped(inner, "notify::visible-child-name",
                                     G_CALLBACK(clawt_gtk_update_unread_tab),
                                     self);
            g_signal_connect_swapped(inner, "notify::visible-child-name",
                                     G_CALLBACK(on_page_shown), self);
        }
    }

    g_signal_connect_swapped(self->split, "notify::collapsed",
                             G_CALLBACK(clawt_gtk_update_unread_tab), self);
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
     * And below 1150px the alerts panel overlays rather than pushes.
     *
     * Derived from what is left for the transcript once the agent list
     * and the panel have taken their share.  With W the window, the
     * content beside the list is W - 280, the panel takes 0.26 of it and
     * the transcript keeps the other 0.74.  Fitting the 600px clamp is
     * not the criterion -- it is met at W = 1090 and looks wrong there,
     * because the column would sit 3.5px from the panel's edge.  The
     * criterion is the gap from the panel to the row's ink, which the
     * 12px row margin adds to: (T - 600) / 2 + 12 >= 24 gives T >= 624
     * and so W >= 1124.  1150 clears that by 26px.
     *
     * Below it something has to give, and the reader's place in the
     * conversation is the wrong thing to take.
     *
     * Opening the panel does shift the column left -- 122px at 1280,
     * measured.  That is accepted rather than overlooked: the clamp
     * fixes the measure, so pushing translates the column without
     * reflowing it and neither the line breaks nor the scroll position
     * move.  Left-aligning the column would buy stability for a
     * deliberate, occasional toggle at the cost of balance on every
     * window all the time.
     */
    {
        g_autofree gchar *condition =
            g_strdup_printf("max-width: %dpx", CLAWT_ALERTS_PUSH_BREAKPOINT);

        breakpoint =
            adw_breakpoint_new(adw_breakpoint_condition_parse(condition));
    }

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

    adw_application_window_set_content(ADW_APPLICATION_WINDOW(self),
                                       GTK_WIDGET(self->split));

    g_signal_connect(client, "event", G_CALLBACK(on_daemon_event), self);
    watch_the_connection(self);

    /*
     * Asked once, here, rather than left for a feature to discover.
     * `control.status` reported the version from the day the frame
     * existed and no graphical client had ever sent it.
     */
    note_daemon_version(self);

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

        clawt_gtk_apply_appearance(self->appearance);
    }

    reload_connections(self);
    update_connection_label(self);

    clawt_gtk_refresh_agents(self);
    clawt_gtk_refresh_routines(self);

    return self;
}

static void
clawt_window_dispose(GObject *object)
{
    ClawtWindow *self = CLAWT_WINDOW(object);

    /*
     * The composer that is open right now.  Leaving an agent saves its
     * draft; closing the window on one is the other half, and without it
     * the message you were part-way through when you quit is the only
     * one that does not come back.
     */
    if (self->selected_agent != NULL && self->drafts != NULL) {
        g_autofree gchar *draft = clawt_gtk_entry_text(self);

        clawt_gtk_persist_draft(self, self->selected_agent, draft);
    }

    /*
     * Before the client goes, because letting go of a screen is a
     * request. A window that closed while watching would leave the
     * daemon grabbing for a client that is not there, and the count
     * nobody decremented never reaches zero.
     */
    clawt_gtk_stop_watching_screen(self);

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
    g_clear_object(&self->agent_menu_computer);
    g_clear_pointer(&self->teams_seen, json_node_unref);

    g_clear_pointer(&self->appearance, clawt_appearance_free);
    g_clear_pointer(&self->connections, g_ptr_array_unref);
    g_clear_pointer(&self->connection_status, g_hash_table_unref);
    g_clear_pointer(&self->daemon_version, g_free);
    g_clear_pointer(&self->daemon_update, g_free);
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
    g_clear_pointer(&self->selected_color, g_free);
    g_clear_pointer(&self->shown, g_hash_table_unref);

    /*
     * The widget is the transcript's; the array is ours.
     */
    self->steps_block = NULL;
    g_clear_pointer(&self->steps, g_ptr_array_unref);
    g_clear_pointer(&self->flow_shown, g_hash_table_unref);
    g_clear_pointer(&self->drafts, g_hash_table_unref);
    g_clear_pointer(&self->selected_room_entry, g_free);
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
    g_free(self->last_toast);
    g_clear_pointer(&self->settings_bars, g_hash_table_unref);
    g_clear_pointer(&self->settings_catalog, json_node_unref);

    /*
     * The records only, not the widgets: those belong to the group they
     * were added to, which GTK has already taken apart.
     */
    g_clear_pointer(&self->schema_rows, g_ptr_array_unref);

    /*
     * The same rule for the switcher's bookkeeping: these hold borrowed
     * AdwViewStackPage and AdwViewStack pointers owned by the stacks
     * they were added to.  Only the arrays are ours.
     */
    g_clear_pointer(&self->section_stacks, g_free);
    g_clear_pointer(&self->section_tabs, g_free);
    g_clear_pointer(&self->page_tabs, g_free);
    g_clear_pointer(&self->page_badges, g_free);
    g_clear_pointer(&self->page_attention, g_free);

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
    clawt_gtk_set_following(self, TRUE);
    self->connections = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_connection_free);
    self->schema_rows = g_ptr_array_new_with_free_func(clawt_gtk_schema_row_free);
    self->shown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        NULL);

    /*
     * Flow keeps its own, and it is emptied when the conversation
     * changes rather than when the daemon does: it is about which
     * messages of the open room are already drawn, and the room is
     * whichever one the reader clicked last.
     */
    self->flow_shown = g_hash_table_new_full(g_str_hash, g_str_equal,
                                             g_free, NULL);
    self->drafts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         g_free);
    self->unread = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         NULL);
    self->connected_at = g_get_real_time();
    self->alerts = g_ptr_array_new_with_free_func(alert_free);
    self->dm_rooms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           g_free);
    self->pending = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_gtk_attachment_free);

    /*
     * Holds a reference to each bar, so a progress event arriving while
     * the list is being rebuilt finds a live widget rather than one the
     * container has already dropped.
     */
    self->settings_bars = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                g_free, g_object_unref);
}
