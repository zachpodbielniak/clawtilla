/*
 * test-office-feed.c - A profile picture fetched for the drawn office
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The feed is included, the way the web tests include a page: it is not
 * in libclawt.  A daemon answers agent.list and agent.avatar.  The look
 * test always runs.  The pixel test skips, and says why, only when this
 * build cannot decode a PNG.
 */

#include "clawtilla.h"

#include <glib/gstdio.h>
#include <stddef.h>
#include <string.h>

#include "clawt-test-util.h"

/*
 * The stale-list test holds agent.list replies and delivers a newer one
 * before an older one.  Every other request goes straight through.  The
 * macro covers only the feed, which is included below; the hook itself
 * calls the real function through the pointer saved first.
 */
static gboolean hold_agent_lists = FALSE;
static GQueue held_agent_lists = G_QUEUE_INIT;

typedef struct {
    GObject             *source;
    GAsyncResult        *result;
    GAsyncReadyCallback  callback;
    gpointer             user_data;
} HeldAgentList;

static void (*clawt_client_request_async_real)(ClawtClient         *self,
                                               const gchar         *kind,
                                               JsonNode            *payload,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data) =
    clawt_client_request_async;

static void
hold_agent_list_reply(GObject *source, GAsyncResult *result, gpointer user_data)
{
    HeldAgentList *held = user_data;

    held->source = g_object_ref(source);
    held->result = G_ASYNC_RESULT(g_object_ref(G_OBJECT(result)));
    g_queue_push_tail(&held_agent_lists, held);
}

static void
clawt_client_request_async_hook(ClawtClient         *self,
                                const gchar         *kind,
                                JsonNode            *payload,
                                GCancellable        *cancellable,
                                GAsyncReadyCallback  callback,
                                gpointer             user_data)
{
    if (hold_agent_lists && g_strcmp0(kind, "agent.list") == 0) {
        HeldAgentList *held = g_new0(HeldAgentList, 1);

        held->callback = callback;
        held->user_data = user_data;
        clawt_client_request_async_real(self, kind, payload, cancellable,
                                        hold_agent_list_reply, held);
        return;
    }
    clawt_client_request_async_real(self, kind, payload, cancellable, callback,
                                    user_data);
}

#define clawt_client_request_async clawt_client_request_async_hook
#include "../clients/office/office-feed.c"
#undef clawt_client_request_async

/* 4x4 PNGs.  Red is RGB 255,0,17; green is RGB 0,255,17. */
static const guint8 RED_PNG[] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,4,0,0,0,4,8,2,0,0,0,38,147,9,41,
    0,0,0,16,73,68,65,84,120,218,99,248,207,32,8,71,12,196,113,0,201,35,17,1,6,239,147,43,
    0,0,0,0,73,69,78,68,174,66,96,130
};
static const guint8 GREEN_PNG[] = {
    137,80,78,71,13,10,26,10,0,0,0,13,73,72,68,82,0,0,0,4,0,0,0,4,8,2,0,0,0,38,147,9,41,
    0,0,0,15,73,68,65,84,120,218,99,96,248,47,136,64,196,113,0,185,51,17,1,249,252,149,146,
    0,0,0,0,73,69,78,68,174,66,96,130
};

typedef struct {
    gchar        *dir;
    gchar        *socket_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
    ClawtClient  *client;
    ClawtOfficeFeed *feed;
    gint          looks;
    gboolean      has_avatar;
    gint          portraits;
    gboolean      got_rgba;
    gchar        *etag;
    gint          size;
    guint8        center[4];
} FeedFixture;

static void
release_held_agent_list(HeldAgentList *held)
{
    held->callback(held->source, held->result, held->user_data);
    g_object_unref(held->result);
    g_object_unref(held->source);
    g_free(held);
}

static void
release_queued_agent_lists(void)
{
    HeldAgentList *held;

    while ((held = g_queue_pop_head(&held_agent_lists)) != NULL)
        release_held_agent_list(held);
}

static gboolean
wait_held_agent_lists(FeedFixture *fixture, guint want, gint timeout_ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

    while (g_get_monotonic_time() < end) {
        while (g_main_context_pending(fixture->context))
            g_main_context_iteration(fixture->context, FALSE);
        if (g_queue_get_length(&held_agent_lists) >= want)
            return TRUE;
        g_usleep(2000);
    }
    return g_queue_get_length(&held_agent_lists) >= want;
}

static void
on_event(ClawtClient *client, ClawtEvent *event, gpointer user_data)
{
    (void)client;
    clawt_office_feed_event(user_data, event);
}

static void
on_look(const gchar *id, const gchar *name, const gchar *face, gboolean has_avatar,
        gpointer user_data)
{
    FeedFixture *fixture = user_data;

    (void)name;
    (void)face;
    if (g_strcmp0(id, "ada") != 0)
        return;
    fixture->has_avatar = has_avatar;
    fixture->looks++;
}

static void
on_portrait(const gchar *id, const guint8 *rgba, gint size, const gchar *etag,
            gpointer user_data)
{
    FeedFixture *fixture = user_data;
    gint at;

    if (g_strcmp0(id, "ada") != 0)
        return;
    g_free(fixture->etag);
    fixture->etag = g_strdup(etag);
    fixture->size = size;
    fixture->portraits++;
    fixture->got_rgba = rgba != NULL && size > 0;
    if (!fixture->got_rgba)
        return;
    at = (size / 2) * size + (size / 2);
    memcpy(fixture->center, rgba + (gsize)at * 4, 4);
}

static void
pump(FeedFixture *fixture, gint timeout_ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

    while (g_get_monotonic_time() < end) {
        while (g_main_context_pending(fixture->context))
            g_main_context_iteration(fixture->context, FALSE);
        g_usleep(2000);
    }
}

static gboolean
wait_looks(FeedFixture *fixture, gint want, gint timeout_ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

    while (g_get_monotonic_time() < end) {
        while (g_main_context_pending(fixture->context))
            g_main_context_iteration(fixture->context, FALSE);
        if (fixture->looks >= want)
            return TRUE;
        g_usleep(2000);
    }
    return fixture->looks >= want;
}

static void
fixture_setup(FeedFixture *fixture)
{
    g_autofree gchar *config_path = NULL;
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;
    ClawtOfficeFeedFuncs funcs;

    fixture->dir = g_dir_make_tmp("clawt-office-feed-XXXXXX", NULL);
    g_assert_nonnull(fixture->dir);
    config_path = g_build_filename(fixture->dir, "config.yaml", NULL);
    fixture->socket_path = g_build_filename(fixture->dir, "d.sock", NULL);
    yaml = g_strdup_printf(
        "daemon:\n"
        "  tailscale: false\n"
        "  state_dir: \"%s/state\"\n"
        "  socket: \"%s\"\n"
        "  automation_dir: \"%s/pods\"\n"
        "defaults:\n"
        "  workspace_root: \"%s/agents\"\n"
        "  autostart: false\n"
        "office:\n"
        "  enabled: true\n"
        "  world: modern-office\n"
        "  world_dirs: [\"%s/data/office/worlds\"]\n"
        "agents:\n"
        "  - id: ada\n"
        "    name: Ada\n"
        "    color: \"#7c5cff\"\n"
        "  - id: bea\n"
        "    name: Bea\n",
        fixture->dir, fixture->socket_path, fixture->dir, fixture->dir,
        CLAWT_TEST_SRCDIR);
    g_assert_true(g_file_set_contents(config_path, yaml, -1, &error));
    g_assert_no_error(error);

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(config_path, fixture->context);
    clawt_daemon_start(fixture->daemon, &error);
    g_assert_no_error(error);

    g_main_context_push_thread_default(fixture->context);
    fixture->client = clawt_client_new(fixture->socket_path);
    clawt_client_set_auto_reconnect(fixture->client, FALSE);
    g_assert_true(clawt_client_connect(fixture->client, &error));
    g_assert_no_error(error);
    g_assert_true(clawt_client_subscribe(fixture->client, 0, NULL, &error));
    g_assert_no_error(error);

    memset(&funcs, 0, sizeof(funcs));
    funcs.look = on_look;
    funcs.portrait = on_portrait;
    fixture->feed = clawt_office_feed_new(fixture->client, fixture->context, NULL, &funcs,
                                          fixture);
    g_signal_connect(fixture->client, "event", G_CALLBACK(on_event), fixture->feed);
}

static void
fixture_teardown(FeedFixture *fixture)
{
    clawt_office_feed_free(fixture->feed);
    fixture->feed = NULL;
    g_clear_object(&fixture->client);
    g_main_context_pop_thread_default(fixture->context);
    clawt_daemon_stop(fixture->daemon);
    g_clear_object(&fixture->daemon);
    while (g_main_context_iteration(fixture->context, FALSE))
        ;
    g_clear_pointer(&fixture->context, g_main_context_unref);
    clawt_test_remove_tree(fixture->dir);
    g_clear_pointer(&fixture->dir, g_free);
    g_clear_pointer(&fixture->socket_path, g_free);
    g_clear_pointer(&fixture->etag, g_free);
}

static void
set_avatar(FeedFixture *fixture, const guint8 *png, gsize length)
{
    g_autofree gchar *encoded = g_base64_encode(png, length);
    g_autofree gchar *text = g_strdup_printf("{\"agent\":\"ada\",\"data\":\"%s\"}", encoded);
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) reply = NULL;

    g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
    g_assert_no_error(error);
    reply = clawt_client_request(fixture->client, "agent.avatar_set",
                                 json_node_copy(json_parser_get_root(parser)), &error);
    g_assert_no_error(error);
    g_assert_nonnull(reply);
}

/* agent.changed after a picture is stored flips has_avatar.  No decode. */
static void
test_change_flips_has_avatar(void)
{
    FeedFixture fixture = { 0 };

    fixture_setup(&fixture);
    g_assert_true(wait_looks(&fixture, 1, 10000));
    g_assert_false(fixture.has_avatar);

    set_avatar(&fixture, RED_PNG, sizeof(RED_PNG));
    pump(&fixture, 3000);
    g_assert_cmpint(fixture.looks, >=, 2);
    g_assert_true(fixture.has_avatar);

    fixture_teardown(&fixture);
}

/*
 * A second picture, announced by agent.changed, replaces the cached
 * one: a new etag, and the centre pixel of the new PNG.
 */
static void
test_change_refreshes_picture(void)
{
    FeedFixture fixture = { 0 };
    g_autofree gchar *first_etag = NULL;
    gint first_portraits;
    gint64 end;

    if (!clawt_office_portrait_decode_available()) {
        g_test_skip("cairo PNG decode is not in this build (POPPLER=0); "
                    "gdk-pixbuf is not used because glycin's bwrap sandbox "
                    "does not run in these containers");
        return;
    }

    fixture_setup(&fixture);
    /* the opening list, so the picture is a change rather than the first look */
    g_assert_true(wait_looks(&fixture, 1, 10000));
    set_avatar(&fixture, RED_PNG, sizeof(RED_PNG));
    end = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end && !fixture.got_rgba) {
        while (g_main_context_pending(fixture.context))
            g_main_context_iteration(fixture.context, FALSE);
        g_usleep(2000);
    }
    g_assert_true(fixture.got_rgba);
    g_assert_cmpint(fixture.center[0], ==, 255);
    g_assert_cmpint(fixture.center[1], ==, 0);
    g_assert_cmpint(fixture.center[2], ==, 17);
    g_assert_cmpint(fixture.center[3], ==, 255);
    first_etag = g_strdup(fixture.etag);
    first_portraits = fixture.portraits;
    g_assert_nonnull(first_etag);

    set_avatar(&fixture, GREEN_PNG, sizeof(GREEN_PNG));
    end = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end &&
           (fixture.portraits <= first_portraits ||
            g_strcmp0(fixture.etag, first_etag) == 0)) {
        while (g_main_context_pending(fixture.context))
            g_main_context_iteration(fixture.context, FALSE);
        g_usleep(2000);
    }
    g_assert_cmpint(fixture.portraits, >, first_portraits);
    g_assert_cmpstr(fixture.etag, !=, first_etag);
    g_assert_cmpint(fixture.center[0], ==, 0);
    g_assert_cmpint(fixture.center[1], ==, 255);
    g_assert_cmpint(fixture.center[2], ==, 17);
    g_assert_cmpint(fixture.center[3], ==, 255);

    fixture_teardown(&fixture);
}

/*
 * Two agent.list replies, delivered newest first.  The older one still
 * says the agent has no picture.  It must not clear the picture the
 * newer one stored, and it must not flip has_avatar back.
 */
static void
test_stale_list_keeps_picture(void)
{
    FeedFixture fixture = { 0 };
    HeldAgentList *stale = NULL;
    HeldAgentList *later = NULL;
    gint portraits;
    gint64 end;

    if (!clawt_office_portrait_decode_available()) {
        g_test_skip("cairo PNG decode is not in this build (POPPLER=0); "
                    "gdk-pixbuf is not used because glycin's bwrap sandbox "
                    "does not run in these containers");
        return;
    }

    hold_agent_lists = TRUE;
    fixture_setup(&fixture);
    g_assert_true(wait_held_agent_lists(&fixture, 1, 10000));

    set_avatar(&fixture, RED_PNG, sizeof(RED_PNG));
    g_assert_true(wait_held_agent_lists(&fixture, 2, 10000));

    /* head is the list from connect: it was queued before the picture existed */
    stale = g_queue_pop_head(&held_agent_lists);
    g_assert_nonnull(stale);
    while ((later = g_queue_pop_head(&held_agent_lists)) != NULL)
        release_held_agent_list(later);

    end = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end && !fixture.got_rgba) {
        while ((later = g_queue_pop_head(&held_agent_lists)) != NULL)
            release_held_agent_list(later);
        while (g_main_context_pending(fixture.context))
            g_main_context_iteration(fixture.context, FALSE);
        g_usleep(2000);
    }
    g_assert_true(fixture.got_rgba);
    g_assert_cmpint(fixture.center[0], ==, 255);
    g_assert_cmpint(fixture.center[1], ==, 0);
    g_assert_cmpint(fixture.center[2], ==, 17);
    g_assert_cmpint(fixture.center[3], ==, 255);
    g_assert_true(fixture.has_avatar);
    portraits = fixture.portraits;

    release_held_agent_list(stale);
    pump(&fixture, 1000);

    g_assert_true(fixture.got_rgba);
    g_assert_true(fixture.has_avatar);
    g_assert_cmpint(fixture.portraits, ==, portraits);
    g_assert_cmpint(fixture.center[0], ==, 255);
    g_assert_cmpint(fixture.center[1], ==, 0);
    g_assert_cmpint(fixture.center[2], ==, 17);
    g_assert_cmpint(fixture.center[3], ==, 255);

    release_queued_agent_lists();
    hold_agent_lists = FALSE;
    fixture_teardown(&fixture);
}

/*
 * free() cancels, then releases the feed.  A reply still queued runs
 * after that.  The block the feed occupied is taken back and filled
 * with 0xA5.  A callback that still writes portraits_inflight (the
 * avatar reply does, before it looks at the cancellable) changes the
 * fill; one that still reads the feed faults on the fill.  After the
 * fix the fill is untouched and the context drains to idle.
 */
static gpointer
reclaim_feed(ClawtOfficeFeed *dangling)
{
    gpointer slots[32];
    guint n = 0;
    guint i;
    gpointer found = NULL;

    for (i = 0; i < 32 && found == NULL; i++) {
        slots[n] = g_malloc(sizeof(struct _ClawtOfficeFeed));
        if (slots[n] == (gpointer)dangling)
            found = slots[n];
        n++;
    }
    g_assert_nonnull(found);
    memset(found, 0xA5, sizeof(struct _ClawtOfficeFeed));
    for (i = 0; i < n; i++) {
        if (slots[i] != found)
            g_free(slots[i]);
    }
    return found;
}

static void
drain_idle(FeedFixture *fixture)
{
    gint64 end = g_get_monotonic_time() + 2 * G_USEC_PER_SEC;

    while (g_get_monotonic_time() < end) {
        while (g_main_context_pending(fixture->context))
            g_main_context_iteration(fixture->context, FALSE);
        g_usleep(2000);
    }
}

static void
free_feed_poisoned(FeedFixture *fixture, gpointer *block)
{
    ClawtOfficeFeed *dangling = fixture->feed;

    g_signal_handlers_disconnect_by_data(fixture->client, dangling);
    clawt_office_feed_free(dangling);
    fixture->feed = NULL;
    *block = reclaim_feed(dangling);
}

static gboolean
drive_until(FeedFixture *fixture, gboolean (*ready)(FeedFixture *fixture), gint timeout_ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)timeout_ms * 1000;

    while (g_get_monotonic_time() < end) {
        if (ready(fixture))
            return TRUE;
        if (!g_main_context_iteration(fixture->context, FALSE))
            g_usleep(1000);
    }
    return ready(fixture);
}

static gboolean
avatar_request_inflight(FeedFixture *fixture)
{
    return fixture->feed->portraits_inflight > 0;
}

static gboolean
decode_inflight(FeedFixture *fixture)
{
    Portrait *portrait = g_hash_table_lookup(fixture->feed->portraits, "ada");

    return portrait != NULL && portrait->inflight && fixture->feed->portraits_inflight == 0;
}

static void
assert_poison_held(gpointer block, gint portraits_before, FeedFixture *fixture)
{
    guint *counter = (guint *)((guint8 *)block +
                               offsetof(struct _ClawtOfficeFeed, portraits_inflight));

    g_assert_cmpuint(*counter, ==, 0xA5A5A5A5u);
    g_assert_cmpint(fixture->portraits, ==, portraits_before);
    g_free(block);
}

/* The agent.avatar reply is queued and the feed is already gone. */
static void
test_free_during_avatar(void)
{
    FeedFixture fixture = { 0 };
    gpointer block = NULL;
    gint portraits_before;

    fixture_setup(&fixture);
    g_assert_true(wait_looks(&fixture, 1, 10000));
    set_avatar(&fixture, RED_PNG, sizeof(RED_PNG));
    g_assert_true(drive_until(&fixture, avatar_request_inflight, 10000));
    portraits_before = fixture.portraits;
    free_feed_poisoned(&fixture, &block);
    drain_idle(&fixture);
    assert_poison_held(block, portraits_before, &fixture);
    fixture_teardown(&fixture);
}

/* The PNG is in the worker.  Its callback is what would touch the feed. */
static void
test_free_during_decode(void)
{
    FeedFixture fixture = { 0 };
    gpointer block = NULL;
    gint portraits_before;

    if (!clawt_office_portrait_decode_available()) {
        g_test_skip("cairo PNG decode is not in this build (POPPLER=0); "
                    "gdk-pixbuf is not used because glycin's bwrap sandbox "
                    "does not run in these containers");
        return;
    }

    fixture_setup(&fixture);
    g_assert_true(wait_looks(&fixture, 1, 10000));
    set_avatar(&fixture, RED_PNG, sizeof(RED_PNG));
    g_assert_true(drive_until(&fixture, decode_inflight, 10000));
    portraits_before = fixture.portraits;
    free_feed_poisoned(&fixture, &block);
    drain_idle(&fixture);
    assert_poison_held(block, portraits_before, &fixture);
    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/office-feed/change-flips-has-avatar", test_change_flips_has_avatar);
    g_test_add_func("/office-feed/change-refreshes-picture", test_change_refreshes_picture);
    g_test_add_func("/office-feed/stale-list-keeps-picture", test_stale_list_keeps_picture);
    g_test_add_func("/office-feed/free-during-avatar", test_free_during_avatar);
    g_test_add_func("/office-feed/free-during-decode", test_free_during_decode);

    return g_test_run();
}
