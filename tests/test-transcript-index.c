/*
 * test-transcript-index.c - Searching what the fleet actually said
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The two failures this index can have are both silent.  An FTS5 query
 * is syntax rather than a search string, so a stray quote comes back as
 * zero rows -- indistinguishable from a fleet that never said the word.
 * And the room filter is the whole of the permission check: an agent
 * asking for a room it is not in has to get nothing, however the query
 * was spelled.
 */

#include <clawtilla.h>

#include <string.h>

#include "clawt-test-util.h"

typedef struct {
    gchar                *dir;
    ClawtTranscriptIndex *index;
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
    g_autofree gchar *path = NULL;
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-transcript-XXXXXX", NULL);
    path = g_build_filename(fixture->dir, "transcripts.db", NULL);

    fixture->index = clawt_transcript_index_new(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture->index);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->index);

    if (fixture->dir != NULL)
        clawt_test_remove_tree(fixture->dir);

    g_clear_pointer(&fixture->dir, g_free);
}

/* One message, indexed, with the id and time it needs to be found by. */
static void
said(Fixture *fixture, const gchar *room, const gchar *from,
     const gchar *body, gint64 at)
{
    g_autoptr(ClawtMessage) message = clawt_message_new(room, from, body);
    g_autofree gchar *id = clawt_generate_id("msg");
    g_autoptr(GError) error = NULL;

    clawt_message_set_id(message, id);
    clawt_message_set_timestamp(message, at);

    g_assert_true(clawt_transcript_index_add(fixture->index, room, message,
                                             &error));
    g_assert_no_error(error);
}

static void
test_recall_reaches_across_rooms(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) hits = NULL;

    fixture_setup(&fixture);

    said(&fixture, "ops", "alpha", "the deploy key expired again", 1000);
    said(&fixture, "research", "beta", "the deploy key is in the vault",
         2000);
    said(&fixture, "ops", "alpha", "lunch", 3000);

    /*
     * A search with no room filter is the operator's own view, which is
     * every room -- a person can already open any transcript on disk.
     */
    hits = clawt_transcript_index_search(fixture.index, "deploy key", NULL,
                                         NULL, 0, 0, NULL);

    g_assert_cmpuint(hits->len, ==, 2);

    /* Newest first: recall answers "when did we last talk about this". */
    g_assert_cmpstr(
        ((ClawtTranscriptHit *)g_ptr_array_index(hits, 0))->room_id, ==,
        "research");
    g_assert_cmpstr(
        ((ClawtTranscriptHit *)g_ptr_array_index(hits, 1))->room_id, ==,
        "ops");

    fixture_teardown(&fixture);
}

/*
 * A room the caller may not read cannot appear, however the query was
 * spelled.
 *
 * This is the permission, and it is a filter on the query rather than on
 * the result: a hit that was fetched and then dropped would still have
 * been counted, and a limit applied before the drop would have silently
 * hidden the hits the caller *was* entitled to.
 */
static void
test_a_room_you_are_not_in_never_appears(void)
{
    Fixture fixture = { 0 };
    const gchar *allowed[] = { "ops", NULL };
    const gchar *none[] = { NULL };
    g_autoptr(GPtrArray) hits = NULL;
    g_autoptr(GPtrArray) nothing = NULL;

    fixture_setup(&fixture);

    said(&fixture, "ops", "alpha", "the deploy key expired", 1000);
    said(&fixture, "private", "chief", "the deploy key is hunter2", 2000);

    hits = clawt_transcript_index_search(fixture.index, "deploy key",
                                         allowed, NULL, 0, 0, NULL);

    g_assert_cmpuint(hits->len, ==, 1);
    g_assert_cmpstr(
        ((ClawtTranscriptHit *)g_ptr_array_index(hits, 0))->room_id, ==,
        "ops");

    /*
     * And an empty list is "no rooms", not "no filter".  An agent that
     * has just joined the fleet is in none, and reading that as
     * unrestricted would hand it every conversation there is.
     */
    nothing = clawt_transcript_index_search(fixture.index, "deploy key",
                                            none, NULL, 0, 0, NULL);
    g_assert_cmpuint(nothing->len, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * Four queries that are FTS5 *syntax*, each of which used to come back
 * as zero rows rather than as a search or an error.
 *
 * The failure is the bad kind: a parse error and an empty store produce
 * the same answer, so nothing anywhere reports that the query was never
 * run.  Quoted as a phrase literal, each of these is searched for.
 */
static void
test_hostile_queries_search_rather_than_fail(void)
{
    static const gchar *queries[] = {
        "the \"quoted\" thing",
        "NOT",
        "a (broken paren",
        "wildcard *",
        "AND OR NOT",
        NULL
    };

    Fixture fixture = { 0 };
    g_autoptr(GError) refusal = NULL;
    g_autoptr(GPtrArray) nothing = NULL;
    gsize i;

    fixture_setup(&fixture);

    said(&fixture, "ops", "alpha", "the \"quoted\" thing is NOT a "
                                   "(broken paren wildcard * AND OR NOT",
         1000);

    for (i = 0; queries[i] != NULL; i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(GPtrArray) hits = NULL;

        hits = clawt_transcript_index_search(fixture.index, queries[i], NULL,
                                             NULL, 0, 0, &error);

        /*
         * A result, not a silent nothing.  The message with every one of
         * these in it is in the index, so a zero here means the query
         * was parsed as syntax and never ran.
         */
        g_assert_no_error(error);
        g_assert_cmpuint(hits->len, >, 0);
    }

    /*
     * And the one query that genuinely cannot match: a lone quote
     * tokenizes to nothing at all, so the phrase is empty and an empty
     * phrase matches no row anywhere.  That is refused with a reason
     * rather than answered "no matches", because "no matches" is what an
     * empty store says and the two need different next steps.
     */
    nothing = clawt_transcript_index_search(fixture.index, "\"", NULL, NULL,
                                            0, 0, &refusal);

    g_assert_error(refusal, CLAWT_ERROR, CLAWT_ERROR_INVALID_ARGUMENT);
    g_assert_cmpuint(nothing->len, ==, 0);

    fixture_teardown(&fixture);
}

/*
 * Re-indexing the same message does not duplicate it.
 *
 * The daemon walks every room's transcript on the first start after the
 * index appears, and a fleet that had been running for months would
 * otherwise return each line once per start.
 */
static void
test_indexing_a_message_twice_keeps_one(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMessage) message = clawt_message_new("ops", "alpha",
                                                        "the same line");
    g_autoptr(GPtrArray) hits = NULL;

    fixture_setup(&fixture);

    clawt_message_set_id(message, "msg-fixed");
    clawt_message_set_timestamp(message, 1000);

    g_assert_true(clawt_transcript_index_add(fixture.index, "ops", message,
                                             NULL));
    g_assert_true(clawt_transcript_index_add(fixture.index, "ops", message,
                                             NULL));

    g_assert_cmpuint(clawt_transcript_index_count(fixture.index), ==, 1);

    hits = clawt_transcript_index_search(fixture.index, "same line", NULL,
                                         NULL, 0, 0, NULL);
    g_assert_cmpuint(hits->len, ==, 1);

    fixture_teardown(&fixture);
}

static void
test_narrowing_by_sender_and_age(void)
{
    Fixture fixture = { 0 };
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    g_autoptr(GPtrArray) by_sender = NULL;
    g_autoptr(GPtrArray) recent = NULL;

    fixture_setup(&fixture);

    said(&fixture, "ops", "alpha", "the release is cut", now - 100);
    said(&fixture, "ops", "beta", "the release is broken", now - 100);
    said(&fixture, "ops", "alpha", "the release was fine last month",
         now - (40 * 86400));

    by_sender = clawt_transcript_index_search(fixture.index, "release", NULL,
                                              "beta", 0, 0, NULL);
    g_assert_cmpuint(by_sender->len, ==, 1);

    recent = clawt_transcript_index_search(fixture.index, "release", NULL,
                                           NULL, now - (7 * 86400), 0, NULL);
    g_assert_cmpuint(recent->len, ==, 2);

    fixture_teardown(&fixture);
}

/*
 * The index survives being reopened, and so does its schema.
 *
 * CREATE TABLE IF NOT EXISTS does nothing to a file that already has the
 * table, so a column added later would reach new databases only -- which
 * is how a live fleet's mailboxes came to be quarantined as corrupt on
 * upgrade.  apply_schema() asks PRAGMA table_info instead, and this is
 * the assertion that the second open goes through it.
 */
static void
test_the_index_survives_being_reopened(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *path = NULL;
    g_autoptr(ClawtTranscriptIndex) reopened = NULL;
    g_autoptr(GPtrArray) hits = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);

    said(&fixture, "ops", "alpha", "worth finding again", 1000);
    path = g_build_filename(fixture.dir, "transcripts.db", NULL);

    g_clear_object(&fixture.index);

    reopened = clawt_transcript_index_new(path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(reopened);

    g_assert_cmpuint(clawt_transcript_index_count(reopened), ==, 1);

    hits = clawt_transcript_index_search(reopened, "finding again", NULL,
                                         NULL, 0, 0, NULL);
    g_assert_cmpuint(hits->len, ==, 1);

    fixture_teardown(&fixture);
}

/*
 * A message with no id or no body is not an error.
 *
 * This runs on the delivery path, and a send that failed because
 * something could not be indexed would be the search taking the
 * conversation down with it.
 */
static void
test_an_unindexable_message_is_not_a_failed_send(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMessage) empty = clawt_message_new("ops", "alpha", "");

    fixture_setup(&fixture);

    clawt_message_set_id(empty, "msg-empty");

    g_assert_true(clawt_transcript_index_add(fixture.index, "ops", empty,
                                             NULL));
    g_assert_cmpuint(clawt_transcript_index_count(fixture.index), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * The index is redacted, because it is the copy that gets read back.
 *
 * clawt_room_append() scrubs a body before writing the JSONL transcript
 * and says why: a transcript is replayed into every context rebuild, so
 * a key that reached the file would be handed back to the model for
 * ever.  The row written beside it kept the plaintext, and this is the
 * store clawtilla_recall searches -- so the careful half wrote
 * "[REDACTED]" to a file nothing greps while the other half served the
 * key to any agent that asked, in any room it could see.  Redaction
 * lives in the store now, so the daemon's start-time re-index is covered
 * by the same change.
 */
static void
test_a_secret_is_not_indexed(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) hits = NULL;
    const gchar *key = "sk-ant-api03-AAAAAAAAAAAAAAAAAAAAAAAA";
    guint i;

    fixture_setup(&fixture);

    said(&fixture, "ops", "alpha",
         "here is the key you asked for: sk-ant-api03-"
         "AAAAAAAAAAAAAAAAAAAAAAAA", 1000);
    said(&fixture, "ops", "beta",
         "Authorization: Bearer ghp_bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
         2000);

    /*
     * Searched for by a word that survives, so the row is definitely
     * there and it is the body being checked rather than the indexing.
     */
    hits = clawt_transcript_index_search(fixture.index, "key", NULL, NULL,
                                         0, 10, NULL);

    g_assert_nonnull(hits);
    g_assert_cmpuint(hits->len, >, 0);

    for (i = 0; i < hits->len; i++) {
        ClawtTranscriptHit *hit = g_ptr_array_index(hits, i);

        g_assert_null(strstr(hit->body, key));
        g_assert_nonnull(strstr(hit->body, "[REDACTED]"));
    }

    fixture_teardown(&fixture);
}

/*
 * And the full-text side too, which is the one that would answer.
 *
 * Asserting only on the returned body would pass against an index that
 * kept the key in messages_fts: the row would still be findable by the
 * secret itself, which is exactly the query somebody hunting a leaked
 * credential would run, and exactly the one an agent asked to "find the
 * API key" would run too.
 */
static void
test_a_secret_cannot_be_searched_for(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) hits = NULL;

    fixture_setup(&fixture);

    said(&fixture, "ops", "alpha",
         "the token is glpat-cccccccccccccccccccc", 1000);

    hits = clawt_transcript_index_search(fixture.index,
                                         "glpat-cccccccccccccccccccc", NULL,
                                         NULL, 0, 10, NULL);

    g_assert_nonnull(hits);
    g_assert_cmpuint(hits->len, ==, 0);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/transcript/across-rooms",
                    test_recall_reaches_across_rooms);
    g_test_add_func("/transcript/room-permission",
                    test_a_room_you_are_not_in_never_appears);
    g_test_add_func("/transcript/hostile-queries",
                    test_hostile_queries_search_rather_than_fail);
    g_test_add_func("/transcript/reindex-is-idempotent",
                    test_indexing_a_message_twice_keeps_one);
    g_test_add_func("/transcript/narrowing",
                    test_narrowing_by_sender_and_age);
    g_test_add_func("/transcript/survives-reopening",
                    test_the_index_survives_being_reopened);
    g_test_add_func("/transcript/secret-not-stored",
                    test_a_secret_is_not_indexed);
    g_test_add_func("/transcript/secret-not-searchable",
                    test_a_secret_cannot_be_searched_for);
    g_test_add_func("/transcript/unindexable-is-not-a-failure",
                    test_an_unindexable_message_is_not_a_failed_send);

    return g_test_run();
}
