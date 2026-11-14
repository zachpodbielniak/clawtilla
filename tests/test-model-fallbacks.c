/*
 * test-model-fallbacks.c - agents.model.fallbacks, end to end on our side
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * libreclaw does the failing over; clawtilla's part is the list's shape
 * and its one text spelling, the refusal of a list that would rotate
 * subscription accounts, the render into the agent's config (read back
 * with libreclaw's own parser and handed to libreclaw's own list, so the
 * two halves are shown to agree rather than assumed to), and turning the
 * agent's `agent.failover` frame into state, an event and a sentence.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"

#include <glib/gstdio.h>
#include <string.h>
#include <libreclaw.h>

static ClawtAgentConfig *
first_agent(ClawtConfig *config)
{
    GPtrArray *agents = clawt_config_get_agents(config);

    g_assert_cmpuint(agents->len, >, 0);
    return g_ptr_array_index(agents, 0);
}

static ClawtConfig *
load(const gchar *agent_yaml)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *yaml = g_strdup_printf(
        "accounts:\n"
        "  - id: work\n"
        "    provider: claude-code\n"
        "    config_dir: /acct/claude-work\n"
        "  - id: api\n"
        "    provider: claude-code\n"
        "    config_dir: /acct/claude-api\n"
        "    pay_per_use: true\n"
        "  - id: codex-b\n"
        "    provider: codex-cli\n"
        "    config_dir: /acct/codex-b\n"
        "agents:\n"
        "%s", agent_yaml);
    ClawtConfig *config = clawt_config_load_from_string(yaml, &error);

    g_assert_no_error(error);
    return config;
}

/* ------------------------------------------------------------------ */
/* The text spelling                                                   */
/* ------------------------------------------------------------------ */

static void
test_parse_and_format_round_trip(void)
{
    g_autoptr(GPtrArray) list = NULL;
    g_autoptr(GPtrArray) again = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *text_again = NULL;
    g_autoptr(GError) error = NULL;
    ClawtModelFallback *entry;

    list = clawt_model_fallback_parse_list(
        " codex-cli:gpt-5.5@codex-b , claude-code:sonnet,opencode:xai/grok-4:fast ",
        &error);
    g_assert_no_error(error);
    g_assert_cmpuint(list->len, ==, 3);

    entry = g_ptr_array_index(list, 0);
    g_assert_cmpstr(entry->provider, ==, "codex-cli");
    g_assert_cmpstr(entry->model, ==, "gpt-5.5");
    g_assert_cmpstr(entry->account, ==, "codex-b");

    entry = g_ptr_array_index(list, 1);
    g_assert_null(entry->account);

    /* The provider ends at the first colon; a model may hold more. */
    entry = g_ptr_array_index(list, 2);
    g_assert_cmpstr(entry->provider, ==, "opencode");
    g_assert_cmpstr(entry->model, ==, "xai/grok-4:fast");

    text = clawt_model_fallback_list_to_string(list);
    g_assert_cmpstr(text, ==,
                    "codex-cli:gpt-5.5@codex-b, claude-code:sonnet, "
                    "opencode:xai/grok-4:fast");

    again = clawt_model_fallback_parse_list(text, &error);
    g_assert_no_error(error);
    text_again = clawt_model_fallback_list_to_string(again);
    g_assert_cmpstr(text_again, ==, text);
}

/* Half an entry is refused, never saved as a shorter list. */
static void
test_parse_refuses_half_an_entry(void)
{
    g_autoptr(GPtrArray) empty = NULL;
    GError *error = NULL;

    g_assert_null(clawt_model_fallback_parse_list("codex-cli", &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    g_assert_null(clawt_model_fallback_parse_list("codex-cli:", &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    g_assert_null(clawt_model_fallback_parse_list(":gpt-5.5", &error));
    g_clear_error(&error);

    /* Blank is the empty list, which clears the key. */
    empty = clawt_model_fallback_parse_list("  ", &error);
    g_assert_no_error(error);
    g_assert_cmpuint(empty->len, ==, 0);
}

/* ------------------------------------------------------------------ */
/* Config                                                              */
/* ------------------------------------------------------------------ */

/*
 * The list renders as ai.fallbacks with each account resolved to its
 * directory, libreclaw reads it back, and libreclaw's own list takes
 * every entry -- the two halves of the refusal agree on this one.
 */
static void
test_renders_and_libreclaw_accepts(void)
{
    g_autoptr(ClawtConfig) config = load(
        "  - id: a\n"
        "    model:\n"
        "      provider: claude-code\n"
        "      model: opus\n"
        "      fallbacks:\n"
        "        - provider: codex-cli\n"
        "          model: gpt-5.5\n"
        "          account: codex-b\n"
        "        - provider: claude-code\n"
        "          model: opus\n"
        "          account: api\n"
        "        - provider: claude-code\n"
        "          model: sonnet\n");
    ClawtAgentConfig *agent = first_agent(config);
    g_autofree gchar *rendered = NULL;
    g_autofree gchar *dir = g_dir_make_tmp("clawt-fallbacks-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "config.yaml", NULL);
    g_autoptr(LcConfig) parsed = lc_config_new();
    g_autoptr(GPtrArray) entries = NULL;
    g_autoptr(LcFailoverEntry) primary = NULL;
    g_autoptr(LcFailover) failover = NULL;
    g_autoptr(GError) error = NULL;
    guint i;

    g_assert_false(clawt_agent_config_is_shadow(agent));

    rendered = clawt_config_render_agent(config, agent, "/tmp/s.sock",
                                         "/tmp/state", &error);
    g_assert_no_error(error);
    g_assert_nonnull(strstr(rendered, "  fallbacks:\n"));

    g_assert_true(g_file_set_contents(path, rendered, -1, &error));
    g_assert_true(lc_config_load_from_path(parsed, path, &error));
    g_assert_no_error(error);

    entries = lc_config_get_ai_fallbacks(parsed);
    g_assert_cmpuint(entries->len, ==, 3);
    g_assert_cmpstr(lc_failover_entry_get_provider(g_ptr_array_index(entries, 0)),
                    ==, "codex-cli");
    g_assert_cmpstr(lc_failover_entry_get_config_dir(g_ptr_array_index(entries, 0)),
                    ==, "/acct/codex-b");
    g_assert_false(lc_failover_entry_get_pay_per_use(g_ptr_array_index(entries, 0)));
    g_assert_cmpstr(lc_failover_entry_get_config_dir(g_ptr_array_index(entries, 1)),
                    ==, "/acct/claude-api");
    g_assert_true(lc_failover_entry_get_pay_per_use(g_ptr_array_index(entries, 1)));
    g_assert_null(lc_failover_entry_get_config_dir(g_ptr_array_index(entries, 2)));

    primary = lc_failover_entry_new("claude-code", "opus", NULL, FALSE);
    failover = lc_failover_new(primary);
    for (i = 0; i < entries->len; i++) {
        g_assert_true(lc_failover_add_fallback(failover,
                                               g_ptr_array_index(entries, i),
                                               &error));
        g_assert_no_error(error);
    }

    clawt_test_remove_tree(dir);
}

static void
test_none_renders_nothing(void)
{
    g_autoptr(ClawtConfig) config = load("  - id: a\n");
    g_autofree gchar *rendered = clawt_config_render_agent(
        config, first_agent(config), "/tmp/s.sock", "/tmp/state", NULL);

    g_assert_null(strstr(rendered, "fallbacks:"));
}

static void
assert_shadow(const gchar *agent_yaml, const gchar *reason_part)
{
    g_autoptr(ClawtConfig) config = load(agent_yaml);
    ClawtAgentConfig *agent = first_agent(config);

    g_assert_true(clawt_agent_config_is_shadow(agent));
    g_assert_nonnull(strstr(clawt_agent_config_get_shadow_reason(agent),
                            reason_part));
}

/*
 * The refusal the whole feature is careful about: another sign-in to the
 * subscription the agent already uses.  Shadowed at load with the reason,
 * never handed to libreclaw to drop quietly.
 */
static void
test_same_subscription_is_shadow(void)
{
    /* The agent on its default Claude login, a fallback on another. */
    assert_shadow("  - id: a\n"
                  "    model:\n"
                  "      provider: claude-code\n"
                  "      fallbacks:\n"
                  "        - provider: claude-code\n"
                  "          model: opus\n"
                  "          account: work\n",
                  "same claude subscription");

    /* claude-tmux is the same subscription driven another way. */
    assert_shadow("  - id: a\n"
                  "    model:\n"
                  "      provider: claude-code\n"
                  "      account: work\n"
                  "      fallbacks:\n"
                  "        - provider: claude-tmux\n"
                  "          model: opus\n",
                  "same claude subscription");

    /* Two codex logins in the list rotate codex accounts. */
    assert_shadow("  - id: a\n"
                  "    model:\n"
                  "      provider: claude-code\n"
                  "      fallbacks:\n"
                  "        - provider: codex-cli\n"
                  "          model: gpt-5.5\n"
                  "        - provider: codex-cli\n"
                  "          model: gpt-5.5\n"
                  "          account: codex-b\n",
                  "same codex-cli subscription");
}

/* A pay-per-use login on the same engine bills the work: allowed. */
static void
test_pay_per_use_is_allowed(void)
{
    g_autoptr(ClawtConfig) config = load(
        "  - id: a\n"
        "    model:\n"
        "      provider: claude-code\n"
        "      account: work\n"
        "      fallbacks:\n"
        "        - provider: claude-code\n"
        "          model: opus\n"
        "          account: api\n");

    g_assert_false(clawt_agent_config_is_shadow(first_agent(config)));
}

static void
test_bad_entries_are_shadow(void)
{
    assert_shadow("  - id: a\n"
                  "    model:\n"
                  "      fallbacks:\n"
                  "        - provider: codex-cli\n"
                  "          model: gpt-5.5\n"
                  "          account: nobody\n",
                  "no account 'nobody'");

    /* A Claude login on a codex fallback would be CODEX_HOME at ~/.claude. */
    assert_shadow("  - id: a\n"
                  "    model:\n"
                  "      fallbacks:\n"
                  "        - provider: codex-cli\n"
                  "          model: gpt-5.5\n"
                  "          account: work\n",
                  "is a claude-code login");

    /* libreclaw would run Claude Code for this, so it is refused. */
    assert_shadow("  - id: a\n"
                  "    model:\n"
                  "      fallbacks:\n"
                  "        - provider: openai\n"
                  "          model: gpt-4o\n",
                  "not a provider an agent can run on");

    assert_shadow("  - id: a\n"
                  "    model:\n"
                  "      fallbacks:\n"
                  "        - provider: codex-cli\n",
                  "needs a provider and a model");
}

/* The setter writes what the getter reads, and an empty list removes the key. */
static void
test_set_round_trips(void)
{
    g_autoptr(ClawtConfig) config = load("  - id: a\n");
    ClawtAgentConfig *agent = first_agent(config);
    g_autoptr(GPtrArray) read_back = NULL;
    g_autoptr(GPtrArray) cleared = NULL;
    g_autofree gchar *spelled = NULL;
    g_autofree gchar *saved = NULL;

    g_assert_true(clawt_agent_config_set_from_string(
        agent, "model.fallbacks", "codex-cli:gpt-5.5, claude-code:sonnet@api"));

    read_back = clawt_agent_config_get_fallbacks(agent);
    spelled = clawt_model_fallback_list_to_string(read_back);
    g_assert_cmpstr(spelled, ==, "codex-cli:gpt-5.5, claude-code:sonnet@api");

    saved = clawt_config_to_string(config);
    g_assert_nonnull(strstr(saved, "account: api"));

    /* Text that does not parse writes nothing at all. */
    g_assert_false(clawt_agent_config_set_from_string(agent, "model.fallbacks",
                                                      "codex-cli"));

    g_assert_true(clawt_agent_config_set_from_string(agent, "model.fallbacks",
                                                     ""));
    cleared = clawt_agent_config_get_fallbacks(agent);
    g_assert_cmpuint(cleared->len, ==, 0);
}

/* ------------------------------------------------------------------ */
/* What the agent reports                                              */
/* ------------------------------------------------------------------ */

static JsonObject *
report(const gchar *state, gint64 reset_at, gint64 retry_at, gboolean lost)
{
    JsonObject *payload = json_object_new();
    JsonObject *primary = json_object_new();
    JsonObject *active = json_object_new();

    json_object_set_string_member(primary, "provider", "claude-code");
    json_object_set_string_member(primary, "model", "opus");
    json_object_set_string_member(active, "provider", "codex-cli");
    json_object_set_string_member(active, "model", "gpt-5.5");
    json_object_set_int_member(active, "index", 1);

    json_object_set_string_member(payload, "state", state);
    json_object_set_object_member(payload, "primary", primary);
    json_object_set_object_member(payload, "active", active);
    json_object_set_string_member(payload, "reason", "usage-limit");
    json_object_set_int_member(payload, "reset_at", reset_at);
    json_object_set_int_member(payload, "retry_at", retry_at);
    json_object_set_boolean_member(payload, "context_lost", lost);

    return payload;
}

static void
test_describe(void)
{
    gint64 soon = g_get_real_time() / G_USEC_PER_SEC + 600;
    g_autoptr(JsonObject) away = report("fallback", soon, soon, TRUE);
    g_autoptr(JsonObject) guessed = report("fallback", 0, soon, FALSE);
    g_autoptr(JsonObject) home = report("primary", 0, 0, FALSE);
    g_autofree gchar *line = clawt_failover_describe("Researcher", away);
    g_autofree gchar *guess = clawt_failover_describe("r", guessed);
    g_autofree gchar *back = clawt_failover_describe("Researcher", home);
    g_autofree gchar *badge = clawt_failover_badge(away);
    g_autofree gchar *no_badge = clawt_failover_badge(home);

    g_assert_nonnull(strstr(line, "Researcher is running on fallback "
                                  "codex-cli gpt-5.5 because claude-code "
                                  "opus hit its usage limit, back at "));
    g_assert_nonnull(strstr(line, "context did not come along"));

    /* A backoff is an attempt, not a promise. */
    g_assert_nonnull(strstr(guess, "trying claude-code opus again at "));
    g_assert_null(strstr(guess, "back at"));
    g_assert_null(strstr(guess, "context"));

    g_assert_cmpstr(back, ==, "Researcher is back on claude-code opus");
    g_assert_cmpstr(badge, ==, "On fallback: codex-cli gpt-5.5");
    g_assert_null(no_badge);

    g_assert_true(clawt_failover_is_active(away));
    g_assert_false(clawt_failover_is_active(home));
    g_assert_false(clawt_failover_is_active(NULL));
    g_assert_cmpstr(clawt_failover_reason_phrase("something-new"), ==,
                    "refused a turn");
}

/*
 * The alert's tier follows the state: leaving is a notice, coming back
 * is routine.
 */
static void
test_alert_tier(void)
{
    g_autoptr(ClawtEvent) away = clawt_event_new("agent.failover", "a");
    g_autoptr(ClawtEvent) home = clawt_event_new("agent.failover", "a");

    clawt_event_set_detail(away, "state", "fallback");
    clawt_event_set_detail(home, "state", "primary");

    g_assert_cmpint(clawt_alert_tier_for_event(away), ==, CLAWT_ALERT_NOTICE);
    g_assert_cmpint(clawt_alert_tier_for_event(home), ==, CLAWT_ALERT_ROUTINE);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/model-fallbacks/parse-format-round-trip",
                    test_parse_and_format_round_trip);
    g_test_add_func("/model-fallbacks/parse-refuses-half-an-entry",
                    test_parse_refuses_half_an_entry);
    g_test_add_func("/model-fallbacks/renders-and-libreclaw-accepts",
                    test_renders_and_libreclaw_accepts);
    g_test_add_func("/model-fallbacks/none-renders-nothing",
                    test_none_renders_nothing);
    g_test_add_func("/model-fallbacks/same-subscription-is-shadow",
                    test_same_subscription_is_shadow);
    g_test_add_func("/model-fallbacks/pay-per-use-is-allowed",
                    test_pay_per_use_is_allowed);
    g_test_add_func("/model-fallbacks/bad-entries-are-shadow",
                    test_bad_entries_are_shadow);
    g_test_add_func("/model-fallbacks/set-round-trips", test_set_round_trips);
    g_test_add_func("/model-fallbacks/describe", test_describe);
    g_test_add_func("/model-fallbacks/alert-tier", test_alert_tier);

    return g_test_run();
}
