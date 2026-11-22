/*
 * clawt-office-presence.c - What each participant in the office is doing
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "office/clawt-office-presence.h"

#include <string.h>

typedef struct {
    ClawtOfficeParticipantKind kind;
    gchar      *id;
    gchar      *name;
    gchar      *team;
    gchar      *parent;
    gchar      *role;
    gchar      *color;
    gboolean    running;
    gint64      arrived_at;
    gint64      leaving_at;

    GHashTable *turns;          /* room -> gint64 * (rising edge) */
    gint64      step_at;
    ClawtOfficeStation station;

    gchar      *call_peer;
    gint64      call_at;
    gchar      *meeting_room;
    gint64      meeting_at;
    gchar      *delivery_to;
    gint64      delivery_at;
    GHashTable *waiting;        /* decision ids */
    gchar      *error;

    /* what was last reported, for clawt_office_presence_changed_since() */
    gboolean    published;
    ClawtOfficeActivity last_activity;
    ClawtOfficeStation  last_station;
    gchar      *last_peer;
    gchar      *last_room;
    gint64      since;
} Participant;

struct _ClawtOfficePresence {
    GObject     parent_instance;
    gchar      *fleet_id;
    gint64      stale_seconds;
    GHashTable *participants;   /* id -> Participant */
    GPtrArray  *order;          /* ids, in the order first seen */
};

G_DEFINE_TYPE(ClawtOfficePresence, clawt_office_presence, G_TYPE_OBJECT)

static void
participant_free(Participant *p)
{
    g_free(p->id);
    g_free(p->name);
    g_free(p->team);
    g_free(p->parent);
    g_free(p->role);
    g_free(p->color);
    g_hash_table_unref(p->turns);
    g_free(p->call_peer);
    g_free(p->meeting_room);
    g_free(p->delivery_to);
    g_hash_table_unref(p->waiting);
    g_free(p->error);
    g_free(p->last_peer);
    g_free(p->last_room);
    g_free(p);
}

static void
clawt_office_presence_finalize(GObject *object)
{
    ClawtOfficePresence *self = CLAWT_OFFICE_PRESENCE(object);

    g_free(self->fleet_id);
    g_hash_table_unref(self->participants);
    g_ptr_array_unref(self->order);
    G_OBJECT_CLASS(clawt_office_presence_parent_class)->finalize(object);
}

static void
clawt_office_presence_class_init(ClawtOfficePresenceClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = clawt_office_presence_finalize;
}

static void
clawt_office_presence_init(ClawtOfficePresence *self)
{
    self->participants = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                               (GDestroyNotify)participant_free);
    self->order = g_ptr_array_new_with_free_func(g_free);
}

ClawtOfficePresence *
clawt_office_presence_new(const gchar *fleet_id, gint64 stale_seconds)
{
    ClawtOfficePresence *self = g_object_new(CLAWT_TYPE_OFFICE_PRESENCE, NULL);

    self->fleet_id = g_strdup(fleet_id);
    self->stale_seconds = stale_seconds > 0 ? stale_seconds : 600;
    return self;
}

static Participant *
lookup(ClawtOfficePresence *self, const gchar *id)
{
    return id != NULL ? g_hash_table_lookup(self->participants, id) : NULL;
}

static void
replace(gchar **slot, const gchar *value)
{
    if (g_strcmp0(*slot, value) == 0)
        return;
    g_free(*slot);
    *slot = g_strdup(value);
}

gboolean
clawt_office_presence_set_participant(ClawtOfficePresence        *self,
                                      ClawtOfficeParticipantKind  kind,
                                      const gchar                *id,
                                      const gchar                *name,
                                      const gchar                *team,
                                      const gchar                *parent,
                                      gboolean                    running,
                                      gint64                      now)
{
    Participant *p;
    gboolean changed = FALSE;

    g_return_val_if_fail(CLAWT_IS_OFFICE_PRESENCE(self), FALSE);
    g_return_val_if_fail(id != NULL, FALSE);

    p = lookup(self, id);
    if (p == NULL) {
        p = g_new0(Participant, 1);
        p->id = g_strdup(id);
        p->kind = kind;
        p->turns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        p->waiting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        p->since = now;
        g_hash_table_insert(self->participants, p->id, p);
        g_ptr_array_add(self->order, g_strdup(id));
        /* a helper walks in; an agent is simply there */
        if (kind == CLAWT_OFFICE_KIND_EPHEMERAL)
            p->arrived_at = now;
        changed = TRUE;
    }

    if (p->kind != kind || g_strcmp0(p->name, name) != 0 ||
        g_strcmp0(p->team, team) != 0 || g_strcmp0(p->parent, parent) != 0)
        changed = TRUE;
    p->kind = kind;
    replace(&p->name, name);
    replace(&p->team, team);
    replace(&p->parent, parent);

    if (p->running != running) {
        changed = TRUE;
        /* a process that ended has no turns left, whatever was noted */
        if (!running)
            g_hash_table_remove_all(p->turns);
    }
    p->running = running;
    return changed;
}

void
clawt_office_presence_set_role(ClawtOfficePresence *self, const gchar *id,
                               const gchar *role)
{
    Participant *p = lookup(self, id);

    if (p != NULL)
        replace(&p->role, role);
}

void
clawt_office_presence_set_color(ClawtOfficePresence *self, const gchar *id,
                                const gchar *color)
{
    Participant *p = lookup(self, id);

    if (p != NULL)
        replace(&p->color, color);
}

gboolean
clawt_office_presence_remove(ClawtOfficePresence *self, const gchar *id)
{
    guint i;

    g_return_val_if_fail(CLAWT_IS_OFFICE_PRESENCE(self), FALSE);

    if (id == NULL || !g_hash_table_remove(self->participants, id))
        return FALSE;
    for (i = 0; i < self->order->len; i++)
        if (g_strcmp0(g_ptr_array_index(self->order, i), id) == 0) {
            g_ptr_array_remove_index(self->order, i);
            break;
        }
    return TRUE;
}

void
clawt_office_presence_note_turn(ClawtOfficePresence *self, const gchar *id,
                                const gchar *room, gboolean running, gint64 now)
{
    Participant *p = lookup(self, id);

    if (p == NULL || room == NULL)
        return;
    if (running) {
        gint64 *at = g_new(gint64, 1);

        *at = now;
        g_hash_table_replace(p->turns, g_strdup(room), at);
        /* a new turn is a fresh start: the last one's trouble is over */
        g_clear_pointer(&p->error, g_free);
        p->running = TRUE;
    } else {
        g_hash_table_remove(p->turns, room);
    }
}

void
clawt_office_presence_note_step(ClawtOfficePresence *self, const gchar *id,
                                const gchar *room, const gchar *tool, gint64 now)
{
    Participant *p = lookup(self, id);

    if (p == NULL)
        return;
    p->step_at = now;
    p->station = clawt_office_station_for_tool(tool);
    /* a step proves the turn is alive, so it is not stale */
    if (room != NULL && g_hash_table_contains(p->turns, room)) {
        gint64 *at = g_hash_table_lookup(p->turns, room);

        *at = MAX(*at, now - 1);
    }
}

void
clawt_office_presence_note_call(ClawtOfficePresence *self, const gchar *from,
                                const gchar *to, gint64 now)
{
    Participant *a = lookup(self, from);
    Participant *b = lookup(self, to);

    if (a == NULL || b == NULL || a == b)
        return;
    replace(&a->call_peer, to);
    a->call_at = now;
    replace(&b->call_peer, from);
    b->call_at = now;
}

void
clawt_office_presence_note_meeting(ClawtOfficePresence *self, const gchar *room,
                                   const gchar *const *members, gboolean town_hall,
                                   gint64 now)
{
    g_autofree gchar *where = NULL;
    gsize i;

    if (room == NULL || members == NULL)
        return;
    /* the room's id carries the town hall, so the renderer can pick the hall */
    where = town_hall ? g_strdup_printf("town-hall:%s", room) : g_strdup(room);
    for (i = 0; members[i] != NULL; i++) {
        Participant *p = lookup(self, members[i]);

        if (p == NULL)
            continue;
        replace(&p->meeting_room, where);
        p->meeting_at = now;
    }
}

void
clawt_office_presence_note_delivery(ClawtOfficePresence *self, const gchar *from,
                                    const gchar *to, gint64 now)
{
    Participant *p = lookup(self, from);

    if (p == NULL || to == NULL || g_strcmp0(from, to) == 0)
        return;
    replace(&p->delivery_to, to);
    p->delivery_at = now;
}

void
clawt_office_presence_note_waiting(ClawtOfficePresence *self, const gchar *id,
                                   const gchar *decision, gboolean open)
{
    Participant *p = lookup(self, id);
    GHashTableIter iter;
    gpointer value;

    if (decision == NULL)
        return;
    if (p != NULL) {
        if (open)
            g_hash_table_add(p->waiting, g_strdup(decision));
        else
            g_hash_table_remove(p->waiting, decision);
        return;
    }
    /* settled without naming the asker: whoever was holding it */
    if (!open) {
        g_hash_table_iter_init(&iter, self->participants);
        while (g_hash_table_iter_next(&iter, NULL, &value))
            g_hash_table_remove(((Participant *)value)->waiting, decision);
    }
}

void
clawt_office_presence_note_error(ClawtOfficePresence *self, const gchar *id,
                                 const gchar *reason, gint64 now)
{
    Participant *p = lookup(self, id);

    if (p == NULL)
        return;
    replace(&p->error, reason);
}

void
clawt_office_presence_note_leaving(ClawtOfficePresence *self, const gchar *id,
                                   gint64 now)
{
    Participant *p = lookup(self, id);

    if (p != NULL)
        p->leaving_at = now;
}

/* Turns whose rising edge is older than the stale window go: never closed. */
static guint
live_turns(ClawtOfficePresence *self, Participant *p, gint64 now)
{
    GHashTableIter iter;
    gpointer value;
    guint live = 0;

    g_hash_table_iter_init(&iter, p->turns);
    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        gint64 at = *(gint64 *)value;

        if (now - at > self->stale_seconds && now - p->step_at > self->stale_seconds)
            g_hash_table_iter_remove(&iter);
        else
            live++;
    }
    return live;
}

ClawtOfficeActivity
clawt_office_presence_resolve(ClawtOfficePresence  *self,
                              const gchar          *id,
                              gint64                now,
                              ClawtOfficeStation   *out_station,
                              const gchar         **out_peer,
                              const gchar         **out_room)
{
    Participant *p = lookup(self, id);
    guint turns;

    if (out_station != NULL)
        *out_station = CLAWT_OFFICE_STATION_NONE;
    if (out_peer != NULL)
        *out_peer = NULL;
    if (out_room != NULL)
        *out_room = NULL;
    if (p == NULL)
        return CLAWT_OFFICE_OFFLINE;

    if (p->leaving_at > 0)
        return CLAWT_OFFICE_LEAVING;
    if (!p->running)
        return CLAWT_OFFICE_OFFLINE;
    /* the loudest thing first: a person is needed */
    if (g_hash_table_size(p->waiting) > 0)
        return CLAWT_OFFICE_WAITING_ON_YOU;
    if (p->error != NULL)
        return CLAWT_OFFICE_ERROR;
    if (p->meeting_room != NULL && now - p->meeting_at <= CLAWT_OFFICE_MEETING_SECONDS) {
        if (out_room != NULL)
            *out_room = p->meeting_room;
        return CLAWT_OFFICE_IN_MEETING;
    }
    if (p->call_peer != NULL && now - p->call_at <= CLAWT_OFFICE_CALL_SECONDS) {
        if (out_peer != NULL)
            *out_peer = p->call_peer;
        return CLAWT_OFFICE_ON_CALL;
    }
    if (p->delivery_to != NULL && now - p->delivery_at <= CLAWT_OFFICE_DELIVERY_SECONDS) {
        if (out_peer != NULL)
            *out_peer = p->delivery_to;
        return CLAWT_OFFICE_DELIVERING;
    }

    turns = live_turns(self, p, now);
    if (turns > 0) {
        if (p->station != CLAWT_OFFICE_STATION_NONE &&
            now - p->step_at <= CLAWT_OFFICE_STEP_SECONDS) {
            if (out_station != NULL)
                *out_station = p->station;
            return CLAWT_OFFICE_WORKING;
        }
        return CLAWT_OFFICE_THINKING;
    }
    if (p->arrived_at > 0 && now - p->arrived_at <= CLAWT_OFFICE_ARRIVAL_SECONDS)
        return CLAWT_OFFICE_ARRIVING;
    return CLAWT_OFFICE_IDLE;
}

GPtrArray *
clawt_office_presence_changed_since(ClawtOfficePresence *self, gint64 now)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    guint i;

    g_return_val_if_fail(CLAWT_IS_OFFICE_PRESENCE(self), out);

    for (i = 0; i < self->order->len; i++) {
        Participant *p = lookup(self, g_ptr_array_index(self->order, i));
        ClawtOfficeStation station;
        const gchar *peer;
        const gchar *room;
        ClawtOfficeActivity activity;

        if (p == NULL)
            continue;
        activity = clawt_office_presence_resolve(self, p->id, now, &station, &peer, &room);
        if (p->published && activity == p->last_activity &&
            station == p->last_station && g_strcmp0(peer, p->last_peer) == 0 &&
            g_strcmp0(room, p->last_room) == 0)
            continue;
        if (!p->published || activity != p->last_activity)
            p->since = now;
        p->published = TRUE;
        p->last_activity = activity;
        p->last_station = station;
        replace(&p->last_peer, peer);
        replace(&p->last_room, room);
        g_ptr_array_add(out, g_strdup(p->id));
    }
    return out;
}

static void
add_string(JsonBuilder *builder, const gchar *name, const gchar *value)
{
    if (value == NULL)
        return;
    json_builder_set_member_name(builder, name);
    json_builder_add_string_value(builder, value);
}

gboolean
clawt_office_presence_add_participant_json(ClawtOfficePresence *self,
                                           const gchar         *id,
                                           gint64               now,
                                           JsonBuilder         *builder)
{
    Participant *p = lookup(self, id);
    ClawtOfficeStation station;
    const gchar *peer;
    const gchar *room;
    ClawtOfficeActivity activity;

    if (p == NULL)
        return FALSE;
    activity = clawt_office_presence_resolve(self, id, now, &station, &peer, &room);

    json_builder_begin_object(builder);
    add_string(builder, "fleet", self->fleet_id);
    add_string(builder, "kind", clawt_office_kind_nick(p->kind));
    add_string(builder, "id", p->id);
    add_string(builder, "name", p->name != NULL ? p->name : p->id);
    add_string(builder, "team", p->team);
    add_string(builder, "parent", p->parent);
    add_string(builder, "role", p->role);
    add_string(builder, "color", p->color);
    add_string(builder, "activity", clawt_office_activity_nick(activity));
    add_string(builder, "station", clawt_office_station_nick(station));
    add_string(builder, "peer", peer);
    add_string(builder, "room", room);
    if (activity == CLAWT_OFFICE_ERROR)
        add_string(builder, "detail", p->error);
    json_builder_set_member_name(builder, "since");
    json_builder_add_int_value(builder, p->published && p->last_activity == activity
                                            ? p->since : now);
    json_builder_end_object(builder);
    return TRUE;
}

JsonNode *
clawt_office_presence_snapshot(ClawtOfficePresence *self, gint64 now)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();
    guint i;

    json_builder_begin_object(builder);
    add_string(builder, "fleet", self->fleet_id);
    json_builder_set_member_name(builder, "now");
    json_builder_add_int_value(builder, now);
    json_builder_set_member_name(builder, "participants");
    json_builder_begin_array(builder);
    for (i = 0; i < self->order->len; i++)
        clawt_office_presence_add_participant_json(
            self, g_ptr_array_index(self->order, i), now, builder);
    json_builder_end_array(builder);
    json_builder_end_object(builder);
    return json_builder_get_root(builder);
}

/* ── Vocabulary ─────────────────────────────────────────────────── */

/*
 * Tool names are matched by what they do, case-insensitively, over the
 * spellings the CLIs use (Claude Code's Read and Bash, codex's shell,
 * clawtilla's own tools).  Anything unrecognised is the desk: writing
 * is what an agent most often does.
 */
ClawtOfficeStation
clawt_office_station_for_tool(const gchar *tool)
{
    static const struct {
        const gchar *word;
        ClawtOfficeStation station;
    } words[] = {
        { "read", CLAWT_OFFICE_STATION_LIBRARY },
        { "grep", CLAWT_OFFICE_STATION_LIBRARY },
        { "glob", CLAWT_OFFICE_STATION_LIBRARY },
        { "search", CLAWT_OFFICE_STATION_LIBRARY },
        { "fetch", CLAWT_OFFICE_STATION_LIBRARY },
        { "research", CLAWT_OFFICE_STATION_LIBRARY },
        { "recall", CLAWT_OFFICE_STATION_LIBRARY },
        { "memory", CLAWT_OFFICE_STATION_LIBRARY },
        { "browser", CLAWT_OFFICE_STATION_TERMINAL },
        { "display", CLAWT_OFFICE_STATION_TERMINAL },
        { "screen", CLAWT_OFFICE_STATION_TERMINAL },
        { "computer", CLAWT_OFFICE_STATION_LAB },
        { "bash", CLAWT_OFFICE_STATION_LAB },
        { "shell", CLAWT_OFFICE_STATION_LAB },
        { "exec", CLAWT_OFFICE_STATION_LAB },
        { "test", CLAWT_OFFICE_STATION_LAB },
        { "todo", CLAWT_OFFICE_STATION_WHITEBOARD },
        { "plan", CLAWT_OFFICE_STATION_WHITEBOARD },
        { "task", CLAWT_OFFICE_STATION_WHITEBOARD },
        { "delegate", CLAWT_OFFICE_STATION_WHITEBOARD },
        { "ephemeral", CLAWT_OFFICE_STATION_WHITEBOARD },
        { "decision", CLAWT_OFFICE_STATION_WHITEBOARD },
    };
    g_autofree gchar *lower = NULL;
    gsize i;

    if (tool == NULL || *tool == '\0')
        return CLAWT_OFFICE_STATION_NONE;
    lower = g_ascii_strdown(tool, -1);
    for (i = 0; i < G_N_ELEMENTS(words); i++)
        if (strstr(lower, words[i].word) != NULL)
            return words[i].station;
    return CLAWT_OFFICE_STATION_DESK;
}

static const gchar *const activity_nicks[] = {
    "offline", "idle", "working", "thinking", "on_call", "in_meeting",
    "delivering", "waiting_on_you", "error", "arriving", "leaving"
};

const gchar *
clawt_office_activity_nick(ClawtOfficeActivity activity)
{
    return (guint)activity < G_N_ELEMENTS(activity_nicks)
        ? activity_nicks[activity] : "idle";
}

gboolean
clawt_office_activity_from_nick(const gchar *nick, ClawtOfficeActivity *out)
{
    guint i;

    for (i = 0; nick != NULL && i < G_N_ELEMENTS(activity_nicks); i++)
        if (g_strcmp0(nick, activity_nicks[i]) == 0) {
            *out = (ClawtOfficeActivity)i;
            return TRUE;
        }
    return FALSE;
}

guint
clawt_office_activity_count(void)
{
    return G_N_ELEMENTS(activity_nicks);
}

const gchar *
clawt_office_activity_label(ClawtOfficeActivity activity)
{
    switch (activity) {
    case CLAWT_OFFICE_OFFLINE: return "out of the office";
    case CLAWT_OFFICE_IDLE: return "in the lounge";
    case CLAWT_OFFICE_WORKING: return "working";
    case CLAWT_OFFICE_THINKING: return "thinking at their desk";
    case CLAWT_OFFICE_ON_CALL: return "on a call";
    case CLAWT_OFFICE_IN_MEETING: return "in a meeting";
    case CLAWT_OFFICE_DELIVERING: return "handing work over";
    case CLAWT_OFFICE_WAITING_ON_YOU: return "waiting on you";
    case CLAWT_OFFICE_ERROR: return "in the bug corner";
    case CLAWT_OFFICE_ARRIVING: return "just arrived";
    case CLAWT_OFFICE_LEAVING: return "heading out";
    }
    return "in the lounge";
}

const gchar *
clawt_office_activity_tone(ClawtOfficeActivity activity)
{
    switch (activity) {
    case CLAWT_OFFICE_OFFLINE: return "neutral";
    case CLAWT_OFFICE_IDLE: return "neutral";
    case CLAWT_OFFICE_WORKING: return "good";
    case CLAWT_OFFICE_THINKING: return "good";
    case CLAWT_OFFICE_ON_CALL: return "info";
    case CLAWT_OFFICE_IN_MEETING: return "info";
    case CLAWT_OFFICE_DELIVERING: return "info";
    case CLAWT_OFFICE_WAITING_ON_YOU: return "warn";
    case CLAWT_OFFICE_ERROR: return "bad";
    case CLAWT_OFFICE_ARRIVING: return "info";
    case CLAWT_OFFICE_LEAVING: return "neutral";
    }
    return "neutral";
}

const gchar *
clawt_office_station_nick(ClawtOfficeStation station)
{
    switch (station) {
    case CLAWT_OFFICE_STATION_NONE: return "none";
    case CLAWT_OFFICE_STATION_DESK: return "desk";
    case CLAWT_OFFICE_STATION_LIBRARY: return "library";
    case CLAWT_OFFICE_STATION_LAB: return "lab";
    case CLAWT_OFFICE_STATION_WHITEBOARD: return "whiteboard";
    case CLAWT_OFFICE_STATION_TERMINAL: return "terminal";
    }
    return "none";
}

ClawtOfficeStation
clawt_office_station_from_nick(const gchar *nick)
{
    ClawtOfficeStation s;

    for (s = CLAWT_OFFICE_STATION_NONE; s <= CLAWT_OFFICE_STATION_TERMINAL; s++)
        if (g_strcmp0(nick, clawt_office_station_nick(s)) == 0)
            return s;
    return CLAWT_OFFICE_STATION_NONE;
}

const gchar *
clawt_office_kind_nick(ClawtOfficeParticipantKind kind)
{
    switch (kind) {
    case CLAWT_OFFICE_KIND_AGENT: return "agent";
    case CLAWT_OFFICE_KIND_EPHEMERAL: return "ephemeral";
    case CLAWT_OFFICE_KIND_GROK_BOT: return "grok_bot";
    case CLAWT_OFFICE_KIND_EXTERNAL: return "external";
    }
    return "external";
}

static const gchar *
station_phrase(ClawtOfficeStation station)
{
    switch (station) {
    case CLAWT_OFFICE_STATION_NONE: return "working";
    case CLAWT_OFFICE_STATION_DESK: return "writing at their desk";
    case CLAWT_OFFICE_STATION_LIBRARY: return "reading in the library";
    case CLAWT_OFFICE_STATION_LAB: return "at the lab bench running things";
    case CLAWT_OFFICE_STATION_WHITEBOARD: return "planning at the whiteboard";
    case CLAWT_OFFICE_STATION_TERMINAL: return "at a terminal driving a screen";
    }
    return "working";
}

gchar *
clawt_office_roster_line(JsonObject *participant, gint64 now)
{
    const gchar *nick = json_object_get_string_member_with_default(participant,
                                                                   "activity", NULL);
    const gchar *peer = json_object_get_string_member_with_default(participant,
                                                                   "peer", NULL);
    const gchar *room = json_object_get_string_member_with_default(participant,
                                                                   "room", NULL);
    const gchar *detail = json_object_get_string_member_with_default(participant,
                                                                     "detail", NULL);
    gint64 since = json_object_get_int_member_with_default(participant, "since", now);
    ClawtOfficeActivity activity = CLAWT_OFFICE_IDLE;
    g_autofree gchar *what = NULL;
    g_autofree gchar *ago = NULL;

    if (!clawt_office_activity_from_nick(nick, &activity))
        activity = CLAWT_OFFICE_IDLE;

    switch (activity) {
    case CLAWT_OFFICE_ON_CALL:
        what = g_strdup_printf("on a call with %s", peer != NULL ? peer : "somebody");
        break;
    case CLAWT_OFFICE_DELIVERING:
        what = g_strdup_printf("handing work to %s", peer != NULL ? peer : "somebody");
        break;
    case CLAWT_OFFICE_IN_MEETING:
        if (room != NULL && g_str_has_prefix(room, "town-hall:"))
            what = g_strdup_printf("at the town hall (%s)", room + strlen("town-hall:"));
        else
            what = g_strdup_printf("in a meeting in %s", room != NULL ? room : "a room");
        break;
    case CLAWT_OFFICE_WORKING:
        what = g_strdup(station_phrase(clawt_office_station_from_nick(
            json_object_get_string_member_with_default(participant, "station", NULL))));
        break;
    case CLAWT_OFFICE_ERROR:
        what = detail != NULL ? g_strdup_printf("in the bug corner: %s", detail)
                              : g_strdup("in the bug corner");
        break;
    case CLAWT_OFFICE_OFFLINE:
    case CLAWT_OFFICE_IDLE:
    case CLAWT_OFFICE_THINKING:
    case CLAWT_OFFICE_WAITING_ON_YOU:
    case CLAWT_OFFICE_ARRIVING:
    case CLAWT_OFFICE_LEAVING:
        what = g_strdup(clawt_office_activity_label(activity));
        break;
    }

    if (now - since < 60)
        return g_steal_pointer(&what);
    ago = clawt_time_ago_label(since * G_USEC_PER_SEC, now * G_USEC_PER_SEC);
    return g_strdup_printf("%s \xc2\xb7 since %s", what, ago);
}
