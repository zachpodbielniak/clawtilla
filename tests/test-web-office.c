/*
 * test-web-office.c - the web client's drawn office: its routes and markup
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The web client is a binary, so its sources are included rather than
 * linked, as test-web-app and test-web-render do.  The routes are driven
 * against a real daemon over its socket -- office on, the shipped world
 * packs, agents configured but never started -- because what they serve
 * is the daemon's answer reshaped, and a stubbed answer would test the
 * stub.  Nothing here reaches the network: the daemon is told to bind
 * nothing and the web app is never served.
 *
 * What the Content-Type of a response is cannot be read back from an
 * HtmxResponse (it has no getter), so the file route's type is checked
 * through office_file_type(), the one function the route sets it from.
 */

#include "clawtilla.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#include "clawt-test-util.h"

#include "../clients/web/web-app.c"
#include "../clients/web/web-files.c"
#include "../clients/web/web-ui.c"
#include "../clients/web/web-style.c"
#include "../clients/web/web-office-view.c"

/* The page frame needs the fleet and the topbar; nothing here draws one. */
HtmxElement *
clawt_web_sidebar(ClawtWebApp *app, const gchar *selected, ClawtPage view,
                  const ClawtWebLook *look)
{
    (void)app;
    (void)selected;
    (void)view;
    (void)look;
    return HTMX_ELEMENT(htmx_div_new());
}

HtmxElement *
clawt_web_topbar(ClawtWebApp *app, const gchar *agent_id, ClawtPage view)
{
    (void)app;
    (void)agent_id;
    (void)view;
    return HTMX_ELEMENT(htmx_div_new());
}

/* ── A daemon, and a web app talking to it ───────────────────────── */

typedef struct {
    gchar        *dir;
    gchar        *socket_path;
    ClawtDaemon  *daemon;
    GMainContext *context;
    ClawtClient  *client;
    ClawtWebApp  *app;
} OfficeFixture;

/*
 * A fleet of two agents, one of them named with markup, and the office
 * @on or off.  The world directory is the checkout's own shipped packs,
 * named outright so the test does not depend on where the binary is.
 */
static void
fixture_setup(OfficeFixture *fixture, gboolean on)
{
    g_autofree gchar *config_path = NULL;
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-web-office-XXXXXX", NULL);
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
        "  enabled: %s\n"
        "  world: modern-office\n"
        "  world_dirs: [\"%s/data/office/worlds\"]\n"
        "agents:\n"
        "  - id: ada\n"
        "    name: Ada\n"
        "    color: \"#7c5cff\"\n"
        "  - id: mallory\n"
        "    name: \"Mal </script><b>lory\"\n",
        fixture->dir, fixture->socket_path, fixture->dir, fixture->dir,
        on ? "true" : "false", CLAWT_TEST_SRCDIR);
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
    fixture->app = clawt_web_app_new(fixture->client);
}

static void
fixture_teardown(OfficeFixture *fixture)
{
    g_clear_object(&fixture->app);
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
}

/* GET @path through @handler, with @world as the route's :world. */
static HtmxResponse *
get(OfficeFixture *fixture, HtmxRouteCallback handler, const gchar *path,
    const gchar *world)
{
    g_autoptr(HtmxRequest) request = htmx_request_new_for_path(HTMX_METHOD_GET, path);
    g_autoptr(GHashTable) params = g_hash_table_new(g_str_hash, g_str_equal);

    if (world != NULL)
        g_hash_table_insert(params, (gpointer)"world", (gpointer)world);
    return handler(request, params, fixture->app);
}

static JsonObject *
parse_object(const gchar *text, JsonParser *parser)
{
    g_autoptr(GError) error = NULL;

    g_assert_nonnull(text);
    g_assert_true(json_parser_load_from_data(parser, text, -1, &error));
    g_assert_no_error(error);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser)));
    return json_node_get_object(json_parser_get_root(parser));
}

/* ── Without a daemon ────────────────────────────────────────────── */

/*
 * The types the file route answers with.  SVG is bytes on purpose: a pack
 * is written by agents, and an SVG served as an image from this origin is
 * a document that runs script with the page's cookies.
 */
static void
test_file_types(void)
{
    g_assert_cmpstr(office_file_type("tiles.png"), ==, "image/png");
    g_assert_cmpstr(office_file_type("art/Hero.JPG"), ==, "image/jpeg");
    g_assert_cmpstr(office_file_type("map.tmj"), ==, "application/json");
    g_assert_cmpstr(office_file_type("set.tsj"), ==, "application/json");
    g_assert_cmpstr(office_file_type("office.yaml"), ==, "text/plain; charset=utf-8");
    g_assert_cmpstr(office_file_type("face.svg"), ==, "application/octet-stream");
    g_assert_cmpstr(office_file_type("page.html"), ==, "application/octet-stream");
    g_assert_cmpstr(office_file_type(NULL), ==, "application/octet-stream");
}

/* Somebody's words in the data block cannot end it and start a script. */
static void
test_json_for_script_escapes(void)
{
    g_autoptr(JsonObject) object = json_object_new();
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *text = NULL;
    JsonObject *back;

    json_object_set_string_member(object, "detail", "</script><script>alert(1)</script> & <!--");
    text = office_json_for_script(object);
    g_assert_null(strchr(text, '<'));
    g_assert_null(strchr(text, '>'));
    g_assert_null(strchr(text, '&'));
    back = parse_object(text, parser);
    g_assert_cmpstr(json_object_get_string_member(back, "detail"), ==,
                    "</script><script>alert(1)</script> & <!--");
}

/* A path the pack rule refuses never reaches the daemon at all. */
static void
test_file_refused_without_asking(void)
{
    g_autoptr(ClawtClient) client = clawt_client_new("/nonexistent/clawt-test.sock");
    g_autoptr(ClawtWebApp) app = clawt_web_app_new(client);
    const gchar *paths[] = {
        "/office/file/modern-office?path=../cozy-town/tiles.png",
        "/office/file/modern-office?path=/etc/passwd",
        "/office/file/modern-office?path=.hidden",
        "/office/file/modern-office?path=a//b.png",
        "/office/file/modern-office",
    };
    /*
     * The app asked the daemon once already, on creation, and kept the
     * failure; every call replaces that string, so it staying the same
     * string is the route not having called.
     */
    const gchar *before = clawt_web_app_last_error(app);
    guint i;

    for (i = 0; i < G_N_ELEMENTS(paths); i++) {
        g_autoptr(HtmxRequest) request = htmx_request_new_for_path(HTMX_METHOD_GET, paths[i]);
        g_autoptr(GHashTable) params = g_hash_table_new(g_str_hash, g_str_equal);
        g_autoptr(HtmxResponse) response = NULL;

        g_hash_table_insert(params, (gpointer)"world", (gpointer)"modern-office");
        response = on_file(request, params, app);
        g_assert_cmpuint(htmx_response_get_status(response), ==, SOUP_STATUS_FORBIDDEN);
        g_assert_true(clawt_web_app_last_error(app) == before);
    }

    /* and a world id that is a path, escaped or not */
    {
        g_autoptr(HtmxRequest) request = htmx_request_new_for_path(
            HTMX_METHOD_GET, "/office/file/x?path=tiles.png");
        g_autoptr(GHashTable) params = g_hash_table_new(g_str_hash, g_str_equal);
        g_autoptr(HtmxResponse) response = NULL;

        g_hash_table_insert(params, (gpointer)"world", (gpointer)"..%2Fmodern-office");
        response = on_file(request, params, app);
        g_assert_cmpuint(htmx_response_get_status(response), ==, SOUP_STATUS_FORBIDDEN);
    }
}

/* ── Against a daemon ────────────────────────────────────────────── */

/* A pack file comes back byte for byte; one outside the pack does not. */
static void
test_file_route(void)
{
    OfficeFixture fixture = { 0 };
    g_autoptr(HtmxResponse) png = NULL;
    g_autoptr(HtmxResponse) missing = NULL;
    g_autofree gchar *expected = NULL;
    g_autofree gchar *path = NULL;
    gsize length = 0;
    GBytes *bytes;

    fixture_setup(&fixture, TRUE);

    path = g_build_filename(CLAWT_TEST_SRCDIR, "data", "office", "worlds",
                            "modern-office", "tiles.png", NULL);
    g_assert_true(g_file_get_contents(path, &expected, &length, NULL));

    png = get(&fixture, on_file, "/office/file/modern-office?path=tiles.png",
              "modern-office");
    g_assert_cmpuint(htmx_response_get_status(png), ==, SOUP_STATUS_OK);
    bytes = htmx_response_get_bytes(png);
    g_assert_nonnull(bytes);
    g_assert_cmpuint(g_bytes_get_size(bytes), ==, length);
    g_assert_true(memcmp(g_bytes_get_data(bytes, NULL), expected, length) == 0);
    /* the PNG signature survives: a body written as a string stops at its NUL */
    g_assert_true(memcmp(g_bytes_get_data(bytes, NULL), "\x89PNG\r\n\x1a\n", 8) == 0);

    missing = get(&fixture, on_file, "/office/file/modern-office?path=nope.png",
                  "modern-office");
    g_assert_cmpuint(htmx_response_get_status(missing), ==, SOUP_STATUS_NOT_FOUND);

    fixture_teardown(&fixture);
}

/*
 * The poll's answer: the layout, a face for everybody in it (the colour
 * the sidebar paints), each one's roster line, and the world it names.
 */
static void
test_layout_route(void)
{
    OfficeFixture fixture = { 0 };
    g_autoptr(HtmxResponse) response = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    JsonObject *body;
    JsonObject *faces;
    JsonObject *ada;

    fixture_setup(&fixture, TRUE);
    response = get(&fixture, on_layout_json, "/office/layout.json", NULL);
    g_assert_cmpuint(htmx_response_get_status(response), ==, SOUP_STATUS_OK);
    body = parse_object(htmx_response_get_content(response), parser);

    g_assert_true(json_object_get_boolean_member(body, "enabled"));
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(body, "layout"), "world"), ==, "modern-office");
    faces = json_object_get_object_member(body, "faces");
    g_assert_nonnull(faces);
    ada = json_object_get_object_member(faces, "ada");
    g_assert_nonnull(ada);
    g_assert_cmpstr(json_object_get_string_member(ada, "color"), ==,
                    clawt_avatar_color("ada", "Ada", "#7c5cff"));
    g_assert_true(json_object_has_member(faces, "mallory"));
    g_assert_true(json_object_has_member(
        json_object_get_object_member(body, "lines"), "ada"));

    fixture_teardown(&fixture);
}

/* The world, with its map parsed and every tileset inlined as data. */
static void
test_world_route(void)
{
    OfficeFixture fixture = { 0 };
    g_autoptr(HtmxResponse) response = NULL;
    g_autoptr(HtmxResponse) other = NULL;
    g_autoptr(HtmxResponse) bad = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(JsonParser) other_parser = json_parser_new();
    JsonObject *body;
    JsonObject *map;
    JsonArray *tilesets;
    const gchar *src;

    fixture_setup(&fixture, TRUE);
    response = get(&fixture, on_world_json, "/office/world.json", NULL);
    g_assert_cmpuint(htmx_response_get_status(response), ==, SOUP_STATUS_OK);
    body = parse_object(htmx_response_get_content(response), parser);
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(body, "world"), "id"), ==, "modern-office");
    map = json_object_get_object_member(body, "map");
    g_assert_nonnull(map);
    g_assert_cmpint(json_object_get_int_member(map, "tilewidth"), ==, 32);
    tilesets = json_object_get_array_member(map, "tilesets");
    g_assert_cmpuint(json_array_get_length(tilesets), >=, 1);
    src = json_object_get_string_member(json_array_get_object_element(tilesets, 0), "src");
    g_assert_true(g_str_has_prefix(src, "data:image/png;base64,"));

    /* another pack by name, as the script asks after a world change */
    other = get(&fixture, on_world_json, "/office/world.json?world=cozy-town", NULL);
    g_assert_cmpuint(htmx_response_get_status(other), ==, SOUP_STATUS_OK);
    body = parse_object(htmx_response_get_content(other), other_parser);
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(body, "world"), "id"), ==, "cozy-town");

    bad = get(&fixture, on_world_json, "/office/world.json?world=../etc", NULL);
    g_assert_cmpuint(htmx_response_get_status(bad), ==, SOUP_STATUS_BAD_REQUEST);

    fixture_teardown(&fixture);
}

/*
 * The page: two canvases, the first scene embedded as JSON that parses
 * back to the layout, the script from this server's own /static -- and a
 * name with markup in it reaches the data block only escaped.
 */
static void
test_markup_when_on(void)
{
    OfficeFixture fixture = { 0 };
    g_autoptr(HtmxDiv) parent = htmx_div_new();
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *html = NULL;
    g_autofree gchar *json = NULL;
    const gchar *start;
    const gchar *end;
    JsonObject *scene;

    fixture_setup(&fixture, TRUE);
    clawt_web_office_view_add(fixture.app, HTMX_ELEMENT(parent));
    html = htmx_element_render(HTMX_ELEMENT(parent));

    g_assert_nonnull(strstr(html, "id=\"office-view\""));
    g_assert_nonnull(strstr(html, "<canvas class=\"office-floor\""));
    g_assert_nonnull(strstr(html, "<canvas class=\"office-actors\""));
    g_assert_nonnull(strstr(html, "src=\"/static/office.js\""));
    g_assert_nonnull(strstr(html, "data-office-tilt"));
    /* nothing loaded from elsewhere (an SVG's xmlns is a name, not a fetch) */
    g_assert_null(strstr(html, "src=\"http"));
    g_assert_null(strstr(html, "href=\"http"));
    g_assert_null(strstr(html, "src=\"//"));
    g_assert_null(strstr(html, "</script><b>"));

    start = strstr(html, "id=\"office-scene\"");
    g_assert_nonnull(start);
    g_assert_nonnull(strstr(html, "type=\"application/json\""));
    start = strchr(start, '>');
    g_assert_nonnull(start);
    start++;
    end = strstr(start, "</script>");
    g_assert_nonnull(end);
    json = g_strndup(start, (gsize)(end - start));
    scene = parse_object(json, parser);

    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(scene, "layout"), "world"), ==, "modern-office");
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(scene, "world"), "id"), ==, "modern-office");
    g_assert_true(json_object_has_member(scene, "map"));
    g_assert_true(json_object_has_member(scene, "faces"));
    /* the activity words come from libclawt, every one of them */
    g_assert_cmpuint(json_object_get_size(json_object_get_object_member(scene, "words")),
                     ==, clawt_office_activity_count());
    g_assert_cmpstr(json_object_get_string_member(
        json_object_get_object_member(json_object_get_object_member(scene, "words"),
                                      "waiting_on_you"), "label"), ==,
        clawt_office_activity_label(CLAWT_OFFICE_WAITING_ON_YOU));

    fixture_teardown(&fixture);
}

/* Off: the view adds nothing, and the poll says off so the script stops. */
static void
test_nothing_when_off(void)
{
    OfficeFixture fixture = { 0 };
    g_autoptr(HtmxDiv) parent = htmx_div_new();
    g_autoptr(HtmxResponse) response = NULL;
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *html = NULL;
    JsonObject *body;

    fixture_setup(&fixture, FALSE);
    clawt_web_office_view_add(fixture.app, HTMX_ELEMENT(parent));
    html = htmx_element_render(HTMX_ELEMENT(parent));
    g_assert_null(strstr(html, "canvas"));
    g_assert_null(strstr(html, "office-scene"));
    g_assert_null(strstr(html, "office.js"));
    g_assert_null(strstr(html, "OFFICE=1"));

    response = get(&fixture, on_layout_json, "/office/layout.json", NULL);
    g_assert_cmpuint(htmx_response_get_status(response), ==, SOUP_STATUS_OK);
    body = parse_object(htmx_response_get_content(response), parser);
    g_assert_false(json_object_get_boolean_member(body, "enabled"));

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/web-office/file-types", test_file_types);
    g_test_add_func("/web-office/json-for-script-escapes", test_json_for_script_escapes);
    g_test_add_func("/web-office/file-refused-without-asking",
                    test_file_refused_without_asking);
    g_test_add_func("/web-office/file-route", test_file_route);
    g_test_add_func("/web-office/layout-route", test_layout_route);
    g_test_add_func("/web-office/world-route", test_world_route);
    g_test_add_func("/web-office/markup-when-on", test_markup_when_on);
    g_test_add_func("/web-office/nothing-when-off", test_nothing_when_off);

    return g_test_run();
}
