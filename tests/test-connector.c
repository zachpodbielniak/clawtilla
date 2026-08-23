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

    return g_test_run();
}
