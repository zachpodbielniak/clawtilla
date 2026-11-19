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
#include "../clients/web/web-files.c"

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

/* ── Sending files with a message ────────────────────────────────── */

/*
 * A composer post as a browser sends it: the text, two files, and the
 * empty part a file input always contributes for the slot nobody used.
 */
static GBytes *
composer_post(void)
{
    static const gchar post[] =
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"body\"\r\n\r\n"
        "  have a look  \r\n"
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file\"; "
        "filename=\"shot.png\"\r\n"
        "Content-Type: image/png\r\n\r\n"
        "\211PNG\r\n"
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file\"; "
        "filename=\"report.pdf\"\r\n"
        "Content-Type: application/pdf\r\n\r\n"
        "%PDF-1.4\r\n"
        "--XyZ\r\n"
        "Content-Disposition: form-data; name=\"file\"; filename=\"\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n"
        "\r\n"
        "--XyZ--\r\n";

    return g_bytes_new_static(post, sizeof(post) - 1);
}

/* What the fake attachment.put was asked, and what it refuses. */
typedef struct {
    GPtrArray   *agents;
    GPtrArray   *names;
    const gchar *refuse;
} FakePut;

static JsonNode *
fake_put(const gchar *agent_id, const gchar *name, const gchar *encoded,
         gpointer user_data)
{
    FakePut *fake = user_data;
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autofree gchar *host = NULL;
    g_autofree gchar *id = NULL;

    g_assert_nonnull(encoded);
    g_ptr_array_add(fake->agents, g_strdup(agent_id));
    g_ptr_array_add(fake->names, g_strdup(name));

    if (g_strcmp0(name, fake->refuse) == 0)
        return NULL;

    host = g_strdup_printf("/state/exchange/%s/%s", agent_id, name);
    id = g_strdup_printf("0id-%s", name);

    /* attachment.put's reply, for a container agent */
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "name");
    json_builder_add_string_value(builder, name);
    json_builder_set_member_name(builder, "host_path");
    json_builder_add_string_value(builder, host);
    json_builder_set_member_name(builder, "path");
    json_builder_add_string_value(builder, "/exchange/in-box");
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, id);
    json_builder_end_object(builder);

    return json_builder_get_root(builder);
}

/*
 * The text and both files from one multipart post, the empty slot
 * dropped.
 *
 * What breaks without it: the composer's files are read from a body
 * the urlencoded form reader never sees, so the message would go with
 * no text -- or, without the empty-part rule, every plain message would
 * carry an unnamed "(could not be saved)" line.
 */
static void
test_a_composer_post_is_read(void)
{
    g_autoptr(GBytes) raw = composer_post();
    g_autoptr(GPtrArray) files = NULL;
    g_autofree gchar *body = NULL;
    g_autoptr(GError) error = NULL;

    files = clawt_web_composer_read("multipart/form-data; boundary=XyZ",
                                    raw, &body, &error);
    g_assert_no_error(error);
    g_assert_nonnull(files);
    g_assert_cmpstr(body, ==, "  have a look  ");
    g_assert_cmpuint(files->len, ==, 2);
    g_assert_cmpstr(htmx_uploaded_file_get_filename(
                        g_ptr_array_index(files, 0)), ==, "shot.png");
    g_assert_cmpstr(htmx_uploaded_file_get_filename(
                        g_ptr_array_index(files, 1)), ==, "report.pdf");

    /* A plain form post has no files and leaves the body to the form. */
    g_clear_pointer(&files, g_ptr_array_unref);
    g_clear_pointer(&body, g_free);
    files = clawt_web_composer_read("application/x-www-form-urlencoded",
                                    raw, &body, NULL);
    g_assert_nonnull(files);
    g_assert_cmpuint(files->len, ==, 0);
    g_assert_null(body);
}

/*
 * Body plus two files gives one message whose block names both, by id,
 * in the shared format -- and each file was put under the one agent.
 *
 * What breaks without it: the web client's old Attach wrote the file
 * into the workspace and sent nothing, so the agent never learned it
 * was there.  This is the message the agent actually receives.
 */
static void
test_files_go_with_the_message(void)
{
    g_autoptr(GBytes) raw = composer_post();
    g_autoptr(GPtrArray) files = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *message = NULL;
    FakePut fake = { 0 };
    GString *expected;
    g_autofree gchar *want = NULL;

    fake.agents = g_ptr_array_new_with_free_func(g_free);
    fake.names = g_ptr_array_new_with_free_func(g_free);

    files = clawt_web_composer_read("multipart/form-data; boundary=XyZ",
                                    raw, &body, NULL);
    g_strstrip(body);
    message = clawt_web_body_with_files("scout", body, files, fake_put,
                                        &fake);

    g_assert_cmpuint(fake.names->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(fake.agents, 0), ==, "scout");
    g_assert_cmpstr(g_ptr_array_index(fake.agents, 1), ==, "scout");

    expected = clawt_attachment_block_new("have a look");
    clawt_attachment_block_add(expected, "shot.png",
                               "/state/exchange/scout/shot.png",
                               "/exchange/in-box", "0id-shot.png");
    clawt_attachment_block_add(expected, "report.pdf",
                               "/state/exchange/scout/report.pdf",
                               "/exchange/in-box", "0id-report.pdf");
    want = g_string_free(expected, FALSE);

    g_assert_cmpstr(message, ==, want);
    g_assert_nonnull(strstr(message, "clawt:0id-shot.png\n"));
    g_assert_nonnull(strstr(message, "clawt:0id-report.pdf\n"));

    g_ptr_array_unref(fake.agents);
    g_ptr_array_unref(fake.names);
}

/*
 * A put the daemon refused is listed, not dropped, and the others
 * still go.
 *
 * What breaks without it: an agent told nothing about a file it was
 * meant to have answers as if it was sent nothing, and the operator
 * never learns the upload failed -- GTK and lrg both list it.
 */
static void
test_a_refused_file_is_listed(void)
{
    g_autoptr(GBytes) raw = composer_post();
    g_autoptr(GPtrArray) files = NULL;
    g_autofree gchar *body = NULL;
    g_autofree gchar *message = NULL;
    FakePut fake = { 0 };

    fake.agents = g_ptr_array_new_with_free_func(g_free);
    fake.names = g_ptr_array_new_with_free_func(g_free);
    fake.refuse = "shot.png";

    files = clawt_web_composer_read("multipart/form-data; boundary=XyZ",
                                    raw, &body, NULL);
    message = clawt_web_body_with_files("scout", "", files, fake_put, &fake);

    g_assert_true(g_str_has_prefix(message, CLAWT_ATTACHMENT_MARKER));
    g_assert_nonnull(strstr(message, "- shot.png (could not be saved)\n"));
    g_assert_nonnull(strstr(message, "clawt:0id-report.pdf\n"));

    g_ptr_array_unref(fake.agents);
    g_ptr_array_unref(fake.names);
}

/*
 * A room takes files only with exactly one agent in it.
 *
 * What breaks without it: attachment.put files under one agent's
 * exchange, so in a room of two the second agent is sent a path it
 * cannot open.
 */
static void
test_a_room_files_under_its_one_agent(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *one = NULL;
    g_autofree gchar *none = NULL;
    g_autofree gchar *two = NULL;

    g_assert_true(json_parser_load_from_data(
        parser, "[[\"user\",\"scout\"],[\"user\"],"
                "[\"user\",\"scout\",\"lead\"]]", -1, NULL));

    {
        JsonArray *all = json_node_get_array(json_parser_get_root(parser));

        one = clawt_web_room_file_agent(json_array_get_array_element(all, 0));
        none = clawt_web_room_file_agent(json_array_get_array_element(all, 1));
        two = clawt_web_room_file_agent(json_array_get_array_element(all, 2));
    }

    g_assert_cmpstr(one, ==, "scout");
    g_assert_null(none);
    g_assert_null(two);
    g_assert_null(clawt_web_room_file_agent(NULL));
}

/*
 * The Files page: a PDF tile is its first page from the thumb route, an
 * image tile is the image itself, a text tile its first lines (escaped),
 * and a file that is gone has no link.
 *
 * What breaks without it: a PDF drawn as <img src=/f/attachment/<id>>
 * is a broken image (the browser cannot draw a PDF in an <img>), and a
 * text file's lines unescaped would put a model's words into the page
 * as markup.
 */
static void
test_the_files_grid(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GHashTable) texts = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    g_autoptr(HtmxElement) grid = NULL;
    g_autoptr(HtmxElement) empty = NULL;
    g_autofree gchar *html = NULL;
    g_autofree gchar *none = NULL;
    JsonArray *items;

    g_assert_true(json_parser_load_from_data(parser,
        "[{\"id\":\"1-paper.pdf\",\"name\":\"paper.pdf\",\"kind\":\"pdf\","
        "  \"bytes\":2048,\"exists\":true,\"sender\":\"scout\",\"ts\":1},"
        " {\"id\":\"2-shot.png\",\"name\":\"shot.png\",\"kind\":\"image\","
        "  \"bytes\":10,\"exists\":true,\"sender\":\"user\",\"ts\":1},"
        " {\"id\":\"3-log.txt\",\"name\":\"log.txt\",\"kind\":\"text\","
        "  \"bytes\":10,\"exists\":true,\"sender\":\"scout\",\"ts\":1},"
        " {\"id\":\"4-gone.pdf\",\"name\":\"gone.pdf\",\"kind\":\"pdf\","
        "  \"bytes\":0,\"exists\":false,\"sender\":\"scout\",\"ts\":1}]",
        -1, NULL));
    items = json_node_get_array(json_parser_get_root(parser));
    g_hash_table_insert(texts, g_strdup("3-log.txt"),
                        g_strdup("<script>alert(1)</script>"));

    grid = clawt_web_files_grid(items, texts);
    html = htmx_element_render(grid);

    g_assert_nonnull(strstr(html, "src=\"/f/attachment/1-paper.pdf/thumb"
                                  "?size=256\""));
    g_assert_nonnull(strstr(html, "src=\"/f/attachment/2-shot.png\""));
    g_assert_nonnull(strstr(html, "&lt;script&gt;"));
    g_assert_null(strstr(html, "<script>"));
    g_assert_nonnull(strstr(html, "href=\"/f/attachment/1-paper.pdf\""));
    g_assert_null(strstr(html, "href=\"/f/attachment/4-gone.pdf\""));
    g_assert_null(strstr(html, "4-gone.pdf/thumb"));
    g_assert_nonnull(strstr(html, "file-gone"));

    empty = clawt_web_files_grid(NULL, NULL);
    none = htmx_element_render(empty);
    g_assert_nonnull(strstr(none, "No files have been sent"));
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

    g_test_add_func("/web-app/files/composer-post-is-read",
                    test_a_composer_post_is_read);
    g_test_add_func("/web-app/files/go-with-the-message",
                    test_files_go_with_the_message);
    g_test_add_func("/web-app/files/refused-file-is-listed",
                    test_a_refused_file_is_listed);
    g_test_add_func("/web-app/files/room-files-under-one-agent",
                    test_a_room_files_under_its_one_agent);
    g_test_add_func("/web-app/files/grid", test_the_files_grid);

    return g_test_run();
}
