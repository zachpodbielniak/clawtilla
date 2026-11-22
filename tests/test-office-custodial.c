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
        "office:\n  enabled: true\n  asset_generator: \"%s\"\n"
        "  world_dirs: [\"%s/../../data/office/worlds\"]\n"
        "teams:\n  - id: core\n"
        "agents:\n"
        "  - id: chief\n    chief_of_staff: true\n    runtime:\n      autostart: false\n"
        "  - id: kudu\n    team: core\n    color: \"#3584e4\"\n"
        "    runtime:\n      autostart: false\n",
        fixture->dir, fixture->dir, fixture->dir, fixture->dir, generator,
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

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/office-custodial/paths", test_paths);
    g_test_add_func("/office-custodial/refused-write-is-undone", test_refused_write_is_undone);
    g_test_add_func("/office-custodial/link-does-not-escape", test_link_does_not_escape);
    g_test_add_func("/office-custodial/fork", test_fork);
    g_test_add_func("/office-custodial/import-asset", test_import_asset);
    g_test_add_func("/office-custodial/tools-gated", test_tools_gated);
    g_test_add_func("/office-custodial/write-copies-under-state", test_write_copies_under_state);
    g_test_add_func("/office-custodial/import-only-from-own-workspace",
                    test_import_only_from_own_workspace);
    g_test_add_func("/office-custodial/install", test_install);
    g_test_add_func("/office-custodial/commission-character", test_commission_character);
    g_test_add_func("/office-custodial/no-generator-no-character",
                    test_no_generator_no_character);

    return g_test_run();
}
