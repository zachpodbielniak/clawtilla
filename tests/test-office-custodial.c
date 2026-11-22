/*
 * test-office-custodial.c - Editing world packs, and the team that does it
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The library half is the safety of a write: a path that leaves the pack,
 * a link planted in one, a manifest that would stop the pack loading and
 * an asset with no provenance are each refused, and a refused write
 * leaves the pack as it was.  The daemon half installs the custodial team
 * against a fixture fleet (nothing is started: make test runs no agent
 * CLI), gates the office tools, and commissions a character, checking
 * the picture lands where a confined steward can read it.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <ai-glib.h>

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

static const gchar *const good_manifest =
    "name: Test\n"
    "license: CC0-1.0\n"
    "renderers:\n"
    "  2d:\n"
    "    map: map.tmj\n"
    "zones:\n"
    "  - id: floor\n"
    "    tags: [desk]\n"
    "    anchors_2d: [[10, 10], [20, 10]]\n"
    "  - id: lounge\n"
    "    tags: [lounge]\n"
    "    anchors_2d: [[40, 40]]\n";

static gchar *
make_pack(const gchar *parent, const gchar *id)
{
    gchar *dir = g_build_filename(parent, id, NULL);
    g_autofree gchar *manifest = g_build_filename(dir, "office.yaml", NULL);
    g_autofree gchar *map = g_build_filename(dir, "map.tmj", NULL);

    g_assert_cmpint(g_mkdir_with_parents(dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(manifest, good_manifest, -1, NULL));
    g_assert_true(g_file_set_contents(map, "{}", -1, NULL));
    return dir;
}

/* ── Paths ──────────────────────────────────────────────────────── */

static void
test_paths(void)
{
    g_assert_true(clawt_office_pack_path_is_safe("office.yaml"));
    g_assert_true(clawt_office_pack_path_is_safe("tiles/desk.png"));

    g_assert_false(clawt_office_pack_path_is_safe(NULL));
    g_assert_false(clawt_office_pack_path_is_safe(""));
    g_assert_false(clawt_office_pack_path_is_safe("/etc/passwd"));
    g_assert_false(clawt_office_pack_path_is_safe("../other/office.yaml"));
    g_assert_false(clawt_office_pack_path_is_safe("tiles/../../x"));
    g_assert_false(clawt_office_pack_path_is_safe(".hidden"));
    g_assert_false(clawt_office_pack_path_is_safe("tiles//x"));
    g_assert_false(clawt_office_pack_path_is_safe("tiles\\x"));
}

/* ── Writing ────────────────────────────────────────────────────── */

/* A manifest that would not load is refused, and the old one put back. */
static void
test_refused_write_is_undone(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-pack-XXXXXX", NULL);
    g_autofree gchar *pack = make_pack(root, "test");
    g_autofree gchar *manifest = g_build_filename(pack, "office.yaml", NULL);
    g_autofree gchar *after = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtOfficeWorld) world = NULL;
    const gchar *no_licence = "name: Test\nrenderers:\n  2d:\n    map: map.tmj\n";
    const gchar *better = "name: Better\nlicense: CC0-1.0\nrenderers:\n  2d:\n"
                          "    map: map.tmj\nzones:\n  - id: lounge\n    tags: [lounge]\n"
                          "    anchors_2d: [[1, 1]]\n";

    g_assert_false(clawt_office_pack_write_file(pack, "office.yaml", no_licence,
                                                strlen(no_licence), &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
    g_assert_nonnull(strstr(error->message, "was not kept"));
    g_assert_true(g_file_get_contents(manifest, &after, NULL, NULL));
    g_assert_cmpstr(after, ==, good_manifest);
    g_clear_error(&error);

    /* a good one stays, and loads */
    g_assert_true(clawt_office_pack_write_file(pack, "office.yaml", better,
                                               strlen(better), &error));
    g_assert_no_error(error);
    world = clawt_office_world_load(pack, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(clawt_office_world_get_name(world), ==, "Better");

    /* a choreography rule naming no activity is refused, and nothing left */
    {
        g_autofree gchar *chor = g_build_filename(pack, "choreography.yaml", NULL);
        const gchar *bad = "choreography:\n  - { activity: dancing, zone: lounge }\n";

        g_assert_false(clawt_office_pack_write_file(pack, "choreography.yaml", bad,
                                                    strlen(bad), &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
        g_assert_false(g_file_test(chor, G_FILE_TEST_EXISTS));
    }

    clawt_test_remove_tree(root);
}

/* A link planted in a pack does not carry a write out of it. */
static void
test_link_does_not_escape(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-pack-XXXXXX", NULL);
    g_autofree gchar *pack = make_pack(root, "test");
    g_autofree gchar *outside = g_build_filename(root, "outside", NULL);
    g_autofree gchar *link = g_build_filename(pack, "tiles", NULL);
    g_autofree gchar *escaped = g_build_filename(outside, "x.txt", NULL);
    g_autoptr(GError) error = NULL;

    g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);
    g_assert_cmpint(symlink(outside, link), ==, 0);

    g_assert_false(clawt_office_pack_write_file(pack, "tiles/x.txt", "hi", 2, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PERMISSION_DENIED);
    g_assert_false(g_file_test(escaped, G_FILE_TEST_EXISTS));

    clawt_test_remove_tree(root);
}

/* A fork copies files and directories and never a link. */
static void
test_fork(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-pack-XXXXXX", NULL);
    g_autofree gchar *pack = make_pack(root, "test");
    g_autofree gchar *copy = g_build_filename(root, "copy", NULL);
    g_autofree gchar *link = g_build_filename(pack, "passwd", NULL);
    g_autofree gchar *copied_link = g_build_filename(copy, "passwd", NULL);
    g_autofree gchar *copied_map = g_build_filename(copy, "map.tmj", NULL);
    g_autoptr(GError) error = NULL;

    g_assert_cmpint(symlink("/etc/passwd", link), ==, 0);
    g_assert_true(clawt_office_pack_fork(pack, copy, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(copied_map, G_FILE_TEST_IS_REGULAR));
    g_assert_false(g_file_test(copied_link, G_FILE_TEST_EXISTS));

    /* never over something that is there */
    g_assert_false(clawt_office_pack_fork(pack, copy, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);

    clawt_test_remove_tree(root);
}

/* An asset is recorded once, with its checksum and provenance, or refused. */
static void
test_import_asset(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-pack-XXXXXX", NULL);
    g_autofree gchar *pack = make_pack(root, "test");
    g_autofree gchar *source = g_build_filename(root, "desk.png", NULL);
    g_autofree gchar *sum = NULL;
    g_autofree gchar *expected = g_compute_checksum_for_string(G_CHECKSUM_SHA256,
                                                               "pixels", -1);
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtOfficeWorld) world = NULL;
    GPtrArray *assets;
    ClawtOfficeAsset *asset;

    g_assert_true(g_file_set_contents(source, "pixels", -1, NULL));

    g_assert_false(clawt_office_pack_import_asset(pack, source, "tiles/desk.png", NULL,
                                                  NULL, NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(clawt_office_pack_import_asset(pack, source, "office.yaml", NULL,
                                                  "me", NULL, &error));
    g_clear_error(&error);

    g_assert_true(clawt_office_pack_import_asset(pack, source, "tiles/desk.png", "CC-BY-4.0",
                                                 "drawn by hand", &sum, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(sum, ==, expected);
    /* again: replaced, not listed twice */
    g_assert_true(clawt_office_pack_import_asset(pack, source, "tiles/desk.png", NULL,
                                                 "drawn again", NULL, &error));

    world = clawt_office_world_load(pack, &error);
    g_assert_no_error(error);
    assets = clawt_office_world_get_assets(world);
    g_assert_cmpuint(assets->len, ==, 1);
    asset = g_ptr_array_index(assets, 0);
    g_assert_cmpstr(asset->path, ==, "tiles/desk.png");
    g_assert_cmpstr(asset->sha256, ==, expected);
    g_assert_cmpstr(asset->source, ==, "drawn again");
    g_assert_true(clawt_office_world_verify_assets(world, &error));
    g_assert_no_error(error);
    /* the zones survived the rewrite */
    g_assert_nonnull(clawt_office_world_find_zone(world, "lounge", NULL, 0));

    clawt_test_remove_tree(root);
}

/* ── Characters ─────────────────────────────────────────────────── */

/* A PNG's signature and IHDR: all the size check reads. */
static void
write_png_header(const gchar *path, guint width, guint height)
{
    guchar bytes[33] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
                         0, 0, 0, 13, 'I', 'H', 'D', 'R' };

    bytes[16] = (guchar)(width >> 24);
    bytes[17] = (guchar)(width >> 16);
    bytes[18] = (guchar)(width >> 8);
    bytes[19] = (guchar)width;
    bytes[20] = (guchar)(height >> 24);
    bytes[21] = (guchar)(height >> 16);
    bytes[22] = (guchar)(height >> 8);
    bytes[23] = (guchar)height;
    bytes[24] = 8;
    bytes[25] = 6;
    g_assert_true(g_file_set_contents(path, (const gchar *)bytes, sizeof(bytes), NULL));
}

static JsonObject *
json_of(const gchar *text)
{
    g_autoptr(JsonParser) parser = json_parser_new();

    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    return json_object_ref(json_node_get_object(json_parser_get_root(parser)));
}

/*
 * A sheet is described only after it is imported, and only as far as it
 * goes: a row or a frame past the image is refused, so a renderer can
 * never read outside it; a refusal leaves the manifest as it was.
 */
static void
test_character_layout(void)
{
    g_autofree gchar *root = g_dir_make_tmp("clawt-chars-XXXXXX", NULL);
    g_autofree gchar *source = g_build_filename(root, "sheet.png", NULL);
    g_autofree gchar *kudu = g_build_filename(root, "kudu", NULL);
    g_autoptr(JsonObject) good = json_of(
        "{\"idle\":{\"row\":0,\"frames\":1},\"walk_down\":{\"row\":1,\"frames\":4,\"fps\":8}}");
    g_autoptr(JsonObject) past_row = json_of("{\"idle\":{\"row\":6,\"frames\":1}}");
    g_autoptr(JsonObject) past_frames = json_of("{\"idle\":{\"row\":0,\"frames\":5}}");
    g_autoptr(JsonObject) bad_name = json_of("{\"Idle!\":{\"row\":0,\"frames\":1}}");
    g_autoptr(JsonObject) none = json_of("{}");
    g_autoptr(ClawtOfficeCharacter) loaded = NULL;
    g_autoptr(GError) error = NULL;
    guint w = 0;
    guint h = 0;

    write_png_header(source, 128, 288);
    g_assert_true(clawt_office_png_size(source, &w, &h));
    g_assert_cmpuint(w, ==, 128);
    g_assert_cmpuint(h, ==, 288);

    /* nothing imported yet: nothing to describe */
    g_assert_false(clawt_office_character_set_sheet(root, "kudu", "sprites/sheet.png", 32, 48,
                                                    good, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_FOUND);
    g_clear_error(&error);

    g_assert_true(clawt_office_character_import_asset(root, "kudu", source,
                                                      "sprites/sheet.png", "CC0-1.0",
                                                      "drawn for a test", NULL, &error));
    g_assert_no_error(error);

    /* each refusal names its reason and writes nothing */
    g_assert_false(clawt_office_character_set_sheet(root, "kudu", "sprites/sheet.png", 32, 48,
                                                    past_row, &error));
    g_assert_nonnull(strstr(error->message, "runs past"));
    g_clear_error(&error);
    g_assert_false(clawt_office_character_set_sheet(root, "kudu", "sprites/sheet.png", 32, 48,
                                                    past_frames, &error));
    g_clear_error(&error);
    g_assert_false(clawt_office_character_set_sheet(root, "kudu", "sprites/sheet.png", 32, 48,
                                                    bad_name, &error));
    g_clear_error(&error);
    g_assert_false(clawt_office_character_set_sheet(root, "kudu", "sprites/sheet.png", 32, 48,
                                                    none, &error));
    g_clear_error(&error);
    g_assert_false(clawt_office_character_set_sheet(root, "kudu", "../sheet.png", 32, 48,
                                                    good, &error));
    g_clear_error(&error);
    g_assert_false(clawt_office_character_set_sheet(root, "../kudu", "sprites/sheet.png", 32,
                                                    48, good, &error));
    g_clear_error(&error);
    loaded = clawt_office_character_load(kudu, &error);
    g_assert_no_error(error);
    g_assert_null(loaded->sheet);
    g_clear_pointer(&loaded, clawt_office_character_free);

    /* a good one, read back with the checksum the import recorded */
    g_assert_true(clawt_office_character_set_sheet(root, "kudu", "sprites/sheet.png", 32, 48,
                                                   good, &error));
    g_assert_no_error(error);
    loaded = clawt_office_character_load(kudu, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(loaded->sheet, ==, "sprites/sheet.png");
    g_assert_nonnull(loaded->sheet_sha256);
    g_assert_cmpuint(loaded->frame_width, ==, 32);
    g_assert_cmpuint(loaded->frame_height, ==, 48);
    g_assert_cmpuint(loaded->animations->len, ==, 2);
    {
        g_autoptr(JsonNode) json = clawt_office_character_to_json(loaded);
        JsonObject *animations = json_object_get_object_member(json_node_get_object(json),
                                                               "animations");

        g_assert_cmpint(json_object_get_int_member(
            json_object_get_object_member(animations, "walk_down"), "frames"), ==, 4);
    }

    clawt_test_remove_tree(root);
}

/* ── The daemon ─────────────────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    gchar        *config_path;
    GMainContext *context;
    ClawtDaemon  *daemon;
} DaemonFixture;

static void
daemon_setup(DaemonFixture *fixture, const gchar *generator)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-custodial-XXXXXX", NULL);
    fixture->config_path = g_build_filename(fixture->dir, "config.yaml", NULL);
    yaml = g_strdup_printf(
        "daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
        "  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
        "defaults:\n  workspace_root: \"%s/agents\"\n"
        "secrets:\n  dir: \"%s/secrets\"\n"
        "office:\n  enabled: true\n  asset_generator: \"%s\"\n"
        "  world_dirs: [\"%s/../../data/office/worlds\"]\n"
        "teams:\n  - id: core\n"
        "agents:\n"
        "  - id: chief\n    chief_of_staff: true\n    runtime:\n      autostart: false\n"
        "  - id: kudu\n    team: core\n    color: \"#3584e4\"\n"
        "    runtime:\n      autostart: false\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir, fixture->dir, generator,
        CLAWT_TEST_FIXTURES);
    g_assert_true(g_file_set_contents(fixture->config_path, yaml, -1, &error));

    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(fixture->config_path, fixture->context);
    clawt_daemon_start(fixture->daemon, &error);
    g_assert_no_error(error);
    fixture->daemon->ephemeral_no_start = TRUE;
}

static void
daemon_teardown(DaemonFixture *fixture)
{
    clawt_daemon_stop(fixture->daemon);
    g_clear_object(&fixture->daemon);
    while (g_main_context_iteration(fixture->context, FALSE))
        ;
    g_clear_pointer(&fixture->context, g_main_context_unref);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
    g_free(fixture->config_path);
}

static gchar *
call_tool(DaemonFixture *fixture, const gchar *agent_id, const gchar *tool,
          const gchar *json, gboolean *is_error)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *request = NULL;
    g_autoptr(JsonNode) reply = NULL;
    JsonObject *result;
    JsonArray *content;

    request = g_strdup_printf(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\","
        "\"params\":{\"name\":\"%s\",\"arguments\":%s}}", tool, json);
    g_assert_true(json_parser_load_from_data(parser, request, -1, NULL));
    reply = clawt_mcp_tools_call(fixture->daemon->mcp_tools, agent_id, NULL,
                                 json_parser_get_root(parser));
    g_assert_nonnull(reply);
    result = json_object_get_object_member(json_node_get_object(reply), "result");
    g_assert_nonnull(result);
    *is_error = json_object_get_boolean_member_with_default(result, "isError", FALSE);
    content = json_object_get_array_member(result, "content");
    return g_strdup(json_object_get_string_member(
        json_array_get_object_element(content, 0), "text"));
}

/* The chief keeps the office by role; nobody else until told. */
static void
test_tools_gated(void)
{
    DaemonFixture fixture = { 0 };
    ClawtMcpTools *tools;

    daemon_setup(&fixture, "");
    tools = fixture.daemon->mcp_tools;
    g_assert_true(clawt_agent_config_get_boolean(
        clawt_config_get_agent(fixture.daemon->config, "chief"), "tools.office"));
    g_assert_false(clawt_agent_config_get_boolean(
        clawt_config_get_agent(fixture.daemon->config, "kudu"), "tools.office"));
    g_assert_true(clawt_mcp_tools_is_permitted(tools, "chief", "clawtilla_office_worlds"));
    g_assert_true(clawt_mcp_tools_is_permitted(tools, "chief",
                                               "clawtilla_office_world_write"));
    g_assert_false(clawt_mcp_tools_is_permitted(tools, "kudu", "clawtilla_office_worlds"));
    daemon_teardown(&fixture);
}

/*
 * The operator's own pack (office.world_dirs) is never written: a copy
 * of it under the state directory would be outranked by the original
 * and never drawn.  The way that works is a new world copied from it.
 */
static void
test_write_copies_under_state(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *text = NULL;
    g_autofree gchar *copy = NULL;
    g_autofree gchar *rules = NULL;
    g_autofree gchar *original = g_build_filename(CLAWT_TEST_FIXTURES, "..", "..", "data",
                                                  "office", "worlds", "modern-office",
                                                  "choreography.yaml", NULL);
    gboolean is_error = FALSE;
#define WRITE_RULES \
        "{\"world\":\"%s\",\"path\":\"choreography.yaml\",%s" \
        "\"content\":\"choreography:\\n  - { activity: idle, zone: lounge, pose: sit, " \
        "prop: coffee }\\n\"}"
    g_autofree gchar *refused = g_strdup_printf(WRITE_RULES, "modern-office", "");
    g_autofree gchar *made = g_strdup_printf(WRITE_RULES, "winter",
                                             "\"from\":\"modern-office\",");
#undef WRITE_RULES

    daemon_setup(&fixture, "");
    text = call_tool(&fixture, "chief", "clawtilla_office_world_write", refused, &is_error);
    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "operator's own"));
    g_assert_false(g_file_test(original, G_FILE_TEST_EXISTS));

    g_free(text);
    text = call_tool(&fixture, "chief", "clawtilla_office_world_write", made, &is_error);
    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "copied"));
    copy = g_build_filename(fixture.dir, "state", "office", "worlds", "winter",
                            "office.yaml", NULL);
    g_assert_true(g_file_test(copy, G_FILE_TEST_IS_REGULAR));
    rules = g_build_filename(fixture.dir, "state", "office", "worlds", "winter",
                             "choreography.yaml", NULL);
    g_assert_true(g_file_test(rules, G_FILE_TEST_IS_REGULAR));
    g_assert_false(g_file_test(original, G_FILE_TEST_EXISTS));

    g_free(text);
    text = call_tool(&fixture, "chief", "clawtilla_office_worlds", "{}", &is_error);
    g_assert_false(is_error);
    g_assert_nonnull(strstr(text, "winter"));
    g_assert_nonnull(strstr(text, "written here"));

    /* and it can be applied */
    g_free(text);
    text = call_tool(&fixture, "chief", "clawtilla_office_world_apply",
                     "{\"world\":\"winter\"}", &is_error);
    g_assert_false(is_error);
    g_assert_cmpstr(clawt_config_get_string(fixture.daemon->config, "office.world"), ==,
                    "winter");

    /* an id that is a path is refused */
    g_free(text);
    text = call_tool(&fixture, "chief", "clawtilla_office_world_write",
                     "{\"world\":\"../escape\",\"path\":\"office.yaml\",\"content\":\"x\"}",
                     &is_error);
    g_assert_true(is_error);
    daemon_teardown(&fixture);
}

/* An asset must come from the caller's own workspace. */
static void
test_import_only_from_own_workspace(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *text = NULL;
    g_autofree gchar *json = NULL;
    g_autofree gchar *outside = NULL;
    g_autofree gchar *workspace = NULL;
    g_autofree gchar *inside = NULL;
    gboolean is_error = FALSE;

    daemon_setup(&fixture, "");
    outside = g_build_filename(fixture.dir, "secret.png", NULL);
    g_assert_true(g_file_set_contents(outside, "x", -1, NULL));
    json = g_strdup_printf("{\"world\":\"mine\",\"source\":\"%s\",\"path\":\"a.png\","
                           "\"provenance\":\"test\"}", outside);
    text = call_tool(&fixture, "chief", "clawtilla_office_asset_import", json, &is_error);
    g_assert_true(is_error);
    g_assert_nonnull(strstr(text, "not in your workspace"));

    workspace = clawt_agent_config_get_workspace(
        clawt_config_get_agent(fixture.daemon->config, "chief"));
    g_assert_cmpint(g_mkdir_with_parents(workspace, 0700), ==, 0);
    inside = g_build_filename(workspace, "tile.png", NULL);
    g_assert_true(g_file_set_contents(inside, "y", -1, NULL));
    g_free(json);
    json = g_strdup_printf("{\"character\":\"kudu\",\"source\":\"%s\",\"path\":"
                           "\"sprites/walk.png\",\"provenance\":\"test\"}", inside);
    g_free(text);
    text = call_tool(&fixture, "chief", "clawtilla_office_asset_import", json, &is_error);
    if (is_error)
        g_printerr("import refused: %s\n", text);
    g_assert_false(is_error);
    {
        g_autofree gchar *landed = g_build_filename(fixture.dir, "state", "office",
                                                    "characters", "kudu", "sprites",
                                                    "walk.png", NULL);

        g_assert_true(g_file_test(landed, G_FILE_TEST_IS_REGULAR));
    }
    daemon_teardown(&fixture);
}

/*
 * The install makes a tagged team of three, refuses a second, and gives
 * the steward an approval rule on the generator.
 */
static void
test_install(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GPtrArray) created = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GError) error = NULL;
    g_autofree gchar *team = NULL;
    ClawtAgentConfig *steward;
    ClawtAgentConfig *lead;

    daemon_setup(&fixture, "hf");
    g_assert_true(clawt_daemon_office_custodians_install(fixture.daemon, "groundskeepers",
                                                         FALSE, created, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(created->len, ==, 3);

    team = clawt_daemon_office_custodial_team(fixture.daemon);
    g_assert_cmpstr(team, ==, "groundskeepers");
    lead = clawt_config_get_agent(fixture.daemon->config, "groundskeepers-lead");
    g_assert_nonnull(lead);
    g_assert_cmpstr(clawt_agent_config_get_string(lead, "team_role"), ==, "lead");
    g_assert_true(clawt_agent_config_get_boolean(lead, "tools.office"));
    steward = clawt_config_get_agent(fixture.daemon->config, "groundskeepers-assets");
    g_assert_nonnull(steward);
    {
        g_auto(GStrv) rules = clawt_agent_config_get_string_list(steward, "policy.rules");

        g_assert_nonnull(rules);
        g_assert_true(g_strv_contains((const gchar *const *)rules,
                                      "mcp__clawtilla-hf__*=ask"));
    }

    /* a second is refused, naming the first, and makes nothing */
    g_assert_false(clawt_daemon_office_custodians_install(fixture.daemon, "others", FALSE,
                                                          NULL, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);
    g_assert_nonnull(strstr(error->message, "groundskeepers"));
    g_assert_null(clawt_config_get_team(fixture.daemon->config, "others"));
    daemon_teardown(&fixture);
}

/*
 * A character is commissioned from the custodial lead, with the agent's
 * face staged in every custodian's workspace -- the steward is the one
 * that makes it, and a confined steward cannot read the lead's.
 */
static void
test_commission_character(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *task_id = NULL;
    ClawtTask *task;
    guint i;
    static const gchar *const members[] = {
        "facilities-lead", "facilities-assets", "facilities-designer", NULL
    };

    daemon_setup(&fixture, "hf");

    /* nobody keeps it but the chief, by role */
    task_id = clawt_daemon_office_commission_character(fixture.daemon, "kudu", &error);
    g_assert_no_error(error);
    task = clawt_task_manager_get(fixture.daemon->tasks, task_id);
    g_assert_cmpstr(clawt_task_get_assignee(task), ==, "chief");
    g_clear_pointer(&task_id, g_free);

    g_assert_true(clawt_daemon_office_custodians_install(fixture.daemon, NULL, FALSE,
                                                         NULL, &error));
    task_id = clawt_daemon_office_commission_character(fixture.daemon, "kudu", &error);
    g_assert_no_error(error);
    task = clawt_task_manager_get(fixture.daemon->tasks, task_id);
    g_assert_cmpstr(clawt_task_get_assignee(task), ==, "facilities-lead");
    g_assert_nonnull(strstr(clawt_task_get_prompt(task), "office-characters/kudu/face.svg"));
    g_assert_nonnull(strstr(clawt_task_get_prompt(task), "'hf'"));

    for (i = 0; members[i] != NULL; i++) {
        g_autofree gchar *workspace = clawt_agent_config_get_workspace(
            clawt_config_get_agent(fixture.daemon->config, members[i]));
        g_autofree gchar *face = g_build_filename(workspace, "office-characters", "kudu",
                                                  "face.svg", NULL);
        g_autofree gchar *svg = NULL;

        g_assert_true(g_file_get_contents(face, &svg, NULL, NULL));
        g_assert_true(g_str_has_prefix(svg, "<svg"));
        /* the agent's own colour */
        g_assert_nonnull(strstr(svg, "#3584e4"));
    }
    daemon_teardown(&fixture);
}

/* No generator, no character -- refused, with what to set. */
static void
test_no_generator_no_character(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *task_id = NULL;

    daemon_setup(&fixture, "");
    task_id = clawt_daemon_office_commission_character(fixture.daemon, "kudu", &error);
    g_assert_null(task_id);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_CONFIG_INVALID);
    g_assert_nonnull(strstr(error->message, "office.asset_generator"));
    daemon_teardown(&fixture);
}

/* office.characters lists a whole character only; office.character_file stays inside. */
static void
test_daemon_characters(void)
{
    DaemonFixture fixture = { 0 };
    g_autofree gchar *chars = NULL;
    g_autofree gchar *source = NULL;
    g_autofree gchar *secret = NULL;
    g_autofree gchar *link = NULL;
    g_autoptr(JsonObject) layout = json_of("{\"idle\":{\"row\":0,\"frames\":1}}");
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) list = NULL;
    JsonArray *characters;

    daemon_setup(&fixture, "");
    chars = clawt_daemon_office_characters_dir(fixture.daemon);
    source = g_build_filename(fixture.dir, "sheet.png", NULL);
    write_png_header(source, 64, 96);

    /* imported, not described: not listed yet */
    g_assert_true(clawt_office_character_import_asset(chars, "kudu", source, "sheet.png",
                                                      NULL, "test", NULL, &error));
    {
        g_autoptr(JsonNode) frame = clawt_ipc_request_new("office.characters", "c1");

        list = clawt_daemon_handle_request(fixture.daemon, frame);
        characters = json_object_get_array_member(clawt_ipc_frame_get_payload(list),
                                                   "characters");
        g_assert_cmpuint(json_array_get_length(characters), ==, 0);
        g_clear_pointer(&list, json_node_unref);
    }

    g_assert_true(clawt_office_character_set_sheet(chars, "kudu", "sheet.png", 32, 48, layout,
                                                   &error));
    {
        g_autoptr(JsonNode) frame = clawt_ipc_request_new("office.characters", "c2");

        list = clawt_daemon_handle_request(fixture.daemon, frame);
        characters = json_object_get_array_member(clawt_ipc_frame_get_payload(list),
                                                   "characters");
        g_assert_cmpuint(json_array_get_length(characters), ==, 1);
        g_assert_cmpstr(json_object_get_string_member(
            json_array_get_object_element(characters, 0), "agent"), ==, "kudu");
    }

    /* a link in a character does not reach outside it */
    secret = g_build_filename(fixture.dir, "secret.txt", NULL);
    g_assert_true(g_file_set_contents(secret, "not the character's", -1, NULL));
    link = g_build_filename(chars, "kudu", "leak.txt", NULL);
    g_assert_cmpint(symlink(secret, link), ==, 0);
    {
        g_autoptr(JsonNode) frame = clawt_ipc_request_new("office.character_file", "c3");
        g_autoptr(JsonNode) reply = NULL;

        clawt_ipc_frame_set_payload(frame, clawt_ipc_payload_new("agent", "kudu",
                                                                 "path", "leak.txt", NULL));
        reply = clawt_daemon_handle_request(fixture.daemon, frame);
        g_assert_true(clawt_ipc_frame_is_error(reply));
    }
    {
        g_autoptr(JsonNode) frame = clawt_ipc_request_new("office.character_file", "c4");
        g_autoptr(JsonNode) reply = NULL;

        clawt_ipc_frame_set_payload(frame, clawt_ipc_payload_new("agent", "kudu",
                                                                 "path", "sheet.png", NULL));
        reply = clawt_daemon_handle_request(fixture.daemon, frame);
        g_assert_false(clawt_ipc_frame_is_error(reply));
    }
    g_clear_pointer(&list, json_node_unref);
    daemon_teardown(&fixture);
}

/* ── Made by the daemon itself ──────────────────────────────────── */

/*
 * A generator that answers at once and counts what it was asked to do:
 * make test reaches no network, and the question here is what the
 * daemon spends money on, which is the count.
 */
typedef struct {
    GObject parent_instance;
    guint   estimates;
    guint   submits;
    gint64  price;          /* what the estimate says, micro-USD; 0 is free */
} MockGenerator;

typedef struct {
    GObjectClass parent_class;
} MockGeneratorClass;

static void mock_generator_iface_init(AiMediaGeneratorInterface *iface);

G_DEFINE_TYPE_WITH_CODE(MockGenerator, mock_generator, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(AI_TYPE_MEDIA_GENERATOR,
                                              mock_generator_iface_init))

static MockGenerator *the_mock;

static void
mock_generator_init(MockGenerator *self)
{
    (void)self;
}

static void
mock_generator_class_init(MockGeneratorClass *klass)
{
    (void)klass;
}

static void
mock_estimate_async(AiMediaGenerator *self, AiMediaRequest *request, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);

    (void)request;
    ((MockGenerator *)self)->estimates++;
    g_task_return_int(task, ((MockGenerator *)self)->price);
}

static gboolean
mock_estimate_finish(AiMediaGenerator *self, GAsyncResult *result, gint64 *micro_usd,
                     GError **error)
{
    (void)self;
    *micro_usd = g_task_propagate_int(G_TASK(result), error);
    return TRUE;
}

static void
mock_submit_async(AiMediaGenerator *self, AiMediaRequest *request, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);

    (void)request;
    ((MockGenerator *)self)->submits++;
    g_task_return_pointer(task, g_strdup("job-1"), g_free);
}

static gchar *
mock_submit_finish(AiMediaGenerator *self, GAsyncResult *result, GError **error)
{
    (void)self;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
mock_poll_async(AiMediaGenerator *self, const gchar *job_id, GCancellable *cancellable,
                GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
    AiMediaJob *job = ai_media_job_new(job_id, AI_MEDIA_JOB_STATE_DONE);

    ai_media_job_add_result_url(job, "https://results.invalid/portrait.png");
    g_task_return_pointer(task, job, g_object_unref);
}

static AiMediaJob *
mock_poll_finish(AiMediaGenerator *self, GAsyncResult *result, GError **error)
{
    (void)self;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
mock_download_async(AiMediaGenerator *self, const gchar *url, GCancellable *cancellable,
                    GAsyncReadyCallback callback, gpointer user_data)
{
    g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
    guchar png[33] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n',
                       0, 0, 0, 13, 'I', 'H', 'D', 'R',
                       0, 0, 0, 64, 0, 0, 0, 96, 8, 6 };

    (void)url;
    g_task_return_pointer(task, g_bytes_new(png, sizeof(png)), (GDestroyNotify)g_bytes_unref);
}

static GBytes *
mock_download_finish(AiMediaGenerator *self, GAsyncResult *result, GError **error)
{
    (void)self;
    return g_task_propagate_pointer(G_TASK(result), error);
}

static void
mock_generator_iface_init(AiMediaGeneratorInterface *iface)
{
    iface->estimate_async = mock_estimate_async;
    iface->estimate_finish = mock_estimate_finish;
    iface->submit_async = mock_submit_async;
    iface->submit_finish = mock_submit_finish;
    iface->poll_async = mock_poll_async;
    iface->poll_finish = mock_poll_finish;
    iface->download_async = mock_download_async;
    iface->download_finish = mock_download_finish;
}

static struct _AiMediaGenerator *
mock_factory(ClawtDaemon *self)
{
    (void)self;
    return g_object_ref(AI_MEDIA_GENERATOR(the_mock));
}

/* The open generation decision, once the estimate has landed. */
static ClawtDecision *
wait_for_generation_decision(DaemonFixture *fixture)
{
    gint64 deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;

    while (g_get_monotonic_time() < deadline) {
        g_autoptr(GPtrArray) open = clawt_decision_store_list(fixture->daemon->decisions, TRUE);
        guint i;

        for (i = 0; i < open->len; i++) {
            ClawtDecision *d = g_ptr_array_index(open, i);

            if (g_str_has_prefix(clawt_decision_get_id(d), "office-generate-"))
                return clawt_decision_copy(d);
        }
        g_main_context_iteration(fixture->context, FALSE);
    }
    return NULL;
}

static void
answer(DaemonFixture *fixture, ClawtDecision *decision, const gchar *what)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new("decision.answer", "a1");
    g_autoptr(JsonNode) reply = NULL;

    clawt_ipc_frame_set_payload(frame, clawt_ipc_payload_new(
        "decision", clawt_decision_get_id(decision), "answer", what, NULL));
    reply = clawt_daemon_handle_request(fixture->daemon, frame);
    g_assert_false(clawt_ipc_frame_is_error(reply));
}

/*
 * The estimate is asked before anything is spent and put to a person; a
 * decline spends nothing, an approve makes the character -- imported
 * with its provenance and laid out as one frame -- and spends once.
 */
static void
test_direct_generation(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtDecision) first = NULL;
    g_autoptr(ClawtDecision) second = NULL;
    g_autofree gchar *chars = NULL;
    g_autofree gchar *kudu = NULL;
    g_autoptr(ClawtOfficeCharacter) made = NULL;
    gint64 deadline;

    daemon_setup(&fixture, "");
    the_mock = g_object_new(mock_generator_get_type(), NULL);
    the_mock->price = 40000;
    fixture.daemon->office_generator_factory = mock_factory;
    fixture.daemon->office_generate_poll_ms = 5;
    chars = clawt_daemon_office_characters_dir(fixture.daemon);
    kudu = g_build_filename(chars, "kudu", NULL);

    /* asked, not spent: a decision with the cost in it */
    g_assert_true(clawt_daemon_office_generate_ask(fixture.daemon, "kudu", &error));
    g_assert_no_error(error);
    first = wait_for_generation_decision(&fixture);
    g_assert_nonnull(first);
    g_assert_nonnull(strstr(clawt_decision_get_question(first), "$0.04"));
    g_assert_cmpuint(the_mock->estimates, ==, 1);
    g_assert_cmpuint(the_mock->submits, ==, 0);

    /* declined: nothing spent, nothing made */
    answer(&fixture, first, "decline");
    while (g_main_context_iteration(fixture.context, FALSE))
        ;
    g_assert_cmpuint(the_mock->submits, ==, 0);
    g_assert_false(g_file_test(kudu, G_FILE_TEST_EXISTS));

    /* approved: spent once, and the character is whole */
    g_assert_true(clawt_daemon_office_generate_ask(fixture.daemon, "kudu", &error));
    second = wait_for_generation_decision(&fixture);
    g_assert_nonnull(second);
    answer(&fixture, second, "approve");
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < deadline) {
        g_clear_pointer(&made, clawt_office_character_free);
        made = clawt_office_character_load(kudu, NULL);
        if (made != NULL && made->sheet != NULL)
            break;
        g_main_context_iteration(fixture.context, FALSE);
    }
    g_assert_nonnull(made);
    g_assert_cmpstr(made->sheet, ==, "generated/portrait.png");
    g_assert_cmpuint(made->frame_width, ==, 64);
    g_assert_cmpuint(made->frame_height, ==, 96);
    g_assert_cmpuint(the_mock->submits, ==, 1);

    g_clear_object(&the_mock);
    daemon_teardown(&fixture);
}

/*
 * A generation that costs nothing (a self-hosted ComfyUI says 0) has
 * nothing to approve: it is made at once and no card is posted.
 */
static void
test_free_generation_needs_no_card(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *chars = NULL;
    g_autofree gchar *kudu = NULL;
    g_autoptr(ClawtOfficeCharacter) made = NULL;
    gint64 deadline;

    daemon_setup(&fixture, "");
    the_mock = g_object_new(mock_generator_get_type(), NULL);
    the_mock->price = 0;
    fixture.daemon->office_generator_factory = mock_factory;
    fixture.daemon->office_generate_poll_ms = 5;
    chars = clawt_daemon_office_characters_dir(fixture.daemon);
    kudu = g_build_filename(chars, "kudu", NULL);

    g_assert_true(clawt_daemon_office_generate_ask(fixture.daemon, "kudu", &error));
    deadline = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < deadline) {
        g_clear_pointer(&made, clawt_office_character_free);
        made = clawt_office_character_load(kudu, NULL);
        if (made != NULL && made->sheet != NULL)
            break;
        g_main_context_iteration(fixture.context, FALSE);
    }
    g_assert_nonnull(made);
    g_assert_cmpuint(the_mock->submits, ==, 1);
    {
        g_autoptr(GPtrArray) open = clawt_decision_store_list(fixture.daemon->decisions, TRUE);

        g_assert_cmpuint(open->len, ==, 0);
    }
    g_clear_object(&the_mock);
    daemon_teardown(&fixture);
}

/* The key is stored as a file, 0600, and never given back. */
static void
test_api_key(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    GStatBuf st;

    daemon_setup(&fixture, "");
    g_assert_null(clawt_daemon_office_store_api_key(fixture.daemon, "higgsfield", "no-colon",
                                                    &error));
    g_clear_error(&error);
    g_assert_null(clawt_daemon_office_store_api_key(fixture.daemon, "elsewhere", "a:b",
                                                    &error));
    g_clear_error(&error);
    /* a ComfyUI proxy token is one line */
    g_assert_null(clawt_daemon_office_store_api_key(fixture.daemon, "comfyui", "a\nb",
                                                    &error));
    g_clear_error(&error);
    path = clawt_daemon_office_store_api_key(fixture.daemon, "higgsfield", "id:sup3rsecret",
                                             &error);
    g_assert_no_error(error);
    g_assert_nonnull(path);
    g_assert_null(strstr(path, "sup3rsecret"));
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    {
        g_autoptr(JsonNode) frame = clawt_ipc_request_new("office.api_key", "k1");
        g_autoptr(JsonNode) reply = NULL;
        g_autofree gchar *text = NULL;

        clawt_ipc_frame_set_payload(frame, clawt_ipc_payload_new(
            "service", "higgsfield", "key", "id:sup3rsecret", NULL));
        reply = clawt_daemon_handle_request(fixture.daemon, frame);
        text = json_to_string(reply, FALSE);
        g_assert_null(strstr(text, "sup3rsecret"));
    }
    daemon_teardown(&fixture);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/office-custodial/paths", test_paths);
    g_test_add_func("/office-custodial/refused-write-is-undone", test_refused_write_is_undone);
    g_test_add_func("/office-custodial/link-does-not-escape", test_link_does_not_escape);
    g_test_add_func("/office-custodial/fork", test_fork);
    g_test_add_func("/office-custodial/import-asset", test_import_asset);
    g_test_add_func("/office-custodial/character-layout", test_character_layout);
    g_test_add_func("/office-custodial/tools-gated", test_tools_gated);
    g_test_add_func("/office-custodial/write-copies-under-state", test_write_copies_under_state);
    g_test_add_func("/office-custodial/import-only-from-own-workspace",
                    test_import_only_from_own_workspace);
    g_test_add_func("/office-custodial/install", test_install);
    g_test_add_func("/office-custodial/commission-character", test_commission_character);
    g_test_add_func("/office-custodial/no-generator-no-character",
                    test_no_generator_no_character);
    g_test_add_func("/office-custodial/daemon-characters", test_daemon_characters);
    g_test_add_func("/office-custodial/direct-generation", test_direct_generation);
    g_test_add_func("/office-custodial/api-key", test_api_key);
    g_test_add_func("/office-custodial/free-generation-needs-no-card",
                    test_free_generation_needs_no_card);

    return g_test_run();
}
