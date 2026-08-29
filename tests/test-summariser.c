/*
 * test-summariser.c - Turning finished work into memories, and nudges
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * Driven with AiMockProvider, so the tool loop runs exactly as it would
 * against a real provider and no test reaches the network.
 *
 * The two answers that matter most are the boring ones: a model that
 * calls nothing leaves nothing behind, and the executor grants no tool
 * but `remember`.  The second is not a nicety -- ai_tool_executor_new()
 * silently hands out bash, read, write and edit, and this thing runs
 * unattended after every finished task.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar            *dir;
    ClawtConfig      *config;
    ClawtMemoryStore *store;
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
    g_autofree gchar *yaml = NULL;
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-summarise-XXXXXX", NULL);

    /*
     * Pinned away from the real fleet for the reason every fixture here
     * is: `defaults.workspace_root` is ~/.clawtilla/agents unless told
     * otherwise, and a test that scaffolds into it leaves directories
     * indistinguishable from agents somebody meant to keep.
     */
    yaml = g_strdup_printf("daemon:\n"
                           "  tailscale: false\n"
                           "  state_dir: \"%s/state\"\n"
                           "  socket: \"%s/daemon.sock\"\n"
                           "  automation_dir: \"%s/pods\"\n"
                           "defaults:\n"
                           "  workspace_root: \"%s/agents\"\n",
                           fixture->dir, fixture->dir, fixture->dir,
                           fixture->dir);

    fixture->config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    path = g_build_filename(fixture->dir, "memory.db", NULL);
    fixture->store = clawt_memory_store_new(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture->store);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->store);
    g_clear_object(&fixture->config);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

static void
test_a_finished_task_becomes_memories(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSummariser) summariser = NULL;
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GPtrArray) written = NULL;
    g_autoptr(GError) error = NULL;
    ClawtMemory *memory;
    guint count;

    fixture_setup(&fixture);

    summariser = clawt_summariser_new(fixture.config);
    clawt_summariser_set_provider(summariser, AI_PROVIDER(provider));

    ai_mock_provider_push_tool_use(
        provider, "remember",
        "{\"content\":\"the runner needs qemu-img before it can provision\","
        "\"category\":\"gotcha\",\"tags\":\"vm\"}");
    ai_mock_provider_push_text(provider, "One thing worth keeping.");

    count = clawt_summariser_summarise(summariser, fixture.store,
                                       "task:tsk-1", "we tried and failed "
                                       "until qemu-img was installed",
                                       1000, 2000, NULL, &error);

    g_assert_no_error(error);
    g_assert_cmpuint(count, ==, 1);

    written = clawt_memory_store_list(fixture.store, NULL, FALSE, 0, NULL);
    g_assert_cmpuint(written->len, ==, 1);

    memory = g_ptr_array_index(written, 0);

    g_assert_cmpstr(memory->category, ==, "gotcha");

    /*
     * The source and the transcript range are stamped by clawtilla, not
     * asked of the model: a memory that turns out to be wrong is only
     * fixable if the conversation behind it can be found again, and it
     * is the one fact in the memory the model does not reliably know.
     */
    g_assert_cmpstr(memory->source, ==, "task:tsk-1");
    g_assert_nonnull(strstr(memory->tags, "transcript:1000-2000"));
    g_assert_nonnull(strstr(memory->tags, "vm"));

    fixture_teardown(&fixture);
}

/*
 * A model that called nothing leaves nothing behind, and is not an
 * error.
 *
 * Most finished work establishes nothing worth remembering, and a
 * summariser that treated that as a failure would fill a fleet's log
 * with reports of it working correctly -- or, worse, invent a memory to
 * have produced one.
 */
static void
test_a_summary_of_nothing_writes_nothing(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSummariser) summariser = NULL;
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GPtrArray) written = NULL;
    g_autoptr(GError) error = NULL;
    guint count;

    fixture_setup(&fixture);

    summariser = clawt_summariser_new(fixture.config);
    clawt_summariser_set_provider(summariser, AI_PROVIDER(provider));

    ai_mock_provider_push_text(provider,
                               "Nothing here is worth remembering.");

    count = clawt_summariser_summarise(summariser, fixture.store,
                                       "task:tsk-2", "we renamed a variable",
                                       1000, 2000, NULL, &error);

    g_assert_no_error(error);
    g_assert_cmpuint(count, ==, 0);

    written = clawt_memory_store_list(fixture.store, NULL, FALSE, 0, NULL);
    g_assert_cmpuint(written->len, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * A summariser with no provider refuses rather than silently doing
 * nothing.
 *
 * "It wrote no memories" is the common correct answer, so a summariser
 * that could not run at all has to be distinguishable from one that ran
 * and found nothing.
 */
static void
test_no_provider_is_a_refusal(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSummariser) summariser = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);

    summariser = clawt_summariser_new(fixture.config);

    g_assert_cmpuint(clawt_summariser_summarise(summariser, fixture.store,
                                                "task:tsk-3", "anything",
                                                0, 0, NULL, &error),
                     ==, 0);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);

    fixture_teardown(&fixture);
}

/*
 * The model is offered `remember` and nothing else.
 *
 * ai_tool_executor_new() grants bash, read, write and edit, and
 * unregister() cannot take a built-in back -- so a summariser built with
 * it could run commands on the machine, unattended, after every task
 * that finished.  Asserted by *calling* the tools rather than by reading
 * a list: a name absent from a listing but still executable is exactly
 * the failure this guards.
 */
static void
test_the_model_gets_no_tool_but_remember(void)
{
    static const gchar *forbidden[] = { "bash", "read", "write", "edit",
                                        NULL };
    Fixture fixture = { 0 };
    g_autoptr(ClawtSummariser) summariser = NULL;
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autofree gchar *marker = NULL;
    g_autoptr(GPtrArray) written = NULL;
    g_autoptr(GError) error = NULL;
    gsize i;

    fixture_setup(&fixture);

    summariser = clawt_summariser_new(fixture.config);
    clawt_summariser_set_provider(summariser, AI_PROVIDER(provider));

    marker = g_build_filename(fixture.dir, "should-not-exist", NULL);

    for (i = 0; forbidden[i] != NULL; i++) {
        g_autofree gchar *arguments =
            g_strdup_printf("{\"command\":\"touch %s\",\"path\":\"%s\","
                            "\"content\":\"x\"}", marker, marker);

        ai_mock_provider_push_tool_use(provider, forbidden[i], arguments);
    }

    ai_mock_provider_push_text(provider, "Done.");

    clawt_summariser_summarise(summariser, fixture.store, "task:tsk-4",
                               "a transcript", 0, 0, NULL, &error);

    /* Nothing ran, and nothing was remembered either. */
    g_assert_false(g_file_test(marker, G_FILE_TEST_EXISTS));

    written = clawt_memory_store_list(fixture.store, NULL, FALSE, 0, NULL);
    g_assert_cmpuint(written->len, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * The transcript is cut to the budget, from the end.
 *
 * What a piece of work concluded is at the end of it, so a budget taken
 * off the front records the plan rather than the outcome.
 */
static void
test_the_budget_keeps_the_end_of_the_transcript(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtSummariser) summariser = NULL;
    g_autoptr(AiMockProvider) provider = ai_mock_provider_new();
    g_autoptr(GString) transcript = g_string_new(NULL);
    g_autoptr(GError) error = NULL;
    guint i;

    fixture_setup(&fixture);

    summariser = clawt_summariser_new(fixture.config);
    clawt_summariser_set_provider(summariser, AI_PROVIDER(provider));
    clawt_summariser_set_budget_bytes(summariser, 64);

    g_assert_cmpuint(clawt_summariser_get_budget_bytes(summariser), ==, 64);

    for (i = 0; i < 200; i++)
        g_string_append(transcript, "planning. ");

    g_string_append(transcript, "CONCLUSION");

    ai_mock_provider_push_text(provider, "Nothing to keep.");

    clawt_summariser_summarise(summariser, fixture.store, "task:tsk-5",
                               transcript->str, 0, 0, NULL, &error);

    g_assert_no_error(error);

    /*
     * The mock keeps the messages it was handed, which is how the cut
     * can be asserted rather than assumed.  A test that only checked
     * that the call succeeded would pass against a summariser that sent
     * the whole transcript.
     */
    {
        GList *messages = ai_mock_provider_get_last_messages(provider);
        const gchar *sent;

        g_assert_nonnull(messages);
        sent = ai_message_get_text(AI_MESSAGE(messages->data));

        g_assert_nonnull(sent);
        g_assert_cmpuint(strlen(sent), <=, 64);
        g_assert_nonnull(strstr(sent, "CONCLUSION"));
    }

    fixture_teardown(&fixture);
}

/* ── The nudge ───────────────────────────────────────────────────── */

/*
 * 0 turns off means no text at all, not an empty reminder.
 *
 * A blank line in a per-turn suffix costs context on every turn and
 * instructs nobody.
 */
static void
test_a_nudge_of_zero_says_nothing(void)
{
    g_autofree gchar *off = clawt_summariser_nudge_text(0);
    g_autofree gchar *on = clawt_summariser_nudge_text(12);

    g_assert_null(off);
    g_assert_nonnull(on);

    /*
     * The cadence is in the text, because the suffix reaches every turn
     * and the number is what the reminder asks for rather than something
     * clawtilla counts.
     */
    g_assert_nonnull(strstr(on, "12"));
    g_assert_nonnull(strstr(on, "clawtilla_memory_add"));
}

/* ── The provenance rule ─────────────────────────────────────────── */

/*
 * One spelling of the rule, reachable from the library.
 *
 * It appears in the memory tool descriptions, in the summariser's system
 * prompt and in every agent's AGENTS.org -- and a rule written out three
 * times is a rule with three versions of it in a fleet.  Memory is a
 * prompt-injection *persistence* vector: an instruction an agent copies
 * into its own memory is read back later as its own conclusion.
 */
static void
test_the_provenance_rule_has_one_spelling(void)
{
    const gchar *rule = clawt_memory_provenance_rule();

    g_assert_nonnull(rule);
    g_assert_nonnull(strstr(rule, "verified"));
    g_assert_nonnull(strstr(rule, "webhook"));
    g_assert_nonnull(strstr(rule, "another agent"));
    g_assert_nonnull(strstr(rule, "imported file"));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/summariser/finished-work-becomes-memories",
                    test_a_finished_task_becomes_memories);
    g_test_add_func("/summariser/nothing-worth-keeping",
                    test_a_summary_of_nothing_writes_nothing);
    g_test_add_func("/summariser/no-provider-refuses",
                    test_no_provider_is_a_refusal);
    g_test_add_func("/summariser/no-tool-but-remember",
                    test_the_model_gets_no_tool_but_remember);
    g_test_add_func("/summariser/budget-keeps-the-end",
                    test_the_budget_keeps_the_end_of_the_transcript);
    g_test_add_func("/summariser/nudge-off-is-silent",
                    test_a_nudge_of_zero_says_nothing);
    g_test_add_func("/summariser/provenance-rule",
                    test_the_provenance_rule_has_one_spelling);

    return g_test_run();
}
