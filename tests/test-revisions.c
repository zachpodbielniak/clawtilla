/*
 * test-revisions.c - The version store and its diff
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * - a save that changed nothing is not a version; one that did is the
 *   newest, read back whole, listed without its text, filtered by kind
 *   and subject; too large is refused; prune keeps the newest per subject;
 * - skill test runs: started, found by task, finished, the last per test;
 * - the diff: identical texts, a change in the middle with context, an
 *   insertion at the top, a deletion at the end, two changes far apart as
 *   two hunks, and the size guard;
 * - a skill test's verdict: expect and reject, case-insensitively.
 */

#include "clawtilla.h"
#include "clawt-test-util.h"
#include "core/clawt-daemon-private.h"

#include <string.h>

static ClawtRevisions *
open_store(gchar **out_dir)
{
    g_autoptr(GError) error = NULL;
    gchar *dir = g_dir_make_tmp("clawt-revisions-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "revisions.db", NULL);
    ClawtRevisions *store = clawt_revisions_new(path, &error);

    g_assert_no_error(error);
    *out_dir = dir;
    return store;
}

static void
test_record_and_read(void)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtRevisions) store = open_store(&dir);
    g_autoptr(GError) error = NULL;
    g_autoptr(JsonNode) all = NULL;
    g_autoptr(JsonNode) one = NULL;
    g_autofree gchar *text = NULL;
    g_autofree gchar *kind = NULL;
    g_autofree gchar *subject = NULL;
    gint64 first;
    gint64 same;
    gint64 second;
    JsonObject *newest;

    first = clawt_revisions_record(store, "persona", "scout/SOUL.org",
                                   "one\n", "user", NULL, &error);
    g_assert_cmpint(first, >, 0);

    /* nothing changed: the same id, no new row */
    same = clawt_revisions_record(store, "persona", "scout/SOUL.org",
                                  "one\n", "user", NULL, &error);
    g_assert_cmpint(same, ==, first);

    second = clawt_revisions_record(store, "persona", "scout/SOUL.org",
                                    "two\n", "scout", "edited", &error);
    g_assert_cmpint(second, >, first);
    clawt_revisions_record(store, "config", "config", "agents: []\n", NULL,
                           NULL, &error);
    g_assert_no_error(error);

    text = clawt_revisions_get(store, first, &kind, &subject);
    g_assert_cmpstr(text, ==, "one\n");
    g_assert_cmpstr(kind, ==, "persona");
    g_assert_cmpstr(subject, ==, "scout/SOUL.org");
    g_assert_null(clawt_revisions_get(store, 9999, NULL, NULL));

    all = clawt_revisions_list(store, NULL, NULL, 0);
    g_assert_cmpuint(json_array_get_length(json_node_get_array(all)), ==, 3);

    one = clawt_revisions_list(store, "persona", "scout/SOUL.org", 0);
    g_assert_cmpuint(json_array_get_length(json_node_get_array(one)), ==, 2);
    newest = json_array_get_object_element(json_node_get_array(one), 0);
    g_assert_cmpint(json_object_get_int_member(newest, "id"), ==, second);
    g_assert_cmpstr(json_object_get_string_member(newest, "author"), ==,
                    "scout");
    g_assert_cmpstr(json_object_get_string_member(newest, "note"), ==,
                    "edited");
    g_assert_cmpint(json_object_get_int_member(newest, "bytes"), ==, 4);
    g_assert_false(json_object_has_member(newest, "text"));
    g_assert_cmpint(clawt_revisions_newest_id(store, "persona",
                                              "scout/SOUL.org"), ==, second);

    /* too large: refused, not cut */
    {
        gchar *huge = g_malloc(CLAWT_REVISIONS_MAX_BYTES + 2);

        memset(huge, 'x', CLAWT_REVISIONS_MAX_BYTES + 1);
        huge[CLAWT_REVISIONS_MAX_BYTES + 1] = '\0';
        g_assert_cmpint(clawt_revisions_record(store, "skill", "big", huge,
                                               NULL, NULL, &error), ==, 0);
        g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
        g_clear_error(&error);
        g_free(huge);
    }

    /* prune: the newest per subject stays */
    g_assert_cmpuint(clawt_revisions_prune(store, 1), ==, 1);
    g_assert_null(clawt_revisions_get(store, first, NULL, NULL));
    {
        g_autofree gchar *kept = clawt_revisions_get(store, second, NULL,
                                                     NULL);

        g_assert_cmpstr(kept, ==, "two\n");
    }

    clawt_test_remove_tree(dir);
}

static void
test_skill_test_runs(void)
{
    g_autofree gchar *dir = NULL;
    g_autoptr(ClawtRevisions) store = open_store(&dir);
    g_autoptr(JsonNode) last = NULL;
    g_autofree gchar *skill = NULL;
    g_autofree gchar *test = NULL;
    JsonObject *object;

    g_assert_null(clawt_revisions_last_test_run(store, "release", "basic"));
    g_assert_cmpint(clawt_revisions_start_test_run(store, "release", "basic",
                                                   7, "scout", "task-1"),
                    >, 0);
    g_assert_true(clawt_revisions_test_run_for_task(store, "task-1", &skill,
                                                    &test));
    g_assert_cmpstr(skill, ==, "release");
    g_assert_cmpstr(test, ==, "basic");
    g_assert_false(clawt_revisions_test_run_for_task(store, "task-2", NULL,
                                                     NULL));

    clawt_revisions_finish_test_run(store, "task-1", "passed", "fine");
    g_assert_false(clawt_revisions_test_run_for_task(store, "task-1", NULL,
                                                     NULL));

    last = clawt_revisions_last_test_run(store, "release", "basic");
    object = json_node_get_object(last);
    g_assert_cmpstr(json_object_get_string_member(object, "state"), ==,
                    "passed");
    g_assert_cmpint(json_object_get_int_member(object, "revision"), ==, 7);
    g_assert_cmpstr(json_object_get_string_member(object, "task"), ==,
                    "task-1");

    clawt_test_remove_tree(dir);
}

static void
test_diff(void)
{
    g_autofree gchar *same = clawt_revisions_diff("a\nb\n", "a\nb\n", "x",
                                                  "y");
    g_autofree gchar *middle = clawt_revisions_diff(
        "1\n2\n3\n4\n5\n6\n7\n8\n9\n", "1\n2\n3\n4\nFIVE\n6\n7\n8\n9\n",
        "old", "new");
    g_autofree gchar *top = clawt_revisions_diff("b\nc\n", "a\nb\nc\n", "o",
                                                 "n");
    g_autofree gchar *end = clawt_revisions_diff("a\nb\nc\n", "a\nb\n", "o",
                                                 "n");
    g_autoptr(GString) left = g_string_new(NULL);
    g_autoptr(GString) right = NULL;
    g_autofree gchar *apart = NULL;
    guint i;

    g_assert_cmpstr(same, ==, "");

    g_assert_cmpstr(middle, ==,
        "--- old\n+++ new\n"
        "@@ -2,7 +2,7 @@\n"
        " 2\n 3\n 4\n-5\n+FIVE\n 6\n 7\n 8\n");

    g_assert_cmpstr(top, ==, "--- o\n+++ n\n@@ -1,2 +1,3 @@\n+a\n b\n c\n");
    g_assert_cmpstr(end, ==, "--- o\n+++ n\n@@ -1,3 +1,2 @@\n a\n b\n-c\n");

    /* two changes forty lines apart are two hunks */
    for (i = 0; i < 50; i++)
        g_string_append_printf(left, "line %u\n", i);
    right = g_string_new(left->str);
    g_string_replace(right, "line 5\n", "line five\n", 1);
    g_string_replace(right, "line 45\n", "line forty-five\n", 1);
    apart = clawt_revisions_diff(left->str, right->str, "a", "b");
    g_assert_nonnull(strstr(apart, "@@ -3,7 +3,7 @@"));
    g_assert_nonnull(strstr(apart, "@@ -43,7 +43,7 @@"));
    g_assert_nonnull(strstr(apart, "+line forty-five"));

    /* the size guard */
    {
        g_autoptr(GString) a = g_string_new(NULL);
        g_autoptr(GString) b = g_string_new(NULL);
        g_autofree gchar *guarded = NULL;

        for (i = 0; i < 4000; i++) {
            g_string_append_printf(a, "a%u\n", i);
            g_string_append_printf(b, "b%u\n", i);
        }
        guarded = clawt_revisions_diff(a->str, b->str, "a", "b");
        g_assert_nonnull(strstr(guarded, "too large to compare"));
    }
}

static void
test_skill_test_verdict(void)
{
    static const gchar *const expect[] = { "## Fixes", "release", NULL };
    static const gchar *const reject[] = { "TODO", NULL };
    g_autofree gchar *passed = clawt_daemon_skill_test_verdict(
        "## fixes\nThe Release went out.", expect, reject);
    g_autofree gchar *missing = clawt_daemon_skill_test_verdict(
        "## Fixes only", expect, reject);
    g_autofree gchar *rejected = clawt_daemon_skill_test_verdict(
        "## Fixes in this release. todo: more", expect, reject);

    g_assert_null(passed);
    g_assert_nonnull(strstr(missing, "\"release\""));
    g_assert_nonnull(strstr(rejected, "\"TODO\""));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/revisions/record-and-read", test_record_and_read);
    g_test_add_func("/revisions/skill-test-runs", test_skill_test_runs);
    g_test_add_func("/revisions/diff", test_diff);
    g_test_add_func("/revisions/skill-test-verdict", test_skill_test_verdict);

    return g_test_run();
}
