/*
 * lrg-fleet-model.c - Decisions behind Flow, Activity, Skills and Memory
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * See clawt-lrg-fleet-model.h.  Nothing here includes libregnum: every
 * function takes a daemon reply (or a few strings) and returns what a
 * page should draw, so tests/test-lrg-models-fleet.c can check it with
 * no window.  Each member read here was checked against the
 * json_builder_set_member_name() that produces it in src/core/daemon-*.c
 * -- a reader that guesses a name gets its fallback in silence.
 */

#include "clawt-lrg-fleet-model.h"

#include <math.h>
#include <string.h>

/* ── Small readers ────────────────────────────────────────────────── */

/* An array member, or NULL -- json-glib asserts on a missing one. */
static JsonArray *
array_member(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (object == NULL || !json_object_has_member(object, name))
        return NULL;

    node = json_object_get_member(object, name);

    if (node == NULL || !JSON_NODE_HOLDS_ARRAY(node))
        return NULL;

    return json_node_get_array(node);
}

/* A string element of an array, or NULL when it is not a string. */
static const gchar *
array_string(JsonArray *array, guint i)
{
    JsonNode *node = json_array_get_element(array, i);

    if (node == NULL || json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string(node);
}

/* An object element of an array, or NULL. */
static JsonObject *
array_object(JsonArray *array, guint i)
{
    JsonNode *node = json_array_get_element(array, i);

    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;

    return json_node_get_object(node);
}

static GStrv
strv_from_array(JsonArray *array)
{
    GPtrArray *out = g_ptr_array_new();
    guint i;

    for (i = 0; array != NULL && i < json_array_get_length(array); i++) {
        const gchar *s = array_string(array, i);

        if (s != NULL)
            g_ptr_array_add(out, g_strdup(s));
    }

    g_ptr_array_add(out, NULL);
    return (GStrv)g_ptr_array_free(out, FALSE);
}

/* "2026-11-07 14:02" for unix seconds, or NULL for no time at all. */
static gchar *
day_and_clock(gint64 unix_seconds)
{
    g_autoptr(GDateTime) when = NULL;
    g_autofree gchar *day = NULL;
    g_autofree gchar *clock = NULL;

    if (unix_seconds <= 0)
        return NULL;

    when = g_date_time_new_from_unix_local(unix_seconds);

    if (when == NULL)
        return NULL;

    day = g_date_time_format(when, "%Y-%m-%d");
    clock = clawt_chat_time_label(when);

    return g_strdup_printf("%s %s", day, clock);
}

/* ── Flow: the conversation list ──────────────────────────────────── */

void
clawt_lrg_flow_room_free(ClawtLrgFlowRoom *room)
{
    if (room == NULL)
        return;

    g_free(room->id);
    g_free(room->label);
    g_free(room->subtitle);
    g_free(room->count);
    g_strfreev(room->members);
    g_free(room);
}

gchar *
clawt_lrg_flow_room_label(JsonArray *members)
{
    GString *out = g_string_new(NULL);
    guint n = (members != NULL) ? json_array_get_length(members) : 0;
    guint i;

    for (i = 0; i < n; i++) {
        const gchar *member = array_string(members, i);

        if (member == NULL)
            continue;

        if (out->len > 0)
            g_string_append(out, (i + 1 == n) ? " and " : ", ");

        g_string_append(out, g_strcmp0(member, "user") == 0 ? "you" : member);
    }

    return g_string_free(out, FALSE);
}

gboolean
clawt_lrg_flow_room_involves_user(JsonArray *members)
{
    guint i;

    for (i = 0; members != NULL && i < json_array_get_length(members); i++) {
        if (g_strcmp0(array_string(members, i), "user") == 0)
            return TRUE;
    }

    return FALSE;
}

gchar *
clawt_lrg_flow_one_line(const gchar *body, glong limit)
{
    g_autofree gchar *flat = NULL;
    const gchar *newline;

    if (body == NULL)
        return g_strdup("");

    newline = strchr(body, '\n');
    flat = (newline != NULL) ? g_strndup(body, (gsize)(newline - body))
                             : g_strdup(body);
    g_strstrip(flat);

    /*
     * Cut on a character, not a byte: an agent's reply is as likely to
     * hold an em dash as not, and half a UTF-8 sequence is a replacement
     * glyph for the rest of the row.
     */
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

static gint
compare_rooms_by_last(gconstpointer a, gconstpointer b)
{
    const ClawtLrgFlowRoom *left = *(ClawtLrgFlowRoom *const *)a;
    const ClawtLrgFlowRoom *right = *(ClawtLrgFlowRoom *const *)b;

    if (left->last_ts == right->last_ts)
        return g_strcmp0(left->id, right->id);

    return (left->last_ts > right->last_ts) ? -1 : 1;
}

GPtrArray *
clawt_lrg_flow_rooms(JsonNode *reply, gboolean include_user, gint64 now_us)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_flow_room_free);
    JsonArray *rooms = array_member(clawt_ipc_reply_object(reply), "rooms");
    guint i;

    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = array_object(rooms, i);
        JsonArray *members;
        ClawtLrgFlowRoom *row;
        g_autofree gchar *snippet = NULL;
        g_autofree gchar *when = NULL;
        const gchar *sender;
        gint64 messages;

        if (room == NULL)
            continue;

        /*
         * An empty room is one the daemon made because somebody could
         * have talked, not one where anybody did.  A fleet accumulates a
         * direct room per pair, and listing them all buries the few that
         * matter.
         */
        messages = clawt_ipc_payload_int(room, "messages", 0);

        if (messages <= 0)
            continue;

        members = array_member(room, "members");

        if (!include_user && clawt_lrg_flow_room_involves_user(members))
            continue;

        row = g_new0(ClawtLrgFlowRoom, 1);
        row->id = g_strdup(clawt_ipc_payload_string(room, "id"));
        row->label = clawt_lrg_flow_room_label(members);
        row->members = strv_from_array(members);
        row->last_ts = clawt_ipc_payload_int(room, "last_ts", 0);
        row->count = g_strdup_printf("%" G_GINT64_FORMAT, messages);

        /* A room of nobody still needs something to click on. */
        if (*row->label == '\0') {
            g_free(row->label);
            row->label = g_strdup(row->id != NULL ? row->id : "?");
        }

        sender = clawt_ipc_payload_string(room, "last_sender");
        snippet = clawt_lrg_flow_one_line(
            clawt_ipc_payload_string(room, "last_body"), 44);
        when = (row->last_ts > 0)
            ? clawt_time_ago_label(row->last_ts * G_USEC_PER_SEC, now_us)
            : g_strdup("");

        row->subtitle = g_strdup_printf(
            "%s \xc2\xb7 %s: %s", when,
            g_strcmp0(sender, "user") == 0 ? "you"
                : (sender != NULL ? sender : "?"),
            snippet);

        g_ptr_array_add(out, row);
    }

    g_ptr_array_sort(out, compare_rooms_by_last);
    return out;
}

GStrv
clawt_lrg_flow_room_members(JsonNode *room_list, const gchar *room_id)
{
    JsonArray *rooms = array_member(clawt_ipc_reply_object(room_list),
                                    "rooms");
    guint i;

    if (room_id == NULL)
        return NULL;

    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = array_object(rooms, i);

        if (room != NULL &&
            g_strcmp0(clawt_ipc_payload_string(room, "id"), room_id) == 0)
            return strv_from_array(array_member(room, "members"));
    }

    return NULL;
}

gchar *
clawt_lrg_flow_count_label(guint n)
{
    return g_strdup_printf("%u message%s", n, n == 1 ? "" : "s");
}

gchar *
clawt_lrg_flow_task_chip(const gchar *task)
{
    if (task == NULL || *task == '\0')
        return NULL;

    return g_strdup_printf("task %.8s", task);
}

gchar *
clawt_lrg_flow_hop_chip(gint64 depth)
{
    if (depth <= 1)
        return NULL;

    return g_strdup_printf("hop %" G_GINT64_FORMAT, depth);
}

/* ── Flow: the conversation graph ─────────────────────────────────── */

typedef struct {
    gchar *name;
    gchar *color;
    guint  order;
} AgentInfo;

static void
agent_info_free(gpointer data)
{
    AgentInfo *info = data;

    g_free(info->name);
    g_free(info->color);
    g_free(info);
}

struct _ClawtLrgFlowGraph {
    GHashTable *agents;    /* id -> AgentInfo, from agent.list */
    GHashTable *edges;     /* "from\x1fto" -> ClawtLrgFlowEdge */
    GHashTable *seen;      /* message ids already counted */
};

static void
edge_free(gpointer data)
{
    ClawtLrgFlowEdge *edge = data;

    g_free(edge->from);
    g_free(edge->to);
    g_free(edge);
}

static void
node_free(gpointer data)
{
    ClawtLrgFlowNode *node = data;

    g_free(node->id);
    g_free(node->name);
    g_free(node->color);
    g_free(node);
}

ClawtLrgFlowGraph *
clawt_lrg_flow_graph_new(void)
{
    ClawtLrgFlowGraph *graph = g_new0(ClawtLrgFlowGraph, 1);

    graph->agents = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                          agent_info_free);
    graph->edges = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                         edge_free);
    graph->seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                        NULL);

    return graph;
}

void
clawt_lrg_flow_graph_free(ClawtLrgFlowGraph *graph)
{
    if (graph == NULL)
        return;

    g_hash_table_unref(graph->agents);
    g_hash_table_unref(graph->edges);
    g_hash_table_unref(graph->seen);
    g_free(graph);
}

void
clawt_lrg_flow_graph_set_agents(ClawtLrgFlowGraph *graph, JsonNode *agent_list)
{
    JsonArray *agents = array_member(clawt_ipc_reply_object(agent_list),
                                     "agents");
    guint i;

    g_return_if_fail(graph != NULL);

    /*
     * Replaced wholesale: an agent renamed, recoloured or removed must
     * not keep its old look on the graph.  The edges stay -- what was
     * said was said, whoever has since left.
     */
    g_hash_table_remove_all(graph->agents);

    for (i = 0; agents != NULL && i < json_array_get_length(agents); i++) {
        JsonObject *agent = array_object(agents, i);
        const gchar *id = clawt_ipc_payload_string(agent, "id");
        AgentInfo *info;

        if (id == NULL)
            continue;

        info = g_new0(AgentInfo, 1);
        info->name = g_strdup(clawt_ipc_payload_string(agent, "name"));
        info->color = g_strdup(clawt_ipc_payload_string(agent, "color"));
        info->order = i;
        g_hash_table_replace(graph->agents, g_strdup(id), info);
    }
}

static gchar *
edge_key(const gchar *from, const gchar *to)
{
    return g_strconcat(from, "\x1f", to, NULL);
}

gboolean
clawt_lrg_flow_graph_fold(ClawtLrgFlowGraph *graph, const gchar *message_id,
                          const gchar *sender, const gchar *const *members,
                          gint64 ts_us)
{
    gboolean added = FALSE;
    guint i;

    g_return_val_if_fail(graph != NULL, FALSE);

    if (sender == NULL || *sender == '\0' || members == NULL)
        return FALSE;

    /*
     * Counted once, by id: the page reads each room's history and also
     * hears every message live, and a message both of those deliver is
     * still one message.
     */
    if (message_id != NULL && *message_id != '\0') {
        if (g_hash_table_contains(graph->seen, message_id))
            return FALSE;

        g_hash_table_add(graph->seen, g_strdup(message_id));
    }

    for (i = 0; members[i] != NULL; i++) {
        g_autofree gchar *key = NULL;
        ClawtLrgFlowEdge *edge;

        if (g_strcmp0(members[i], sender) == 0)
            continue;

        key = edge_key(sender, members[i]);
        edge = g_hash_table_lookup(graph->edges, key);

        if (edge == NULL) {
            edge = g_new0(ClawtLrgFlowEdge, 1);
            edge->from = g_strdup(sender);
            edge->to = g_strdup(members[i]);
            g_hash_table_insert(graph->edges, g_steal_pointer(&key), edge);
        }

        edge->count++;
        edge->last_us = MAX(edge->last_us, ts_us);
        added = TRUE;
    }

    return added;
}

guint
clawt_lrg_flow_graph_fold_history(ClawtLrgFlowGraph *graph,
                                  const gchar *const *members,
                                  JsonNode *reply)
{
    JsonArray *messages = array_member(clawt_ipc_reply_object(reply),
                                       "messages");
    guint added = 0;
    guint i;

    for (i = 0; messages != NULL && i < json_array_get_length(messages);
         i++) {
        JsonObject *message = array_object(messages, i);

        if (message == NULL)
            continue;

        /* room.history stamps in seconds; the graph keeps microseconds */
        if (clawt_lrg_flow_graph_fold(
                graph, clawt_ipc_payload_string(message, "id"),
                clawt_ipc_payload_string(message, "sender"), members,
                clawt_ipc_payload_int(message, "ts", 0) * G_USEC_PER_SEC))
            added++;
    }

    return added;
}

static gboolean
edge_drawn(const ClawtLrgFlowEdge *edge, gboolean include_user)
{
    return include_user || (g_strcmp0(edge->from, "user") != 0 &&
                            g_strcmp0(edge->to, "user") != 0);
}

const gchar *
clawt_lrg_flow_graph_name(ClawtLrgFlowGraph *graph, const gchar *id)
{
    AgentInfo *info;

    if (g_strcmp0(id, "user") == 0)
        return "You";

    info = (id != NULL) ? g_hash_table_lookup(graph->agents, id) : NULL;

    if (info != NULL && info->name != NULL && *info->name != '\0')
        return info->name;

    return (id != NULL) ? id : "?";
}

/*
 * Fleet order, then the ones the listing does not have (an agent since
 * removed whose words are still in a transcript) by id at the end.
 */
static gint
compare_participants(gconstpointer a, gconstpointer b, gpointer user_data)
{
    ClawtLrgFlowGraph *graph = user_data;
    const gchar *left = *(const gchar *const *)a;
    const gchar *right = *(const gchar *const *)b;
    AgentInfo *li = g_hash_table_lookup(graph->agents, left);
    AgentInfo *ri = g_hash_table_lookup(graph->agents, right);

    if (li != NULL && ri != NULL)
        return (li->order < ri->order) ? -1 : (li->order > ri->order);

    if (li != NULL)
        return -1;

    if (ri != NULL)
        return 1;

    return g_strcmp0(left, right);
}

/* How much room a node card takes, graph units, for the ring's size. */
#define NODE_SPAN   (260.0)
#define NODE_WIDTH  (200.0)
#define NODE_HEIGHT (90.0)
#define RING_MIN    (240.0)

GPtrArray *
clawt_lrg_flow_graph_nodes(ClawtLrgFlowGraph *graph, gboolean include_user)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(node_free);
    g_autoptr(GHashTable) sent = g_hash_table_new(g_str_hash, g_str_equal);
    g_autoptr(GHashTable) received = g_hash_table_new(g_str_hash,
                                                      g_str_equal);
    g_autoptr(GPtrArray) ring = g_ptr_array_new();
    gboolean user_present = FALSE;
    GHashTableIter iter;
    gpointer value;
    gdouble radius;
    guint i;

    g_return_val_if_fail(graph != NULL, out);

    /* who has traffic, and how much each way */
    g_hash_table_iter_init(&iter, graph->edges);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        ClawtLrgFlowEdge *edge = value;

        if (!edge_drawn(edge, include_user))
            continue;

        g_hash_table_insert(sent, edge->from, GUINT_TO_POINTER(
            GPOINTER_TO_UINT(g_hash_table_lookup(sent, edge->from)) +
            edge->count));
        g_hash_table_insert(received, edge->to, GUINT_TO_POINTER(
            GPOINTER_TO_UINT(g_hash_table_lookup(received, edge->to)) +
            edge->count));
    }

    g_hash_table_iter_init(&iter, sent);

    while (g_hash_table_iter_next(&iter, &value, NULL)) {
        if (g_strcmp0(value, "user") == 0)
            user_present = TRUE;
        else if (!g_ptr_array_find_with_equal_func(ring, value, g_str_equal,
                                                   NULL))
            g_ptr_array_add(ring, value);
    }

    g_hash_table_iter_init(&iter, received);

    while (g_hash_table_iter_next(&iter, &value, NULL)) {
        if (g_strcmp0(value, "user") == 0)
            user_present = TRUE;
        else if (!g_ptr_array_find_with_equal_func(ring, value, g_str_equal,
                                                   NULL))
            g_ptr_array_add(ring, value);
    }

    /*
     * Every agent of the fleet sits on the ring, talking or not.  Drawn
     * from traffic alone, a fleet whose agents have not yet spoken to
     * one another showed an empty grid -- which reads as the page being
     * broken rather than as a quiet fleet -- and an agent nobody has
     * written to was missing from the one picture of who talks to whom.
     */
    g_hash_table_iter_init(&iter, graph->agents);

    while (g_hash_table_iter_next(&iter, &value, NULL)) {
        if (!g_ptr_array_find_with_equal_func(ring, value, g_str_equal, NULL))
            g_ptr_array_add(ring, value);
    }

    g_ptr_array_sort_with_data(ring, compare_participants, graph);

    /*
     * A ring big enough that neighbouring cards do not touch: its
     * circumference has to hold every card's span.  Started at the top
     * and walked clockwise, so the first agent of the fleet is at
     * twelve o'clock however many there are.
     */
    radius = MAX(RING_MIN, ((gdouble)ring->len * NODE_SPAN) / (2.0 * G_PI));

    for (i = 0; i < ring->len; i++) {
        const gchar *id = g_ptr_array_index(ring, i);
        AgentInfo *info = g_hash_table_lookup(graph->agents, id);
        ClawtLrgFlowNode *node = g_new0(ClawtLrgFlowNode, 1);
        gdouble angle = (2.0 * G_PI * (gdouble)i) / (gdouble)ring->len -
                        G_PI / 2.0;

        node->id = g_strdup(id);
        node->name = g_strdup(clawt_lrg_flow_graph_name(graph, id));
        node->color = (info != NULL) ? g_strdup(info->color) : NULL;
        node->x = (gfloat)(radius * cos(angle) - NODE_WIDTH / 2.0);
        node->y = (gfloat)(radius * sin(angle) - NODE_HEIGHT / 2.0);
        node->sent = GPOINTER_TO_UINT(g_hash_table_lookup(sent, id));
        node->received = GPOINTER_TO_UINT(g_hash_table_lookup(received, id));
        g_ptr_array_add(out, node);
    }

    if (user_present) {
        ClawtLrgFlowNode *node = g_new0(ClawtLrgFlowNode, 1);

        node->id = g_strdup("user");
        node->name = g_strdup("You");
        node->x = (gfloat)(-NODE_WIDTH / 2.0);
        node->y = (gfloat)(-NODE_HEIGHT / 2.0);
        node->sent = GPOINTER_TO_UINT(g_hash_table_lookup(sent, "user"));
        node->received = GPOINTER_TO_UINT(g_hash_table_lookup(received,
                                                              "user"));
        node->is_user = TRUE;
        g_ptr_array_add(out, node);
    }

    return out;
}

static gint
compare_edges(gconstpointer a, gconstpointer b)
{
    const ClawtLrgFlowEdge *left = *(ClawtLrgFlowEdge *const *)a;
    const ClawtLrgFlowEdge *right = *(ClawtLrgFlowEdge *const *)b;
    gint by_from = g_strcmp0(left->from, right->from);

    return (by_from != 0) ? by_from : g_strcmp0(left->to, right->to);
}

GPtrArray *
clawt_lrg_flow_graph_edges(ClawtLrgFlowGraph *graph, gboolean include_user)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(edge_free);
    GHashTableIter iter;
    gpointer value;

    g_return_val_if_fail(graph != NULL, out);

    g_hash_table_iter_init(&iter, graph->edges);

    while (g_hash_table_iter_next(&iter, NULL, &value)) {
        ClawtLrgFlowEdge *edge = value;
        ClawtLrgFlowEdge *copy;

        if (!edge_drawn(edge, include_user))
            continue;

        copy = g_new0(ClawtLrgFlowEdge, 1);
        copy->from = g_strdup(edge->from);
        copy->to = g_strdup(edge->to);
        copy->count = edge->count;
        copy->last_us = edge->last_us;
        g_ptr_array_add(out, copy);
    }

    g_ptr_array_sort(out, compare_edges);
    return out;
}

const ClawtLrgFlowEdge *
clawt_lrg_flow_graph_edge(ClawtLrgFlowGraph *graph, const gchar *from,
                          const gchar *to)
{
    g_autofree gchar *key = NULL;

    g_return_val_if_fail(graph != NULL, NULL);

    if (from == NULL || to == NULL)
        return NULL;

    key = edge_key(from, to);
    return g_hash_table_lookup(graph->edges, key);
}

gchar *
clawt_lrg_flow_graph_signature(ClawtLrgFlowGraph *graph, gboolean include_user)
{
    g_autoptr(GPtrArray) nodes = clawt_lrg_flow_graph_nodes(graph,
                                                            include_user);
    g_autoptr(GPtrArray) edges = clawt_lrg_flow_graph_edges(graph,
                                                            include_user);
    GString *out = g_string_new(NULL);
    guint i;

    /*
     * Names and colours too: a renamed agent is a changed picture even
     * when nobody said anything new.
     */
    for (i = 0; i < nodes->len; i++) {
        ClawtLrgFlowNode *node = g_ptr_array_index(nodes, i);

        g_string_append_printf(out, "n:%s:%s:%s;", node->id, node->name,
                               node->color != NULL ? node->color : "");
    }

    for (i = 0; i < edges->len; i++) {
        ClawtLrgFlowEdge *edge = g_ptr_array_index(edges, i);

        g_string_append_printf(out, "e:%s>%s:%u;", edge->from, edge->to,
                               edge->count);
    }

    return g_string_free(out, FALSE);
}

gdouble
clawt_lrg_flow_edge_heat(const ClawtLrgFlowEdge *edge, gint64 now_us)
{
    gdouble age;
    gdouble fade;

    g_return_val_if_fail(edge != NULL, CLAWT_LRG_FLOW_HEAT_FLOOR);

    if (edge->last_us <= 0)
        return CLAWT_LRG_FLOW_HEAT_FLOOR;

    age = (gdouble)(now_us - edge->last_us) / (gdouble)G_USEC_PER_SEC;

    /* the first minute is "just happened"; a skewed clock is too */
    if (age <= 60.0)
        return 1.0;

    if (age >= (gdouble)CLAWT_LRG_FLOW_HEAT_SECONDS)
        return CLAWT_LRG_FLOW_HEAT_FLOOR;

    fade = (age - 60.0) / ((gdouble)CLAWT_LRG_FLOW_HEAT_SECONDS - 60.0);
    return 1.0 - fade * (1.0 - CLAWT_LRG_FLOW_HEAT_FLOOR);
}

gchar *
clawt_lrg_flow_port_label(ClawtLrgFlowGraph *graph,
                          const ClawtLrgFlowEdge *edge, gboolean output)
{
    g_return_val_if_fail(graph != NULL && edge != NULL, g_strdup(""));

    if (output)
        return g_strdup_printf("to %s \xc2\xb7 %u",
                               clawt_lrg_flow_graph_name(graph, edge->to),
                               edge->count);

    return g_strdup_printf("from %s",
                           clawt_lrg_flow_graph_name(graph, edge->from));
}

static gboolean
members_contain(JsonArray *members, const gchar *id)
{
    guint i;

    for (i = 0; members != NULL && i < json_array_get_length(members); i++) {
        if (g_strcmp0(array_string(members, i), id) == 0)
            return TRUE;
    }

    return FALSE;
}

gchar *
clawt_lrg_flow_room_between(JsonNode *room_list, const gchar *a,
                            const gchar *b)
{
    JsonArray *rooms = array_member(clawt_ipc_reply_object(room_list),
                                    "rooms");
    const gchar *pair = NULL;
    const gchar *busiest = NULL;
    gint64 busiest_ts = -1;
    guint i;

    if (a == NULL || b == NULL)
        return NULL;

    for (i = 0; rooms != NULL && i < json_array_get_length(rooms); i++) {
        JsonObject *room = array_object(rooms, i);
        JsonArray *members = array_member(room, "members");
        gint64 last;

        if (room == NULL || !members_contain(members, a) ||
            !members_contain(members, b))
            continue;

        if (json_array_get_length(members) == 2 && pair == NULL)
            pair = clawt_ipc_payload_string(room, "id");

        last = clawt_ipc_payload_int(room, "last_ts", 0);

        if (last > busiest_ts) {
            busiest_ts = last;
            busiest = clawt_ipc_payload_string(room, "id");
        }
    }

    return g_strdup(pair != NULL ? pair : busiest);
}

/* ── Activity ─────────────────────────────────────────────────────── */

void
clawt_lrg_turn_free(ClawtLrgTurn *turn)
{
    if (turn == NULL)
        return;

    g_free(turn->key);
    g_free(turn->agent_id);
    g_free(turn->title);
    g_free(turn->origin);
    g_free(turn->task);
    g_clear_pointer(&turn->steps, g_ptr_array_unref);
    g_free(turn);
}

GPtrArray *
clawt_lrg_activity_turns(JsonNode *reply, gint64 *now_us)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_turn_free);
    JsonObject *root = clawt_ipc_reply_object(reply);
    JsonArray *turns = array_member(root, "turns");
    guint i;

    /* The daemon's clock, so an age is not skewed by this machine's. */
    if (now_us != NULL)
        *now_us = clawt_ipc_payload_int(root, "now", g_get_real_time());

    for (i = 0; turns != NULL && i < json_array_get_length(turns); i++) {
        JsonObject *object = array_object(turns, i);
        JsonArray *steps;
        ClawtLrgTurn *turn;
        const gchar *agent_id;
        const gchar *room_id;
        const gchar *room_name;
        const gchar *origin;
        guint s;

        if (object == NULL)
            continue;

        agent_id = clawt_ipc_payload_string(object, "agent");
        room_id = clawt_ipc_payload_string(object, "room");
        room_name = clawt_ipc_payload_string(object, "room_name");
        origin = clawt_ipc_payload_string(object, "origin");

        if (agent_id == NULL)
            agent_id = "?";

        if (room_name == NULL)
            room_name = room_id;

        turn = g_new0(ClawtLrgTurn, 1);
        turn->agent_id = g_strdup(agent_id);
        turn->key = g_strdup_printf("%s\x1f%s", agent_id,
                                    room_id != NULL ? room_id : "");
        turn->title = g_strdup_printf(
            "%s in %s",
            clawt_ipc_payload_string(object, "agent_name") != NULL
                ? clawt_ipc_payload_string(object, "agent_name") : agent_id,
            (room_name != NULL && *room_name != '\0') ? room_name
                                                      : "an unnamed room");

        /* "for you" says nothing; only a peer or a routine is news */
        if (origin != NULL && g_strcmp0(origin, "user") != 0)
            turn->origin = g_strdup(origin);

        turn->task = g_strdup(clawt_ipc_payload_string(object, "task"));
        turn->started_us = clawt_ipc_payload_int(object, "started", 0);
        turn->running = (guint)MAX(1, clawt_ipc_payload_int(
            object, "running_turns", 1));
        turn->step_count = (guint)MAX(0, clawt_ipc_payload_int(
            object, "step_count", 0));
        turn->steerable = clawt_ipc_payload_boolean(object, "steerable",
                                                    FALSE);
        turn->steps = g_ptr_array_new_with_free_func(
            (GDestroyNotify)clawt_turn_step_free);

        steps = array_member(object, "steps");

        for (s = 0; steps != NULL && s < json_array_get_length(steps); s++) {
            JsonObject *step_object = array_object(steps, s);
            ClawtTurnStep *step = (step_object != NULL)
                ? clawt_turn_step_new_from_object(step_object, agent_id)
                : NULL;

            if (step != NULL)
                g_ptr_array_add(turn->steps, step);
        }

        g_ptr_array_add(out, turn);
    }

    return out;
}

gchar *
clawt_lrg_activity_subtitle(const ClawtLrgTurn *turn, gint64 now_us)
{
    g_autofree gchar *elapsed = NULL;
    GString *out;

    g_return_val_if_fail(turn != NULL, g_strdup(""));

    elapsed = clawt_activity_elapsed_label(turn->started_us, now_us);
    out = g_string_new(NULL);

    g_string_append_printf(out, "running %s", elapsed);

    if (turn->origin != NULL)
        g_string_append_printf(out, " \xc2\xb7 for %s", turn->origin);

    if (turn->task != NULL)
        g_string_append_printf(out, " \xc2\xb7 task %s", turn->task);

    g_string_append_printf(out, " \xc2\xb7 %u step(s)", turn->step_count);

    return g_string_free(out, FALSE);
}

gchar *
clawt_lrg_activity_order(GPtrArray *turns)
{
    GString *out = g_string_new(NULL);
    guint i;

    for (i = 0; turns != NULL && i < turns->len; i++) {
        ClawtLrgTurn *turn = g_ptr_array_index(turns, i);

        g_string_append(out, turn->key);
        g_string_append_c(out, '\n');
    }

    return g_string_free(out, FALSE);
}

void
clawt_lrg_step_clip_free(ClawtLrgStepClip *clip)
{
    if (clip == NULL)
        return;

    g_free(clip->label);
    g_free(clip);
}

/* A clip shorter than this is a sliver nobody can read or hit. */
#define CLIP_MIN_SECONDS (0.2)

GPtrArray *
clawt_lrg_activity_clips(const ClawtLrgTurn *turn, gint64 now_us)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_step_clip_free);
    gdouble previous_end = 0.0;
    guint i;

    g_return_val_if_fail(turn != NULL, out);

    for (i = 0; turn->steps != NULL && i < turn->steps->len; i++) {
        ClawtTurnStep *step = g_ptr_array_index(turn->steps, i);
        gint64 at = clawt_turn_step_get_timestamp(step);
        gint64 until = (i + 1 < turn->steps->len)
            ? clawt_turn_step_get_timestamp(
                  g_ptr_array_index(turn->steps, i + 1))
            : now_us;
        ClawtLrgStepClip *clip = g_new0(ClawtLrgStepClip, 1);
        gdouble start = (turn->started_us > 0)
            ? (gdouble)(at - turn->started_us) / (gdouble)G_USEC_PER_SEC
            : 0.0;
        gdouble end = (turn->started_us > 0)
            ? (gdouble)(until - turn->started_us) / (gdouble)G_USEC_PER_SEC
            : start + CLIP_MIN_SECONDS;

        /*
         * Never before the turn began and never over the clip before it:
         * a timeline track refuses overlapping clips, and a step stamped
         * a hair early by another clock would otherwise be dropped.
         */
        start = MAX(start, MAX(0.0, previous_end));
        end = MAX(end, start + CLIP_MIN_SECONDS);

        clip->start = start;
        clip->duration = end - start;
        clip->label = clawt_turn_step_summary(step);
        clip->tone = clawt_turn_step_tone(step);
        previous_end = end;

        g_ptr_array_add(out, clip);
    }

    return out;
}

gchar *
clawt_lrg_activity_steer_body(const gchar *typed)
{
    g_autofree gchar *body = NULL;

    if (typed == NULL)
        return NULL;

    body = g_strstrip(g_strdup(typed));

    if (*body == '\0')
        return NULL;

    return g_steal_pointer(&body);
}

const gchar *
clawt_lrg_activity_steer_result(JsonNode *reply)
{
    return clawt_ipc_payload_boolean(clawt_ipc_reply_object(reply), "steered",
                                     FALSE)
        ? "Steered: it reads this at its next step."
        : "It had finished, so this went as a new message.";
}

gchar *
clawt_lrg_activity_stop_result(const gchar *agent_id, JsonNode *reply)
{
    gint64 killed = clawt_ipc_payload_int(clawt_ipc_reply_object(reply),
                                          "killed", 0);

    if (killed > 0)
        return g_strdup_printf("Stopped %s.", agent_id);

    return g_strdup_printf("%s had nothing running by the time it was asked.",
                           agent_id);
}

/* ── Skills ───────────────────────────────────────────────────────── */

void
clawt_lrg_note_free(ClawtLrgNote *note)
{
    if (note == NULL)
        return;

    g_free(note->title);
    g_free(note->body);
    g_free(note);
}

static void
add_note(GPtrArray *notes, const gchar *title, const gchar *body,
         const gchar *tone)
{
    ClawtLrgNote *note = g_new0(ClawtLrgNote, 1);

    note->title = g_strdup(title);
    note->body = g_strdup(body != NULL ? body : "");
    note->tone = tone;
    g_ptr_array_add(notes, note);
}

GPtrArray *
clawt_lrg_skill_notes(JsonObject *skill)
{
    GPtrArray *notes = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_note_free);
    JsonArray *warnings = array_member(skill, "warnings");
    JsonArray *skipped = array_member(skill, "skipped");
    const gchar *origin = clawt_ipc_payload_string(skill, "origin");
    const gchar *digest = clawt_ipc_payload_string(skill, "sha256");
    guint i;

    if (!clawt_ipc_payload_boolean(skill, "enabled", FALSE))
        add_note(notes, "Not enabled",
                 "Nothing in this skill reaches an agent yet. Read it first: "
                 "what a skill says goes into a model's context with your "
                 "agent's own authority.", "neutral");

    for (i = 0; warnings != NULL && i < json_array_get_length(warnings); i++)
        add_note(notes, "Warning", array_string(warnings, i), "warn");

    if (skipped != NULL && json_array_get_length(skipped) > 0) {
        g_auto(GStrv) names = strv_from_array(skipped);
        g_autofree gchar *list = g_strjoinv(", ", names);

        add_note(notes, "Not copied", list, "warn");
    }

    {
        const gchar *source = clawt_ipc_payload_string(skill, "source");
        g_autofree gchar *detail = g_strdup_printf(
            "%s%s%s", source != NULL ? source : "user",
            origin != NULL ? ", from " : "",
            origin != NULL ? origin : "");

        add_note(notes, "Provenance", detail, "info");
    }

    /*
     * The digest is part of provenance: it is what says the directory
     * on disk is still the one that was imported and read.
     */
    if (digest != NULL)
        add_note(notes, "sha256", digest, "neutral");

    add_note(notes, "Directory", clawt_ipc_payload_string(skill, "directory"),
             "neutral");

    return notes;
}

gchar *
clawt_lrg_skill_caption(JsonObject *skill)
{
    JsonArray *warnings = array_member(skill, "warnings");
    guint n = (warnings != NULL) ? json_array_get_length(warnings) : 0;
    const gchar *state = clawt_ipc_payload_boolean(skill, "enabled", FALSE)
        ? "Enabled" : "Disabled";

    if (n == 0)
        return g_strdup(state);

    return g_strdup_printf("%s \xc2\xb7 %u warning%s", state, n,
                           n == 1 ? "" : "s");
}

const gchar *
clawt_lrg_skill_tone(JsonObject *skill)
{
    JsonArray *warnings = array_member(skill, "warnings");

    if (warnings != NULL && json_array_get_length(warnings) > 0)
        return "warn";

    return clawt_ipc_payload_boolean(skill, "enabled", FALSE) ? "good"
                                                              : "neutral";
}

void
clawt_lrg_skills_empty(JsonObject *payload, const gchar **title,
                       const gchar **body)
{
    gboolean on = clawt_ipc_payload_boolean(payload, "enabled", FALSE);
    const gchar *directory = clawt_ipc_payload_string(payload, "directory");

    /*
     * Anywhere an empty result could read as an answer, say why it is
     * empty: these two send somebody to different places.
     */
    *title = on ? "No skills yet" : "Skills are turned off";
    *body = on ? (directory != NULL ? directory
                                    : "Nothing in the skills directory.")
               : "Set skills.enabled to scan and link them.";
}

JsonNode *
clawt_lrg_skill_enable_payload(const gchar *name, gboolean enabled)
{
    g_autoptr(JsonBuilder) builder = json_builder_new();

    /* a real boolean: the daemon reads it with the boolean reader */
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "enabled");
    json_builder_add_boolean_value(builder, enabled);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

JsonNode *
clawt_lrg_skill_name_payload(const gchar *name)
{
    return clawt_ipc_payload_new("name", name, NULL);
}

/* Stripped, or NULL when nothing is left. */
static gchar *
typed(const gchar *text)
{
    gchar *out;

    if (text == NULL)
        return NULL;

    out = g_strstrip(g_strdup(text));

    if (*out == '\0') {
        g_free(out);
        return NULL;
    }

    return out;
}

JsonNode *
clawt_lrg_skill_import_payload(const gchar *source, const gchar *origin,
                               gchar **refusal)
{
    g_autofree gchar *where = typed(source);
    g_autofree gchar *from = typed(origin);

    if (where == NULL) {
        if (refusal != NULL)
            *refusal = g_strdup("Name the directory that holds the "
                                "SKILL.md.");
        return NULL;
    }

    return clawt_ipc_payload_new("source", where, "origin", from, NULL);
}

JsonNode *
clawt_lrg_skill_create_payload(const gchar *name, const gchar *description,
                               gchar **refusal)
{
    g_autofree gchar *called = typed(name);
    g_autofree gchar *about = typed(description);

    if (called == NULL) {
        if (refusal != NULL)
            *refusal = g_strdup("A skill needs a name.");
        return NULL;
    }

    return clawt_ipc_payload_new("name", called, "description",
                                 about != NULL ? about : "", NULL);
}

gchar *
clawt_lrg_skill_remove_prompt(const gchar *name)
{
    return g_strdup_printf(
        "Delete the skill \"%s\" and take its links out of every agent's "
        "workspace? The directory and everything in it goes.", name);
}

/* ── Teaching a task ─────────────────────────────────────────────── */

static const ClawtLrgTeachButton running_buttons[] = {
    { CLAWT_LRG_TEACH_STOP, "Stop", "danger" }
};

static const ClawtLrgTeachButton finished_buttons[] = {
    { CLAWT_LRG_TEACH_SHOW,       "Steps",         NULL },
    { CLAWT_LRG_TEACH_SYNTHESIZE, "Draft a skill", "primary" },
    { CLAWT_LRG_TEACH_COMMIT,     "Commit",        NULL },
    { CLAWT_LRG_TEACH_REMOVE,     "Remove",        NULL }
};

const ClawtLrgTeachButton *
clawt_lrg_teach_buttons(gboolean active, guint *n)
{
    if (active) {
        *n = G_N_ELEMENTS(running_buttons);
        return running_buttons;
    }

    *n = G_N_ELEMENTS(finished_buttons);
    return finished_buttons;
}

const gchar *
clawt_lrg_teach_done(ClawtLrgTeachAction action)
{
    switch (action) {
    case CLAWT_LRG_TEACH_SYNTHESIZE:
        return "Drafted. Read it, then commit it.";
    case CLAWT_LRG_TEACH_COMMIT:
        return "Written, and disabled. Read it before you enable it.";
    case CLAWT_LRG_TEACH_STOP:
    case CLAWT_LRG_TEACH_SHOW:
    case CLAWT_LRG_TEACH_REMOVE:
        return NULL;
    }

    return NULL;
}

void
clawt_lrg_recording_free(ClawtLrgRecording *recording)
{
    if (recording == NULL)
        return;

    g_free(recording->id);
    g_free(recording->title);
    g_free(recording->subtitle);
    g_strfreev(recording->caveats);
    g_free(recording);
}

GPtrArray *
clawt_lrg_teach_recordings(JsonNode *reply)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_recording_free);
    JsonArray *recordings = array_member(clawt_ipc_reply_object(reply),
                                         "recordings");
    guint i;

    for (i = 0; recordings != NULL && i < json_array_get_length(recordings);
         i++) {
        JsonObject *trace = array_object(recordings, i);
        ClawtLrgRecording *row;
        const gchar *id;
        const gchar *goal;
        const gchar *agent;
        const gchar *source;

        if (trace == NULL)
            continue;

        id = clawt_ipc_payload_string(trace, "id");
        goal = clawt_ipc_payload_string(trace, "goal");
        agent = clawt_ipc_payload_string(trace, "agent");
        source = clawt_ipc_payload_string(trace, "source");

        row = g_new0(ClawtLrgRecording, 1);
        row->id = g_strdup(id != NULL ? id : "?");
        row->title = g_strdup((goal != NULL && *goal != '\0') ? goal
                                                              : row->id);
        row->active = clawt_ipc_payload_boolean(trace, "active", FALSE);
        row->subtitle = g_strdup_printf(
            "%s, agent %s, %" G_GINT64_FORMAT " step(s)%s",
            source != NULL ? source : "?", agent != NULL ? agent : "-",
            clawt_ipc_payload_int(trace, "step_count", 0),
            row->active ? " -- recording now" : "");
        row->caveats = strv_from_array(array_member(trace, "caveats"));

        g_ptr_array_add(out, row);
    }

    return out;
}

gchar *
clawt_lrg_teach_steps_text(JsonNode *reply)
{
    JsonArray *steps = array_member(clawt_ipc_reply_object(reply), "steps");
    GString *text = g_string_new(NULL);
    guint i;

    for (i = 0; steps != NULL && i < json_array_get_length(steps); i++) {
        JsonObject *step = array_object(steps, i);
        const gchar *kind = clawt_ipc_payload_string(step, "kind");
        const gchar *label = clawt_ipc_payload_string(step, "label");
        const gchar *detail = clawt_ipc_payload_string(step, "detail");

        if (step == NULL)
            continue;

        g_string_append_printf(text, "%u. [%s] %s\n", i + 1,
                               kind != NULL ? kind : "?",
                               label != NULL ? label : "");

        if (detail != NULL)
            g_string_append_printf(text, "    %s\n", detail);
    }

    if (text->len == 0)
        g_string_append(text, "Nothing was captured.");

    return g_string_free(text, FALSE);
}

gchar *
clawt_lrg_teach_draft_text(JsonNode *reply)
{
    JsonObject *draft = clawt_ipc_reply_object(reply);
    const gchar *name = clawt_ipc_payload_string(draft, "name");
    const gchar *description = clawt_ipc_payload_string(draft, "description");
    const gchar *preview = clawt_ipc_payload_string(draft, "preview");
    const gchar *note = clawt_ipc_payload_string(draft, "note");
    GString *text;

    if (name == NULL && preview == NULL)
        return NULL;

    text = g_string_new(NULL);

    if (name != NULL)
        g_string_append_printf(text, "name: %s\n", name);

    if (description != NULL)
        g_string_append_printf(text, "description: %s\n", description);

    if (preview != NULL)
        g_string_append_printf(text, "\n%s\n", preview);

    if (note != NULL)
        g_string_append_printf(text, "\n%s\n", note);

    return g_string_free(text, FALSE);
}

JsonNode *
clawt_lrg_teach_start_payload(const gchar *agent, const gchar *source_nick,
                              const gchar *goal, gchar **refusal)
{
    g_autofree gchar *who = typed(agent);
    g_autofree gchar *what = typed(goal);

    if (who == NULL) {
        if (refusal != NULL)
            *refusal = g_strdup("Choose the agent to record.");
        return NULL;
    }

    return clawt_ipc_payload_new("agent", who, "source", source_nick,
                                 "goal", what, NULL);
}

GStrv
clawt_lrg_teach_sources(GStrv *labels)
{
    guint n = clawt_teach_source_count();
    GStrv nicks = g_new0(gchar *, n + 1);
    guint i;

    *labels = g_new0(gchar *, n + 1);

    for (i = 0; i < n; i++) {
        nicks[i] = g_strdup(clawt_teach_source_nth_nick(i));
        (*labels)[i] = g_strdup(clawt_teach_source_nth_label(i));
    }

    return nicks;
}

/* ── Memory ───────────────────────────────────────────────────────── */

JsonNode *
clawt_lrg_recall_payload(const gchar *query)
{
    g_autofree gchar *what = typed(query);

    if (what == NULL)
        return NULL;

    return clawt_ipc_payload_new("query", what, NULL);
}

void
clawt_lrg_recall_hit_free(ClawtLrgRecallHit *hit)
{
    if (hit == NULL)
        return;

    g_free(hit->body);
    g_free(hit->subtitle);
    g_free(hit->stamp);
    g_free(hit);
}

GPtrArray *
clawt_lrg_recall_hits(JsonNode *reply)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_recall_hit_free);
    JsonArray *hits = array_member(clawt_ipc_reply_object(reply), "hits");
    guint i;

    for (i = 0; hits != NULL && i < json_array_get_length(hits); i++) {
        JsonObject *hit = array_object(hits, i);
        ClawtLrgRecallHit *row;
        const gchar *from;
        const gchar *room;
        const gchar *body;

        if (hit == NULL)
            continue;

        from = clawt_ipc_payload_string(hit, "from_name");

        if (from == NULL)
            from = clawt_ipc_payload_string(hit, "from");

        room = clawt_ipc_payload_string(hit, "room");
        body = clawt_ipc_payload_string(hit, "body");

        row = g_new0(ClawtLrgRecallHit, 1);
        row->body = g_strdup(body != NULL ? body : "");
        row->subtitle = g_strdup_printf("%s in %s",
                                        from != NULL ? from : "?",
                                        room != NULL ? room : "?");
        row->stamp = day_and_clock(clawt_ipc_payload_int(hit, "at", 0));
        g_ptr_array_add(out, row);
    }

    return out;
}

void
clawt_lrg_recall_empty(JsonNode *reply, const gchar **title,
                       const gchar **body)
{
    gint64 indexed = clawt_ipc_payload_int(clawt_ipc_reply_object(reply),
                                           "indexed", 0);

    /*
     * The query reaches FTS5 as one quoted phrase, so a stray quote is
     * searched for rather than being a syntax error -- but an empty
     * index looks exactly like a query that matched nothing.
     */
    *title = (indexed > 0) ? "Nothing matched" : "Nothing has been said yet";
    *body = (indexed > 0)
        ? "Every word is searched as a phrase, so quotes and brackets are "
          "looked for rather than interpreted."
        : "The transcript index fills as the fleet talks.";
}

gboolean
clawt_lrg_recall_is_substring(JsonNode *reply)
{
    JsonObject *root = clawt_ipc_reply_object(reply);

    /* absent is not "no FTS5": an older daemon simply did not say */
    return root != NULL && json_object_has_member(root, "full_text") &&
           !clawt_ipc_payload_boolean(root, "full_text", TRUE);
}

GPtrArray *
clawt_lrg_operator_learned(JsonNode *reply)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_note_free);
    JsonArray *learned = array_member(clawt_ipc_reply_object(reply),
                                      "learned");
    guint i;

    for (i = 0; learned != NULL && i < json_array_get_length(learned); i++) {
        JsonObject *memory = array_object(learned, i);
        gint64 created = clawt_ipc_payload_int(memory, "created_at", 0);
        const gchar *source = clawt_ipc_payload_string(memory, "source");
        g_autofree gchar *day = NULL;
        g_autofree gchar *subtitle = NULL;

        if (memory == NULL)
            continue;

        if (created > 0) {
            g_autoptr(GDateTime) when =
                g_date_time_new_from_unix_local(created);

            if (when != NULL)
                day = g_date_time_format(when, "%Y-%m-%d");
        }

        subtitle = g_strdup_printf("%s%s%s",
                                   source != NULL ? source : "unattributed",
                                   day != NULL ? ", " : "",
                                   day != NULL ? day : "");
        add_note(out, clawt_ipc_payload_string(memory, "content"), subtitle,
                 "info");

        /* add_note() copies a NULL title as NULL; a row needs text */
        {
            ClawtLrgNote *note = g_ptr_array_index(out, out->len - 1);

            if (note->title == NULL)
                note->title = g_strdup("");
        }
    }

    return out;
}

JsonNode *
clawt_lrg_agent_memory_payload(const gchar *agent, const gchar *query,
                               gboolean *search)
{
    g_autofree gchar *what = typed(query);

    *search = (what != NULL);
    return clawt_ipc_payload_new("agent", agent, "query", what, NULL);
}

void
clawt_lrg_memory_row_free(ClawtLrgMemoryRow *row)
{
    if (row == NULL)
        return;

    g_free(row->title);
    g_free(row->body);
    g_free(row->category);
    g_free(row->scope);
    g_free(row->scope_label);
    g_free(row->when);
    g_free(row);
}

GPtrArray *
clawt_lrg_agent_memories(JsonNode *reply, const gchar *scope, gint64 now_us)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_lrg_memory_row_free);
    JsonArray *memories = array_member(clawt_ipc_reply_object(reply),
                                       "memories");
    guint i;

    for (i = 0; memories != NULL && i < json_array_get_length(memories);
         i++) {
        JsonObject *memory = array_object(memories, i);
        const gchar *content;
        const gchar *summary;
        const gchar *row_scope;
        gint64 created;
        ClawtLrgMemoryRow *row;

        if (memory == NULL)
            continue;

        row_scope = clawt_ipc_payload_string(memory, "scope");

        /*
         * A row with no scope came from the agent's own store before
         * scopes existed; it is the agent's, and filtering it out of
         * "This agent only" would hide exactly the oldest memories.
         */
        if (scope != NULL &&
            g_strcmp0(scope, row_scope != NULL ? row_scope : "agent") != 0)
            continue;

        content = clawt_ipc_payload_string(memory, "content");
        summary = clawt_ipc_payload_string(memory, "summary");
        created = clawt_ipc_payload_int(memory, "created_at", 0);

        row = g_new0(ClawtLrgMemoryRow, 1);
        row->body = g_strdup(content != NULL ? content : "");
        row->title = clawt_memory_title(summary, content, 80);
        row->category = g_strdup(clawt_ipc_payload_string(memory,
                                                          "category"));
        row->scope = g_strdup(row_scope);
        row->scope_label = (row_scope != NULL)
            ? g_strdup(row_scope) : NULL;
        row->pinned = clawt_ipc_payload_boolean(memory, "pinned", FALSE);

        /* created_at is unix seconds, like every memory timestamp */
        row->when = (created > 0)
            ? clawt_time_ago_label(created * G_USEC_PER_SEC, now_us)
            : g_strdup("");

        g_ptr_array_add(out, row);
    }

    return out;
}
