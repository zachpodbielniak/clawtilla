/*
 * test-connector.c - The catalogue, and what reaches the tool server
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The plan tests carry the weight here, and the assertion that matters
 * in them is a negative one: that the credential is in the environment
 * or a header and *nowhere in the argument vector*.  An argv is readable
 * from the process table by everything on the machine, so a credential
 * that reached it would be exposed to precisely what the broker exists
 * to keep it from -- and that is not visible by reading the code which
 * builds the argv. It is visible by looking for the value in the result.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <json-glib/json-glib.h>
#include <string.h>

#include "clawt-test-util.h"

/* ── The catalogue ───────────────────────────────────────────────── */

static void
test_the_builtin_catalogue_is_whole(void)
{
    gsize n = 0;
    const ClawtConnectorInfo *builtin = clawt_connector_catalog_builtin(&n);
    gsize i;

    g_assert_cmpuint(n, >, 0);

    for (i = 0; i < n; i++) {
        /*
         * A connector with no category is invisible in a list grouped by
         * category, and one with no name shows as a blank row.
         */
        g_assert_nonnull(builtin[i].id);
        g_assert_nonnull(builtin[i].name);
        g_assert_nonnull(builtin[i].category);

        /* An OAuth connector with no endpoints cannot start a flow. */
        if (builtin[i].auth == CLAWT_CONNECTOR_AUTH_DEVICE ||
            builtin[i].auth == CLAWT_CONNECTOR_AUTH_PKCE) {
            g_assert_nonnull(builtin[i].auth_url);
            g_assert_nonnull(builtin[i].token_url);
        }

        /* And one with no name for its credential cannot pass it on. */
        g_assert_nonnull(builtin[i].credential_name);
    }
}

static gchar *
write_overlay(const gchar *body)
{
    gchar *dir = g_dir_make_tmp("clawt-conn-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "10-extra.yaml", NULL);

    g_file_set_contents(path, body, -1, NULL);

    return dir;
}

static void
test_an_overlay_adds_a_connector(void)
{
    g_autofree gchar *dir = write_overlay(
        "connectors:\n"
        "  - id: acme\n"
        "    name: Acme\n"
        "    category: Testing\n"
        "    auth: api_key\n"
        "    credential_name: ACME_KEY\n");
    g_autoptr(GPtrArray) catalog = clawt_connector_catalog_load(dir, NULL);
    const ClawtConnectorInfo *found =
        clawt_connector_catalog_find(catalog, "acme");

    g_assert_nonnull(found);
    g_assert_cmpstr(found->name, ==, "Acme");
    g_assert_cmpint(found->auth, ==, CLAWT_CONNECTOR_AUTH_API_KEY);

    /* And the built-ins are still there beside it. */
    g_assert_nonnull(clawt_connector_catalog_find(catalog, "github"));

    clawt_test_remove_tree(dir);
}

/*
 * Replaced wholesale rather than merged.
 *
 * Half an override -- a new token endpoint against an old authorization
 * endpoint -- is a combination nobody wrote down, and it fails pointing
 * at neither file.
 */
static void
test_an_overlay_replaces_rather_than_merges(void)
{
    g_autofree gchar *dir = write_overlay(
        "connectors:\n"
        "  - id: github\n"
        "    name: Our GitHub Enterprise\n"
        "    category: Code forges\n"
        "    auth: device\n"
        "    auth_url: https://ghe.example.com/login/device/code\n"
        "    credential_name: GH_TOKEN\n");
    g_autoptr(GPtrArray) catalog = clawt_connector_catalog_load(dir, NULL);
    const ClawtConnectorInfo *found =
        clawt_connector_catalog_find(catalog, "github");

    g_assert_nonnull(found);
    g_assert_cmpstr(found->name, ==, "Our GitHub Enterprise");
    g_assert_cmpstr(found->auth_url, ==,
                    "https://ghe.example.com/login/device/code");

    /* The built-in's token_url must not survive underneath. */
    g_assert_null(found->token_url);

    clawt_test_remove_tree(dir);
}

/*
 * One unusable entry must not take the others away: this is read on the
 * path that starts the daemon.
 */
static void
test_a_broken_entry_does_not_take_the_file_with_it(void)
{
    g_autofree gchar *dir = write_overlay(
        "connectors:\n"
        "  - name: no id at all\n"
        "  - id: fine\n"
        "    name: Fine\n"
        "    category: Testing\n"
        "    credential_name: K\n");
    g_autoptr(GPtrArray) catalog = NULL;

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING, "*needs an id*");

    catalog = clawt_connector_catalog_load(dir, NULL);

    g_test_assert_expected_messages();

    g_assert_nonnull(clawt_connector_catalog_find(catalog, "fine"));

    clawt_test_remove_tree(dir);
}

/*
 * `credential_format` reaches a substitution and arrives from a file
 * somebody edited.  Anything other than one %s is refused at load, so a
 * typo cannot become a wrong credential -- or worse.
 */
static void
test_a_format_string_must_be_one_placeholder(void)
{
    g_autofree gchar *dir = write_overlay(
        "connectors:\n"
        "  - id: bad\n"
        "    name: Bad\n"
        "    category: Testing\n"
        "    credential_name: Authorization\n"
        "    credential_format: \"Bearer %d\"\n");
    g_autoptr(GPtrArray) catalog = NULL;

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*credential_format*");

    catalog = clawt_connector_catalog_load(dir, NULL);

    g_test_assert_expected_messages();
    g_assert_null(clawt_connector_catalog_find(catalog, "bad"));

    clawt_test_remove_tree(dir);
}

static void
test_a_credential_is_formatted(void)
{
    gsize n = 0;
    const ClawtConnectorInfo *builtin = clawt_connector_catalog_builtin(&n);
    const ClawtConnectorInfo *bearer = NULL;
    const ClawtConnectorInfo *plain = NULL;
    g_autofree gchar *decorated = NULL;
    g_autofree gchar *bare = NULL;
    gsize i;

    for (i = 0; i < n; i++) {
        if (g_strcmp0(builtin[i].id, "bearer") == 0)
            bearer = &builtin[i];
        else if (g_strcmp0(builtin[i].id, "api-key") == 0)
            plain = &builtin[i];
    }

    g_assert_nonnull(bearer);
    g_assert_nonnull(plain);

    decorated = clawt_connector_format_credential(bearer, "abc123");
    bare = clawt_connector_format_credential(plain, "abc123");

    /*
     * The difference between these two is invisible in a config file and
     * produces a 401 that names neither.
     */
    g_assert_cmpstr(decorated, ==, "Bearer abc123");
    g_assert_cmpstr(bare, ==, "abc123");
}

/* ── Endpoints for a service somebody hosts themselves ───────────── */

static void
test_a_hosted_service_joins_its_endpoints(void)
{
    g_autoptr(GPtrArray) catalog = clawt_connector_catalog_load(NULL, NULL);
    const ClawtConnectorInfo *gitlab =
        clawt_connector_catalog_find(catalog, "gitlab");
    const ClawtConnectorInfo *github =
        clawt_connector_catalog_find(catalog, "github");
    g_autofree gchar *flagship = NULL;
    g_autofree gchar *own = NULL;
    g_autofree gchar *bare_host = NULL;
    g_autofree gchar *trailing = NULL;
    g_autofree gchar *absolute = NULL;

    g_assert_nonnull(gitlab);
    g_assert_nonnull(github);

    /* No instance named: the provider's own host. */
    flagship = clawt_connector_resolve_url(gitlab, gitlab->token_url, NULL);
    g_assert_cmpstr(flagship, ==, "https://gitlab.com/oauth/token");

    /* One named: theirs. */
    own = clawt_connector_resolve_url(gitlab, gitlab->token_url,
                                      "https://gitlab.example.com");
    g_assert_cmpstr(own, ==, "https://gitlab.example.com/oauth/token");

    /*
     * A host with no scheme gets https rather than http. Somebody who
     * typed a bare hostname did not mean to send an OAuth exchange in
     * the clear.
     */
    bare_host = clawt_connector_resolve_url(gitlab, gitlab->token_url,
                                            "gitlab.example.com");
    g_assert_cmpstr(bare_host, ==, "https://gitlab.example.com/oauth/token");

    /* A trailing slash must not produce a doubled one. */
    trailing = clawt_connector_resolve_url(gitlab, gitlab->token_url,
                                           "https://gitlab.example.com/");
    g_assert_cmpstr(trailing, ==, "https://gitlab.example.com/oauth/token");

    /*
     * A connector that is not self-hostable ignores an instance
     * entirely: github.com is where GitHub is.
     */
    absolute = clawt_connector_resolve_url(github, github->token_url,
                                           "https://elsewhere.example.com");
    g_assert_cmpstr(absolute, ==,
                    "https://github.com/login/oauth/access_token");
}

/*
 * An instance name comes from a config file, so it may hold a slash --
 * which would put the credential outside the secrets directory, possibly
 * on top of something else.
 */
static void
test_a_token_path_stays_in_the_secrets_directory(void)
{
    g_autofree gchar *ordinary = clawt_connector_token_path("/s", "work");
    g_autofree gchar *awkward =
        clawt_connector_token_path("/s", "../../etc/shadow");

    g_assert_cmpstr(ordinary, ==, "/s/connector-work.json");
    g_assert_null(strstr(awkward, "/etc/"));
    g_assert_true(g_str_has_prefix(awkward, "/s/"));
}

/* ── What reaches the tool server ────────────────────────────────── */

typedef struct {
    gchar       *dir;
    ClawtConfig *config;
    GPtrArray   *catalog;
} Fixture;

static ClawtIntegrationBinding *
build_binding(Fixture *fixture, const gchar *body, const gchar *overlay,
              const ClawtConnectorInfo **out_info)
{
    g_autofree gchar *yaml = NULL;
    g_autofree gchar *overlay_dir = NULL;
    g_autoptr(GError) error = NULL;
    ClawtIntegrationConfig *instance;

    fixture->dir = g_dir_make_tmp("clawt-plan-XXXXXX", NULL);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "integrations:\n"
        "  - name: acct\n"
        "    type: connector\n"
        "%s",
        fixture->dir, body);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    overlay_dir = write_overlay(overlay);
    fixture->catalog = clawt_connector_catalog_load(overlay_dir, NULL);
    clawt_test_remove_tree(overlay_dir);

    instance = clawt_config_get_integration(fixture->config, "acct");
    g_assert_nonnull(instance);

    *out_info = clawt_connector_catalog_find(fixture->catalog, "acme");
    g_assert_nonnull(*out_info);

    return clawt_integration_binding_for_instance(
        instance, clawt_integration_find("connector"), NULL);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_pointer(&fixture->catalog, g_ptr_array_unref);
    g_clear_object(&fixture->config);
    clawt_test_remove_tree(fixture->dir);
    g_free(fixture->dir);
}

static gboolean
argv_contains(GStrv argv, const gchar *needle)
{
    gsize i;

    if (argv == NULL)
        return FALSE;

    for (i = 0; argv[i] != NULL; i++) {
        if (strstr(argv[i], needle) != NULL)
            return TRUE;
    }

    return FALSE;
}

/*
 * The test this file exists for.
 *
 * The credential must reach the server's environment and must not reach
 * its command line, because an argv is world-readable in the process
 * table and an environment is not.
 */
static void
test_the_credential_goes_in_the_environment_not_the_argv(void)
{
    Fixture fixture = { 0 };
    const ClawtConnectorInfo *info = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;
    g_autoptr(GError) error = NULL;

    binding = build_binding(&fixture,
                            "    provider: acme\n",
                            "connectors:\n"
                            "  - id: acme\n"
                            "    name: Acme\n"
                            "    category: Testing\n"
                            "    auth: api_key\n"
                            "    server_command: acme-mcp\n"
                            "    server_args: [stdio]\n"
                            "    placement: env\n"
                            "    credential_name: ACME_KEY\n",
                            &info);

    plan = clawt_connector_plan_new(info, binding, "sup3rsecret", &error);

    g_assert_no_error(error);
    g_assert_nonnull(plan);

    /* The server the catalogue named. */
    g_assert_cmpstr(plan->argv[0], ==, "acme-mcp");
    g_assert_cmpstr(plan->argv[1], ==, "stdio");

    /* The credential, where it belongs. */
    g_assert_nonnull(plan->envp);
    g_assert_cmpstr(plan->envp[0], ==, "ACME_KEY=sup3rsecret");

    /* And nowhere it does not. */
    g_assert_false(argv_contains(plan->argv, "sup3rsecret"));
    g_assert_null(plan->header_value);

    fixture_teardown(&fixture);
}

static void
test_a_header_connector_puts_it_in_a_header(void)
{
    Fixture fixture = { 0 };
    const ClawtConnectorInfo *info = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;
    g_autoptr(GError) error = NULL;

    binding = build_binding(&fixture,
                            "    provider: acme\n",
                            "connectors:\n"
                            "  - id: acme\n"
                            "    name: Acme\n"
                            "    category: Testing\n"
                            "    auth: api_key\n"
                            "    server_url: https://acme.test/mcp\n"
                            "    placement: header\n"
                            "    credential_name: Authorization\n"
                            "    credential_format: \"Bearer %s\"\n",
                            &info);

    plan = clawt_connector_plan_new(info, binding, "sup3rsecret", &error);

    g_assert_no_error(error);
    g_assert_cmpstr(plan->url, ==, "https://acme.test/mcp");
    g_assert_cmpstr(plan->header_name, ==, "Authorization");
    g_assert_cmpstr(plan->header_value, ==, "Bearer sup3rsecret");
    g_assert_null(plan->argv);
    g_assert_null(plan->envp);

    fixture_teardown(&fixture);
}

/* Somebody who wrote a command into their own config meant it. */
static void
test_the_integration_overrides_the_catalogue(void)
{
    Fixture fixture = { 0 };
    const ClawtConnectorInfo *info = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;
    g_autoptr(GError) error = NULL;

    binding = build_binding(&fixture,
                            "    provider: acme\n"
                            "    command: /opt/my-fork/acme-mcp\n"
                            "    credential_name: MY_KEY\n",
                            "connectors:\n"
                            "  - id: acme\n"
                            "    name: Acme\n"
                            "    category: Testing\n"
                            "    auth: api_key\n"
                            "    server_command: acme-mcp\n"
                            "    placement: env\n"
                            "    credential_name: ACME_KEY\n",
                            &info);

    plan = clawt_connector_plan_new(info, binding, "k", &error);

    g_assert_no_error(error);
    g_assert_cmpstr(plan->argv[0], ==, "/opt/my-fork/acme-mcp");
    g_assert_cmpstr(plan->envp[0], ==, "MY_KEY=k");

    fixture_teardown(&fixture);
}

/*
 * No list means every tool; an empty list would mean none.  Getting this
 * backwards hands the agent an empty tool list, which looks exactly like
 * a server that failed to start.
 */
static void
test_no_tool_list_is_not_an_empty_tool_list(void)
{
    Fixture fixture = { 0 };
    const ClawtConnectorInfo *info = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;
    const gchar *overlay =
        "connectors:\n"
        "  - id: acme\n"
        "    name: Acme\n"
        "    category: Testing\n"
        "    auth: api_key\n"
        "    server_command: acme-mcp\n"
        "    placement: env\n"
        "    credential_name: ACME_KEY\n";

    binding = build_binding(&fixture, "    provider: acme\n", overlay, &info);

    plan = clawt_connector_plan_new(info, binding, "k", NULL);

    /* NULL here is read by the relay as "do not filter at all". */
    g_assert_null(plan->permitted);

    fixture_teardown(&fixture);
}

static void
test_a_tool_list_is_carried_through(void)
{
    Fixture fixture = { 0 };
    const ClawtConnectorInfo *info = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;

    binding = build_binding(&fixture,
                            "    provider: acme\n"
                            "    tools: [read_thing]\n",
                            "connectors:\n"
                            "  - id: acme\n"
                            "    name: Acme\n"
                            "    category: Testing\n"
                            "    auth: api_key\n"
                            "    server_command: acme-mcp\n"
                            "    placement: env\n"
                            "    credential_name: ACME_KEY\n",
                            &info);

    plan = clawt_connector_plan_new(info, binding, "k", NULL);

    g_assert_nonnull(plan->permitted);
    g_assert_cmpstr(plan->permitted[0], ==, "read_thing");
    g_assert_null(plan->permitted[1]);

    fixture_teardown(&fixture);
}

/*
 * A connector that authenticates perfectly and has no server hands the
 * agent nothing, so it is refused with the field that would fix it.
 */
static void
test_a_connector_with_no_server_is_refused(void)
{
    Fixture fixture = { 0 };
    const ClawtConnectorInfo *info = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(GError) error = NULL;
    ClawtConnectorPlan *plan;

    binding = build_binding(&fixture,
                            "    provider: acme\n",
                            "connectors:\n"
                            "  - id: acme\n"
                            "    name: Acme\n"
                            "    category: Testing\n"
                            "    auth: api_key\n"
                            "    placement: env\n"
                            "    credential_name: ACME_KEY\n",
                            &info);

    plan = clawt_connector_plan_new(info, binding, "k", &error);

    g_assert_null(plan);
    g_assert_nonnull(error);
    g_assert_nonnull(g_strstr_len(error->message, -1, "command or url"));

    fixture_teardown(&fixture);
}

/* ── The type itself ─────────────────────────────────────────────── */

static void
test_connector_is_a_tools_integration(void)
{
    const ClawtIntegrationInfo *info = clawt_integration_find("connector");

    g_assert_nonnull(info);
    g_assert_cmpint(info->kind, ==, CLAWT_INTEGRATION_KIND_TOOLS);

    /*
     * Several connectors on one agent is ordinary -- a repository host
     * and a calendar -- and sharing one across the fleet is the point of
     * it, so neither restriction applies.
     */
    g_assert_false(info->one_per_agent);
    g_assert_false(info->one_per_fleet);
}

/* ── The MCP registry importer ───────────────────────────────────── */

static JsonParser *
load_fixture_json(const gchar *name)
{
    g_autofree gchar *path = g_build_filename(CLAWT_TEST_FIXTURES, name, NULL);
    JsonParser *parser = json_parser_new();
    g_autoptr(GError) error = NULL;

    if (!json_parser_load_from_file(parser, path, &error))
        g_error("fixture %s did not parse: %s", name, error->message);

    return parser;
}

/*
 * A page shaped the way the real registry answers on 2026-08-29: one
 * server naming an npm package with a secret environment variable, one
 * naming an HTTP remote directly, and a `metadata.nextCursor` to follow.
 */
static void
test_a_page_parses_into_catalog_entries(void)
{
    g_autoptr(JsonParser) parser = load_fixture_json("mcp-registry-page.json");
    g_autoptr(GPtrArray) entries =
        g_ptr_array_new_with_free_func((GDestroyNotify)
                                       clawt_connector_info_free);
    g_autofree gchar *next_cursor = NULL;
    g_autoptr(GError) error = NULL;
    const ClawtConnectorInfo *package_entry;
    const ClawtConnectorInfo *remote_entry;
    gboolean ok;

    ok = clawt_connector_registry_parse_page(json_parser_get_root(parser),
                                             entries, &next_cursor, &error);

    g_assert_true(ok);
    g_assert_no_error(error);
    g_assert_cmpuint(entries->len, ==, 2);
    g_assert_cmpstr(next_cursor, ==, "io.github.acme/widget-remote:1.0.0");

    /* Prefixed so it can never collide with a curated id. */
    package_entry = clawt_connector_catalog_find(
        entries, "registry:io.github.acme/widget-tools");
    g_assert_nonnull(package_entry);
    g_assert_cmpstr(package_entry->name, ==, "Widget Tools");
    g_assert_cmpstr(package_entry->summary, ==,
                    "Manage widgets from an agent session.");
    g_assert_cmpstr(package_entry->category, ==, "MCP Registry");

    /* The secret environment variable decided the auth kind and the
     * credential name; the runtime hint decided the command. */
    g_assert_cmpint(package_entry->auth, ==, CLAWT_CONNECTOR_AUTH_API_KEY);
    g_assert_cmpstr(package_entry->credential_name, ==, "ACME_API_TOKEN");
    g_assert_cmpstr(package_entry->server_command, ==, "npx");
    g_assert_cmpstr(package_entry->server_args[0], ==, "-y");
    g_assert_cmpstr(package_entry->server_args[1], ==, "@acme/widget-mcp");
    g_assert_cmpstr(package_entry->server_args[2], ==, "--read-only");
    g_assert_null(package_entry->server_args[3]);

    remote_entry = clawt_connector_catalog_find(
        entries, "registry:io.github.acme/widget-remote");
    g_assert_nonnull(remote_entry);
    /* No `title`, so the full name is what is shown. */
    g_assert_cmpstr(remote_entry->name, ==, "io.github.acme/widget-remote");
    g_assert_cmpstr(remote_entry->server_url, ==,
                    "https://widgets.example/mcp");
    g_assert_null(remote_entry->server_command);
    g_assert_cmpint(remote_entry->auth, ==, CLAWT_CONNECTOR_AUTH_NONE);
}

/*
 * One entry with no name and one naming neither a package nor a remote,
 * either side of a usable one -- proving a bad listing costs only
 * itself, the same rule an overlay file's own broken entry follows.
 */
static void
test_a_malformed_entry_is_skipped_with_a_warning(void)
{
    g_autoptr(JsonParser) parser =
        load_fixture_json("mcp-registry-page-malformed.json");
    g_autoptr(GPtrArray) entries =
        g_ptr_array_new_with_free_func((GDestroyNotify)
                                       clawt_connector_info_free);
    g_autofree gchar *next_cursor = NULL;
    gboolean ok;

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*entry 0 skipped*needs a name*");
    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*entry 2 skipped*neither a package nor a "
                          "remote*");

    ok = clawt_connector_registry_parse_page(json_parser_get_root(parser),
                                             entries, &next_cursor, NULL);

    g_test_assert_expected_messages();

    g_assert_true(ok);
    g_assert_cmpuint(entries->len, ==, 1);
    g_assert_cmpstr(
        ((ClawtConnectorInfo *)g_ptr_array_index(entries, 0))->id, ==,
        "registry:io.github.acme/fine");
    g_assert_null(next_cursor);
}

/*
 * A response that is not shaped like a response at all -- as opposed to
 * one bad entry inside an otherwise usable page -- is the one failure
 * that takes the whole page down.
 */
static void
test_a_page_with_no_servers_list_fails(void)
{
    g_autoptr(JsonParser) parser =
        load_fixture_json("mcp-registry-page-no-servers.json");
    g_autoptr(GPtrArray) entries =
        g_ptr_array_new_with_free_func((GDestroyNotify)
                                       clawt_connector_info_free);
    g_autoptr(GError) error = NULL;
    gboolean ok;

    ok = clawt_connector_registry_parse_page(json_parser_get_root(parser),
                                             entries, NULL, &error);

    g_assert_false(ok);
    g_assert_nonnull(error);
    g_assert_cmpuint(entries->len, ==, 0);
}

static void
test_updated_since_is_null_for_a_fresh_cache(void)
{
    g_autofree gchar *since = clawt_connector_registry_updated_since_for(0);

    g_assert_null(since);
}

/*
 * The registry takes RFC 3339, the cache keeps Unix seconds -- this is
 * the one conversion that makes the second refresh incremental instead
 * of a second full walk.
 */
static void
test_updated_since_formats_rfc3339(void)
{
    g_autofree gchar *since =
        clawt_connector_registry_updated_since_for(1700000000);
    g_autoptr(GDateTime) parsed = NULL;

    g_assert_nonnull(since);
    g_assert_nonnull(strchr(since, 'T'));

    parsed = g_date_time_new_from_iso8601(since, NULL);
    g_assert_nonnull(parsed);
    g_assert_cmpint(g_date_time_to_unix(parsed), ==, 1700000000);
}

static void
test_a_missing_cache_is_empty_not_an_error(void)
{
    gint64 fetched_at = -1;
    g_autoptr(GPtrArray) loaded = clawt_connector_registry_cache_load(
        "/no/such/path/clawtilla-test-never-writes-here.json", &fetched_at);

    g_assert_cmpuint(loaded->len, ==, 0);
    g_assert_cmpint(fetched_at, ==, 0);
}

static void
test_the_cache_round_trips(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-registry-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "cache.json", NULL);
    g_autoptr(GPtrArray) entries =
        g_ptr_array_new_with_free_func((GDestroyNotify)
                                       clawt_connector_info_free);
    ClawtConnectorInfo *entry = g_new0(ClawtConnectorInfo, 1);
    static const gchar *const args[] = { "-y", "@acme/widget-mcp", NULL };
    static const gchar *const tools[] = { "list_widgets", NULL };
    g_autoptr(GPtrArray) loaded = NULL;
    gint64 fetched_at = 0;
    const ClawtConnectorInfo *round_tripped;
    g_autoptr(GError) error = NULL;

    entry->id = g_strdup("registry:io.github.acme/widget-tools");
    entry->name = g_strdup("Widget Tools");
    entry->category = g_strdup("MCP Registry");
    entry->auth = CLAWT_CONNECTOR_AUTH_API_KEY;
    entry->server_command = g_strdup("npx");
    entry->server_args = (const gchar *const *)g_strdupv((GStrv)args);
    entry->placement = CLAWT_CREDENTIAL_PLACEMENT_ENV;
    entry->credential_name = g_strdup("ACME_API_TOKEN");
    entry->known_tools = (const gchar *const *)g_strdupv((GStrv)tools);
    g_ptr_array_add(entries, entry);

    g_assert_true(clawt_connector_registry_cache_save(path, entries,
                                                       1700000000, &error));
    g_assert_no_error(error);

    loaded = clawt_connector_registry_cache_load(path, &fetched_at);

    g_assert_cmpint(fetched_at, ==, 1700000000);
    g_assert_cmpuint(loaded->len, ==, 1);

    round_tripped = g_ptr_array_index(loaded, 0);
    g_assert_cmpstr(round_tripped->id, ==, entry->id);
    g_assert_cmpstr(round_tripped->name, ==, entry->name);
    g_assert_cmpint(round_tripped->auth, ==, CLAWT_CONNECTOR_AUTH_API_KEY);
    g_assert_cmpstr(round_tripped->server_command, ==, "npx");
    g_assert_cmpstr(round_tripped->server_args[0], ==, "-y");
    g_assert_cmpstr(round_tripped->server_args[1], ==, "@acme/widget-mcp");
    g_assert_null(round_tripped->server_args[2]);
    g_assert_cmpstr(round_tripped->credential_name, ==, "ACME_API_TOKEN");
    g_assert_cmpstr(round_tripped->known_tools[0], ==, "list_widgets");

    clawt_test_remove_tree(dir);
}

/*
 * The built-in table and a person's own overlay are both a choice
 * somebody already made; an imported id must fill a gap and never touch
 * either -- even when it happens to collide, which a reverse-DNS name
 * should never do but this proves the rule rather than trusting the
 * naming convention alone.
 */
static void
test_merge_only_fills_gaps(void)
{
    g_autoptr(GPtrArray) catalog = clawt_connector_catalog_load(NULL, NULL);
    g_autoptr(GPtrArray) imported =
        g_ptr_array_new_with_free_func((GDestroyNotify)
                                       clawt_connector_info_free);
    ClawtConnectorInfo *collision = g_new0(ClawtConnectorInfo, 1);
    ClawtConnectorInfo *fresh = g_new0(ClawtConnectorInfo, 1);
    const ClawtConnectorInfo *github_before;
    const ClawtConnectorInfo *github_after;
    const ClawtConnectorInfo *added;
    guint github_count = 0;
    guint i;

    github_before = clawt_connector_catalog_find(catalog, "github");
    g_assert_nonnull(github_before);

    collision->id = g_strdup("github");
    collision->name = g_strdup("Somebody Else's GitHub Entirely");
    collision->category = g_strdup("MCP Registry");
    g_ptr_array_add(imported, collision);

    fresh->id = g_strdup("registry:io.github.acme/widget-tools");
    fresh->name = g_strdup("Widget Tools");
    fresh->category = g_strdup("MCP Registry");
    g_ptr_array_add(imported, fresh);

    clawt_connector_catalog_merge_registry(catalog, imported);

    github_after = clawt_connector_catalog_find(catalog, "github");
    g_assert_cmpstr(github_after->name, ==, github_before->name);

    /*
     * clawt_connector_catalog_find() answers the first match, so a
     * collision appended after the original would pass the check above
     * by accident -- counting every "github" is what actually proves
     * the entry was skipped rather than merely shadowed.
     */
    for (i = 0; i < catalog->len; i++) {
        const ClawtConnectorInfo *entry = g_ptr_array_index(catalog, i);

        if (g_strcmp0(entry->id, "github") == 0)
            github_count++;
    }

    g_assert_cmpuint(github_count, ==, 1);

    added = clawt_connector_catalog_find(
        catalog, "registry:io.github.acme/widget-tools");
    g_assert_nonnull(added);
    g_assert_cmpstr(added->name, ==, "Widget Tools");
}

/* ── An unknown value in a hand-written entry ────────────────────── */

/*
 * An absent `auth:` is CLAWT_CONNECTOR_AUTH_NONE, which is legitimate --
 * plenty of servers want no credential at all.  A *present* one that
 * fails to parse must not fall back to the same value: that would tell
 * an operator their typo'd "device" connector needs no authorization,
 * which is worse than the typo.
 */
static void
test_an_unknown_auth_kind_is_refused(void)
{
    g_autofree gchar *dir = write_overlay(
        "connectors:\n"
        "  - id: bogus-auth\n"
        "    name: Bogus\n"
        "    category: Testing\n"
        "    auth: telepathy\n"
        "    credential_name: X\n");
    g_autoptr(GPtrArray) catalog = NULL;

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*telepathy*not a known auth kind*");

    catalog = clawt_connector_catalog_load(dir, NULL);

    g_test_assert_expected_messages();
    g_assert_null(clawt_connector_catalog_find(catalog, "bogus-auth"));

    clawt_test_remove_tree(dir);
}

/*
 * A pack that declares what its server offers lets a typo in an
 * integration's own `tools:` be caught here instead of narrowing an
 * agent down to nothing with no way to say why.
 */
static void
test_a_tool_the_server_does_not_offer_is_warned_about(void)
{
    Fixture fixture = { 0 };
    const ClawtConnectorInfo *info = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;

    binding = build_binding(&fixture,
                            "    provider: acme\n"
                            "    tools: [list_widgets, delete_everything]\n",
                            "connectors:\n"
                            "  - id: acme\n"
                            "    name: Acme\n"
                            "    category: Testing\n"
                            "    auth: api_key\n"
                            "    server_command: acme-mcp\n"
                            "    placement: env\n"
                            "    credential_name: ACME_KEY\n"
                            "    known_tools: [list_widgets, rename_widget]\n",
                            &info);

    g_assert_nonnull(info->known_tools);

    g_test_expect_message(G_LOG_DOMAIN, G_LOG_LEVEL_WARNING,
                          "*delete_everything*not known to offer*");

    plan = clawt_connector_plan_new(info, binding, "k", NULL);

    g_test_assert_expected_messages();
    g_assert_nonnull(plan->permitted);

    fixture_teardown(&fixture);
}

/*
 * A missing binary should say where it looked, not fail later as a bare
 * "No such file or directory" from a subprocess that names nothing an
 * operator can act on.
 */
static void
test_a_command_that_resolves_nowhere_names_all_three_locations(void)
{
    g_autoptr(GError) error = NULL;
    gchar *resolved = clawt_connector_resolve_command(
        "this-binary-does-not-exist-anywhere-clawtilla-2026", &error);

    g_assert_null(resolved);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "beside clawtillad"));
    g_assert_nonnull(strstr(error->message, "mcp-servers"));
    g_assert_nonnull(strstr(error->message, "PATH"));
}

/* ── The shipped FOSS packs ───────────────────────────────────────── */

static gboolean
looks_absolute(const gchar *value)
{
    return value != NULL &&
           (g_str_has_prefix(value, "http://") ||
            g_str_has_prefix(value, "https://"));
}

/*
 * A self-hostable pack's fields are paths, joined onto whichever
 * instance the operator configures -- never a mixture, because a mixture
 * is how a connector quietly authorises against somebody else's server.
 * Walks every shipped pack rather than naming one, so a future addition
 * is covered the moment it exists.
 */
static void
test_shipped_self_hosted_packs_leave_no_url_field_absolute(void)
{
    g_autofree gchar *dir = g_build_filename(CLAWT_TEST_SRCDIR, "data",
                                             "connectors.d", NULL);
    g_autoptr(GPtrArray) catalog = clawt_connector_catalog_load(dir, NULL);
    guint i;
    guint checked = 0;

    for (i = 0; i < catalog->len; i++) {
        const ClawtConnectorInfo *info = g_ptr_array_index(catalog, i);

        if (info->default_instance == NULL)
            continue;

        checked++;

        g_assert_false(looks_absolute(info->auth_url));
        g_assert_false(looks_absolute(info->token_url));
        g_assert_false(looks_absolute(info->revoke_url));
        g_assert_false(looks_absolute(info->server_url));
    }

    /* Reaches the rule: if nothing shipped were self-hostable this would
     * pass by finding nothing to check. */
    g_assert_cmpuint(checked, >, 0);
}

/*
 * The one shipped pack that both overrides a built-in entry *and* adds a
 * server -- proving the override replicates every field the built-in
 * table sets (auth_url, token_url, revoke_url) and that the new
 * server_url joins the same way.
 */
static void
test_the_gitlab_pack_joins_every_field_onto_the_instance(void)
{
    g_autofree gchar *dir = g_build_filename(CLAWT_TEST_SRCDIR, "data",
                                             "connectors.d", NULL);
    g_autoptr(GPtrArray) catalog = clawt_connector_catalog_load(dir, NULL);
    const ClawtConnectorInfo *gitlab =
        clawt_connector_catalog_find(catalog, "gitlab");
    g_autofree gchar *auth = NULL;
    g_autofree gchar *token = NULL;
    g_autofree gchar *revoke = NULL;
    g_autofree gchar *server = NULL;

    g_assert_nonnull(gitlab);
    g_assert_nonnull(gitlab->server_url);

    auth = clawt_connector_resolve_url(gitlab, gitlab->auth_url,
                                       "https://gitlab.example.com");
    token = clawt_connector_resolve_url(gitlab, gitlab->token_url,
                                        "https://gitlab.example.com");
    revoke = clawt_connector_resolve_url(gitlab, gitlab->revoke_url,
                                         "https://gitlab.example.com");
    server = clawt_connector_resolve_url(gitlab, gitlab->server_url,
                                         "https://gitlab.example.com");

    g_assert_cmpstr(auth, ==,
                    "https://gitlab.example.com/oauth/authorize_device");
    g_assert_cmpstr(token, ==, "https://gitlab.example.com/oauth/token");
    g_assert_cmpstr(revoke, ==, "https://gitlab.example.com/oauth/revoke");
    g_assert_cmpstr(server, ==, "https://gitlab.example.com/api/v4/mcp");
}

/*
 * The credential test this file exists for, run against a real shipped
 * pack rather than only the synthetic "acme" fixture: GitLab's own MCP
 * server is HTTP, so the credential belongs in a header, and it must
 * reach exactly that header and nothing else -- no argv, since there is
 * none for a URL-based server, and no envp either.
 */
static void
test_the_gitlab_pack_puts_its_credential_in_a_header_not_argv(void)
{
    g_autofree gchar *packs_dir = g_build_filename(CLAWT_TEST_SRCDIR, "data",
                                                   "connectors.d", NULL);
    g_autoptr(GPtrArray) catalog = clawt_connector_catalog_load(packs_dir,
                                                                NULL);
    const ClawtConnectorInfo *gitlab =
        clawt_connector_catalog_find(catalog, "gitlab");
    g_autofree gchar *dir = g_dir_make_tmp("clawt-gitlab-XXXXXX", NULL);
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    ClawtIntegrationConfig *instance;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtConnectorPlan) plan = NULL;

    g_assert_nonnull(gitlab);

    yaml = g_strdup_printf(
        "daemon:\n"
        "  state_dir: \"%s/state\"\n"
        "integrations:\n"
        "  - name: work\n"
        "    type: connector\n"
        "    provider: gitlab\n"
        "    instance: https://gitlab.example.com\n",
        dir);

    config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    instance = clawt_config_get_integration(config, "work");
    g_assert_nonnull(instance);

    binding = clawt_integration_binding_for_instance(
        instance, clawt_integration_find("connector"), NULL);

    plan = clawt_connector_plan_new(gitlab, binding, "sup3rsecret", &error);

    g_assert_no_error(error);
    g_assert_null(plan->argv);
    g_assert_cmpstr(plan->url, ==, "https://gitlab.example.com/api/v4/mcp");
    g_assert_cmpstr(plan->header_name, ==, "Authorization");
    g_assert_cmpstr(plan->header_value, ==, "Bearer sup3rsecret");
    g_assert_false(argv_contains(plan->argv, "sup3rsecret"));
    g_assert_null(plan->envp);

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/connector/builtin-catalogue",
                    test_the_builtin_catalogue_is_whole);
    g_test_add_func("/connector/overlay-adds", test_an_overlay_adds_a_connector);
    g_test_add_func("/connector/overlay-replaces",
                    test_an_overlay_replaces_rather_than_merges);
    g_test_add_func("/connector/overlay-broken-entry",
                    test_a_broken_entry_does_not_take_the_file_with_it);
    g_test_add_func("/connector/format-string-safety",
                    test_a_format_string_must_be_one_placeholder);
    g_test_add_func("/connector/credential-format",
                    test_a_credential_is_formatted);
    g_test_add_func("/connector/self-hosted-endpoints",
                    test_a_hosted_service_joins_its_endpoints);
    g_test_add_func("/connector/token-path",
                    test_a_token_path_stays_in_the_secrets_directory);

    g_test_add_func("/connector/credential-not-in-argv",
                    test_the_credential_goes_in_the_environment_not_the_argv);
    g_test_add_func("/connector/credential-in-header",
                    test_a_header_connector_puts_it_in_a_header);
    g_test_add_func("/connector/integration-overrides-catalogue",
                    test_the_integration_overrides_the_catalogue);
    g_test_add_func("/connector/no-tool-list",
                    test_no_tool_list_is_not_an_empty_tool_list);
    g_test_add_func("/connector/tool-list",
                    test_a_tool_list_is_carried_through);
    g_test_add_func("/connector/no-server",
                    test_a_connector_with_no_server_is_refused);
    g_test_add_func("/connector/is-a-tools-integration",
                    test_connector_is_a_tools_integration);

    g_test_add_func("/connector/registry-page-parses",
                    test_a_page_parses_into_catalog_entries);
    g_test_add_func("/connector/registry-malformed-entry-warned",
                    test_a_malformed_entry_is_skipped_with_a_warning);
    g_test_add_func("/connector/registry-no-servers-list-fails",
                    test_a_page_with_no_servers_list_fails);
    g_test_add_func("/connector/registry-updated-since-null-when-fresh",
                    test_updated_since_is_null_for_a_fresh_cache);
    g_test_add_func("/connector/registry-updated-since-rfc3339",
                    test_updated_since_formats_rfc3339);
    g_test_add_func("/connector/registry-missing-cache-is-empty",
                    test_a_missing_cache_is_empty_not_an_error);
    g_test_add_func("/connector/registry-cache-round-trips",
                    test_the_cache_round_trips);
    g_test_add_func("/connector/registry-merge-fills-gaps-only",
                    test_merge_only_fills_gaps);

    g_test_add_func("/connector/unknown-auth-kind-refused",
                    test_an_unknown_auth_kind_is_refused);
    g_test_add_func("/connector/unknown-tool-warned-about",
                    test_a_tool_the_server_does_not_offer_is_warned_about);
    g_test_add_func("/connector/command-resolves-nowhere",
                    test_a_command_that_resolves_nowhere_names_all_three_locations);

    g_test_add_func("/connector/shipped-packs-no-absolute-fields",
                    test_shipped_self_hosted_packs_leave_no_url_field_absolute);
    g_test_add_func("/connector/shipped-gitlab-pack-joins-fields",
                    test_the_gitlab_pack_joins_every_field_onto_the_instance);
    g_test_add_func("/connector/shipped-gitlab-pack-credential-in-header",
                    test_the_gitlab_pack_puts_its_credential_in_a_header_not_argv);

    return g_test_run();
}
