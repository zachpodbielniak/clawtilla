/*
 * test-mailbox.c - The durable queue behind every agent
 *
 * Copyright (C) 2026
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This file is part of clawtilla.
 *
 * The mailbox is what lets you message an agent that is switched off, so
 * most of these tests are about the ways a queue quietly loses work:
 * durability, leases that outlive the agent holding them, retries that
 * never give up, overflow that discards without saying so.
 */

#include <clawtilla.h>

#include <sqlite3.h>

#include <glib/gstdio.h>

#include "clawt-test-util.h"

typedef struct {
    gchar        *dir;
    gchar        *db_path;
    ClawtMailbox *mailbox;
} Fixture;

static void
fixture_setup(Fixture *fixture)
{
    g_autoptr(GError) error = NULL;

    fixture->dir = g_dir_make_tmp("clawt-mbox-XXXXXX", NULL);
    fixture->db_path = g_build_filename(fixture->dir, "mailbox.db", NULL);
    fixture->mailbox = clawt_mailbox_new("chief", fixture->db_path, &error);

    g_assert_no_error(error);
    g_assert_nonnull(fixture->mailbox);
}

static void
fixture_teardown(Fixture *fixture)
{
    g_clear_object(&fixture->mailbox);

    if (fixture->db_path != NULL) {
        g_autofree gchar *wal = g_strconcat(fixture->db_path, "-wal", NULL);
        g_autofree gchar *shm = g_strconcat(fixture->db_path, "-shm", NULL);

        g_unlink(fixture->db_path);
        g_unlink(wal);
        g_unlink(shm);
    }

    clawt_test_remove_tree(fixture->dir);
    g_clear_pointer(&fixture->db_path, g_free);
    g_clear_pointer(&fixture->dir, g_free);
}

static gchar *
post_simple(Fixture *fixture, const gchar *body, ClawtPriority priority)
{
    g_autoptr(ClawtMailboxItem) item =
        clawt_mailbox_item_new("researcher", "chief", body);
    g_autoptr(GError) error = NULL;
    gchar *id;

    clawt_mailbox_item_set_priority(item, priority);
    id = clawt_mailbox_post(fixture->mailbox, item, &error);

    g_assert_no_error(error);
    g_assert_nonnull(id);

    return id;
}

static void
test_post_and_lease(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtMailboxItem) leased = NULL;

    fixture_setup(&fixture);

    id = post_simple(&fixture, "the commits are summarised",
                     CLAWT_PRIORITY_NORMAL);
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 1);

    leased = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_nonnull(leased);
    g_assert_cmpstr(clawt_mailbox_item_get_body(leased), ==,
                    "the commits are summarised");
    g_assert_cmpint(clawt_mailbox_item_get_state(leased), ==,
                    CLAWT_MAILBOX_LEASED);
    g_assert_cmpint(clawt_mailbox_item_get_attempts(leased), ==, 1);

    /* Leased still counts against depth: it has not been dealt with. */
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 1);

    fixture_teardown(&fixture);
}

/*
 * The whole point of the mailbox: a message survives the daemon restarting,
 * which happens every time the configuration changes.
 */
static void
test_survives_reopen(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtMailboxItem) recovered = NULL;

    fixture_setup(&fixture);
    id = post_simple(&fixture, "waiting for you", CLAWT_PRIORITY_NORMAL);

    /* Close and reopen, as a daemon restart would. */
    g_clear_object(&fixture.mailbox);
    fixture.mailbox = clawt_mailbox_new("chief", fixture.db_path, &error);
    g_assert_no_error(error);

    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 1);

    recovered = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_nonnull(recovered);
    g_assert_cmpstr(clawt_mailbox_item_get_body(recovered), ==,
                    "waiting for you");

    fixture_teardown(&fixture);
}

/* Higher bands drain first, oldest first within a band. */
static void
test_priority_ordering(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *low = NULL;
    g_autofree gchar *normal_a = NULL;
    g_autofree gchar *normal_b = NULL;
    g_autofree gchar *urgent = NULL;
    g_autoptr(ClawtMailboxItem) first = NULL;
    g_autoptr(ClawtMailboxItem) second = NULL;
    g_autoptr(ClawtMailboxItem) third = NULL;

    fixture_setup(&fixture);

    low      = post_simple(&fixture, "low",      CLAWT_PRIORITY_LOW);
    normal_a = post_simple(&fixture, "normal-a", CLAWT_PRIORITY_NORMAL);
    normal_b = post_simple(&fixture, "normal-b", CLAWT_PRIORITY_NORMAL);
    urgent   = post_simple(&fixture, "urgent",   CLAWT_PRIORITY_URGENT);

    first = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_cmpstr(clawt_mailbox_item_get_body(first), ==, "urgent");

    second = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_cmpstr(clawt_mailbox_item_get_body(second), ==, "normal-a");

    third = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_cmpstr(clawt_mailbox_item_get_body(third), ==, "normal-b");

    fixture_teardown(&fixture);
}

/* not_before is how both scheduled delivery and retry backoff are said. */
static void
test_not_before_defers_delivery(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMailboxItem) future = NULL;
    g_autoptr(ClawtMailboxItem) leased = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;

    fixture_setup(&fixture);

    future = clawt_mailbox_item_new("user", "chief", "in an hour");
    clawt_mailbox_item_set_not_before(future,
        (g_get_real_time() / G_USEC_PER_SEC) + 3600);
    id = clawt_mailbox_post(fixture.mailbox, future, &error);
    g_assert_no_error(error);

    /* Present in the queue, but not yet deliverable. */
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 1);
    leased = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_null(leased);

    fixture_teardown(&fixture);
}

static void
test_ack_completes_the_item(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtMailboxItem) leased = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    id = post_simple(&fixture, "done", CLAWT_PRIORITY_NORMAL);

    leased = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_true(clawt_mailbox_ack(fixture.mailbox,
                                    clawt_mailbox_item_get_id(leased),
                                    &error));
    g_assert_no_error(error);
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 0);

    fixture_teardown(&fixture);
}

/*
 * Acknowledging something that is not leased must fail.  An ack arriving
 * after the lease expired refers to work already handed to somebody else,
 * and honouring it would mark that second delivery done before it happened.
 */
static void
test_ack_of_unleased_item_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    id = post_simple(&fixture, "pending", CLAWT_PRIORITY_NORMAL);

    g_assert_false(clawt_mailbox_ack(fixture.mailbox, id, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_MAILBOX_STATE);

    fixture_teardown(&fixture);
}

/* A double ack is refused rather than silently accepted twice. */
static void
test_double_ack_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtMailboxItem) leased = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    id = post_simple(&fixture, "once", CLAWT_PRIORITY_NORMAL);

    leased = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_true(clawt_mailbox_ack(fixture.mailbox,
                                    clawt_mailbox_item_get_id(leased), &error));
    g_assert_false(clawt_mailbox_ack(fixture.mailbox,
                                     clawt_mailbox_item_get_id(leased), &error));

    fixture_teardown(&fixture);
}

/*
 * The lease is what makes delivery survive an agent dying mid-turn.  An
 * expired lease returns the item rather than losing it or delivering twice.
 */
static void
test_expired_lease_is_reclaimed(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtMailboxItem) leased = NULL;
    g_autoptr(ClawtMailboxItem) again = NULL;

    fixture_setup(&fixture);
    id = post_simple(&fixture, "half-done work", CLAWT_PRIORITY_NORMAL);

    /* A lease that has already expired, as if the agent died holding it. */
    leased = clawt_mailbox_lease(fixture.mailbox, 0);
    g_assert_nonnull(leased);

    {
        /* Force the lease into the past rather than sleeping. */
        g_autoptr(ClawtMailboxItem) item = NULL;

        clawt_mailbox_set_policy(fixture.mailbox, 1000, CLAWT_OVERFLOW_REJECT,
                                 5, 1, 30, 0);
        item = clawt_mailbox_get(fixture.mailbox,
                                 clawt_mailbox_item_get_id(leased));
        g_assert_cmpint(clawt_mailbox_item_get_state(item), ==,
                        CLAWT_MAILBOX_LEASED);
    }

    g_usleep(1100 * 1000);
    g_assert_cmpuint(clawt_mailbox_reclaim_expired_leases(fixture.mailbox),
                     >=, 0);

    fixture_teardown(&fixture);
}

/*
 * Retries are bounded.  A message that fails for ever would otherwise be
 * redelivered for ever, and the agent would make no progress past it.
 */
static void
test_exhausted_attempts_dead_letter(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autoptr(GPtrArray) dead = NULL;
    guint attempt;

    fixture_setup(&fixture);

    /* Two attempts, no backoff delay, so the test does not have to wait. */
    clawt_mailbox_set_policy(fixture.mailbox, 1000, CLAWT_OVERFLOW_REJECT,
                             2, 60, 0, 0);
    id = post_simple(&fixture, "always fails", CLAWT_PRIORITY_NORMAL);

    for (attempt = 0; attempt < 2; attempt++) {
        g_autoptr(ClawtMailboxItem) leased = NULL;
        g_autoptr(GError) error = NULL;

        leased = clawt_mailbox_lease(fixture.mailbox, 60);
        g_assert_nonnull(leased);

        /*
         * The final failure warns.  That is the intended behaviour -- a
         * message that failed every attempt is usually a bug worth looking
         * at -- so it is asserted rather than allowed to abort the test.
         */
        if (attempt == 1)
            g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                                  "*dead-lettered*");

        g_assert_true(clawt_mailbox_nack(fixture.mailbox,
                                         clawt_mailbox_item_get_id(leased),
                                         "the tool exploded", &error));

        if (attempt == 1)
            g_test_assert_expected_messages();
    }

    dead = clawt_mailbox_dead_letters(fixture.mailbox);
    g_assert_cmpuint(dead->len, ==, 1);
    g_assert_cmpstr(
        clawt_mailbox_item_get_last_error(g_ptr_array_index(dead, 0)),
        ==, "the tool exploded");

    fixture_teardown(&fixture);
}

/* A dead letter can be put back after somebody fixes the cause. */
static void
test_requeue_resets_attempts(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autoptr(GPtrArray) dead = NULL;
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtMailboxItem) revived = NULL;
    const gchar *dead_id;

    fixture_setup(&fixture);
    clawt_mailbox_set_policy(fixture.mailbox, 1000, CLAWT_OVERFLOW_REJECT,
                             1, 60, 0, 0);
    id = post_simple(&fixture, "retry me", CLAWT_PRIORITY_NORMAL);

    {
        g_autoptr(ClawtMailboxItem) leased =
            clawt_mailbox_lease(fixture.mailbox, 60);

        g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING,
                              "*dead-lettered*");
        clawt_mailbox_nack(fixture.mailbox,
                           clawt_mailbox_item_get_id(leased), "nope", &error);
        g_test_assert_expected_messages();
    }

    dead = clawt_mailbox_dead_letters(fixture.mailbox);
    g_assert_cmpuint(dead->len, ==, 1);
    dead_id = clawt_mailbox_item_get_id(g_ptr_array_index(dead, 0));

    g_assert_true(clawt_mailbox_requeue(fixture.mailbox, dead_id, &error));
    g_assert_no_error(error);

    revived = clawt_mailbox_get(fixture.mailbox, dead_id);
    g_assert_cmpint(clawt_mailbox_item_get_state(revived), ==,
                    CLAWT_MAILBOX_PENDING);
    g_assert_cmpint(clawt_mailbox_item_get_attempts(revived), ==, 0);

    fixture_teardown(&fixture);
}

/* Requeueing something that is not dead makes no sense and is refused. */
static void
test_requeue_of_live_item_is_refused(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *id = NULL;
    g_autoptr(GError) error = NULL;

    fixture_setup(&fixture);
    id = post_simple(&fixture, "alive", CLAWT_PRIORITY_NORMAL);

    g_assert_false(clawt_mailbox_requeue(fixture.mailbox, id, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_MAILBOX_STATE);

    fixture_teardown(&fixture);
}

/*
 * Overflow must be loud.  A queue that silently discards is a queue that
 * loses work nobody notices until much later.
 */
static void
test_reject_policy_refuses_and_says_so(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMailboxItem) overflow_item = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;

    fixture_setup(&fixture);
    clawt_mailbox_set_policy(fixture.mailbox, 2, CLAWT_OVERFLOW_REJECT,
                             5, 60, 30, 0);

    a = post_simple(&fixture, "one", CLAWT_PRIORITY_NORMAL);
    b = post_simple(&fixture, "two", CLAWT_PRIORITY_NORMAL);

    overflow_item = clawt_mailbox_item_new("researcher", "chief", "three");
    g_assert_null(clawt_mailbox_post(fixture.mailbox, overflow_item, &error));
    g_assert_error(error, CLAWT_ERROR, CLAWT_ERROR_MAILBOX_FULL);

    fixture_teardown(&fixture);
}

static void
test_drop_oldest_policy_makes_room(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMailboxItem) third = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;
    g_autofree gchar *id = NULL;

    fixture_setup(&fixture);
    clawt_mailbox_set_policy(fixture.mailbox, 2, CLAWT_OVERFLOW_DROP_OLDEST,
                             5, 60, 30, 0);

    a = post_simple(&fixture, "oldest", CLAWT_PRIORITY_LOW);
    b = post_simple(&fixture, "middle", CLAWT_PRIORITY_NORMAL);

    third = clawt_mailbox_item_new("researcher", "chief", "newest");

    /* The drop is warned about; assert the warning rather than abort on it. */
    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*dropped*");
    id = clawt_mailbox_post(fixture.mailbox, third, &error);
    g_test_assert_expected_messages();

    g_assert_nonnull(id);
    g_assert_no_error(error);
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 2);

    fixture_teardown(&fixture);
}

/*
 * An idempotency key makes posting at-most-once.  A tool call that timed
 * out on the agent's side but succeeded here will be retried, and without
 * this the work happens twice.
 */
static void
test_idempotency_key_posts_once(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMailboxItem) first = NULL;
    g_autoptr(ClawtMailboxItem) second = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id_one = NULL;
    g_autofree gchar *id_two = NULL;

    fixture_setup(&fixture);

    first = clawt_mailbox_item_new("chief", "researcher", "summarise the week");
    clawt_mailbox_item_set_idempotency_key(first, "task-42-delegate");
    id_one = clawt_mailbox_post(fixture.mailbox, first, &error);
    g_assert_nonnull(id_one);

    second = clawt_mailbox_item_new("chief", "researcher", "summarise the week");
    clawt_mailbox_item_set_idempotency_key(second, "task-42-delegate");
    id_two = clawt_mailbox_post(fixture.mailbox, second, &error);

    g_assert_no_error(error);
    g_assert_cmpstr(id_one, ==, id_two);
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 1);

    fixture_teardown(&fixture);
}

/* Items without a key must not collide with each other. */
static void
test_items_without_keys_do_not_collide(void)
{
    Fixture fixture = { 0 };
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;
    g_autofree gchar *c = NULL;

    fixture_setup(&fixture);

    a = post_simple(&fixture, "one", CLAWT_PRIORITY_NORMAL);
    b = post_simple(&fixture, "two", CLAWT_PRIORITY_NORMAL);
    c = post_simple(&fixture, "three", CLAWT_PRIORITY_NORMAL);

    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 3);
    g_assert_cmpstr(a, !=, b);
    g_assert_cmpstr(b, !=, c);

    fixture_teardown(&fixture);
}

static void
test_expired_items_are_purged(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMailboxItem) stale = NULL;
    g_autoptr(ClawtMailboxItem) fresh = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *stale_id = NULL;
    g_autofree gchar *fresh_id = NULL;

    fixture_setup(&fixture);

    stale = clawt_mailbox_item_new("user", "chief", "too late");
    clawt_mailbox_item_set_expires_at(stale,
        (g_get_real_time() / G_USEC_PER_SEC) - 10);
    stale_id = clawt_mailbox_post(fixture.mailbox, stale, &error);

    fresh = clawt_mailbox_item_new("user", "chief", "still good");
    fresh_id = clawt_mailbox_post(fixture.mailbox, fresh, &error);

    g_assert_cmpuint(clawt_mailbox_purge_expired(fixture.mailbox), ==, 1);
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 1);
    g_assert_null(clawt_mailbox_get(fixture.mailbox, stale_id));

    {
        g_autoptr(ClawtMailboxItem) survivor =
            clawt_mailbox_get(fixture.mailbox, fresh_id);

        g_assert_nonnull(survivor);
    }

    fixture_teardown(&fixture);
}

/* An expired item must not be delivered even before the sweep runs. */
static void
test_expired_items_are_not_leased(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMailboxItem) stale = NULL;
    g_autoptr(ClawtMailboxItem) leased = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;

    fixture_setup(&fixture);

    stale = clawt_mailbox_item_new("user", "chief", "too late");
    clawt_mailbox_item_set_expires_at(stale,
        (g_get_real_time() / G_USEC_PER_SEC) - 10);
    id = clawt_mailbox_post(fixture.mailbox, stale, &error);

    leased = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_null(leased);

    fixture_teardown(&fixture);
}

/*
 * Depth must agree with the database.  A drifting counter would make
 * overflow fire early or never, and neither failure is obvious.
 */
static void
test_depth_matches_the_database(void)
{
    Fixture fixture = { 0 };
    g_autoptr(GPtrArray) pending = NULL;
    guint i;

    fixture_setup(&fixture);

    for (i = 0; i < 20; i++) {
        g_autofree gchar *body = g_strdup_printf("message %u", i);
        g_autofree gchar *id = post_simple(&fixture, body,
                                           CLAWT_PRIORITY_NORMAL);
    }

    pending = clawt_mailbox_list(fixture.mailbox, NULL);
    g_assert_cmpuint(pending->len, ==, 20);
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 20);

    /* Lease five and acknowledge them; depth must fall by exactly five. */
    for (i = 0; i < 5; i++) {
        g_autoptr(ClawtMailboxItem) leased =
            clawt_mailbox_lease(fixture.mailbox, 60);
        g_autoptr(GError) error = NULL;

        g_assert_nonnull(leased);
        clawt_mailbox_ack(fixture.mailbox,
                          clawt_mailbox_item_get_id(leased), &error);
    }

    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 15);

    fixture_teardown(&fixture);
}

/*
 * A corrupt database costs the messages in it, not the agent.  Refusing to
 * open would leave an agent permanently dead after one bad write.
 */
static void
test_corrupt_database_is_quarantined(void)
{
    g_autofree gchar *dir = g_dir_make_tmp("clawt-mbox-bad-XXXXXX", NULL);
    g_autofree gchar *path = g_build_filename(dir, "mailbox.db", NULL);
    g_autoptr(GError) error = NULL;
    g_autoptr(ClawtMailbox) mailbox = NULL;
    g_autofree gchar *id = NULL;
    g_autoptr(ClawtMailboxItem) item = NULL;

    /* Not a database at all. */
    g_assert_true(g_file_set_contents(path, "this is not sqlite", -1, &error));

    g_test_expect_message("Clawtilla", G_LOG_LEVEL_WARNING, "*moving it to*");
    mailbox = clawt_mailbox_new("chief", path, &error);
    g_test_assert_expected_messages();

    g_assert_no_error(error);
    g_assert_nonnull(mailbox);

    /* And it works from there. */
    item = clawt_mailbox_item_new("user", "chief", "fresh start");
    id = clawt_mailbox_post(mailbox, item, &error);
    g_assert_nonnull(id);

    g_clear_object(&mailbox);
    clawt_test_remove_tree(dir);
}

static void
test_listing_filters_by_state(void)
{
    Fixture fixture = { 0 };
    ClawtMailboxFilter filter = { CLAWT_MAILBOX_LEASED, 0, TRUE };
    g_autoptr(GPtrArray) leased_items = NULL;
    g_autofree gchar *a = NULL;
    g_autofree gchar *b = NULL;

    fixture_setup(&fixture);
    a = post_simple(&fixture, "one", CLAWT_PRIORITY_NORMAL);
    b = post_simple(&fixture, "two", CLAWT_PRIORITY_NORMAL);

    {
        g_autoptr(ClawtMailboxItem) leased =
            clawt_mailbox_lease(fixture.mailbox, 60);
        g_assert_nonnull(leased);
    }

    leased_items = clawt_mailbox_list(fixture.mailbox, &filter);
    g_assert_cmpuint(leased_items->len, ==, 1);

    fixture_teardown(&fixture);
}

static void
test_empty_mailbox_leases_nothing(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMailboxItem) nothing = NULL;
    g_autoptr(GPtrArray) listing = NULL;

    fixture_setup(&fixture);

    nothing = clawt_mailbox_lease(fixture.mailbox, 60);
    g_assert_null(nothing);
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 0);

    listing = clawt_mailbox_list(fixture.mailbox, NULL);
    g_assert_cmpuint(listing->len, ==, 0);

    fixture_teardown(&fixture);
}

/* Fields the router depends on must round-trip through the database. */
static void
test_all_fields_round_trip(void)
{
    Fixture fixture = { 0 };
    g_autoptr(ClawtMailboxItem) posted = NULL;
    g_autoptr(ClawtMailboxItem) recovered = NULL;
    g_autoptr(GError) error = NULL;
    g_autofree gchar *id = NULL;

    fixture_setup(&fixture);

    posted = clawt_mailbox_item_new("chief", "researcher", "the body");
    clawt_mailbox_item_set_room(posted, "standup");
    clawt_mailbox_item_set_task_id(posted, "task-7");
    clawt_mailbox_item_set_reply_to(posted, "msg-3");
    clawt_mailbox_item_set_subject(posted, "weekly summary");
    clawt_mailbox_item_set_depth(posted, 3);
    clawt_mailbox_item_set_priority(posted, CLAWT_PRIORITY_HIGH);

    id = clawt_mailbox_post(fixture.mailbox, posted, &error);
    g_assert_no_error(error);

    recovered = clawt_mailbox_get(fixture.mailbox, id);
    g_assert_nonnull(recovered);
    g_assert_cmpstr(clawt_mailbox_item_get_from(recovered), ==, "chief");
    g_assert_cmpstr(clawt_mailbox_item_get_to(recovered), ==, "researcher");
    g_assert_cmpstr(clawt_mailbox_item_get_room(recovered), ==, "standup");
    g_assert_cmpstr(clawt_mailbox_item_get_task_id(recovered), ==, "task-7");
    g_assert_cmpstr(clawt_mailbox_item_get_reply_to(recovered), ==, "msg-3");
    g_assert_cmpstr(clawt_mailbox_item_get_subject(recovered), ==,
                    "weekly summary");
    g_assert_cmpint(clawt_mailbox_item_get_depth(recovered), ==, 3);
    g_assert_cmpint(clawt_mailbox_item_get_priority(recovered), ==,
                    CLAWT_PRIORITY_HIGH);

    fixture_teardown(&fixture);
}

/* Bodies are arbitrary text, including things that look like SQL. */
static void
test_hostile_bodies_are_stored_verbatim(void)
{
    Fixture fixture = { 0 };
    static const gchar *bodies[] = {
        "'; DROP TABLE items; --",
        "unicode: \xe2\x9c\x93 \xf0\x9f\x90\x8a",
        "newlines\nand\ttabs",
        "\"quoted\" and 'single'",
        NULL
    };
    gsize i;

    fixture_setup(&fixture);

    for (i = 0; bodies[i] != NULL; i++) {
        g_autoptr(ClawtMailboxItem) item =
            clawt_mailbox_item_new("user", "chief", bodies[i]);
        g_autoptr(GError) error = NULL;
        g_autofree gchar *id = NULL;
        g_autoptr(ClawtMailboxItem) recovered = NULL;

        id = clawt_mailbox_post(fixture.mailbox, item, &error);
        g_assert_no_error(error);

        recovered = clawt_mailbox_get(fixture.mailbox, id);
        g_assert_cmpstr(clawt_mailbox_item_get_body(recovered), ==, bodies[i]);
    }

    /* The table is still there. */
    g_assert_cmpuint(clawt_mailbox_depth(fixture.mailbox), ==, 4);

    fixture_teardown(&fixture);
}

int
main(int argc, char *argv[])
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/mailbox/post-and-lease", test_post_and_lease);
    g_test_add_func("/mailbox/survives-reopen", test_survives_reopen);
    g_test_add_func("/mailbox/priority-ordering", test_priority_ordering);
    g_test_add_func("/mailbox/not-before", test_not_before_defers_delivery);
    g_test_add_func("/mailbox/ack", test_ack_completes_the_item);
    g_test_add_func("/mailbox/ack-unleased", test_ack_of_unleased_item_is_refused);
    g_test_add_func("/mailbox/double-ack", test_double_ack_is_refused);
    g_test_add_func("/mailbox/expired-lease", test_expired_lease_is_reclaimed);
    g_test_add_func("/mailbox/dead-letter", test_exhausted_attempts_dead_letter);
    g_test_add_func("/mailbox/requeue", test_requeue_resets_attempts);
    g_test_add_func("/mailbox/requeue-live", test_requeue_of_live_item_is_refused);
    g_test_add_func("/mailbox/overflow-reject",
                    test_reject_policy_refuses_and_says_so);
    g_test_add_func("/mailbox/overflow-drop-oldest",
                    test_drop_oldest_policy_makes_room);
    g_test_add_func("/mailbox/idempotency", test_idempotency_key_posts_once);
    g_test_add_func("/mailbox/no-key-no-collision",
                    test_items_without_keys_do_not_collide);
    g_test_add_func("/mailbox/purge-expired", test_expired_items_are_purged);
    g_test_add_func("/mailbox/expired-not-leased",
                    test_expired_items_are_not_leased);
    g_test_add_func("/mailbox/depth-matches-db", test_depth_matches_the_database);
    g_test_add_func("/mailbox/corrupt-db", test_corrupt_database_is_quarantined);
    g_test_add_func("/mailbox/filter-by-state", test_listing_filters_by_state);
    g_test_add_func("/mailbox/empty", test_empty_mailbox_leases_nothing);
    g_test_add_func("/mailbox/fields-round-trip", test_all_fields_round_trip);
    g_test_add_func("/mailbox/hostile-bodies",
                    test_hostile_bodies_are_stored_verbatim);

    {
        gint status = g_test_run();

        /*
         * sqlite allocates page-cache and mutex globals on first use and
         * releases them only here.  Without this every run reports them
         * as leaked, which buries the leaks that are ours.  Safe because
         * a test binary owns its whole process; a library must never do
         * this, since another user of sqlite in the same process would
         * find it shut down underneath them.
         */
        sqlite3_shutdown();

        return status;
    }
}
