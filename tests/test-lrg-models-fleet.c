/*
 * test-lrg-models-fleet.c - The Flow, Activity, Skills and Memory models
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Hermetic: every reply here is a literal shaped like the daemon's own
 * (read off the json_builder_set_member_name() calls in
 * src/core/daemon-room.c, daemon-activity.c, daemon-skill.c,
 * daemon-teach.c and daemon-memory.c), so no daemon, network or display
 * is needed.  The model is #included rather than linked, as the
 * Makefile's test-lrg-models% rule expects.
 */

#include <clawtilla.h>

#include "../clients/lrg/lrg-fleet-model.c"

static JsonNode *
parse(const gchar *json)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;

    g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
    g_assert_no_error(error);

    return json_node_copy(json_parser_get_root(parser));
}

/* ── Flow: the list ───────────────────────────────────────────────── */

static const gchar *room_list_json =
    "{\"rooms\":["
    " {\"id\":\"dm:alpha:beta\",\"name\":\"a-b\",\"members\":[\"alpha\",\"beta\"],"
    "  \"messages\":4,\"last_sender\":\"beta\",\"last_body\":\"done\\nmore\","
    "  \"last_ts\":2000},"
    " {\"id\":\"dm:alpha:user\",\"name\":\"a-u\",\"members\":[\"alpha\",\"user\"],"
    "  \"messages\":9,\"last_sender\":\"user\",\"last_body\":\"hi\","
    "  \"last_ts\":3000},"
    " {\"id\":\"dm:beta:gamma\",\"name\":\"b-g\",\"members\":[\"beta\",\"gamma\"],"
    "  \"messages\":0},"
    " {\"id\":\"crew\",\"name\":\"crew\",\"members\":[\"alpha\",\"beta\",\"gamma\"],"
    "  \"messages\":2,\"last_sender\":\"gamma\",\"last_body\":\"ok\","
    "  \"last_ts\":2500}"
    "]}";

static void
test_flow_rooms(void)
{
    g_autoptr(JsonNode) reply = parse(room_list_json);
    g_autoptr(GPtrArray) without = clawt_lrg_flow_rooms(reply, FALSE,
                                                        (gint64)4000 * G_USEC_PER_SEC);
    g_autoptr(GPtrArray) with = clawt_lrg_flow_rooms(reply, TRUE,
                                                     (gint64)4000 * G_USEC_PER_SEC);
    ClawtLrgFlowRoom *first;

    /* the empty room is never listed; the operator's only on request */
    g_assert_cmpuint(without->len, ==, 2);
    g_assert_cmpuint(with->len, ==, 3);

    /* newest first */
    first = g_ptr_array_index(without, 0);
    g_assert_cmpstr(first->id, ==, "crew");
    g_assert_cmpstr(first->label, ==, "alpha, beta and gamma");
    g_assert_cmpstr(first->count, ==, "2");
    g_assert_nonnull(strstr(first->subtitle, "gamma: ok"));

    first = g_ptr_array_index(with, 0);
    g_assert_cmpstr(first->id, ==, "dm:alpha:user");
    g_assert_cmpstr(first->label, ==, "alpha and you");
    g_assert_nonnull(strstr(first->subtitle, "you: hi"));

    /* only the first line of the last body */
    first = g_ptr_array_index(without, 1);
    g_assert_cmpstr(first->id, ==, "dm:alpha:beta");
    g_assert_nonnull(strstr(first->subtitle, "beta: done"));
    g_assert_null(strstr(first->subtitle, "more"));
    g_assert_cmpstr(first->members[0], ==, "alpha");
    g_assert_cmpstr(first->members[1], ==, "beta");
    g_assert_null(first->members[2]);
}

static void
test_flow_rooms_failure(void)
{
    g_autoptr(GPtrArray) none = clawt_lrg_flow_rooms(NULL, TRUE, 0);
    g_autoptr(JsonNode) empty = json_node_new(JSON_NODE_OBJECT);
    g_autoptr(GPtrArray) hollow = clawt_lrg_flow_rooms(empty, TRUE, 0);

    /* a failed request, and the "object type, no object" node */
    g_assert_cmpuint(none->len, ==, 0);
    g_assert_cmpuint(hollow->len, ==, 0);
}

static void
test_flow_one_line(void)
{
    g_autofree gchar *short_line = clawt_lrg_flow_one_line("  hello \nx", 44);
    g_autofree gchar *cut = clawt_lrg_flow_one_line("abcdef", 3);
    g_autofree gchar *dash = clawt_lrg_flow_one_line(
        "a\xe2\x80\x94" "bcd", 2);
    g_autofree gchar *nothing = clawt_lrg_flow_one_line(NULL, 4);
    g_autofree gchar *invalid = clawt_lrg_flow_one_line("\xff\xfe", 4);

    g_assert_cmpstr(short_line, ==, "hello");
    g_assert_cmpstr(cut, ==, "abc\xe2\x80\xa6");
    /* the em dash is kept whole, never halved */
    g_assert_cmpstr(dash, ==, "a\xe2\x80\x94\xe2\x80\xa6");
    g_assert_cmpstr(nothing, ==, "");
    g_assert_cmpstr(invalid, ==, "");
}

static void
test_flow_chips(void)
{
    g_autofree gchar *task = clawt_lrg_flow_task_chip("1234567890abcdef");
    g_autofree gchar *hop = clawt_lrg_flow_hop_chip(3);
    g_autofree gchar *one = clawt_lrg_flow_count_label(1);
    g_autofree gchar *many = clawt_lrg_flow_count_label(12);
    g_autofree gchar *hop1 = clawt_lrg_flow_hop_chip(1);
    g_autofree gchar *notask = clawt_lrg_flow_task_chip(NULL);

    g_assert_cmpstr(task, ==, "task 12345678");
    g_assert_cmpstr(hop, ==, "hop 3");
    g_assert_null(hop1);
    g_assert_null(notask);
    g_assert_cmpstr(one, ==, "1 message");
    g_assert_cmpstr(many, ==, "12 messages");
}

static void
test_flow_room_members_and_between(void)
{
    g_autoptr(JsonNode) reply = parse(room_list_json);
    g_auto(GStrv) members = clawt_lrg_flow_room_members(reply, "crew");
    g_auto(GStrv) missing = clawt_lrg_flow_room_members(reply, "nope");
    g_autofree gchar *pair = clawt_lrg_flow_room_between(reply, "beta",
                                                         "alpha");
    g_autofree gchar *group = clawt_lrg_flow_room_between(reply, "alpha",
                                                          "gamma");
    g_autofree gchar *none = clawt_lrg_flow_room_between(reply, "gamma",
                                                         "user");

    g_assert_cmpuint(g_strv_length(members), ==, 3);
    g_assert_null(missing);

    /* a room of exactly the two beats a busier group room */
    g_assert_cmpstr(pair, ==, "dm:alpha:beta");
    g_assert_cmpstr(group, ==, "crew");
    g_assert_null(none);
}

/* ── Flow: the graph ──────────────────────────────────────────────── */

static const gchar *agent_list_json =
    "{\"agents\":["
    " {\"id\":\"gamma\",\"name\":\"Gamma\",\"color\":\"#00ff00\"},"
    " {\"id\":\"alpha\",\"name\":\"Alpha\",\"color\":\"#ff0000\"},"
    " {\"id\":\"beta\",\"name\":\"Beta\",\"color\":\"#0000ff\"}"
    "]}";

static ClawtLrgFlowNode *
find_node(GPtrArray *nodes, const gchar *id)
{
    guint i;

    for (i = 0; i < nodes->len; i++) {
        ClawtLrgFlowNode *node = g_ptr_array_index(nodes, i);

        if (g_strcmp0(node->id, id) == 0)
            return node;
    }

    return NULL;
}

static void
test_flow_graph_three_agents(void)
{
    g_autoptr(ClawtLrgFlowGraph) graph = clawt_lrg_flow_graph_new();
    g_autoptr(JsonNode) agents = parse(agent_list_json);
    const gchar *const ab[] = { "alpha", "beta", NULL };
    const gchar *const bg[] = { "beta", "gamma", NULL };
    const ClawtLrgFlowEdge *edge;
    g_autoptr(GPtrArray) nodes = NULL;
    g_autoptr(GPtrArray) edges = NULL;
    ClawtLrgFlowNode *alpha;
    ClawtLrgFlowNode *gamma;

    clawt_lrg_flow_graph_set_agents(graph, agents);

    /* both directions between alpha and beta, one way beta to gamma */
    g_assert_true(clawt_lrg_flow_graph_fold(graph, "m1", "alpha", ab, 100));
    g_assert_true(clawt_lrg_flow_graph_fold(graph, "m2", "alpha", ab, 200));
    g_assert_true(clawt_lrg_flow_graph_fold(graph, "m3", "beta", ab, 300));
    g_assert_true(clawt_lrg_flow_graph_fold(graph, "m4", "beta", bg, 400));

    /* the same message twice is one message */
    g_assert_false(clawt_lrg_flow_graph_fold(graph, "m2", "alpha", ab, 999));

    edge = clawt_lrg_flow_graph_edge(graph, "alpha", "beta");
    g_assert_nonnull(edge);
    g_assert_cmpuint(edge->count, ==, 2);
    g_assert_cmpint(edge->last_us, ==, 200);

    edge = clawt_lrg_flow_graph_edge(graph, "beta", "alpha");
    g_assert_nonnull(edge);
    g_assert_cmpuint(edge->count, ==, 1);
    g_assert_cmpint(edge->last_us, ==, 300);

    edge = clawt_lrg_flow_graph_edge(graph, "beta", "gamma");
    g_assert_nonnull(edge);
    g_assert_cmpuint(edge->count, ==, 1);
    g_assert_null(clawt_lrg_flow_graph_edge(graph, "gamma", "beta"));

    edges = clawt_lrg_flow_graph_edges(graph, FALSE);
    g_assert_cmpuint(edges->len, ==, 3);

    /* stable order: by sender, then recipient */
    g_assert_cmpstr(((ClawtLrgFlowEdge *)g_ptr_array_index(edges, 0))->from,
                    ==, "alpha");
    g_assert_cmpstr(((ClawtLrgFlowEdge *)g_ptr_array_index(edges, 2))->to,
                    ==, "gamma");

    nodes = clawt_lrg_flow_graph_nodes(graph, FALSE);
    g_assert_cmpuint(nodes->len, ==, 3);

    /* fleet order puts gamma first, so gamma is at twelve o'clock */
    gamma = g_ptr_array_index(nodes, 0);
    g_assert_cmpstr(gamma->id, ==, "gamma");
    g_assert_cmpstr(gamma->name, ==, "Gamma");
    g_assert_cmpstr(gamma->color, ==, "#00ff00");
    g_assert_cmpfloat(gamma->y, <, -100.0f);
    g_assert_cmpuint(gamma->sent, ==, 0);
    g_assert_cmpuint(gamma->received, ==, 1);

    alpha = find_node(nodes, "alpha");
    g_assert_nonnull(alpha);
    g_assert_cmpuint(alpha->sent, ==, 2);
    g_assert_cmpuint(alpha->received, ==, 1);

    /* no two cards on the same spot */
    g_assert_true(alpha->x != gamma->x || alpha->y != gamma->y);
}

static void
test_flow_graph_user_and_history(void)
{
    g_autoptr(ClawtLrgFlowGraph) graph = clawt_lrg_flow_graph_new();
    g_autoptr(JsonNode) agents = parse(agent_list_json);
    g_autoptr(JsonNode) history = parse(
        "{\"room\":\"dm:alpha:user\",\"messages\":["
        " {\"id\":\"h1\",\"sender\":\"user\",\"body\":\"go\",\"ts\":10,\"depth\":0},"
        " {\"id\":\"h2\",\"sender\":\"alpha\",\"body\":\"ok\",\"ts\":20,\"depth\":1}"
        "]}");
    const gchar *const au[] = { "alpha", "user", NULL };
    g_autoptr(GPtrArray) without = NULL;
    g_autoptr(GPtrArray) with = NULL;
    g_autofree gchar *sig_without = NULL;
    g_autofree gchar *sig_with = NULL;
    const ClawtLrgFlowEdge *edge;
    ClawtLrgFlowNode *user;

    clawt_lrg_flow_graph_set_agents(graph, agents);

    g_assert_cmpuint(clawt_lrg_flow_graph_fold_history(graph, au, history),
                     ==, 2);

    /* read again: nothing new */
    g_assert_cmpuint(clawt_lrg_flow_graph_fold_history(graph, au, history),
                     ==, 0);

    /* seconds in, microseconds kept */
    edge = clawt_lrg_flow_graph_edge(graph, "alpha", "user");
    g_assert_nonnull(edge);
    g_assert_cmpint(edge->last_us, ==, 20 * G_USEC_PER_SEC);

    /*
     * Every agent of the fleet is on the ring whether or not it talks --
     * an empty grid read as a broken page -- and "You" joins it only
     * when your own chats are included.
     */
    without = clawt_lrg_flow_graph_nodes(graph, FALSE);
    with = clawt_lrg_flow_graph_nodes(graph, TRUE);
    g_assert_cmpuint(without->len, ==, 3);
    g_assert_null(find_node(without, "user"));
    g_assert_cmpuint(with->len, ==, 4);

    user = find_node(with, "user");
    g_assert_nonnull(user);
    g_assert_true(user->is_user);
    g_assert_cmpstr(user->name, ==, "You");

    sig_without = clawt_lrg_flow_graph_signature(graph, FALSE);
    sig_with = clawt_lrg_flow_graph_signature(graph, TRUE);
    /* the ring's nodes, and no wire: the only traffic was with you */
    g_assert_nonnull(strstr(sig_without, "n:alpha:Alpha:"));
    g_assert_null(strstr(sig_without, "e:"));
    g_assert_nonnull(strstr(sig_with, "e:alpha>user:1;"));
}

static void
test_flow_graph_signature_moves(void)
{
    g_autoptr(ClawtLrgFlowGraph) graph = clawt_lrg_flow_graph_new();
    const gchar *const ab[] = { "alpha", "beta", NULL };
    g_autofree gchar *before = NULL;
    g_autofree gchar *after = NULL;
    g_autofree gchar *again = NULL;

    clawt_lrg_flow_graph_fold(graph, NULL, "alpha", ab, 1);
    before = clawt_lrg_flow_graph_signature(graph, FALSE);

    /* a message with no id is always counted */
    clawt_lrg_flow_graph_fold(graph, NULL, "alpha", ab, 2);
    after = clawt_lrg_flow_graph_signature(graph, FALSE);
    again = clawt_lrg_flow_graph_signature(graph, FALSE);

    g_assert_cmpstr(before, !=, after);
    g_assert_cmpstr(after, ==, again);

    /* an unknown agent is titled by its id */
    g_assert_cmpstr(clawt_lrg_flow_graph_name(graph, "alpha"), ==, "alpha");
    g_assert_cmpstr(clawt_lrg_flow_graph_name(graph, "user"), ==, "You");

    /* bad input adds nothing */
    g_assert_false(clawt_lrg_flow_graph_fold(graph, "x", NULL, ab, 1));
    g_assert_false(clawt_lrg_flow_graph_fold(graph, "y", "alpha", NULL, 1));
}

static void
test_flow_heat_and_ports(void)
{
    g_autoptr(ClawtLrgFlowGraph) graph = clawt_lrg_flow_graph_new();
    g_autoptr(JsonNode) agents = parse(agent_list_json);
    ClawtLrgFlowEdge edge = { (gchar *)"alpha", (gchar *)"beta", 7, 0 };
    gint64 now = (gint64)1000000 * G_USEC_PER_SEC;
    g_autofree gchar *out = NULL;
    g_autofree gchar *in = NULL;
    gdouble mid;

    clawt_lrg_flow_graph_set_agents(graph, agents);

    edge.last_us = now - 10 * G_USEC_PER_SEC;
    g_assert_cmpfloat(clawt_lrg_flow_edge_heat(&edge, now), ==, 1.0);

    edge.last_us = now - (gint64)2 * CLAWT_LRG_FLOW_HEAT_SECONDS * G_USEC_PER_SEC;
    g_assert_cmpfloat(clawt_lrg_flow_edge_heat(&edge, now), ==,
                      CLAWT_LRG_FLOW_HEAT_FLOOR);

    edge.last_us = now - 300 * G_USEC_PER_SEC;
    mid = clawt_lrg_flow_edge_heat(&edge, now);
    g_assert_cmpfloat(mid, >, CLAWT_LRG_FLOW_HEAT_FLOOR);
    g_assert_cmpfloat(mid, <, 1.0);

    /* never stamped: dim, not hot */
    edge.last_us = 0;
    g_assert_cmpfloat(clawt_lrg_flow_edge_heat(&edge, now), ==,
                      CLAWT_LRG_FLOW_HEAT_FLOOR);

    out = clawt_lrg_flow_port_label(graph, &edge, TRUE);
    in = clawt_lrg_flow_port_label(graph, &edge, FALSE);
    g_assert_cmpstr(out, ==, "to Beta \xc2\xb7 7");
    g_assert_cmpstr(in, ==, "from Alpha");
}

/* ── Activity ─────────────────────────────────────────────────────── */

static const gchar *activity_json =
    "{\"now\":100000000,\"turns\":["
    " {\"agent\":\"alpha\",\"agent_name\":\"Alpha\",\"room\":\"dm:alpha:user\","
    "  \"room_name\":\"Alpha\",\"started\":40000000,\"origin\":\"user\","
    "  \"steerable\":true,\"running_turns\":2,\"step_count\":9,"
    "  \"steps\":["
    "   {\"step_kind\":\"tool\",\"room_id\":\"dm:alpha:user\",\"tool\":\"bash\","
    "    \"detail\":\"ls\",\"failed\":false,\"ts\":50000000,\"agent\":\"alpha\"},"
    "   {\"step_kind\":\"text\",\"room_id\":\"dm:alpha:user\",\"text\":\"hm\","
    "    \"failed\":false,\"ts\":50000000,\"agent\":\"alpha\"}"
    "  ]},"
    " {\"agent\":\"alpha\",\"room\":\"crew\",\"started\":90000000,"
    "  \"origin\":\"beta\",\"task\":\"t-1\",\"steerable\":false,"
    "  \"running_turns\":2,\"step_count\":0,\"steps\":[]}"
    "]}";

static void
test_activity_turns(void)
{
    g_autoptr(JsonNode) reply = parse(activity_json);
    gint64 now = 0;
    g_autoptr(GPtrArray) turns = clawt_lrg_activity_turns(reply, &now);
    ClawtLrgTurn *mine;
    ClawtLrgTurn *peer;
    g_autofree gchar *sub_mine = NULL;
    g_autofree gchar *sub_peer = NULL;
    g_autofree gchar *order = NULL;

    g_assert_cmpint(now, ==, 100000000);
    g_assert_cmpuint(turns->len, ==, 2);

    mine = g_ptr_array_index(turns, 0);
    peer = g_ptr_array_index(turns, 1);

    /* one agent, two rooms: two keys */
    g_assert_cmpstr(mine->key, !=, peer->key);
    g_assert_cmpstr(mine->title, ==, "Alpha in Alpha");
    g_assert_cmpstr(peer->title, ==, "alpha in crew");
    g_assert_true(mine->steerable);
    g_assert_false(peer->steerable);
    g_assert_cmpuint(mine->running, ==, 2);
    g_assert_cmpuint(mine->steps->len, ==, 2);

    /* "for you" is not news; a peer is */
    g_assert_null(mine->origin);
    g_assert_cmpstr(peer->origin, ==, "beta");

    sub_mine = clawt_lrg_activity_subtitle(mine, now);
    sub_peer = clawt_lrg_activity_subtitle(peer, now);
    g_assert_true(g_str_has_prefix(sub_mine, "running 1m 00s"));
    g_assert_nonnull(strstr(sub_mine, "9 step(s)"));
    g_assert_null(strstr(sub_mine, "for "));
    g_assert_nonnull(strstr(sub_peer, "for beta"));
    g_assert_nonnull(strstr(sub_peer, "task t-1"));

    order = clawt_lrg_activity_order(turns);
    g_assert_cmpstr(order, ==, "alpha\x1f" "dm:alpha:user\nalpha\x1f" "crew\n");
}

static void
test_activity_clips(void)
{
    g_autoptr(JsonNode) reply = parse(activity_json);
    gint64 now = 0;
    g_autoptr(GPtrArray) turns = clawt_lrg_activity_turns(reply, &now);
    g_autoptr(GPtrArray) clips =
        clawt_lrg_activity_clips(g_ptr_array_index(turns, 0), now);
    g_autoptr(GPtrArray) none =
        clawt_lrg_activity_clips(g_ptr_array_index(turns, 1), now);
    ClawtLrgStepClip *first;
    ClawtLrgStepClip *second;

    g_assert_cmpuint(clips->len, ==, 2);
    g_assert_cmpuint(none->len, ==, 0);

    first = g_ptr_array_index(clips, 0);
    second = g_ptr_array_index(clips, 1);

    /* 10s into the turn; two steps at one instant still both fit */
    g_assert_cmpfloat(first->start, ==, 10.0);
    g_assert_cmpfloat(first->duration, >=, 0.199);
    g_assert_cmpfloat(second->start, >=, first->start + first->duration - 1e-9);

    /* the last runs until now: 60s from start */
    g_assert_cmpfloat_with_epsilon(second->start + second->duration, 60.0, 1e-9);
    g_assert_nonnull(first->label);
    g_assert_nonnull(first->tone);
}

static void
test_activity_steer_and_stop(void)
{
    g_autoptr(JsonNode) steered = parse("{\"queued\":true,\"steered\":true}");
    g_autoptr(JsonNode) queued = parse("{\"queued\":true,\"steered\":false}");
    g_autoptr(JsonNode) killed = parse("{\"killed\":2}");
    g_autoptr(JsonNode) idle = parse("{\"killed\":0}");
    g_autofree gchar *body = clawt_lrg_activity_steer_body("  stop that \n");
    g_autofree gchar *blank = clawt_lrg_activity_steer_body("   ");
    g_autofree gchar *stopped = clawt_lrg_activity_stop_result("alpha",
                                                               killed);
    g_autofree gchar *nothing = clawt_lrg_activity_stop_result("alpha", idle);
    g_autofree gchar *failed = clawt_lrg_activity_stop_result("alpha", NULL);

    g_assert_cmpstr(body, ==, "stop that");
    g_assert_null(blank);
    g_assert_null(clawt_lrg_activity_steer_body(NULL));
    g_assert_true(g_str_has_prefix(clawt_lrg_activity_steer_result(steered),
                                   "Steered"));
    g_assert_true(g_str_has_prefix(clawt_lrg_activity_steer_result(queued),
                                   "It had finished"));
    g_assert_cmpstr(stopped, ==, "Stopped alpha.");
    g_assert_nonnull(strstr(nothing, "nothing running"));
    g_assert_nonnull(strstr(failed, "nothing running"));
}

/* ── Skills ───────────────────────────────────────────────────────── */

static void
test_skill_notes(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"enabled\":true,\"directory\":\"/s\",\"problems\":[],\"skills\":["
        " {\"name\":\"tidy\",\"description\":\"use this when\","
        "  \"directory\":\"/s/tidy\",\"origin\":\"https://x\","
        "  \"sha256\":\"abc\",\"source\":\"imported\",\"enabled\":false,"
        "  \"imported_at\":1,\"warnings\":[\"runs curl\"],"
        "  \"skipped\":[\"run.sh\",\"x.py\"]},"
        " {\"name\":\"plain\",\"directory\":\"/s/plain\",\"source\":\"user\","
        "  \"enabled\":true,\"warnings\":[],\"skipped\":[]}"
        "]}");
    JsonArray *skills = json_object_get_array_member(
        clawt_ipc_reply_object(reply), "skills");
    JsonObject *tidy = json_array_get_object_element(skills, 0);
    JsonObject *plain = json_array_get_object_element(skills, 1);
    g_autoptr(GPtrArray) tidy_notes = clawt_lrg_skill_notes(tidy);
    g_autoptr(GPtrArray) plain_notes = clawt_lrg_skill_notes(plain);
    g_autofree gchar *tidy_caption = clawt_lrg_skill_caption(tidy);
    g_autofree gchar *plain_caption = clawt_lrg_skill_caption(plain);
    ClawtLrgNote *note;

    /* Not enabled, Warning, Not copied, Provenance, sha256, Directory */
    g_assert_cmpuint(tidy_notes->len, ==, 6);
    note = g_ptr_array_index(tidy_notes, 0);
    g_assert_cmpstr(note->title, ==, "Not enabled");
    note = g_ptr_array_index(tidy_notes, 1);
    g_assert_cmpstr(note->title, ==, "Warning");
    g_assert_cmpstr(note->body, ==, "runs curl");
    g_assert_cmpstr(note->tone, ==, "warn");
    note = g_ptr_array_index(tidy_notes, 2);
    g_assert_cmpstr(note->title, ==, "Not copied");
    g_assert_cmpstr(note->body, ==, "run.sh, x.py");
    note = g_ptr_array_index(tidy_notes, 3);
    g_assert_cmpstr(note->title, ==, "Provenance");
    g_assert_cmpstr(note->body, ==, "imported, from https://x");
    note = g_ptr_array_index(tidy_notes, 4);
    g_assert_cmpstr(note->body, ==, "abc");
    note = g_ptr_array_index(tidy_notes, 5);
    g_assert_cmpstr(note->body, ==, "/s/tidy");

    /* enabled, clean: provenance and directory only */
    g_assert_cmpuint(plain_notes->len, ==, 2);
    note = g_ptr_array_index(plain_notes, 0);
    g_assert_cmpstr(note->body, ==, "user");

    g_assert_cmpstr(tidy_caption, ==, "Disabled \xc2\xb7 1 warning");
    g_assert_cmpstr(plain_caption, ==, "Enabled");
    g_assert_cmpstr(clawt_lrg_skill_tone(tidy), ==, "warn");
    g_assert_cmpstr(clawt_lrg_skill_tone(plain), ==, "good");
}

static void
test_skills_empty_and_payloads(void)
{
    g_autoptr(JsonNode) off = parse("{\"enabled\":false,\"skills\":[]}");
    g_autoptr(JsonNode) on = parse(
        "{\"enabled\":true,\"directory\":\"/d\",\"skills\":[]}");
    g_autoptr(JsonNode) enable = clawt_lrg_skill_enable_payload("tidy", TRUE);
    g_autoptr(JsonNode) import = NULL;
    g_autoptr(JsonNode) create = NULL;
    g_autoptr(JsonNode) refused = NULL;
    g_autofree gchar *refusal = NULL;
    g_autofree gchar *prompt = clawt_lrg_skill_remove_prompt("tidy");
    const gchar *title;
    const gchar *body;
    JsonObject *object;

    clawt_lrg_skills_empty(clawt_ipc_reply_object(off), &title, &body);
    g_assert_cmpstr(title, ==, "Skills are turned off");
    clawt_lrg_skills_empty(clawt_ipc_reply_object(on), &title, &body);
    g_assert_cmpstr(title, ==, "No skills yet");
    g_assert_cmpstr(body, ==, "/d");

    /* a JSON boolean, which the daemon reads with the boolean reader */
    object = json_node_get_object(enable);
    g_assert_cmpint(json_node_get_value_type(json_object_get_member(
                        object, "enabled")), ==, G_TYPE_BOOLEAN);
    g_assert_true(clawt_ipc_payload_boolean(object, "enabled", FALSE));

    refused = clawt_lrg_skill_import_payload("  ", "x", &refusal);
    g_assert_null(refused);
    g_assert_nonnull(refusal);

    import = clawt_lrg_skill_import_payload(" /tmp/s ", "", NULL);
    object = json_node_get_object(import);
    g_assert_cmpstr(clawt_ipc_payload_string(object, "source"), ==, "/tmp/s");

    /* an empty origin is absent, not "" */
    g_assert_false(json_object_has_member(object, "origin"));

    g_assert_null(clawt_lrg_skill_create_payload("", "d", NULL));
    create = clawt_lrg_skill_create_payload("tidy", NULL, NULL);
    object = json_node_get_object(create);
    g_assert_cmpstr(clawt_ipc_payload_string(object, "description"), ==, "");
    g_assert_nonnull(strstr(prompt, "\"tidy\""));
}

/* ── Teaching ─────────────────────────────────────────────────────── */

static void
test_teach(void)
{
    g_autoptr(JsonNode) list = parse(
        "{\"recordings\":["
        " {\"id\":\"r1\",\"source\":\"agent\",\"agent\":\"alpha\",\"goal\":\"file a bug\","
        "  \"step_count\":3,\"caveats\":[\"keys\"],\"active\":true},"
        " {\"id\":\"r2\",\"source\":\"host-demo\",\"step_count\":0,"
        "  \"caveats\":[],\"active\":false}"
        "]}");
    g_autoptr(JsonNode) show = parse(
        "{\"id\":\"r2\",\"steps\":["
        " {\"kind\":\"key\",\"label\":\"Ctrl+S\"},"
        " {\"kind\":\"click\",\"label\":\"Save\",\"detail\":\"at 10,20\"}"
        "]}");
    g_autoptr(JsonNode) draft = parse(
        "{\"id\":\"r2\",\"name\":\"save\",\"description\":\"use when\","
        "\"preview\":\"# Save\",\"note\":\"lands disabled\"}");
    g_autoptr(GPtrArray) rows = clawt_lrg_teach_recordings(list);
    g_autofree gchar *steps = clawt_lrg_teach_steps_text(show);
    g_autofree gchar *nothing = clawt_lrg_teach_steps_text(NULL);
    g_autofree gchar *draft_text = clawt_lrg_teach_draft_text(draft);
    g_autofree gchar *refusal = NULL;
    g_autoptr(JsonNode) start = NULL;
    ClawtLrgRecording *active;
    ClawtLrgRecording *done;
    const ClawtLrgTeachButton *buttons;
    guint n;

    g_assert_cmpuint(rows->len, ==, 2);
    active = g_ptr_array_index(rows, 0);
    done = g_ptr_array_index(rows, 1);

    g_assert_true(active->active);
    g_assert_cmpstr(active->title, ==, "file a bug");
    g_assert_cmpstr(active->subtitle, ==,
                    "agent, agent alpha, 3 step(s) -- recording now");
    g_assert_cmpstr(active->caveats[0], ==, "keys");

    /* no goal: titled by its id */
    g_assert_cmpstr(done->title, ==, "r2");
    g_assert_cmpuint(g_strv_length(done->caveats), ==, 0);

    buttons = clawt_lrg_teach_buttons(TRUE, &n);
    g_assert_cmpuint(n, ==, 1);
    g_assert_cmpint(buttons[0].action, ==, CLAWT_LRG_TEACH_STOP);

    buttons = clawt_lrg_teach_buttons(FALSE, &n);
    g_assert_cmpuint(n, ==, 4);
    g_assert_cmpint(buttons[1].action, ==, CLAWT_LRG_TEACH_SYNTHESIZE);

    g_assert_nonnull(clawt_lrg_teach_done(CLAWT_LRG_TEACH_COMMIT));
    g_assert_null(clawt_lrg_teach_done(CLAWT_LRG_TEACH_REMOVE));

    g_assert_cmpstr(steps, ==,
                    "1. [key] Ctrl+S\n2. [click] Save\n    at 10,20\n");
    g_assert_cmpstr(nothing, ==, "Nothing was captured.");
    g_assert_nonnull(strstr(draft_text, "# Save"));
    g_assert_null(clawt_lrg_teach_draft_text(NULL));

    g_assert_null(clawt_lrg_teach_start_payload(NULL, "agent", "x",
                                                &refusal));
    g_assert_nonnull(refusal);

    start = clawt_lrg_teach_start_payload("alpha", "host-demo", " ", NULL);
    g_assert_cmpstr(clawt_ipc_payload_string(json_node_get_object(start),
                                             "source"), ==, "host-demo");
    g_assert_false(json_object_has_member(json_node_get_object(start),
                                          "goal"));
}

static void
test_teach_sources_walk_the_library(void)
{
    g_auto(GStrv) labels = NULL;
    g_auto(GStrv) nicks = clawt_lrg_teach_sources(&labels);
    guint i;

    g_assert_cmpuint(g_strv_length(nicks), ==, clawt_teach_source_count());
    g_assert_cmpuint(g_strv_length(labels), ==, clawt_teach_source_count());

    for (i = 0; i < clawt_teach_source_count(); i++) {
        g_assert_cmpstr(nicks[i], ==, clawt_teach_source_nth_nick(i));
        g_assert_cmpstr(labels[i], ==, clawt_teach_source_nth_label(i));
    }
}

/* ── Memory ───────────────────────────────────────────────────────── */

static void
test_recall(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"hits\":["
        " {\"id\":\"1\",\"room\":\"crew\",\"from\":\"alpha\",\"from_name\":\"Alpha\","
        "  \"body\":\"the build is green\",\"at\":1700000000},"
        " {\"id\":\"2\",\"room\":\"crew\",\"from\":\"beta\",\"body\":\"x\",\"at\":0}"
        "],\"full_text\":false,\"indexed\":40}");
    g_autoptr(JsonNode) empty = parse("{\"hits\":[],\"indexed\":0}");
    g_autoptr(JsonNode) none = parse("{\"hits\":[],\"indexed\":5}");
    g_autoptr(GPtrArray) hits = clawt_lrg_recall_hits(reply);
    g_autoptr(JsonNode) payload = clawt_lrg_recall_payload(" green ");
    ClawtLrgRecallHit *hit;
    const gchar *title;
    const gchar *body;

    g_assert_cmpuint(hits->len, ==, 2);
    hit = g_ptr_array_index(hits, 0);
    g_assert_cmpstr(hit->body, ==, "the build is green");
    g_assert_cmpstr(hit->subtitle, ==, "Alpha in crew");
    g_assert_nonnull(hit->stamp);
    g_assert_cmpuint(strlen(hit->stamp), >, 10);

    /* falls back to the id, and no time is no stamp */
    hit = g_ptr_array_index(hits, 1);
    g_assert_cmpstr(hit->subtitle, ==, "beta in crew");
    g_assert_null(hit->stamp);

    g_assert_true(clawt_lrg_recall_is_substring(reply));
    g_assert_false(clawt_lrg_recall_is_substring(empty));

    clawt_lrg_recall_empty(empty, &title, &body);
    g_assert_cmpstr(title, ==, "Nothing has been said yet");
    clawt_lrg_recall_empty(none, &title, &body);
    g_assert_cmpstr(title, ==, "Nothing matched");

    g_assert_null(clawt_lrg_recall_payload("   "));
    g_assert_cmpstr(clawt_ipc_payload_string(json_node_get_object(payload),
                                             "query"), ==, "green");
}

static void
test_operator(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"text\":\"I like tea\",\"path\":\"/p\",\"enabled\":false,"
        "\"learned\":["
        " {\"id\":\"1\",\"content\":\"prefers brevity\",\"source\":\"alpha\","
        "  \"created_at\":1700000000},"
        " {\"id\":\"2\",\"content\":\"is on UTC\",\"created_at\":0}"
        "]}");
    g_autoptr(GPtrArray) learned = clawt_lrg_operator_learned(reply);
    g_autoptr(GPtrArray) nothing = clawt_lrg_operator_learned(NULL);
    ClawtLrgNote *note;

    g_assert_cmpuint(learned->len, ==, 2);
    g_assert_cmpuint(nothing->len, ==, 0);

    note = g_ptr_array_index(learned, 0);
    g_assert_cmpstr(note->title, ==, "prefers brevity");
    g_assert_true(g_str_has_prefix(note->body, "alpha, 20"));

    note = g_ptr_array_index(learned, 1);
    g_assert_cmpstr(note->body, ==, "unattributed");
}

static void
test_agent_memories(void)
{
    g_autoptr(JsonNode) reply = parse(
        "{\"total\":3,\"memories\":["
        " {\"id\":\"a\",\"content\":\"line one\\nline two\",\"category\":\"fact\","
        "  \"importance\":\"normal\",\"scope\":\"agent\",\"pinned\":false,"
        "  \"created_at\":100,\"access_count\":0},"
        " {\"id\":\"b\",\"content\":\"shared\",\"summary\":\"team note\","
        "  \"category\":\"fact\",\"importance\":\"high\",\"scope\":\"team\","
        "  \"pinned\":true,\"created_at\":0,\"access_count\":2},"
        " {\"id\":\"c\",\"content\":\"old\",\"category\":\"fact\","
        "  \"importance\":\"normal\",\"pinned\":false,\"created_at\":0,"
        "  \"access_count\":0}"
        "]}");
    g_autoptr(GPtrArray) all = clawt_lrg_agent_memories(reply, NULL,
                                                        (gint64)200 * G_USEC_PER_SEC);
    g_autoptr(GPtrArray) agent = clawt_lrg_agent_memories(reply, "agent", 0);
    g_autoptr(GPtrArray) team = clawt_lrg_agent_memories(reply, "team", 0);
    g_autoptr(GPtrArray) fleet = clawt_lrg_agent_memories(reply, "fleet", 0);
    g_autoptr(JsonNode) list = NULL;
    g_autoptr(JsonNode) search = NULL;
    gboolean is_search = TRUE;
    ClawtLrgMemoryRow *row;

    g_assert_cmpuint(all->len, ==, 3);

    /* a row with no scope is the agent's own */
    g_assert_cmpuint(agent->len, ==, 2);
    g_assert_cmpuint(team->len, ==, 1);
    g_assert_cmpuint(fleet->len, ==, 0);

    row = g_ptr_array_index(all, 0);
    g_assert_cmpstr(row->title, ==, "line one");
    g_assert_cmpstr(row->body, ==, "line one\nline two");
    /* the daemon's own spelling: no client offers a scope as a choice */
    g_assert_cmpstr(row->scope_label, ==, "agent");
    g_assert_cmpstr(row->when, !=, "");

    row = g_ptr_array_index(all, 1);
    g_assert_cmpstr(row->title, ==, "team note");
    g_assert_true(row->pinned);
    g_assert_cmpstr(row->when, ==, "");

    list = clawt_lrg_agent_memory_payload("alpha", "  ", &is_search);
    g_assert_false(is_search);
    g_assert_false(json_object_has_member(json_node_get_object(list),
                                          "query"));

    search = clawt_lrg_agent_memory_payload("alpha", "tea", &is_search);
    g_assert_true(is_search);
    g_assert_cmpstr(clawt_ipc_payload_string(json_node_get_object(search),
                                             "query"), ==, "tea");
}

gint
main(gint argc, gchar **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/lrg/fleet/flow/rooms", test_flow_rooms);
    g_test_add_func("/lrg/fleet/flow/rooms-failure", test_flow_rooms_failure);
    g_test_add_func("/lrg/fleet/flow/one-line", test_flow_one_line);
    g_test_add_func("/lrg/fleet/flow/chips", test_flow_chips);
    g_test_add_func("/lrg/fleet/flow/members-between",
                    test_flow_room_members_and_between);
    g_test_add_func("/lrg/fleet/flow/graph-three-agents",
                    test_flow_graph_three_agents);
    g_test_add_func("/lrg/fleet/flow/graph-user-history",
                    test_flow_graph_user_and_history);
    g_test_add_func("/lrg/fleet/flow/graph-signature",
                    test_flow_graph_signature_moves);
    g_test_add_func("/lrg/fleet/flow/heat-ports", test_flow_heat_and_ports);
    g_test_add_func("/lrg/fleet/activity/turns", test_activity_turns);
    g_test_add_func("/lrg/fleet/activity/clips", test_activity_clips);
    g_test_add_func("/lrg/fleet/activity/steer-stop",
                    test_activity_steer_and_stop);
    g_test_add_func("/lrg/fleet/skills/notes", test_skill_notes);
    g_test_add_func("/lrg/fleet/skills/empty-payloads",
                    test_skills_empty_and_payloads);
    g_test_add_func("/lrg/fleet/teach/recordings", test_teach);
    g_test_add_func("/lrg/fleet/teach/sources",
                    test_teach_sources_walk_the_library);
    g_test_add_func("/lrg/fleet/memory/recall", test_recall);
    g_test_add_func("/lrg/fleet/memory/operator", test_operator);
    g_test_add_func("/lrg/fleet/memory/agent", test_agent_memories);

    return g_test_run();
}
