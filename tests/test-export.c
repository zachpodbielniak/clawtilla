/*
 * test-export.c - Taking a conversation somewhere else
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

static GPtrArray *
two_turns(void)
{
    GPtrArray *messages = g_ptr_array_new_with_free_func(
        (GDestroyNotify)clawt_message_free);
    ClawtMessage *asked = clawt_message_new("dm:user:scribe", "user",
                                            "why is the build flaky?");
    ClawtMessage *answered = clawt_message_new(
        "dm:user:scribe", "scribe",
        "The **lease deadline** races the sweep.\n\n"
        "Reproduce with `make test`.\n\n"
        "- check `lease_seconds`\n- check the sweep interval\n");

    g_ptr_array_add(messages, asked);
    g_ptr_array_add(messages, answered);

    return messages;
}

static void
test_markdown_needs_nothing_installed(void)
{
    g_autoptr(GPtrArray) messages = two_turns();
    g_autofree gchar *out = NULL;

    g_assert_true(clawt_export_available(CLAWT_EXPORT_MARKDOWN));

    out = clawt_export_transcript("dm:user:scribe", messages,
                                  CLAWT_EXPORT_MARKDOWN, NULL);

    g_assert_nonnull(out);
    g_assert_nonnull(strstr(out, "why is the build flaky?"));
    g_assert_nonnull(strstr(out, "**lease deadline**"));

    /* Who said it, and when, are part of the document. */
    g_assert_nonnull(strstr(out, "## user"));
    g_assert_nonnull(strstr(out, "## scribe"));

    g_assert_cmpstr(clawt_export_format_extension(CLAWT_EXPORT_MARKDOWN),
                    ==, ".md");
}

/*
 * The formats that need pandoc say so rather than quietly handing back
 * markdown under an org label.
 */
static void
test_a_missing_pandoc_is_reported(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *out = NULL;

    if (clawt_export_available(CLAWT_EXPORT_ORG)) {
        g_test_skip("pandoc is installed here");
        return;
    }

    out = clawt_export_convert("hello", CLAWT_EXPORT_ORG, &error);

    g_assert_null(out);
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_NOT_SUPPORTED);
}

/*
 * Running pandoc needs infrastructure, and its presence does not say so.
 *
 * clawt_export_available() asks g_find_program_in_path(), which answers
 * "there is a file called pandoc on PATH" -- a different question from
 * "running it will convert something".  On a machine where pandoc is a
 * distrobox or toolbox shim it is a two-line shell script that starts a
 * container: the suite then needed podman, reached a registry, and on a
 * host without that image already built *prompted* `[Y/n]` and waited
 * for a person for ever.  A test that can hang is worse than one that
 * fails, because it stops every test after it and looks identical to a
 * slow machine.
 *
 * So the three tests that actually spawn pandoc are integration tests.
 * The fourth is not: it runs only when pandoc is absent, where
 * clawt_export_convert() refuses before spawning anything.
 */
static gboolean
pandoc_may_run(ClawtExportFormat format)
{
    if (g_getenv("CLAWT_TEST_INTEGRATION") == NULL) {
        g_test_skip("needs CLAWT_TEST_INTEGRATION: this runs pandoc");
        return FALSE;
    }

    if (!clawt_export_available(format)) {
        g_test_skip("pandoc is not installed here");
        return FALSE;
    }

    return TRUE;
}

/*
 * One paragraph, one line.
 *
 * pandoc rewraps at 72 columns by default, which turns a paragraph into
 * a block of hard-wrapped lines -- unreadable in an editor that soft
 * wraps and a nuisance to edit afterwards. --wrap=none is the argument
 * that stops it, and this is the test that notices if it is dropped.
 */
static void
test_a_paragraph_stays_one_line(void)
{
    g_autofree gchar *paragraph = NULL;
    g_autofree gchar *converted = NULL;
    g_autoptr(GError) error = NULL;
    g_auto(GStrv) lines = NULL;
    gsize longest = 0;
    gsize i;

    if (!pandoc_may_run(CLAWT_EXPORT_ORG))
        return;

    /* Comfortably past pandoc's default 72-column wrap. */
    paragraph = g_strdup(
        "This is a single paragraph written as one long line, well past "
        "any sensible column limit, so that a converter which rewraps "
        "its output will visibly break it into several shorter lines "
        "and this test will notice that it did.");

    converted = clawt_export_convert(paragraph, CLAWT_EXPORT_ORG, &error);
    g_assert_no_error(error);
    g_assert_nonnull(converted);

    lines = g_strsplit(converted, "\n", -1);

    for (i = 0; lines[i] != NULL; i++)
        longest = MAX(longest, strlen(lines[i]));

    g_assert_cmpuint(longest, >, 100);
}

static void
test_org_looks_like_org(void)
{
    g_autoptr(GPtrArray) messages = two_turns();
    g_autofree gchar *out = NULL;
    g_autoptr(GError) error = NULL;

    if (!pandoc_may_run(CLAWT_EXPORT_ORG))
        return;

    out = clawt_export_transcript("dm:user:scribe", messages,
                                  CLAWT_EXPORT_ORG, &error);
    g_assert_no_error(error);
    g_assert_nonnull(out);

    /* Headings became outline levels and bold became org's markers. */
    g_assert_nonnull(strstr(out, "* dm:user:scribe"));
    g_assert_nonnull(strstr(out, "** user"));
    g_assert_nonnull(strstr(out, "*lease deadline*"));
    /* pandoc writes org verbatim with "=", not "~". Both are org. */
    g_assert_nonnull(strstr(out, "=make test="));

    g_assert_cmpstr(clawt_export_format_extension(CLAWT_EXPORT_ORG),
                    ==, ".org");
}

static void
test_plain_has_no_markup_left(void)
{
    g_autoptr(GPtrArray) messages = two_turns();
    g_autofree gchar *out = NULL;

    if (!pandoc_may_run(CLAWT_EXPORT_PLAIN))
        return;

    out = clawt_export_transcript("dm:user:scribe", messages,
                                  CLAWT_EXPORT_PLAIN, NULL);
    g_assert_nonnull(out);

    /* The words survive; the asterisks and backticks do not. */
    g_assert_nonnull(strstr(out, "lease deadline"));
    g_assert_null(strstr(out, "**"));
    g_assert_null(strstr(out, "`"));

    g_assert_cmpstr(clawt_export_format_extension(CLAWT_EXPORT_PLAIN),
                    ==, ".txt");
}

static void
test_an_empty_conversation_still_exports(void)
{
    g_autoptr(GPtrArray) empty = g_ptr_array_new();
    g_autofree gchar *out = clawt_export_transcript("nobody", empty,
                                                     CLAWT_EXPORT_MARKDOWN,
                                                     NULL);

    g_assert_nonnull(out);
    g_assert_nonnull(strstr(out, "nobody"));
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/export/markdown", test_markdown_needs_nothing_installed);
    g_test_add_func("/export/missing-pandoc-reported",
                    test_a_missing_pandoc_is_reported);
    g_test_add_func("/export/paragraph-stays-one-line",
                    test_a_paragraph_stays_one_line);
    g_test_add_func("/export/org", test_org_looks_like_org);
    g_test_add_func("/export/plain", test_plain_has_no_markup_left);
    g_test_add_func("/export/empty", test_an_empty_conversation_still_exports);

    return g_test_run();
}
