/*
 * test-provider-usage.c - Each login's plan allowance, read and shown
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The library half turns ai-glib's usage reports into windows and
 * phrases them; the fixtures are the shapes ai-glib's own documentation
 * gives (docs/cli-reporting.org), nulls included, because a null read
 * as zero is the bug the report's schema exists to prevent.  The daemon
 * half drives usage.providers with a query hook in place of the CLIs:
 * make test must not spawn a provider, and a real one would answer with
 * whatever this machine is signed in as.
 */

#include "clawtilla.h"
#include <glib/gstdio.h>
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <string.h>

/* An ai-glib usage document with every unknown field null, as it sends them. */
static JsonNode *
report_node(const gchar *entries_json, const gchar *extra_json)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *text = g_strdup_printf(
        "{\"schema_version\":1,\"provider\":\"claude-code\",\"kind\":\"usage\","
        "\"source\":\"get_usage\",\"collected_at\":\"2026-11-21T19:00:00Z\","
        "\"availability\":\"available\",\"detail\":null,\"plan\":\"max\","
        "\"source_updated_at\":null,\"truncated\":false%s%s,\"entries\":[%s]}",
        extra_json != NULL ? "," : "", extra_json != NULL ? extra_json : "",
        entries_json);

    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    return json_node_copy(json_parser_get_root(parser));
}

/* One entry, nulls everywhere except what @fields sets. */
static gchar *
entry(const gchar *label, const gchar *unit, const gchar *fields)
{
    return g_strdup_printf(
        "{\"label\":\"%s\",\"unit\":\"%s\",\"used\":null,\"limit\":null,"
        "\"remaining\":null,\"used_percent\":null,\"remaining_percent\":null,"
        "\"percent\":null,\"start_at\":null,\"end_at\":null,\"reset_at\":null,"
        "\"reset_text\":null,\"input_tokens\":null,\"output_tokens\":null,"
        "\"total_tokens\":null,\"cost_usd\":null,\"window_minutes\":null,"
        "\"included_cost_usd\":null,\"on_demand_cost_usd\":null,\"model\":null%s%s}",
        label, unit, fields != NULL ? "," : "", fields != NULL ? fields : "");
}

/*
 * json-glib keeps the first of a duplicated member, so a fixture field
 * written after the nulls has to replace them, not follow them.
 */
static gchar *
entry_with(const gchar *label, const gchar *unit, const gchar *const *pairs)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *base = entry(label, unit, NULL);
    JsonObject *object;
    guint i;

    g_assert_true(json_parser_load_from_data(parser, base, -1, NULL));
    object = json_node_get_object(json_parser_get_root(parser));
    for (i = 0; pairs[i] != NULL; i += 2) {
        g_autoptr(JsonParser) value = json_parser_new();

        g_assert_true(json_parser_load_from_data(value, pairs[i + 1], -1, NULL));
        json_object_set_member(object, pairs[i],
                               json_node_copy(json_parser_get_root(value)));
    }
    return json_to_string(json_parser_get_root(parser), FALSE);
}

static JsonObject *
window_at(JsonNode *login, guint index)
{
    JsonArray *windows = json_object_get_array_member(json_node_get_object(login),
                                                      "windows");

    g_assert_cmpuint(json_array_get_length(windows), >, index);
    return json_array_get_object_element(windows, index);
}

/* Used becomes left; a null stays absent; the instant is Unix seconds. */
static void
test_claude_windows(void)
{
    const gchar *session[] = { "used_percent", "38", "window_minutes", "300",
                               "reset_at", "\"2026-11-21T22:00:00Z\"", NULL };
    const gchar *weekly[] = { "used_percent", "92", "window_minutes", "10080", NULL };
    g_autofree gchar *a = entry_with("Session (5h)", "percent", session);
    g_autofree gchar *b = entry_with("Weekly", "percent", weekly);
    g_autofree gchar *entries = g_strdup_printf("%s,%s", a, b);
    g_autoptr(JsonNode) report = report_node(entries, NULL);
    g_autoptr(JsonNode) login = clawt_provider_usage_normalize(report);
    g_autoptr(GDateTime) reset = g_date_time_new_from_iso8601("2026-11-21T22:00:00Z", NULL);
    gint64 reset_at = g_date_time_to_unix(reset);
    JsonObject *first;
    JsonObject *second;
    gdouble left = -1.0;
    g_autofree gchar *value = NULL;
    g_autofree gchar *resets = NULL;

    g_assert_nonnull(login);
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(login), "plan"),
                    ==, "max");
    g_assert_false(json_object_get_boolean_member(json_node_get_object(login), "partial"));

    first = window_at(login, 0);
    g_assert_true(clawt_usage_window_remaining(first, &left));
    g_assert_cmpfloat_with_epsilon(left, 62.0, 0.001);
    g_assert_cmpint(json_object_get_int_member(first, "reset_at"), ==, reset_at);
    g_assert_cmpint(json_object_get_int_member(first, "window_minutes"), ==, 300);
    /* unknown is not zero: not there at all */
    g_assert_false(json_object_has_member(first, "used"));
    g_assert_false(json_object_has_member(first, "cost_usd"));
    g_assert_false(json_object_has_member(first, "reset_text"));

    value = clawt_usage_window_value_label(first);
    g_assert_cmpstr(value, ==, "62% left");
    resets = clawt_usage_window_reset_label(first, reset_at - (2 * 3600 + 41 * 60));
    g_assert_cmpstr(resets, ==, "resets in 2h 41m");
    g_assert_cmpint(clawt_usage_window_level(first), ==, CLAWT_ALLOWANCE_PLENTY);

    second = window_at(login, 1);
    g_assert_cmpint(clawt_usage_window_level(second), ==, CLAWT_ALLOWANCE_CRITICAL);
    g_assert_null(clawt_usage_window_reset_label(second, reset_at));
    /* the login is as worrying as its worst window */
    g_assert_cmpint(clawt_usage_login_level(json_node_get_object(login)), ==,
                    CLAWT_ALLOWANCE_CRITICAL);
}

/* A percentage with no direction is shown as given and never coloured. */
static void
test_direction_unknown(void)
{
    const gchar *panel[] = { "percent", "40", "reset_text", "\"Tue 9am\"", NULL };
    g_autofree gchar *e = entry_with("Quota", "percent", panel);
    g_autoptr(JsonNode) report = report_node(e, "\"x\":1");
    g_autoptr(JsonNode) login = clawt_provider_usage_normalize(report);
    JsonObject *window = window_at(login, 0);
    g_autofree gchar *value = clawt_usage_window_value_label(window);
    g_autofree gchar *resets = clawt_usage_window_reset_label(window, 0);

    g_assert_false(clawt_usage_window_remaining(window, NULL));
    g_assert_cmpint(clawt_usage_window_level(window), ==, CLAWT_ALLOWANCE_UNKNOWN);
    g_assert_cmpstr(clawt_allowance_level_tone(clawt_usage_window_level(window)),
                    ==, "neutral");
    g_assert_cmpstr(value, ==, "40% (direction not reported)");
    g_assert_cmpstr(resets, ==, "resets Tue 9am");
}

/* Money reads as money; a date with no time is the source's text, not midnight. */
static void
test_money_and_dates(void)
{
    const gchar *money[] = { "used", "3.2", "limit", "10", "reset_at", "\"2026-12-01\"",
                             NULL };
    const gchar *tokens[] = { "used", "1234567", NULL };
    g_autofree gchar *a = entry_with("Monthly credit", "USD", money);
    g_autofree gchar *b = entry_with("Today", "tokens", tokens);
    g_autofree gchar *entries = g_strdup_printf("%s,%s", a, b);
    g_autoptr(JsonNode) report = report_node(entries, NULL);
    g_autoptr(JsonNode) login = clawt_provider_usage_normalize(report);
    JsonObject *credit = window_at(login, 0);
    JsonObject *today = window_at(login, 1);
    g_autofree gchar *credit_value = clawt_usage_window_value_label(credit);
    g_autofree gchar *credit_reset = clawt_usage_window_reset_label(credit, 0);
    g_autofree gchar *today_value = clawt_usage_window_value_label(today);
    gdouble left = 0.0;

    g_assert_true(clawt_usage_window_remaining(credit, &left));
    g_assert_cmpfloat_with_epsilon(left, 68.0, 0.001);
    g_assert_cmpstr(credit_value, ==, "$3.20 of $10.00 used");
    g_assert_false(json_object_has_member(credit, "reset_at"));
    g_assert_cmpstr(credit_reset, ==, "resets 2026-12-01");
    g_assert_cmpstr(today_value, ==, "1,234,567 tokens used");
    g_assert_cmpint(clawt_usage_window_level(today), ==, CLAWT_ALLOWANCE_UNKNOWN);
}

/* A later schema, or a history report, is refused rather than misread. */
static void
test_refuses_other_documents(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(JsonNode) v2 = NULL;
    g_autoptr(JsonNode) history = NULL;

    g_assert_true(json_parser_load_from_data(parser,
        "{\"schema_version\":2,\"kind\":\"usage\",\"entries\":[]}", -1, NULL));
    v2 = clawt_provider_usage_normalize(json_parser_get_root(parser));
    g_assert_null(v2);

    g_assert_true(json_parser_load_from_data(parser,
        "{\"schema_version\":1,\"kind\":\"history\",\"entries\":[]}", -1, NULL));
    history = clawt_provider_usage_normalize(json_parser_get_root(parser));
    g_assert_null(history);
    g_assert_null(clawt_provider_usage_normalize(NULL));
}

/* Every level has a tone the stylesheets paint; the bar fills for what is left. */
static void
test_tones_and_meter(void)
{
    const gchar *painted[] = { "good", "warn", "bad", "neutral", NULL };
    ClawtAllowanceLevel level;
    g_autofree gchar *half = clawt_usage_meter_text(50.0, 10);
    g_autofree gchar *full = clawt_usage_meter_text(140.0, 4);

    for (level = CLAWT_ALLOWANCE_UNKNOWN; level <= CLAWT_ALLOWANCE_CRITICAL; level++)
        g_assert_true(g_strv_contains(painted, clawt_allowance_level_tone(level)));

    g_assert_cmpstr(half, ==,
        "\xe2\x96\x95\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88"
        "\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91\xe2\x96\x8f");
    g_assert_cmpstr(full, ==,
        "\xe2\x96\x95\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x8f");
}

/* "Has no plan limits" and "failed" must never read alike. */
static void
test_status_labels(void)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autofree gchar *unsupported = NULL;
    g_autofree gchar *failed = NULL;
    g_autofree gchar *pending = NULL;
    g_autofree gchar *title = NULL;

    g_assert_true(json_parser_load_from_data(parser,
        "[{\"provider\":\"claude-code\",\"state\":\"unsupported\",\"message\":\"API key\"},"
        " {\"provider\":\"codex-cli\",\"state\":\"error\",\"message\":\"timed out\"},"
        " {\"provider\":\"codex-cli\",\"state\":\"pending\"},"
        " {\"provider\":\"claude-code\",\"account\":\"work\",\"plan\":\"max\"}]", -1, NULL));
    {
        JsonArray *rows = json_node_get_array(json_parser_get_root(parser));

        unsupported = clawt_usage_login_status_label(json_array_get_object_element(rows, 0), 0);
        failed = clawt_usage_login_status_label(json_array_get_object_element(rows, 1), 0);
        pending = clawt_usage_login_status_label(json_array_get_object_element(rows, 2), 0);
        title = clawt_usage_login_title(json_array_get_object_element(rows, 3));
    }

    g_assert_cmpstr(unsupported, ==, "Reports no plan limits: API key");
    g_assert_cmpstr(failed, ==, "Could not be read: timed out");
    g_assert_cmpstr(pending, ==, "Asking the provider\xe2\x80\xa6");
    g_assert_cmpstr(title, ==, "Claude Code \xe2\x80\x94 work (max)");
}

/* ── The daemon ─────────────────────────────────────────────────── */

typedef struct {
    gchar        *dir;
    GMainContext *context;
    ClawtDaemon  *daemon;
} DaemonFixture;

/* Which logins the hook was asked about, in order. */
static GPtrArray *asked;

/* Codex answers with a window; Claude's login is an API key, so it has none. */
static void
fake_query(ClawtDaemon  *self,
           const gchar  *login_id,
           const gchar  *provider,
           const gchar  *config_dir,
           GCancellable *cancellable)
{
    g_ptr_array_add(asked, g_strdup_printf("%s|%s", login_id,
                                           config_dir != NULL ? "dir" : "home"));

    if (g_strcmp0(provider, "codex-cli") == 0) {
        const gchar *weekly[] = { "used_percent", "25", NULL };
        g_autofree gchar *e = entry_with("Weekly", "percent", weekly);
        g_autoptr(JsonNode) report = report_node(e, NULL);

        clawt_daemon_provider_usage_landed(self, login_id, report, NULL);
    } else {
        g_autoptr(GError) error = g_error_new(AI_ERROR, AI_ERROR_NOT_SUPPORTED,
                                              "plan limits do not apply to this login");

        clawt_daemon_provider_usage_landed(self, login_id, NULL, error);
    }
}

static void
daemon_setup(DaemonFixture *fixture)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *yaml = NULL;

    fixture->dir = g_dir_make_tmp("clawt-provider-usage-XXXXXX", NULL);
    path = g_build_filename(fixture->dir, "config.yaml", NULL);
    yaml = g_strdup_printf("daemon:\n  tailscale: false\n  state_dir: \"%s/state\"\n"
                           "  socket: \"%s/d.sock\"\n  automation_dir: \"%s/pods\"\n"
                           "defaults:\n  workspace_root: \"%s/agents\"\n"
                           "accounts:\n"
                           "  - id: work\n    provider: claude-code\n"
                           "    config_dir: \"%s/claude-work\"\n"
                           "agents:\n"
                           "  - id: kudu\n    model:\n      provider: codex-cli\n"
                           "    runtime:\n      autostart: false\n"
                           "  - id: oryx\n    model:\n      provider: claude-code\n"
                           "      account: work\n    runtime:\n      autostart: false\n"
                           "  - id: pika\n    model:\n      provider: codex-cli\n"
                           "    runtime:\n      autostart: false\n",
                           fixture->dir, fixture->dir, fixture->dir, fixture->dir,
                           fixture->dir);
    g_assert_true(g_file_set_contents(path, yaml, -1, &error));
    fixture->context = g_main_context_new();
    fixture->daemon = clawt_daemon_new(path, fixture->context);
    clawt_daemon_start(fixture->daemon, &error);
    g_assert_no_error(error);
    fixture->daemon->provider_usage_query = fake_query;
    asked = g_ptr_array_new_with_free_func(g_free);
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
    g_clear_pointer(&asked, g_ptr_array_unref);
}

static JsonNode *
providers(DaemonFixture *fixture, gboolean refresh)
{
    g_autoptr(JsonNode) frame = clawt_ipc_request_new("usage.providers", "t1");
    g_autoptr(JsonBuilder) builder = json_builder_new();

    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "refresh");
    json_builder_add_boolean_value(builder, refresh);
    json_builder_end_object(builder);
    clawt_ipc_frame_set_payload(frame, json_builder_get_root(builder));
    return clawt_daemon_handle_request(fixture->daemon, frame);
}

static JsonObject *
find_login(JsonNode *reply, const gchar *id)
{
    JsonArray *logins = json_object_get_array_member(clawt_ipc_frame_get_payload(reply),
                                                     "logins");
    guint i;

    for (i = 0; i < json_array_get_length(logins); i++) {
        JsonObject *login = json_array_get_object_element(logins, i);

        if (g_strcmp0(json_object_get_string_member(login, "id"), id) == 0)
            return login;
    }
    return NULL;
}

/*
 * A login, not an agent: two codex agents on the default login are one
 * allowance.  Nothing is asked until a client asks for a refresh, and a
 * fresh reading is not asked for again.
 */
static void
test_daemon_lists_logins_and_reads_on_request(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(JsonNode) idle = NULL;
    g_autoptr(JsonNode) read = NULL;
    g_autoptr(JsonNode) again = NULL;
    JsonObject *codex;
    JsonObject *claude;
    JsonArray *logins;

    daemon_setup(&fixture);

    idle = providers(&fixture, FALSE);
    logins = json_object_get_array_member(clawt_ipc_frame_get_payload(idle), "logins");
    g_assert_cmpuint(json_array_get_length(logins), ==, 2);
    g_assert_cmpuint(asked->len, ==, 0);
    g_assert_cmpstr(json_object_get_string_member(find_login(idle, "codex-cli:default"),
                                                  "state"), ==, "idle");

    read = providers(&fixture, TRUE);
    g_assert_cmpuint(asked->len, ==, 2);
    /* the account's own directory, never the daemon's home */
    g_assert_true(g_ptr_array_find_with_equal_func(asked, "claude-code:work|dir",
                                                   g_str_equal, NULL));
    g_assert_true(g_ptr_array_find_with_equal_func(asked, "codex-cli:default|home",
                                                   g_str_equal, NULL));

    g_clear_pointer(&read, json_node_unref);
    read = providers(&fixture, FALSE);
    codex = find_login(read, "codex-cli:default");
    claude = find_login(read, "claude-code:work");
    g_assert_cmpstr(json_object_get_string_member(codex, "state"), ==, "ok");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(codex, "windows")),
                     ==, 1);
    g_assert_cmpstr(json_object_get_string_member(claude, "state"), ==, "unsupported");
    g_assert_cmpstr(json_object_get_string_member(claude, "account"), ==, "work");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(claude, "windows")),
                     ==, 0);

    /* both answered just now: a second refresh asks nobody */
    again = providers(&fixture, TRUE);
    g_assert_cmpuint(asked->len, ==, 2);

    daemon_teardown(&fixture);
}

/*
 * A reading outlives the daemon that took it.  The cache was memory only,
 * so a restart blanked every meter until somebody refreshed -- which
 * spawns every login's CLI.  Now the next daemon starts from the readings
 * on disk, with their own times, and asks no CLI to show them.
 */
static void
test_readings_survive_a_restart(void)
{
    DaemonFixture fixture = { 0 };
    g_autoptr(JsonNode) read = NULL;
    g_autoptr(JsonNode) after = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *file = NULL;
    gint64 read_at;
    JsonObject *codex;
    JsonObject *claude;
    GStatBuf info;

    daemon_setup(&fixture);
    g_clear_pointer(&read, json_node_unref);
    read = providers(&fixture, TRUE);
    g_clear_pointer(&read, json_node_unref);
    read = providers(&fixture, FALSE);
    read_at = json_object_get_int_member(find_login(read, "codex-cli:default"), "read_at");
    g_assert_cmpint(read_at, >, 0);

    /* kept where only the daemon's user can read it */
    file = g_build_filename(fixture.dir, "state", "usage", "providers.json", NULL);
    g_assert_cmpint(g_stat(file, &info), ==, 0);
    g_assert_cmpint(info.st_mode & 0777, ==, 0600);

    /* a new daemon on the same state */
    clawt_daemon_stop(fixture.daemon);
    g_clear_object(&fixture.daemon);
    while (g_main_context_iteration(fixture.context, FALSE))
        ;
    g_ptr_array_set_size(asked, 0);
    path = g_build_filename(fixture.dir, "config.yaml", NULL);
    fixture.daemon = clawt_daemon_new(path, fixture.context);
    clawt_daemon_start(fixture.daemon, &error);
    g_assert_no_error(error);
    fixture.daemon->provider_usage_query = fake_query;

    after = providers(&fixture, FALSE);
    codex = find_login(after, "codex-cli:default");
    claude = find_login(after, "claude-code:work");
    g_assert_cmpstr(json_object_get_string_member(codex, "state"), ==, "ok");
    g_assert_cmpint(json_object_get_int_member(codex, "read_at"), ==, read_at);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(codex, "windows")),
                     ==, 1);
    g_assert_cmpstr(json_object_get_string_member(claude, "state"), ==, "unsupported");
    g_assert_cmpuint(asked->len, ==, 0);

    daemon_teardown(&fixture);
}

gint
main(gint argc, gchar *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/provider-usage/claude-windows", test_claude_windows);
    g_test_add_func("/provider-usage/direction-unknown", test_direction_unknown);
    g_test_add_func("/provider-usage/money-and-dates", test_money_and_dates);
    g_test_add_func("/provider-usage/refuses-other-documents",
                    test_refuses_other_documents);
    g_test_add_func("/provider-usage/tones-and-meter", test_tones_and_meter);
    g_test_add_func("/provider-usage/status-labels", test_status_labels);
    g_test_add_func("/provider-usage/readings-survive-a-restart",
                    test_readings_survive_a_restart);
    g_test_add_func("/provider-usage/daemon-lists-logins-and-reads-on-request",
                    test_daemon_lists_logins_and_reads_on_request);

    return g_test_run();
}
