/*
 * test-fed-link.c - Two fleets, linked, in one process
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Two daemons share a main context, each with its own state, secrets,
 * socket and workspace root.  Every scenario runs over both transports
 * from one table: the HTTP listener and the simple daemon connection
 * (the other daemon's own IPC socket).  Both are unix sockets here --
 * `make test` opens no network socket -- and the link code dials them
 * through the same paths it uses for a tunnel or a tailnet address.
 *
 * Checked: an invite made on B and accepted on A links both, the
 * fingerprint check holds, each side records the tier it chose; pings
 * make the link live; an unrelated timer keeps running while the accept
 * is deferred (an IPC handler must not wait on the network); a fleet
 * connection is refused everything but fed.* frames and gets no events;
 * a replayed frame is refused; and a revoke reaches the other side.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"
#include <string.h>

typedef struct {
    const gchar *name;
    gboolean     ipc;
} Transport;

static const Transport transports[] = {
    { "http", FALSE },
    { "ipc", TRUE },
};

typedef struct {
    gchar        *dir;
    GMainContext *context;
    ClawtDaemon  *a;
    ClawtDaemon  *b;
    ClawtClient  *ca;        /* the operator's client on A */
    ClawtClient  *cb;        /* ... on B */
} Pair;

static gchar *
daemon_yaml(
    const gchar *dir,
    const gchar *who,
    const gchar *name
){
    return g_strdup_printf(
        "daemon:\n  tailscale: false\n  state_dir: \"%s/%s\"\n"
        "  socket: \"%s/%s.sock\"\n  automation_dir: \"%s/%s-pods\"\n"
        "secrets:\n  dir: \"%s/%s-secrets\"\n"
        "defaults:\n  workspace_root: \"%s/%s-agents\"\n"
        "federation:\n  enabled: true\n  name: \"%s\"\n  owner: \"Owner of %s\"\n"
        "  ping_seconds: 1\n  listen: [\"unix:%s/%s-fed.sock\"]\n"
        "agents:\n  - id: %s-chief\n    chief_of_staff: true\n"
        "    tools:\n      federation: true\n"
        "    runtime:\n      autostart: false\n"
        "  - id: %s-worker\n    runtime:\n      autostart: false\n",
        dir, who, dir, who, dir, who, dir, who, dir, who, name, name, dir, who, who, who);
}

static void
pair_up(
    Pair *p
){
    g_autoptr(GError) error = NULL;
    g_autofree gchar *a_path = NULL;
    g_autofree gchar *b_path = NULL;
    g_autofree gchar *a_yaml = NULL;
    g_autofree gchar *b_yaml = NULL;
    g_autofree gchar *a_sock = NULL;
    g_autofree gchar *b_sock = NULL;

    p->dir = g_dir_make_tmp("clawt-fed-XXXXXX", NULL);
    p->context = g_main_context_new();
    g_main_context_push_thread_default(p->context);
    a_path = g_build_filename(p->dir, "a.yaml", NULL);
    b_path = g_build_filename(p->dir, "b.yaml", NULL);
    a_yaml = daemon_yaml(p->dir, "a", "Alpha");
    b_yaml = daemon_yaml(p->dir, "b", "Bravo Lab");
    g_assert_true(g_file_set_contents(a_path, a_yaml, -1, NULL));
    g_assert_true(g_file_set_contents(b_path, b_yaml, -1, NULL));

    p->a = clawt_daemon_new(a_path, p->context);
    clawt_daemon_start(p->a, &error);
    g_assert_no_error(error);
    p->b = clawt_daemon_new(b_path, p->context);
    clawt_daemon_start(p->b, &error);
    g_assert_no_error(error);
    g_assert_nonnull(p->a->fed_key);
    g_assert_nonnull(p->b->fed_key);
    /* The fleet id is the key's fingerprint. */
    g_assert_cmpstr(clawt_daemon_fleet_id(p->a), ==, p->a->fed_fleet_id);
    g_assert_cmpuint(strlen(p->a->fed_fleet_id), ==, CLAWT_FLEET_FINGERPRINT_LENGTH);

    a_sock = g_build_filename(p->dir, "a.sock", NULL);
    b_sock = g_build_filename(p->dir, "b.sock", NULL);
    p->ca = clawt_client_new(a_sock);
    p->cb = clawt_client_new(b_sock);
    g_assert_true(clawt_client_connect(p->ca, &error));
    g_assert_true(clawt_client_connect(p->cb, &error));
}

static void
pair_down(
    Pair *p
){
    clawt_client_disconnect(p->ca);
    clawt_client_disconnect(p->cb);
    g_object_unref(p->ca);
    g_object_unref(p->cb);
    clawt_daemon_stop(p->a);
    clawt_daemon_stop(p->b);
    g_object_unref(p->a);
    g_object_unref(p->b);
    while (g_main_context_iteration(p->context, FALSE))
        ;
    g_main_context_pop_thread_default(p->context);
    g_main_context_unref(p->context);
    clawt_test_remove_tree(p->dir);
    g_free(p->dir);
}

static JsonObject *
ask(
    ClawtClient *client,
    const gchar *kind,
    JsonNode    *payload,
    GError     **error
){
    g_autoptr(JsonNode) reply = clawt_client_request_full(client, kind, payload, 20, error);

    if (reply == NULL || !JSON_NODE_HOLDS_OBJECT(reply))
        return NULL;
    return json_object_ref(json_node_get_object(reply));
}

static JsonNode *
payload_of(
    const gchar *json
){
    return json_from_string(json, NULL);
}

static const gchar *
peer_field(
    ClawtClient *client,
    const gchar *field
){
    static gchar value[256];
    g_autoptr(JsonObject) list = ask(client, "federation.peer.list", NULL, NULL);
    JsonArray *peers = list != NULL ? json_object_get_array_member(list, "peers") : NULL;

    value[0] = '\0';
    if (peers != NULL && json_array_get_length(peers) > 0) {
        JsonObject *p = json_array_get_object_element(peers, 0);
        if (json_object_has_member(p, field))
            g_strlcpy(value, json_object_get_string_member(p, field), sizeof(value));
    }
    return value;
}

static gboolean
count_tick(
    gpointer user_data
){
    (*(guint *)user_data)++;
    return G_SOURCE_CONTINUE;
}

/* B invites, A accepts; returns A's alias for B. */
static gchar *
link_them(
    Pair            *p,
    const Transport *t
){
    g_autoptr(GError) error = NULL;
    g_autofree gchar *endpoint = NULL;
    g_autofree gchar *req = NULL;
    g_autoptr(JsonObject) invite = NULL;
    g_autoptr(JsonObject) accepted = NULL;
    g_autofree gchar *accept_req = NULL;
    GSource *ticker;
    guint ticks = 0;

    endpoint = t->ipc ? g_strdup_printf("unix:%s/b.sock#ipc", p->dir)
                      : g_strdup_printf("unix:%s/b-fed.sock", p->dir);
    req = g_strdup_printf("{\"tier\":\"limited\",\"endpoint\":\"%s\"}", endpoint);
    invite = ask(p->cb, "federation.invite.create", payload_of(req), &error);
    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(json_object_get_string_member(invite, "paste"),
                                   CLAWT_FED_INVITE_PREFIX));

    /* The preview reads the invite offline, and names the fleet. */
    {
        g_autofree gchar *inspect = g_strdup_printf("{\"invite\":\"%s\"}",
            json_object_get_string_member(invite, "paste"));
        g_autoptr(JsonObject) preview = ask(p->ca, "federation.invite.inspect",
                                            payload_of(inspect), &error);
        g_assert_no_error(error);
        g_assert_cmpstr(json_object_get_string_member(preview, "name"), ==, "Bravo Lab");
        g_assert_cmpstr(json_object_get_string_member(preview, "fleet_id"), ==,
                        p->b->fed_fleet_id);
        g_assert_true(json_object_get_boolean_member(preview, "reachable"));
    }

    /* A timer keeps running while the accept waits on the other fleet. */
    ticker = g_timeout_source_new(1);
    g_source_set_callback(ticker, count_tick, &ticks, NULL);
    g_source_attach(ticker, p->context);
    accept_req = g_strdup_printf("{\"invite\":\"%s\",\"tier\":\"trusted\",\"tags\":[\"lab\",\"gpu\"]}",
                                 json_object_get_string_member(invite, "uri"));
    accepted = ask(p->ca, "federation.invite.accept", payload_of(accept_req), &error);
    g_source_destroy(ticker);
    g_source_unref(ticker);
    g_assert_no_error(error);
    g_assert_nonnull(accepted);
    g_assert_cmpuint(ticks, >, 0);

    g_assert_cmpstr(json_object_get_string_member(accepted, "fleet_id"), ==, p->b->fed_fleet_id);
    g_assert_cmpstr(json_object_get_string_member(accepted, "tier"), ==, "trusted");

    /* Each side holds the tier it chose for the other. */
    g_assert_cmpstr(peer_field(p->cb, "tier"), ==, "limited");
    g_assert_cmpstr(peer_field(p->cb, "fleet_id"), ==, p->a->fed_fleet_id);
    g_assert_cmpstr(peer_field(p->cb, "label"), ==, "Alpha");
    /* The invite is spent. */
    {
        g_autofree gchar *again = g_strdup_printf("{\"invite\":\"%s\",\"tier\":\"trusted\","
                                                  "\"alias\":\"again\"}",
                                                  json_object_get_string_member(invite, "uri"));
        g_autoptr(JsonObject) no = NULL;
        clawt_fed_registry_remove(p->a->fed_registry,
                                  json_object_get_string_member(accepted, "alias"));
        no = ask(p->ca, "federation.invite.accept", payload_of(again), &error);
        g_assert_null(no);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
        g_clear_error(&error);
        /* Put A's record back for the rest of the test. */
        clawt_fed_registry_load(p->a->fed_registry, NULL);
        clawt_daemon_federation_refresh_links(p->a);
    }
    return g_strdup(json_object_get_string_member(accepted, "alias"));
}

static void
wait_for(
    Pair        *p,
    ClawtClient *client,
    const gchar *field,
    const gchar *want
){
    gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

    while (g_strcmp0(peer_field(client, field), want) != 0) {
        g_assert_cmpint(g_get_monotonic_time(), <, deadline);
        g_main_context_iteration(p->context, FALSE);
        g_usleep(20000);
    }
}

static void
test_link_ping_revoke(
    gconstpointer data
){
    const Transport *t = data;
    Pair p;
    g_autofree gchar *alias = NULL;
    g_autoptr(GError) error = NULL;

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    alias = link_them(&p, t);
    g_assert_cmpstr(alias, ==, "bravo-lab");

    /* A dials B on its timer; both come to see the link live. */
    wait_for(&p, p.ca, "health", "live");
    wait_for(&p, p.cb, "health", "live");
    g_assert_cmpstr(peer_field(p.ca, "transport"), ==, t->ipc ? "ipc" : "http");
    /* B's grant reached A through the hello: what B allows A. */
    g_assert_nonnull(clawt_fed_registry_get(p.a->fed_registry, alias)->their_grant);

    /* An explicit ping is a deferred request with an answer. */
    {
        g_autofree gchar *req = g_strdup_printf("{\"alias\":\"%s\"}", alias);
        g_autoptr(JsonObject) pong = ask(p.ca, "federation.peer.ping", payload_of(req), &error);
        g_assert_no_error(error);
        g_assert_true(json_object_has_member(pong, "ms"));
    }

    /* Revoked here, told there. */
    {
        g_autofree gchar *req = g_strdup_printf("{\"alias\":\"%s\"}", alias);
        g_autoptr(JsonObject) gone = ask(p.ca, "federation.peer.revoke", payload_of(req), &error);
        g_assert_no_error(error);
        g_assert_cmpstr(json_object_get_string_member(gone, "status"), ==, "revoked");
    }
    wait_for(&p, p.cb, "status", "revoked");
    pair_down(&p);
}

/* The refs another fleet's agents have in @client's agent.list. */
static gchar *
mirrored_refs(
    ClawtClient *client
){
    g_autoptr(JsonObject) list = ask(client, "agent.list", NULL, NULL);
    JsonArray *fleets = list != NULL && json_object_has_member(list, "fleets")
        ? json_object_get_array_member(list, "fleets") : NULL;
    GString *out = g_string_new(NULL);
    guint i;
    guint j;

    for (i = 0; fleets != NULL && i < json_array_get_length(fleets); i++) {
        JsonArray *agents = json_object_get_array_member(json_array_get_object_element(fleets, i),
                                                         "agents");
        for (j = 0; agents != NULL && j < json_array_get_length(agents); j++)
            g_string_append_printf(out, "%s%s", out->len > 0 ? " " : "",
                                   json_object_get_string_member(
                                       json_array_get_object_element(agents, j), "ref"));
    }
    return g_string_free(out, FALSE);
}

static void
wait_for_refs(
    Pair        *p,
    ClawtClient *client,
    const gchar *want
){
    gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

    for (;;) {
        g_autofree gchar *got = mirrored_refs(client);
        if (g_strcmp0(got, want) == 0)
            return;
        if (g_get_monotonic_time() > deadline)
            g_error("expected refs \"%s\", have \"%s\"", want, got);
        g_main_context_iteration(p->context, FALSE);
        g_usleep(20000);
    }
}

/*
 * The office visitors A draws from its links: their ids, space-joined,
 * once the floor has @want of them.  Stopped agents stay off the floor,
 * and so does everybody whose fleet withholds presence -- except that
 * withholding presence draws them idle rather than not at all.
 */
static void
wait_for_visitors(
    Pair        *p,
    const gchar *want
){
    gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

    for (;;) {
        g_autoptr(JsonArray) floor = json_array_new();
        g_autoptr(GHashTable) drawn = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        g_autoptr(GString) got = g_string_new(NULL);
        guint i;

        clawt_daemon_federation_office_visitors(p->a, floor, drawn);
        for (i = 0; i < json_array_get_length(floor); i++) {
            JsonObject *v = json_array_get_object_element(floor, i);

            g_assert_cmpstr(json_object_get_string_member(v, "kind"), ==, "external");
            g_assert_cmpstr(json_object_get_string_member(v, "fleet"), ==, p->b->fed_fleet_id);
            g_assert_cmpstr(json_object_get_string_member(v, "activity"), ==, "idle");
            g_string_append_printf(got, "%s%s", got->len > 0 ? " " : "",
                                   json_object_get_string_member(v, "id"));
        }
        if (g_strcmp0(got->str, want) == 0) {
            g_assert_true(g_hash_table_contains(drawn, p->b->fed_fleet_id));
            return;
        }
        if (g_get_monotonic_time() > deadline)
            g_error("expected visitors \"%s\", have \"%s\"", want, got->str);
        g_main_context_iteration(p->context, FALSE);
        g_usleep(20000);
    }
}

/*
 * Each fleet sees exactly what the other's grant lets it: B gave A the
 * limited tier (its inbox, the chief, only), A gave B trusted (all).  A
 * grant changed on one side reaches the other's mirror without a
 * restart, and a revoked fleet leaves the list.
 */
static void
test_rosters_follow_grants(
    gconstpointer data
){
    const Transport *t = data;
    Pair p;
    g_autofree gchar *alias = NULL;
    g_autofree gchar *b_alias_on_a = NULL;
    g_autoptr(GError) error = NULL;

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    alias = link_them(&p, t);
    wait_for_refs(&p, p.ca, "b-chief@bravo-lab");
    wait_for_refs(&p, p.cb, "a-chief@alpha a-worker@alpha");

    /* B widens what A may see; A's mirror follows. */
    {
        g_autofree gchar *req = g_strdup_printf("{\"alias\":\"alpha\",\"grant\":{\"reach\":\"all\"}}");
        g_autoptr(JsonObject) shown = ask(p.cb, "federation.peer.set", payload_of(req), &error);
        g_assert_no_error(error);
    }
    wait_for_refs(&p, p.ca, "b-chief@bravo-lab b-worker@bravo-lab");

    /* Nobody is running, so nobody visits A's office... */
    wait_for_visitors(&p, "");
    /* ...until B stops telling A who is running: then they are drawn idle. */
    {
        g_autoptr(JsonObject) shown = ask(p.cb, "federation.peer.set",
            payload_of("{\"alias\":\"alpha\",\"grant\":{\"see_presence\":\"false\"}}"),
            &error);
        g_assert_no_error(error);
    }
    wait_for_visitors(&p, "bravo-lab:b-chief bravo-lab:b-worker");

    /* An agent's own reach only narrows: B's worker takes owned fleets only. */
    {
        g_autoptr(JsonObject) set = ask(p.cb, "agent.set",
            payload_of("{\"agent\":\"b-worker\",\"key\":\"federation_reach\","
                       "\"value\":\"tier:owned\"}"), &error);
        g_assert_no_error(error);
    }
    wait_for_refs(&p, p.ca, "b-chief@bravo-lab");

    /* A takes the roster away from B entirely. */
    {
        g_autofree gchar *req = g_strdup_printf("{\"alias\":\"%s\",\"grant\":{\"see_roster\":\"false\"}}",
                                                alias);
        g_autoptr(JsonObject) shown = ask(p.ca, "federation.peer.set", payload_of(req), &error);
        g_assert_no_error(error);
    }
    wait_for_refs(&p, p.cb, "");
    (void)b_alias_on_a;
    pair_down(&p);
}

/* "sender: body" for each message in @room, newest last, one per line. */
static gchar *
room_lines(
    ClawtClient *client,
    const gchar *room
){
    g_autofree gchar *req = g_strdup_printf("{\"room\":\"%s\"}", room);
    g_autoptr(JsonObject) history = ask(client, "room.history", payload_of(req), NULL);
    JsonArray *messages = history != NULL && json_object_has_member(history, "messages")
        ? json_object_get_array_member(history, "messages") : NULL;
    GString *out = g_string_new(NULL);
    guint i;

    for (i = 0; messages != NULL && i < json_array_get_length(messages); i++) {
        JsonObject *m = json_array_get_object_element(messages, i);
        g_string_append_printf(out, "%s: %s\n", json_object_get_string_member(m, "sender"),
                               json_object_get_string_member(m, "body"));
    }
    return g_string_free(out, FALSE);
}

static void
wait_for_line(
    Pair        *p,
    ClawtClient *client,
    const gchar *room,
    const gchar *want
){
    gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

    for (;;) {
        g_autofree gchar *got = room_lines(client, room);
        if (strstr(got, want) != NULL)
            return;
        if (g_get_monotonic_time() > deadline)
            g_error("expected \"%s\" in %s, have:\n%s", want, room, got);
        g_main_context_iteration(p->context, FALSE);
        g_usleep(20000);
    }
}

static void
send_as_operator(
    ClawtClient *client,
    const gchar *target,
    const gchar *body,
    const gchar *want_room
){
    g_autoptr(GError) error = NULL;
    g_autofree gchar *req = g_strdup_printf("{\"target\":\"%s\",\"body\":\"%s\"}", target, body);
    g_autoptr(JsonObject) sent = ask(client, "msg.send", payload_of(req), &error);

    g_assert_no_error(error);
    g_assert_cmpstr(json_object_get_string_member(sent, "room"), ==, want_room);
}

static void
send_as_operator_plain(
    ClawtClient *client,
    const gchar *target,
    const gchar *body
){
    g_autoptr(GError) error = NULL;
    g_autofree gchar *req = g_strdup_printf("{\"target\":\"%s\",\"body\":\"%s\"}", target, body);
    g_autoptr(JsonObject) sent = ask(client, "msg.send", payload_of(req), &error);

    g_assert_no_error(error);
}

/*
 * Messages between fleets (0.68.0), both ways, through both fleets'
 * grants.  A's operator writes to B's chief, which B's grant to A
 * reaches; the chief's answer comes back into A's conversation (B dials
 * nothing, so it is pushed).  B's worker is beyond A's reach and A is
 * told so in the conversation, not left waiting.  Then B makes A ask
 * first: the message is held as a decision, A is told, and B's operator
 * delivering it lands it.  A fleet's words reach the agent fenced.
 */
static void
test_messages_cross(
    gconstpointer data
){
    const Transport *t = data;
    Pair p;
    g_autofree gchar *alias = NULL;
    g_autoptr(GError) error = NULL;
    const gchar *a_room = "fed:bravo-lab:user:b-chief";
    const gchar *b_room = "fed:alpha:b-chief:user";

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    alias = link_them(&p, t);
    g_assert_cmpstr(alias, ==, "bravo-lab");
    wait_for_refs(&p, p.ca, "b-chief@bravo-lab");

    /* Out, in, and the answer back. */
    send_as_operator(p.ca, "b-chief@bravo-lab", "hello from alpha", a_room);
    wait_for_line(&p, p.ca, a_room, "user: hello from alpha");
    wait_for_line(&p, p.cb, b_room, "fleet:alpha:user: hello from alpha");
    g_assert_cmpstr(clawt_room_get_name(clawt_room_manager_get(p.b->rooms, b_room)), ==,
                    "b-chief \342\206\224 user@alpha");
    /* B's chief (stopped, so it is in the mailbox) gets it fenced, under a preamble. */
    {
        ClawtAgent *chief = clawt_agent_manager_get(p.b->agents, "b-chief");
        g_autoptr(GPtrArray) items = clawt_mailbox_list(clawt_agent_get_mailbox(chief), NULL);
        g_autofree gchar *delivered = NULL;

        g_assert_cmpuint(items->len, >, 0);
        delivered = clawt_mailbox_item_dup_delivered_body(g_ptr_array_index(items, items->len - 1));
        g_assert_true(g_str_has_prefix(delivered, CLAWT_PREAMBLE_MARKER));
        g_assert_nonnull(strstr(delivered, "not your operator"));
        g_assert_nonnull(strstr(delivered, "```untrusted-message-from-user@alpha"));
        g_assert_nonnull(strstr(delivered, "hello from alpha"));
    }
    {
        g_autoptr(ClawtMessage) reply = clawt_message_new(b_room, "b-chief", "hello back");
        g_assert_cmpint(clawt_mailbox_router_send(p.b->router, reply, &error), >=, 0);
        g_assert_no_error(error);
    }
    wait_for_line(&p, p.ca, a_room, "fleet:bravo-lab:b-chief: hello back");

    /* Beyond reach: said in the conversation. */
    send_as_operator(p.ca, "b-worker@bravo-lab", "psst", "fed:bravo-lab:user:b-worker");
    wait_for_line(&p, p.ca, "fed:bravo-lab:user:b-worker", "Not delivered to Bravo Lab");
    {
        g_autofree gchar *b_side = room_lines(p.cb, "fed:alpha:b-worker:user");
        g_assert_null(strstr(b_side, "psst"));
    }

    /* Held for B's operator, then delivered. */
    {
        g_autoptr(JsonObject) set = ask(p.cb, "federation.peer.set",
            payload_of("{\"alias\":\"alpha\",\"grant\":{\"message\":\"ask\"}}"), &error);
        g_assert_no_error(error);
    }
    send_as_operator(p.ca, "b-chief@bravo-lab", "needs approval", a_room);
    wait_for_line(&p, p.ca, a_room, "holding this message for its operator");
    {
        g_autofree gchar *b_side = room_lines(p.cb, b_room);
        g_autoptr(GPtrArray) open = clawt_decision_store_list(p.b->decisions, TRUE);
        const gchar *held = NULL;
        guint i;

        g_assert_null(strstr(b_side, "needs approval"));
        for (i = 0; open != NULL && i < open->len; i++)
            if (g_str_has_prefix(clawt_decision_get_id(g_ptr_array_index(open, i)), "fed-hold-"))
                held = clawt_decision_get_id(g_ptr_array_index(open, i));
        g_assert_nonnull(held);
        {
            g_autofree gchar *req = g_strdup_printf("{\"decision\":\"%s\",\"answer\":\"deliver\"}",
                                                    held);
            g_autoptr(JsonObject) answered = ask(p.cb, "decision.answer", payload_of(req), &error);
            g_assert_no_error(error);
        }
    }
    wait_for_line(&p, p.cb, b_room, "fleet:alpha:user: needs approval");
    g_assert_cmpuint(g_hash_table_size(p.b->fed_holds), ==, 0);
    pair_down(&p);
}

/* One agent tool call on @daemon, as @agent_id; what it said. */
static gchar *
call_tool(
    ClawtDaemon *daemon,
    const gchar *agent_id,
    const gchar *tool,
    const gchar *arguments,
    gboolean    *is_error
){
    g_autofree gchar *request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, arguments);
    g_autoptr(JsonNode) req = json_from_string(request, NULL);
    g_autoptr(JsonNode) response = clawt_mcp_tools_call(clawt_daemon_get_mcp_tools(daemon),
                                                        agent_id, NULL, req);
    JsonObject *result = json_object_get_object_member(json_node_get_object(response), "result");
    JsonArray *content = json_object_get_array_member(result, "content");

    *is_error = json_object_get_boolean_member_with_default(result, "isError", FALSE);
    return g_strdup(json_object_get_string_member(json_array_get_object_element(content, 0),
                                                  "text"));
}

/*
 * An agent writes to another fleet only with tools.federation; it sees
 * the agents it may write to, and the answer reaches it as an answer --
 * one that asks for nothing back, so two fleets' agents cannot sign off
 * at each other for ever.
 */
static void
test_agent_tools(void)
{
    Pair p;
    g_autofree gchar *alias = NULL;
    g_autoptr(GError) error = NULL;
    gboolean is_error = FALSE;
    const gchar *a_room = "fed:bravo-lab:a-chief:b-chief";
    const gchar *b_room = "fed:alpha:b-chief:a-chief";

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    alias = link_them(&p, &transports[1]);
    wait_for_refs(&p, p.ca, "b-chief@bravo-lab");

    {
        g_autofree gchar *said = call_tool(p.a, "a-worker", "clawtilla_message_agent",
            "{\"agent_id\":\"b-chief@bravo-lab\",\"body\":\"hi\"}", &is_error);
        g_assert_true(is_error);
        g_assert_nonnull(strstr(said, "tools.federation"));
    }
    {
        g_autofree gchar *listing = call_tool(p.a, "a-chief", "clawtilla_list_agents", "{}",
                                              &is_error);
        g_autofree gchar *peers = call_tool(p.a, "a-chief", "clawtilla_federation_peers", "{}",
                                            &is_error);
        g_assert_false(is_error);
        g_assert_nonnull(strstr(listing, "b-chief@bravo-lab"));
        g_assert_nonnull(strstr(peers, "bravo-lab"));
    }
    {
        g_autofree gchar *said = call_tool(p.a, "a-chief", "clawtilla_message_agent",
            "{\"agent_id\":\"b-chief@bravo-lab\",\"body\":\"from the chief\"}", &is_error);
        g_assert_false(is_error);
    }
    wait_for_line(&p, p.cb, b_room, "fleet:alpha:a-chief: from the chief");
    {
        g_autoptr(ClawtMessage) reply = clawt_message_new(b_room, "b-chief", "the answer");
        g_assert_cmpint(clawt_mailbox_router_send(p.b->router, reply, &error), >=, 0);
        g_assert_no_error(error);
    }
    wait_for_line(&p, p.ca, a_room, "fleet:bravo-lab:b-chief: the answer");
    {
        ClawtAgent *chief = clawt_agent_manager_get(p.a->agents, "a-chief");
        g_autoptr(GPtrArray) items = clawt_mailbox_list(clawt_agent_get_mailbox(chief), NULL);
        g_autofree gchar *delivered = NULL;

        g_assert_cmpuint(items->len, >, 0);
        delivered = clawt_mailbox_item_dup_delivered_body(g_ptr_array_index(items, items->len - 1));
        g_assert_nonnull(strstr(delivered, "This is their answer"));
        g_assert_nonnull(strstr(delivered, "the answer"));
    }
    pair_down(&p);
}

/* The task B runs for A's proxy @proxy_id, waiting for it to exist. */
static ClawtTask *
wait_for_doer_task(
    Pair        *p,
    const gchar *proxy_id
){
    gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

    for (;;) {
        g_autoptr(GPtrArray) all = clawt_task_manager_list(clawt_daemon_get_tasks(p->b), NULL, TRUE);
        guint i;

        for (i = 0; all != NULL && i < all->len; i++) {
            ClawtTask *task = g_ptr_array_index(all, i);
            if (g_strcmp0(clawt_task_get_remote_task(task), proxy_id) == 0
                && g_strcmp0(clawt_task_get_origin(task), "fleet:alpha:a-chief") == 0)
                return task;
        }
        if (g_get_monotonic_time() > deadline)
            g_error("B never made a task for %s", proxy_id);
        g_main_context_iteration(p->context, FALSE);
        g_usleep(20000);
    }
}

static void
wait_for_state(
    Pair        *p,
    ClawtDaemon *daemon,
    const gchar *task_id,
    ClawtTaskState want
){
    gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

    for (;;) {
        ClawtTask *task = clawt_task_manager_get(clawt_daemon_get_tasks(daemon), task_id);
        if (task != NULL && clawt_task_get_state(task) == want)
            return;
        if (g_get_monotonic_time() > deadline)
            g_error("task %s is %s, not %s", task_id,
                    task != NULL ? clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE, clawt_task_get_state(task))
                                 : "gone",
                    clawt_enum_to_nick(CLAWT_TYPE_TASK_STATE, want));
        g_main_context_iteration(p->context, FALSE);
        g_usleep(20000);
    }
}

/* "Delegated to X ... as task T." -> T */
static gchar *
task_id_in(
    const gchar *said
){
    const gchar *at = strstr(said, "as task ");
    const gchar *end;

    g_assert_nonnull(at);
    at += strlen("as task ");
    end = strchr(at, '.');
    g_assert_nonnull(end);
    return g_strndup(at, (gsize)(end - at));
}

/*
 * Delegation between fleets (0.69.0).  B's grant to A asks first, so the
 * first piece of work is held and B's operator lets it through; B's
 * task finishing completes A's proxy with B's result.  With the grant
 * opened, a second piece is taken at once, and A cancelling its proxy
 * cancels the real task.  And B's grant caps what B's agents may do in
 * work for A.
 */
static void
test_delegation(
    gconstpointer data
){
    const Transport *t = data;
    Pair p;
    g_autofree gchar *alias = NULL;
    g_autoptr(GError) error = NULL;
    gboolean is_error = FALSE;
    g_autofree gchar *first = NULL;
    g_autofree gchar *second = NULL;
    ClawtTask *real;

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    alias = link_them(&p, t);
    wait_for_refs(&p, p.ca, "b-chief@bravo-lab");

    /* limited: delegate=ask, so it is held for B's operator */
    {
        g_autofree gchar *said = call_tool(p.a, "a-chief", "clawtilla_delegate",
            "{\"agent_id\":\"b-chief@bravo-lab\",\"task\":\"summarise the lab notes\"}",
            &is_error);
        g_assert_false(is_error);
        first = task_id_in(said);
    }
    {
        gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
        g_autofree gchar *held = NULL;

        while (held == NULL) {
            g_autoptr(GPtrArray) open = clawt_decision_store_list(p.b->decisions, TRUE);
            guint i;

            for (i = 0; open != NULL && i < open->len && held == NULL; i++)
                if (g_str_has_prefix(clawt_decision_get_id(g_ptr_array_index(open, i)), "fed-hold-"))
                    held = g_strdup(clawt_decision_get_id(g_ptr_array_index(open, i)));
            if (held != NULL)
                break;
            if (g_get_monotonic_time() > deadline)
                g_error("B never held the task");
            g_main_context_iteration(p.context, FALSE);
            g_usleep(20000);
        }
        {
            g_autofree gchar *req = g_strdup_printf("{\"decision\":\"%s\",\"answer\":\"deliver\"}",
                                                    held);
            g_autoptr(JsonObject) answered = ask(p.cb, "decision.answer", payload_of(req), &error);
            g_assert_no_error(error);
        }
    }
    real = wait_for_doer_task(&p, first);
    g_assert_cmpstr(clawt_task_get_assignee(real), ==, "b-chief");
    g_assert_cmpstr(clawt_task_get_remote_peer(real), ==, "alpha");
    {
        g_autofree gchar *real_id = g_strdup(clawt_task_get_id(real));
        ClawtTask *proxy = clawt_task_manager_get(clawt_daemon_get_tasks(p.a), first);

        g_assert_cmpstr(clawt_task_get_assignee(proxy), ==, "b-chief@bravo-lab");
        g_assert_true(clawt_task_manager_start(clawt_daemon_get_tasks(p.b), real_id));
        wait_for_state(&p, p.a, first, CLAWT_TASK_RUNNING);
        g_assert_true(clawt_task_manager_complete(clawt_daemon_get_tasks(p.b), real_id,
                                                  "three findings, all minor"));
        wait_for_state(&p, p.a, first, CLAWT_TASK_COMPLETED);
        g_assert_cmpstr(clawt_task_get_result(clawt_task_manager_get(clawt_daemon_get_tasks(p.a),
                                                                     first)),
                        ==, "three findings, all minor");
    }

    /* Opened: taken at once; cancelling the proxy cancels the work there. */
    {
        g_autoptr(JsonObject) set = ask(p.cb, "federation.peer.set",
            payload_of("{\"alias\":\"alpha\",\"grant\":{\"delegate\":\"allow\","
                       "\"cap.exec\":\"deny\",\"subdelegate\":\"false\"}}"), &error);
        g_assert_no_error(error);
    }
    {
        g_autofree gchar *said = call_tool(p.a, "a-chief", "clawtilla_delegate",
            "{\"agent_id\":\"b-chief@bravo-lab\",\"task\":\"check the nightly build\"}",
            &is_error);
        g_assert_false(is_error);
        second = task_id_in(said);
    }
    real = wait_for_doer_task(&p, second);
    {
        g_autofree gchar *real_id = g_strdup(clawt_task_get_id(real));
        g_autofree gchar *req = g_strdup_printf("{\"task\":\"%s\"}", second);
        g_autoptr(JsonObject) cancelled = ask(p.ca, "task.cancel", payload_of(req), &error);

        g_assert_no_error(error);
        wait_for_state(&p, p.b, real_id, CLAWT_TASK_CANCELLED);

        /* The cap: exec is denied in work for A, reading is not. */
        g_assert_cmpint(clawt_daemon_federation_cap(p.b, "b-chief", "fed:alpha:b-chief:a-chief",
                                                    "Bash", CLAWT_ACTION_EXEC),
                        ==, CLAWT_POLICY_DENY);
        g_assert_cmpint(clawt_daemon_federation_cap(p.b, "b-chief", "fed:alpha:b-chief:a-chief",
                                                    "Read", CLAWT_ACTION_READ),
                        ==, CLAWT_POLICY_ALLOW);
        g_assert_cmpint(clawt_daemon_federation_cap(p.b, "b-chief", "fed:alpha:b-chief:a-chief",
                                                    "clawtilla_delegate", CLAWT_ACTION_MESSAGE),
                        ==, CLAWT_POLICY_DENY);
        g_assert_cmpint(clawt_daemon_federation_cap(p.b, "b-chief", "fed:alpha:b-chief:a-chief",
                                                    "x", CLAWT_ACTION_FLEET_ADMIN),
                        ==, CLAWT_POLICY_DENY);
        /* ...and nothing is capped outside work for another fleet */
        g_assert_cmpint(clawt_daemon_federation_cap(p.b, "b-chief", "dm:b-chief:user",
                                                    "Bash", CLAWT_ACTION_EXEC),
                        ==, CLAWT_POLICY_ALLOW);
    }
    /* The estimate before delegating, and the weather, from A's view of B. */
    {
        gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
        const gchar *escrow = NULL;

        for (;;) {
            g_autoptr(JsonObject) shown = ask(p.ca, "federation.peer.show",
                                              payload_of("{\"alias\":\"bravo-lab\"}"), &error);
            g_autofree gchar *weather = NULL;

            g_assert_no_error(error);
            escrow = json_object_has_member(shown, "escrow")
                ? json_object_get_string_member(shown, "escrow") : NULL;
            weather = clawt_fed_weather_line(shown);
            if (escrow != NULL && strstr(escrow, "allows $5.00 a day") != NULL
                && strstr(escrow, "on average") != NULL) {
                g_assert_nonnull(strstr(weather, "traffic"));
                break;
            }
            if (g_get_monotonic_time() > deadline)
                g_error("no estimate: %s", escrow != NULL ? escrow : "(none)");
            g_main_context_iteration(p.context, FALSE);
            g_usleep(20000);
        }
    }
    /* A worker without tools.federation may not hand work to another fleet. */
    {
        g_autofree gchar *said = call_tool(p.a, "a-worker", "clawtilla_delegate",
            "{\"agent_id\":\"b-chief@bravo-lab\",\"task\":\"x\"}", &is_error);
        g_autoptr(GPtrArray) b_tasks = clawt_task_manager_list(clawt_daemon_get_tasks(p.b), NULL, TRUE);

        g_assert_true(is_error);
        g_assert_true(strstr(said, "tools.federation") != NULL
                      || strstr(said, "not permitted") != NULL);
        g_assert_cmpuint(b_tasks->len, ==, 2);
    }
    pair_down(&p);
}

static gboolean
has_room(
    ClawtDaemon *daemon,
    const gchar *room
){
    return clawt_room_manager_get(daemon->rooms, room) != NULL;
}

/*
 * 0.70.0.  B shares a room with A: what B says lands in A's mirror, what
 * A's operator writes there lands in B's room, and unsharing takes the
 * mirror away.  A reads back work it gave B, as far as B's grant lets
 * it.  And a visitor talking to one of ours is drawn on a call with it.
 */
static void
test_rooms_and_transcripts(
    gconstpointer data
){
    const Transport *t = data;
    Pair p;
    g_autofree gchar *alias = NULL;
    g_autoptr(GError) error = NULL;
    gboolean is_error = FALSE;
    const gchar *mirror = "fedroom:bravo-lab:design";
    g_autofree gchar *proxy = NULL;

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    alias = link_them(&p, t);
    wait_for_refs(&p, p.ca, "b-chief@bravo-lab");

    {
        g_autoptr(JsonObject) r = ask(p.cb, "room.create",
            payload_of("{\"room\":\"design\",\"members\":\"b-chief\"}"), &error);
        g_assert_no_error(error);
    }
    /* Refused until B's grant to A lets it join rooms. */
    {
        g_autoptr(JsonObject) r = NULL;
        g_autoptr(JsonObject) set = ask(p.cb, "federation.peer.set",
            payload_of("{\"alias\":\"alpha\",\"grant\":{\"join_rooms\":\"false\"}}"), &error);
        g_assert_no_error(error);
        r = ask(p.cb, "room.share", payload_of("{\"room\":\"design\",\"alias\":\"alpha\"}"), &error);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
        g_clear_error(&error);
    }
    {
        g_autoptr(JsonObject) set = ask(p.cb, "federation.peer.set",
            payload_of("{\"alias\":\"alpha\",\"grant\":{\"join_rooms\":\"true\","
                       "\"delegate\":\"allow\",\"transcripts\":\"own_tasks\"}}"), &error);
        g_autoptr(JsonObject) set_a = NULL;
        g_autoptr(JsonObject) shared = NULL;
        g_assert_no_error(error);
        set_a = ask(p.ca, "federation.peer.set",
            payload_of("{\"alias\":\"bravo-lab\",\"grant\":{\"join_rooms\":\"true\","
                       "\"see_presence\":\"false\"}}"), &error);
        g_assert_no_error(error);
        shared = ask(p.cb, "room.share", payload_of("{\"room\":\"design\",\"alias\":\"alpha\"}"),
                     &error);
        g_assert_no_error(error);
    }
    {
        gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
        while (!has_room(p.a, mirror)) {
            if (g_get_monotonic_time() > deadline)
                g_error("A never got the mirror");
            g_main_context_iteration(p.context, FALSE);
            g_usleep(20000);
        }
    }
    g_assert_true(clawt_room_is_listed(mirror));
    g_assert_cmpstr(clawt_room_get_name(clawt_room_manager_get(p.a->rooms, mirror)), ==,
                    "design @ bravo-lab");

    send_as_operator_plain(p.cb, "design", "the design review is at three");
    wait_for_line(&p, p.ca, mirror, "fleet:bravo-lab:user: the design review is at three");
    send_as_operator_plain(p.ca, mirror, "we will be there");
    wait_for_line(&p, p.cb, "design", "fleet:alpha:user: we will be there");
    /* @a-chief@alpha in B's room wakes A's chief, in A's mirror of it (0.72.0). */
    send_as_operator_plain(p.cb, "design", "@a-chief@alpha can you take the notes?");
    wait_for_line(&p, p.ca, mirror, "can you take the notes?");
    {
        ClawtAgent *chief = clawt_agent_manager_get(p.a->agents, "a-chief");
        g_autoptr(GPtrArray) items = clawt_mailbox_list(clawt_agent_get_mailbox(chief), NULL);
        g_autofree gchar *delivered = NULL;

        g_assert_cmpuint(items->len, >, 0);
        delivered = clawt_mailbox_item_dup_delivered_body(g_ptr_array_index(items, items->len - 1));
        g_assert_nonnull(strstr(delivered, "mentioned you"));
    }
    /* and B's composer can complete A's agents in the room it shares */
    {
        g_autoptr(JsonObject) listed = ask(p.cb, "room.list", NULL, &error);
        JsonArray *rooms = json_object_get_array_member(listed, "rooms");
        gboolean offered = FALSE;
        guint r;

        g_assert_no_error(error);
        for (r = 0; r < json_array_get_length(rooms); r++) {
            JsonObject *one = json_array_get_object_element(rooms, r);
            JsonArray *names;
            guint n;

            if (g_strcmp0(json_object_get_string_member(one, "id"), "design") != 0
                || !json_object_has_member(one, "mention_names"))
                continue;
            names = json_object_get_array_member(one, "mention_names");
            for (n = 0; n < json_array_get_length(names); n++)
                if (g_strcmp0(json_array_get_string_element(names, n), "a-chief@alpha") == 0)
                    offered = TRUE;
        }
        g_assert_true(offered);
    }
    /* nothing echoes back into A's mirror from A's own post */
    {
        g_autofree gchar *a_side = room_lines(p.ca, mirror);
        g_assert_null(strstr(a_side, "fleet:bravo-lab:fleet:alpha"));
    }

    /* Read back work A gave B. */
    {
        g_autofree gchar *said = call_tool(p.a, "a-chief", "clawtilla_delegate",
            "{\"agent_id\":\"b-chief@bravo-lab\",\"task\":\"list the open design questions\"}",
            &is_error);
        g_assert_false(is_error);
        proxy = task_id_in(said);
    }
    wait_for_doer_task(&p, proxy);
    {
        g_autofree gchar *req = g_strdup_printf("{\"alias\":\"bravo-lab\",\"task\":\"%s\"}", proxy);
        g_autoptr(JsonObject) transcript = ask(p.ca, "federation.transcript", payload_of(req),
                                               &error);
        JsonArray *rooms;
        g_autofree gchar *text = NULL;
        g_autoptr(JsonNode) whole = NULL;

        g_assert_no_error(error);
        g_assert_true(json_object_has_member(transcript, "task"));
        rooms = json_object_get_array_member(transcript, "rooms");
        g_assert_cmpuint(json_array_get_length(rooms), ==, 1);
        whole = json_node_init_object(json_node_alloc(), transcript);
        text = json_to_string(whole, FALSE);
        g_assert_nonnull(strstr(text, "list the open design questions"));
    }
    /* own_tasks does not open the conversations themselves */
    {
        g_autoptr(JsonObject) none = ask(p.ca, "federation.transcript",
            payload_of("{\"alias\":\"bravo-lab\",\"agent\":\"b-chief\"}"), &error);
        g_assert_nonnull(error);
        g_clear_error(&error);
    }

    /* The handshake: A's chief, writing to B's chief, is on a call with it on B's floor. */
    {
        g_autofree gchar *said = call_tool(p.a, "a-chief", "clawtilla_message_agent",
            "{\"agent_id\":\"b-chief@bravo-lab\",\"body\":\"quick question\"}", &is_error);
        gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
        gboolean on_call = FALSE;

        g_assert_false(is_error);
        while (!on_call) {
            g_autoptr(JsonArray) floor = json_array_new();
            guint i;

            clawt_daemon_federation_office_visitors(p.b, floor, NULL);
            for (i = 0; i < json_array_get_length(floor); i++) {
                JsonObject *v = json_array_get_object_element(floor, i);
                if (g_strcmp0(json_object_get_string_member(v, "id"), "alpha:a-chief") == 0
                    && g_strcmp0(json_object_get_string_member(v, "activity"), "on_call") == 0
                    && g_strcmp0(json_object_get_string_member(v, "peer"), "b-chief") == 0)
                    on_call = TRUE;
            }
            if (on_call)
                break;
            if (g_get_monotonic_time() > deadline)
                g_error("a-chief never showed on a call with b-chief");
            g_main_context_iteration(p.context, FALSE);
            g_usleep(20000);
        }
    }

    /* Unshared: the mirror goes. */
    {
        g_autoptr(JsonObject) r = ask(p.cb, "room.unshare",
            payload_of("{\"room\":\"design\",\"alias\":\"alpha\"}"), &error);
        gint64 deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;

        g_assert_no_error(error);
        while (has_room(p.a, mirror)) {
            if (g_get_monotonic_time() > deadline)
                g_error("A kept the mirror");
            g_main_context_iteration(p.context, FALSE);
            g_usleep(20000);
        }
    }
    pair_down(&p);
}

/*
 * 0.71.0.  discover finds a fleet at an endpoint it is given (and
 * nothing at one that is not there), saying it is linked once it is;
 * migrate-office links an office.peers entry as a fleet A owns; and once
 * work has crossed, A's view of B carries the estimate shown before
 * delegating and the link's weather.
 */
static void
test_discover_and_migrate(void)
{
    Pair p;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *b_sock = NULL;
    g_autofree gchar *req = NULL;

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    b_sock = g_build_filename(p.dir, "b.sock", NULL);

    req = g_strdup_printf("{\"candidates\":[\"unix:%s#ipc\",\"unix:%s/none.sock#ipc\"]}",
                          b_sock, p.dir);
    {
        g_autoptr(JsonObject) found = ask(p.ca, "federation.discover", payload_of(req), &error);
        JsonArray *fleets;
        JsonObject *f;

        g_assert_no_error(error);
        fleets = json_object_get_array_member(found, "fleets");
        g_assert_cmpuint(json_array_get_length(fleets), ==, 1);
        f = json_array_get_object_element(fleets, 0);
        g_assert_cmpstr(json_object_get_string_member(f, "name"), ==, "Bravo Lab");
        g_assert_cmpstr(json_object_get_string_member(f, "fleet_id"), ==, p.b->fed_fleet_id);
        g_assert_false(json_object_has_member(f, "linked_as"));
    }

    /* office.peers -> a link */
    {
        g_autofree gchar *entry = g_strdup_printf("lab=%s", b_sock);
        const gchar *entries[] = { entry, NULL };
        g_autoptr(JsonObject) listed = NULL;
        g_autoptr(JsonObject) linked = NULL;
        JsonArray *rows;

        g_assert_true(clawt_config_set_string_list(p.a->config, "office.peers", entries));
        {
            g_autoptr(JsonObject) status = ask(p.ca, "federation.status", NULL, &error);
            g_assert_no_error(error);
            g_assert_nonnull(strstr(json_object_get_string_member(status, "deprecated"),
                                    "migrate-office"));
        }
        listed = ask(p.ca, "federation.migrate_office", NULL, &error);
        g_assert_no_error(error);
        rows = json_object_get_array_member(listed, "entries");
        g_assert_cmpuint(json_array_get_length(rows), ==, 1);
        g_assert_false(json_object_has_member(json_array_get_object_element(rows, 0), "linked_as"));
        linked = ask(p.ca, "federation.migrate_office", payload_of("{\"name\":\"lab\"}"), &error);
        g_assert_no_error(error);
        g_assert_cmpstr(json_object_get_string_member(linked, "tier"), ==, "owned");
    }
    {
        g_autoptr(JsonObject) found = ask(p.ca, "federation.discover", payload_of(req), &error);
        JsonObject *f;

        g_assert_no_error(error);
        f = json_array_get_object_element(json_object_get_array_member(found, "fleets"), 0);
        g_assert_cmpstr(json_object_get_string_member(f, "linked_as"), ==, "lab");
    }
    pair_down(&p);
}

static void
set_agent(
    ClawtClient *client,
    const gchar *agent,
    const gchar *key,
    const gchar *value
){
    g_autoptr(GError) error = NULL;
    g_autofree gchar *req = g_strdup_printf("{\"agent\":\"%s\",\"key\":\"%s\",\"value\":\"%s\"}",
                                            agent, key, value);
    g_autoptr(JsonObject) r = ask(client, "agent.set", payload_of(req), &error);

    g_assert_no_error(error);
}

/*
 * 0.72.0.  An agent's federation_may_reach keeps it to the fleets it
 * names; the admin tools tag, lower a tier at once, put a raise to the
 * operator, and invite; the rules editor refuses a rule that would not
 * apply and saves one that would, which then shows in explain.
 */
static void
test_reach_admin_and_rules(void)
{
    Pair p;
    g_autofree gchar *alias = NULL;
    g_autoptr(GError) error = NULL;
    gboolean is_error = FALSE;

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    alias = link_them(&p, &transports[1]);
    wait_for_refs(&p, p.ca, "b-chief@bravo-lab");

    /* B is trusted on A: a chief kept to owned fleets may not write to it */
    set_agent(p.ca, "a-chief", "federation_may_reach", "tier:owned");
    {
        g_autofree gchar *said = call_tool(p.a, "a-chief", "clawtilla_message_agent",
            "{\"agent_id\":\"b-chief@bravo-lab\",\"body\":\"hi\"}", &is_error);
        g_autofree gchar *listing = call_tool(p.a, "a-chief", "clawtilla_list_agents", "{}",
                                              &is_error);
        g_assert_nonnull(strstr(said, "federation_may_reach"));
        g_assert_null(strstr(listing, "b-chief@bravo-lab"));
    }
    set_agent(p.ca, "a-chief", "federation_may_reach", "*");

    /* The admin tools, for a chief that may also manage the fleet. */
    set_agent(p.ca, "a-chief", "tools.manage_fleet", "true");
    {
        g_autofree gchar *tagged = call_tool(p.a, "a-chief", "clawtilla_federation_tag",
            "{\"alias\":\"bravo-lab\",\"add\":\"partner\"}", &is_error);
        if (is_error)
            g_error("tag said: %s", tagged);
        g_autofree gchar *raised = NULL;
        g_autofree gchar *lowered = NULL;
        g_autofree gchar *invite = NULL;
        g_autoptr(GPtrArray) open = NULL;
        g_autofree gchar *held = NULL;
        guint i;

        g_assert_false(is_error);
        {
            g_autoptr(JsonObject) shown = ask(p.ca, "federation.peer.show",
                                              payload_of("{\"alias\":\"bravo-lab\"}"), &error);
            JsonArray *tags = json_object_get_array_member(shown, "tags");
            gboolean found = FALSE;
            guint t;

            g_assert_no_error(error);
            for (t = 0; tags != NULL && t < json_array_get_length(tags); t++)
                if (g_strcmp0(json_array_get_string_element(tags, t), "partner") == 0)
                    found = TRUE;
            g_assert_true(found);
        }
        raised = call_tool(p.a, "a-chief", "clawtilla_federation_set_tier",
                           "{\"alias\":\"bravo-lab\",\"tier\":\"owned\"}", &is_error);
        if (is_error)
            g_error("set_tier said: %s", raised);
        g_assert_nonnull(strstr(raised, "operator"));
        g_assert_cmpstr(peer_field(p.ca, "tier"), ==, "trusted");
        open = clawt_decision_store_list(p.a->decisions, TRUE);
        for (i = 0; open != NULL && i < open->len && held == NULL; i++)
            if (g_str_has_prefix(clawt_decision_get_id(g_ptr_array_index(open, i)), "fed-tier-"))
                held = g_strdup(clawt_decision_get_id(g_ptr_array_index(open, i)));
        g_assert_nonnull(held);
        {
            g_autofree gchar *req = g_strdup_printf("{\"decision\":\"%s\",\"answer\":\"approve\"}",
                                                    held);
            g_autoptr(JsonObject) answered = ask(p.ca, "decision.answer", payload_of(req), &error);
            g_assert_no_error(error);
        }
        g_assert_cmpstr(peer_field(p.ca, "tier"), ==, "owned");
        lowered = call_tool(p.a, "a-chief", "clawtilla_federation_set_tier",
                            "{\"alias\":\"bravo-lab\",\"tier\":\"limited\"}", &is_error);
        g_assert_false(is_error);
        g_assert_cmpstr(peer_field(p.ca, "tier"), ==, "limited");
        invite = call_tool(p.a, "a-chief", "clawtilla_federation_invite",
                           "{\"tier\":\"trusted\"}", &is_error);
        g_assert_true(is_error);
        g_free(invite);
        {
            g_autofree gchar *args = g_strdup_printf("{\"endpoint\":\"unix:%s/a.sock#ipc\"}", p.dir);
            invite = call_tool(p.a, "a-chief", "clawtilla_federation_invite", args, &is_error);
        }
        if (is_error)
            g_error("invite said: %s", invite);
        if (strstr(invite, CLAWT_FED_INVITE_PREFIX) == NULL)
            g_error("invite said: %s", invite);
    }
    /* A worker without manage_fleet is not offered them. */
    {
        g_autofree gchar *said = call_tool(p.a, "a-worker", "clawtilla_federation_revoke",
            "{\"alias\":\"bravo-lab\"}", &is_error);
        g_assert_true(is_error);
    }

    /* Rules: refused when they would not apply, explained when they do. */
    {
        g_autoptr(JsonObject) bad = ask(p.ca, "federation.rules",
            payload_of("{\"rules\":[\"tag:partner cap.exec=deny\"]}"), &error);
        g_autoptr(JsonObject) good = NULL;
        g_autoptr(JsonObject) shown = NULL;
        g_autofree gchar *text = NULL;
        g_autoptr(JsonNode) whole = NULL;

        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
        g_clear_error(&error);
        good = ask(p.ca, "federation.rules",
                   payload_of("{\"rules\":[\"tag:partner: cap.exec=deny\"]}"), &error);
        g_assert_no_error(error);
        g_assert_cmpuint(json_array_get_length(json_object_get_array_member(good, "rules")), ==, 1);
        shown = ask(p.ca, "federation.peer.explain", payload_of("{\"alias\":\"bravo-lab\"}"), &error);
        g_assert_no_error(error);
        whole = json_node_init_object(json_node_alloc(), shown);
        text = json_to_string(whole, FALSE);
        g_assert_nonnull(strstr(text, "rule 1: tag:partner"));
    }
    pair_down(&p);
}

/* What B's hello says when signed by A, for a hand-made connection. */
static const gchar *
hello_as_a(
    ClawtClient  *client,
    JsonNode    **out_payload,
    gpointer      user_data
){
    Pair *p = user_data;
    g_autoptr(ClawtFedStamp) stamp = clawt_fed_stamp_sign(
        p->a->fed_key, p->a->fed_key_index, "hello", p->a->fed_fleet_id,
        p->b->fed_fleet_id, "{}", "IPC fed.hello", NULL);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT);

    (void)client;
    json_node_take_object(node, clawt_fed_envelope_build(stamp, "{}"));
    *out_payload = node;
    return "fed.hello";
}

/* A fleet connection is a fleet: fed.* frames only, no events, no replays. */
static void
test_fleet_connection_is_fenced(void)
{
    Pair p;
    g_autofree gchar *alias = NULL;
    g_autofree gchar *b_sock = NULL;
    g_autoptr(ClawtClient) fleet = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) refused = NULL;
    g_autoptr(ClawtFedStamp) stamp = NULL;
    g_autoptr(JsonNode) once = NULL;
    g_autoptr(JsonNode) twice = NULL;
    JsonNode *frame;

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    alias = link_them(&p, &transports[1]);

    b_sock = g_build_filename(p.dir, "b.sock", NULL);
    fleet = clawt_client_new(b_sock);
    clawt_client_set_hello_func(fleet, hello_as_a, &p, NULL);
    g_assert_true(clawt_client_connect(fleet, &error));
    g_assert_no_error(error);

    /* The owner's socket, and still: once a fleet, never a client. */
    refused = clawt_client_request_full(fleet, "agent.list", NULL, 10, &error);
    g_assert_null(refused);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);
    g_clear_error(&error);
    g_assert_false(clawt_client_subscribe(fleet, 0, NULL, &error));
    g_clear_error(&error);

    /* A signed ping answers once; the same frame again is a replay. */
    stamp = clawt_fed_stamp_sign(p.a->fed_key, p.a->fed_key_index, "ping", p.a->fed_fleet_id,
                                 p.b->fed_fleet_id, "{}", "IPC fed.envelope", NULL);
    frame = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(frame, clawt_fed_envelope_build(stamp, "{}"));
    once = clawt_client_request_full(fleet, "fed.envelope", json_node_copy(frame), 10, &error);
    g_assert_no_error(error);
    g_assert_nonnull(once);
    twice = clawt_client_request_full(fleet, "fed.envelope", frame, 10, &error);
    g_assert_null(twice);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_AUTH);

    clawt_client_disconnect(fleet);
    pair_down(&p);
}

/* Grants are set through the field table and explained by source. */
static void
test_grant_override_and_explain(void)
{
    Pair p;
    g_autofree gchar *alias = NULL;
    g_autofree gchar *req = NULL;
    g_autoptr(JsonObject) shown = NULL;
    g_autoptr(GError) error = NULL;
    JsonArray *fields;
    gboolean saw = FALSE;
    guint i;

    memset(&p, 0, sizeof(p));
    pair_up(&p);
    alias = link_them(&p, &transports[0]);

    req = g_strdup_printf("{\"alias\":\"%s\",\"grant\":{\"cap.exec\":\"deny\",\"reach\":\"listed\"}}",
                          alias);
    shown = ask(p.ca, "federation.peer.set", payload_of(req), &error);
    g_assert_no_error(error);
    fields = json_object_get_array_member(shown, "grant");
    for (i = 0; i < json_array_get_length(fields); i++) {
        JsonObject *f = json_array_get_object_element(fields, i);
        if (g_strcmp0(json_object_get_string_member(f, "name"), "cap.exec") == 0) {
            g_assert_cmpstr(json_object_get_string_member(f, "value"), ==, "deny");
            g_assert_cmpstr(json_object_get_string_member(f, "source"), ==, "override");
            saw = TRUE;
        }
    }
    g_assert_true(saw);

    /* A value the field cannot hold is refused and nothing is changed. */
    g_free(req);
    req = g_strdup_printf("{\"alias\":\"%s\",\"grant\":{\"cap.write\":\"deny\",\"reach\":\"most\"}}",
                          alias);
    g_assert_null(ask(p.ca, "federation.peer.set", payload_of(req), &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(json_object_has_member(
        clawt_fed_registry_get(p.a->fed_registry, alias)->overrides, "cap.write"));

    /* Tags and filters. */
    g_free(req);
    req = g_strdup_printf("{\"alias\":\"%s\",\"add\":[\"client-acme\"],\"remove\":[\"gpu\"]}", alias);
    g_clear_pointer(&shown, json_object_unref);
    shown = ask(p.ca, "federation.peer.tag", payload_of(req), &error);
    g_assert_no_error(error);
    {
        g_autoptr(JsonObject) hit = ask(p.ca, "federation.peer.list",
                                        payload_of("{\"filter\":\"tag:client-acme+tier:trusted\"}"),
                                        &error);
        g_autoptr(JsonObject) miss = ask(p.ca, "federation.peer.list",
                                         payload_of("{\"filter\":\"tag:gpu\"}"), &error);
        g_assert_cmpuint(json_array_get_length(json_object_get_array_member(hit, "peers")), ==, 1);
        g_assert_cmpuint(json_array_get_length(json_object_get_array_member(miss, "peers")), ==, 0);
        g_assert_null(ask(p.ca, "federation.peer.list",
                          payload_of("{\"filter\":\"colour:red\"}"), &error));
        g_clear_error(&error);
    }
    pair_down(&p);
}

int
main(int argc, char *argv[])
{
    guint i;

    /* The skill library reads ~/.agents/skills; never the operator's own. */
    g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

    for (i = 0; i < G_N_ELEMENTS(transports); i++) {
        g_autofree gchar *path = g_strdup_printf("/fed-link/%s/link-ping-revoke",
                                                 transports[i].name);
        g_autofree gchar *rpath = g_strdup_printf("/fed-link/%s/rosters-follow-grants",
                                                  transports[i].name);
        g_test_add_data_func(path, &transports[i], test_link_ping_revoke);
        g_test_add_data_func(rpath, &transports[i], test_rosters_follow_grants);
        {
            g_autofree gchar *mpath = g_strdup_printf("/fed-link/%s/messages-cross",
                                                      transports[i].name);
            g_test_add_data_func(mpath, &transports[i], test_messages_cross);
        }
        {
            g_autofree gchar *dpath = g_strdup_printf("/fed-link/%s/delegation",
                                                      transports[i].name);
            g_test_add_data_func(dpath, &transports[i], test_delegation);
        }
        {
            g_autofree gchar *rpath2 = g_strdup_printf("/fed-link/%s/rooms-and-transcripts",
                                                       transports[i].name);
            g_test_add_data_func(rpath2, &transports[i], test_rooms_and_transcripts);
        }
    }
    g_test_add_func("/fed-link/fleet-connection-is-fenced", test_fleet_connection_is_fenced);
    g_test_add_func("/fed-link/grant-override-and-explain", test_grant_override_and_explain);
    g_test_add_func("/fed-link/agent-tools", test_agent_tools);
    g_test_add_func("/fed-link/discover-and-migrate", test_discover_and_migrate);
    g_test_add_func("/fed-link/reach-admin-and-rules", test_reach_admin_and_rules);

    return g_test_run();
}
