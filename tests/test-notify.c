/*
 * test-notify.c - What is worth interrupting somebody for, and how
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * A notifier is the one thing in a fleet you cannot tell is working by
 * looking at it: it is correct precisely when nothing happens.  So the
 * parts that decide *whether* to fire are pure functions, and this is
 * where they are held to it -- including the one that is only wrong
 * between eleven at night and seven in the morning.
 */

#include <clawtilla.h>

#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include "clawt-test-util.h"

/* ── Quiet hours ─────────────────────────────────────────────────── */

static gint
minutes(gint hour, gint minute)
{
    return hour * 60 + minute;
}

static void
test_a_range_is_parsed(void)
{
    gint start = 0;
    gint end = 0;

    g_assert_true(clawt_notify_parse_quiet_hours("23:00-07:00", &start,
                                                 &end));
    g_assert_cmpint(start, ==, minutes(23, 0));
    g_assert_cmpint(end, ==, minutes(7, 0));

    /* Spaces around the dash are what people type. */
    g_assert_true(clawt_notify_parse_quiet_hours("09:30 - 17:45", &start,
                                                 &end));
    g_assert_cmpint(start, ==, minutes(9, 30));
    g_assert_cmpint(end, ==, minutes(17, 45));
}

static void
test_nonsense_is_not_a_range(void)
{
    gint start = 0;
    gint end = 0;

    g_assert_false(clawt_notify_parse_quiet_hours(NULL, &start, &end));
    g_assert_false(clawt_notify_parse_quiet_hours("", &start, &end));
    g_assert_false(clawt_notify_parse_quiet_hours("23:00", &start, &end));
    g_assert_false(clawt_notify_parse_quiet_hours("25:00-07:00", &start,
                                                  &end));
    g_assert_false(clawt_notify_parse_quiet_hours("23:60-07:00", &start,
                                                  &end));
    g_assert_false(clawt_notify_parse_quiet_hours("night-morning", &start,
                                                  &end));
    g_assert_false(clawt_notify_parse_quiet_hours("23:00-07:00-09:00",
                                                  &start, &end));
}

/*
 * The one that matters.  People sleep across midnight, so the wrapping
 * range is the ordinary spelling -- and getting it backwards produces a
 * notifier that is silent all day and loud all night, which is the same
 * bug either way round and only noticed at 3am.
 */
static void
test_a_range_that_wraps_midnight(void)
{
    gint start = minutes(23, 0);
    gint end = minutes(7, 0);

    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(23, 0)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(23, 59)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(0, 0)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(3, 0)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(6, 59)));

    /* And loud for the rest of it. */
    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(7, 0)));
    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(12, 0)));
    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(22, 59)));
}

static void
test_a_range_that_does_not_wrap(void)
{
    gint start = minutes(9, 0);
    gint end = minutes(17, 0);

    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(8, 59)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(9, 0)));
    g_assert_true(clawt_notify_in_quiet_hours(start, end, minutes(16, 59)));

    /* The end is exclusive, so 17:00 is audible again. */
    g_assert_false(clawt_notify_in_quiet_hours(start, end, minutes(17, 0)));
}

/* ── Which events ────────────────────────────────────────────────── */

static void
test_events_are_read_from_names(void)
{
    static const gchar *const both[] = { "question", "error", NULL };
    static const gchar *const one[] = { "done", NULL };
    static const gchar *const none[] = { NULL };
    g_autoptr(GError) error = NULL;

    g_assert_cmpuint(clawt_notify_events_from_strv(both, &error), ==,
                     CLAWT_NOTIFY_EVENTS_QUESTION |
                     CLAWT_NOTIFY_EVENTS_ERROR);
    g_assert_no_error(error);

    g_assert_cmpuint(clawt_notify_events_from_strv(one, &error), ==,
                     CLAWT_NOTIFY_EVENTS_DONE);
    g_assert_cmpuint(clawt_notify_events_from_strv(none, &error), ==,
                     CLAWT_NOTIFY_EVENTS_NONE);
    g_assert_cmpuint(clawt_notify_events_from_strv(NULL, &error), ==,
                     CLAWT_NOTIFY_EVENTS_NONE);
}

/*
 * A name nobody recognises is an error rather than a silent omission.
 * The failure it prevents is a notifier that looks configured and never
 * fires.
 */
static void
test_an_unknown_event_is_refused(void)
{
    static const gchar *const wrong[] = { "question", "shouting", NULL };
    g_autoptr(GError) error = NULL;

    g_assert_cmpuint(clawt_notify_events_from_strv(wrong, &error), ==,
                     CLAWT_NOTIFY_EVENTS_NONE);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "shouting"));
    g_assert_nonnull(strstr(error->message, "question"));
}

/*
 * These are flags, not an enum, and g_enum_get_value_by_nick() asserts
 * on a flags type rather than returning nothing.  That is how every
 * notifier came to be unable to parse its own event list while the test
 * button worked perfectly -- so the parser is exercised here directly.
 */
static void
test_the_flags_parser_is_the_flags_parser(void)
{
    guint value = 0;

    g_assert_true(clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS,
                                        "question", &value));
    g_assert_cmpuint(value, ==, CLAWT_NOTIFY_EVENTS_QUESTION);

    /* Case-insensitively, so a config file is not a spelling test. */
    g_assert_true(clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS, "ERROR",
                                        &value));
    g_assert_cmpuint(value, ==, CLAWT_NOTIFY_EVENTS_ERROR);

    g_assert_false(clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS, "nope",
                                         &value));
    g_assert_false(clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS, NULL,
                                         &value));
}

/* ── Priorities ──────────────────────────────────────────────────── */

static void
test_priorities_map_onto_each_backend(void)
{
    g_assert_cmpstr(clawt_notify_priority_for_ntfy("urgent"), ==, "urgent");
    g_assert_cmpstr(clawt_notify_priority_for_ntfy("high"), ==, "high");
    g_assert_cmpstr(clawt_notify_priority_for_ntfy("low"), ==, "low");

    /* Anything unrecognised is the middle, not a failure. */
    g_assert_cmpstr(clawt_notify_priority_for_ntfy(NULL), ==, "default");
    g_assert_cmpstr(clawt_notify_priority_for_ntfy("shouty"), ==, "default");

    /*
     * gotify treats 8 and above as demanding attention -- that is what
     * turns off auto-dismiss on its Android client, so urgent has to
     * clear it or it means nothing.
     */
    g_assert_cmpint(clawt_notify_priority_for_gotify("urgent"), >=, 8);
    g_assert_cmpint(clawt_notify_priority_for_gotify("high"), <, 8);
    g_assert_cmpint(clawt_notify_priority_for_gotify("normal"), ==, 5);
    g_assert_cmpint(clawt_notify_priority_for_gotify("low"), <, 5);

    /* freedesktop urgency: 0 low, 1 normal, 2 critical. */
    g_assert_cmpuint(clawt_notify_priority_for_desktop("low"), ==, 0);
    g_assert_cmpuint(clawt_notify_priority_for_desktop("normal"), ==, 1);
    g_assert_cmpuint(clawt_notify_priority_for_desktop("urgent"), ==, 2);
}

/* ── The line a lock screen shows ────────────────────────────────── */

static void
test_a_summary_is_one_line(void)
{
    g_autofree gchar *wrapped =
        clawt_notify_summarize("First line.\n\nSecond   line.\n", 0);

    g_assert_cmpstr(wrapped, ==, "First line. Second line.");
}

/*
 * A model's answer often opens with a code fence, and the first hundred
 * characters of a shell script tell a person nothing about why they are
 * being interrupted.
 */
static void
test_code_fences_go_first(void)
{
    g_autofree gchar *summary = clawt_notify_summarize(
        "```bash\nrm -rf /very/important\n```\nShall I run this?", 0);

    g_assert_null(strstr(summary, "rm -rf"));
    g_assert_nonnull(strstr(summary, "Shall I run this?"));
}

static void
test_a_long_answer_is_cut_and_says_so(void)
{
    g_autofree gchar *long_text = g_strnfill(400, 'a');
    g_autofree gchar *summary = clawt_notify_summarize(long_text, 20);
    g_autofree gchar *short_summary =
        clawt_notify_summarize("Short.", 20);

    /* The ellipsis only appears when something was actually left out. */
    g_assert_nonnull(strstr(summary, "\342\200\246"));
    g_assert_null(strstr(short_summary, "\342\200\246"));

    {
        g_autofree gchar *nothing = clawt_notify_summarize(NULL, 0);

        g_assert_cmpstr(nothing, ==, "");
    }
}

/* ── The command backend ─────────────────────────────────────────── */

static void
test_a_program_with_no_placeholders_gets_the_text(void)
{
    g_autoptr(ClawtNotification) notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_QUESTION, "researcher", "Researcher",
        "is waiting", "on an answer");
    g_auto(GStrv) argv = NULL;

    /*
     * This is what makes `command: receipt-print` work with nothing else
     * written down, which is the whole reason the backend exists.
     */
    argv = clawt_notify_expand_argv("receipt-print", NULL, notification);

    g_assert_cmpstr(argv[0], ==, "receipt-print");
    g_assert_cmpstr(argv[1], ==, "is waiting");
    g_assert_cmpstr(argv[2], ==, "on an answer");
    g_assert_null(argv[3]);
}

static void
test_placeholders_are_substituted_where_they_appear(void)
{
    g_autoptr(ClawtNotification) notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_ERROR, "scribe", "Scribe", "broke", "badly");
    static const gchar *const args[] = {
        "--from", "{{agent}}", "--subject", "{{title}}", "{{body}}", NULL
    };
    g_auto(GStrv) argv = NULL;

    argv = clawt_notify_expand_argv("send", args, notification);

    g_assert_cmpstr(argv[0], ==, "send");
    g_assert_cmpstr(argv[1], ==, "--from");
    g_assert_cmpstr(argv[2], ==, "scribe");
    g_assert_cmpstr(argv[3], ==, "--subject");
    g_assert_cmpstr(argv[4], ==, "broke");
    g_assert_cmpstr(argv[5], ==, "badly");

    /* Nothing appended, because the placeholders said where it goes. */
    g_assert_null(argv[6]);
}

/*
 * A brace pair that is not one of ours is somebody's literal text, and
 * eating it would silently change the command they wrote.
 */
static void
test_an_unknown_placeholder_is_left_alone(void)
{
    g_autoptr(ClawtNotification) notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_QUESTION, "a", "A", "t", "b");
    static const gchar *const args[] = { "{{title}}", "{{elsewhere}}", NULL };
    g_auto(GStrv) argv = NULL;

    argv = clawt_notify_expand_argv("x", args, notification);

    g_assert_cmpstr(argv[1], ==, "t");
    g_assert_cmpstr(argv[2], ==, "{{elsewhere}}");
}

static void
test_an_empty_body_appends_nothing(void)
{
    g_autoptr(ClawtNotification) notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_QUESTION, "a", "A", "just a title", NULL);
    g_auto(GStrv) argv = NULL;

    argv = clawt_notify_expand_argv("say", NULL, notification);

    g_assert_cmpstr(argv[1], ==, "just a title");
    g_assert_null(argv[2]);
}

/* ── Apprise ─────────────────────────────────────────────────────── */

#define APPRISE_URL "tgram://123456:SECRET-BOT-TOKEN/987654"

/*
 * The URL carries the service's token, and an argv is world-readable
 * through /proc -- so the negative assertion, as for ClawtConnectorPlan:
 * the URL is in the 0600 file and nowhere in the command line.
 */
static void
test_apprise_keeps_the_url_off_argv(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *path = NULL;
    g_autofree gchar *contents = NULL;
    g_auto(GStrv) argv = NULL;
    struct stat st;
    guint i;

    path = clawt_notify_apprise_write_config(APPRISE_URL "\n", &error);
    g_assert_no_error(error);
    g_assert_nonnull(path);

    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, APPRISE_URL "\n");

    argv = clawt_notify_apprise_argv(NULL, path, "researcher is waiting");

    g_assert_cmpstr(argv[0], ==, "apprise");
    g_assert_cmpstr(argv[1], ==, "--config");
    g_assert_cmpstr(argv[2], ==, path);
    g_assert_cmpstr(argv[3], ==, "--title");
    g_assert_cmpstr(argv[4], ==, "researcher is waiting");
    g_assert_null(argv[5]);

    for (i = 0; argv[i] != NULL; i++) {
        g_assert_null(strstr(argv[i], "SECRET-BOT-TOKEN"));
        g_assert_null(strstr(argv[i], "tgram://"));
    }

    g_unlink(path);
}

typedef struct {
    gboolean  done;
    gboolean  ok;
    gchar    *message;
} SendResult;

static void
on_sent(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SendResult *sent = user_data;
    g_autoptr(GError) error = NULL;

    sent->ok = clawt_notify_send_finish(result, &error);
    sent->message = (error != NULL) ? g_strdup(error->message) : NULL;
    sent->done = TRUE;
}

static gboolean
on_watchdog(gpointer user_data)
{
    *(gboolean *)user_data = TRUE;
    return G_SOURCE_REMOVE;
}

/*
 * Sends through the backend to tests/fixtures/fake-apprise, which writes
 * down what it was given.  Returns the result; @prefix names the files.
 */
static void
send_through_fake(const gchar *prefix, const gchar *exit_status,
                  SendResult *sent)
{
    g_autofree gchar *fake = g_build_filename(CLAWT_TEST_FIXTURES,
                                              "fake-apprise", NULL);
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtNotification) notification = NULL;
    ClawtIntegrationConfig *instance;
    gboolean expired = FALSE;
    guint watchdog;

    yaml = g_strdup_printf(
        "integrations:\n"
        "  - name: phone\n"
        "    type: notify\n"
        "    scope: all\n"
        "    backend: apprise\n"
        "    command: \"%s\"\n", fake);
    config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    instance = clawt_config_get_integration(config, "phone");
    g_assert_nonnull(instance);
    binding = clawt_integration_binding_for_instance(
        instance, clawt_integration_find("notify"), NULL);

    g_setenv("CLAWT_FAKE_APPRISE_OUT", prefix, TRUE);
    g_setenv("CLAWT_FAKE_APPRISE_EXIT", exit_status, TRUE);

    notification = clawt_notification_new(
        CLAWT_NOTIFY_EVENTS_QUESTION, "researcher", "Researcher",
        "researcher is waiting", "Which branch should I rebase onto?");

    /* The URL arrives as the resolved secret, as clawt_notifier_reload()
     * hands it over. */
    clawt_notify_send_async(binding, notification, APPRISE_URL, NULL,
                            on_sent, sent);

    watchdog = g_timeout_add_seconds(20, on_watchdog, &expired);
    while (!sent->done && !expired)
        g_main_context_iteration(NULL, TRUE);
    g_assert_false(expired);
    g_source_remove(watchdog);
}

/*
 * End to end through a stand-in apprise: the URL in a 0600 file named by
 * --config, the body on stdin, the URL on no argument -- and the file
 * gone once apprise has exited, success or not.
 */
static void
test_apprise_sends_through_a_file_and_stdin(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-apprise-XXXXXX", NULL);
    g_autofree gchar *prefix = g_build_filename(dir, "run", NULL);
    g_autofree gchar *argv_path = g_strconcat(prefix, ".argv", NULL);
    g_autofree gchar *stdin_path = g_strconcat(prefix, ".stdin", NULL);
    g_autofree gchar *config_path = g_strconcat(prefix, ".config", NULL);
    g_autofree gchar *mode_path = g_strconcat(prefix, ".mode", NULL);
    g_autofree gchar *argv_text = NULL;
    g_autofree gchar *stdin_text = NULL;
    g_autofree gchar *config_text = NULL;
    g_autofree gchar *mode_text = NULL;
    g_auto(GStrv) args = NULL;
    SendResult sent = { FALSE, FALSE, NULL };
    SendResult refused = { FALSE, FALSE, NULL };

    send_through_fake(prefix, "0", &sent);
    g_assert_true(sent.ok);

    g_assert_true(g_file_get_contents(argv_path, &argv_text, NULL, NULL));
    g_assert_true(g_file_get_contents(stdin_path, &stdin_text, NULL, NULL));
    g_assert_true(g_file_get_contents(config_path, &config_text, NULL, NULL));
    g_assert_true(g_file_get_contents(mode_path, &mode_text, NULL, NULL));

    g_assert_null(strstr(argv_text, "SECRET-BOT-TOKEN"));
    g_assert_null(strstr(argv_text, "Which branch"));
    g_assert_cmpstr(stdin_text, ==, "Which branch should I rebase onto?");
    g_assert_cmpstr(config_text, ==, APPRISE_URL);
    g_assert_cmpstr(g_strstrip(mode_text), ==, "600");

    /*
     * The file it named is gone now that apprise has exited.  The
     * fixture records "$@", which starts after the program name.
     */
    args = g_strsplit(argv_text, "\n", -1);
    g_assert_cmpstr(args[0], ==, "--config");
    g_assert_false(g_file_test(args[1], G_FILE_TEST_EXISTS));

    /* A refusal says what apprise said, and still removes the file. */
    send_through_fake(prefix, "1", &refused);
    g_assert_false(refused.ok);
    g_assert_nonnull(strstr(refused.message, "unauthorized"));
    g_assert_null(strstr(refused.message, "SECRET-BOT-TOKEN"));
    {
        g_autofree gchar *again = NULL;
        g_auto(GStrv) second = NULL;

        g_assert_true(g_file_get_contents(argv_path, &again, NULL, NULL));
        second = g_strsplit(again, "\n", -1);
        g_assert_false(g_file_test(second[1], G_FILE_TEST_EXISTS));
    }

    g_unsetenv("CLAWT_FAKE_APPRISE_OUT");
    g_unsetenv("CLAWT_FAKE_APPRISE_EXIT");
    g_free(sent.message);
    g_free(refused.message);
    clawt_test_remove_tree(dir);
}

/* Without a URL there is nothing to send, and it says which key. */
static void
test_apprise_without_a_url_is_refused(void)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtIntegrationBinding) binding = NULL;
    g_autoptr(ClawtNotification) notification = NULL;
    SendResult sent = { FALSE, FALSE, NULL };

    config = clawt_config_load_from_string(
        "integrations:\n"
        "  - name: phone\n"
        "    type: notify\n"
        "    scope: all\n"
        "    backend: apprise\n", &error);
    g_assert_no_error(error);
    binding = clawt_integration_binding_for_instance(
        clawt_config_get_integration(config, "phone"),
        clawt_integration_find("notify"), NULL);
    notification = clawt_notification_new(CLAWT_NOTIFY_EVENTS_ERROR, NULL,
                                          NULL, "t", "b");

    clawt_notify_send_async(binding, notification, NULL, NULL, on_sent,
                            &sent);
    while (!sent.done)
        g_main_context_iteration(NULL, TRUE);

    g_assert_false(sent.ok);
    g_assert_nonnull(strstr(sent.message, "apprise_url"));
    g_free(sent.message);
}

static void
on_tested(GObject *source, GAsyncResult *result, gpointer user_data)
{
    SendResult *sent = user_data;
    g_autoptr(GError) error = NULL;

    sent->ok = clawt_notifier_test_finish(CLAWT_NOTIFIER(source), result,
                                          &error);
    sent->message = (error != NULL) ? g_strdup(error->message) : NULL;
    sent->done = TRUE;
}

/*
 * The daemon's half: the notifier resolves `apprise_url` -- not `token`
 * -- for an apprise instance, and `integration test` reaches apprise
 * with it.  Grepped for the caller, since a resolver keyed on the wrong
 * name would pass every test above and send nothing.
 */
static void
test_the_notifier_resolves_apprise_url(void)
{
    g_autofree gchar *fake = g_build_filename(CLAWT_TEST_FIXTURES,
                                              "fake-apprise", NULL);
    g_autofree gchar *dir = g_dir_make_tmp("clawt-apprise-XXXXXX", NULL);
    g_autofree gchar *prefix = g_build_filename(dir, "run", NULL);
    g_autofree gchar *config_path = g_strconcat(prefix, ".config", NULL);
    g_autofree gchar *config_text = NULL;
    g_autofree gchar *yaml = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtConfig) config = NULL;
    g_autoptr(ClawtNotifier) notifier = NULL;
    SendResult sent = { FALSE, FALSE, NULL };
    gboolean expired = FALSE;
    guint watchdog;

    g_setenv("CLAWT_TEST_APPRISE_URL", APPRISE_URL, TRUE);
    g_setenv("CLAWT_FAKE_APPRISE_OUT", prefix, TRUE);
    g_setenv("CLAWT_FAKE_APPRISE_EXIT", "0", TRUE);

    yaml = g_strdup_printf(
        "integrations:\n"
        "  - name: phone\n"
        "    type: notify\n"
        "    scope: all\n"
        "    backend: apprise\n"
        "    apprise_url: {env: CLAWT_TEST_APPRISE_URL}\n"
        "    command: \"%s\"\n", fake);
    config = clawt_config_load_from_string(yaml, &error);
    g_assert_no_error(error);

    notifier = clawt_notifier_new(config);
    clawt_notifier_test_async(notifier, "phone", NULL, on_tested, &sent);

    watchdog = g_timeout_add_seconds(20, on_watchdog, &expired);
    while (!sent.done && !expired)
        g_main_context_iteration(NULL, TRUE);
    g_assert_false(expired);
    g_source_remove(watchdog);

    g_assert_true(sent.ok);
    g_assert_true(g_file_get_contents(config_path, &config_text, NULL, NULL));
    g_assert_cmpstr(config_text, ==, APPRISE_URL);

    g_unsetenv("CLAWT_TEST_APPRISE_URL");
    g_unsetenv("CLAWT_FAKE_APPRISE_OUT");
    g_unsetenv("CLAWT_FAKE_APPRISE_EXIT");
    g_free(sent.message);
    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/notify/quiet-hours-parse", test_a_range_is_parsed);
    g_test_add_func("/notify/quiet-hours-nonsense",
                    test_nonsense_is_not_a_range);
    g_test_add_func("/notify/quiet-hours-wrapping",
                    test_a_range_that_wraps_midnight);
    g_test_add_func("/notify/quiet-hours-daytime",
                    test_a_range_that_does_not_wrap);
    g_test_add_func("/notify/events", test_events_are_read_from_names);
    g_test_add_func("/notify/events-unknown",
                    test_an_unknown_event_is_refused);
    g_test_add_func("/notify/flags-parser",
                    test_the_flags_parser_is_the_flags_parser);
    g_test_add_func("/notify/priorities",
                    test_priorities_map_onto_each_backend);
    g_test_add_func("/notify/summary", test_a_summary_is_one_line);
    g_test_add_func("/notify/summary-code", test_code_fences_go_first);
    g_test_add_func("/notify/summary-long",
                    test_a_long_answer_is_cut_and_says_so);
    g_test_add_func("/notify/command-plain",
                    test_a_program_with_no_placeholders_gets_the_text);
    g_test_add_func("/notify/command-placeholders",
                    test_placeholders_are_substituted_where_they_appear);
    g_test_add_func("/notify/command-unknown-placeholder",
                    test_an_unknown_placeholder_is_left_alone);
    g_test_add_func("/notify/apprise/url-off-argv",
                    test_apprise_keeps_the_url_off_argv);
    g_test_add_func("/notify/apprise/file-and-stdin",
                    test_apprise_sends_through_a_file_and_stdin);
    g_test_add_func("/notify/apprise/needs-a-url",
                    test_apprise_without_a_url_is_refused);
    g_test_add_func("/notify/apprise/notifier-resolves-apprise-url",
                    test_the_notifier_resolves_apprise_url);
    g_test_add_func("/notify/command-empty-body",
                    test_an_empty_body_appends_nothing);

    return g_test_run();
}
