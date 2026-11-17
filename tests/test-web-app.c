/*
 * test-web-app.c - the web client's state between requests
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * web-app.c is included rather than linked, as test-web-render includes
 * the renderers: the web client is a binary, not a library.  Its daemon
 * client is never connected; what is checked is the state the pages are
 * drawn from -- which rooms the sidebar draws and what is unread.
 *
 * Every critical is fatal here, because the bug this was written for
 * showed only as a GLib-CRITICAL in a log nobody read.
 */

#include "clawtilla.h"

#include <glib.h>
#include <string.h>

#include "../clients/web/web-app.c"

/* The agents and rooms a fleet answer carries, as note_fleet() reads them. */
static gchar *scout_room;
static gchar *lead_room;

static JsonArray *
agents_with_rooms(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *text = g_strdup_printf(
        "[{\"id\":\"scout\",\"dm_room\":\"%s\"},"
        " {\"id\":\"lead\",\"dm_room\":\"%s\"}]", scout_room, lead_room);
    JsonArray *agents;

    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    agents = json_node_get_array(json_parser_get_root(parser));
    return json_array_ref(agents);
}

static ClawtWebApp *
new_app(void)
{
    g_autoptr(ClawtClient) client =
        clawt_client_new("/nonexistent/clawt-test.sock");

    return clawt_web_app_new(client);
}

/* An agent's message, as the daemon publishes it. */
static void
deliver(ClawtWebApp *app, const gchar *room, const gchar *from)
{
    g_autoptr(ClawtEvent) event = clawt_event_new("message", room);

    clawt_event_set_detail(event, "from", from);
    clawt_event_set_timestamp(event, g_get_real_time() + 1000);
    on_daemon_event(app->client, event, app);
}

/*
 * Opening one agent's chat, then drawing the sidebar again, then a
 * message from another agent: the other agent's row counts it.
 *
 * What broke: clawt_web_app_set_viewing() freed the set of rooms the
 * sidebar draws and nothing made it again.  From the first page anybody
 * opened, every sidebar render was a GLib-CRITICAL, and no message was
 * ever counted -- the rule only counts a room it can see a row for, and
 * there were no rows.  The web client's unread badges never lit.
 */
static void
test_viewing_keeps_the_sidebar_and_counts(void)
{
    g_autoptr(ClawtWebApp) app = new_app();
    g_autoptr(JsonArray) agents = agents_with_rooms();

    clawt_web_app_note_fleet(app, agents, NULL);
    clawt_web_app_set_viewing(app, "lead");

    /* a render after the click, as every page does */
    clawt_web_app_note_fleet(app, agents, NULL);

    deliver(app, scout_room, "scout");
    deliver(app, scout_room, "scout");
    g_assert_cmpuint(clawt_web_app_unread(app, scout_room), ==, 2);

    /* the room on screen does not count its own arrivals */
    deliver(app, lead_room, "lead");
    g_assert_cmpuint(clawt_web_app_unread(app, lead_room), ==, 0);

    /* opening it clears it, and drawing the sidebar keeps the rest */
    clawt_web_app_set_viewing(app, "scout");
    clawt_web_app_note_fleet(app, agents, NULL);
    g_assert_cmpuint(clawt_web_app_unread(app, scout_room), ==, 0);
    deliver(app, lead_room, "lead");
    g_assert_cmpuint(clawt_web_app_unread(app, lead_room), ==, 1);
    g_assert_cmpuint(clawt_web_app_unread_total(app), ==, 1);
}

/* A room with no row -- peer traffic between agents -- is never counted. */
static void
test_a_room_with_no_row_is_not_counted(void)
{
    g_autoptr(ClawtWebApp) app = new_app();
    g_autoptr(JsonArray) agents = agents_with_rooms();

    clawt_web_app_note_fleet(app, agents, NULL);
    clawt_web_app_set_viewing(app, "lead");
    deliver(app, "peer:lead:scout", "scout");
    g_assert_cmpuint(clawt_web_app_unread_total(app), ==, 0);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    /* The rooms as the daemon names them, so "on screen" matches. */
    scout_room = clawt_room_manager_direct_id("user", "scout");
    lead_room = clawt_room_manager_direct_id("user", "lead");
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING);

    g_test_add_func("/web-app/viewing-keeps-the-sidebar-and-counts",
                    test_viewing_keeps_the_sidebar_and_counts);
    g_test_add_func("/web-app/a-room-with-no-row-is-not-counted",
                    test_a_room_with_no_row_is_not_counted);

    return g_test_run();
}
