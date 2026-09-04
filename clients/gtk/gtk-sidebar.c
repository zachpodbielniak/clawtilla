/*
 * gtk-sidebar.c - The sidebar
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The fleet as the daemon groups it, the unread counts beside it, the
 * drag that reorders it, and the menu a right-click opens.
 *
 * One of the pages split out of clawt-window.c.  Everything it needs
 * from the window -- the instance struct, and the helpers more than one
 * page uses -- comes from clawt-window-private.h.
 */

#include "clawt-window-private.h"

#include <string.h>

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
    gtk_widget_add_css_class(dot, "clawt-agent-state");
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

/* ── Unread ──────────────────────────────────────────────────────── */

static guint
unread_for(ClawtWindow *self, const gchar *room_id)
{
    if (room_id == NULL || self->unread == NULL)
        return 0;

    return GPOINTER_TO_UINT(g_hash_table_lookup(self->unread, room_id));
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
void
clawt_gtk_update_unread_tab(ClawtWindow *self)
{
    GHashTableIter iter;
    gpointer value;
    guint total = 0;
    gboolean hide;

    if (self->unread == NULL)
        return;

    g_hash_table_iter_init(&iter, self->unread);

    while (g_hash_table_iter_next(&iter, NULL, &value))
        total += GPOINTER_TO_UINT(value);

    hide = clawt_gtk_current_page(self) == CLAWT_PAGE_CHAT &&
           self->split != NULL &&
           !adw_overlay_split_view_get_collapsed(self->split);

    clawt_gtk_set_page_badge(self, CLAWT_PAGE_CHAT, hide ? 0 : total,
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
void
clawt_gtk_note_unread(ClawtWindow *self, ClawtEvent *event, const gchar *from)
{
    const gchar *room_id = clawt_event_get_subject(event);
    const gchar *agent_id = NULL;
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

    /*
     * Counted against the room rather than the agent whose room it is.
     *
     * It used to resolve the room to an agent and give up when there
     * was none -- which is every group room, so a group could never
     * light a badge and a chat you have to remember to open is a chat
     * you forget.  Every row in the sidebar has a room, so the room is
     * the thing both kinds of row have in common.
     */
    (void)agent_id;

    count = unread_for(self, room_id) + 1;
    g_hash_table_insert(self->unread, g_strdup(room_id),
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
agent_row(ClawtWindow *self, JsonObject *agent, guint unread)
{
    GtkWidget *row = adw_action_row_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const gchar *state = clawt_json_string(agent, "state", "stopped");
    const gchar *agent_id = clawt_json_string(agent, "id", "");
    const gchar *name = clawt_json_string(agent, "name", agent_id);
    gboolean has_avatar = clawt_json_boolean(agent, "has_avatar", FALSE);
    gtk_widget_add_css_class(row, "clawt-agent-row");
    gtk_widget_add_css_class(box, "clawt-agent-caps");
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
        const gchar *description =
            clawt_json_string(agent, "description", "");
        gboolean show_descriptions =
            clawt_appearance_get_show_descriptions(self->appearance);
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
        {
            /*
             * What it is doing comes from the library, so that this row,
             * the web client's badge and the CLI's WORK column cannot
             * drift into three descriptions of one field.  What is
             * *waiting* stays here: the queue is merged into the
             * subtitle only in this client, because only this client has
             * nowhere else to put it.
             */
            g_autofree gchar *doing = clawt_agent_activity_label(busy, peer);
            g_autofree gchar *hold = clawt_hold_label(
                clawt_json_boolean(agent, "held", FALSE),
                clawt_json_boolean(agent, "draining", FALSE));

            /*
             * A hold outranks what the agent is doing, because it
             * changes what "working" means: a draining agent is
             * finishing its last turn and will not start another.
             */
            if (hold != NULL && depth > 0)
                activity = g_strdup_printf(
                    "%s \302\267 %" G_GINT64_FORMAT " waiting", hold, depth);
            else if (hold != NULL)
                activity = g_steal_pointer(&hold);
            else if (doing != NULL && depth > 0)
                activity = g_strdup_printf(
                    "%s · %" G_GINT64_FORMAT " waiting", doing, depth);
            else if (doing != NULL)
                activity = g_steal_pointer(&doing);
            else if (depth > 0)
                activity = g_strdup_printf(
                    "%" G_GINT64_FORMAT " waiting to be read", depth);
        }

        /*
         * The description under the name, unless somebody has turned
         * that off -- at which point it becomes the row's tooltip
         * rather than disappearing.  A description is written to be
         * read; what this setting is about is a fleet of ten agents
         * with a paragraph each not fitting on a screen.
         *
         * The *activity* is never hidden.  It is transient status
         * rather than the description -- the comment above already
         * says it stands "in place of its description while it is
         * doing it" -- and a list that stops saying which agents are
         * working answers a different question from the one this
         * setting was turned off to answer.
         */
        clawt_gtk_set_row_text(row,
                               clawt_json_string(agent, "name",
                                                 clawt_json_string(agent, "id", "?")),
                               activity != NULL
                         ? activity
                         : (show_descriptions ? description : NULL));

        if (!show_descriptions && *description != '\0')
            gtk_widget_set_tooltip_text(row, description);

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

    /*
     * The face, so picking an agent out of the list does not start with
     * reading its name.  Built through the one shared face-builder the
     * transcript and the inspector also call -- three copies of "how is
     * this agent drawn" is how the sidebar ended up as the client that
     * never got a face at all while the transcript header did.
     */
    adw_action_row_add_prefix(
        ADW_ACTION_ROW(row),
        clawt_gtk_build_avatar(self->client, name, agent_id, has_avatar,
                               clawt_json_string(agent, "color", NULL), 32));

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
                       clawt_gtk_badge("HOST", "error",
                                       "this agent can run commands on this machine"));

    /*
     * Who may put work on somebody's list.
     *
     * Which of the two badges -- and that it is never both -- is
     * clawt_team_badge_for()'s, in the library, because the web
     * sidebar draws the same pair and two answers to "is this agent a
     * lead" would differ exactly once.  `team_role` has been in
     * `agent.list` since the sidebar learned to group by team and
     * neither client drew it, so the chief was marked and every lead
     * under it was not.
     *
     * No `default:`, so a standing added to the enum draws a -Wswitch
     * warning here rather than silently going unmarked -- which is the
     * failure this whole badge has already had once, and it reports
     * itself to nobody.
     */
    switch (clawt_team_badge_for(
                clawt_json_boolean(agent, "chief_of_staff", FALSE),
                clawt_json_string(agent, "team_role", NULL))) {
    case CLAWT_TEAM_BADGE_CHIEF:
        gtk_box_append(GTK_BOX(box),
                       clawt_gtk_badge("CHIEF", "accent",
                                       "hands work to the other agents"));
        break;
    case CLAWT_TEAM_BADGE_LEAD:
        gtk_box_append(GTK_BOX(box),
                       clawt_gtk_badge("LEAD", "success",
                                       "hands work to its own team"));
        break;
    case CLAWT_TEAM_BADGE_NONE:
        break;
    }

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
     * What kind of machine it has, and whether stopping that machine
     * destroys it.
     *
     * Both come from the daemon rather than being worked out here.
     * Whether a computer type has a machine is a library question, and a
     * client answering it from a list of its own would offer Stop on a
     * backend added later -- or fail to offer it -- with nothing to say
     * which. And `stop_removes` has to be known *before* the menu item
     * is pressed, because for a container the contents do not come back.
     */
    {
        gboolean machine =
            json_object_has_member(agent, "computer_machine") &&
            json_object_get_boolean_member(agent, "computer_machine");
        gboolean removes =
            json_object_has_member(agent, "computer_stop_removes") &&
            json_object_get_boolean_member(agent, "computer_stop_removes");

        g_object_set_data(G_OBJECT(row), "agent-machine",
                          GINT_TO_POINTER(machine));
        g_object_set_data(G_OBJECT(row), "agent-stop-removes",
                          GINT_TO_POINTER(removes));
        g_object_set_data_full(
            G_OBJECT(row), "agent-computer",
            g_strdup(clawt_json_string(agent, "computer", "none")), g_free);
    }

    /*
     * And how it is drawn, which the transcript needs when a run header
     * is built for it.  Kept on the row rather than fetched with
     * agent.show at selection time: the sidebar was drawn from the same
     * reply a moment ago, and a round trip between clicking an agent and
     * seeing its first message is a round trip nobody asked for.
     *
     * "agent-has-avatar" rather than a path: the bytes themselves come
     * from clawt_gtk_avatar_texture()'s own cache, keyed on the agent id
     * clawt_window already carries selected -- a path only ever worked
     * when the client and the daemon shared a filesystem.
     */
    g_object_set_data_full(
        G_OBJECT(row), "agent-has-avatar",
        g_strdup(has_avatar ? "1" : ""), g_free);
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

    clawt_gtk_refresh_agents(self);
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
GtkStringList *
clawt_gtk_team_choices(ClawtWindow *self, const gchar *current, GStrv *out_ids)
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
guint
clawt_gtk_team_index_of(GStrv ids, const gchar *current)
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
                guint        total,
                guint        busy)
{
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *button = gtk_button_new();
    gtk_widget_add_css_class(row, "clawt-team-header");
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
    /*
     * A spinner when anybody on the team is mid-turn, matching the
     * agent rows underneath: a colour that means "busy" is a colour
     * somebody has to learn and movement is not.
     *
     * It matters most here.  A folded team hides exactly the rows that
     * would have shown it, so the one line standing in for them is the
     * only place left to say the team is working.
     */
    if (busy > 0) {
        GtkWidget *spinner = gtk_spinner_new();

        gtk_spinner_set_spinning(GTK_SPINNER(spinner), TRUE);
        /*
         * Named so `make parity` can see it.  A bare gtk_spinner_new()
         * is not a marker for this feature -- the agent rows have their
         * own spinners, so the check reported OK with the team one
         * removed.  Verified by removing it.
         */
        gtk_widget_add_css_class(spinner, "clawt-team-busy");
        gtk_widget_set_tooltip_text(spinner, "somebody here is taking a turn");
        gtk_box_append(GTK_BOX(box), spinner);
    }

    /*
     * Running out of total, rather than a single number. "3" could be
     * either, and the question somebody scanning a sidebar is asking is
     * how much of this team is awake.
     *
     * And how many of those are working, when any are.  One busy out of
     * seven is a different fleet state from seven out of seven, and a
     * bare spinner collapses the two; the number is left off entirely
     * when it is zero rather than drawn as "0 working", which is a
     * count nobody is looking for taking space on every idle heading.
     */
    tally = (busy > 0) ? g_strdup_printf("%u working \302\267 %u/%u",
                                         busy, running, total)
                       : g_strdup_printf("%u/%u", running, total);
    /*
     * One condition, not two.  busy is a subset of running -- the tally
     * enforces it -- so a `busy ? accent : running ? accent : dim` reads
     * as though the two cases differ and cannot.  The spinner above is
     * what says the team is working; this badge says whether it is up.
     */
    gtk_box_append(GTK_BOX(box),
                   clawt_gtk_badge(tally, (running > 0) ? "accent" : "dim",
                                   (busy > 0)
                             ? "agents running on this team, and how many"
                               " are taking a turn"
                             : "agents running on this team"));

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
const gchar *
clawt_gtk_agent_row_data(ClawtWindow *self, const gchar *agent_id, const gchar *key)
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
    return clawt_gtk_agent_row_data(self, agent_id, "agent-team");
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
/*
 * Which team's group a room row is currently under.
 *
 * By id rather than by widget pointer, for the same reason the agent
 * lookup is: a refresh can arrive from an idle mid-drag and destroy
 * every row.
 */
static const gchar *
team_of_room_row(ClawtWindow *self, const gchar *room_id)
{
    GtkWidget *child;

    for (child = gtk_widget_get_first_child(GTK_WIDGET(self->sidebar));
         child != NULL;
         child = gtk_widget_get_next_sibling(child)) {
        const gchar *id;

        if (!GTK_IS_LIST_BOX_ROW(child))
            continue;

        id = g_object_get_data(G_OBJECT(child), "room-id");

        if (g_strcmp0(id, room_id) == 0)
            return g_object_get_data(G_OBJECT(child), "room-team");
    }

    return NULL;
}

/*
 * Moves a room into a team's group, which is presentation and nothing
 * else: it changes neither who is in the room nor who a message
 * reaches.
 */
static gboolean
move_room_to_team(ClawtWindow *self, const gchar *room_id,
                  const gchar *team, const gchar *team_label)
{
    g_autoptr(JsonNode) reply = NULL;
    g_autofree gchar *message = NULL;

    reply = clawt_window_request(
        self, "room.set",
        clawt_build_payload("room", room_id, "team",
                            team != NULL ? team : "", NULL));

    if (reply == NULL)
        return FALSE;

    message = (team != NULL && *team != '\0')
              ? g_strdup_printf("%s moved to %s.", room_id,
                                team_label != NULL ? team_label : team)
              : g_strdup_printf("%s taken off its team.", room_id);

    clawt_window_toast(self, message);

    return TRUE;
}

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
    const gchar *token = g_value_get_string(value);
    const gchar *dragged;
    gboolean dragged_is_a_room;
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

    if (token == NULL || team == NULL || self == NULL)
        return FALSE;

    /* Either kind of row can be dropped onto a heading. */
    dragged_is_a_room = g_str_has_prefix(token, "r:");
    dragged = g_str_has_prefix(token, "a:") || dragged_is_a_room
        ? token + 2 : token;

    /*
     * Already there.  Accepted rather than refused: the drag animating
     * back to where it started reads as "that did not work", and it did
     * work -- there was simply nothing to do.
     */
    was = dragged_is_a_room ? team_of_room_row(self, dragged)
                            : team_of_agent_row(self, dragged);

    if (g_strcmp0(was != NULL ? was : "", team) == 0)
        return TRUE;

    if (dragged_is_a_room) {
        if (!move_room_to_team(self, dragged, team, label))
            return FALSE;
    } else if (!move_agent_to_team(self, dragged, team, label)) {
        return FALSE;
    }

    clawt_gtk_refresh_agents(self);

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
    const gchar *room_id = g_object_get_data(G_OBJECT(row), "room-id");
    g_autofree gchar *token = NULL;

    (void)source;
    (void)x;
    (void)y;

    /*
     * The kind travels with the id, because the drop handler cannot
     * work it out from a bare string: it has to know whether to move a
     * room's team or an agent's, and a room read as an agent silently
     * reaches nobody.  Typing only the reorder frame would not have
     * been enough -- the handler decides that before it builds one.
     */
    if (agent_id != NULL)
        token = g_strconcat("a:", agent_id, NULL);
    else if (room_id != NULL)
        token = g_strconcat("r:", room_id, NULL);
    else
        return NULL;

    return gdk_content_provider_new_typed(G_TYPE_STRING, token);
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
    const gchar *token = g_value_get_string(value);
    const gchar *dragged;
    gboolean dragged_is_a_room;
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

    if (token == NULL || self == NULL)
        return FALSE;

    dragged_is_a_room = g_str_has_prefix(token, "r:");
    dragged = g_str_has_prefix(token, "a:") || dragged_is_a_room
        ? token + 2 : token;

    /* The row it was dropped on is either kind too. */
    if (landed == NULL) {
        landed = g_object_get_data(G_OBJECT(onto), "room-id");
        onto_team = g_object_get_data(G_OBJECT(onto), "room-team");
    }

    if (landed == NULL)
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
    from_team = dragged_is_a_room ? team_of_room_row(self, dragged)
                                  : team_of_agent_row(self, dragged);

    if (g_strcmp0(from_team != NULL ? from_team : "",
                  onto_team != NULL ? onto_team : "") != 0) {
        const gchar *label = NULL;

        if (self->teams_seen != NULL && onto_team != NULL)
            label = team_display_name(
                json_object_get_array_member(
                    clawt_payload_of(self->teams_seen), "teams"),
                onto_team);

        if (dragged_is_a_room) {
            if (!move_room_to_team(self, dragged, onto_team, label))
                return FALSE;
        } else if (!move_agent_to_team(self, dragged, onto_team, label)) {
            return FALSE;
        }
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
        const gchar *room_id;
        g_autofree gchar *entry = NULL;
        gint index;

        if (!GTK_IS_LIST_BOX_ROW(child))
            continue;

        agent_id = g_object_get_data(G_OBJECT(child), "agent-id");
        room_id = g_object_get_data(G_OBJECT(child), "room-id");

        /* A team heading is neither, and is not part of the order. */
        if (agent_id != NULL)
            entry = g_strconcat("a:", agent_id, NULL);
        else if (room_id != NULL)
            entry = g_strconcat("r:", room_id, NULL);
        else
            continue;

        index = gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(child));

        /* Taken out of where it was... */
        if (g_strcmp0(agent_id != NULL ? agent_id : room_id, dragged) == 0)
            continue;

        if (index == onto_index && !after)
            g_string_append_printf(ids, "%s%s", ids->len > 0 ? "," : "",
                                   token);

        g_string_append_printf(ids, "%s%s", ids->len > 0 ? "," : "", entry);

        /* ...and put back beside the row it was dropped on. */
        if (index == onto_index && after)
            g_string_append_printf(ids, ",%s", token);
    }

    reply = clawt_window_request(
        self, "fleet.reorder",
        clawt_build_payload("entries", ids->str, NULL));

    if (reply == NULL)
        return FALSE;

    clawt_gtk_refresh_agents(self);

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
/* ── Making a room ───────────────────────────────────────────────── */

typedef struct {
    ClawtWindow *window;
    AdwDialog   *dialog;
    GtkWidget   *id_entry;
    GtkWidget   *name_entry;
    GPtrArray   *checks;   /* GtkCheckButton*, borrowed */
} NewRoomDialog;

static void
new_room_dialog_free(gpointer data, GClosure *closure)
{
    NewRoomDialog *dialog = data;

    (void)closure;

    g_clear_pointer(&dialog->checks, g_ptr_array_unref);
    g_free(dialog);
}

/*
 * Creates the room and, when it has more than two members, says how it
 * delivers.
 *
 * The second half is not decoration: a room of three reaches only the
 * members a message names, and somebody who made one and then typed a
 * greeting into it would otherwise conclude it was broken.
 */
static void
on_new_room_create(GtkButton *button, gpointer user_data)
{
    NewRoomDialog *dialog = user_data;
    ClawtWindow *self = dialog->window;
    const gchar *id = gtk_editable_get_text(GTK_EDITABLE(dialog->id_entry));
    const gchar *name =
        gtk_editable_get_text(GTK_EDITABLE(dialog->name_entry));
    g_autoptr(GString) members = g_string_new(NULL);
    g_autoptr(JsonNode) reply = NULL;
    guint chosen = 0;
    guint i;

    (void)button;

    if (id == NULL || *id == '\0') {
        clawt_window_toast(self, "A room needs a name to be addressed by.");
        return;
    }

    for (i = 0; i < dialog->checks->len; i++) {
        GtkWidget *check = g_ptr_array_index(dialog->checks, i);
        const gchar *agent_id;

        if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(check)))
            continue;

        agent_id = g_object_get_data(G_OBJECT(check), "agent-id");

        if (members->len > 0)
            g_string_append_c(members, ',');

        g_string_append(members, agent_id);
        chosen++;
    }

    if (chosen < 2) {
        clawt_window_toast(self, "Pick at least two agents -- a room with "
                                 "one is the conversation you already have "
                                 "with it.");
        return;
    }

    reply = clawt_window_request(
        self, "room.create",
        clawt_build_payload("room", id, "name",
                            (name != NULL && *name != '\0') ? name : id,
                            "members", members->str, NULL));

    if (reply == NULL)
        return;

    /*
     * More than two members is a group, and a group delivers by
     * mention.  Said now rather than left in the sidebar's subtitle,
     * because this is the moment somebody is about to type in it.
     */
    if (chosen > 2)
        clawt_window_toast(self,
                           "Created. A message there reaches only the "
                           "members you name with @their-id -- everyone "
                           "can still read everything.");
    else
        clawt_window_toast(self, "Created.");

    adw_dialog_close(dialog->dialog);
    clawt_gtk_refresh_agents(self);
}

void
clawt_gtk_on_new_room(GtkButton *button, gpointer user_data)
{
    ClawtWindow *self = user_data;
    NewRoomDialog *dialog = g_new0(NewRoomDialog, 1);
    AdwDialog *window = adw_dialog_new();
    GtkWidget *page = adw_preferences_page_new();
    GtkWidget *group = adw_preferences_group_new();
    GtkWidget *members = adw_preferences_group_new();
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    GtkWidget *create = gtk_button_new_with_label("Create");
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *agents;
    guint i;

    (void)button;

    dialog->window = self;
    dialog->dialog = window;
    dialog->checks = g_ptr_array_new();

    adw_dialog_set_title(window, "New room");
    adw_dialog_set_content_width(window, 460);

    dialog->id_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(dialog->id_entry), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->id_entry),
                                  "Id");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              dialog->id_entry);

    dialog->name_entry = adw_entry_row_new();
    adw_preferences_row_set_use_markup(
        ADW_PREFERENCES_ROW(dialog->name_entry), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(dialog->name_entry),
                                  "Name (optional)");
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(group),
                              dialog->name_entry);

    adw_preferences_group_set_description(
        ADW_PREFERENCES_GROUP(members),
        "Everyone here can read everything said in the room. With more "
        "than two, a message is only delivered to the members it names "
        "with @their-id -- so being in a room does not mean answering "
        "everything in it.");

    reply = clawt_window_request(self, "agent.list", NULL);

    if (reply != NULL) {
        agents = json_object_get_array_member(clawt_payload_of(reply),
                                              "agents");

        for (i = 0; i < json_array_get_length(agents); i++) {
            JsonObject *agent = json_array_get_object_element(agents, i);
            const gchar *id = clawt_json_string(agent, "id", "");
            GtkWidget *row = adw_action_row_new();
            GtkWidget *check = gtk_check_button_new();

            clawt_gtk_set_row_text(row, clawt_json_string(agent, "name", id),
                                   id);
            g_object_set_data_full(G_OBJECT(check), "agent-id",
                                   g_strdup(id), g_free);

            /*
             * The switch is the activatable widget rather than the row
             * itself: adw_action_row_set_activatable_widget(row, row)
             * recurses to a segfault while looking exactly right.
             */
            adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
            adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row),
                                                  check);

            adw_preferences_group_add(ADW_PREFERENCES_GROUP(members), row);
            g_ptr_array_add(dialog->checks, check);
        }
    }

    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(group));
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page),
                             ADW_PREFERENCES_GROUP(members));

    gtk_widget_add_css_class(create, "suggested-action");
    gtk_widget_set_halign(create, GTK_ALIGN_END);
    gtk_widget_set_margin_end(create, 12);
    gtk_widget_set_margin_bottom(create, 12);

    g_signal_connect_data(create, "clicked",
                          G_CALLBACK(on_new_room_create), dialog,
                          new_room_dialog_free, 0);

    gtk_widget_set_vexpand(page, TRUE);
    gtk_box_append(GTK_BOX(box), page);
    gtk_box_append(GTK_BOX(box), create);

    adw_dialog_set_child(window, box);
    adw_dialog_present(window, GTK_WIDGET(self));
}

void
clawt_gtk_on_row_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *agent_id;
    const gchar *room_id;

    (void)box;

    /* Emptying the list to rebuild it unselects; that is not a choice. */
    if (row == NULL)
        return;

    agent_id = g_object_get_data(G_OBJECT(row), "agent-id");

    if (agent_id != NULL) {
        clawt_gtk_select_agent(self, agent_id);
        return;
    }

    /*
     * Or a room, which is a first-class entry rather than one of a
     * selected agent's conversations.  A row with neither key does
     * nothing and says nothing, which is how a new kind of row fails
     * invisibly -- so both are read here, in the one handler.
     */
    room_id = g_object_get_data(G_OBJECT(row), "room-id");

    if (room_id != NULL)
        clawt_gtk_select_room(self, room_id);
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
    guint busy = 0;

    if (g_hash_table_contains(*emitted, id))
        return;

    clawt_team_tally(agents, team_id, &total, &running, &busy);

    gtk_list_box_append(
        self->sidebar,
        team_header_row(self, id,
                        (*id != '\0') ? team_display_name(teams, id)
                                      : "No team",
                        team_description(teams, team_id),
                        running, total, busy));

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

/*
 * Takes the keyboard focus off the rows a refresh is about to destroy.
 *
 * A GtkScrolledWindow scrolls whatever holds the focus into view, and
 * the sidebar is rebuilt from nothing on every daemon event.  Destroying
 * the focused row hands the focus to the list, whose own origin is the
 * first agent, and the fleet jumps to the top -- so somebody reading the
 * bottom of a long sidebar was pulled back to the start every time
 * anything in the fleet spoke.  Parking the focus on the scroller
 * instead moves nothing, because the scroller is not inside the area it
 * scrolls.
 *
 * Returns: (transfer full) (nullable): the agent whose new row should be
 *   given the focus back, or %NULL to leave it parked.  Only a row that
 *   is on screen right now earns that, because grabbing the focus
 *   scrolls the row into view and a refresh must not move the view.
 */
static gchar *
park_sidebar_focus(ClawtWindow *self)
{
    GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self->sidebar));
    GtkWidget *focus = root != NULL ? gtk_root_get_focus(root) : NULL;
    GtkWidget *row;
    graphene_rect_t bounds;
    const gchar *id;

    if (self->sidebar_scroll == NULL || focus == NULL ||
        !gtk_widget_is_ancestor(focus, GTK_WIDGET(self->sidebar)))
        return NULL;

    /*
     * The focus is on the row itself in practice, but a focusable widget
     * inside one would be a child of it -- so walk up rather than assume.
     */
    for (row = focus; row != NULL && !GTK_IS_LIST_BOX_ROW(row);
         row = gtk_widget_get_parent(row))
        ;

    id = row != NULL ? g_object_get_data(G_OBJECT(row), "agent-id") : NULL;

    /*
     * Read before the focus moves, because the answer is about where the
     * row is on screen and the grab below can scroll.
     */
    if (id != NULL &&
        (!gtk_widget_compute_bounds(row, self->sidebar_scroll, &bounds) ||
         bounds.origin.y + bounds.size.height <= 0.0f ||
         bounds.origin.y >=
             (gfloat)gtk_widget_get_height(self->sidebar_scroll)))
        id = NULL;

    gtk_widget_grab_focus(self->sidebar_scroll);

    return g_strdup(id);
}

/*
 * A room's label, from its members rather than its id.
 *
 * How a room is named is the daemon's business, and a client that takes
 * `dm:a:b` apart is a client that breaks when that changes.  A room
 * somebody named uses that name; one that has none is described by who
 * is in it, which is the only other true thing about it.
 *
 * Returns: (transfer full): the label
 */
static gchar *
room_label(JsonObject *room)
{
    const gchar *id = clawt_json_string(room, "id", "");
    const gchar *name = clawt_json_string(room, "name", NULL);
    JsonArray *members;
    g_autoptr(GString) out = NULL;
    guint i;

    if (name != NULL && *name != '\0' && g_strcmp0(name, id) != 0)
        return g_strdup(name);

    if (!json_object_has_member(room, "members"))
        return g_strdup(id);

    members = json_object_get_array_member(room, "members");
    out = g_string_new(NULL);

    for (i = 0; i < json_array_get_length(members); i++) {
        const gchar *member = json_array_get_string_element(members, i);

        if (g_strcmp0(member, "user") == 0)
            continue;

        if (out->len > 0)
            g_string_append(out, ", ");

        g_string_append(out, member);
    }

    if (out->len == 0)
        return g_strdup(id);

    return g_string_free(g_steal_pointer(&out), FALSE);
}

/*
 * One room in the sidebar, beside the agents.
 *
 * Carries `room-id` where an agent row carries `agent-id`, which is
 * also how every handler tells the two apart -- a row with neither does
 * nothing at all, silently, which is how a new row type fails
 * invisibly.
 */
static GtkWidget *
room_row(ClawtWindow *self, JsonObject *room, guint unread)
{
    GtkWidget *row = adw_action_row_new();
    const gchar *room_id = clawt_json_string(room, "id", "");
    g_autofree gchar *label = room_label(room);
    g_autofree gchar *subtitle = NULL;
    JsonArray *members = json_object_has_member(room, "members")
        ? json_object_get_array_member(room, "members") : NULL;
    guint count = (members != NULL) ? json_array_get_length(members) : 0;

    gtk_widget_add_css_class(row, "clawt-agent-row");

    /*
     * How many are in it, and whether being in it means answering
     * everything.  The second is the thing somebody actually needs to
     * know about a room before they type in it.
     */
    subtitle = clawt_json_boolean(room, "require_mention", FALSE)
        ? g_strdup_printf("%u members \xc2\xb7 answers when named", count)
        : g_strdup_printf("%u members \xc2\xb7 everyone answers", count);

    clawt_gtk_set_row_text(row, label, subtitle);

    if (unread > 0)
        adw_action_row_add_suffix(ADW_ACTION_ROW(row), unread_badge(unread));

    g_object_set_data_full(G_OBJECT(row), "room-id", g_strdup(room_id),
                           g_free);
    g_object_set_data_full(G_OBJECT(row), "room-team",
                           g_strdup(clawt_json_string(room, "team", "")),
                           g_free);

    (void)self;

    return row;
}

/*
 * Every room in @team's group, appended after that team's agents.
 *
 * Rooms sit with the agents they concern rather than in a section of
 * their own, which is what "move it wherever you like" has to mean if
 * a team's group is somewhere you can put one.  A room with no team
 * goes with the agents that have none.
 */
static void
append_rooms_for_team(ClawtWindow *self, JsonArray *rooms,
                      const gchar *team, GHashTable *emitted)
{
    guint i;

    if (rooms == NULL)
        return;

    for (i = 0; i < json_array_get_length(rooms); i++) {
        JsonObject *room = json_array_get_object_element(rooms, i);
        const gchar *id = clawt_json_string(room, "id", "");
        const gchar *room_team = clawt_json_string(room, "team", "");
        GtkWidget *row;

        /*
         * Only rooms somebody made.  A direct conversation is already
         * the agent's own row, and a routine's or a trigger's belongs to
         * that routine or trigger -- listing them here would be three
         * more entries per agent, none of which anybody chose.
         */
        if (!clawt_json_boolean(room, "declared", FALSE))
            continue;

        if (g_strcmp0(room_team, team != NULL ? team : "") != 0)
            continue;

        if (g_hash_table_contains(emitted, id))
            continue;

        g_hash_table_add(emitted, g_strdup(id));

        row = room_row(self, room, unread_for(self, id));
        gtk_list_box_append(self->sidebar, row);
        make_row_draggable(self, row);

        if (g_strcmp0(id, self->selected_room_entry) == 0)
            gtk_list_box_select_row(self->sidebar, GTK_LIST_BOX_ROW(row));
    }
}

static void
refresh_agents_once(ClawtWindow *self)
{
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *agents;
    g_autoptr(JsonNode) team_reply = NULL;
    g_autofree gchar *shown_team = NULL;
    g_autofree gchar *refocus = NULL;
    JsonArray *teams = NULL;
    g_autoptr(JsonNode) room_reply = NULL;
    JsonArray *rooms = NULL;
    g_autoptr(GHashTable) emitted = g_hash_table_new_full(g_str_hash,
                                                          g_str_equal,
                                                          g_free, NULL);
    g_autoptr(GHashTable) rooms_emitted = g_hash_table_new_full(g_str_hash,
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
     * And the rooms, already ordered by the daemon.  Sorting them here
     * would be a second answer to what order the sidebar is in, and the
     * two would differ exactly once.
     */
    room_reply = clawt_window_request(self, "room.list", NULL);

    if (room_reply != NULL)
        rooms = json_object_get_array_member(clawt_payload_of(room_reply),
                                             "rooms");

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

    /*
     * Before anything is torn down, because it is about the row that has
     * the focus now.
     */
    refocus = park_sidebar_focus(self);

    clawt_gtk_clear_list(self->sidebar);

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

    clawt_gtk_update_unread_tab(self);

    if (json_array_get_length(agents) == 0) {
        GtkWidget *row = adw_action_row_new();

        clawt_gtk_set_row_text(row, "No agents yet", "Use the + button to add one");
        gtk_list_box_append(self->sidebar, row);

        /*
         * And any rooms, which the loop below never reaches because it
         * walks agents.  A fleet with no agents has no rooms worth
         * having either -- but a config declaring both, with the agents
         * removed, would otherwise show a sidebar that has silently
         * lost them.
         */
        append_rooms_for_team(self, rooms, "", rooms_emitted);
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
            /*
             * The outgoing team's rooms, before the next heading takes
             * the floor -- so a room sits under the team it was put in
             * rather than at the end of the list.
             */
            if (i > 0 && !team_is_collapsed(self,
                                            shown_team != NULL
                                                ? shown_team : ""))
                append_rooms_for_team(self, rooms, shown_team,
                                      rooms_emitted);

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

        row = agent_row(self, agent,
                       unread_for(self,
                                  clawt_json_string(agent, "dm_room", "")));
        gtk_list_box_append(self->sidebar, row);

        /*
         * After the append, because a drop reads the row's index and a
         * row that is not in the list yet does not have one.
         */
        make_row_draggable(self, row);

        /*
         * The focus back onto the agent that had it, now that its row is
         * a different object.  park_sidebar_focus() has already decided
         * this is safe -- the row it names was on screen a moment ago,
         * so the scroll this grab asks for finds it already in view.
         */
        if (refocus != NULL &&
            g_strcmp0(clawt_json_string(agent, "id", ""), refocus) == 0)
            gtk_widget_grab_focus(row);

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
            (self->selected_agent == NULL &&
             self->selected_room_entry == NULL && i == 0))
            gtk_list_box_select_row(self->sidebar, GTK_LIST_BOX_ROW(row));

        /*
         * Stop follows the selected agent's own row.
         *
         * Done here as well as on the typing event, because a typing
         * frame only arrives on a transition: selecting an agent that is
         * already mid-turn, or reconnecting to a daemon that has been
         * working the whole time, produces no event at all and the
         * button would never appear.
         */
        if (g_strcmp0(clawt_json_string(agent, "id", ""),
                      self->selected_agent) == 0) {
            self->selected_can_interrupt =
                strstr(clawt_json_string(agent, "caps", ""),
                       "interrupt") != NULL;

            clawt_gtk_sync_stop_turn(self,
                                     json_object_has_member(agent, "busy") &&
                                     json_object_get_boolean_member(agent, "busy"));
        }
    }

    /* The last team's rooms, which no following header will flush. */
    if (!team_is_collapsed(self, shown_team != NULL ? shown_team : ""))
        append_rooms_for_team(self, rooms, shown_team, rooms_emitted);

    /* Whatever the fleet declares and nobody is on yet, at the bottom. */
    emit_empty_headers_before(self, teams, agents, NULL, &emitted);

    /*
     * And any room whose team holds no agents at all, which the loop
     * above never reaches: it walks agents, so a team with none of them
     * is a heading it never stands under.  A room that vanished because
     * the last agent left its team would read as a room that was
     * deleted.
     */
    if (rooms != NULL) {
        for (i = 0; i < json_array_get_length(rooms); i++) {
            JsonObject *room = json_array_get_object_element(rooms, i);

            append_rooms_for_team(self, rooms,
                                  clawt_json_string(room, "team", ""),
                                  rooms_emitted);
        }
    }
}

void
clawt_gtk_refresh_agents(ClawtWindow *self)
{
    if (!clawt_gtk_refresh_enter(self, CLAWT_REFRESH_AGENTS))
        return;

    do {
        refresh_agents_once(self);
    } while (clawt_gtk_refresh_repeat(self, CLAWT_REFRESH_AGENTS));
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
        clawt_gtk_delete_agent(self);
        return;
    }

    {
        g_autofree gchar *kind = g_strconcat("agent.", name, NULL);

        clawt_gtk_agent_action(self, kind);
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

    if (!clawt_gtk_apply_setting(self, "team", team))
        return;

    g_simple_action_set_state(action, g_variant_new_string(team));

    message = (*team != '\0')
              ? g_strdup_printf("%s moved to %s.", self->selected_agent, team)
              : g_strdup_printf("%s taken off its team.",
                                self->selected_agent);

    clawt_window_toast(self, message);

    /* The sidebar groups by team, so the row belongs somewhere else now. */
    clawt_gtk_refresh_agents(self);
}

/*
 * Powers the right-clicked agent's machine on, off, or off and on.
 *
 * The agent and its machine are different things and the menu says so:
 * these are under a Computer submenu, where the bare verbs are
 * unambiguous, rather than beside the agent's own Start and Stop, where
 * "Stop machine" and "Stop" would be a guess every time.
 */
static void
on_menu_computer(GSimpleAction *action, GVariant *parameter,
                 gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));
    const gchar *kind;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonBuilder) payload = json_builder_new();

    (void)parameter;

    if (self->selected_agent == NULL)
        return;

    /*
     * Spelled out rather than assembled from the action name.  A kind
     * built with g_strconcat() is invisible to `make parity`, which
     * reads the frame kinds each client mentions -- so the check would
     * have reported OK with this feature present in one client and not
     * the other, which is the exact blind spot it exists to close.
     */
    if (g_strcmp0(name, "computer-start") == 0)
        kind = "computer.start";
    else if (g_strcmp0(name, "computer-stop") == 0)
        kind = "computer.stop";
    else
        kind = "computer.restart";

    /*
     * `remove` is a boolean and has to arrive as one: the daemon refuses
     * a stop that would destroy the machine unless it is told to go
     * ahead, and a string "true" reads as absent there.
     *
     * Sent from here rather than only when the daemon asks, because the
     * confirmation has already happened -- ask_before_removing_stop()
     * would not have called us otherwise.
     */
    json_builder_begin_object(payload);
    json_builder_set_member_name(payload, "agent");
    json_builder_add_string_value(payload, self->selected_agent);
    json_builder_set_member_name(payload, "remove");
    json_builder_add_boolean_value(payload, TRUE);
    json_builder_end_object(payload);

    reply = clawt_window_request(self, kind,
                                 json_node_ref(json_builder_get_root(payload)));

    if (reply != NULL) {
        JsonObject *root = clawt_payload_of(reply);
        const gchar *state = clawt_json_string(root, "state", "?");
        g_autofree gchar *message = NULL;

        message = (json_object_has_member(root, "removes") &&
                   json_object_get_boolean_member(root, "removes"))
                  ? g_strdup_printf(
                        "%s: the machine is %s. It does not survive a stop, "
                        "so starting it builds a fresh one.",
                        self->selected_agent, state)
                  : g_strdup_printf("%s: the machine is %s.",
                                    self->selected_agent, state);

        clawt_window_toast(self, message);
    }

    /*
     * Refreshed whichever way it went, for the reason agent_action() is:
     * a stop that failed may still have left the machine somewhere new,
     * and a sidebar disagreeing with a toast is two answers on screen.
     */
    clawt_gtk_refresh_agents(self);
    clawt_gtk_refresh_selected(self);
}

typedef struct {
    ClawtWindow   *window;
    GSimpleAction *action;
} ComputerConfirm;

static void
on_removing_stop_confirmed(AdwAlertDialog *dialog, const gchar *response,
                           gpointer user_data)
{
    ComputerConfirm *confirm = user_data;

    (void)dialog;

    if (g_strcmp0(response, "stop") == 0)
        on_menu_computer(confirm->action, NULL, confirm->window);

    g_clear_object(&confirm->action);
    g_free(confirm);
}

/*
 * A container does not survive a stop unless computer.container.keep is
 * set, so Stop there is Stop-and-delete -- the contents are gone rather
 * than offline.  That is not a word anybody reads that way, so it is
 * asked before it is done.
 *
 * The daemon refuses it too, without the flag. Both, deliberately: the
 * fence is what protects a client that does not know to ask, and the
 * dialog is what stops the fence being an error message somebody has to
 * decode.
 */
static void
on_menu_computer_maybe_confirm(GSimpleAction *action, GVariant *parameter,
                               gpointer user_data)
{
    ClawtWindow *self = user_data;
    const gchar *name = g_action_get_name(G_ACTION(action));
    ComputerConfirm *confirm;
    AdwAlertDialog *ask;
    g_autofree gchar *body = NULL;

    if (!self->menu_stop_removes ||
        g_strcmp0(name, "computer-start") == 0) {
        on_menu_computer(action, parameter, user_data);
        return;
    }

    body = g_strdup_printf(
        "%s does not keep its machine across a stop, so everything inside "
        "it goes -- anything the agent installed, and anything it wrote "
        "outside a shared folder.\n\n"
        "Set computer.container.keep to keep it instead.",
        self->selected_agent != NULL ? self->selected_agent : "This agent");

    ask = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        g_strcmp0(name, "computer-stop") == 0
        ? "Stop this machine, and lose it?"
        : "Restart this machine, and lose it?", body));

    adw_alert_dialog_add_responses(ask, "cancel", "Cancel", "stop",
                                   g_strcmp0(name, "computer-stop") == 0
                                   ? "Stop" : "Restart", NULL);
    adw_alert_dialog_set_response_appearance(ask, "stop",
                                             ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response(ask, "cancel");
    adw_alert_dialog_set_close_response(ask, "cancel");

    confirm = g_new0(ComputerConfirm, 1);
    confirm->window = self;
    confirm->action = g_object_ref(action);

    g_signal_connect(ask, "response", G_CALLBACK(on_removing_stop_confirmed),
                     confirm);
    adw_dialog_present(ADW_DIALOG(ask), GTK_WIDGET(self));
}

/*
 * Fills the Computer section, or empties it.
 *
 * Emptied rather than greyed out for an agent with no machine: a `none`
 * or `host` agent has nothing to power on, and three permanently dead
 * entries would be three things to read past on every right-click. An
 * empty section draws as nothing.
 *
 * Whether there is a machine comes from the daemon --
 * clawt_computer_type_has_machine() -- so a backend added later appears
 * here without this function being touched.
 */
static void
fill_computer_menu(ClawtWindow *self, gboolean machine, const gchar *type)
{
    g_autoptr(GMenu) verbs = NULL;
    g_autofree gchar *label = NULL;

    if (self->agent_menu_computer == NULL)
        return;

    g_menu_remove_all(self->agent_menu_computer);

    if (!machine)
        return;

    verbs = g_menu_new();
    g_menu_append(verbs, "Start", "agent.computer-start");
    g_menu_append(verbs, "Stop", "agent.computer-stop");
    g_menu_append(verbs, "Restart", "agent.computer-restart");

    /*
     * Named, so the submenu says which kind of machine it is about
     * without the entries having to.  "Container" and "VM" are what
     * somebody is thinking of; "Computer" alone reads as a category.
     */
    label = g_strdup_printf("Computer (%s)",
                            type != NULL ? type : "machine");

    g_menu_append_submenu(self->agent_menu_computer, label,
                          G_MENU_MODEL(verbs));
}

static void
popup_agent_menu(ClawtWindow *self, gdouble x, gdouble y)
{
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(self->sidebar, (gint)y);
    g_autofree gchar *state = NULL;
    g_autofree gchar *team = NULL;
    g_autofree gchar *computer_type = NULL;
    const gchar *agent_id;
    GdkRectangle rect;
    graphene_point_t clicked;
    graphene_point_t at;
    gboolean machine;

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
    machine = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row),
                                                "agent-machine"));
    computer_type = g_strdup(g_object_get_data(G_OBJECT(row),
                                               "agent-computer"));
    self->menu_stop_removes =
        GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row),
                                          "agent-stop-removes"));

    /* Act on what was right-clicked, not on what happened to be selected. */
    gtk_list_box_select_row(self->sidebar, row);

    set_agent_action_states(self, state);
    fill_team_menu(self, team);
    fill_computer_menu(self, machine, computer_type);

    /*
     * The gesture reports where in the *list* the click landed and the
     * menu hangs off the scroller around it, so the point has to cross
     * that boundary -- the two differ by however far the fleet is
     * scrolled, which is precisely the case this menu is used in.
     */
    clicked.x = (gfloat)x;
    clicked.y = (gfloat)y;
    at = clicked;

    if (self->sidebar_scroll != NULL &&
        !gtk_widget_compute_point(GTK_WIDGET(self->sidebar),
                                  self->sidebar_scroll, &clicked, &at))
        at = clicked;

    rect.x = (gint)at.x;
    rect.y = (gint)at.y;
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

void
clawt_gtk_build_agent_menu(ClawtWindow *self)
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

    /*
     * Loudly, rather than falling back to the list: parenting the menu
     * there is the defect this arrangement exists to prevent, and a
     * quiet fallback would put it back the first time somebody moved
     * this call above the scroller's construction.
     */
    g_return_if_fail(self->sidebar_scroll != NULL);

    self->agent_actions = g_simple_action_group_new();

    for (i = 0; i < G_N_ELEMENTS(names); i++) {
        g_autoptr(GSimpleAction) action = g_simple_action_new(names[i], NULL);

        g_signal_connect(action, "activate", G_CALLBACK(on_menu_action), self);
        g_action_map_add_action(G_ACTION_MAP(self->agent_actions),
                                G_ACTION(action));
    }

    /*
     * The machine's own verbs.  Separate actions rather than a parameter
     * on the agent ones, because they act on a different thing: an agent
     * and the container it runs commands in are not the same object, and
     * a menu that blurred them would be a menu somebody misreads once
     * and then distrusts.
     */
    {
        static const gchar *const power[] = { "computer-start",
                                              "computer-stop",
                                              "computer-restart" };
        guint p;

        for (p = 0; p < G_N_ELEMENTS(power); p++) {
            g_autoptr(GSimpleAction) action =
                g_simple_action_new(power[p], NULL);

            g_signal_connect(action, "activate",
                             G_CALLBACK(on_menu_computer_maybe_confirm), self);
            g_action_map_add_action(G_ACTION_MAP(self->agent_actions),
                                    G_ACTION(action));
        }
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

    /*
     * Its own section, filled per right-click.  An agent with no machine
     * leaves it empty, and an empty section draws as nothing -- so the
     * menu on a chat-only agent is exactly the menu it was before.
     */
    self->agent_menu_computer = g_menu_new();
    g_menu_append_section(menu, NULL,
                          G_MENU_MODEL(self->agent_menu_computer));

    /* Its own section, so Delete is never the neighbour of Restart. */
    g_menu_append(danger, "Delete\342\200\246", "agent.delete");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(danger));

    gtk_widget_insert_action_group(GTK_WIDGET(self), "agent",
                                   G_ACTION_GROUP(self->agent_actions));

    self->agent_menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(menu));
    gtk_popover_set_has_arrow(GTK_POPOVER(self->agent_menu), FALSE);
    gtk_widget_set_halign(self->agent_menu, GTK_ALIGN_START);

    /*
     * Parented to the scroller, not to the list it scrolls.
     *
     * A popover takes the keyboard focus when it opens and hands it back
     * to its parent when it closes, and a GtkScrolledWindow scrolls
     * whatever holds the focus into view.  The list's own origin is the
     * fleet's first agent, so with the menu parented there every
     * right-click scrolled the sidebar to the top before the menu was
     * even read -- measured at 600px to 0 in one frame, on a list of
     * sixty rows.  The scroller is outside the scrolling area, so
     * focusing it moves nothing.
     *
     * A row would be the natural parent and cannot be one: a refresh
     * rebuilds the sidebar while the menu is open and frees it.
     */
    gtk_widget_set_parent(self->agent_menu, self->sidebar_scroll);

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
