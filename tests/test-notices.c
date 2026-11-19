/*
 * test-notices.c - One notification list, held by the daemon
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Three halves, failing in different ways.
 *
 * The **store** is a record: a row added comes back, read and dismissed
 * are separate flags, a clear can be narrowed to one agent, pruning bites
 * by count and by age, and a timed mute lapses on its own clock.  Every
 * time is passed in, so "lapses at `until`" is asserted at `until` rather
 * than after a sleep.
 *
 * The **wording** is a golden per kind.  The sentences used to be made in
 * three clients; they are made once now, in clawt_alert_text_for_event(),
 * and these are the GTK client's sentences, so moving them changed no
 * word anybody reads.
 *
 * The **daemon** half drives a real daemon in a private directory: an
 * event published on its bus becomes a notice or does not, a muted
 * agent's arrives read, the `sources` counts leave a muted agent out,
 * and clawt_daemon_notify() -- the one door to the push and desktop
 * notifiers -- stays shut for a muted agent.  It includes
 * core/clawt-daemon-private.h for the same reason tests/test-handoff.c
 * does: that header is daemon-notice.c's interface, and a hermetic suite
 * has no phone to buzz, so the notifier is replaced by a recorder there.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

#include "core/clawt-daemon-private.h"

#define DAY ((gint64)24 * 60 * 60)

/* ── The store ───────────────────────────────────────────────────── */

typedef struct {
    gchar            *dir;
    gchar            *path;
    ClawtNoticeStore *store;
} StoreFixture;

static void
store_setup(StoreFixture *fixture)
{
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-notices-XXXXXX", NULL);
    fixture->path = g_build_filename(fixture->dir, "notices.db", NULL);
    fixture->store = clawt_notice_store_new(fixture->path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(fixture->store);
}

static void
store_teardown(StoreFixture *fixture)
{
    g_clear_object(&fixture->store);
    clawt_test_remove_tree(fixture->dir);
    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->path, g_free);
}

static gint64
add_one(StoreFixture *fixture, const gchar *agent, const gchar *title,
        gint64 at)
{
    ClawtNotice notice = { 0 };
    g_autoptr(GError) error = NULL;
    gint64 id;

    notice.agent = (gchar *)agent;
    notice.tier = CLAWT_ALERT_ERROR;
    notice.event_kind = (gchar *)"turn.timed_out";
    notice.source = (gchar *)"turn.timed_out";
    notice.subject = (gchar *)agent;
    notice.title = (gchar *)title;
    notice.created_at = at;

    id = clawt_notice_store_add(fixture->store, &notice, FALSE, &error);
    g_assert_no_error(error);
    g_assert_cmpint(id, >, 0);

    return id;
}

/*
 * Every field written comes back, across a reopen -- a column bound to
 * the wrong index is silent, and "it survives a restart" asserted
 * against a live object asserts nothing.
 */
static void
test_a_notice_round_trips(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(ClawtNotice) read = NULL;
    g_autoptr(GError) error = NULL;
    gint64 id;

    store_setup(&fixture);

    id = add_one(&fixture, "scout", "scout timed out", 1000);
    g_clear_object(&fixture.store);

    fixture.store = clawt_notice_store_new(fixture.path, &error);
    g_assert_no_error(error);

    read = clawt_notice_store_get(fixture.store, id);
    g_assert_nonnull(read);
    g_assert_cmpstr(read->agent, ==, "scout");
    g_assert_cmpint(read->tier, ==, CLAWT_ALERT_ERROR);
    g_assert_cmpstr(read->event_kind, ==, "turn.timed_out");
    g_assert_cmpstr(read->source, ==, "turn.timed_out");
    g_assert_cmpstr(read->subject, ==, "scout");
    g_assert_cmpstr(read->title, ==, "scout timed out");
    g_assert_cmpint(read->created_at, ==, 1000);
    g_assert_false(read->read);
    g_assert_false(read->dismissed);

    g_assert_null(clawt_notice_store_get(fixture.store, id + 99));

    store_teardown(&fixture);
}

/*
 * Read and dismissed are separate: reading one leaves it showing,
 * dismissing hides it and reads it, "read all" reads every one, and a
 * clear narrowed to an agent touches only that agent's.  Regressing any
 * of them is a bell that cannot be put out, or a Clear that wipes a
 * different agent's errors.
 */
static void
test_read_dismiss_and_clear(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GPtrArray) showing = NULL;
    g_autoptr(GPtrArray) everything = NULL;
    gint64 a;
    gint64 b;
    gint64 c;

    store_setup(&fixture);

    a = add_one(&fixture, "scout", "one", 10);
    b = add_one(&fixture, "scout", "two", 20);
    c = add_one(&fixture, "zed", "three", 30);
    g_assert_cmpuint(clawt_notice_store_count_unread(fixture.store, NULL),
                     ==, 3);

    /* one read: still listed, no longer counted */
    g_assert_cmpuint(clawt_notice_store_mark_read(fixture.store, a), ==, 1);
    g_assert_cmpuint(clawt_notice_store_mark_read(fixture.store, a), ==, 0);
    g_assert_cmpuint(clawt_notice_store_count_unread(fixture.store, NULL),
                     ==, 2);

    /* one dismissed: gone from the list, and read */
    g_assert_true(clawt_notice_store_dismiss(fixture.store, b));
    g_assert_false(clawt_notice_store_dismiss(fixture.store, b));
    showing = clawt_notice_store_list(fixture.store, NULL, FALSE, 0);
    g_assert_cmpuint(showing->len, ==, 2);
    g_assert_cmpuint(clawt_notice_store_count_unread(fixture.store, NULL),
                     ==, 1);

    /* newest first */
    g_assert_cmpint(((ClawtNotice *)g_ptr_array_index(showing, 0))->id, ==,
                    c);

    /* a clear for scout leaves zed's */
    g_assert_cmpuint(clawt_notice_store_clear(fixture.store, "scout"), ==, 1);
    g_clear_pointer(&showing, g_ptr_array_unref);
    showing = clawt_notice_store_list(fixture.store, NULL, FALSE, 0);
    g_assert_cmpuint(showing->len, ==, 1);
    g_assert_cmpstr(((ClawtNotice *)g_ptr_array_index(showing, 0))->agent,
                    ==, "zed");

    /* and a clear for everybody takes the rest; nothing is deleted */
    g_assert_cmpuint(clawt_notice_store_clear(fixture.store, NULL), ==, 1);
    g_clear_pointer(&showing, g_ptr_array_unref);
    showing = clawt_notice_store_list(fixture.store, NULL, FALSE, 0);
    g_assert_cmpuint(showing->len, ==, 0);
    everything = clawt_notice_store_list(fixture.store, NULL, TRUE, 0);
    g_assert_cmpuint(everything->len, ==, 3);

    /* "read all" on a fresh one */
    add_one(&fixture, "scout", "four", 40);
    add_one(&fixture, "zed", "five", 50);
    g_assert_cmpuint(clawt_notice_store_mark_read(fixture.store, 0), ==, 2);
    g_assert_cmpuint(clawt_notice_store_count_unread(fixture.store, NULL),
                     ==, 0);

    store_teardown(&fixture);
}

/*
 * notices.keep and notices.keep_days.  By count: the newest N stay.  By
 * age: anything older than the window goes, at an injected `now`.
 * Regressing it is a database that grows for as long as the fleet runs.
 */
static void
test_pruning_by_count_and_by_age(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GPtrArray) left = NULL;
    gint64 now = 100 * DAY;
    gint i;

    store_setup(&fixture);

    for (i = 0; i < 5; i++)
        add_one(&fixture, "scout", "by count", now - i);

    g_assert_cmpuint(clawt_notice_store_prune(fixture.store, 3, 0, now), ==,
                     2);
    g_assert_cmpuint(clawt_notice_store_count(fixture.store), ==, 3);

    /* the three kept are the newest three */
    left = clawt_notice_store_list(fixture.store, NULL, TRUE, 0);
    g_assert_cmpint(((ClawtNotice *)g_ptr_array_index(left, 2))->created_at,
                    ==, now - 2);

    /* two days old, with a one-day window: gone; a fresh one stays */
    add_one(&fixture, "scout", "old", now - 2 * DAY);
    add_one(&fixture, "scout", "new", now);
    g_assert_cmpuint(clawt_notice_store_prune(fixture.store, 0, 1, now), ==,
                     1);
    g_assert_cmpuint(clawt_notice_store_count(fixture.store), ==, 4);

    /* zero for both keeps everything */
    g_assert_cmpuint(clawt_notice_store_prune(fixture.store, 0, 0,
                                              now + 1000 * DAY), ==, 0);

    store_teardown(&fixture);
}

/*
 * A timed mute holds until `until` and not a second longer, whether or
 * not a prune has run; one with no `until` holds until unmuted.  The
 * soonest lapse is what the daemon arms a timer for.  Regressing it is
 * "mute for 2h" that never ends, or ends at the next hourly prune.
 */
static void
test_a_timed_mute_lapses(void)
{
    StoreFixture fixture = { 0 };
    g_autoptr(GPtrArray) mutes = NULL;
    g_autoptr(GHashTable) set = NULL;
    g_autoptr(GError) error = NULL;

    store_setup(&fixture);

    g_assert_true(clawt_notice_store_mute(fixture.store, "scout", 2000,
                                          &error));
    g_assert_true(clawt_notice_store_mute(fixture.store, "zed", 0, &error));
    g_assert_no_error(error);

    g_assert_true(clawt_notice_store_is_muted(fixture.store, "scout", 1999));
    g_assert_false(clawt_notice_store_is_muted(fixture.store, "scout", 2000));
    g_assert_true(clawt_notice_store_is_muted(fixture.store, "zed", 99999));
    g_assert_false(clawt_notice_store_is_muted(fixture.store, "alpha", 0));

    g_assert_cmpint(clawt_notice_store_next_lapse(fixture.store, 1000), ==,
                    2000);
    g_assert_cmpint(clawt_notice_store_next_lapse(fixture.store, 2000), ==,
                    0);

    mutes = clawt_notice_store_list_mutes(fixture.store, 2500);
    g_assert_cmpuint(mutes->len, ==, 1);
    g_assert_cmpstr(((ClawtNoticeMute *)g_ptr_array_index(mutes, 0))->agent,
                    ==, "zed");

    set = clawt_notice_store_muted_set(fixture.store, 1500);
    g_assert_cmpuint(g_hash_table_size(set), ==, 2);

    /* a muted agent's unread notices leave the bell */
    add_one(&fixture, "zed", "hidden", 10);
    add_one(&fixture, "alpha", "shown", 10);
    g_assert_cmpuint(clawt_notice_store_count_unread(fixture.store, set), ==,
                     1);

    g_assert_true(clawt_notice_store_unmute(fixture.store, "zed"));
    g_assert_false(clawt_notice_store_unmute(fixture.store, "zed"));
    g_assert_false(clawt_notice_store_is_muted(fixture.store, "zed", 1500));

    store_teardown(&fixture);
}

/*
 * `clawtilla mute AGENT --for 2h`: the units, the bare number as
 * minutes, and the refusals.  Regressing it mutes for two minutes when
 * somebody asked for two hours.
 */
static void
test_the_mute_length_parses(void)
{
    gint64 seconds = -1;

    g_assert_true(clawt_notice_mute_for_parse("2h", &seconds));
    g_assert_cmpint(seconds, ==, 2 * 60 * 60);
    g_assert_true(clawt_notice_mute_for_parse("90s", &seconds));
    g_assert_cmpint(seconds, ==, 90);
    g_assert_true(clawt_notice_mute_for_parse("30m", &seconds));
    g_assert_cmpint(seconds, ==, 30 * 60);
    g_assert_true(clawt_notice_mute_for_parse("15", &seconds));
    g_assert_cmpint(seconds, ==, 15 * 60);
    g_assert_true(clawt_notice_mute_for_parse("3d", &seconds));
    g_assert_cmpint(seconds, ==, 3 * DAY);

    g_assert_false(clawt_notice_mute_for_parse("0h", &seconds));
    g_assert_cmpint(seconds, ==, 0);
    g_assert_false(clawt_notice_mute_for_parse("2hours", &seconds));
    g_assert_false(clawt_notice_mute_for_parse("-2h", &seconds));
    g_assert_false(clawt_notice_mute_for_parse("h", &seconds));
    g_assert_false(clawt_notice_mute_for_parse("2w", &seconds));
    g_assert_false(clawt_notice_mute_for_parse("400d", &seconds));
    g_assert_false(clawt_notice_mute_for_parse(NULL, &seconds));
}

/* ── The wording ─────────────────────────────────────────────────── */

static gchar *
text_of(ClawtEvent *event, gchar **source, gchar **agent)
{
    return clawt_alert_text_for_event(event, source, agent);
}

/*
 * The GTK client's sentences, one per kind, exactly.  Every client and
 * the daemon now draw these rows from one function, so a word changed
 * here is a word changed on every screen -- which is the point, and why
 * it is pinned.
 */
static void
test_the_alert_text_is_gtks(void)
{
    g_autoptr(ClawtEvent) refused = clawt_event_new("message.refused", NULL);
    g_autoptr(ClawtEvent) refused_bare =
        clawt_event_new("message.refused", NULL);
    g_autoptr(ClawtEvent) download = clawt_event_new("image.finished",
                                                     "debian-13");
    g_autoptr(ClawtEvent) downloaded = clawt_event_new("image.finished",
                                                       "debian-13");
    g_autoptr(ClawtEvent) routine = clawt_event_new("routine.turned_off",
                                                    "nightly");
    g_autoptr(ClawtEvent) state = clawt_event_new("agent.state", "scout");
    g_autoptr(ClawtEvent) tree = clawt_event_new("task.tree_budget",
                                                 "chief");
    g_autoptr(ClawtEvent) failover = clawt_event_new("agent.failover",
                                                     "scout");
    g_autoptr(ClawtEvent) timed_out = clawt_event_new("turn.timed_out",
                                                      "scout");
    g_autoptr(ClawtEvent) bare = clawt_event_new("agent.contested", NULL);
    g_autoptr(ClawtEvent) typing = clawt_event_new("agent.typing", "scout");
    g_autoptr(ClawtEvent) bookkeeping = clawt_event_new("notice.changed",
                                                        NULL);
    g_autofree gchar *expected_routine = NULL;
    g_autofree gchar *expected_tree = NULL;
    gchar *text;
    gchar *source = NULL;
    gchar *agent = NULL;

    clawt_event_set_detail(refused, "from", "scout");
    clawt_event_set_detail(refused, "to", "zed");
    clawt_event_set_detail(refused, "reason", "the hop limit");
    text = text_of(refused, &source, &agent);
    g_assert_cmpstr(text, ==, "scout to zed was stopped: the hop limit");
    g_assert_cmpstr(source, ==, "message");
    g_assert_cmpstr(agent, ==, "scout");
    g_free(text);
    g_clear_pointer(&source, g_free);
    g_clear_pointer(&agent, g_free);

    text = text_of(refused_bare, &source, &agent);
    g_assert_cmpstr(text, ==, "? to ? was stopped: a limit");
    g_assert_null(agent);
    g_free(text);
    g_clear_pointer(&source, g_free);

    clawt_event_set_detail(download, "error", "404");
    text = text_of(download, &source, &agent);
    g_assert_cmpstr(text, ==, "debian-13 could not be downloaded: 404");
    g_assert_cmpstr(source, ==, "download");
    g_assert_cmpstr(agent, ==, "debian-13");
    g_free(text);
    g_clear_pointer(&source, g_free);
    g_clear_pointer(&agent, g_free);

    /* a download that worked is routine, and worded as one */
    text = text_of(downloaded, NULL, NULL);
    g_assert_cmpstr(text, ==, "image.finished \302\267 debian-13");
    g_free(text);

    clawt_event_set_detail(routine, "agent", "scout");
    clawt_event_set_detail(routine, "failures", "3");
    clawt_event_set_detail(routine, "error", "no podman");
    expected_routine = clawt_routine_turned_off_alert("nightly", 3,
                                                      "no podman");
    text = text_of(routine, &source, &agent);
    g_assert_cmpstr(text, ==, expected_routine);
    g_assert_cmpstr(source, ==, "routine.turned_off");
    g_assert_cmpstr(agent, ==, "scout");
    g_free(text);
    g_clear_pointer(&source, g_free);
    g_clear_pointer(&agent, g_free);

    clawt_event_set_detail(state, "state", "error");
    text = text_of(state, NULL, NULL);
    g_assert_cmpstr(text, ==, "scout is error");
    g_free(text);

    clawt_event_set_detail(tree, "tree", "the release");
    clawt_event_set_detail(tree, "count", "40");
    clawt_event_set_detail(tree, "limit", "40");
    expected_tree = clawt_loop_guard_tree_alert_text("the release", 40, 40);
    text = text_of(tree, NULL, NULL);
    g_assert_cmpstr(text, ==, expected_tree);
    g_free(text);

    clawt_event_set_detail(failover, "state", "fallback");
    clawt_event_set_detail(failover, "message",
                           "scout is on gpt-5 until 14:00: rate limited");
    text = text_of(failover, NULL, NULL);
    g_assert_cmpstr(text, ==, "scout is on gpt-5 until 14:00: rate limited");
    g_free(text);

    text = text_of(timed_out, &source, &agent);
    g_assert_cmpstr(text, ==, "turn.timed_out \302\267 scout");
    g_assert_cmpstr(source, ==, "turn.timed_out");
    g_assert_cmpstr(agent, ==, "scout");
    g_free(text);
    g_clear_pointer(&source, g_free);
    g_clear_pointer(&agent, g_free);

    text = text_of(bare, NULL, &agent);
    g_assert_cmpstr(text, ==, "agent.contested \302\267 the fleet");
    g_assert_null(agent);
    g_free(text);

    /* skipped kinds have no words, and leave the outputs NULL */
    g_assert_null(text_of(typing, &source, &agent));
    g_assert_null(source);
    g_assert_null(agent);
    g_assert_null(text_of(bookkeeping, NULL, NULL));
}

/*
 * A client's bell, from a reply: the four counts added, a missing member
 * counted as 0; and the mutes as a set.  Regressing the first is a bell
 * whose number differs between clients; the second, a muted pill that
 * still counts.
 */
static void
test_a_reply_reads_into_a_badge_and_mutes(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GHashTable) mutes = NULL;
    g_autoptr(GHashTable) none = NULL;
    JsonObject *reply;

    g_assert_true(json_parser_load_from_data(
        parser,
        "{\"sources\":{\"chats_unread\":4,\"decisions_pending\":2,"
        "\"grok_attention\":1},\"unread\":3,"
        "\"mutes\":[{\"agent\":\"zed\",\"until\":0},{\"until\":5}]}",
        -1, NULL));
    reply = json_node_get_object(json_parser_get_root(parser));

    g_assert_cmpuint(clawt_notice_list_badge(reply), ==, 10);
    g_assert_cmpuint(clawt_notice_list_badge(NULL), ==, 0);

    mutes = clawt_notice_list_mutes(reply);
    g_assert_cmpuint(g_hash_table_size(mutes), ==, 1);
    g_assert_true(g_hash_table_contains(mutes, "zed"));

    none = clawt_notice_list_mutes(NULL);
    g_assert_cmpuint(g_hash_table_size(none), ==, 0);

    g_assert_true(json_parser_load_from_data(parser, "{\"unread\":2}", -1,
                                             NULL));
    g_assert_cmpuint(clawt_notice_list_badge(json_node_get_object(
                         json_parser_get_root(parser))), ==, 2);
}

/* ── The daemon around it ─────────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
    GPtrArray    *notified;   /* agent ids the sink was handed */
    guint         changed;    /* notice.changed events seen */
    gulong        handler;
} Fixture;

static void
on_notified(ClawtDaemon *daemon, ClawtNotification *notification,
            gpointer user_data)
{
    Fixture *fixture = user_data;

    (void)daemon;

    g_ptr_array_add(fixture->notified, g_strdup(notification->agent_id));
}

static void
on_event(ClawtEventBus *bus, ClawtEvent *event, gpointer user_data)
{
    Fixture *fixture = user_data;

    (void)bus;

    if (g_strcmp0(clawt_event_get_kind(event), "notice.changed") == 0)
        fixture->changed++;
}

/*
 * Two agents, everything under a private directory -- the socket, the
 * state, the workspaces -- so nothing here touches a fleet somebody is
 * running.  The notifier is replaced by a recorder.
 */
static void
fixture_setup(Fixture *fixture)
{
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-notices-d-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml",
                                            NULL);
    fixture->notified = g_ptr_array_new_with_free_func(g_free);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s/daemon.sock\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "skills:\n  dir: \"%s/skills\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "notices:\n  keep: 3\n"
        "agents:\n  - id: alpha\n  - id: zed\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir,
        fixture->dir);

    g_assert_true(g_file_set_contents(fixture->config_path, yaml, -1, NULL));

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path,
                                       fixture->context);
    g_assert_true(clawt_daemon_start(fixture->daemon, &error));
    g_assert_no_error(error);
    g_assert_nonnull(fixture->daemon->notices);

    fixture->daemon->notify_sink = on_notified;
    fixture->daemon->notify_sink_data = fixture;
    fixture->handler = g_signal_connect(fixture->daemon->bus, "event",
                                        G_CALLBACK(on_event), fixture);
}

static void
fixture_teardown(Fixture *fixture)
{
    if (fixture->daemon != NULL) {
        g_signal_handler_disconnect(fixture->daemon->bus, fixture->handler);
        clawt_daemon_stop(fixture->daemon);
        g_clear_object(&fixture->daemon);
    }

    if (fixture->context != NULL) {
        while (g_main_context_iteration(fixture->context, FALSE))
            ;
    }

    g_clear_pointer(&fixture->context, g_main_context_unref);
    g_clear_pointer(&fixture->notified, g_ptr_array_unref);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
    g_free(fixture->config_path);
}

static JsonNode *
request(Fixture *fixture, const gchar *kind, const gchar *payload_json)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new(kind, "r1");

    if (payload_json != NULL) {
        g_autoptr(JsonParser) parser = json_parser_new();

        g_assert_true(json_parser_load_from_data(parser, payload_json, -1,
                                                 NULL));
        clawt_ipc_frame_set_payload(
            frame, json_node_copy(json_parser_get_root(parser)));
    }

    return clawt_daemon_handle_request(fixture->daemon, frame);
}

/* Publishes an event on the daemon's own bus, as the daemon would. */
static void
publish(Fixture *fixture, const gchar *kind, const gchar *subject,
        const gchar *detail, const gchar *value)
{
    g_autoptr(ClawtEvent) event = clawt_event_new(kind, subject);

    if (detail != NULL)
        clawt_event_set_detail(event, detail, value);

    clawt_event_bus_publish(fixture->daemon->bus, event);
}

static JsonObject *
payload_of(JsonNode *reply)
{
    g_assert_nonnull(reply);
    g_assert_false(clawt_ipc_frame_is_error(reply));

    return clawt_ipc_frame_get_payload(reply);
}

/*
 * An error and a notice each become a row; a routine event and a
 * skipped one do not.  Each recording says so with notice.changed.
 * Regressing the filter fills the list with the routine stream the
 * event log already keeps; regressing the event leaves every client
 * showing the list as it was.
 */
static void
test_loud_events_become_notices(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    JsonArray *notices;
    JsonObject *first;

    fixture_setup(&fixture);
    fixture.changed = 0;

    publish(&fixture, "turn.timed_out", "alpha", NULL, NULL);
    publish(&fixture, "agent.state", "zed", "state", "degraded");
    publish(&fixture, "agent.state", "zed", "state", "running");
    publish(&fixture, "agent.typing", "zed", NULL, NULL);
    g_assert_cmpuint(fixture.changed, ==, 2);

    reply = request(&fixture, "notice.list", NULL);
    notices = json_object_get_array_member(payload_of(reply), "notices");
    g_assert_cmpuint(json_array_get_length(notices), ==, 2);

    first = json_array_get_object_element(notices, 0);
    g_assert_cmpstr(json_object_get_string_member(first, "title"), ==,
                    "zed is degraded");
    g_assert_cmpstr(json_object_get_string_member(first, "tier"), ==,
                    "notice");
    g_assert_false(json_object_get_boolean_member(first, "read"));
    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "unread"),
                    ==, 2);

    fixture_teardown(&fixture);
}

/*
 * A muted agent's notice is still recorded, already read -- the list is
 * a record and the mute is about interruption.  A timed mute lapses at
 * `until` on the daemon's clock, after which the agent's notices count
 * again.  Regressing it is a mute that hides history, or never ends.
 */
static void
test_a_muted_agent_arrives_read(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) muted = NULL;
    g_autoptr(JsonNode) refused = NULL;
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonNode) later = NULL;
    g_autoptr(ClawtEvent) event = NULL;
    g_autoptr(ClawtNotice) recorded = NULL;
    gint64 id;

    fixture_setup(&fixture);
    fixture.daemon->notices_clock = 1000000;

    muted = request(&fixture, "notice.mute",
                    "{\"agent\":\"zed\",\"until\":\"1003600\"}");
    g_assert_true(json_object_get_boolean_member(payload_of(muted),
                                                 "muted"));

    /* an agent the fleet does not have, and a time already gone */
    refused = request(&fixture, "notice.mute", "{\"agent\":\"nobody\"}");
    g_assert_true(clawt_ipc_frame_is_error(refused));
    g_clear_pointer(&refused, json_node_unref);
    refused = request(&fixture, "notice.mute",
                      "{\"agent\":\"zed\",\"until\":5}");
    g_assert_true(clawt_ipc_frame_is_error(refused));

    publish(&fixture, "turn.timed_out", "zed", NULL, NULL);
    reply = request(&fixture, "notice.list", NULL);
    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "unread"),
                    ==, 0);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
                         payload_of(reply), "notices")), ==, 1);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
                         payload_of(reply), "mutes")), ==, 1);

    /* an hour on, the mute has lapsed and the next one counts */
    event = clawt_event_new("turn.timed_out", "zed");
    id = clawt_daemon_notices_record(fixture.daemon, event, 1003600);
    recorded = clawt_notice_store_get(fixture.daemon->notices, id);
    g_assert_nonnull(recorded);
    g_assert_false(recorded->read);

    fixture.daemon->notices_clock = 1003600;
    later = request(&fixture, "notice.list", NULL);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
                         payload_of(later), "mutes")), ==, 0);
    g_assert_cmpint(json_object_get_int_member(payload_of(later), "unread"),
                    ==, 1);

    fixture_teardown(&fixture);
}

/*
 * The verbs over IPC: read one, read all, dismiss, clear per agent and
 * all, each publishing notice.changed, and the refusals for a missing
 * id.  Regressing these is a panel whose buttons do nothing in the
 * other clients.
 */
static void
test_the_verbs(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) reply = NULL;
    g_autoptr(JsonNode) list = NULL;
    g_autofree gchar *one = NULL;
    JsonArray *notices;
    gint64 id;

    fixture_setup(&fixture);

    publish(&fixture, "turn.timed_out", "alpha", NULL, NULL);
    publish(&fixture, "turn.timed_out", "zed", NULL, NULL);
    publish(&fixture, "exchange.stalled", "zed", NULL, NULL);

    list = request(&fixture, "notice.list", "{\"agent\":\"alpha\"}");
    notices = json_object_get_array_member(payload_of(list), "notices");
    g_assert_cmpuint(json_array_get_length(notices), ==, 1);
    id = json_object_get_int_member(json_array_get_object_element(notices, 0),
                                    "id");

    fixture.changed = 0;

    /* one read, as a string id -- the GTK and lrg clients send strings */
    one = g_strdup_printf("{\"id\":\"%" G_GINT64_FORMAT "\"}", id);
    reply = request(&fixture, "notice.read", one);
    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "read"),
                    ==, 1);
    g_clear_pointer(&reply, json_node_unref);
    g_assert_cmpuint(fixture.changed, ==, 1);

    reply = request(&fixture, "notice.dismiss", one);
    payload_of(reply);
    g_clear_pointer(&reply, json_node_unref);
    reply = request(&fixture, "notice.dismiss", one);
    g_assert_true(clawt_ipc_frame_is_error(reply));
    g_clear_pointer(&reply, json_node_unref);
    reply = request(&fixture, "notice.read", "{}");
    g_assert_true(clawt_ipc_frame_is_error(reply));
    g_clear_pointer(&reply, json_node_unref);

    reply = request(&fixture, "notice.read", "{\"all\":true}");
    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "read"),
                    ==, 2);
    g_clear_pointer(&reply, json_node_unref);

    reply = request(&fixture, "notice.clear", "{\"agent\":\"zed\"}");
    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "cleared"),
                    ==, 2);
    g_clear_pointer(&reply, json_node_unref);

    g_clear_pointer(&list, json_node_unref);
    list = request(&fixture, "notice.list", "{\"include_dismissed\":true}");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
                         payload_of(list), "notices")), ==, 3);

    reply = request(&fixture, "notice.clear", NULL);
    g_assert_cmpint(json_object_get_int_member(payload_of(reply), "cleared"),
                    ==, 0);

    g_assert_cmpuint(fixture.changed, ==, 4);

    fixture_teardown(&fixture);
}

/*
 * notices.keep from the config, on the prune the hourly tick runs.  The
 * fixture keeps 3.
 */
static void
test_the_daemon_prunes_to_the_config(void)
{
    Fixture fixture = { 0 };
    gint i;

    fixture_setup(&fixture);

    for (i = 0; i < 5; i++)
        publish(&fixture, "turn.timed_out", "alpha", NULL, NULL);

    g_assert_cmpuint(clawt_notice_store_count(fixture.daemon->notices), ==,
                     5);
    clawt_daemon_notices_prune(fixture.daemon,
                               clawt_daemon_notices_now(fixture.daemon));
    g_assert_cmpuint(clawt_notice_store_count(fixture.daemon->notices), ==,
                     3);

    fixture_teardown(&fixture);
}

/* A message in a room, through the transcript, as a turn would leave it. */
static void
say(Fixture *fixture, const gchar *agent)
{
    ClawtRoom *room = clawt_room_manager_get_direct(fixture->daemon->rooms,
                                                    "user", agent);
    g_autoptr(ClawtMessage) message = NULL;

    g_assert_nonnull(room);

    message = clawt_message_new(clawt_room_get_id(room), agent, "hello");
    g_assert_true(clawt_room_append(room, message, NULL));
}

static void
ask(Fixture *fixture, const gchar *agent)
{
    g_autoptr(ClawtDecision) decision = clawt_decision_new(NULL, agent,
                                                           "which one?");
    g_autofree gchar *id = NULL;

    id = clawt_decision_store_post(fixture->daemon->decisions, decision,
                                   NULL);
    g_assert_nonnull(id);
}

/*
 * The `sources` summary leaves a muted agent out of every count -- its
 * unread conversation and its waiting decision -- and the total is the
 * bell.  Regressing it is a muted agent still lighting every badge.
 */
static void
test_sources_leave_a_muted_agent_out(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) before = NULL;
    g_autoptr(JsonNode) muted = NULL;
    g_autoptr(JsonNode) after = NULL;
    JsonObject *sources;

    fixture_setup(&fixture);
    g_assert_nonnull(fixture.daemon->decisions);

    say(&fixture, "alpha");
    say(&fixture, "zed");
    say(&fixture, "zed");
    ask(&fixture, "zed");

    before = request(&fixture, "notice.list", NULL);
    sources = json_object_get_object_member(payload_of(before), "sources");
    g_assert_cmpint(json_object_get_int_member(sources, "chats_unread"), ==,
                    3);
    g_assert_cmpint(json_object_get_int_member(sources, "decisions_pending"),
                    ==, 1);
    g_assert_cmpuint(clawt_notice_list_badge(payload_of(before)), ==, 4);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(
                         payload_of(before), "chats")), ==, 2);

    muted = request(&fixture, "notice.mute", "{\"agent\":\"zed\"}");
    payload_of(muted);

    after = request(&fixture, "notice.list", NULL);
    sources = json_object_get_object_member(payload_of(after), "sources");
    g_assert_cmpint(json_object_get_int_member(sources, "chats_unread"), ==,
                    1);
    g_assert_cmpint(json_object_get_int_member(sources, "decisions_pending"),
                    ==, 0);
    g_assert_cmpint(json_object_get_int_member(sources, "grok_attention"),
                    ==, 0);
    g_assert_cmpint(json_object_get_int_member(payload_of(after), "total"),
                    ==, 1);

    fixture_teardown(&fixture);
}

/*
 * clawt_daemon_notify() is the one door to the notifiers, and it stays
 * shut for a muted agent -- here, the recorder never hears of zed while
 * alpha goes through; and once unmuted, zed does too.  Regressing it is
 * a phone buzzing for an agent every client shows as quiet.
 */
static void
test_notify_skips_a_muted_agent(void)
{
    Fixture fixture = { 0 };
    g_autoptr(JsonNode) muted = NULL;
    g_autoptr(JsonNode) unmuted = NULL;
    g_autoptr(ClawtNotification) for_zed = NULL;
    g_autoptr(ClawtNotification) for_alpha = NULL;

    fixture_setup(&fixture);

    for_zed = clawt_notification_new(CLAWT_NOTIFY_EVENTS_ERROR, "zed", NULL,
                                     "stopped with an error", NULL);
    for_alpha = clawt_notification_new(CLAWT_NOTIFY_EVENTS_ERROR, "alpha",
                                       NULL, "stopped with an error", NULL);

    muted = request(&fixture, "notice.mute", "{\"agent\":\"zed\"}");
    payload_of(muted);

    clawt_daemon_notify(fixture.daemon, for_zed);
    clawt_daemon_notify(fixture.daemon, for_alpha);
    g_assert_cmpuint(fixture.notified->len, ==, 1);
    g_assert_cmpstr(g_ptr_array_index(fixture.notified, 0), ==, "alpha");

    unmuted = request(&fixture, "notice.unmute", "{\"agent\":\"zed\"}");
    g_assert_false(json_object_get_boolean_member(payload_of(unmuted),
                                                  "muted"));

    clawt_daemon_notify(fixture.daemon, for_zed);
    g_assert_cmpuint(fixture.notified->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(fixture.notified, 1), ==, "zed");

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/notices/store/round-trip", test_a_notice_round_trips);
    g_test_add_func("/notices/store/read-dismiss-clear",
                    test_read_dismiss_and_clear);
    g_test_add_func("/notices/store/prune", test_pruning_by_count_and_by_age);
    g_test_add_func("/notices/store/timed-mute-lapses",
                    test_a_timed_mute_lapses);
    g_test_add_func("/notices/mute-length", test_the_mute_length_parses);
    g_test_add_func("/notices/alert-text-golden",
                    test_the_alert_text_is_gtks);
    g_test_add_func("/notices/reply-badge-and-mutes",
                    test_a_reply_reads_into_a_badge_and_mutes);
    g_test_add_func("/notices/daemon/loud-events",
                    test_loud_events_become_notices);
    g_test_add_func("/notices/daemon/muted-arrives-read",
                    test_a_muted_agent_arrives_read);
    g_test_add_func("/notices/daemon/verbs", test_the_verbs);
    g_test_add_func("/notices/daemon/prune-from-config",
                    test_the_daemon_prunes_to_the_config);
    g_test_add_func("/notices/daemon/sources-leave-muted-out",
                    test_sources_leave_a_muted_agent_out);
    g_test_add_func("/notices/daemon/notify-skips-muted",
                    test_notify_skips_a_muted_agent);

    return g_test_run();
}
