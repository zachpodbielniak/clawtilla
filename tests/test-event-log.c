/*
 * test-event-log.c - Reading events back without reading everything
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * clawt_event_log_read() is called from an IPC handler, on the daemon's
 * main context, with the requesting client blocked and every agent
 * waiting behind it.  It used to open every retained day file, parse
 * every line into a ClawtEvent, and only then throw all but the last
 * @limit away -- so a fleet publishing fifty thousand events a day, with
 * the default thirty-day retention, built and discarded one and a half
 * million JsonParsers to answer a request for two hundred.
 *
 * The correctness tests here would pass against that version.  The one
 * that would not is the last: it asks whether the work was actually
 * avoided, by comparing a limited read against a full one over the same
 * corpus.
 */

#include <clawtilla.h>

#include "clawt-test-util.h"

#include <string.h>

/*
 * Days of history, oldest first, with @per_day events in each.
 *
 * Written by hand rather than through clawt_event_log_append(), because
 * the point is to control which *day file* each event lands in -- append
 * always writes today's.
 */
static void
write_history(const gchar *dir, guint days, guint per_day,
              const gchar *subject)
{
    guint day;

    for (day = 0; day < days; day++) {
        g_autofree gchar *name = g_strdup_printf("2026-%02u-%02u.ndjson",
                                                 (day / 28) + 1,
                                                 (day % 28) + 1);
        g_autofree gchar *path = g_build_filename(dir, name, NULL);
        g_autoptr(GString) body = g_string_new(NULL);
        guint i;

        for (i = 0; i < per_day; i++) {
            g_string_append_printf(
                body,
                "{\"kind\":\"agent.typing\",\"subject\":\"%s\","
                "\"at\":%u,\"detail\":{\"n\":\"d%u-%u\"}}\n",
                subject, day * per_day + i, day, i);
        }

        g_assert_true(g_file_set_contents(path, body->str, -1, NULL));
    }
}

/* The most recent @limit, and in the order a reader renders them. */
static void
test_a_limited_read_is_the_newest_and_in_order(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-evlog-XXXXXX", NULL);
    g_autoptr(ClawtEventLog) log = NULL;
    g_autoptr(GPtrArray) events = NULL;

    g_assert_nonnull(dir);
    write_history(dir, 4, 5, "chief");

    log = clawt_event_log_new(dir, 30);
    events = clawt_event_log_read(log, NULL, 3);

    g_assert_cmpuint(events->len, ==, 3);

    /*
     * The last three written, oldest first: day 3's events 2, 3 and 4.
     * Asserted on the values rather than only on the count -- a reader
     * that walked backwards and forgot to put them back in order
     * returns three of the right events in the wrong sequence, and a
     * count cannot see that.
     */
    {
        ClawtEvent *first = g_ptr_array_index(events, 0);
        ClawtEvent *last = g_ptr_array_index(events, 2);

        g_assert_cmpstr(clawt_event_get_detail(first, "n"), ==, "d3-2");
        g_assert_cmpstr(clawt_event_get_detail(last, "n"), ==, "d3-4");
    }

    clawt_test_remove_tree(dir);
}

/* A limit larger than the history returns the history. */
static void
test_a_limit_past_the_end_returns_everything(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-evlog2-XXXXXX", NULL);
    g_autoptr(ClawtEventLog) log = NULL;
    g_autoptr(GPtrArray) events = NULL;

    write_history(dir, 3, 2, "chief");

    log = clawt_event_log_new(dir, 30);
    events = clawt_event_log_read(log, NULL, 100);

    g_assert_cmpuint(events->len, ==, 6);

    {
        ClawtEvent *first = g_ptr_array_index(events, 0);

        g_assert_cmpstr(clawt_event_get_detail(first, "n"), ==, "d0-0");
    }

    clawt_test_remove_tree(dir);
}

/* And no limit at all is still the whole history. */
static void
test_no_limit_reads_everything(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-evlog3-XXXXXX", NULL);
    g_autoptr(ClawtEventLog) log = NULL;
    g_autoptr(GPtrArray) events = NULL;

    write_history(dir, 5, 4, "chief");

    log = clawt_event_log_new(dir, 30);
    events = clawt_event_log_read(log, NULL, 0);

    g_assert_cmpuint(events->len, ==, 20);

    clawt_test_remove_tree(dir);
}

/*
 * The subject filter, and the substring shortcut that precedes it.
 *
 * A line that does not contain the subject as a substring cannot be
 * about it, which is what makes skipping the parse safe.  The reverse is
 * not true -- a line mentioning "chief" in a detail is not necessarily
 * about chief -- so the parsed comparison has to stay, and this checks
 * it does.
 */
static void
test_the_subject_filter_still_compares_the_parsed_field(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-evlog4-XXXXXX", NULL);
    g_autofree gchar *path = NULL;
    g_autoptr(ClawtEventLog) log = NULL;
    g_autoptr(GPtrArray) events = NULL;
    const gchar *body =
        "{\"kind\":\"agent.typing\",\"subject\":\"chief\",\"at\":1,"
        "\"detail\":{\"n\":\"real\"}}\n"
        /* Mentions chief, is about somebody else. */
        "{\"kind\":\"agent.typing\",\"subject\":\"scribe\",\"at\":2,"
        "\"detail\":{\"note\":\"asked chief\"}}\n"
        /* Does not mention it at all. */
        "{\"kind\":\"agent.typing\",\"subject\":\"scribe\",\"at\":3,"
        "\"detail\":{\"n\":\"other\"}}\n";

    path = g_build_filename(dir, "2026-01-01.ndjson", NULL);
    g_assert_true(g_file_set_contents(path, body, -1, NULL));

    log = clawt_event_log_new(dir, 30);
    events = clawt_event_log_read(log, "chief", 10);

    g_assert_cmpuint(events->len, ==, 1);
    g_assert_cmpstr(clawt_event_get_detail(g_ptr_array_index(events, 0), "n"),
                    ==, "real");

    clawt_test_remove_tree(dir);
}

/* A truncated line is skipped rather than throwing the file away. */
static void
test_a_malformed_line_does_not_lose_the_file(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-evlog5-XXXXXX", NULL);
    g_autofree gchar *path = NULL;
    g_autoptr(ClawtEventLog) log = NULL;
    g_autoptr(GPtrArray) events = NULL;
    const gchar *body =
        "{\"kind\":\"agent.typing\",\"subject\":\"chief\",\"at\":1,"
        "\"detail\":{\"n\":\"before\"}}\n"
        "{\"kind\":\"agent.typing\",\"subj\n"
        "{\"kind\":\"agent.typing\",\"subject\":\"chief\",\"at\":3,"
        "\"detail\":{\"n\":\"after\"}}\n";

    path = g_build_filename(dir, "2026-01-01.ndjson", NULL);
    g_assert_true(g_file_set_contents(path, body, -1, NULL));

    log = clawt_event_log_new(dir, 30);
    events = clawt_event_log_read(log, NULL, 10);

    g_assert_cmpuint(events->len, ==, 2);

    clawt_test_remove_tree(dir);
}

/*
 * The one that fails against the old reader.
 *
 * Every test above passes whether or not the work is avoided, because
 * they assert on the answer and the answer was already right.  This one
 * asks whether a limited read stops: it compares the same corpus read
 * with a small limit against the same corpus read whole.
 *
 * A ratio rather than a wall-clock constant, so it does not depend on
 * how fast this machine is.  The margin is deliberately huge -- the old
 * reader does the *identical* work in both arms, so it lands at roughly
 * 1.0, while a reader that stops after one day of thirty is far below
 * the quarter asked for here.
 */
static void
test_a_limited_read_does_not_parse_the_whole_history(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-evlog6-XXXXXX", NULL);
    g_autoptr(ClawtEventLog) log = NULL;
    gint64 limited;
    gint64 whole;
    gint64 started;

    write_history(dir, 28, 1500, "chief");

    log = clawt_event_log_new(dir, 30);

    /* Warm the page cache so this measures parsing, not disk. */
    {
        g_autoptr(GPtrArray) warm = clawt_event_log_read(log, NULL, 0);

        g_assert_cmpuint(warm->len, ==, 28 * 1500);
    }

    started = g_get_monotonic_time();
    {
        g_autoptr(GPtrArray) few = clawt_event_log_read(log, NULL, 20);

        g_assert_cmpuint(few->len, ==, 20);
    }
    limited = g_get_monotonic_time() - started;

    started = g_get_monotonic_time();
    {
        g_autoptr(GPtrArray) all = clawt_event_log_read(log, NULL, 0);

        g_assert_cmpuint(all->len, ==, 28 * 1500);
    }
    whole = g_get_monotonic_time() - started;

    g_test_message("limited %" G_GINT64_FORMAT " us, whole %"
                   G_GINT64_FORMAT " us", limited, whole);

    g_assert_cmpint(limited * 4, <, whole);

    clawt_test_remove_tree(dir);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/event-log/limited-read-is-newest-and-in-order",
                    test_a_limited_read_is_the_newest_and_in_order);
    g_test_add_func("/event-log/limit-past-the-end",
                    test_a_limit_past_the_end_returns_everything);
    g_test_add_func("/event-log/no-limit-reads-everything",
                    test_no_limit_reads_everything);
    g_test_add_func("/event-log/subject-filter-compares-parsed",
                    test_the_subject_filter_still_compares_the_parsed_field);
    g_test_add_func("/event-log/malformed-line-skipped",
                    test_a_malformed_line_does_not_lose_the_file);
    g_test_add_func("/event-log/limited-read-stops-early",
                    test_a_limited_read_does_not_parse_the_whole_history);

    return g_test_run();
}
