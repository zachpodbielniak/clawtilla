/*
 * test-update.c - Knowing that a newer version exists
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The daemon knew its own version and nothing ever asked whether it was
 * the current one, so an operator found out a fix existed by reading the
 * upstream log by hand -- and a defect was once diagnosed, worked around
 * and written up as unfixed while the fix had been sitting upstream for
 * seven commits.
 *
 * Two halves are worth holding to account without a network, and they
 * are the two that decide what a person is told:
 *
 *   - the comparison, because "0.10.0 is older than 0.9.0" is the
 *     obvious way to get this wrong and it looks exactly like a right
 *     answer;
 *   - reading a version out of whatever the source answered with,
 *     because the source is somebody else's server and this code is
 *     parsing it.
 *
 * The fetch itself is not tested here on purpose: `make test` opens no
 * network socket at all, and a check that reached one from a fixture
 * would be the very thing the daemon deliberately does not do at start.
 */

#include <clawtilla.h>

#include "clawt-test-util.h"

/* ── Comparing ───────────────────────────────────────────────────── */

/*
 * Components are numbers, not text.
 *
 * The case that matters is the two-digit one: as strings "0.10.0" sorts
 * before "0.9.0", so a lexical comparison would report a fleet on 0.9.0
 * as newer than the 0.10.0 release and quietly never mention it again.
 */
static void
test_components_compare_as_numbers(void)
{
    g_assert_cmpint(clawt_update_version_compare("0.10.0", "0.9.0"), ==, 1);
    g_assert_cmpint(clawt_update_version_compare("0.9.0", "0.10.0"), ==, -1);
    g_assert_cmpint(clawt_update_version_compare("1.0.0", "0.99.99"), ==, 1);
    g_assert_cmpint(clawt_update_version_compare("0.2.0", "0.2.0"), ==, 0);
}

/*
 * A leading "v" is not part of the version.
 *
 * A release tag almost always carries one and CLAWT_VERSION_STRING never
 * does, so without this every tagged release compares as a different
 * version from the build asking about it -- which reads as "an update is
 * available" for ever, including immediately after updating.
 */
static void
test_a_tag_prefix_is_ignored(void)
{
    g_assert_cmpint(clawt_update_version_compare("v0.2.0", "0.2.0"), ==, 0);
    g_assert_cmpint(clawt_update_version_compare("0.2.0", "v0.2.0"), ==, 0);
    g_assert_cmpint(clawt_update_version_compare("V0.3.0", "v0.2.0"), ==, 1);
}

/*
 * A missing component is zero, so "0.2" and "0.2.0" are one version.
 */
static void
test_a_missing_component_is_zero(void)
{
    g_assert_cmpint(clawt_update_version_compare("0.2", "0.2.0"), ==, 0);
    g_assert_cmpint(clawt_update_version_compare("0.2.1", "0.2"), ==, 1);
    g_assert_cmpint(clawt_update_version_compare("1", "0.9.9"), ==, 1);
}

/*
 * A pre-release is older than the release it leads to, and neither is
 * newer than the next version.
 */
static void
test_a_prerelease_is_older_than_its_release(void)
{
    g_assert_cmpint(clawt_update_version_compare("0.3.0", "0.3.0-rc1"), ==, 1);
    g_assert_cmpint(clawt_update_version_compare("0.3.0-rc1", "0.3.0"), ==, -1);
    g_assert_cmpint(clawt_update_version_compare("0.4.0", "0.3.0-rc1"), ==, 1);
}

/*
 * A version that does not parse is never newer.
 *
 * The direction is the whole point rather than a tidy-up: the caller
 * spends this as "is what the source said newer than what I am", so a
 * source answering with an error page, an empty string or a name must
 * not be able to produce "an update is available" -- which is a
 * notification, and then a banner in every client, about nothing.
 */
static void
test_rubbish_is_never_newer(void)
{
    g_assert_cmpint(clawt_update_version_compare(NULL, "0.2.0"), ==, -1);
    g_assert_cmpint(clawt_update_version_compare("", "0.2.0"), ==, -1);
    g_assert_cmpint(clawt_update_version_compare("latest", "0.2.0"), ==, -1);
    g_assert_cmpint(clawt_update_version_compare("<html>", "0.2.0"), ==, -1);

    /* And two unparseable ones are equal, not an ordering by accident. */
    g_assert_cmpint(clawt_update_version_compare("latest", "stable"), ==, 0);
}

/* ── Reading the source's answer ─────────────────────────────────── */

static gchar *
version_of(const gchar *json)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;

    g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
    g_assert_no_error(error);

    return clawt_update_version_from_json(json_parser_get_root(parser));
}

/*
 * The three shapes that are actually out there.
 *
 * None of them is ours: a file somebody publishes holds a bare string, a
 * single release is an object, and Forgejo, Gitea and GitLab all answer
 * their releases endpoint with an array.  Hard-coding one would make the
 * URL key a lie about what it accepts.
 */
static void
test_the_three_shapes_are_read(void)
{
    g_autofree gchar *bare = version_of("\"0.3.0\"");
    g_autofree gchar *object = version_of("{\"tag_name\": \"v0.3.0\"}");
    g_autofree gchar *array =
        version_of("[{\"tag_name\": \"v0.3.0\"}, {\"tag_name\": \"v0.2.0\"}]");
    g_autofree gchar *named = version_of("{\"version\": \"0.3.0\"}");

    g_assert_cmpstr(bare, ==, "0.3.0");
    g_assert_cmpstr(object, ==, "v0.3.0");
    g_assert_cmpstr(array, ==, "v0.3.0");
    g_assert_cmpstr(named, ==, "0.3.0");
}

/*
 * An entry at the front that says nothing does not answer for the list.
 *
 * A releases array is newest first, so the temptation is to read element
 * zero.  A draft, or an entry whose name is not a version, would then
 * make a fleet with a release waiting report that there was none.
 */
static void
test_a_useless_first_entry_does_not_answer(void)
{
    g_autofree gchar *skipped = version_of(
        "[{\"name\": \"nightly\"}, {\"tag_name\": \"v0.3.0\"}]");
    g_autofree gchar *not_an_object = version_of(
        "[\"nightly\", {\"tag_name\": \"v0.3.0\"}]");

    g_assert_cmpstr(skipped, ==, "v0.3.0");
    g_assert_cmpstr(not_an_object, ==, "v0.3.0");
}

/*
 * A member that is present and null is not a version.
 *
 * json_object_has_member() is true for it and json_node_get_string() on
 * it is a critical, and this is reading somebody else's server -- so the
 * value's type is checked rather than its presence.
 */
static void
test_a_null_member_is_not_a_version(void)
{
    g_autofree gchar *null_tag =
        version_of("{\"tag_name\": null, \"version\": \"0.3.0\"}");
    g_autofree gchar *numeric = version_of("{\"tag_name\": 3}");

    g_assert_cmpstr(null_tag, ==, "0.3.0");
    g_assert_null(numeric);
}

/*
 * Anything else is nothing, not an error and not a version.
 *
 * A source that answered with something this code does not understand
 * has to reach the caller as a failed check, which is reported.  Guessing
 * would put a made-up version in front of somebody.
 */
static void
test_an_unrecognised_answer_yields_nothing(void)
{
    g_autofree gchar *empty_array = version_of("[]");
    g_autofree gchar *wrong_object = version_of("{\"status\": \"ok\"}");
    g_autofree gchar *number = version_of("42");

    g_assert_null(empty_array);
    g_assert_null(wrong_object);
    g_assert_null(number);
    g_assert_null(clawt_update_version_from_json(NULL));
}

/* ── What a client is told ───────────────────────────────────────── */

/*
 * A checker that has asked nothing says so, and says nothing is
 * available.
 *
 * `checked_at` is written even at zero on purpose.  A client with no
 * member to read draws nothing, and nothing reads as "up to date" -- so
 * a check that has been quietly erroring for a month would be worse than
 * having no check at all.
 */
static void
test_a_checker_that_has_not_asked_says_so(void)
{
    g_autoptr(ClawtUpdateCheck) check =
        clawt_update_check_new("0.2.0", "https://example.invalid/v", 24);
    g_autoptr(JsonBuilder) builder = json_builder_new();
    g_autoptr(JsonNode) root = NULL;
    JsonObject *update;

    json_builder_begin_object(builder);
    clawt_update_check_describe(check, builder);
    json_builder_end_object(builder);

    root = json_builder_get_root(builder);
    update = json_object_get_object_member(json_node_get_object(root),
                                           "update");

    g_assert_false(json_object_get_boolean_member(update, "available"));
    g_assert_true(json_object_has_member(update, "checked_at"));
    g_assert_cmpint(json_object_get_int_member(update, "checked_at"), ==, 0);
    g_assert_null(clawt_update_check_get_latest(check));

    /* Nothing has failed yet either, so there is no error to report. */
    g_assert_false(json_object_has_member(update, "error"));
}

/*
 * The update line is below the version mismatch, and both are below a
 * connection that is not up.
 *
 * A client and a daemon that disagree is broken now; an update is better
 * later.  And telling somebody about an update to a daemon they cannot
 * reach is advice about the wrong problem.
 */
static void
test_the_notice_puts_the_update_last(void)
{
    g_autoptr(ClawtConnection) local =
        clawt_connection_new_local("this machine", NULL);
    g_autofree gchar *only_update = NULL;
    g_autofree gchar *mismatch_wins = NULL;
    g_autofree gchar *down = NULL;
    g_autofree gchar *quiet = NULL;

    only_update = clawt_connection_notice_text(CLAWT_DAEMON_LINK_UP, local,
                                               NULL, "0.3.0");
    g_assert_nonnull(only_update);
    g_assert_nonnull(strstr(only_update, "0.3.0"));

    /* A mismatch outranks it, and must not mention the update. */
    mismatch_wins = clawt_connection_notice_text(CLAWT_DAEMON_LINK_UP, local,
                                                 "99.0.0", "0.3.0");
    g_assert_nonnull(mismatch_wins);
    g_assert_null(strstr(mismatch_wins, "0.3.0"));

    /* A connection that never came up outranks both. */
    down = clawt_connection_notice_text(CLAWT_DAEMON_LINK_NEVER, local, NULL,
                                        "0.3.0");
    g_assert_nonnull(down);
    g_assert_null(strstr(down, "0.3.0"));

    /* And with nothing to say it still says nothing. */
    quiet = clawt_connection_notice_text(CLAWT_DAEMON_LINK_UP, local, NULL,
                                         NULL);
    g_assert_null(quiet);

    /* An empty string is not a version either. */
    quiet = clawt_connection_notice_text(CLAWT_DAEMON_LINK_UP, local, NULL,
                                         "");
    g_assert_null(quiet);
}

/*
 * "update" is a thing somebody can ask to be notified about.
 *
 * Asked of the type rather than of a list written here, because the
 * refusal message that tells a person what they may type is now built
 * from the same type -- so this covers both, and a member added later
 * without a nickname fails here rather than in a message.
 */
static void
test_update_is_a_notifiable_event(void)
{
    guint value = 0;
    g_autofree gchar *offered =
        clawt_flags_list_nicks(CLAWT_TYPE_NOTIFY_EVENTS);

    g_assert_true(clawt_flags_from_nick(CLAWT_TYPE_NOTIFY_EVENTS, "update",
                                        &value));
    g_assert_cmpuint(value, ==, CLAWT_NOTIFY_EVENTS_UPDATE);

    /* And the sentence a refusal shows names it. */
    g_assert_nonnull(strstr(offered, "update"));
    g_assert_nonnull(strstr(offered, "question"));
    g_assert_nonnull(strstr(offered, " or "));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/update/components-compare-as-numbers",
                    test_components_compare_as_numbers);
    g_test_add_func("/update/a-tag-prefix-is-ignored",
                    test_a_tag_prefix_is_ignored);
    g_test_add_func("/update/a-missing-component-is-zero",
                    test_a_missing_component_is_zero);
    g_test_add_func("/update/a-prerelease-is-older",
                    test_a_prerelease_is_older_than_its_release);
    g_test_add_func("/update/rubbish-is-never-newer",
                    test_rubbish_is_never_newer);
    g_test_add_func("/update/the-three-shapes-are-read",
                    test_the_three_shapes_are_read);
    g_test_add_func("/update/a-useless-first-entry-does-not-answer",
                    test_a_useless_first_entry_does_not_answer);
    g_test_add_func("/update/a-null-member-is-not-a-version",
                    test_a_null_member_is_not_a_version);
    g_test_add_func("/update/an-unrecognised-answer-yields-nothing",
                    test_an_unrecognised_answer_yields_nothing);
    g_test_add_func("/update/a-checker-that-has-not-asked-says-so",
                    test_a_checker_that_has_not_asked_says_so);
    g_test_add_func("/update/the-notice-puts-the-update-last",
                    test_the_notice_puts_the_update_last);
    g_test_add_func("/update/update-is-a-notifiable-event",
                    test_update_is_a_notifiable_event);

    return g_test_run();
}
