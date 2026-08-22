/*
 * test-plugin.c - Finding, loading and refusing plugins
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * These load real .so files rather than mocks, because everything worth
 * testing here -- the ABI gate, a register function returning the wrong
 * GType, an activate() that fails -- only happens through g_module_open
 * on a genuine module.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

/*
 * Where the fixture plugins and the example plugin were built.
 * BUILD_OUTDIR is the build tree matching this test's own build type, so
 * `make DEBUG=1 test` does not quietly exercise release artefacts.
 */
#define FIXTURE_PLUGIN_DIR BUILD_OUTDIR "/test-plugins"
#define EXAMPLE_PLUGIN_DIR BUILD_OUTDIR "/plugins"

static gchar *
fixture_path(const gchar *name)
{
    return g_build_filename(FIXTURE_PLUGIN_DIR, name, NULL);
}

/* ── Loading ─────────────────────────────────────────────────────── */

static void
test_loads_the_example_plugin(void)
{
    g_autoptr(ClawtPluginManager) manager = clawt_plugin_manager_new(NULL);
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    ClawtPlugin *plugin;

    path = g_build_filename(EXAMPLE_PLUGIN_DIR,
                            "libclawt-plugin-example.so", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_test_skip("the example plugin has not been built");
        return;
    }

    plugin = clawt_plugin_manager_load_file(manager, path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(plugin);

    /* The id comes from the filename, which is what plugins.<id> uses. */
    g_assert_cmpstr(clawt_plugin_get_id(plugin), ==, "example");
    g_assert_cmpstr(clawt_plugin_get_name(plugin), ==, "Example");
    g_assert_true(clawt_plugin_is_active(plugin));

    /* It declares two interfaces, and the manager finds them by type. */
    g_assert_true(CLAWT_IS_EVENT_HANDLER(plugin));
    g_assert_true(CLAWT_IS_TOOL_PROVIDER(plugin));
}

/*
 * A plugin from the wrong era is refused before anything in it is called.
 * Its register function aborts if reached, so a passing test proves the
 * gate is ahead of the call rather than behind it.
 */
static void
test_abi_mismatch_is_refused(void)
{
    g_autoptr(ClawtPluginManager) manager = clawt_plugin_manager_new(NULL);
    g_autofree gchar *path = fixture_path("libclawt-plugin-bad-abi-plugin.so");
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_plugin_manager_load_file(manager, path, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PLUGIN_ABI);

    /* The message names both versions, so the fix is obvious. */
    g_assert_nonnull(strstr(error->message, "9999"));
    g_assert_nonnull(strstr(error->message, "rebuild"));
}

/* A module registering something that is not a ClawtPlugin is refused. */
static void
test_wrong_type_is_refused(void)
{
    g_autoptr(ClawtPluginManager) manager = clawt_plugin_manager_new(NULL);
    g_autofree gchar *path = fixture_path("libclawt-plugin-not-a-plugin.so");
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_plugin_manager_load_file(manager, path, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PLUGIN_LOAD);
    g_assert_nonnull(strstr(error->message, "not a ClawtPlugin"));
}

/* A plugin whose activate() fails disables itself and nothing else. */
static void
test_failing_activate_is_isolated(void)
{
    g_autoptr(ClawtPluginManager) manager = clawt_plugin_manager_new(NULL);
    g_autofree gchar *bad = fixture_path("libclawt-plugin-refuses-plugin.so");
    g_autofree gchar *good = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) loaded = NULL;

    g_assert_null(clawt_plugin_manager_load_file(manager, bad, &error));
    g_assert_nonnull(error);

    good = g_build_filename(EXAMPLE_PLUGIN_DIR,
                            "libclawt-plugin-example.so", NULL);

    if (g_file_test(good, G_FILE_TEST_EXISTS)) {
        g_clear_error(&error);
        g_assert_nonnull(clawt_plugin_manager_load_file(manager, good,
                                                        &error));
        g_assert_no_error(error);
    }

    loaded = clawt_plugin_manager_list(manager);

    /* The one that refused is not in the list; the other one is. */
    g_assert_null(clawt_plugin_manager_get(manager, "refuses-plugin"));

    if (g_file_test(good, G_FILE_TEST_EXISTS))
        g_assert_cmpuint(loaded->len, ==, 1);
}

/* A file not named libclawt-plugin-<id>.so has no id, so it is refused. */
static void
test_a_misnamed_file_is_refused(void)
{
    g_autoptr(ClawtPluginManager) manager = clawt_plugin_manager_new(NULL);
    g_autoptr(GError) error = NULL;

    g_assert_null(clawt_plugin_manager_load_file(manager, "/tmp/random.so",
                                                 &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_PLUGIN_LOAD);
}

/* Loading the same plugin twice is refused rather than duplicated. */
static void
test_loading_twice_is_refused(void)
{
    g_autoptr(ClawtPluginManager) manager = clawt_plugin_manager_new(NULL);
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    path = g_build_filename(EXAMPLE_PLUGIN_DIR,
                            "libclawt-plugin-example.so", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_test_skip("the example plugin has not been built");
        return;
    }

    g_assert_nonnull(clawt_plugin_manager_load_file(manager, path, &error));
    g_assert_no_error(error);

    g_assert_null(clawt_plugin_manager_load_file(manager, path, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_ALREADY_EXISTS);
}

/* ── Configuration ───────────────────────────────────────────────── */

/*
 * A plugin the core has never heard of can still be turned off by name.
 * gst's plugin system hardcodes an if/else over known module names, so a
 * third-party plugin cannot be disabled at all; that is the gap this
 * closes.
 */
static void
test_config_can_disable_an_unknown_plugin(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtPluginManager) manager = NULL;
    g_autoptr(GError) error = NULL;
    guint loaded;

    config = clawt_config_load_from_string(
        "plugins:\n"
        "  disabled:\n"
        "    - example\n", &error);
    g_assert_no_error(error);

    manager = clawt_plugin_manager_new(config);

    g_setenv("CLAWT_PLUGIN_PATH", EXAMPLE_PLUGIN_DIR, TRUE);
    loaded = clawt_plugin_manager_load_all(manager);
    g_unsetenv("CLAWT_PLUGIN_PATH");

    g_assert_cmpuint(loaded, ==, 0);
    g_assert_null(clawt_plugin_manager_get(manager, "example"));
}

/* The keyed form works the same way, for a plugin with its own settings. */
static void
test_config_enabled_false_disables(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtPluginManager) manager = NULL;
    g_autoptr(GError) error = NULL;

    config = clawt_config_load_from_string(
        "plugins:\n"
        "  example:\n"
        "    enabled: false\n", &error);
    g_assert_no_error(error);

    manager = clawt_plugin_manager_new(config);

    g_setenv("CLAWT_PLUGIN_PATH", EXAMPLE_PLUGIN_DIR, TRUE);
    clawt_plugin_manager_load_all(manager);
    g_unsetenv("CLAWT_PLUGIN_PATH");

    g_assert_null(clawt_plugin_manager_get(manager, "example"));
}

/* plugins.enabled: false turns the whole mechanism off. */
static void
test_plugins_can_be_turned_off_entirely(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtPluginManager) manager = NULL;
    g_autoptr(GError) error = NULL;

    config = clawt_config_load_from_string(
        "plugins:\n  enabled: false\n", &error);
    g_assert_no_error(error);

    manager = clawt_plugin_manager_new(config);

    g_setenv("CLAWT_PLUGIN_PATH", EXAMPLE_PLUGIN_DIR, TRUE);
    g_assert_cmpuint(clawt_plugin_manager_load_all(manager), ==, 0);
    g_unsetenv("CLAWT_PLUGIN_PATH");
}

/* Settings reach the plugin's configure(). */
static void
test_settings_reach_the_plugin(void)
{
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtPluginManager) manager = NULL;
    g_autoptr(GError) error = NULL;
    ClawtPlugin *plugin;

    config = clawt_config_load_from_string(
        "plugins:\n"
        "  example:\n"
        "    mode: \"verbose\"\n", &error);
    g_assert_no_error(error);

    manager = clawt_plugin_manager_new(config);

    g_setenv("CLAWT_PLUGIN_PATH", EXAMPLE_PLUGIN_DIR, TRUE);
    clawt_plugin_manager_load_all(manager);
    g_unsetenv("CLAWT_PLUGIN_PATH");

    plugin = clawt_plugin_manager_get(manager, "example");

    if (plugin == NULL) {
        g_test_skip("the example plugin has not been built");
        return;
    }

    /*
     * In verbose mode the example plugin logs each event it sees, which
     * is how the setting is observable from outside.  The domain is NULL
     * because the plugin is built without one of its own -- matching any
     * domain here is honest rather than lax.
     */
    {
        g_autoptr(ClawtEvent) event = clawt_event_new("test.event",
                                                      "subject");

        g_test_expect_message(NULL, G_LOG_LEVEL_MESSAGE, "example: *");
        clawt_event_handler_handle(CLAWT_EVENT_HANDLER(plugin), event);
        g_test_assert_expected_messages();
    }
}

/* ── Doing something ─────────────────────────────────────────────── */

/* Events reach every handler plugin through the bus. */
static void
test_events_reach_plugins(void)
{
    g_autoptr(ClawtPluginManager) manager = clawt_plugin_manager_new(NULL);
    g_autoptr(ClawtEventBus) bus = clawt_event_bus_new(16);
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *tally = NULL;
    ClawtPlugin *plugin;

    path = g_build_filename(EXAMPLE_PLUGIN_DIR,
                            "libclawt-plugin-example.so", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_test_skip("the example plugin has not been built");
        return;
    }

    plugin = clawt_plugin_manager_load_file(manager, path, &error);
    g_assert_no_error(error);

    clawt_plugin_manager_attach_bus(manager, bus);

    clawt_event_bus_emit(bus, "agent.started", "chief");
    clawt_event_bus_emit(bus, "agent.started", "researcher");
    clawt_event_bus_emit(bus, "agent.stopped", "chief");

    tally = clawt_tool_provider_call(CLAWT_TOOL_PROVIDER(plugin), "chief",
                                     "example_fleet_tally", NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(strstr(tally, "agent.started: 2"));
    g_assert_nonnull(strstr(tally, "agent.stopped: 1"));
}

/* A plugin tool is offered to agents alongside the built-in ones. */
static void
test_plugin_tools_are_offered_to_agents(void)
{
    g_autoptr(ClawtPluginManager) manager = clawt_plugin_manager_new(NULL);
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtAgentManager) agents = NULL;
    g_autoptr(ClawtMcpTools) tools = NULL;
    g_autoptr(GPtrArray) providers = NULL;
    g_autoptr(JsonNode) listing = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *dir = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = NULL;
    JsonArray *listed;
    guint i;
    gboolean saw_plugin_tool = FALSE;

    path = g_build_filename(EXAMPLE_PLUGIN_DIR,
                            "libclawt-plugin-example.so", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_test_skip("the example plugin has not been built");
        return;
    }

    g_assert_nonnull(clawt_plugin_manager_load_file(manager, path, &error));

    dir = g_dir_make_tmp("clawt-plugin-XXXXXX", NULL);
    yaml = g_strdup_printf("daemon:\n  state_dir: \"%s\"\n"
                           "agents:\n  - id: chief\n", dir);

    config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    agents = clawt_agent_manager_new(config);
    clawt_agent_manager_load(agents, NULL);

    tools = clawt_mcp_tools_new(agents, NULL, NULL);
    providers = clawt_plugin_manager_tool_providers(manager);
    clawt_mcp_tools_set_tool_providers(tools, providers);

    g_assert_true(clawt_mcp_tools_is_permitted(tools, "chief",
                                               "example_fleet_tally"));

    listing = clawt_mcp_tools_list(tools, "chief");
    listed = json_object_get_array_member(json_node_get_object(listing),
                                          "tools");

    for (i = 0; i < json_array_get_length(listed); i++) {
        JsonObject *tool = json_array_get_object_element(listed, i);

        if (g_strcmp0(json_object_get_string_member(tool, "name"),
                      "example_fleet_tally") == 0) {
            saw_plugin_tool = TRUE;

            /* Its schema is generated, not hand-written. */
            g_assert_true(json_object_has_member(tool, "inputSchema"));
        }
    }

    g_assert_true(saw_plugin_tool);

    clawt_test_remove_tree(dir);
}

/*
 * A plugin tool is still subject to the agent's deny list.  A plugin must
 * not be able to hand an agent something its operator turned off.
 */
static void
test_plugin_tools_obey_the_deny_list(void)
{
    g_autoptr(ClawtPluginManager) manager = clawt_plugin_manager_new(NULL);
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtAgentManager) agents = NULL;
    g_autoptr(ClawtMcpTools) tools = NULL;
    g_autoptr(GPtrArray) providers = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *dir = NULL;
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;

    path = g_build_filename(EXAMPLE_PLUGIN_DIR,
                            "libclawt-plugin-example.so", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_test_skip("the example plugin has not been built");
        return;
    }

    g_assert_nonnull(clawt_plugin_manager_load_file(manager, path, &error));

    dir = g_dir_make_tmp("clawt-plugin-XXXXXX", NULL);
    yaml = g_strdup_printf(
        "daemon:\n  state_dir: \"%s\"\n"
        "agents:\n"
        "  - id: chief\n"
        "    tools:\n"
        "      deny:\n"
        "        - example_fleet_tally\n", dir);

    config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    agents = clawt_agent_manager_new(config);
    clawt_agent_manager_load(agents, NULL);

    tools = clawt_mcp_tools_new(agents, NULL, NULL);
    providers = clawt_plugin_manager_tool_providers(manager);
    clawt_mcp_tools_set_tool_providers(tools, providers);

    g_assert_false(clawt_mcp_tools_is_permitted(tools, "chief",
                                                "example_fleet_tally"));

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/plugin/loads-example", test_loads_the_example_plugin);
    g_test_add_func("/plugin/abi-mismatch", test_abi_mismatch_is_refused);
    g_test_add_func("/plugin/wrong-type", test_wrong_type_is_refused);
    g_test_add_func("/plugin/failing-activate",
                    test_failing_activate_is_isolated);
    g_test_add_func("/plugin/misnamed", test_a_misnamed_file_is_refused);
    g_test_add_func("/plugin/loaded-twice", test_loading_twice_is_refused);

    g_test_add_func("/plugin/disable-unknown",
                    test_config_can_disable_an_unknown_plugin);
    g_test_add_func("/plugin/enabled-false",
                    test_config_enabled_false_disables);
    g_test_add_func("/plugin/all-off",
                    test_plugins_can_be_turned_off_entirely);
    g_test_add_func("/plugin/settings", test_settings_reach_the_plugin);

    g_test_add_func("/plugin/events-reach-plugins", test_events_reach_plugins);
    g_test_add_func("/plugin/tools-offered",
                    test_plugin_tools_are_offered_to_agents);
    g_test_add_func("/plugin/tools-obey-deny",
                    test_plugin_tools_obey_the_deny_list);

    return g_test_run();
}
