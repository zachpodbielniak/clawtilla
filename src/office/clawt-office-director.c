/*
 * clawt-office-director.c - Who stands where: presence and a world into a layout
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include "clawtilla.h"
#include "office/clawt-office-director.h"

#include <string.h>

typedef struct {
    const ClawtOfficeZone *zone;
    GHashTable            *taken;   /* seat index -> participant id */
} Occupancy;

typedef struct {
    ClawtOfficeWorld *world;
    GHashTable       *occupancy;    /* zone id -> Occupancy */

} Director;

static guint
seat_count(const ClawtOfficeZone *zone)
{
    guint two = zone->anchors_2d->len / 2;
    guint three = zone->anchors_3d->len / 3;

    return MAX(MAX(two, three), 1);
}

static Occupancy *
occupancy_of(Director *d, const ClawtOfficeZone *zone)
{
    Occupancy *o = g_hash_table_lookup(d->occupancy, zone->id);

    if (o == NULL) {
        o = g_new0(Occupancy, 1);
        o->zone = zone;
        o->taken = g_hash_table_new(g_direct_hash, g_direct_equal);
        g_hash_table_insert(d->occupancy, zone->id, o);
    }
    return o;
}

static void
occupancy_free(Occupancy *o)
{
    g_hash_table_unref(o->taken);
    g_free(o);
}

/* The first free seat of @zone, or -1 when it is full. */
static gint
free_seat(Director *d, const ClawtOfficeZone *zone)
{
    Occupancy *o = occupancy_of(d, zone);
    guint i;

    for (i = 0; i < seat_count(zone); i++)
        if (!g_hash_table_contains(o->taken, GUINT_TO_POINTER(i + 1)))
            return (gint)i;
    return -1;
}

static void
take(Director *d, const ClawtOfficeZone *zone, guint seat, const gchar *id)
{
    g_hash_table_insert(occupancy_of(d, zone)->taken, GUINT_TO_POINTER(seat + 1),
                        (gpointer)id);
}

/*
 * A seat in a zone tagged @tag: the team's first, then the smallest that
 * fits @need, then any with room, then standing at the last anchor of the
 * first such zone.  NULL when no zone carries @tag.
 */
static const ClawtOfficeZone *
seat_in(Director *d, const gchar *tag, const gchar *team, guint need,
        const gchar *id, guint *out_seat)
{
    const ClawtOfficeZone *first = clawt_office_world_find_zone(d->world, tag, team, need);
    GPtrArray *zones = clawt_office_world_get_zones(d->world);
    guint i;
    gint seat;

    if (first == NULL)
        first = clawt_office_world_find_zone(d->world, tag, team, 0);
    if (first == NULL)
        return NULL;

    seat = free_seat(d, first);
    if (seat >= 0) {
        take(d, first, (guint)seat, id);
        *out_seat = (guint)seat;
        return first;
    }
    /* full: another zone with the tag */
    for (i = 0; i < zones->len; i++) {
        const ClawtOfficeZone *zone = g_ptr_array_index(zones, i);
        g_autofree gchar *probe = NULL;

        if (zone == first || clawt_office_world_find_zone(d->world, tag, zone->team, 0) == NULL)
            continue;
        if (!g_strv_contains((const gchar *const *)zone->tags, tag)) {
            gsize t;
            gboolean prefixed = FALSE;

            for (t = 0; zone->tags[t] != NULL; t++)
                if (g_str_has_prefix(zone->tags[t], tag) && zone->tags[t][strlen(tag)] == ':')
                    prefixed = TRUE;
            if (!prefixed)
                continue;
        }
        seat = free_seat(d, zone);
        if (seat >= 0) {
            take(d, zone, (guint)seat, id);
            *out_seat = (guint)seat;
            return zone;
        }
    }
    /* standing room */
    *out_seat = seat_count(first) - 1;
    return first;
}

/* Where somebody sits when nothing is going on: their own desk. */
static const gchar *
home_tag(JsonObject *p)
{
    const gchar *kind = json_object_get_string_member_with_default(p, "kind", "agent");
    const gchar *role = json_object_get_string_member_with_default(p, "role", NULL);
    gboolean chief = g_strcmp0(role, "chief") == 0;
    gboolean lead = g_strcmp0(role, "lead") == 0;

    if (g_strcmp0(kind, "grok_bot") == 0 || g_strcmp0(kind, "external") == 0)
        return "visitor";
    if (chief)
        return "chief_office";
    if (lead)
        return "lead_office";
    return "desk";
}

static void
add_anchor(JsonBuilder *b, const ClawtOfficeZone *zone, guint seat, gdouble offset)
{
    guint two = zone->anchors_2d->len / 2;
    guint three = zone->anchors_3d->len / 3;

    if (two > 0) {
        guint s = seat % two;

        json_builder_set_member_name(b, "x");
        json_builder_add_double_value(b, g_array_index(zone->anchors_2d, gdouble, s * 2) +
                                         offset * 32.0);
        json_builder_set_member_name(b, "y");
        json_builder_add_double_value(b, g_array_index(zone->anchors_2d, gdouble, s * 2 + 1));
    }
    if (three > 0) {
        guint s = seat % three;

        json_builder_set_member_name(b, "x3");
        json_builder_add_double_value(b, g_array_index(zone->anchors_3d, gdouble, s * 3) +
                                         offset);
        json_builder_set_member_name(b, "z3");
        json_builder_add_double_value(b, g_array_index(zone->anchors_3d, gdouble, s * 3 + 2));
    }
}

static void
add_member(JsonBuilder *b, const gchar *name, const gchar *value)
{
    if (value == NULL)
        return;
    json_builder_set_member_name(b, name);
    json_builder_add_string_value(b, value);
}

static gint
by_id(gconstpointer a, gconstpointer b)
{
    JsonObject *pa = *(JsonObject **)a;
    JsonObject *pb = *(JsonObject **)b;

    return g_strcmp0(json_object_get_string_member(pa, "id"),
                     json_object_get_string_member(pb, "id"));
}

JsonNode *
clawt_office_direct(ClawtOfficeWorld        *world,
                    ClawtOfficeChoreography *rules,
                    JsonObject              *snapshot)
{
    g_autoptr(JsonBuilder) b = json_builder_new();
    g_autoptr(GPtrArray) people = g_ptr_array_new();
    g_autoptr(GHashTable) meeting_size = g_hash_table_new(g_str_hash, g_str_equal);
    /* participant id -> seat + 1, an integer: nothing to free */
    g_autoptr(GHashTable) homes = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GHashTable) home_zone = g_hash_table_new(g_str_hash, g_str_equal);
    JsonArray *list;
    Director d;
    guint pass;
    guint i;

    d.world = world;
    d.occupancy = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                        (GDestroyNotify)occupancy_free);


    list = json_object_has_member(snapshot, "participants")
        ? json_object_get_array_member(snapshot, "participants") : NULL;
    for (i = 0; list != NULL && i < json_array_get_length(list); i++) {
        JsonObject *p = json_array_get_object_element(list, i);
        const gchar *room = json_object_get_string_member_with_default(p, "room", NULL);

        if (g_strcmp0(json_object_get_string_member_with_default(p, "activity", "offline"),
                      "offline") == 0)
            continue;
        g_ptr_array_add(people, p);
        if (room != NULL)
            g_hash_table_insert(meeting_size, (gpointer)room, GUINT_TO_POINTER(
                GPOINTER_TO_UINT(g_hash_table_lookup(meeting_size, room)) + 1));
    }
    /* by id, so the order participants arrived in never moves a chair */
    g_ptr_array_sort(people, by_id);

    /*
     * Every desk first: a desk is somebody's even while they are in a
     * meeting, so a call or a delivery can find them there, and coming
     * back from the meeting finds the same chair.
     */
    for (i = 0; i < people->len; i++) {
        JsonObject *p = g_ptr_array_index(people, i);
        const gchar *id = json_object_get_string_member(p, "id");
        guint seat = 0;
        const ClawtOfficeZone *zone = seat_in(&d, home_tag(p),
            json_object_get_string_member_with_default(p, "team", NULL), 0, id, &seat);

        if (zone == NULL)
            zone = seat_in(&d, "desk", NULL, 0, id, &seat);
        if (zone != NULL) {
            g_hash_table_insert(homes, (gpointer)id, GUINT_TO_POINTER(seat + 1));
            g_hash_table_insert(home_zone, (gpointer)id, (gpointer)zone);
        }
    }

    json_builder_begin_object(b);
    add_member(b, "world", clawt_office_world_get_id(world));
    json_builder_set_member_name(b, "placements");
    json_builder_begin_array(b);

    /* meetings first, as the docs say: several people need seats together */
    for (pass = 0; pass < 2; pass++)
        for (i = 0; i < people->len; i++) {
            JsonObject *p = g_ptr_array_index(people, i);
            const gchar *id = json_object_get_string_member(p, "id");
            const gchar *nick = json_object_get_string_member_with_default(p, "activity", "idle");
            const gchar *room = json_object_get_string_member_with_default(p, "room", NULL);
            const gchar *peer = json_object_get_string_member_with_default(p, "peer", NULL);
            const gchar *team = json_object_get_string_member_with_default(p, "team", NULL);
            ClawtOfficeActivity activity = CLAWT_OFFICE_IDLE;
            ClawtOfficeStation station = clawt_office_station_from_nick(
                json_object_get_string_member_with_default(p, "station", NULL));
            const ClawtOfficeCue *cue;
            const ClawtOfficeZone *zone = NULL;
            const gchar *tag;
            const gchar *fallback = NULL;
            guint seat = 0;
            gdouble offset = 0.0;
            gboolean at_home;

            clawt_office_activity_from_nick(nick, &activity);
            if ((pass == 0) != (activity == CLAWT_OFFICE_IN_MEETING))
                continue;

            cue = clawt_office_choreography_cue(rules, activity, station);
            tag = cue->zone;
            if (activity == CLAWT_OFFICE_IN_MEETING && room != NULL &&
                g_str_has_prefix(room, "town-hall:"))
                tag = "town_hall";

            /*
             * A delivery first: its cue says `desk` too, but the desk is
             * the *assignee's*, and checking "at home" first walked the
             * deliverer to its own chair.
             */
            at_home = g_strcmp0(tag, "desk") == 0;
            /*
             * A visitor from another fleet is a guest whatever it is
             * doing: its meetings, calls and idle hours happen in its own
             * office, so it keeps to the visitor desk and its activity is
             * drawn on it there -- not sent to sit in this fleet's lounge.
             */
            if (g_strcmp0(json_object_get_string_member_with_default(p, "kind", "agent"),
                          "external") == 0)
                at_home = TRUE;
            if (activity == CLAWT_OFFICE_DELIVERING && peer != NULL &&
                g_hash_table_contains(homes, peer)) {
                /* beside the assignee's desk, folder in hand */
                zone = g_hash_table_lookup(home_zone, peer);
                seat = GPOINTER_TO_UINT(g_hash_table_lookup(homes, peer)) - 1;
                offset = 0.8;
            } else if (at_home && g_hash_table_contains(homes, id)) {
                /* desk, office and the thinking chair are the participant's own */
                zone = g_hash_table_lookup(home_zone, id);
                seat = GPOINTER_TO_UINT(g_hash_table_lookup(homes, id)) - 1;
            } else {
                guint need = activity == CLAWT_OFFICE_IN_MEETING && room != NULL
                    ? GPOINTER_TO_UINT(g_hash_table_lookup(meeting_size, room)) : 0;

                zone = seat_in(&d, tag, team, need, id, &seat);
                if (zone == NULL) {
                    fallback = "the world has no zone for this; at the lounge instead";
                    /* a call or work with nowhere special happens at the desk */
                    if ((activity == CLAWT_OFFICE_ON_CALL || activity == CLAWT_OFFICE_WORKING ||
                         activity == CLAWT_OFFICE_WAITING_ON_YOU) &&
                        g_hash_table_contains(homes, id)) {
                        zone = g_hash_table_lookup(home_zone, id);
                        seat = GPOINTER_TO_UINT(g_hash_table_lookup(homes, id)) - 1;
                        fallback = "the world has no zone for this; at the desk instead";
                    } else {
                        zone = seat_in(&d, "lounge", NULL, 0, id, &seat);
                    }
                }
            }

            json_builder_begin_object(b);
            add_member(b, "id", id);
            add_member(b, "activity", nick);
            if (zone != NULL) {
                add_member(b, "zone", zone->id);
                add_member(b, "zone_tag", tag);
                json_builder_set_member_name(b, "seat");
                json_builder_add_int_value(b, seat);
                add_anchor(b, zone, seat, offset);
            }
            add_member(b, "pose", cue->pose);
            add_member(b, "prop", cue->prop);
            add_member(b, "emote", cue->emote);
            add_member(b, "facing", peer);
            add_member(b, "fallback", fallback);
            json_builder_end_object(b);
        }

    json_builder_end_array(b);
    json_builder_end_object(b);
    g_hash_table_unref(d.occupancy);
    return json_builder_get_root(b);
}
