/*
 * lrg-chat-model.c - What the sidebar and the chat page decide
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Every rule here is the GTK client's (clients/gtk/gtk-sidebar.c,
 * gtk-chat.c, the chat half of clawt-window.c), moved out of the drawing
 * code so tests/test-lrg-models-chat.c can check it without a canvas.
 * Rules both clients already share stay in libclawt and are only called
 * from here -- this file decides what is particular to drawing them.
 */

#include "clawt-lrg-chat-model.h"
#include "clawt-lrg-agent-model.h"

#include <string.h>

/* ── Small readers ────────────────────────────────────────────────── */

/* The array @member of a reply's object, or NULL. */
static JsonArray *
reply_array(JsonNode *reply, const gchar *member)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonNode *node;

    if (root == NULL || !json_object_has_member(root, member))
        return NULL;

    node = json_object_get_member(root, member);

    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;

    return json_node_get_array(node);
}

/* The array member of an object, or NULL. */
static JsonArray *
object_array(JsonObject *object, const gchar *member)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, member))
        return NULL;

    node = json_object_get_member(object, member);

    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;

    return json_node_get_array(node);
}

/* An object element, or NULL for anything else in the array. */
static JsonObject *
array_object(JsonArray *array, guint index)
{
    JsonNode *node = json_array_get_element(array, index);

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    return json_node_get_object(node);
}

static const gchar *
or_default(const gchar *value, const gchar *fallback)
{
    return (value != NULL) ? value : fallback;
}

/* ── Unread ───────────────────────────────────────────────────────── */

GHashTable *
clawt_lrg_chat_row_rooms(JsonNode *agents, JsonNode *rooms)
{
    GHashTable *rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             NULL);
    JsonArray *array;
    guint i;

    /*
     * Rebuilt from the listings every time rather than kept, so a room or
     * agent removed cannot leave a countable key behind that nothing can
     * clear.
     */
    array = reply_array(agents, "agents");

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        const gchar *dm = clawt_ipc_payload_string(array_object(array, i),
                                                   "dm_room");

        if (dm != NULL && *dm != '\0')
            g_hash_table_add(rows, g_strdup(dm));
    }

    array = reply_array(rooms, "rooms");

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonObject *room = array_object(array, i);
        const gchar *id = clawt_ipc_payload_string(room, "id");

        if (id != NULL && clawt_ipc_payload_boolean(room, "declared", FALSE))
            g_hash_table_add(rows, g_strdup(id));
    }

    return rows;
}

guint
clawt_lrg_unread_count(GHashTable *unread, const gchar *room_id)
{
    if (unread == NULL || room_id == NULL)
        return 0;

    return GPOINTER_TO_UINT(g_hash_table_lookup(unread, room_id));
}

gboolean
clawt_lrg_unread_note(GHashTable *unread, const gchar *room_id,
                      const gchar *viewing_room, const gchar *from,
                      gint64 event_ts, gint64 connected_at, GHashTable *rows)
{
    g_return_val_if_fail(unread != NULL, FALSE);

    /*
     * The rule is libclawt's, because the web client applies the same
     * one and two copies would differ exactly once.  Counted against the
     * room, never the agent: a group room has no agent to key on, and
     * every sidebar row has a room.
     */
    if (!clawt_unread_should_count(room_id, viewing_room, from, event_ts,
                                   connected_at, rows))
        return FALSE;

    g_hash_table_insert(unread, g_strdup(room_id),
                        GUINT_TO_POINTER(clawt_lrg_unread_count(unread,
                                                                room_id) + 1));
    return TRUE;
}

gboolean
clawt_lrg_unread_clear(GHashTable *unread, const gchar *room_id)
{
    if (unread == NULL || room_id == NULL)
        return FALSE;

    return g_hash_table_remove(unread, room_id);
}

gboolean
clawt_lrg_unread_prune(GHashTable *unread, GHashTable *rows)
{
    GHashTableIter iter;
    gpointer key;
    gboolean dropped = FALSE;

    if (unread == NULL || rows == NULL)
        return FALSE;

    g_hash_table_iter_init(&iter, unread);

    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (!g_hash_table_contains(rows, key)) {
            g_hash_table_iter_remove(&iter);
            dropped = TRUE;
        }
    }

    return dropped;
}

guint
clawt_lrg_unread_total(GHashTable *unread)
{
    GHashTableIter iter;
    gpointer value;
    guint total = 0;

    if (unread == NULL)
        return 0;

    g_hash_table_iter_init(&iter, unread);

    while (g_hash_table_iter_next(&iter, NULL, &value))
        total += GPOINTER_TO_UINT(value);

    return total;
}

guint
clawt_lrg_unread_badge(GHashTable *unread, gboolean on_chat)
{
    return on_chat ? 0 : clawt_lrg_unread_total(unread);
}

/* ── Typing ───────────────────────────────────────────────────────── */

gboolean
clawt_lrg_typing_note(GHashTable *typing, const gchar *agent_id,
                      const gchar *room_id, gboolean typing_now)
{
    GHashTable *rooms;
    gboolean was;

    if (typing == NULL || agent_id == NULL)
        return FALSE;

    rooms = g_hash_table_lookup(typing, agent_id);

    if (rooms == NULL) {
        rooms = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_insert(typing, g_strdup(agent_id), rooms);
    }

    was = g_hash_table_size(rooms) > 0;

    /*
     * A set, not a counter: the frames are neither unique nor reliably
     * paired, so "true" twice for one room is one turn and a "false"
     * for a room never raised is nothing.
     */
    if (typing_now)
        g_hash_table_add(rooms, g_strdup(room_id != NULL ? room_id : ""));
    else
        g_hash_table_remove(rooms, room_id != NULL ? room_id : "");

    return was != (g_hash_table_size(rooms) > 0);
}

gboolean
clawt_lrg_typing_busy(GHashTable *typing, const gchar *agent_id,
                      gboolean listed_busy)
{
    GHashTable *rooms;

    if (typing == NULL || agent_id == NULL)
        return listed_busy;

    rooms = g_hash_table_lookup(typing, agent_id);

    if (rooms == NULL)
        return listed_busy;

    return g_hash_table_size(rooms) > 0;
}

gboolean
clawt_lrg_typing_in_room(GHashTable *typing, const gchar *agent_id,
                         const gchar *room_id)
{
    GHashTable *rooms;

    if (typing == NULL || agent_id == NULL || room_id == NULL)
        return FALSE;

    rooms = g_hash_table_lookup(typing, agent_id);

    return rooms != NULL && g_hash_table_contains(rooms, room_id);
}

gboolean
clawt_lrg_typing_forget(GHashTable *typing, const gchar *agent_id)
{
    GHashTable *rooms;
    gboolean was;

    if (typing == NULL || agent_id == NULL)
        return FALSE;

    rooms = g_hash_table_lookup(typing, agent_id);

    if (rooms == NULL)
        return FALSE;

    was = g_hash_table_size(rooms) > 0;

    /* Kept, empty: "known idle" outranks a listing that may be older. */
    g_hash_table_remove_all(rooms);
    return was;
}

gchar *
clawt_lrg_typing_activity(gboolean typing_now, const gchar *peer)
{
    if (!typing_now)
        return NULL;

    /*
     * Who it is for when it is not for you: "thinking" under a chat
     * while the agent is answering a peer is the lie the sidebar used to
     * tell.
     */
    if (peer != NULL && *peer != '\0' && g_strcmp0(peer, "user") != 0)
        return g_strdup_printf("working for %s", peer);

    return g_strdup("thinking");
}

/* ── Agent rows ───────────────────────────────────────────────────── */

gboolean
clawt_lrg_agent_verb_allowed(const gchar *state, const gchar *verb)
{
    gboolean shadow = g_strcmp0(state, "shadow") == 0;

    if (g_strcmp0(verb, "start") == 0)
        return !shadow && g_strcmp0(state, "running") != 0;

    if (g_strcmp0(verb, "stop") == 0)
        return !shadow && g_strcmp0(state, "stopped") != 0;

    if (g_strcmp0(verb, "restart") == 0)
        return !shadow;

    return g_strcmp0(verb, "delete") == 0;
}

ClawtLrgAgentRow *
clawt_lrg_agent_row_new(JsonObject *agent, GHashTable *unread,
                        GHashTable *typing, gboolean show_descriptions)
{
    ClawtLrgAgentRow *row = g_new0(ClawtLrgAgentRow, 1);
    const gchar *id = or_default(clawt_ipc_payload_string(agent, "id"), "");
    const gchar *description = clawt_ipc_payload_string(agent, "description");
    const gchar *peer = clawt_ipc_payload_string(agent, "peer");
    const gchar *caps = or_default(clawt_ipc_payload_string(agent, "caps"),
                                   "");
    gint64 depth = clawt_ipc_payload_int(agent, "mailbox_depth", 0);
    g_autofree gchar *doing = NULL;
    g_autofree gchar *hold = NULL;
    g_autofree gchar *activity = NULL;

    row->id = g_strdup(id);
    row->name = g_strdup(or_default(clawt_ipc_payload_string(agent, "name"),
                                    *id != '\0' ? id : "?"));
    row->state = g_strdup(or_default(clawt_ipc_payload_string(agent, "state"),
                                     "stopped"));
    row->state_tone = clawt_lrg_agent_state_tone(row->state);
    row->team = g_strdup(or_default(clawt_ipc_payload_string(agent, "team"),
                                    ""));
    row->dm_room = g_strdup(clawt_ipc_payload_string(agent, "dm_room"));
    row->color = g_strdup(clawt_ipc_payload_string(agent, "color"));
    row->computer = g_strdup(or_default(
        clawt_ipc_payload_string(agent, "computer"), "none"));
    row->description = g_strdup(description);
    row->live = g_strcmp0(row->state, "running") == 0;
    row->has_avatar = clawt_ipc_payload_boolean(agent, "has_avatar", FALSE);
    row->host = strstr(caps, "host-control") != NULL;
    row->can_interrupt = strstr(caps, "interrupt") != NULL;
    row->machine = clawt_ipc_payload_boolean(agent, "computer_machine", FALSE);
    row->stop_removes = clawt_ipc_payload_boolean(agent,
                                                  "computer_stop_removes",
                                                  FALSE);
    row->standing = clawt_team_badge_for(
        clawt_ipc_payload_boolean(agent, "chief_of_staff", FALSE),
        clawt_ipc_payload_string(agent, "team_role"));
    row->unread = clawt_lrg_unread_count(unread, row->dm_room);

    /*
     * On one of its fallbacks: the badge text is the library's, asked by
     * every client, and the sentence is what the daemon wrote.
     */
    if (json_object_has_member(agent, "failover") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(agent, "failover"))) {
        row->fallback = clawt_failover_badge(
            json_object_get_object_member(agent, "failover"));

        if (row->fallback != NULL)
            row->fallback_message = g_strdup(
                clawt_ipc_payload_string(agent, "failover_message"));
    }

    /*
     * Busy from the typing frames when there have been any, because the
     * shell does not re-list the fleet on every frame; from the listing
     * otherwise.  A stopped agent is never mid-turn, whatever was heard.
     */
    row->busy = row->live &&
                clawt_lrg_typing_busy(typing, id,
                                      clawt_ipc_payload_boolean(agent, "busy",
                                                                FALSE));

    /*
     * What it is doing from the library, so this row, the web badge and
     * the CLI's WORK column cannot drift; the hold outranks it, because
     * a draining agent will not start another turn; the queue is merged
     * in because it was the subtitle's in GTK too.
     */
    doing = clawt_agent_activity_label(row->busy, peer);
    hold = clawt_hold_label(clawt_ipc_payload_boolean(agent, "held", FALSE),
                            clawt_ipc_payload_boolean(agent, "draining",
                                                      FALSE));

    if (hold != NULL && depth > 0)
        activity = g_strdup_printf("%s \302\267 %" G_GINT64_FORMAT " waiting",
                                   hold, depth);
    else if (hold != NULL)
        activity = g_steal_pointer(&hold);
    else if (doing != NULL && depth > 0)
        activity = g_strdup_printf("%s \302\267 %" G_GINT64_FORMAT " waiting",
                                   doing, depth);
    else if (doing != NULL)
        activity = g_steal_pointer(&doing);
    else if (depth > 0)
        activity = g_strdup_printf("%" G_GINT64_FORMAT " waiting to be read",
                                   depth);

    /*
     * The activity is never hidden: it is status, not description, and
     * a list that stops saying who is working answers a different
     * question from the one the setting was turned off to answer.
     */
    if (activity != NULL)
        row->subtitle = g_steal_pointer(&activity);
    else if (show_descriptions && description != NULL && *description != '\0')
        row->subtitle = g_strdup(description);

    return row;
}

void
clawt_lrg_agent_row_free(ClawtLrgAgentRow *row)
{
    if (row == NULL)
        return;

    g_free(row->id);
    g_free(row->name);
    g_free(row->subtitle);
    g_free(row->description);
    g_free(row->state);
    g_free(row->team);
    g_free(row->dm_room);
    g_free(row->color);
    g_free(row->computer);
    g_free(row->fallback);
    g_free(row->fallback_message);
    g_free(row);
}

/* ── The sidebar's order ──────────────────────────────────────────── */

static void
entry_free(gpointer data)
{
    ClawtLrgSidebarEntry *entry = data;

    g_free(entry->id);
    g_free(entry->team);
    g_free(entry->title);
    g_free(entry->subtitle);
    g_free(entry->description);
    g_free(entry);
}

const gchar *
clawt_lrg_team_name(JsonNode *teams, const gchar *team_id)
{
    JsonArray *array = reply_array(teams, "teams");
    guint i;

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonObject *team = array_object(array, i);

        if (g_strcmp0(clawt_ipc_payload_string(team, "id"), team_id) == 0)
            return or_default(clawt_ipc_payload_string(team, "name"),
                              team_id);
    }

    /*
     * A team nobody declared keeps its own name rather than vanishing
     * into an "unknown" bucket: the usual cause is a typo, and hiding it
     * is how a typo survives.
     */
    return team_id;
}

static const gchar *
team_description(JsonNode *teams, const gchar *team_id)
{
    JsonArray *array = reply_array(teams, "teams");
    guint i;

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonObject *team = array_object(array, i);

        if (g_strcmp0(clawt_ipc_payload_string(team, "id"), team_id) == 0)
            return clawt_ipc_payload_string(team, "description");
    }

    return NULL;
}

gchar *
clawt_lrg_team_tally_text(guint running, guint total, guint busy)
{
    /*
     * Running out of total, because "3" could be either; and how many
     * are working, when any are -- one of seven busy is a different fleet
     * from seven of seven, and "0 working" on every idle heading is a
     * count nobody is looking for.
     */
    if (busy > 0)
        return g_strdup_printf("%u working \302\267 %u/%u", busy, running,
                               total);

    return g_strdup_printf("%u/%u", running, total);
}

gchar *
clawt_lrg_room_label(JsonObject *room)
{
    const gchar *id = or_default(clawt_ipc_payload_string(room, "id"), "");
    const gchar *name = clawt_ipc_payload_string(room, "name");
    JsonArray *members = object_array(room, "members");
    GString *out;
    guint i;

    if (name != NULL && *name != '\0' && g_strcmp0(name, id) != 0)
        return g_strdup(name);

    if (members == NULL)
        return g_strdup(id);

    /*
     * Described by who is in it, which is the only other true thing
     * about a room nobody named.  Never by taking its id apart: how a
     * room is spelled is the daemon's business.
     */
    out = g_string_new(NULL);

    for (i = 0; i < json_array_get_length(members); i++) {
        const gchar *member = json_array_get_string_element(members, i);

        if (member == NULL || g_strcmp0(member, "user") == 0)
            continue;

        if (out->len > 0)
            g_string_append(out, ", ");

        g_string_append(out, member);
    }

    if (out->len == 0) {
        g_string_free(out, TRUE);
        return g_strdup(id);
    }

    return g_string_free(out, FALSE);
}

gchar *
clawt_lrg_room_subtitle(JsonObject *room)
{
    JsonArray *members = object_array(room, "members");
    guint count = (members != NULL) ? json_array_get_length(members) : 0;

    /*
     * Whether being in it means answering everything: the thing somebody
     * needs to know about a room before they type in it.
     */
    if (clawt_ipc_payload_boolean(room, "require_mention", FALSE))
        return g_strdup_printf("%u members \302\267 answers when named",
                               count);

    return g_strdup_printf("%u members \302\267 everyone answers", count);
}

typedef struct {
    JsonNode   *teams;
    JsonArray  *agents;
    JsonArray  *rooms;
    GHashTable *collapsed;
    GHashTable *headers;       /* team ids already headed */
    GHashTable *rooms_done;    /* room ids already placed */
    GPtrArray  *out;
} Layout;

static gboolean
layout_collapsed(Layout *layout, const gchar *team)
{
    return layout->collapsed != NULL &&
           g_hash_table_contains(layout->collapsed, team != NULL ? team : "");
}

/* One team's heading, remembered so it is never drawn twice. */
static void
layout_header(Layout *layout, const gchar *team_id)
{
    const gchar *id = (team_id != NULL) ? team_id : "";
    ClawtLrgSidebarEntry *entry;

    if (g_hash_table_contains(layout->headers, id))
        return;

    entry = g_new0(ClawtLrgSidebarEntry, 1);
    entry->kind = CLAWT_LRG_ENTRY_TEAM;
    entry->id = g_strdup(id);
    entry->team = g_strdup(id);
    entry->title = g_strdup((*id != '\0')
                            ? clawt_lrg_team_name(layout->teams, id)
                            : "No team");
    entry->description = g_strdup(team_description(layout->teams, id));
    entry->collapsed = layout_collapsed(layout, id);

    /* NULL and "" name the same group there, as here. */
    if (layout->agents != NULL)
        clawt_team_tally(layout->agents, id, &entry->total, &entry->running,
                         &entry->busy);

    entry->subtitle = clawt_lrg_team_tally_text(entry->running, entry->total,
                                                entry->busy);

    g_ptr_array_add(layout->out, entry);
    g_hash_table_add(layout->headers, g_strdup(id));
}

/*
 * The headings of every group that sorts before @next and has nobody in
 * it, so an empty team is still somewhere to drop an agent.  NULL means
 * everything that is left.  The order is the daemon's: teamless first,
 * then the declared teams as team.list gives them.
 */
static void
layout_empty_headers_before(Layout *layout, const gchar *next)
{
    const gchar *want = (next != NULL) ? next : "";
    gboolean to_the_end = (next == NULL);
    JsonArray *teams = reply_array(layout->teams, "teams");
    guint i;

    /*
     * Teamless comes first and is drawn even when nobody is teamless: it
     * is the only place to drop an agent being taken off a team.
     */
    if (to_the_end || g_strcmp0(want, "") != 0)
        layout_header(layout, "");

    if (!to_the_end && g_strcmp0(want, "") == 0)
        return;

    for (i = 0; teams != NULL && i < json_array_get_length(teams); i++) {
        const gchar *id = or_default(
            clawt_ipc_payload_string(array_object(teams, i), "id"), "");

        if (!to_the_end && g_strcmp0(id, want) == 0)
            return;

        layout_header(layout, id);
    }
}

/*
 * Every declared room in @team's group, after that team's agents.  The
 * collapse rule is here rather than at each call site: a rule applied at
 * two of three call sites moved a folded team's rooms to the bottom
 * instead of hiding them.
 */
static void
layout_rooms_for_team(Layout *layout, const gchar *team)
{
    guint i;

    if (layout->rooms == NULL || layout_collapsed(layout, team))
        return;

    for (i = 0; i < json_array_get_length(layout->rooms); i++) {
        JsonObject *room = array_object(layout->rooms, i);
        const gchar *id = clawt_ipc_payload_string(room, "id");
        const gchar *room_team = or_default(
            clawt_ipc_payload_string(room, "team"), "");
        ClawtLrgSidebarEntry *entry;

        /*
         * Only rooms somebody made.  A direct conversation is already the
         * agent's own row, and a routine's or a trigger's belongs to it.
         */
        if (id == NULL ||
            !clawt_ipc_payload_boolean(room, "declared", FALSE) ||
            g_strcmp0(room_team, team != NULL ? team : "") != 0 ||
            g_hash_table_contains(layout->rooms_done, id))
            continue;

        g_hash_table_add(layout->rooms_done, g_strdup(id));

        entry = g_new0(ClawtLrgSidebarEntry, 1);
        entry->kind = CLAWT_LRG_ENTRY_ROOM;
        entry->id = g_strdup(id);
        entry->team = g_strdup(room_team);
        entry->title = clawt_lrg_room_label(room);
        entry->subtitle = clawt_lrg_room_subtitle(room);
        entry->object = room;
        g_ptr_array_add(layout->out, entry);
    }
}

GPtrArray *
clawt_lrg_sidebar_entries(JsonNode *agents, JsonNode *teams, JsonNode *rooms,
                          GHashTable *collapsed)
{
    Layout layout;
    g_autofree gchar *shown_team = NULL;
    guint n_agents;
    guint i;

    memset(&layout, 0, sizeof(layout));
    layout.teams = teams;
    layout.agents = reply_array(agents, "agents");
    layout.rooms = reply_array(rooms, "rooms");
    layout.collapsed = collapsed;
    layout.headers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                           NULL);
    layout.rooms_done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                              NULL);
    layout.out = g_ptr_array_new_with_free_func(entry_free);

    n_agents = (layout.agents != NULL)
               ? json_array_get_length(layout.agents) : 0;

    if (n_agents == 0) {
        ClawtLrgSidebarEntry *entry = g_new0(ClawtLrgSidebarEntry, 1);

        entry->kind = CLAWT_LRG_ENTRY_EMPTY;
        entry->id = g_strdup("");
        entry->team = g_strdup("");
        entry->title = g_strdup("No agents yet");
        entry->subtitle = g_strdup("Use New agent to add one");
        g_ptr_array_add(layout.out, entry);

        /*
         * And the rooms, which the agent walk below never reaches: a
         * config declaring rooms with every agent removed must not show
         * a sidebar that silently lost them.
         */
        layout_rooms_for_team(&layout, "");
        goto done;
    }

    for (i = 0; i < n_agents; i++) {
        JsonObject *agent = array_object(layout.agents, i);
        const gchar *team = clawt_ipc_payload_string(agent, "team");
        ClawtLrgSidebarEntry *entry;

        if (agent == NULL)
            continue;

        /*
         * A heading whenever the team changes, which works because the
         * daemon returns the fleet grouped; grouping here as well would
         * be a second answer to what order the fleet is in.
         */
        if (i == 0 || g_strcmp0(team, shown_team) != 0) {
            /* the outgoing team's rooms, before the next heading */
            if (i > 0)
                layout_rooms_for_team(&layout, shown_team);

            layout_empty_headers_before(&layout, team != NULL ? team : "");
            layout_header(&layout, team);

            g_free(shown_team);
            shown_team = g_strdup(team);
        }

        /* folded away, but still counted in the heading's tally */
        if (layout_collapsed(&layout, team))
            continue;

        entry = g_new0(ClawtLrgSidebarEntry, 1);
        entry->kind = CLAWT_LRG_ENTRY_AGENT;
        entry->id = g_strdup(or_default(clawt_ipc_payload_string(agent, "id"),
                                        ""));
        entry->team = g_strdup(team != NULL ? team : "");
        entry->title = g_strdup(or_default(
            clawt_ipc_payload_string(agent, "name"), entry->id));
        entry->object = agent;
        g_ptr_array_add(layout.out, entry);
    }

    /* the last team's rooms, which no following heading flushes */
    layout_rooms_for_team(&layout, shown_team);

    /* whatever the fleet declares and nobody is on yet */
    layout_empty_headers_before(&layout, NULL);

    /*
     * And any room whose team holds no agents, which the walk above
     * never stands under: a room that vanished because its team emptied
     * would read as deleted.
     */
    for (i = 0; layout.rooms != NULL &&
                i < json_array_get_length(layout.rooms); i++)
        layout_rooms_for_team(&layout, or_default(
            clawt_ipc_payload_string(array_object(layout.rooms, i), "team"),
            ""));

done:
    g_hash_table_unref(layout.headers);
    g_hash_table_unref(layout.rooms_done);
    return layout.out;
}

gchar *
clawt_lrg_entry_token(const ClawtLrgSidebarEntry *entry)
{
    if (entry == NULL || entry->id == NULL)
        return NULL;

    /*
     * The kind travels with the id: a drop has to know whether to move a
     * room's team or an agent's, and a room read as an agent silently
     * reaches nobody.
     */
    switch (entry->kind) {
    case CLAWT_LRG_ENTRY_AGENT:
        return g_strconcat("a:", entry->id, NULL);
    case CLAWT_LRG_ENTRY_ROOM:
        return g_strconcat("r:", entry->id, NULL);
    case CLAWT_LRG_ENTRY_TEAM:
    case CLAWT_LRG_ENTRY_EMPTY:
        break;
    }

    return NULL;
}

const gchar *
clawt_lrg_entry_team_of(GPtrArray *entries, const gchar *token)
{
    guint i;

    for (i = 0; entries != NULL && i < entries->len; i++) {
        ClawtLrgSidebarEntry *entry = g_ptr_array_index(entries, i);
        g_autofree gchar *mine = clawt_lrg_entry_token(entry);

        if (mine != NULL && g_strcmp0(mine, token) == 0)
            return entry->team;
    }

    return NULL;
}

gchar *
clawt_lrg_reorder_entries(GPtrArray *entries, const gchar *dragged,
                          const gchar *onto, gboolean after)
{
    GString *ids;
    gboolean landed = FALSE;
    guint i;

    if (dragged == NULL || onto == NULL || g_strcmp0(dragged, onto) == 0)
        return NULL;

    ids = g_string_new(NULL);

    for (i = 0; entries != NULL && i < entries->len; i++) {
        g_autofree gchar *token =
            clawt_lrg_entry_token(g_ptr_array_index(entries, i));

        /* a heading is neither, and is not part of the order */
        if (token == NULL)
            continue;

        /* taken out of where it was... */
        if (g_strcmp0(token, dragged) == 0)
            continue;

        if (g_strcmp0(token, onto) == 0 && !after) {
            g_string_append_printf(ids, "%s%s", ids->len > 0 ? "," : "",
                                   dragged);
            landed = TRUE;
        }

        g_string_append_printf(ids, "%s%s", ids->len > 0 ? "," : "", token);

        /* ...and put back beside the row it was dropped on */
        if (g_strcmp0(token, onto) == 0 && after) {
            g_string_append_printf(ids, ",%s", dragged);
            landed = TRUE;
        }
    }

    if (!landed) {
        g_string_free(ids, TRUE);
        return NULL;
    }

    return g_string_free(ids, FALSE);
}

void
clawt_lrg_team_choices(JsonNode *teams, const gchar *current, GPtrArray *ids,
                       GPtrArray *labels)
{
    JsonArray *array = reply_array(teams, "teams");
    gboolean known = FALSE;
    guint i;

    g_return_if_fail(ids != NULL && labels != NULL);

    /*
     * "No team" first and always: it is how an agent comes off a team,
     * and where the chief of staff lives.
     */
    g_ptr_array_add(ids, g_strdup(""));
    g_ptr_array_add(labels, g_strdup("No team"));

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonObject *team = array_object(array, i);
        const gchar *id = clawt_ipc_payload_string(team, "id");

        if (id == NULL || *id == '\0')
            continue;

        g_ptr_array_add(ids, g_strdup(id));
        g_ptr_array_add(labels, g_strdup(or_default(
            clawt_ipc_payload_string(team, "name"), id)));

        if (g_strcmp0(id, current) == 0)
            known = TRUE;
    }

    /*
     * The team the agent names even when nobody declared it, or the tick
     * lands on "No team" and moving the agent looks like it did nothing
     * -- the typo being what somebody opened this menu to fix.
     */
    if (current != NULL && *current != '\0' && !known) {
        g_ptr_array_add(ids, g_strdup(current));
        g_ptr_array_add(labels, g_strdup(current));
    }
}

gchar *
clawt_lrg_moved_text(const gchar *who, const gchar *team,
                     const gchar *team_label)
{
    if (team != NULL && *team != '\0')
        return g_strdup_printf("%s moved to %s.", who,
                               team_label != NULL ? team_label : team);

    return g_strdup_printf("%s taken off its team.", who);
}

gchar *
clawt_lrg_power_text(const gchar *agent_id, JsonNode *reply)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    const gchar *state = or_default(clawt_ipc_payload_string(root, "state"),
                                    "?");

    if (clawt_ipc_payload_boolean(root, "removes", FALSE))
        return g_strdup_printf("%s: the machine is %s. It does not survive a "
                               "stop, so starting it builds a fresh one.",
                               agent_id, state);

    return g_strdup_printf("%s: the machine is %s.", agent_id, state);
}

gchar *
clawt_lrg_power_confirm_title(const gchar *verb)
{
    return g_strdup(g_strcmp0(verb, "stop") == 0
                    ? "Stop this machine, and lose it?"
                    : "Restart this machine, and lose it?");
}

gchar *
clawt_lrg_power_confirm_body(const gchar *agent_id)
{
    return g_strdup_printf(
        "%s does not keep its machine across a stop, so everything inside "
        "it goes -- anything the agent installed, and anything it wrote "
        "outside a shared folder.\n\n"
        "Set computer.container.keep to keep it instead.",
        agent_id != NULL ? agent_id : "This agent");
}

gchar *
clawt_lrg_removed_text(const gchar *agent_id, JsonNode *reply)
{
    const gchar *computer = clawt_ipc_payload_string(
        clawt_ipc_reply_object(reply), "computer");

    /*
     * The computer's fate is reported rather than assumed: a teardown
     * that failed does not fail the removal, and "is gone" over a
     * container still running would be a lie.
     */
    if (computer != NULL && g_strcmp0(computer, "removed") == 0)
        return g_strdup_printf("%s and its computer are gone.", agent_id);

    if (computer != NULL)
        return g_strdup_printf("%s is gone; its computer was not removed: %s",
                               agent_id, computer);

    return g_strdup_printf("%s is gone.", agent_id);
}

/* ── Slash commands ───────────────────────────────────────────────── */

/*
 * What "/" offers.  Every one is a shortcut to something that exists
 * elsewhere; none is a second way to do anything.  The same set, the
 * same order and the same words as the GTK composer.
 */
static const ClawtLrgSlashCommand slash_commands[] = {
    { "/help",      NULL,      "list these commands" },
    { "/start",     NULL,      "start this agent" },
    { "/stop",      NULL,      "stop this agent" },
    { "/restart",   NULL,      "restart this agent" },
    { "/interrupt", NULL,      "stop what it is doing now, without stopping it" },
    { "/attach",    NULL,      "pick files to send with the next message" },
    { "/compose",   NULL,      "write the message in $EDITOR (same as Ctrl+G)" },
    { "/edit",      "[file]",  "open a workspace file in $EDITOR" },
    { "/files",     NULL,      "list this agent's workspace files" },
    { "/memory",    "<query>", "search what this agent has remembered" },
    { "/recall",    "<query>", "search what was said, in every room and "
                               "every session" },
    { "/agents",    NULL,      "who is in the fleet" },
    { "/flow",      NULL,      "go to the conversations between agents" },
    { "/tasks",     NULL,      "go to the task board" },
    { "/reset",     NULL,      "start the agent's AI session again, from nothing" },
    { "/retry",     NULL,      "send your last message again" },
    { "/export",    "[org]",   "save the conversation: text, markdown or org" },
    { "/copy",      "[org]",   "copy the conversation: text, markdown or org" },
    { "/clear",     NULL,      "clear this transcript on screen only" },
    { "/new",       NULL,      "create an agent" }
};

const ClawtLrgSlashCommand *
clawt_lrg_slash_commands(guint *n_commands)
{
    if (n_commands != NULL)
        *n_commands = G_N_ELEMENTS(slash_commands);

    return slash_commands;
}

const ClawtLrgSlashCommand *
clawt_lrg_slash_find(const gchar *name)
{
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(slash_commands); i++) {
        if (g_strcmp0(slash_commands[i].name, name) == 0)
            return &slash_commands[i];
    }

    return NULL;
}

gboolean
clawt_lrg_slash_split(const gchar *text, gchar **name, gchar **rest)
{
    g_auto(GStrv) parts = NULL;

    *name = NULL;
    *rest = NULL;

    if (text == NULL || text[0] != '/')
        return FALSE;

    parts = g_strsplit(text, " ", 2);
    *name = g_strdup(parts[0]);

    /*
     * Stripped, and NULL rather than "" when only spaces followed: every
     * command reads "no argument" one way.
     */
    if (parts[1] != NULL) {
        g_strstrip(parts[1]);

        if (*parts[1] != '\0')
            *rest = g_strdup(parts[1]);
    }

    return TRUE;
}

gchar *
clawt_lrg_slash_label(const gchar *name, const gchar *argument)
{
    return g_strdup_printf("%s%s%s", name, argument != NULL ? " " : "",
                           argument != NULL ? argument : "");
}

gchar *
clawt_lrg_slash_help(void)
{
    GString *out = g_string_new(NULL);
    gsize i;

    for (i = 0; i < G_N_ELEMENTS(slash_commands); i++) {
        g_autofree gchar *label = clawt_lrg_slash_label(
            slash_commands[i].name, slash_commands[i].argument);

        g_string_append_printf(out, "%s\n    %s\n", label,
                               slash_commands[i].summary);
    }

    g_string_append(out,
                    "\nCtrl+G writes the message in $EDITOR. Paste an image "
                    "or use /attach to send files.");

    return g_string_free(out, FALSE);
}

gboolean
clawt_lrg_slash_completing(const gchar *text)
{
    return text != NULL && text[0] == '/' && strchr(text, ' ') == NULL;
}

gboolean
clawt_lrg_slash_needs_agent(const gchar *name)
{
    /*
     * Everything that acts on, lists or resends to one agent.  The rest
     * -- help, clear, compose, attach, flow, tasks, new, agents, recall
     * -- mean the same thing in a room.
     */
    static const gchar *const agent_scoped[] = {
        "/start", "/stop", "/restart", "/interrupt", "/reset", "/retry",
        "/export", "/copy", "/files", "/edit", "/memory", NULL
    };

    return g_strv_contains(agent_scoped, name != NULL ? name : "");
}

const gchar *
clawt_lrg_slash_needs_agent_text(gboolean room_selected)
{
    return room_selected
           ? "That command is about one agent, and a room is several. Pick "
             "an agent from the sidebar."
           : "Pick an agent first.";
}

gchar *
clawt_lrg_slash_unknown_text(const gchar *name)
{
    return g_strdup_printf("There is no %s. Type /help for the list.", name);
}

/* ── What the commands print ──────────────────────────────────────── */

gchar *
clawt_lrg_chat_agents_text(JsonNode *reply)
{
    JsonArray *agents = reply_array(reply, "agents");
    GString *out = g_string_new(NULL);
    guint i;

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *one = array_object(agents, i);

        g_string_append_printf(
            out, "%-20s %-10s %s\n",
            or_default(clawt_ipc_payload_string(one, "id"), "?"),
            or_default(clawt_ipc_payload_string(one, "state"), "?"),
            or_default(clawt_ipc_payload_string(one, "description"), ""));
    }

    return g_string_free(out, FALSE);
}

gchar *
clawt_lrg_chat_files_text(JsonNode *reply)
{
    JsonArray *files = reply_array(reply, "files");
    GString *out = g_string_new(NULL);
    guint i;

    for (i = 0; files != NULL && i < json_array_get_length(files); i++) {
        JsonObject *file = array_object(files, i);

        g_string_append_printf(
            out, "%-18s %s\n",
            or_default(clawt_ipc_payload_string(file, "name"), "?"),
            or_default(clawt_ipc_payload_string(file, "title"), ""));
    }

    g_string_append(out, "\n/edit <name> opens one in $EDITOR.");
    return g_string_free(out, FALSE);
}

const gchar *
clawt_lrg_chat_file_path(JsonNode *reply, const gchar *name)
{
    JsonArray *files = reply_array(reply, "files");
    guint i;

    /*
     * The daemon's path, never one built here: it owns where a workspace
     * is, and a client that builds the path can be pointed at somebody
     * else's.
     */
    for (i = 0; files != NULL && i < json_array_get_length(files); i++) {
        JsonObject *file = array_object(files, i);

        if (g_strcmp0(clawt_ipc_payload_string(file, "name"), name) == 0)
            return clawt_ipc_payload_string(file, "path");
    }

    return NULL;
}

gchar *
clawt_lrg_chat_memory_text(JsonNode *reply)
{
    JsonArray *memories = reply_array(reply, "memories");
    GString *out;
    guint i;

    if (memories == NULL || json_array_get_length(memories) == 0)
        return g_strdup("Nothing remembered matches that.");

    out = g_string_new(NULL);

    for (i = 0; i < json_array_get_length(memories); i++) {
        JsonObject *memory = array_object(memories, i);
        const gchar *summary = clawt_ipc_payload_string(memory, "summary");

        g_string_append_printf(
            out, "%s [%s]\n  %s\n",
            or_default(clawt_ipc_payload_string(memory, "id"), "?"),
            or_default(clawt_ipc_payload_string(memory, "category"), "?"),
            summary != NULL
                ? summary
                : or_default(clawt_ipc_payload_string(memory, "content"),
                             ""));
    }

    return g_string_free(out, FALSE);
}

gchar *
clawt_lrg_chat_reset_text(JsonNode *reply)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    gint64 cleared = clawt_ipc_payload_int(root, "sessions_cleared", 0);

    /*
     * `restarted` is written as a boolean by the daemon; the GTK client
     * reads it as an int and so gets 0 for TRUE, never saying the agent
     * restarted.  Read with the boolean reader, as AGENTS.md asks.
     */
    return g_strdup_printf(
        "Session reset: %" G_GINT64_FORMAT " stored session%s cleared%s. "
        "The next thing you say starts a new one.",
        cleared, cleared == 1 ? "" : "s",
        clawt_ipc_payload_boolean(root, "restarted", FALSE)
            ? " and the agent restarted" : "");
}

gchar *
clawt_lrg_chat_interrupt_text(const gchar *agent_id, JsonNode *reply)
{
    gint64 killed = clawt_ipc_payload_int(clawt_ipc_reply_object(reply),
                                          "killed", 0);

    if (killed > 0)
        return g_strdup_printf("Stopped %s: %" G_GINT64_FORMAT
                               " process(es) ended.", agent_id, killed);

    return g_strdup_printf("%s was between turns -- nothing was running to "
                           "stop.", agent_id);
}

const gchar *
clawt_lrg_chat_last_user_message(JsonNode *history)
{
    JsonArray *messages = reply_array(history, "messages");
    const gchar *last = NULL;
    guint i;

    for (i = 0; messages != NULL && i < json_array_get_length(messages); i++) {
        JsonObject *one = array_object(messages, i);

        if (g_strcmp0(clawt_ipc_payload_string(one, "sender"), "user") == 0)
            last = clawt_ipc_payload_string(one, "body");
    }

    return last;
}

gchar *
clawt_lrg_chat_send_note(const gchar *target, JsonNode *reply,
                         gboolean *waiting)
{
    JsonObject *root = clawt_ipc_reply_object(reply);
    const gchar *state = clawt_ipc_payload_string(root, "target_state");

    *waiting = FALSE;

    if (clawt_ipc_payload_boolean(root, "steered", FALSE))
        return g_strdup_printf("%s is mid-turn -- held, and sent when this "
                               "turn ends", target);

    /*
     * A stopped agent accepts the message -- that is what a durable
     * mailbox is for -- but will not answer it, and a spinner that spins
     * for ever is a worse lie than none.
     */
    if (state != NULL && g_strcmp0(state, "running") != 0)
        return g_strdup_printf("%s is %s -- held in its mailbox until it "
                               "starts", target, state);

    *waiting = TRUE;
    return NULL;
}

ClawtExportFormat
clawt_lrg_export_format(const gchar *action)
{
    if (action != NULL && g_str_has_suffix(action, "org"))
        return CLAWT_EXPORT_ORG;

    if (action != NULL && g_str_has_suffix(action, "text"))
        return CLAWT_EXPORT_PLAIN;

    return CLAWT_EXPORT_MARKDOWN;
}

GPtrArray *
clawt_lrg_chat_export_messages(JsonNode *history, const gchar *room_id)
{
    JsonArray *array = reply_array(history, "messages");
    GPtrArray *messages = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_message_free);
    guint i;

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        JsonObject *one = array_object(array, i);
        ClawtMessage *message = clawt_message_new(
            room_id != NULL ? room_id : "",
            or_default(clawt_ipc_payload_string(one, "sender"), "?"),
            or_default(clawt_ipc_payload_string(one, "body"), ""));

        clawt_message_set_timestamp(message,
                                    clawt_ipc_payload_int(one, "ts", 0));
        g_ptr_array_add(messages, message);
    }

    return messages;
}

gchar *
clawt_lrg_task_chip_text(const gchar *task)
{
    if (task == NULL || *task == '\0')
        return NULL;

    return g_strdup_printf("task %.8s", task);
}

gchar *
clawt_lrg_hop_text(gint64 depth)
{
    /* every ordinary message is one hop, and "hop 1" everywhere stops
     * being read */
    if (depth <= 1)
        return NULL;

    return g_strdup_printf("hop %" G_GINT64_FORMAT, depth);
}

gchar *
clawt_lrg_step_line(ClawtTurnStep *step)
{
    g_autofree gchar *text = clawt_turn_step_summary(step);

    /*
     * Reasoning is marked: it is explicitly not what the agent is telling
     * anybody, and presenting it in the answer's voice is how a
     * half-formed thought gets quoted back as a decision.
     */
    if (clawt_turn_step_get_kind(step) == CLAWT_STEP_THINKING)
        return g_strdup_printf("thinking: %s", text);

    return g_steal_pointer(&text);
}

guint
clawt_lrg_steps_before(GPtrArray *steps, guint from, gint64 message_ts)
{
    guint run = from;

    /*
     * clawt_turn_step_precedes(), because the two stamps are in
     * different units and doing it by hand is how every step ends up
     * after every message.
     */
    while (steps != NULL && run < steps->len &&
           clawt_turn_step_precedes(g_ptr_array_index(steps, run),
                                    message_ts))
        run++;

    return run;
}

gboolean
clawt_lrg_run_advance(ClawtLrgRun *run, const gchar *sender, GDateTime *when,
                      gboolean *new_day)
{
    g_autofree gchar *day = g_date_time_format(when, "%Y-%m-%d");
    gboolean fresh_day = FALSE;
    gboolean start;

    start = clawt_chat_run_is_start(run->sender, run->day, sender, day,
                                    &fresh_day);

    g_free(run->day);
    run->day = g_steal_pointer(&day);
    g_free(run->sender);
    run->sender = g_strdup(sender);

    if (new_day != NULL)
        *new_day = fresh_day;

    return start;
}

void
clawt_lrg_run_reset(ClawtLrgRun *run)
{
    g_clear_pointer(&run->sender, g_free);
    g_clear_pointer(&run->day, g_free);
}

/* ── Attachments ──────────────────────────────────────────────────── */

gboolean
clawt_lrg_looks_like_image(const gchar *path)
{
    static const gchar *const extensions[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", NULL
    };
    g_autofree gchar *lowered = NULL;
    gsize i;

    if (path == NULL)
        return FALSE;

    lowered = g_ascii_strdown(path, -1);

    for (i = 0; extensions[i] != NULL; i++) {
        if (g_str_has_suffix(lowered, extensions[i]))
            return TRUE;
    }

    return FALSE;
}

void
clawt_lrg_attachment_ref_free(ClawtLrgAttachmentRef *ref)
{
    if (ref == NULL)
        return;

    g_free(ref->value);
    g_free(ref);
}

GPtrArray *
clawt_lrg_attachment_refs(const gchar *body)
{
    GPtrArray *refs = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_attachment_ref_free);
    g_auto(GStrv) lines = NULL;
    gboolean in_block = FALSE;
    gsize i;

    if (body == NULL || strstr(body, CLAWT_ATTACHMENT_MARKER) == NULL)
        return refs;

    lines = g_strsplit(body, "\n", -1);

    for (i = 0; lines[i] != NULL; i++) {
        const gchar *start;
        ClawtLrgAttachmentRef *ref;

        if (strstr(lines[i], CLAWT_ATTACHMENT_MARKER) != NULL) {
            in_block = TRUE;
            continue;
        }

        if (!in_block)
            continue;

        /*
         * The block is its list items and their indented continuation
         * lines; anything at the left margin ends it, so a path an agent
         * quotes further down does not sprout a preview.  (Leaving on "no
         * slash on this line" cost the first preview: every entry starts
         * with a name line.)
         */
        if (lines[i][0] != '\0' && lines[i][0] != ' ' &&
            lines[i][0] != '-' && lines[i][0] != '\t') {
            in_block = FALSE;
            continue;
        }

        ref = g_new0(ClawtLrgAttachmentRef, 1);

        /* an agent's file is `clawt:<id>`: the bytes are the daemon's */
        start = strstr(lines[i], "clawt:");

        if (start != NULL) {
            ref->daemon_copy = TRUE;
            ref->value = g_strdup(start + strlen("clawt:"));
        } else {
            start = strchr(lines[i], '/');

            if (start == NULL) {
                g_free(ref);
                continue;
            }

            ref->value = g_strdup(start);
        }

        g_strstrip(ref->value);

        /* the container path is in brackets; the host one is not */
        if (*ref->value == '\0' || g_str_has_suffix(ref->value, ")")) {
            clawt_lrg_attachment_ref_free(ref);
            continue;
        }

        g_ptr_array_add(refs, ref);
    }

    return refs;
}

void
clawt_lrg_sent_file_free(ClawtLrgSentFile *file)
{
    if (file == NULL)
        return;

    g_free(file->name);
    g_free(file->host_path);
    g_free(file->path);
    g_free(file);
}

gchar *
clawt_lrg_body_with_attachments(const gchar *body, GPtrArray *files)
{
    GString *out;
    guint i;

    if (files == NULL || files->len == 0)
        return g_strdup(body != NULL ? body : "");

    out = g_string_new(body);

    if (out->len > 0)
        g_string_append(out, "\n\n");

    /*
     * Named in the message, because an agent reads files with its own
     * tools and one it has not been told about is one it will not open.
     */
    g_string_append(out, CLAWT_ATTACHMENT_MARKER
                         ". Open them with your own read tool at the host "
                         "path below; it runs on the host even when your "
                         "shell does not.\n");

    for (i = 0; i < files->len; i++) {
        ClawtLrgSentFile *file = g_ptr_array_index(files, i);

        if (!file->saved) {
            g_string_append_printf(out, "- %s (could not be saved)\n",
                                   file->name);
            continue;
        }

        g_string_append_printf(out, "- %s\n  %s\n", file->name,
                               file->host_path != NULL ? file->host_path : "");

        /*
         * Both paths, host first: given only the container one, an agent
         * could stat the file with computer_exec and never open it.
         */
        if (g_strcmp0(file->host_path, file->path) != 0 && file->path != NULL)
            g_string_append_printf(out, "  (inside your container: %s)\n",
                                   file->path);
    }

    return g_string_free(out, FALSE);
}

gboolean
clawt_lrg_attachment_too_big(gsize size)
{
    return size > 32u * 1024u * 1024u;
}

/* ── Mentions and conversations ───────────────────────────────────── */

GPtrArray *
clawt_lrg_room_roster(JsonNode *rooms, const gchar *room_id)
{
    JsonArray *array = reply_array(rooms, "rooms");
    GPtrArray *roster = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; room_id != NULL && array != NULL &&
                i < json_array_get_length(array); i++) {
        JsonObject *room = array_object(array, i);
        JsonArray *members;
        guint m;

        if (g_strcmp0(clawt_ipc_payload_string(room, "id"), room_id) != 0)
            continue;

        members = object_array(room, "members");

        for (m = 0; members != NULL && m < json_array_get_length(members);
             m++) {
            const gchar *member = json_array_get_string_element(members, m);

            if (member != NULL && *member != '\0')
                g_ptr_array_add(roster, g_strdup(member));
        }

        break;
    }

    return roster;
}

GPtrArray *
clawt_lrg_mention_rows(const gchar *prefix, GPtrArray *roster,
                       gboolean *offer_all)
{
    GPtrArray *candidates;

    *offer_all = FALSE;

    if (prefix == NULL || roster == NULL || roster->len == 0)
        return g_ptr_array_new_with_free_func(g_free);

    candidates = clawt_mention_candidates(prefix, roster);

    /*
     * `@all` last, only on a prefix it starts with, and not at all when a
     * member is literally called `all`: the resolver defers to that
     * member, so two rows inserting the same text would mean two things.
     */
    *offer_all = g_ascii_strncasecmp(CLAWT_MENTION_ALL, prefix,
                                     strlen(prefix)) == 0 &&
                 !g_ptr_array_find_with_equal_func(candidates,
                                                   CLAWT_MENTION_ALL,
                                                   g_str_equal, NULL);

    return candidates;
}

GPtrArray *
clawt_lrg_conversation_peers(JsonNode *rooms, const gchar *agent_id)
{
    JsonArray *array = reply_array(rooms, "rooms");
    GPtrArray *peers = g_ptr_array_new_with_free_func(g_free);
    guint i;

    for (i = 0; agent_id != NULL && array != NULL &&
                i < json_array_get_length(array); i++) {
        JsonArray *members = object_array(array_object(array, i), "members");
        g_autofree const gchar **ids = NULL;
        const gchar *peer;
        guint m;

        if (members == NULL)
            continue;

        ids = g_new0(const gchar *, json_array_get_length(members) + 1);

        for (m = 0; m < json_array_get_length(members); m++)
            ids[m] = json_array_get_string_element(members, m);

        /*
         * Told apart by membership, never by taking `dm:a:b` apart; the
         * operator's own room is always first and is not one of these.
         */
        peer = clawt_chat_conversation_peer(ids, agent_id);

        if (peer == NULL || g_strcmp0(peer, "user") == 0)
            continue;

        g_ptr_array_add(peers, g_strdup(peer));
    }

    return peers;
}

gchar *
clawt_lrg_conversation_label(const gchar *peer)
{
    if (peer == NULL || *peer == '\0')
        return g_strdup("Chat");

    return g_strdup_printf("with %s", peer);
}

JsonNode *
clawt_lrg_history_payload(const gchar *room_entry, const gchar *conversation,
                          const gchar *agent_id, const gchar *limit)
{
    if (room_entry != NULL)
        return clawt_ipc_payload_new("room", room_entry, "as", "user",
                                     "limit", limit, NULL);

    if (agent_id == NULL)
        return NULL;

    if (conversation != NULL)
        return clawt_ipc_payload_new("room", conversation, "as", agent_id,
                                     "limit", limit, NULL);

    return clawt_ipc_payload_new("room", agent_id, "as", "user",
                                 "limit", limit, NULL);
}
